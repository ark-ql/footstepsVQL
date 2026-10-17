#!/usr/bin/env python3
"""One conductor run in detail: per-bot delivery, per-second counters, and the
listener-to-mover distances right now. Read-only.

    ./run_detail.py <run> [RUNS] [LOG]
    ./run_detail.py pos             distances only
"""
import math
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import botsnap as bs

RUNS = "/tmp/fsvql-botsnap/current-runs.tsv"
LOG = "/tmp/fsvql-botsnap/current-log.tsv"
PS_ORIGIN = 20
ES_TRBASE = 24
# Q3 snd_dma: full volume to 80 units, then linear falloff of 0.0008 per unit.
FULLVOL, ATTEN = 80.0, 0.0008


def find_run(path, n):
    with open(path) as fh:
        for line in fh:
            p = line.rstrip("\n").split("\t")
            if p and p[0] == str(n):
                return p
    raise SystemExit("run {} not in {}".format(n, path))


def vol(d):
    return max(0.0, min(1.0, 1.0 - (d - FULLVOL) * ATTEN))


def delivery(run):
    n, start, end, client, fps, kind = run[:6]
    start, end, m = int(start), int(end), int(client)
    rows, quality, roster, ctr = bs.load(LOG)
    per_sec = bs.per_sec_of(quality)
    secs = list(range(start // 1000 + 1, end // 1000))
    print("run {} {} {}: server ms {}..{}, {} whole seconds, {} bot snapshots/s".format(
        n, fps, kind, start, end, len(secs), per_sec))

    print("\n-- per bot (listener) hearing client {} --".format(m))
    print("{:>3} {:<14} {:>5} {:>6} {:>5} {:>6} {:>7} {:>6} {:>5}".format(
        "bot", "name", "full", "part", "out", "steps", "fs/s", "e/o", "<150"))
    for (b, mm), q in sorted(quality.items()):
        if mm != m:
            continue
        full = [s for s in secs if s in q and q[s][0] >= 0.9 * per_sec and q[s][1] == q[s][0]]
        part = [s for s in secs if s in q and 0 < q[s][1] < q[s][0]]
        out = [s for s in secs if s in q and q[s][1] == 0]
        played = bs.replay(sorted(rows.get((b, m), []), key=lambda x: x[0]))[0]
        inwin = [p for p in played if start <= p[0] <= end]
        infull = [p for p in played if p[0] // 1000 in set(full)]
        ts = [p[0] for p in inwin]
        near = [(x, y) for x, y in zip(ts, ts[1:]) if y - x < 150]
        e = sum(1 for p in inwin if p[1] == "entity")
        name = roster.get(b, (1, "?"))[1]
        print("{:>3} {:<14} {:>5} {:>6} {:>5} {:>6} {:>7} {:>6} {:>5}".format(
            b, name[:14], len(full), len(part), len(out), len(inwin),
            "{:.3f}".format(len(infull) / float(len(full))) if full else "-",
            "{}/{}".format(e, len(inwin) - e), len(near)))
        for x, y in near:
            print("      near-double {} -> {} ({} ms)".format(x, y, y - x))

    print("\n-- per second, footstepsVQL counters (all movers) --")
    print("{:>6} {:>4} {:>4} {:>4} {:>4} {:>4} | dt 1/2/3/4/5-8/9-32/>32 ms".format(
        "sec", "inj", "drn", "dfr", "strp", "van"))
    byt = {c[0] // 1000: c for c in ctr}
    whole = range(start // 1000 + 1, end // 1000)
    move = sum(byt[s][7] - byt[s - 1][7] for s in whole if s in byt and s - 1 in byt)
    heard = sum(1 for p in bs.replay(sorted(rows.get((min(b for b, mm in quality if mm == m), m), []),
                                            key=lambda x: x[0]))[0]
                if p[0] // 1000 in set(whole))
    print("grounded and moving: {:.1f} s of {} s; first bot heard {} -> {:.3f} per grounded second".format(
        move / 1000.0, len(whole), heard, heard * 1000.0 / move if move else 0.0))
    for s in range(start // 1000, end // 1000 + 2):
        a, b_ = byt.get(s - 1), byt.get(s)
        if not a or not b_:
            continue
        d = [y - x for x, y in zip(a, b_)]
        print("{:>6} {:>4} {:>4} {:>4} {:>4} {:>4} | {}".format(
            s, d[1], d[2], d[3], d[4], d[5], "/".join(str(x) for x in d[9:16])))


def positions():
    srv = bs.Server()
    active = [p for p in (srv.info(i) for i in range(srv.maxclients)) if p["state"] == 4]
    org = {}
    for p in active:
        f = srv.latest(p["i"])
        raw = srv.read(srv.client(p["i"]) + srv.frames_off + f["slot"] * bs.SIZEOF_FRAME + bs.FR_PS + PS_ORIGIN, 12)
        org[p["i"]] = struct.unpack("<fff", raw)
    print("\n-- positions now (ps.origin), server ms {} --".format(srv.time()))
    for p in active:
        o = org[p["i"]]
        print("  {:>2} {:<14} bot={} ({:8.1f} {:8.1f} {:8.1f})".format(
            p["i"], p["name"][:14], int(p["bot"]), o[0], o[1], o[2]))
    humans = [p["i"] for p in active if not p["bot"]]
    bots = [p["i"] for p in active if p["bot"]]
    for h in humans:
        for b in bots:
            d = math.dist(org[h], org[b])
            fr, ents = srv.snapshot(b)
            seen = [e for e in ents or [] if e[0] == h]
            cross = ""
            if fr is not None and seen:
                idx = (fr["first"] + [e[0] for e in ents].index(h)) % srv.numsnap
                tb = struct.unpack("<fff", srv.read(srv.snapents + idx * bs.SIZEOF_ES + ES_TRBASE, 12))
                cross = " (entity trBase {:.0f} {:.0f} {:.0f})".format(*tb)
            print("  client {} -> bot {}: {:7.1f} units, Q3 volume {:.2f}, in bot's snapshot: {}{}".format(
                h, b, d, vol(d), "yes" if seen else "no", cross))


def main():
    global LOG
    n = sys.argv[1] if len(sys.argv) > 1 else "1"
    if n == "pos":
        positions()
        return 0
    runs = sys.argv[2] if len(sys.argv) > 2 else RUNS
    LOG = sys.argv[3] if len(sys.argv) > 3 else LOG
    delivery(find_run(runs, n))
    positions()
    return 0


if __name__ == "__main__":
    sys.exit(main())
