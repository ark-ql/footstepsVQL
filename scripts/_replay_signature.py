"""Characterise the event replay: what precedes it, and how tightly.

The fix (F2) needs two numbers that must come from data, not from feel:

  1. How long after its own first play does a replay arrive? That sets the
     minimum ledger window.
  2. How long after the *previous event of any kind* does a replay arrive?
     If replays always land in the same drain batch as another event while
     legitimate repeats never do, that is a far sharper discriminator than
     a time window alone, and it is what stops the ledger eating the second
     shot of an LG burst.

It is also the mover check for footstepsVQL: the player's own jumps, shots and
landings must never play twice. The last line of each log is a PASS or FAIL
on that, with the local event counts behind it.

Usage:
    python scripts/_replay_signature.py bin/ql_sndtap_*.log [--ent N]

--ent is the local player's client number. Without it, the entity whose events
arrive through cgame's predictable-playerState call site (+0x43545) is used,
since only the local player's own events take that path.
"""

import argparse
import re
from collections import Counter
from pathlib import Path

EVT = re.compile(
    r"^evt t=([\d.]+) ent=(-?\d+) ev=(\d+) bits=0x([0-9a-f]+) "
    # ret is `+0xRVA` in tap v1-v3 and `e+0xRVA` / `c+0xRVA` / `?0xADDR`
    # from v4 onward, where the module the caller sits in is named.
    r"etype=(-?\d+) ret=[ec]?[+?]0x([0-9a-fA-F]+)"
)

NAMES = {
    1: "FOOTSTEP", 2: "FOOTSTEP_METAL", 6: "FALL_SHORT", 7: "FALL_MEDIUM",
    8: "FALL_FAR", 9: "JUMP_PAD", 10: "JUMP", 18: "CHANGE_WEAPON",
    20: "FIRE_WEAPON", 39: "TELEPORT_IN", 48: "MISSILE_MISS",
    50: "RAILTRAIL", 82: "FOOTSTEP_WOOD",
}

# Candidacy window, and it is load-bearing, not a formality. A legitimate
# NEW event can drain in the same frame as a footstep — a nailgun shot on
# its ~50 ms cadence does exactly that — so the batch test alone produces
# false positives. Only a repeat that is ALSO too soon to be cadence counts.
# Must match ledger_window_us in the DLL or the two disagree.
WINDOW_US = 40000.0

# Wider net used only for reporting the near-misses, never for counting.
DUP_US = 150000.0

# A true replay is emitted in the same ring drain as the event that
# displaced it, so it lands microseconds behind its predecessor. MEASURED
# on our test server: replays at 9-10 us, closest legitimate event 533 us.
# Anything further out is a legitimate fast repeat (LG fires every ~50 ms)
# and counting it as a replay is the mistake this constant exists to avoid.
BATCH_US = 100.0

# cgame's predictable-playerState call into CG_EntityEvent, measured
# (notes/phase2-replay-confirmed.md). Only the local player's events use it.
PS_CALLER_RVA = "43545"


def local_ent(evts):
    """(client number, how it was found) for the local player."""
    via_ps = Counter(e["ent"] for e in evts if e["ret"].lower() == PS_CALLER_RVA)
    if via_ps:
        ent, n = via_ps.most_common(1)[0]
        return ent, "{} events via the playerState path +0x{}".format(n, PS_CALLER_RVA)
    return 0, "no playerState-path events; assumed 0"


def name(n):
    return NAMES.get(n, f"ev{n}")


