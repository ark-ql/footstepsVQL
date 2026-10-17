#!/usr/bin/env python3
"""Vanilla QL footstep rate as a function of com_maxfps, and where it crosses
the 250 fps reference.

The bug, from reference/ql-footsteps-fix/research.md:

    old = pm->ps->bobCycle;
    pm->ps->bobCycle = (int)(old + bobmove * pml.msec) & 255;
    if ( ((old + 64) ^ (pm->ps->bobCycle + 64)) & 128 )
        PM_AddEvent( PM_FootstepForSurface() );

`old` is an integer, so `(int)(old + bobmove*msec)` == old + floor(bobmove*msec):
the fractional part of every command is discarded. A footstep fires once per
128 bob units.

A client at com_maxfps f has a nominal frame time T = 1000/f ms, but usercmd
msec values are integers, so it emits a mix of floor(T) and ceil(T) whose mean
is T. That mix is modelled here, then cross-checked against the three measured
command-interval histograms in research.md.

Usage:
    python scripts/_footstep_rate_model.py [out.txt]
"""

import math
import sys

BOBMOVE_RUN = 0.4          # units per ms, running
UNITS_PER_STEP = 128
TRUE_RATE = BOBMOVE_RUN * 1000.0 / UNITS_PER_STEP   # 3.125/s if nothing truncated


def add_for_msec(msec):
    """bobCycle units added by one usercmd of `msec` milliseconds."""
    return math.floor(BOBMOVE_RUN * msec)


def rate_ideal(fps):
    """steps/s for an ideal two-value msec mix averaging 1000/fps."""
    t = 1000.0 / fps
    lo = math.floor(t)
    hi = math.ceil(t)
    if lo == hi:                      # T is an exact integer
        units = fps * add_for_msec(lo)
    else:
        p_hi = t - lo                 # fraction of commands at ceil(T)
        units = fps * ((1.0 - p_hi) * add_for_msec(lo) + p_hi * add_for_msec(hi))
    return units / UNITS_PER_STEP


def rate_from_histogram(fps, hist, tail_msec=6):
    """steps/s from a measured {msec: fraction} histogram.

    research.md buckets 5-8 ms together; `tail_msec` is the representative value
    used for that bucket and is the one assumption in this cross-check.
    """
    units_per_cmd = 0.0
    for msec, frac in hist.items():
        m = tail_msec if msec == "5-8" else msec
        units_per_cmd += frac * add_for_msec(m)
    return fps * units_per_cmd / UNITS_PER_STEP


# Measured, research.md "Measured command intervals (a test server, 2026-09-12)"
MEASURED = {
    250:  {1: 0.0,    2: 0.053,  3: 0.0001, 4: 0.832,   "5-8": 0.114},
    500:  {1: 0.0,    2: 0.934,  3: 0.065,  4: 0.0008,  "5-8": 0.0001},
    1000: {1: 0.932,  2: 0.064,  3: 0.0029, 4: 0.0007,  "5-8": 0.0002},
}


def crossings(target, lo=250, hi=1000, step=0.25):
    """fps values where the ideal curve crosses `target`, scanning upward."""
    out = []
    f = lo
    prev = rate_ideal(f)
    while f < hi:
        f2 = f + step
        cur = rate_ideal(f2)
        if (prev - target) * (cur - target) <= 0 and prev != cur:
            # linear interpolation within the step
            frac = (target - prev) / (cur - prev)
            out.append(f + frac * step)
        prev = cur
        f = f2
    return out


def main():
    sink = open(sys.argv[1], "w", encoding="utf-8") if len(sys.argv) > 1 else sys.stdout

    ref = 250.0 / UNITS_PER_STEP
    print(f"true rate if the fraction were kept : {TRUE_RATE:.3f} steps/s", file=sink)
    print(f"250 fps reference (250 units/s / 128): {ref:.4f} steps/s", file=sink)

    print("\n## ideal two-value msec mix", file=sink)
    print(f"{'fps':>6}  {'T (ms)':>7}  {'msec mix':>14}  {'units/s':>8}  "
          f"{'steps/s':>8}  {'vs ref':>7}", file=sink)
    for fps in (125, 200, 250, 264, 275, 300, 320, 333, 340, 350, 360, 368,
                375, 385, 400, 420, 450, 475, 500, 600, 700, 800, 1000):
        t = 1000.0 / fps
        lo, hi = math.floor(t), math.ceil(t)
        if lo == hi:
            mix = f"{lo} only"
        else:
            p = t - lo
            mix = f"{1-p:.0%}@{lo} {p:.0%}@{hi}"
        r = rate_ideal(fps)
        print(f"{fps:>6}  {t:>7.3f}  {mix:>14}  {r*UNITS_PER_STEP:>8.1f}  "
              f"{r:>8.4f}  {r/ref:>6.0%}", file=sink)

    print(f"\n## where the ideal curve crosses the 250 fps reference "
          f"({ref:.4f}/s)", file=sink)
    for f in crossings(ref):
        print(f"  {f:.1f} fps", file=sink)

    print("\n## model vs measured command histograms (research.md)", file=sink)
    print(f"{'fps':>6}  {'model (ideal)':>14}  {'model (measured hist)':>22}", file=sink)
    for fps, hist in sorted(MEASURED.items()):
        print(f"{fps:>6}  {rate_ideal(fps):>14.3f}  "
              f"{rate_from_histogram(fps, hist):>22.3f}", file=sink)
    print("\n  README.md records 2.05-2.08 edges/s measured at 250 fps vanilla.", file=sink)

    if sink is not sys.stdout:
        sink.close()


if __name__ == "__main__":
    main()
