#!/usr/bin/env python3
"""botsnap - what other players are actually *sent* about a mover, read from the
snapshots the server builds for its bots.

The server builds a full snapshot for every bot each frame and skips only the
network send. The entity states in it are copies, in svs.snapshotEntities, of
exactly what a human listener in the bot's place would receive. Reading those
copies out of /proc/<pid>/mem gives the listener's side of the footstep stream
with no second client and no human ear.

Read-only. Nothing is written into the server process and nothing is loaded
into it; reading /proc/<pid>/mem does not stop the process.

Layout: minqlx's quake_common.h for QL x64 (server/botsnap_layout.c prints it).
`svs` and `sv_maxclients` are exported by the minqlx(tended) .so. The lower half
of client_t is marked a guess in that header, so `probe` cross-checks frames[]
against the live process before anything is trusted.

    ./botsnap.py probe              layout checks and one look at every client
    ./botsnap.py record SECS [LOG]  every bot snapshot's view of every other client -> LOG
    ./botsnap.py report [LOG]       what each bot, as a listener, would have played
    ./botsnap.py bands [LOG [FROM TO]]  humans, any gait, by command rate; FROM/TO
                                    in seconds from the start of the log
    ./botsnap.py timeline [LOG]     10 s buckets, to choose a bands window

The report replays cgame's rule for what plays. An entity's event plays when it
differs from the last value seen on that entity (CG_CheckEvents); that memory is
reset once the entity has been out of view for EVENT_VALID_MSEC. An event entity
- the overflow temp entity SendPendingPredictableEvents makes - plays once per
entity number within that window. Network loss is not modelled: this is what a
listener is sent, and would play over a clean link.
"""
import os
import re
import struct
import subprocess
import sys
import time

PORT = os.environ.get("FSVQL_PORT", "27960").encode()
LOG = os.environ.get("BOTSNAP_LOG", "/tmp/fsvql-botsnap/botsnap.tsv")

SIZEOF_CLIENT = 158328
SIZEOF_FRAME = 648
SIZEOF_ES = 236
PACKET_BACKUP = 32

CVAR_INTEGER = 76
SVS_TIME, SVS_CLIENTS, SVS_NUMSNAP, SVS_NEXTSNAP, SVS_SNAPENTS = 4, 16, 24, 28, 32
CL_STATE, CL_GENTITY, CL_NAME = 0, 67648, 67656
# quake_common.h says 67908. Measured 2026-10-03 on our test server from a human
# client: all 32 frames built at 71892 and at no other offset.
CL_FRAMES = 71892
CL_STEAMID = 158320
FR_PS, FR_NUM = 36, 628
PS_CMDTIME, PS_CLIENTNUM, PS_PING = 0, 136, 0x1E0
ES_NUMBER, ES_ETYPE, ES_TRDELTA, ES_OTHER, ES_GROUND, ES_EVENT = 0, 4, 0x24, 148, 0x9C, 188
ES_LEGSANIM = 0xD8
# Q3/TA legs animations, ANIM_TOGGLEBIT (128) masked. Vanilla plays a footstep
# on a bob flip only while running: never while walking (the walk key) or
# crouched, which share the bobCycle store footstepsVQL hooks.
ANIM_TOGGLEBIT = 128
LEGS_RUNNING = (15, 16)            # LEGS_RUN, LEGS_BACK
LEGS_WALKING = (14, 32)            # LEGS_WALK, LEGS_BACKWALK
LEGS_CROUCHED = (13, 23, 31)       # LEGS_WALKCR, LEGS_IDLECR, LEGS_BACKCR
GENT_SVFLAGS = 236 + 244
SVF_BOT = 0x8
ET_EVENTS = 13
ENTITYNUM_NONE = 1023
EVENT_BITS = 0x300
EVENT_VALID_MSEC = 300
FOOTSTEPS = (1, 2, 0x51, 0x52)
MOVING_SPEED = 100.0
STATES = ("free", "zombie", "connected", "primed", "active")
COUNTERS = ("g_ring_written", "g_ring_drained", "g_ring_deferred", "g_suppressed",
            "g_vanilla", "g_hits", "g_move_ms")


def find_pid():
    for d in os.listdir("/proc"):
        if not d.isdigit():
            continue
        try:
            with open("/proc/{}/cmdline".format(d), "rb") as fh:
                raw = fh.read()
        except OSError:
            continue
        if b"qzeroded" in raw and b"net_port\x00" + PORT in raw:
            return int(d)
    raise SystemExit("no qzeroded with net_port {}".format(PORT.decode()))


def engine_module(pid):
    """(base, path) of the mapped minqlx or minqlxtended .so."""
    with open("/proc/{}/maps".format(pid)) as fh:
        for line in fh:
            f = line.split()
            if len(f) >= 6 and re.search(r"/minqlx[a-z]*\.x64\.so$", f[5]) and int(f[2], 16) == 0:
                return int(f[0].split("-")[0], 16), f[5]
    raise SystemExit("no minqlx .so mapped in pid {}".format(pid))


def symbols(path, names):
    out = subprocess.run(["nm", "-D", "--defined-only", path], capture_output=True,
                         text=True, check=True).stdout
    found = {}
    for line in out.splitlines():
        f = line.split()
        if len(f) == 3 and f[2] in names:
            found[f[2]] = int(f[0], 16)
    missing = set(names) - set(found)
    if missing:
        raise SystemExit("symbols not exported: {}".format(", ".join(sorted(missing))))
    return found


