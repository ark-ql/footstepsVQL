#!/usr/bin/env python3
"""Uniqueness-check candidate byte signatures for the S5 hook in qagamex64.so.

House rule (from independed-render-ql-patch, and 2.1's own k_core practice):
a signature that matches more than once does not ship. Signatures must also
contain no load-address-dependent bytes. In this ELF that means RIP-relative
displacements and intra-module `call`/`jmp` rel32 are FINE -- they are position
independent -- but GOT-loaded absolutes are not, so any `mov reg,[rip+x]` that
reaches .got is wildcarded here.

`??` marks a wildcard byte.

Usage:
    python scripts/_qagame_sigs.py <qagamex64.so>
"""

import sys
from pathlib import Path

TEXT_NAME = b".text"

SIGS = {
    # 2.1's shipped pattern, re-checked here so the new artifact and the live
    # one are known to key off the same site.
    "PM_Footsteps bob core (2.1 k_core)": (
        "8b 48 08 f3 0f 2a c1 83 c1 40 f3 0f 58 c1 f3 0f 2c d0 "
        "81 e2 ff 00 00 00 89 50 08"
    ),
    # Hook B. rdi = ps, runs once per client per frame from ClientEndFrame.
    # The 0x1ec / 0x6c / 0x80 / 0x1c / 3 / 8 constellation is the event-tag
    # block and should be unique. jge rel32 wildcarded for safety.
    "SendPendingPredictableEvents entry": (
        "41 56 41 55 41 54 55 53 "
        "8b 87 ec 01 00 00 48 89 fb 3b 47 6c 0f 8d ?? ?? ?? ?? "
        "41 89 c4 83 e0 01 44 8b af 80 00 00 00 48 83 c0 1c "
        "41 83 e4 03 41 c1 e4 08 44 0b 24 87"
    ),
    # Narrower variant, body only, in case the prologue differs across builds.
    "SendPendingPredictableEvents body": (
        "8b 87 ec 01 00 00 48 89 fb 3b 47 6c 0f 8d ?? ?? ?? ?? "
        "41 89 c4 83 e0 01 44 8b af 80 00 00 00"
    ),
    # Stage 2 uses this one. Extended through the `call G_TempEntity` so that
    # function's address is derived from a rel32 inside a byte range we have
    # actually verified, rather than from an offset we assumed. Both rel32s are
    # wildcarded; they are intra-module and therefore position independent, but
    # they would differ in another build.
    #
    #   G_TempEntity = site + 0x4C + 5 + *(int32 *)(site + 0x4D)
    #
    # Instruction boundaries from the entry are 2,4,6,7,8,14,17,... so 14 is a
    # clean displacement length for the trampoline: it ends exactly after
    # `mov eax,[rdi+0x1ec]`, leaving `mov rbx,rdi` as the resume point.
    "SendPendingPredictableEvents + call (stage 2)": (
        "41 56 41 55 41 54 55 53 "                    # +0x00 pushes
        "8b 87 ec 01 00 00 "                          # +0x08 mov eax,[rdi+0x1ec]
        "48 89 fb "                                   # +0x0e mov rbx,rdi
        "3b 47 6c 0f 8d ?? ?? ?? ?? "                 # +0x11 cmp / jge
        "41 89 c4 83 e0 01 "                          # +0x1a
        "44 8b af 80 00 00 00 "                       # +0x20 mov r13d,[rdi+0x80]
        "48 83 c0 1c 41 83 e4 03 41 c1 e4 08 "        # +0x27
        "44 0b 24 87 "                                # +0x33 or r12d,[rdi+rax*4]
        "c7 87 80 00 00 00 00 00 00 00 "              # +0x37 mov [rdi+0x80],0
        "48 8d 7f 14 "                                # +0x41 lea rdi,[rdi+0x14]
        "44 89 e6 "                                   # +0x45 mov esi,r12d
        "41 83 c4 0d "                                # +0x48 add r12d,0xd
        "e8 ?? ?? ?? ??"                              # +0x4c call G_TempEntity
    ),
    # Hook C, stage 3 / S2. The tail of ClientThink_real, which is where the whole
    # write-broadcast-undo window lives inside a single function call:
    #
    #   oldEventSequence is saved before Pmove (so a ring event written from our
    #   site A hook is *seen* by the comparison below and ent->eventTime is set),
    #   then BG_PlayerStateToEntityState drains the ring into ent->s.event, which
    #   is how other players actually hear the footstep, then
    #   SendPendingPredictableEvents ships any second event as a temp entity.
    #   Immediately after that call is the only point where others have been
    #   served and the mover's snapshot has not yet been built.
    #
    # S2 therefore does not displace instructions at all: it rewrites the rel32 of
    # the `call SendPendingPredictableEvents` at sig + 0x2F so it lands on a thunk
    # that calls the original and then undoes the ring. Four bytes, and the e8
    # opcode is never touched. Both rel32s are wildcarded; the `74 0b` is not,
    # because a short jump's displacement is intra-region and position independent.
    "ClientThink_real ring window (stage 3 / S2)": (
        "48 8b bd 40 02 00 00 "                       # +0x00 mov rdi,[rbp+0x240]
        "8b 44 24 38 "                                # +0x07 mov eax,[rsp+0x38]  oldEventSequence
        "39 47 6c "                                   # +0x0b cmp [rdi+0x6c],eax  ps->eventSequence
        "74 0b "                                      # +0x0e je  -> skip eventTime
        "41 8b 44 24 24 "                             # +0x10 mov eax,[r12+0x24]  level.time
        "89 85 7c 02 00 00 "                          # +0x15 mov [rbp+0x27c],eax ent->eventTime
        "ba 01 00 00 00 "                             # +0x1b mov edx,1
        "48 89 ee "                                   # +0x20 mov rsi,rbp         &ent->s
        "e8 ?? ?? ?? ?? "                             # +0x23 call BG_PlayerStateToEntityState
        "48 8b bd 40 02 00 00 "                       # +0x28 mov rdi,[rbp+0x240]
        "e8 ?? ?? ?? ??"                              # +0x2f call SendPendingPredictableEvents
    ),
    # Needed to pick the right surface sound for an injected footstep.
    # First insn loads pml.groundTrace.surfaceFlags via RIP -- wildcarded.
    "PM_FootstepForSurface": (
        "8b 15 ?? ?? ?? ?? 31 c0 f6 c6 20 75 ?? f6 c6 10 b0 02 74 ?? "
        "f3 c3"
    ),
    # Same function, keyed on the QL-specific surface constants instead.
    "PM_FootstepForSurface tail": (
        "f7 c2 00 00 08 00 b0 51 75 ?? 81 e2 00 00 10 00 83 fa 01 "
        "19 c0 83 e0 af 83 c0 52 c3"
    ),
}


