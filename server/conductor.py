#!/usr/bin/env python3
"""conductor - start and stop measurement runs from in-game chat.

The tester types, in normal chat:

    <fps> flat start [label words...]     e.g.  1000 flat start rega rtss rtt5
    <fps> stair start [label words...]    (stairs also accepted)
    stop                                  end the current run early
    discard                               end it and mark it to be ignored
    rtt <ms>                              set the rig's added round-trip delay:
                                          10 or less means none (rtt 5), else
                                          that many ms (rtt 50), max 300

The delay is the existing netem qdiscs on ens3 (egress) and ifb0 (ingress),
changed in place with `tc qdisc change`, half each way; nothing else in the
network setup is touched. Each run records the netem setting and the mover's
ps.ping at start and end.

The conductor marks the run in server time (svs.time, the clock botsnap's log
uses), confirms in chat, and after RUN_S seconds says in chat that the run is
over. A new start ends the previous run. Runs go to RUNS, one per line, for
`botsnap.py runs RUNS LOG`.

Only messages from human clients that match the commands are acted on; bots
chat through the same log line. One rcon connection is held open for the whole
session and its replies are drained continuously: a connect per message, or a
reply queue left to fill, is how the 14:23 hang on 2026-10-03 began.

    ./conductor.py [RUNS]        runs in the foreground until killed
"""
import os
import re
import subprocess
import sys
import time
import uuid

import zmq

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import botsnap                                   # noqa: E402
from rcon import find_server                     # noqa: E402

LOG = os.environ.get("CONDUCTOR_LOG",
                     os.path.join(os.environ.get("QL_DIR", "/home/ql/qlds"), "minqlxtended.log"))
RUNS = os.environ.get("CONDUCTOR_RUNS", "/tmp/fsvql-botsnap/runs.tsv")
RUN_S = int(os.environ.get("CONDUCTOR_RUN_S", "30"))

CHAT = re.compile(r"dispatch\] chat\(Player\((\d+):(['\"])(.*?)\2:(\d+)\), (['\"])(.*)\5, \w+, ")
START = re.compile(r"^\s*(\d{2,4})\s+(flat|stair|stairs)\s+start\b\s*(.*)$", re.I)
RTT = re.compile(r"^\s*rtt\s+(\d{1,3})\s*$", re.I)
NETEM_DEVS = ("ens3", "ifb0")


def netem_rtt():
    """The round trip the rig adds now, in ms, from the netem qdiscs."""
    total = 0.0
    for dev in NETEM_DEVS:
        out = subprocess.run(["tc", "qdisc", "show", "dev", dev], capture_output=True,
                             text=True).stdout
        if "netem" not in out:
            return None
        m = re.search(r"netem .*?delay ([\d.]+)(us|ms|s)", out)
        if not m:                                    # netem with delay 0 omits it
            continue
        v = float(m.group(1))
        total += v / 1000.0 if m.group(2) == "us" else v * 1000.0 if m.group(2) == "s" else v
    return int(round(total))


def set_netem_rtt(added):
    for dev in NETEM_DEVS:
        r = subprocess.run(["tc", "qdisc", "change", "dev", dev, "root", "netem", "delay",
                            "{}ms".format(added / 2.0)], capture_output=True, text=True)
        if r.returncode != 0:
            return r.stderr.strip() or "tc failed on {}".format(dev)
    return None


class Rcon:
    def __init__(self):
        pid, port, pw = find_server()
        if not port:
            raise SystemExit("no qzeroded with zmq rcon")
        self.ctx = zmq.Context()
        self.sock = self.ctx.socket(zmq.DEALER)
        self.sock.plain_username = b"rcon"
        self.sock.plain_password = pw.encode()
        self.sock.setsockopt(zmq.IDENTITY, uuid.uuid1().hex.encode())
        self.sock.setsockopt(zmq.LINGER, 0)
        mon = self.sock.get_monitor_socket(zmq.EVENT_CONNECTED)
        self.sock.connect("tcp://127.0.0.1:{}".format(port))
        poller = zmq.Poller()
        poller.register(mon, zmq.POLLIN)
        if dict(poller.poll(timeout=5000)):
            mon.recv_multipart()
        self.sock.disable_monitor()
        mon.close()
        self.sock.send_string("register")
        time.sleep(0.2)

    def say(self, text):
        self.sock.send_string("say " + text)

    def drain(self):
        while True:
            try:
                self.sock.recv(zmq.NOBLOCK)
            except zmq.Again:
                return


