#!/usr/bin/env python3
"""Offline structural self-test for both hook sites. Never touches the game.

`fsvql_selftest()` proves the cadence arithmetic, and `validate.sh` proves nothing
installs with no qagame present. Neither can prove that both sites are *found* in
a real mapping of the real binary, that `PM_FootstepForSurface` and
`SendPendingPredictableEvents` resolve to the addresses the notes measured, that
the ring write and undo behave against a playerState, and that unpatching
restores the original bytes exactly.

This does all of that in a throwaway process, by `dlopen`ing qagamex64.so here
instead of borrowing the live server's copy. That works because every undefined
symbol in qagamex64.so is libc / libstdc++ / libm -- it takes its engine syscalls
through a function pointer the engine hands it, not through the dynamic linker.

What it deliberately does NOT do is *call* any patched code. The trampolines are
installed, their bytes are read back, and they are removed again. Executing qagame
code outside the engine would crash, and would prove nothing about the game.

Expected addresses come from notes/s1-qagame-offsets.md and are only valid for
qagamex64.so sha256 091d568f... -- probe_sigs.sh gates that.

Site C's call lands on a stub page near qagame that jumps to the hook's thunk. A
child process (`--far`) loads qagame more than 2 GB from the hook, out of a
rel32's reach, and checks the install there too.

Usage: ./selftest_emit.py [/path/to/qagamex64.so] [/path/to/footstepsVQL_hook.so]
       ./selftest_emit.py --far /path/to/qagamex64.so /path/to/footstepsVQL_hook.so
"""

import ctypes
import os
import subprocess
import sys

QAGAME = os.path.join(os.environ.get("QL_DIR", "/home/ql/qlds"), "baseq3", "qagamex64.so")
HOOK = os.path.join(os.path.dirname(os.path.abspath(__file__)), "footstepsVQL_hook.so")

# Offsets from the load base, measured in notes/s1-qagame-offsets.md.
SITE_A = 0x58570
SEND_PENDING = 0x59A70
PM_FOOTSTEP_FOR_SURFACE = 0x553A0

SITE_A_LEN = 27
SEND_PENDING_HEAD = 14

# The `call SendPendingPredictableEvents` in ClientThink_real, 0x2f into the
# signature that starts at 0x5a1aa. Site C rewrites its 4-byte rel32 and nothing else.
SITE_C_CALL = 0x5A1D9

VANILLA_A = bytes.fromhex(
    "8b4808" "f30f2ac1" "83c140" "f30f58c1" "f30f2cd0" "81e2ff000000" "895008")
# The entry of SendPendingPredictableEvents. Nothing may write here any more; the
# old s5 emitter used to.
VANILLA_SENDP = bytes.fromhex("415641554154" "55" "53" "8b87ec010000")

FAILURES = []


def check(label, got, want, fmt=repr):
    ok = got == want
    if not ok:
        FAILURES.append(label)
    print("  {:<6} {:<34} {}".format(
        "ok" if ok else "FAIL", label,
        fmt(got) if ok else "{} (expected {})".format(fmt(got), fmt(want))))


def hexf(v):
    return "0x{:x}".format(v) if isinstance(v, int) else repr(v)


STUB_LEN = 14
REL32_REACH = 0x80000000


def stub_jump(addr):
    """The absolute target of a `jmp [rip+0]` stub at addr, or None."""
    data = ctypes.string_at(addr, STUB_LEN)
    if data[:6] != b"\xff\x25\x00\x00\x00\x00":
        return None
    return int.from_bytes(data[6:14], "little")


def mapping_of(addr):
    """(permissions, path) of the mapping holding addr in this process; path is
    "" for an anonymous mapping."""
    with open("/proc/self/maps") as fh:
        for line in fh:
            f = line.split()
            lo, hi = (int(x, 16) for x in f[0].split("-"))
            if lo <= addr < hi:
                return f[1], (f[5] if len(f) > 5 else "")
    return None, None


def check_stub(lib, call_addr, label):
    """Site C's call lands on our stub: its own anonymous page, read+exec only,
    and the stub jumps to the thunk. Being a rel32 target, it is in reach by
    construction."""
    data = ctypes.string_at(call_addr, 5)
    check(label + ": opcode still e8", data[0], 0xE8, hexf)
    stub = call_addr + 5 + int.from_bytes(data[1:5], "little", signed=True)
    check(label + ": stub page anonymous, r-x", mapping_of(stub), ("r-xp", ""))
    check(label + ": stub jumps to the thunk", stub_jump(stub),
          ctypes.cast(lib.fsvql_thunk_sendp, ctypes.c_void_p).value, hexf)
    return stub


def bytef(v):
    return v.hex(" ") if isinstance(v, bytes) else repr(v)


def jmp_target(data):
    """The 8-byte absolute target of an `ff 25 00000000` jmp [rip+0], or None."""
    if len(data) < 14 or data[0] != 0xFF or data[1] != 0x25 or any(data[2:6]):
        return None
    return int.from_bytes(data[6:14], "little")


# playerState_t, as dword indices. All measured, notes/s1-qagame-offsets.md.
I_SEQ = 0x6C // 4
I_EV = 0x70 // 4
I_PARM = 0x78 // 4
I_EESEQ = 0x1EC // 4

I_EXT = 0x80 // 4          # externalEvent

# gentity_t: ent->client and ent->s.event, as fsvql_ring_undo reads them.
ENT_CLIENT = 0x240
ES_EVENT = 0xBC
EV_FOOTSTEP = 1


def make_ent(owner):
    """A zeroed gentity-sized buffer whose client pointer is `owner`."""
    ent = (ctypes.c_ubyte * 0x300)()
    ctypes.c_void_p.from_buffer(ent, ENT_CLIENT).value = owner
    return ent