class Server:
    def __init__(self):
        self.pid = find_pid()
        base, self.engine = engine_module(self.pid)
        sym = symbols(self.engine, ("svs", "sv_maxclients"))
        self.fd = os.open("/proc/{}/mem".format(self.pid), os.O_RDONLY)
        self.svs = self.u64(base + sym["svs"])
        self.maxclients = self.i32(self.u64(base + sym["sv_maxclients"]) + CVAR_INTEGER)
        self.clients = self.u64(self.svs + SVS_CLIENTS)
        self.numsnap = self.i32(self.svs + SVS_NUMSNAP)
        self.snapents = self.u64(self.svs + SVS_SNAPENTS)

    def read(self, addr, n):
        return os.pread(self.fd, n, addr)

    def i32(self, addr):
        return struct.unpack("<i", self.read(addr, 4))[0]

    def u64(self, addr):
        return struct.unpack("<Q", self.read(addr, 8))[0]

    def time(self):
        return self.i32(self.svs + SVS_TIME)

    def client(self, i):
        return self.clients + i * SIZEOF_CLIENT

    def info(self, i):
        c = self.client(i)
        gent = self.u64(c + CL_GENTITY)
        svflags = self.i32(gent + GENT_SVFLAGS) if gent else 0
        return {
            "i": i, "state": self.i32(c + CL_STATE),
            "name": self.read(c + CL_NAME, 32).split(b"\0")[0].decode("latin-1"),
            "bot": bool(svflags & SVF_BOT), "steam": self.u64(c + CL_STEAMID),
        }

    def frame(self, i, slot):
        raw = self.read(self.client(i) + self.frames_off + slot * SIZEOF_FRAME, SIZEOF_FRAME)
        num, first = struct.unpack_from("<ii", raw, FR_NUM)
        return {"slot": slot, "num": num, "first": first,
                "cmdtime": struct.unpack_from("<i", raw, FR_PS + PS_CMDTIME)[0],
                "clientnum": struct.unpack_from("<i", raw, FR_PS + PS_CLIENTNUM)[0],
                "ping": struct.unpack_from("<i", raw, FR_PS + PS_PING)[0]}

    frames_off = CL_FRAMES
    _slot = {}

    def latest(self, i):
        """The client's most recently built frame. first_entity is the raw,
        ever-increasing ring index at build time, so the newest frame is the one
        with the largest. A bot's netchan never transmits, so it may reuse one
        slot; a human's moves on by one per snapshot. Only the remembered slot and
        the one after it are read on the fast path."""
        s = self._slot.get(i)
        if s is not None:
            a, b = self.frame(i, s), self.frame(i, (s + 1) & 31)
            if b["first"] > a["first"]:
                self._slot[i] = b["slot"]
                return b
            if a["first"] >= b["first"]:
                return a
        frames = [self.frame(i, k) for k in range(PACKET_BACKUP)]
        best = max(frames, key=lambda f: f["first"])
        self._slot[i] = best["slot"]
        return best

    def entities(self, first, num):
        """(number, eType, otherEntityNum, event, xy speed, grounded, legsAnim)
        for each entity a snapshot starting at ring index `first` holds."""
        out = []
        idx = first % self.numsnap
        while num > 0:
            run = min(num, self.numsnap - idx)
            raw = self.read(self.snapents + idx * SIZEOF_ES, run * SIZEOF_ES)
            for k in range(run):
                o = k * SIZEOF_ES
                n, et = struct.unpack_from("<ii", raw, o + ES_NUMBER)
                vx, vy = struct.unpack_from("<ff", raw, o + ES_TRDELTA)
                other, = struct.unpack_from("<i", raw, o + ES_OTHER)
                ground, = struct.unpack_from("<i", raw, o + ES_GROUND)
                ev, = struct.unpack_from("<i", raw, o + ES_EVENT)
                legs, = struct.unpack_from("<i", raw, o + ES_LEGSANIM)
                out.append((n, et, other, ev, (vx * vx + vy * vy) ** 0.5,
                            int(ground != ENTITYNUM_NONE), legs & ~ANIM_TOGGLEBIT))
            num -= run
            idx = 0
        return out

    def snapshot(self, i):
        """The bot's latest snapshot, read until its frame record is stable, so a
        read that overlaps the server building it is never used."""
        for _ in range(4):
            a = self.latest(i)
            ents = self.entities(a["first"], a["num"]) if a["num"] > 0 else []
            b = self.latest(i)
            if (a["first"], a["num"]) == (b["first"], b["num"]):
                return a, ents
        return None, None


def frames_offset(srv, i):
    """Offsets in client_t where frame-sized records look like built snapshots
    for client i: ps.clientNum == i, 1..256 entities, and first_entity inside the
    window the ring still holds. Returns (offset, matching slots), best first."""
    nxt = srv.i32(srv.svs + SVS_NEXTSNAP)
    lo, hi = 1000, SIZEOF_CLIENT - PACKET_BACKUP * SIZEOF_FRAME
    blob = srv.read(srv.client(i), SIZEOF_CLIENT)
    hits = []
    for k in range(lo, hi, 4):
        good = 0
        for j in range(PACKET_BACKUP):
            o = k + j * SIZEOF_FRAME
            num, first = struct.unpack_from("<ii", blob, o + FR_NUM)
            cnum, = struct.unpack_from("<i", blob, o + FR_PS + PS_CLIENTNUM)
            if cnum == i and 1 <= num <= 256 and nxt - srv.numsnap < first <= nxt:
                good += 1
        if good:
            hits.append((k, good))
    hits.sort(key=lambda h: -h[1])
    return hits[:5]