def main():
    runs_path = sys.argv[1] if len(sys.argv) > 1 else RUNS
    srv = botsnap.Server()
    rc = Rcon()
    fh = open(LOG, "r", errors="replace")
    fh.seek(0, os.SEEK_END)
    new = not os.path.exists(runs_path)
    out = open(runs_path, "a", buffering=1)
    if new:
        out.write("run\tstart_ms\tend_ms\tclient\tfps\tkind\tlabel\tstatus\tping\tnetem_rtt\n")
    n = sum(1 for line in open(runs_path) if line[:1].isdigit())
    cur = None
    rc.say("^3[fsv] conductor ready: '<fps> flat start' / '<fps> stair start' (+ label), "
           "'stop', 'discard', 'rtt <ms>'. Rig adds {} ms now".format(netem_rtt()))
    print("conductor ready, runs -> {}".format(runs_path), flush=True)

    def ping_of(client):
        try:
            return srv.latest(client)["ping"]
        except (OSError, KeyError):
            return -1

    def finish(status):
        nonlocal cur
        end = srv.time()
        out.write("{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}/{}\t{}\n".format(
            cur["n"], cur["start"], end, cur["client"], cur["fps"], cur["kind"],
            cur["label"], status, cur["ping"], ping_of(cur["client"]), cur["netem"]))
        rc.say("^3[fsv] run {} {} {}: {}".format(
            cur["n"], cur["fps"], cur["kind"],
            "{} sec passed - stop".format(RUN_S) if status == "ok" else status))
        cur = None

    inode = os.fstat(fh.fileno()).st_ino
    next_check = 0.0
    while True:
        line = fh.readline()
        if not line:
            try:
                if cur and srv.time() >= cur["start"] + RUN_S * 1000:
                    finish("ok")
            except OSError:
                raise SystemExit("the server process went away; restart the conductor")
            rc.drain()
            now = time.monotonic()
            if now >= next_check:
                next_check = now + 1.0
                try:
                    if os.stat(LOG).st_ino != inode:       # the log rotated
                        fh.close()
                        fh = open(LOG, "r", errors="replace")
                        inode = os.fstat(fh.fileno()).st_ino
                except OSError:
                    pass
            time.sleep(0.05)
            continue
        m = CHAT.search(line)
        if not m:
            continue
        client, text = int(m.group(1)), m.group(6).strip()
        try:
            if srv.info(client)["bot"]:
                continue
        except OSError:
            continue
        low = text.lower()
        if low in ("stop", "discard"):
            if cur:
                finish("stopped early" if low == "stop" else "discarded")
            continue
        r = RTT.match(text)
        if r:
            want = int(r.group(1))
            added = 0 if want <= 10 else min(want, 300)
            err = set_netem_rtt(added)
            rc.say("^3[fsv] {}".format(
                "rig delay now +{} ms round trip".format(netem_rtt()) if not err
                else "rtt not changed: " + err[:80]))
            continue
        s = START.match(text)
        if not s:
            continue
        if cur:
            finish("superseded by a new start")
        n += 1
        kind = "stair" if s.group(2).lower().startswith("stair") else "flat"
        cur = {"n": n, "start": srv.time(), "client": client, "fps": int(s.group(1)),
               "kind": kind, "label": s.group(3).strip().replace("\t", " "),
               "ping": ping_of(client), "netem": netem_rtt()}
        rc.say("^2[fsv] run {} started: {} {}{} - {} s".format(
            n, cur["fps"], kind, (" (" + cur["label"] + ")") if cur["label"] else "", RUN_S))
        print("run {} start {} {} {}".format(n, cur["fps"], kind, cur["label"]), flush=True)


if __name__ == "__main__":
    sys.exit(main())