def ent_event(ent):
    return ctypes.c_int.from_buffer(ent, ES_EVENT)



# pmove_t, as the trampoline hands it over in r12: noFootsteps and waterlevel,
# the two fields PM_Footsteps' footstep gate reads besides its own flag.
PM_NOFOOTSTEPS = 0x2C
PM_WATERLEVEL = 0xD4
PM_RUNNING = (ctypes.c_ubyte * 0x100)()


def bob(lib, p, addend, footstep=1, pm=PM_RUNNING):
    """One site-A call as the trampoline makes it: ps, addend, PM_Footsteps'
    footstep flag (r13d) and pm (r12). The default is a running command."""
    return lib.fsvql_bob_update(p, addend, footstep, ctypes.addressof(pm))

def drive_to_injection(lib, ps, p, limit=1400):
    """Run 1 ms commands until the cadence writes a ring event, or give up.

    1 ms is the case that matters: the client truncates bobmove * 1 to zero, so
    vanilla never flips and the private cadence accrues a whole footstep of debt
    over 128 * 4 ms. That is the 1000 fps player this mode exists for.
    """
    before = lib.fsvql_ring_written()
    t = ps[0]
    for _ in range(limit):
        t += 1
        ps[0] = t
        bob(lib, p, ctypes.c_float(0.4))
        if lib.fsvql_ring_written() != before:
            return True
    return False


def ring_arithmetic(lib, base):
    """Exercise the S2 ring write and undo against a synthetic playerState.

    This is the part of S2 whose correctness cannot be read off the bytes: the
    write has to look exactly like PM_AddEvent, and the undo has to put back four
    fields including entityEventSequence, whose omission would silently stop the
    *next* real jump or shot from ever reaching other players.

    It works outside the game because fsvql_bob_update takes the playerState as an
    argument, and because PM_FootstepForSurface only reads one static dword of
    surface flags — zero here, which means "ordinary ground", so it returns
    EV_FOOTSTEP rather than touching anything that does not exist.
    """
    ps = (ctypes.c_int * 1024)()
    p = ctypes.cast(ps, ctypes.POINTER(ctypes.c_int))
    ent = make_ent(ctypes.addressof(ps))
    event = ent_event(ent)

    # Seed the ring as if the mover already has a jump pending in the slot our
    # write is about to land on, so the undo has something real to preserve.
    # eventSequence 41 is odd, so slot = 41 & 1 = 1.
    ps[0] = 100000
    ps[2] = 0
    ps[I_SEQ] = 41
    ps[I_EV + 0] = 0
    ps[I_EV + 1] = 7
    ps[I_PARM + 0] = 0
    ps[I_PARM + 1] = 99
    ps[I_EESEQ] = 41

    lib.fsvql_reset_slots()
    lib.fsvql_reset_counters()

    print("== S2 ring write, at 1 ms commands ==")
    if not drive_to_injection(lib, ps, p):
        FAILURES.append("cadence never owed a footstep")
        print("  FAIL   cadence never owed a footstep in 1400 commands")
        return
    check("ring writes", lib.fsvql_ring_written(), 1)
    check("eventSequence advanced by 1", ps[I_SEQ], 42)
    check("events[1] holds EV_FOOTSTEP", ps[I_EV + 1], 1)
    check("eventParms[1] cleared", ps[I_PARM + 1], 0)
    check("events[0] untouched", ps[I_EV + 0], 0)
    check("bobCycle still vanilla (0)", ps[2], 0)
    print()

    # What BG_PlayerStateToEntityState does when it drains our event for the
    # listeners. Without simulating it, the entityEventSequence half of the undo
    # is never exercised.
    print("== undo, after the drain has consumed it ==")
    ps[I_EESEQ] = 42
    event.value = EV_FOOTSTEP | ((41 & 3) << 8)
    lib.fsvql_ring_undo(p, ctypes.addressof(ent))
    # drained is the server-side proof that other players got it: only a listener
    # channel can have advanced entityEventSequence past our write.
    check("counted as delivered", lib.fsvql_ring_drained(), 1)
    check("ring undos", lib.fsvql_ring_undone(), 1)
    check("eventSequence restored", ps[I_SEQ], 41)
    check("events[1] restored to the jump", ps[I_EV + 1], 7)
    check("eventParms[1] restored", ps[I_PARM + 1], 99)
    check("entityEventSequence rolled back", ps[I_EESEQ], 41)
    check("no skips", lib.fsvql_ring_skipped(), 0)
    check("no missed undos", lib.fsvql_ring_missed(), 0)
    # After the rollback the next real event drains at 41 again and would carry
    # the same tag. cgame compares the whole value, so an identical footstep
    # would never play; the undo has to have moved ours two tags on.
    check("entityState re-tagged", event.value, EV_FOOTSTEP | (((41 + 2) & 3) << 8),
          hexf)
    check("re-tag counted", lib.fsvql_ring_retagged(), 1)
    check("next real footstep is distinct",
          (EV_FOOTSTEP | ((41 & 3) << 8)) != event.value, True)
    print()

    # The one interleaving case that can happen in the game: PM_WaterEvents adds an
    # event behind ours. A two-slot ring cannot express "remove the older of
    # these", so the undo must decline rather than roll eventSequence back over
    # somebody else's event.
    print("== undo declines when the ring moved on ==")
    if not drive_to_injection(lib, ps, p):
        FAILURES.append("cadence never owed a second footstep")
        print("  FAIL   no second injection")
        return
    seq_after_write = ps[I_SEQ]
    ps[I_SEQ] = seq_after_write + 1          # someone else added one behind us
    ps[I_EV + (seq_after_write & 1)] = 33
    lib.fsvql_ring_undo(p, ctypes.addressof(ent))
    check("skip counted", lib.fsvql_ring_skipped(), 1)
    check("undo count unchanged", lib.fsvql_ring_undone(), 1)
    # No drain was simulated this time, so it must not be counted as delivered.
    check("not counted as delivered", lib.fsvql_ring_drained(), 1)
    check("eventSequence left alone", ps[I_SEQ], seq_after_write + 1)
    check("the other event survived", ps[I_EV + (seq_after_write & 1)], 33)
    check("no re-tag without a rollback", lib.fsvql_ring_retagged(), 1)
    print()

    # A gentity that does not own this playerState must never be written.
    print("== foreign gentity is left alone ==")
    ps[I_EESEQ] = ps[I_SEQ]
    if not drive_to_injection(lib, ps, p):
        FAILURES.append("cadence never owed a third footstep")
        print("  FAIL   no third injection")
        return
    q = ps[I_SEQ] - 1
    ps[I_EESEQ] = q + 1
    other = (ctypes.c_int * 1024)()
    foreign = make_ent(ctypes.addressof(other))
    mine = EV_FOOTSTEP | ((q & 3) << 8)
    ent_event(foreign).value = mine
    lib.fsvql_ring_undo(p, ctypes.addressof(foreign))
    check("ring still rolled back", ps[I_SEQ], q)
    check("foreign entityState untouched", ent_event(foreign).value, mine, hexf)
    check("re-tag count unchanged", lib.fsvql_ring_retagged(), 1)
    print()

    lib.fsvql_reset_counters()
    lib.fsvql_reset_slots()