def built_here(srv, i):
    """Slots of client i, at srv.frames_off, that look like a snapshot built for
    it: its own ps.clientNum, 1..256 entities, first_entity inside the ring."""
    nxt = srv.i32(srv.svs + SVS_NEXTSNAP)
    return [f for f in (srv.frame(i, k) for k in range(PACKET_BACKUP))
            if f["clientnum"] == i and 1 <= f["num"] <= 256 and nxt - srv.numsnap < f["first"] <= nxt]


def calibrate(srv):
    """Place frames[]. With a human connected, search for it: all 32 of a
    human's frames are built. Otherwise check the measured offset against every
    bot. Returns a line describing the result, or raises."""
    active = [p for p in (srv.info(i) for i in range(srv.maxclients)) if p["state"] == 4]
    for p in active:
        if p["bot"]:
            continue
        hits = frames_offset(srv, p["i"])
        if hits and hits[0][1] >= PACKET_BACKUP // 2 and (len(hits) == 1 or hits[1][1] < hits[0][1]):
            srv.frames_off = hits[0][0]
            return "frames[] at +{} (measured default {}), {} of 32 slots of human client {} " \
                   "look built".format(hits[0][0], CL_FRAMES, hits[0][1], p["i"])
        raise SystemExit("cannot place frames[] from client {}: {}".format(p["i"], hits))
    bots = [p["i"] for p in active if p["bot"]]
    if not bots:
        raise SystemExit("nobody connected to check frames[] against")
    ok = [b for b in bots if built_here(srv, b)]
    if len(ok) != len(bots):
        raise SystemExit("frames[] at +{} does not hold a built snapshot for bots {}".format(
            CL_FRAMES, sorted(set(bots) - set(ok))))
    return "frames[] at +{} (measured default), a built snapshot found for all {} bots".format(
        CL_FRAMES, len(bots))


def probe():
    srv = Server()
    print("pid {}  engine {}".format(srv.pid, srv.engine))
    print("svs 0x{:x}  time {}  sv_maxclients {}".format(srv.svs, srv.time(), srv.maxclients))
    print("clients 0x{:x}  numSnapshotEntities {}  next {}  snapshotEntities 0x{:x}".format(
        srv.clients, srv.numsnap, srv.i32(srv.svs + SVS_NEXTSNAP), srv.snapents))
    if not (0 < srv.maxclients <= 64) or srv.numsnap <= 0:
        raise SystemExit("svs does not look right; stopping")
    print("  numSnapshotEntities / (maxclients * PACKET_BACKUP) = {}".format(
        srv.numsnap / (srv.maxclients * PACKET_BACKUP)))
    print()

    people = [srv.info(i) for i in range(srv.maxclients)]
    people = [p for p in people if p["state"] != 0]
    print("{:>3} {:<10} {:<22} {:<4} {:>20}".format("cl", "state", "name", "bot", "steam id"))
    for p in people:
        print("{:>3} {:<10} {:<22} {:<4} {:>20}".format(
            p["i"], STATES[p["state"]] if 0 <= p["state"] < 5 else p["state"], p["name"][:22],
            "yes" if p["bot"] else "no", p["steam"]))
    print()

    active = [p for p in people if p["state"] == 4]
    bots = [p for p in active if p["bot"]]
    humans = [p for p in active if not p["bot"]]
    print(calibrate(srv))
    for p in bots[:2]:
        print("client {} ({}, bot): frames[] candidates (offset, slots) {}".format(
            p["i"], p["name"], frames_offset(srv, p["i"])))
    for p in humans[:1] + bots[:3]:
        slots = [srv.frame(p["i"], s) for s in range(PACKET_BACKUP)]
        filled = [s for s in slots if s["clientnum"] == p["i"] and s["num"] > 0]
        print("client {:>2} {:<5} at that offset: slots built for it {:>2}/32, "
              "distinct first_entity {:>2}".format(p["i"], "bot" if p["bot"] else "human",
                                                   len(filled), len({s["first"] for s in filled})))
    for p in humans[:1]:
        fr, ents = srv.snapshot(p["i"])
        if fr is None:
            print("  client {}: no stable read".format(p["i"]))
            continue
        nums = [e[0] for e in ents]
        print("client {} latest snapshot: {} entities, numbers strictly increasing {}, "
              "own entity absent {}, ps.clientNum {}".format(
                  p["i"], len(ents), all(x < y for x, y in zip(nums, nums[1:])),
                  p["i"] not in nums, fr["clientnum"]))
    if not bots:
        print("\nno bot connected; nothing more to probe")
        return 0

    b = bots[0]
    print("\n== bot {} ({}): sampling its latest frame every 2 ms for 2 s".format(b["i"], b["name"]))
    seen, slots = [], set()
    end = time.monotonic() + 2.0
    while time.monotonic() < end:
        fr = srv.latest(b["i"])
        if not seen or fr["first"] != seen[-1][0]:
            seen.append((fr["first"], fr["num"], srv.time()))
            slots.add(fr["slot"])
        time.sleep(0.002)
    deltas = {}
    for k in range(len(seen) - 1):
        d = seen[k + 1][2] - seen[k][2]
        deltas[d] = deltas.get(d, 0) + 1
    print("  frame slots it was built into: {}".format(sorted(slots)))
    print("  distinct snapshots {}; server time between them: {}".format(
        len(seen), ", ".join("{} ms x{}".format(k, v) for k, v in sorted(deltas.items()))))

    fr, ents = srv.snapshot(b["i"])
    if fr is None:
        print("  could not get a stable read")
        return 1
    nums = [e[0] for e in ents]
    print("\n== latest snapshot of bot {}: {} entities".format(b["i"], fr["num"]))
    print("  numbers strictly increasing: {}".format(all(x < y for x, y in zip(nums, nums[1:]))))
    print("  players in it: {}".format([e[0] for e in ents if e[0] < srv.maxclients]))
    for e in ents:
        if e[0] < srv.maxclients or e[1] > ET_EVENTS:
            print("   ent {:>4} eType {:>4} other {:>4} event 0x{:x} speed {:.0f} ground {} "
                  "legs {}".format(*e))
    return 0


