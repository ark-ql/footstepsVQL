#!/usr/bin/env python3
"""Send a console/minqlx command to the local qzeroded over ZMQ rcon.

Runs on the server host. The rcon port and password are read straight out of
/proc/<pid>/cmdline and are never printed, logged or passed as arguments -- the
only thing that reaches stdout is the server's own reply, which is why this
exists instead of a one-liner with the password on the command line.

Two things are mandatory and were missing for a long time, which is why an earlier
version of this script authenticated cleanly and then had every command silently
dropped. Both come from the vendor client zmq_rcon.py in the dedicated server directory, which
cannot be run directly here because it is Python 2:

  * an explicit zmq.IDENTITY. QL's rcon socket is a ROUTER, and it addresses
    replies by peer identity; without one set before connect there is nothing
    stable for it to talk back to.
  * a literal "register" message as the first thing sent, and only once the
    monitor reports EVENT_CONNECTED. The vendor source calls it out: "first
    message is ignored and used to notify server of presence". Commands sent
    before it are discarded, with no error anywhere.

Usage:
    ./rcon.py "<command>" [<command> ...]
    ./rcon.py --probe            # connectivity only, sends a harmless command
"""

import sys
import time
import uuid

import zmq

QZ_NAME = "qzeroded"


def find_server():
    """(pid, rcon_port, rcon_password) for the running qzeroded.

    Two servers share this host. The footsteps rig is net_port 27960. Prefer
    that process so a second instance cannot silently receive the command.
    """
    import glob
    import os

    found = []
    for path in glob.glob("/proc/[0-9]*/cmdline"):
        try:
            with open(path, "rb") as fh:
                raw = fh.read()
        except OSError:
            continue
        if QZ_NAME.encode() not in raw:
            continue
        args = [a.decode("utf-8", "replace") for a in raw.split(b"\0") if a]
        port = pw = net = None
        for i, a in enumerate(args):
            if a == "zmq_rcon_port" and i + 1 < len(args):
                port = args[i + 1]
            elif a == "zmq_rcon_password" and i + 1 < len(args):
                pw = args[i + 1]
            elif a == "net_port" and i + 1 < len(args):
                net = args[i + 1]
        if port and pw:
            pid = int(os.path.basename(os.path.dirname(path)))
            found.append((net, pid, port, pw))
    for net, pid, port, pw in found:
        if net == "27960":
            return pid, port, pw
    if found:
        return found[0][1], found[0][2], found[0][3]
    return None, None, None


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)

    pid, port, pw = find_server()
    if not port:
        raise SystemExit("could not find a running qzeroded with zmq_rcon enabled")
    print("qzeroded pid {}, rcon port {} (password read from cmdline, not shown)"
          .format(pid, port))

    ctx = zmq.Context()
    sock = ctx.socket(zmq.DEALER)
    sock.plain_username = b"rcon"
    sock.plain_password = pw.encode()
    # IDENTITY has to be set before connect: the server side is a ROUTER and keys
    # everything on it.
    sock.setsockopt(zmq.IDENTITY, uuid.uuid1().hex.encode())
    sock.setsockopt(zmq.LINGER, 0)
    monitor = sock.get_monitor_socket(zmq.EVENT_CONNECTED)
    sock.connect("tcp://127.0.0.1:{}".format(port))

    poller = zmq.Poller()
    poller.register(sock, zmq.POLLIN)

    # Wait for the connection to be real before registering, rather than sleeping
    # and hoping. A fixed sleep is what made this flaky. recv_multipart, because a
    # monitor event is two frames and taking them one at a time desynchronises.
    mon_poller = zmq.Poller()
    mon_poller.register(monitor, zmq.POLLIN)
    connected = bool(dict(mon_poller.poll(timeout=5000)))
    if connected:
        monitor.recv_multipart()
    else:
        print("  WARNING: never saw EVENT_CONNECTED; registering anyway")
    # Both of these are required before ctx.term(), which otherwise blocks forever
    # waiting on a socket nobody remembered to close.
    sock.disable_monitor()
    monitor.close()

    # The registration message itself. Discarded by the server by design -- it
    # exists only to announce this peer -- and without it every command that
    # follows is dropped in silence.
    sock.send_string("register")
    time.sleep(0.2)

    cmds = sys.argv[1:]
    if cmds == ["--probe"]:
        cmds = ["serverinfo"]

    for cmd in cmds:
        print("\n>>> {}".format(cmd))
        sock.send_string(cmd)
        deadline = time.time() + 3.0
        got = 0
        while time.time() < deadline:
            socks = dict(poller.poll(timeout=300))
            if sock in socks:
                while True:
                    try:
                        msg = sock.recv(zmq.NOBLOCK)
                    except zmq.Again:
                        break
                    text = msg.decode("utf-8", "replace").rstrip("\n")
                    if text:
                        for line in text.split("\n"):
                            print("    {}".format(line))
                        got += 1
                deadline = time.time() + 0.8      # keep draining while it talks
        if not got:
            print("    (no reply)")

    sock.close()
    ctx.term()


if __name__ == "__main__":
    main()