def deferral(lib):
    """An owed footstep the engine could not ship this command is held back, not
    written: nothing in the ring, the step still owed, and it goes out on the next
    command that has room. The engine ships at most one ring event into
    ent->s.event (none while externalEvent holds it) plus one overflow temp entity.
    """
    print("== deferred when the engine has no room this command ==")
    for label, ext, queued in (("pain held + one event queued", 0x135, 1),
                               ("two events queued", 0, 2)):
        ps = (ctypes.c_int * 1024)()
        p = ctypes.cast(ps, ctypes.POINTER(ctypes.c_int))
        ent = make_ent(ctypes.addressof(ps))
        ps[0] = 300000
        ps[I_EESEQ] = 20
        lib.fsvql_reset_slots()
        lib.fsvql_reset_counters()

        # 1 ms commands, each already carrying `queued` unshipped events.
        for _ in range(1400):
            ps[0] += 1
            ps[I_EXT] = ext
            ps[I_SEQ] = ps[I_EESEQ] + queued
            bob(lib, p, ctypes.c_float(0.4))
            if lib.fsvql_ring_deferred():
                break
        check(label + ": held back", lib.fsvql_ring_deferred() >= 1, True)
        check(label + ": nothing written", lib.fsvql_ring_written(), 0)
        check(label + ": ring untouched", ps[I_SEQ], ps[I_EESEQ] + queued)
        check(label + ": not yet counted as owed", lib.fsvql_injections_due(), 0)

        # The engine ships the queue; the next command has room.
        ps[I_EXT] = 0
        ps[I_EESEQ] = ps[I_SEQ]
        ps[0] += 1
        bob(lib, p, ctypes.c_float(0.4))
        check(label + ": written next command", lib.fsvql_ring_written(), 1)
        check(label + ": owed counted once", lib.fsvql_injections_due(), 1)
        ps[I_EESEQ] += 1
        lib.fsvql_ring_undo(p, ctypes.addressof(ent))
        check(label + ": delivered and undone",
              (lib.fsvql_ring_drained(), lib.fsvql_ring_undone()), (1, 1))
    print()
    lib.fsvql_reset_counters()
    lib.fsvql_reset_slots()



def silent_gaits(lib):
    """Vanilla plays no footstep on a walk-key, crouched, wading or
    footsteps-off command, so the clock must not either: at 1 ms commands, where
    vanilla bobCycle never flips and the clock would otherwise owe 1.95/s,
    nothing is injected and nothing is owed. Running again, the clock picks up
    within one reference interval and with no banked debt."""
    print("== silent in vanilla: walk key or crouched, water, footsteps off ==")
    for label, footstep, water, nofoot in (("footstep flag 0", 0, 0, 0),
                                           ("waterlevel 1", 1, 1, 0),
                                           ("noFootsteps", 1, 0, 1)):
        pm = (ctypes.c_ubyte * 0x100)()
        ctypes.c_int.from_buffer(pm, PM_WATERLEVEL).value = water
        ctypes.c_int.from_buffer(pm, PM_NOFOOTSTEPS).value = nofoot
        ps = (ctypes.c_int * 1024)()
        p = ctypes.cast(ps, ctypes.POINTER(ctypes.c_int))
        ps[0] = 400000
        lib.fsvql_reset_slots()
        lib.fsvql_reset_counters()
        for _ in range(1400):
            ps[0] += 1
            bob(lib, p, ctypes.c_float(0.4), footstep, pm)
        check(label + ": nothing written", lib.fsvql_ring_written(), 0)
        check(label + ": nothing owed", lib.fsvql_injections_due(), 0)
        check(label + ": every command counted quiet", lib.fsvql_quiet(), 1400)
        n = 0
        while n < 1400 and not lib.fsvql_ring_written():
            n += 1
            ps[0] += 1
            bob(lib, p, ctypes.c_float(0.4))
        check(label + ": running again, first footstep within 512 ms", 1 <= n <= 512, True)
        check(label + ": one footstep, not a banked burst", lib.fsvql_ring_written(), 1)
    print()
    lib.fsvql_reset_counters()
    lib.fsvql_reset_slots()