GAITS = ("still or airborne", "running", "walking", "crouched", "other")


def gait(e):
    """0 still or airborne, 1 running, 2 walking (walk key), 3 crouched,
    4 moving on the ground in some other animation."""
    speed, ground, legs = e[4], e[5], e[6]
    if not ground:
        return 0
    if legs in LEGS_RUNNING and speed > MOVING_SPEED:
        return 1
    if legs in LEGS_WALKING and speed > 30:
        return 2
    if legs in LEGS_CROUCHED and speed > 30:
        return 3
    return 4 if speed > MOVING_SPEED else 0


def record(secs, log):
    """Every bot is a listener and every other active client a mover. Per
    (listener, mover) pair the log gets only what the replay needs:

      V t b m 0|1         the mover left or entered the listener's snapshot
      E t b m ev gait     the mover's entity event changed (and on every re-entry)
      T t b m ev gait     a new overflow event entity for the mover (cgame's
                          once-per-entity rule within EVENT_VALID_MSEC applied)
      Q sec b m n present run walk crouch other   per second: snapshots, in
                          view, and in view per gait
      R t i bot name      roster
      C t counters...     footstepsVQL counters, once a second
    """
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import steps
    srv = Server()
    note = calibrate(srv)
    print(note)
    try:
        counters = steps.Target()
    except SystemExit as e:
        counters = None
        print("footstepsVQL counters unavailable: {}".format(e))
    os.makedirs(os.path.dirname(log), exist_ok=True)
    fh = open(log, "a", buffering=1)
    fh.write("# start pid {} wall {:.1f} svs.time {}; {}\n".format(
        srv.pid, time.time(), srv.time(), note))
    last_first, bots, everyone, pair = {}, [], [], {}
    next_roster = next_counter = 0.0
    seen = torn = 0
    end = time.monotonic() + secs
    while time.monotonic() < end:
        now = time.monotonic()
        if now >= next_roster:
            active = [p for p in (srv.info(i) for i in range(srv.maxclients)) if p["state"] == 4]
            nb, ne = [p["i"] for p in active if p["bot"]], [p["i"] for p in active]
            if nb != bots or ne != everyone:
                t = srv.time()
                for p in active:
                    fh.write("R\t{}\t{}\t{}\t{}\n".format(t, p["i"], int(p["bot"]), p["name"]))
            bots, everyone = nb, ne
            next_roster = now + 1.0
        if counters and now >= next_counter:
            try:
                s = counters.snap()
                fh.write("C\t{}\t{}\t{}\n".format(srv.time(), "\t".join(str(s[k]) for k in COUNTERS),
                                                  "\t".join(str(x) for x in s["dt"])))
            except OSError:
                counters = None
            next_counter = now + 1.0
        for b in bots:
            head = srv.latest(b)
            if head["num"] <= 0 or head["first"] == last_first.get(b):
                continue
            fr, ents = srv.snapshot(b)
            if fr is None:
                torn += 1
                continue
            last_first[b] = fr["first"]
            t = srv.time()
            players = {e[0]: e for e in ents if e[0] < srv.maxclients}
            temps = {}
            for e in ents:
                if e[1] > ET_EVENTS:
                    temps.setdefault(e[2], []).append((e[0], e[1] - ET_EVENTS))
            sec = t // 1000
            for m in everyone:
                if m == b:
                    continue
                st = pair.get((b, m))
                if st is None:
                    st = pair[(b, m)] = {"present": 0, "ev": None, "sec": sec, "n": 0, "in": 0,
                                         "g": [0] * 5, "temps": {}}
                if st["sec"] != sec:
                    fh.write("Q\t{}\t{}\t{}\t{}\t{}\t{}\n".format(
                        st["sec"], b, m, st["n"], st["in"], "\t".join(str(x) for x in st["g"][1:])))
                    st["sec"], st["n"], st["in"], st["g"] = sec, 0, 0, [0] * 5
                st["n"] += 1
                e = players.get(m)
                moving = 0
                if e is not None:
                    moving = gait(e)
                    st["in"] += 1
                    st["g"][moving] += 1
                    if not st["present"]:
                        fh.write("V\t{}\t{}\t{}\t1\n".format(t, b, m))
                        st["present"] = 1
                        st["ev"] = None
                    if e[3] != st["ev"]:
                        fh.write("E\t{}\t{}\t{}\t{}\t{}\n".format(t, b, m, e[3], moving))
                        st["ev"] = e[3]
                elif st["present"]:
                    fh.write("V\t{}\t{}\t{}\t0\n".format(t, b, m))
                    st["present"] = 0
                for num, raw in temps.get(m, ()):
                    last = st["temps"].get(num)
                    st["temps"][num] = t
                    if last is None or t - last > EVENT_VALID_MSEC:
                        fh.write("T\t{}\t{}\t{}\t{}\t{}\n".format(t, b, m, raw, moving))
            seen += 1
        time.sleep(0.002)
    fh.write("# end svs.time {} bot snapshots {} torn {}\n".format(srv.time(), seen, torn))
    fh.close()
    print("recorded {} bot snapshots ({} unstable reads dropped) to {}".format(seen, torn, log))
    return 0