def load_text(path):
    data = path.read_bytes()
    if data[:4] != b"\x7fELF" or data[4] != 2:
        raise SystemExit(f"{path}: not ELF64")
    e_shoff = int.from_bytes(data[0x28:0x30], "little")
    e_shentsize = int.from_bytes(data[0x3A:0x3C], "little")
    e_shnum = int.from_bytes(data[0x3C:0x3E], "little")
    e_shstrndx = int.from_bytes(data[0x3E:0x40], "little")

    def shdr(i):
        o = e_shoff + i * e_shentsize
        return (int.from_bytes(data[o:o + 4], "little"),
                int.from_bytes(data[o + 0x10:o + 0x18], "little"),
                int.from_bytes(data[o + 0x18:o + 0x20], "little"),
                int.from_bytes(data[o + 0x20:o + 0x28], "little"))

    _, _, stroff, _ = shdr(e_shstrndx)
    for i in range(e_shnum):
        nameoff, addr, off, size = shdr(i)
        base = stroff + nameoff
        if data[base:data.index(b"\0", base)] == TEXT_NAME:
            return data[off:off + size], addr
    raise SystemExit("no .text")


def parse(sig):
    out = []
    for tok in sig.split():
        out.append(None if tok == "??" else int(tok, 16))
    return out


def scan(text, pat):
    n, m = len(text), len(pat)
    first = pat[0]
    hits = []
    start = 0
    while True:
        if first is None:
            raise SystemExit("signature must not start with a wildcard")
        i = text.find(bytes([first]), start)
        if i < 0 or i + m > n:
            break
        if all(p is None or text[i + k] == p for k, p in enumerate(pat)):
            hits.append(i)
        start = i + 1
    return hits


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    so = Path(sys.argv[1])
    text, vaddr = load_text(so)
    print(f"{so.name}: .text vaddr 0x{vaddr:x}, {len(text)} bytes\n")
    width = max(len(k) for k in SIGS)
    ok = True
    for name, sig in SIGS.items():
        pat = parse(sig)
        hits = scan(text, pat)
        wild = sum(1 for p in pat if p is None)
        addrs = ", ".join(f"0x{vaddr + h:x}" for h in hits[:4])
        verdict = "UNIQUE" if len(hits) == 1 else ("MISSING" if not hits else "AMBIGUOUS")
        if len(hits) != 1:
            ok = False
        print(f"{name:<{width}}  {len(pat):>3}B ({wild} wild)  "
              f"{len(hits)} hit  {verdict:<9} {addrs}")
    print()
    print("all signatures unique" if ok else "NOT ALL UNIQUE -- do not ship as-is")


if __name__ == "__main__":
    main()