def add_event(ps, ev):
    """PM_AddEvent: the raw id into the ring, eventSequence on by one."""
    seq = ps[I_SEQ]
    ps[I_EV + (seq & 1)] = ev
    ps[I_PARM + (seq & 1)] = 0
    ps[I_SEQ] = seq + 1


def think(lib, ps, p, ent, msec, air=False, land=False, post=None):
    """One ClientThink_real, in the order the game runs it: Pmove (PM_CrashLand
    on a landing, our site A, then qagame's own PM_AddEvent if vanilla flipped),
    G_TouchTriggers (`post`, e.g. EV_JUMP_PAD), BG_PlayerStateToEntityState,
    SendPendingPredictableEvents, and our undo. An airborne command never
    reaches site A."""
    ps[0] += msec
    if land:
        add_event(ps, EV_FOOTSTEP)
    if not air:
        old = bob(lib, p, ctypes.c_float(0.4 * msec))
        if ((old + 64) ^ (ps[2] + 64)) & 128:
            add_event(ps, EV_FOOTSTEP)
    if post:
        add_event(ps, post)
    if ps[I_EESEQ] < ps[I_SEQ]:
        e = ps[I_EESEQ]
        ent_event(ent).value = ps[I_EV + (e & 1)] | ((e & 3) << 8)
        ps[I_EESEQ] = e + 1
    lib.fsvql_ring_pre(p)
    if ps[I_EESEQ] < ps[I_SEQ]:
        ps[I_EESEQ] += 1                     # the overflow temp entity
    lib.fsvql_ring_undo(p, ctypes.addressof(ent))


EV_FIRE_WEAPON = 20
EV_JUMP_PAD = 9


def overflow_strip(lib):
    """400 fps while firing: the shot is queued in the ring ahead of the vanilla
    footstep, so BG_PlayerStateToEntityState puts the shot in ent->s.event and
    the footstep is next in line for the overflow temp entity. The listeners must
    get the shot and not the footstep; the mover's ring must keep both."""
    ps = (ctypes.c_int * 1024)()
    p = ctypes.cast(ps, ctypes.POINTER(ctypes.c_int))
    ent = make_ent(ctypes.addressof(ps))
    ps[0] = 600000
    lib.fsvql_reset_slots()
    lib.fsvql_reset_counters()

    print("== s2 replace while firing, 400 fps: footstep behind a shot ==")
    flipped = False
    for i in range(4000):
        # Only the flip command's overflow matters; on other commands it can
        # legitimately carry the clock's own injection behind the shot.
        shipped = []
        msec = 2 if i % 2 == 0 else 3
        ps[0] += msec
        seq = ps[I_SEQ]                       # PM_Weapon, before PM_Footsteps
        ps[I_EV + (seq & 1)] = EV_FIRE_WEAPON
        ps[I_SEQ] = seq + 1
        old = bob(lib, p, ctypes.c_float(0.4 * msec))
        if ((old + 64) ^ (ps[2] + 64)) & 128:
            fs = ps[I_SEQ]                    # qagame's own PM_AddEvent
            ps[I_EV + (fs & 1)] = EV_FOOTSTEP
            ps[I_SEQ] = fs + 1
            flipped = True
        if ps[I_EESEQ] < ps[I_SEQ]:           # BG_PlayerStateToEntityState
            e = ps[I_EESEQ]
            ent_event(ent).value = ps[I_EV + (e & 1)] | ((e & 3) << 8)
            ps[I_EESEQ] = e + 1
        lib.fsvql_ring_pre(p)
        if ps[I_EESEQ] < ps[I_SEQ]:           # the overflow temp entity
            e = ps[I_EESEQ]
            shipped.append(ps[I_EV + (e & 1)])
            ps[I_EESEQ] = e + 1
        lib.fsvql_ring_undo(p, ctypes.addressof(ent))
        if flipped:
            break
    check("a vanilla flip happened", flipped, True)
    check("overflow shipped nothing on the flip command", shipped, [])
    check("skipped off the overflow path", lib.fsvql_ring_suppressed_overflow(), 1)
    check("counted as stripped", lib.fsvql_ring_suppressed(), 1)
    check("listeners' entity keeps the shot", ent_event(ent).value & 0xFF, EV_FIRE_WEAPON)
    check("mover's ring keeps the footstep", ps[I_EV + ((ps[I_SEQ] - 1) & 1)], EV_FOOTSTEP)
    check("everything drained", ps[I_EESEQ], ps[I_SEQ])
    check("no missed undos", lib.fsvql_ring_missed(), 0)

    # A CrashLand footstep in the entity slot, with no bob flip pending, is
    # stock and stays.
    ent_event(ent).value = EV_FOOTSTEP | 0x200
    lib.fsvql_ring_undo(p, ctypes.addressof(ent))
    check("a CrashLand footstep is left alone", ent_event(ent).value, EV_FOOTSTEP | 0x200, hexf)
    check("stripped count unchanged", lib.fsvql_ring_suppressed(), 1)
    print()
    lib.fsvql_reset_counters()
    lib.fsvql_reset_slots()