def load(path):
    out = []
    for lineno, raw in enumerate(path.read_text(errors="replace").splitlines(), 1):
        m = EVT.match(raw.strip())
        if m:
            out.append({
                "line": lineno, "t": float(m.group(1)), "ent": int(m.group(2)),
                "ev": int(m.group(3)), "bits": int(m.group(4), 16),
                "ret": m.group(6),
            })
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("logs", nargs="+")
    ap.add_argument("--ent", type=int, default=None,
                    help="local player's client number (default: detected)")
    args = ap.parse_args()
    for arg in args.logs:
        path = Path(arg)
        if not path.exists():
            print(f"missing: {path}")
            continue
        evts = load(path)
        # The replay is a predictable-playerstate phenomenon: local player only.
        if args.ent is None:
            me, how = local_ent(evts)
        else:
            me, how = args.ent, "given with --ent"
        p = [e for e in evts if e["ent"] == me]

        last, cands, dup_ids = {}, [], set()
        for i, e in enumerate(p):
            k = e["ev"]
            if k in last and (e["t"] - last[k]["t"]) <= DUP_US:
                cands.append((e, last[k], p[i - 1] if i else None))
                dup_ids.add(id(e))
            last[k] = e

        true_replays, cadence, batch_only = [], [], []
        for c in cands:
            e, first, prev = c
            in_batch = prev is not None and (e["t"] - prev["t"]) <= BATCH_US
            in_window = (e["t"] - first["t"]) <= WINDOW_US
            if in_batch and in_window:
                true_replays.append(c)
            elif in_batch:
                batch_only.append(c)   # same drain, but too old to be a replay
            else:
                cadence.append(c)

        print(f"=== {path.name} ===")
        print(f"  local player: client {me} ({how})")
        print(f"  events total {len(evts)}, local player {len(p)}")
        print(f"  same-event repeats within {DUP_US/1000:g} ms : {len(cands)}")
        print(f"    TRUE REPLAYS   (<= {BATCH_US:g} us behind previous event"
              f" AND <= {WINDOW_US/1000:g} ms since own first play): "
              f"{len(true_replays)}")
        print(f"    same drain but outside the window (NOT replays, a new"
              f" event that drained with a footstep): {len(batch_only)}")
        print(f"    legitimate fast repeats (cadence): {len(cadence)}")

        for e, first, prev in true_replays:
            print(f"    >> {name(e['ev'])} REPLAY, line {e['line']}")
            print(f"         since its own first play : "
                  f"{(e['t'] - first['t'])/1000.0:9.3f} ms")
            print(f"         since previous event     : "
                  f"{(e['t'] - prev['t'])/1000.0:9.3f} ms"
                  f"   <- {name(prev['ev'])}")
        for e, first, prev in batch_only:
            print(f"    -- {name(e['ev'])} same-drain but "
                  f"{(e['t'] - first['t'])/1000.0:.1f} ms since its own last"
                  f" play (line {e['line']}) -> cadence, correctly NOT a replay")
        if cadence:
            by_type = {}
            for e, first, _ in cadence:
                by_type.setdefault(name(e["ev"]), []).append(
                    (e["t"] - first["t"]) / 1000.0)
            for k, v in sorted(by_type.items()):
                v.sort()
                print(f"    cadence: {k} x{len(v)}"
                      f"  {v[0]:.1f}-{v[-1]:.1f} ms apart (not replays)")

        # The decisive statistic: is ANY consecutive pair inside the batch
        # threshold? That is the fingerprint of a same-drain replay, and it
        # should be present with footsteps on and absent with them off.
        tight = sorted(
            (p[i]["t"] - p[i - 1]["t"]) / 1000.0
            for i in range(1, len(p))
            if (p[i]["t"] - p[i - 1]["t"]) <= BATCH_US
        )
        gaps = sorted((p[i]["t"] - p[i - 1]["t"]) / 1000.0
                      for i in range(1, len(p)))
        if gaps:
            print(f"  gap to previous event, all: n={len(gaps)}"
                  f"  min={gaps[0]:.3f}  p5={gaps[len(gaps)//20]:.3f}"
                  f"  median={gaps[len(gaps)//2]:.1f} ms")
        print(f"  pairs inside the {BATCH_US:g} us batch threshold: "
              f"{len(tight)}  {[f'{g:.3f}' for g in tight[:8]]}")
        kinds = Counter(name(e["ev"]) for e in p)
        shown = ", ".join(f"{k} {kinds[k]}" for k in
                          ("JUMP", "FIRE_WEAPON", "FALL_SHORT", "FOOTSTEP") if kinds[k])
        verdict = "PASS" if not true_replays else "FAIL"
        print(f"  MOVER CHECK: {verdict} - {len(true_replays)} true replays in "
              f"{len(p)} local events ({shown or 'none of jump/fire/fall/footstep'})")
        print()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