def load(log):
    rows, ctr, roster, quality = {}, [], {}, {}
    with open(log) as fh:
        for line in fh:
            p = line.rstrip("\n").split("\t")
            k = p[0]
            if k in ("V", "E", "T"):
                t, b, m = int(p[1]), int(p[2]), int(p[3])
                rows.setdefault((b, m), []).append((t, k, [int(x) for x in p[4:]]))
            elif k == "Q":
                v = [int(x) for x in p[1:]]
                sec, b, m, n, inv = v[:5]
                quality.setdefault((b, m), {})[sec] = (n, inv, [0] + v[5:9])
            elif k == "R":
                roster[int(p[2])] = (int(p[3]), p[4])
            elif k == "C":
                ctr.append([int(x) for x in p[1:]])
    return rows, quality, roster, ctr


def replay(rows):
    """cgame's playback for one listener and one mover, from V/E/T rows. Returns
    the footsteps it plays as (t, path, gait) and the count of other events."""
    played, other = [], 0
    prev_ev, left_at = 0, None
    for t, kind, v in rows:
        if kind == "V":
            if v[0] == 0:
                left_at = t
            elif left_at is not None and t - left_at > EVENT_VALID_MSEC:
                prev_ev = 0
        elif kind == "E":
            ev, g = v
            if ev != prev_ev:
                prev_ev = ev
                if (ev & ~EVENT_BITS) in FOOTSTEPS:
                    played.append((t, "entity", g))
                elif ev & ~EVENT_BITS:
                    other += 1
        elif kind == "T":
            raw, g = v
            if (raw & ~EVENT_BITS) in FOOTSTEPS:
                played.append((t, "overflow", g))
    return played, other