def strip_restores(lib):
    """Stripping a stock footstep out of ent->s.event puts back what its drain
    overwrote, if that appeared within STRIP_RESTORE_MS (50 ms), and otherwise
    leaves the slot empty. bobCycle is set one unit short of a boundary so the
    next 3 ms command flips on cue; the hook resyncs to it."""
    ps = (ctypes.c_int * 1024)()
    p = ctypes.cast(ps, ctypes.POINTER(ctypes.c_int))
    ent = make_ent(ctypes.addressof(ps))
    ps[0] = 700000
    lib.fsvql_reset_slots()
    lib.fsvql_reset_counters()
    for i in range(200):                      # 400 fps, so replace latches
        think(lib, ps, p, ent, 2 if i % 2 == 0 else 3)
    lib.fsvql_reset_counters()

    def flip_command(bob_before):
        ps[0] += 3
        ps[2] = bob_before
        bob(lib, p, ctypes.c_float(0.4 * 3))
        seq = ps[I_SEQ]                       # qagame's own PM_AddEvent
        ps[I_EV + (seq & 1)] = EV_FOOTSTEP
        ps[I_SEQ] = seq + 1
        e = ps[I_EESEQ]                       # BG_PlayerStateToEntityState
        ent_event(ent).value = ps[I_EV + (e & 1)] | ((e & 3) << 8)
        ps[I_EESEQ] = e + 1
        lib.fsvql_ring_pre(p)
        lib.fsvql_ring_undo(p, ctypes.addressof(ent))

    print("== stripping puts back what the drain overwrote ==")
    recent = EV_FOOTSTEP | 0x100              # a clock footstep drained 3 ms ago
    ent_event(ent).value = recent
    lib.fsvql_ring_undo(p, ctypes.addressof(ent))
    flip_command(63)
    check("recent clock footstep put back", ent_event(ent).value, recent, hexf)
    check("stripped and restored", (lib.fsvql_ring_suppressed(),
                                    lib.fsvql_ring_suppressed_restored()), (1, 1))

    stale = EV_FOOTSTEP | 0x300               # drained, then 60 ms with no change
    ent_event(ent).value = stale
    lib.fsvql_ring_undo(p, ctypes.addressof(ent))
    for _ in range(20):
        ps[0] += 3
        bob(lib, p, ctypes.c_float(0.4 * 3))
        lib.fsvql_ring_undo(p, ctypes.addressof(ent))
    # The clock may inject in this window (after the resync it sits on the same
    # boundary); nothing here drains it, so the entity keeps the stale value.
    check("no flip in between", lib.fsvql_ring_suppressed(), 1)
    check("entity still holds the stale event", ent_event(ent).value, stale, hexf)
    flip_command(191)
    check("stale event not put back", ent_event(ent).value, 0, hexf)
    check("stripped, not restored", (lib.fsvql_ring_suppressed(),
                                     lib.fsvql_ring_suppressed_restored()), (2, 1))
    check("no missed undos", lib.fsvql_ring_missed(), 0)
    print()
    lib.fsvql_reset_counters()
    lib.fsvql_reset_slots()


def listener_replace(lib):
    """s2 above ~390 fps: a vanilla bob footstep reaches the mover's ring, as
    his prediction expects, but is stripped from what the listeners receive.
    400 fps sends alternating 2 and 3 ms commands, and only the 3 ms ones
    advance vanilla bobCycle."""
    ps = (ctypes.c_int * 1024)()
    p = ctypes.cast(ps, ctypes.POINTER(ctypes.c_int))
    ent = make_ent(ctypes.addressof(ps))
    ps[0] = 200000

    lib.fsvql_reset_slots()
    lib.fsvql_reset_counters()

    print("== s2 listener replace, 400 fps ==")
    flips = lib.fsvql_vanilla_flips()
    for i in range(4000):
        think(lib, ps, p, ent, 2 if i % 2 == 0 else 3)
        if lib.fsvql_vanilla_flips() != flips:
            break
    check("a vanilla flip happened", lib.fsvql_vanilla_flips() > flips, True)
    check("stripped from the listeners", lib.fsvql_ring_suppressed(), 1)
    check("the stock footstep is out of the entityState",
          ent_event(ent).value != EV_FOOTSTEP | (((ps[I_SEQ] - 1) & 3) << 8), True)
    check("the mover's ring kept it", ps[I_EV + ((ps[I_SEQ] - 1) & 1)], EV_FOOTSTEP)

    # A non-footstep in the entityState on that command is not ours to clear.
    flips = lib.fsvql_vanilla_flips()
    for i in range(4000):
        ps[0] += 2 if i % 2 == 0 else 3
        old = bob(lib, p, ctypes.c_float(0.4 * (2 if i % 2 == 0 else 3)))
        if ((old + 64) ^ (ps[2] + 64)) & 128:
            ent_event(ent).value = 14 | 0x100
            lib.fsvql_ring_undo(p, ctypes.addressof(ent))
            break
        lib.fsvql_ring_undo(p, ctypes.addressof(ent))
    check("a second vanilla flip happened", lib.fsvql_vanilla_flips() > flips, True)
    check("non-footstep left alone", ent_event(ent).value, 14 | 0x100, hexf)
    check("suppressed count unchanged", lib.fsvql_ring_suppressed(), 1)
    check("no missed undos", lib.fsvql_ring_missed(), 0)
    check("no skips", lib.fsvql_ring_skipped(), 0)
    print()

    lib.fsvql_reset_counters()
    lib.fsvql_reset_slots()


def stairs(lib):
    """Down a staircase: 120 ms on the ground, a 40-160 ms drop, a CrashLand
    footstep on landing. Airborne commands still reach fsvql_ring_pre, so their
    time is not credited at site A; a stock 250 fps client is never injected
    into. Then at 1000 fps, where the clock owns the listener stream, a landing
    holds the next clock footstep FS_MIN_STEP_GAP_MS (200 ms) off it."""
    ps = (ctypes.c_int * 1024)()
    p = ctypes.cast(ps, ctypes.POINTER(ctypes.c_int))
    ent = make_ent(ctypes.addressof(ps))
    ps[0] = 800000
    lib.fsvql_reset_slots()
    lib.fsvql_reset_counters()

    print("== stairs ==")
    drops = (10, 20, 30, 40)                  # 4 ms commands in the air
    for k in range(120):                      # ~30 s
        for _ in range(30):
            think(lib, ps, p, ent, 4)
        for _ in range(drops[k % 4]):
            think(lib, ps, p, ent, 4, air=True)
        think(lib, ps, p, ent, 4, land=True)
    check("250 fps stairs: nothing injected", lib.fsvql_ring_written(), 0)
    check("250 fps stairs: nothing owed", lib.fsvql_injections_due(), 0)
    check("250 fps stairs: vanilla flips seen", lib.fsvql_vanilla_flips() > 0, True)
    check("no missed undos", lib.fsvql_ring_missed(), 0)

    lib.fsvql_reset_slots()
    lib.fsvql_reset_counters()
    n = 0
    while n < 2000 and not lib.fsvql_ring_written():
        think(lib, ps, p, ent, 1)
        n += 1
    check("1000 fps: clock running", lib.fsvql_ring_written(), 1)
    for _ in range(400):
        think(lib, ps, p, ent, 1)
    check("1000 fps: no clock footstep for 400 ms", lib.fsvql_ring_written(), 1)
    think(lib, ps, p, ent, 1, land=True)
    landed = ps[0]
    while ps[0] - landed < 1000 and lib.fsvql_ring_written() == 1:
        think(lib, ps, p, ent, 1)
    gap = ps[0] - landed
    check("next clock footstep 200-300 ms after the landing", 200 <= gap <= 300, True)
    print("         (it came {} ms after)".format(gap))

    # A jump pad touched after Pmove (G_TouchTriggers), a few ms before the
    # clock is due: the end-of-command scan must hold the clock off for
    # FS_EVENT_GUARD_MS (50 ms), so it can't overwrite EV_JUMP_PAD in the
    # entity slot before the next snapshot.
    written = lib.fsvql_ring_written()
    n = 0
    while n < 2000 and lib.fsvql_ring_written() == written:
        think(lib, ps, p, ent, 1)
        n += 1
    shot = ps[0]
    for _ in range(470):
        think(lib, ps, p, ent, 1)
    check("1000 fps: clock not yet due 470 ms on", lib.fsvql_ring_written(), written + 1)
    think(lib, ps, p, ent, 1, post=EV_JUMP_PAD)
    pad = ps[0]
    while ps[0] - pad < 1000 and lib.fsvql_ring_written() == written + 1:
        think(lib, ps, p, ent, 1)
    gap = ps[0] - pad
    check("next clock footstep at least 50 ms after a jump pad", gap >= 50, True)
    print("         (it came {} ms after the pad, {} ms after the previous footstep)".format(
        gap, ps[0] - shot))
    print()
    lib.fsvql_reset_counters()
    lib.fsvql_reset_slots()


def deny_mprotect():
    """Make every later mprotect in this process fail with EPERM (seccomp)."""
    class SockFilter(ctypes.Structure):
        _fields_ = [("code", ctypes.c_ushort), ("jt", ctypes.c_ubyte),
                    ("jf", ctypes.c_ubyte), ("k", ctypes.c_uint)]

    class SockFprog(ctypes.Structure):
        _fields_ = [("len", ctypes.c_ushort), ("filter", ctypes.POINTER(SockFilter))]

    ld_abs, jeq, ret = 0x20, 0x15, 0x06
    prog = (SockFilter * 6)(
        SockFilter(ld_abs, 0, 0, 4),              # arch
        SockFilter(jeq, 0, 3, 0xC000003E),        # AUDIT_ARCH_X86_64, else allow
        SockFilter(ld_abs, 0, 0, 0),              # syscall number
        SockFilter(jeq, 0, 1, 10),                # __NR_mprotect, else allow
        SockFilter(ret, 0, 0, 0x00050000 | 1),    # SECCOMP_RET_ERRNO | EPERM
        SockFilter(ret, 0, 0, 0x7FFF0000))        # SECCOMP_RET_ALLOW
    fprog = SockFprog(len(prog), prog)
    libc = ctypes.CDLL(None, use_errno=True)
    if libc.prctl(38, 1, 0, 0, 0) != 0:           # PR_SET_NO_NEW_PRIVS
        return False
    return libc.prctl(22, 2, ctypes.byref(fprog), 0, 0) == 0   # SECCOMP_MODE_FILTER


def unpatch_fails_closed(lib, base, call_target):
    """fsvql_unpatch() when site C cannot be made writable.

    Site C keeps calling the thunk, and the thunk forwards through
    fsvql_real_sendp, so nothing may change: both sites stay ours, every pointer
    stays, and the result is -4. Restoring site A here would clear the forwarding
    pointer under a live call. Runs in a forked child, because the seccomp filter
    that makes mprotect fail cannot be removed again.
    """
    print("== unpatch with mprotect denied (forked child) ==")
    sys.stdout.flush()
    pid = os.fork()
    if pid == 0:
        FAILURES.clear()
        tramp = ctypes.cast(lib.fsvql_tramp, ctypes.c_void_p).value
        thunk = ctypes.cast(lib.fsvql_thunk_sendp, ctypes.c_void_p).value
        sendp, surf = lib.fsvql_sendp_addr(), lib.fsvql_surface_addr()
        if not deny_mprotect():
            check("seccomp filter installed", False, True)
        else:
            check("fsvql_unpatch()", lib.fsvql_unpatch(), -4)
            check("fsvql_status()", lib.fsvql_status(), 1)
            check("site A still ours",
                  jmp_target(ctypes.string_at(base + SITE_A, SITE_A_LEN)), tramp, hexf)
            check("call still lands on the stub to the thunk",
                  stub_jump(call_target(ctypes.string_at(base + SITE_C_CALL, 5))), thunk, hexf)
            check("forwarding target kept", lib.fsvql_sendp_addr(), sendp, hexf)
            check("surface pointer kept", lib.fsvql_surface_addr(), surf, hexf)
        print()
        sys.stdout.flush()
        os._exit(1 if FAILURES else 0)
    _, status = os.waitpid(pid, 0)
    if status != 0:
        FAILURES.append("unpatch with mprotect denied")