def report(log):
    rows, quality, roster, ctr = load(log)
    if not quality:
        print("no bot snapshots in {}".format(log))
        return 1
    ns = [n for q in quality.values() for (n, _, _) in q.values() if n]
    per_sec = max(set(ns), key=ns.count)
    print("log {}".format(log))
    print("  bot snapshots per second, per listener: {} (most common)".format(per_sec))
    print("  listener-seconds under 90% of that (poller missed some, or a join): {} of {}".format(
        sum(1 for n in ns if n < 0.9 * per_sec), len(ns)))
    print("  a full second: every snapshot of it had the mover in view, on the ground, in one gait")
    print()

    # cells[(kind, gait)] -> per (listener, mover) results over its full seconds;
    # totals[(kind, gait)] -> [seconds in view in that gait, footsteps played in it]
    cells, totals = {}, {}
    for (b, m), q in sorted(quality.items()):
        played, other = replay(sorted(rows.get((b, m), []), key=lambda r: r[0]))
        steps_in = {}
        for t, path, g in played:
            steps_in[t // 1000] = steps_in.get(t // 1000, 0) + 1
        kind = "bot" if roster.get(m, (1, ""))[0] else "human"
        for g in range(1, 5):
            t_in = sum(gs[g] for (n, inv, gs) in q.values()) / float(per_sec)
            n_in = sum(1 for p in played if p[2] == g)
            tot = totals.setdefault((kind, g), [0.0, 0])
            tot[0] += t_in
            tot[1] += n_in
        for g in (1, 2, 3):
            full = {sec for sec, (n, inv, gs) in q.items() if n >= 0.9 * per_sec and gs[g] == n}
            if not full:
                continue
            # Gaps only between footsteps inside one unbroken run of full seconds.
            mt = [t for t, path, gg in played if gg == g and t // 1000 in full]
            cells.setdefault((kind, g), []).append({
                "b": b, "m": m, "secs": len(full),
                "steps": sum(steps_in.get(sec, 0) for sec in full),
                "hist": [min(steps_in.get(sec, 0), 4) for sec in full],
                "gaps": [y - x for x, y in zip(mt, mt[1:])
                         if all(s in full for s in range(x // 1000, y // 1000 + 1))],
                "paths": [p[1] for p in played if p[0] // 1000 in full]})

    for kind in ("human", "bot"):
        print("movers that are {}s, heard by bots".format(kind))
        for g in range(1, 5):
            secs_in, n_in = totals.get((kind, g), (0.0, 0))
            if secs_in:
                print("  any time {:<9} {:>7.1f} listener-s in view, {:>5} footsteps played = "
                      "{:.3f}/s".format(GAITS[g], secs_in, n_in, n_in / secs_in))
        any_cell = False
        for g in (1, 2, 3):
            c = cells.get((kind, g))
            if not c:
                continue
            any_cell = True
            secs = sum(x["secs"] for x in c)
            steps_n = sum(x["steps"] for x in c)
            hist = [h for x in c for h in x["hist"]]
            paths = [p for x in c for p in x["paths"]]
            print("  {:<9} {:>5} listener-s, {:.3f} footsteps/s played; seconds with 0/1/2/3/4+: {};"
                  " entity {} overflow {}".format(
                      GAITS[g], secs, steps_n / secs,
                      " / ".join("{:.0f}%".format(100.0 * hist.count(k) / len(hist)) for k in range(5)),
                      paths.count("entity"), paths.count("overflow")))
            for m in sorted({x["m"] for x in c}):
                mine = [x for x in c if x["m"] == m]
                s = sum(x["secs"] for x in mine)
                print("      client {:>2} {:<16} {:>4} listener-s, {:.3f}/s, {} listener(s)".format(
                    m, roster.get(m, (0, "?"))[1][:16], s,
                    sum(x["steps"] for x in mine) / s, len(mine)))
            gaps = sorted(gp for x in c for gp in x["gaps"])
            if gaps and g == 1:
                q = lambda f: gaps[min(len(gaps) - 1, int(f * len(gaps)))]
                print("      gap between played footsteps: p5 {} ms, median {} ms, p95 {} ms, "
                      "over 700 ms {} of {}".format(q(0.05), q(0.5), q(0.95),
                                                   sum(1 for x in gaps if x > 700), len(gaps)))
        if not any_cell:
            print("  none")
        print()

    if len(ctr) >= 2:
        a, z = ctr[0], ctr[-1]
        d = [y - x for x, y in zip(a, z)]
        print("footstepsVQL counters over the same window (all movers, process-global):")
        print("  injected {} delivered {} deferred {} | vanilla {} stripped {} | moving {:.1f} s".format(
            d[1], d[2], d[3], d[5], d[4], d[7] / 1000.0))
        names = ("no-prev", "1ms", "2ms", "3ms", "4ms", "5-8ms", "9-32ms", ">32ms")
        print("  command intervals: {}".format(", ".join(
            "{} {}".format(nm, c) for nm, c in zip(names, d[8:16]) if c)))
    return 0


BANDS = ((1, "~1000 fps (1 ms)"), (2, "~500 fps (2 ms)"), (23, "~400 fps (2 and 3 ms)"),
         (3, "~333 fps (3 ms)"), (4, "250 fps (4 ms)"))


def band_for(fast):
    """The band of one second from its 1, 2, 3, 4 ms command counts, or None.
    400 fps alternates 2 and 3 ms, so neither alone reaches 60%."""
    tot = sum(fast)
    k = max(range(4), key=lambda i: fast[i])
    if fast[k] >= 0.6 * tot:
        return k + 1
    if fast[1] + fast[2] >= 0.8 * tot:
        return 23
    return None


def timeline(log):
    """Ten-second buckets of a human's movement as the bots saw it: seconds at
    each command rate, footsteps heard per listener-second in view, and the
    share of in-view snapshots off the ground or still (stairs show up here).
    Seconds are counted from the start of the log, to pick a `bands` window."""
    rows, quality, roster, ctr = load(log)
    if not quality or len(ctr) < 2:
        print("not enough data in {}".format(log))
        return 1
    t0 = ctr[0][0] // 1000
    rate = {}
    for a, z in zip(ctr, ctr[1:]):
        d = [y - x for x, y in zip(a, z)]
        fast = d[9:13]
        if sum(fast) >= 50:
            rate[(a[0] + z[0]) // 2000] = 1 + max(range(4), key=lambda i: fast[i])
    buckets = {}
    for (b, m), q in quality.items():
        if roster.get(m, (1, ""))[0]:
            continue
        played, other = replay(sorted(rows.get((b, m), []), key=lambda r: r[0]))
        heard = {}
        for t, path, g in played:
            heard[t // 1000] = heard.get(t // 1000, 0) + 1
        for sec, (n, inv, gs) in q.items():
            if not inv:
                continue
            k = (sec - t0) // 10
            bk = buckets.setdefault(k, [0, 0, 0])
            bk[0] += inv
            bk[1] += inv - sum(gs[1:])
            bk[2] += heard.get(sec, 0) * inv / float(n)
    print("{:>9}  {:>15}  {:>13}  {:>12}".format("from s", "ms per command", "footsteps/s", "air or still"))
    for k in sorted(buckets):
        secs = [s for s in range(t0 + 10 * k, t0 + 10 * k + 10) if s in rate]
        mix = {}
        for s in secs:
            mix[rate[s]] = mix.get(rate[s], 0) + 1
        inv, still, heard = buckets[k]
        print("{:>9}  {:>15}  {:>13.2f}  {:>11.0f}%".format(
            10 * k, " ".join("{}ms:{}".format(r, c) for r, c in sorted(mix.items())) or "-",
            heard * per_sec_of(quality) / inv if inv else 0.0, 100.0 * still / inv if inv else 0))
    return 0


def per_sec_of(quality):
    ns = [n for q in quality.values() for (n, _, _) in q.values() if n]
    return max(set(ns), key=ns.count)


def bands(log, lo=None, hi=None):
    """Every footstep a bot heard from a human, in seconds where the bot had him
    in view the whole second, split by the human's command rate. Stairs and
    jumps included: a footstep is counted whatever the gait, as a listener would
    hear it.

    The rate comes from footstepsVQL's own interval histogram, logged once a
    second. Bots command every 25 ms or slower, so the 1-4 ms buckets are the
    humans'. A second belongs to a band when one bucket holds at least 60% of the
    1-4 ms commands (or 2 and 3 ms together hold 80%, the 400 fps band), and
    counts as moving when there were at least 50 of them."""
    rows, quality, roster, ctr = load(log)
    if not quality or len(ctr) < 2:
        print("not enough data in {}".format(log))
        return 1
    ns = [n for q in quality.values() for (n, _, _) in q.values() if n]
    per_sec = max(set(ns), key=ns.count)

    band_of = {}
    t0 = ctr[0][0] // 1000
    for a, z in zip(ctr, ctr[1:]):
        d = [y - x for x, y in zip(a, z)]
        fast = d[9:13]                       # dt buckets 1, 2, 3, 4 ms
        tot = sum(fast)
        if tot < 50:
            continue
        rel = (a[0] + z[0]) // 2000 - t0
        if (lo is not None and rel < lo) or (hi is not None and rel >= hi):
            continue
        band = band_for(fast)
        if band is not None:
            # band, injected, vanilla, stripped, deferred
            band_of[(a[0] + z[0]) // 2000] = (band, d[1], d[5], d[4], d[3])

    print("log {}".format(log))
    print("  seconds with a human moving, by command rate: {}".format(", ".join(
        "{} {}".format(name, sum(1 for b in band_of.values() if b[0] == k))
        for k, name in BANDS if any(b[0] == k for b in band_of.values()))))
    print()
    for k, name in BANDS:
        secs_k = {s for s, b in band_of.items() if b[0] == k}
        if not secs_k:
            continue
        listener_s, steps, hist, gaps, paths = 0, 0, [], [], []
        for (b, m), q in sorted(quality.items()):
            if roster.get(m, (1, ""))[0]:
                continue
            played, other = replay(sorted(rows.get((b, m), []), key=lambda r: r[0]))
            full = {s for s in secs_k
                    if s in q and q[s][0] >= 0.9 * per_sec and q[s][1] == q[s][0]}
            if not full:
                continue
            by_sec = {}
            for t, path, g in played:
                if t // 1000 in full:
                    by_sec[t // 1000] = by_sec.get(t // 1000, 0) + 1
                    paths.append(path)
            listener_s += len(full)
            steps += sum(by_sec.values())
            hist += [min(by_sec.get(s, 0), 4) for s in full]
            mt = [t for t, path, g in played if t // 1000 in full]
            gaps += [y - x for x, y in zip(mt, mt[1:])
                     if all(s in full for s in range(x // 1000, y // 1000 + 1))]
        inj = sum(b[1] for s, b in band_of.items() if b[0] == k)
        van = sum(b[2] for s, b in band_of.items() if b[0] == k)
        strip = sum(b[3] for s, b in band_of.items() if b[0] == k)
        defer = sum(b[4] for s, b in band_of.items() if b[0] == k)
        print("{}: {} s moving; counters (all movers): {} injected, {} deferred, {} vanilla, "
              "{} vanilla stripped from listeners".format(name, len(secs_k), inj, defer, van, strip))
        if not listener_s:
            print("  no listener had the mover in view for a whole second")
            print()
            continue
        print("  heard by bots: {} listener-s, {:.3f} footsteps/s; seconds with 0/1/2/3/4+: {}".format(
            listener_s, steps / listener_s,
            " / ".join("{:.0f}%".format(100.0 * hist.count(i) / len(hist)) for i in range(5))))
        if gaps:
            gaps.sort()
            q = lambda f: gaps[min(len(gaps) - 1, int(f * len(gaps)))]
            print("  gaps: p5 {} ms, median {} ms, p95 {} ms, under 150 ms {}, over 700 ms {} "
                  "(of {})".format(q(0.05), q(0.5), q(0.95), sum(1 for x in gaps if x < 150),
                                   sum(1 for x in gaps if x > 700), len(gaps)))
        print("  by path: entity {}, overflow {}".format(paths.count("entity"),
                                                         paths.count("overflow")))
        print()
    return 0


def run_stats(rows, quality, roster, ctr, per_sec, replays, start, end, m):
    """What the bots heard from client m between server times start and end
    (ms): only whole seconds strictly inside the window, and only seconds a
    listener had him in view in every snapshot."""
    secs = set(range(start // 1000 + 1, end // 1000))
    r = {"lsecs": 0, "steps": 0, "hist": [], "gaps": [], "paths": []}
    for (b, mm), q in quality.items():
        if mm != m:
            continue
        full = {s for s in secs if s in q and q[s][0] >= 0.9 * per_sec and q[s][1] == q[s][0]}
        if not full:
            continue
        if (b, m) not in replays:
            replays[(b, m)] = replay(sorted(rows.get((b, m), []), key=lambda x: x[0]))[0]
        played = [p for p in replays[(b, m)] if p[0] // 1000 in full]
        by = {}
        for t, path, g in played:
            by[t // 1000] = by.get(t // 1000, 0) + 1
            r["paths"].append(path)
        r["lsecs"] += len(full)
        r["steps"] += len(played)
        r["hist"] += [by.get(s, 0) for s in full]
        ts = [p[0] for p in played]
        r["gaps"] += [y - x for x, y in zip(ts, ts[1:])
                      if all(s in full for s in range(x // 1000, y // 1000 + 1))]
    inside = [c for c in ctr if start <= c[0] <= end]
    r["ctr"] = [y - x for x, y in zip(inside[0], inside[-1])] if len(inside) >= 2 else None
    return r


def runs(runs_path, log):
    """One line per conductor run, from the conductor's RUNS file and this log."""
    rows, quality, roster, ctr = load(log)
    if not quality:
        print("no bot snapshots in {}".format(log))
        return 1
    per_sec = per_sec_of(quality)
    runs_ = []
    with open(runs_path) as fh:
        for line in fh:
            p = line.rstrip("\n").split("\t")
            if p and p[0].isdigit():
                runs_.append(p)
    replays = {}
    print("log {}\nruns {}\n".format(log, runs_path))
    print("{:>3} {:>5} {:<5} {:<22} {:>5} {:>7} {:>6} {:>5} {:>6} {:>17} {:>7} {:>5} {:>5} {:>9} {:>8}".format(
        "run", "fps", "kind", "label", "rtt+", "ping", "cmd ms", "lis-s", "fs/s", "secs 1/2/3/4+",
        "gap med", "<150", ">700", "ent/ovf", "inj/strp"))
    for p in runs_:
        n, start, end, client, fps, kind, label, status = p[:8]
        ping = p[8] if len(p) > 8 else "?"
        netem = p[9] if len(p) > 9 else "?"
        start, end, client = int(start), int(end), int(client)
        r = run_stats(rows, quality, roster, ctr, per_sec, replays, start, end, client)
        mix = "-"
        inj = "-"
        if r["ctr"]:
            fast = r["ctr"][9:13]
            if sum(fast):
                mix = "{:.2f}".format(sum((i + 1) * c for i, c in enumerate(fast)) / float(sum(fast)))
            inj = "{}/{}".format(r["ctr"][1], r["ctr"][4])
        if r["lsecs"]:
            h = r["hist"]
            dist = "/".join("{:.0f}".format(100.0 * sum(1 for x in h if min(x, 4) == k) / len(h))
                            for k in (1, 2, 3, 4))
            gaps = sorted(r["gaps"])
            med = gaps[len(gaps) // 2] if gaps else 0
            near = "{:.0f}%".format(100.0 * sum(1 for g in gaps if g < 150) / len(gaps)) if gaps else "-"
            hole = "{:.0f}%".format(100.0 * sum(1 for g in gaps if g > 700) / len(gaps)) if gaps else "-"
            line = "{:>6} {:>5} {:>6.3f} {:>17} {:>7} {:>5} {:>5} {:>9}".format(
                mix, r["lsecs"], r["steps"] / float(r["lsecs"]), dist, med, near, hole,
                "{}/{}".format(r["paths"].count("entity"), r["paths"].count("overflow")))
        else:
            line = "{:>6}  no listener had client {} in view for a whole second".format(mix, client)
        flag = "" if status == "ok" else "  [{}]".format(status)
        print("{:>3} {:>5} {:<5} {:<22} {:>5} {:>7} {} {:>8}{}".format(
            n, fps, kind, label[:22], netem, ping, line, inj, flag))
    print("\nrtt+: delay the rig added (netem, round trip ms). ping: the mover's ps.ping at start/end.")
    print("cmd ms: mean of the mover's 1-4 ms command intervals in the window (1000/x = fps).")
    print("secs 1/2/3/4+: % of listener-seconds with that many footsteps played.")
    print("<150 near-doubles, >700 holes, as % of gaps between played footsteps.")
    print("inj/strp: clock footsteps injected / stock footsteps stripped (all movers).")
    return 0


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "probe"
    if cmd == "runs":
        return runs(sys.argv[2] if len(sys.argv) > 2 else "/tmp/fsvql-botsnap/runs.tsv",
                    sys.argv[3] if len(sys.argv) > 3 else LOG)
    if cmd == "bands":
        window = [int(x) for x in sys.argv[3:5]]
        return bands(sys.argv[2] if len(sys.argv) > 2 else LOG, *window)
    if cmd == "timeline":
        return timeline(sys.argv[2] if len(sys.argv) > 2 else LOG)
    if cmd == "probe":
        return probe()
    if cmd == "record":
        return record(int(sys.argv[2]) if len(sys.argv) > 2 else 300,
                      sys.argv[3] if len(sys.argv) > 3 else LOG)
    if cmd == "report":
        return report(sys.argv[2] if len(sys.argv) > 2 else LOG)
    raise SystemExit(__doc__)


if __name__ == "__main__":
    sys.exit(main())