def far_mapping_in_child(qagame, hook):
    """Run far_mapping() in a fresh process: this one already has qagame mapped
    near the hook, and a library is mapped only once per process."""
    print("== qagame mapped more than 2 GB from the hook (child process) ==")
    sys.stdout.flush()
    rc = subprocess.call([sys.executable, os.path.abspath(__file__), "--far", qagame, hook])
    if rc != 0:
        FAILURES.append("qagame mapped more than 2 GB from the hook")


def far_mapping(qagame, hook):
    """The 2026-10-07 production case: a map change put qagame 2.26 GB from the
    hook, out of reach of a rel32 to the thunk, and 1.0 refused with -5. Load
    the hook, reserve 3 GB of address space below it, then load qagame, which
    lands below the reservation. If it still lands in a hole near the hook,
    block that hole and load it again."""
    libc = ctypes.CDLL(None, use_errno=True)
    libc.mmap.restype = ctypes.c_void_p
    libc.mmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int, ctypes.c_int,
                          ctypes.c_int, ctypes.c_long]
    libc.dlopen.restype = ctypes.c_void_p
    libc.dlopen.argtypes = [ctypes.c_char_p, ctypes.c_int]
    libc.dlclose.argtypes = [ctypes.c_void_p]
    prot_none, map_private, map_anon, map_noreserve = 0, 0x02, 0x20, 0x4000
    rtld_lazy, rtld_local = 1, 0

    lib = ctypes.CDLL(hook)
    for name in ("fsvql_patch", "fsvql_status", "fsvql_unpatch"):
        getattr(lib, name).restype = ctypes.c_int
    for name in ("fsvql_qagame_base", "fsvql_site_c", "fsvql_sendp_addr", "fsvql_stub_pages"):
        if hasattr(lib, name):          # a 1.0 library, run as a control, has no stubs
            getattr(lib, name).restype = ctypes.c_uint64
    thunk = ctypes.cast(lib.fsvql_thunk_sendp, ctypes.c_void_p).value

    libc.mmap(None, 3 << 30, prot_none, map_private | map_anon | map_noreserve, -1, 0)
    handle, base = None, 0
    for _ in range(16):
        handle = libc.dlopen(qagame.encode(), rtld_lazy | rtld_local)
        base = lib.fsvql_qagame_base()
        if not handle or not base or abs(thunk - (base + SITE_C_CALL + 5)) >= REL32_REACH:
            break
        libc.dlclose(handle)
        libc.mmap(base, 0x400000, prot_none, map_private | map_anon | map_noreserve, -1, 0)

    call = base + SITE_C_CALL
    print("  hook thunk 0x{:x}, qagame base 0x{:x}, {:.2f} GB apart".format(
        thunk, base, abs(thunk - (call + 5)) / float(1 << 30)))
    check("qagame loaded", bool(handle and base), True)
    check("beyond a rel32 from the hook", abs(thunk - (call + 5)) >= REL32_REACH, True)
    if FAILURES:
        return 1
    orig = ctypes.string_at(call, 5)
    check("fsvql_patch()", lib.fsvql_patch(), 1)
    if FAILURES:
        return 1
    check("fsvql_status()", lib.fsvql_status(), 1)
    check("site C address", lib.fsvql_site_c(), call, hexf)
    stub = check_stub(lib, call, "site C")
    check("forwarding target", lib.fsvql_sendp_addr(), base + SEND_PENDING, hexf)
    check("fsvql_unpatch()", lib.fsvql_unpatch(), 1)
    check("call bytes restored", ctypes.string_at(call, 5), orig, bytef)
    check("fsvql_patch() again", lib.fsvql_patch(), 1)
    check("the stub was reused", call + 5 + int.from_bytes(
        ctypes.string_at(call + 1, 4), "little", signed=True), stub, hexf)
    check("one stub page", lib.fsvql_stub_pages(), 1)
    check("fsvql_unpatch()", lib.fsvql_unpatch(), 1)
    print()
    return 1 if FAILURES else 0


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--far":
        return far_mapping(sys.argv[2], sys.argv[3])
    qagame = sys.argv[1] if len(sys.argv) > 1 else QAGAME
    hook = sys.argv[2] if len(sys.argv) > 2 else HOOK

    print("qagame {}\nhook   {}\n".format(qagame, hook))

    # RTLD_LAZY, because nothing in here is ever called: binding every relocation
    # eagerly would only widen what this process has to satisfy.
    qa = ctypes.CDLL(qagame, mode=os.RTLD_LAZY | os.RTLD_LOCAL)
    lib = ctypes.CDLL(hook)

    for name in ("fsvql_patch", "fsvql_status", "fsvql_unpatch"):
        getattr(lib, name).restype = ctypes.c_int
    for name in ("fsvql_qagame_base", "fsvql_site", "fsvql_site_c",
                 "fsvql_surface_addr", "fsvql_sendp_addr",
                 "fsvql_ring_written", "fsvql_ring_drained", "fsvql_ring_undone",
                 "fsvql_ring_skipped", "fsvql_ring_missed", "fsvql_ring_suppressed",
                 "fsvql_ring_retagged", "fsvql_ring_deferred", "fsvql_vanilla_flips",
                 "fsvql_injections_due", "fsvql_quiet", "fsvql_stub_pages"):
        getattr(lib, name).restype = ctypes.c_uint64
    lib.fsvql_bob_update.argtypes = [ctypes.POINTER(ctypes.c_int), ctypes.c_float,
                                     ctypes.c_int, ctypes.c_void_p]
    lib.fsvql_bob_update.restype = ctypes.c_int
    lib.fsvql_ring_undo.argtypes = [ctypes.POINTER(ctypes.c_int), ctypes.c_void_p]
    lib.fsvql_ring_undo.restype = None
    lib.fsvql_ring_pre.argtypes = [ctypes.POINTER(ctypes.c_int)]
    lib.fsvql_ring_pre.restype = None
    lib.fsvql_ring_suppressed_overflow.restype = ctypes.c_uint64
    lib.fsvql_ring_suppressed_restored.restype = ctypes.c_uint64
    lib.fsvql_reset_slots.restype = None
    lib.fsvql_reset_counters.restype = None

    base = lib.fsvql_qagame_base()
    if not base:
        print("  FAIL   qagamex64.so is not visible to dl_iterate_phdr")
        return 1
    print("== mapping ==")
    print("  qagame base 0x{:x}".format(base))
    print()

    orig = ctypes.string_at(base + SITE_C_CALL, 5)

    def call_target(data):
        return base + SITE_C_CALL + 5 + int.from_bytes(data[1:5], "little", signed=True)

    def installed(label):
        """Both sites ours, the pointers resolved into this mapping, and the
        SendPendingPredictableEvents entry itself untouched."""
        print("== {} ==".format(label))
        check("fsvql_patch()", lib.fsvql_patch(), 1)
        check("fsvql_status()", lib.fsvql_status(), 1)
        check("site A address", lib.fsvql_site(), base + SITE_A, hexf)
        a = ctypes.string_at(base + SITE_A, SITE_A_LEN)
        check("site A jmp target", jmp_target(a),
              ctypes.cast(lib.fsvql_tramp, ctypes.c_void_p).value, hexf)
        check("site A nop tail", a[14:], b"\x90" * (SITE_A_LEN - 14), bytef)
        check("PM_FootstepForSurface", lib.fsvql_surface_addr(),
              base + PM_FOOTSTEP_FOR_SURFACE, hexf)
        # Site C is a four-byte rel32 edit to an existing `call`: the opcode stays,
        # the call lands on a stub in reach that jumps to our thunk, and the thunk
        # forwards to the entry that a *different* signature found.
        check("site C address", lib.fsvql_site_c(), base + SITE_C_CALL, hexf)
        check_stub(lib, base + SITE_C_CALL, "site C")
        check("one stub page", lib.fsvql_stub_pages(), 1)
        check("forwarding target", lib.fsvql_sendp_addr(), base + SEND_PENDING, hexf)
        check("SendPendingPredictableEvents entry untouched",
              ctypes.string_at(base + SEND_PENDING, SEND_PENDING_HEAD), VANILLA_SENDP,
              bytef)
        print()

    def removed(label):
        print("== {} ==".format(label))
        check("site A bytes", ctypes.string_at(base + SITE_A, SITE_A_LEN), VANILLA_A,
              bytef)
        check("call bytes restored", ctypes.string_at(base + SITE_C_CALL, 5), orig, bytef)
        check("fsvql_status()", lib.fsvql_status(), 0)
        # The interlock: with the sites gone, every pointer into this mapping has
        # to be gone too, so a later install into a *new* mapping cannot call
        # PM_FootstepForSurface or forward to SendPendingPredictableEvents at an
        # address in an unmapped library.
        check("surface pointer dropped", lib.fsvql_surface_addr(), 0, hexf)
        check("site C pointer dropped", lib.fsvql_site_c(), 0, hexf)
        check("forwarding target dropped", lib.fsvql_sendp_addr(), 0, hexf)
        print()

    print("== before install ==")
    check("the call is an e8", orig[0], 0xE8, hexf)
    check("it targets SendPendingPredictableEvents", call_target(orig),
          base + SEND_PENDING, hexf)
    check("fsvql_status()", lib.fsvql_status(), 0)
    print()

    installed("install: both sites, one call")
    first_stub = call_target(ctypes.string_at(base + SITE_C_CALL, 5))
    check("a second fsvql_patch() is a no-op", lib.fsvql_patch(), 1)
    check("call still lands on the same stub", call_target(ctypes.string_at(base + SITE_C_CALL, 5)),
          first_stub, hexf)
    check("still one stub page", lib.fsvql_stub_pages(), 1)
    print()

    print("== ring arithmetic, against a synthetic playerState ==")
    print()
    ring_arithmetic(lib, base)
    listener_replace(lib)
    overflow_strip(lib)
    strip_restores(lib)
    deferral(lib)
    silent_gaits(lib)
    stairs(lib)
    unpatch_fails_closed(lib, base, call_target)

    check("fsvql_unpatch()", lib.fsvql_unpatch(), 1)
    removed("unpatch restores both sites")

    # Reinstalling after a full unpatch is the map-change path: both sites go away
    # with the old mapping and both have to come back.
    installed("reinstall (the map-change path)")
    check("the stub was reused", call_target(ctypes.string_at(base + SITE_C_CALL, 5)),
          first_stub, hexf)
    check("fsvql_unpatch()", lib.fsvql_unpatch(), 1)
    removed("and out again")

    del qa
    far_mapping_in_child(qagame, hook)
    if FAILURES:
        print("HOOK SELF-TEST FAILED: {}".format(", ".join(FAILURES)))
        return 1
    print("hook self-test passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
