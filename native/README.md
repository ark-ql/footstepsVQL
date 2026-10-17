# ql_sndtap — the client-side mover check

Two hooks into the running client, both located by static analysis of the
Steam build during development:

| Hook | Module | RVA | Why |
|---|---|---|---|
| `CG_EntityEvent` | `cgamex86.dll` | `0x19EB0` | every event the client decides to play |
| `S_StartSound` | `quakelive_steam.exe` | `0xDA050` | every start that actually becomes audible |

## Why this exists when `cg_debugevents` / `s_show` already print

Those two retail cvars can **count** — a second `S_StartSound` is a second
printed line no matter what. They cannot **time**. The `s_show` integer is
the mixer's scheduling block: it advances only in 10 ms steps and is
non-monotonic by up to 90 ms (measured during development). That
cannot tell a 2 ms duplicate from a 20 ms one, which is exactly the
distinction that separates the competing hypotheses.

This tap adds what is missing:

- **QPC timestamps**, microsecond resolution, monotonic.
- **The caller's return address**, as a module-relative RVA. Q3 reaches
  `CG_EntityEvent` from three different places; knowing which one delivered
  each half of a duplicate identifies the mechanism almost on its own.
- **The masked event id.** `EV_EVENT_BITS = 0x300` is measured, and the
  hook masks exactly as the engine does, so the two toggle bits do not make
  one logical event look like four.
- **Sound names**, resolved in-process from `s_knownSfx`.

## Build

```
powershell -ExecutionPolicy Bypass -File native\build.ps1
```

Produces `bin\ql_sndtap.dll`, `bin\ql_sndtap_loader.exe`,
`bin\ql_sndtap_ui.exe`. i686 MinGW, `-Wall -Wextra -Werror`.

The build **runs `test_stub.exe` and fails if it does not pass.** The stub
emitter pushes arguments at a constant displacement, which is easy to get
wrong by one slot, and the symptom of getting it wrong is a crash inside
Quake Live rather than a clear error. The self-test exercises the real
emitter against a synthetic target: argument order, return address, stack
balance over 2000 calls, and the suppress path.

## Run

Double-click:

```
bin\ql_sndtap_ui.exe
```

That is the whole tool. No Steam launch option, no wrapper script. The
panel waits for `quakelive_steam.exe`, injects the DLL itself, and attaches
to the page the DLL publishes. Order does not matter — start the game
first or the panel first. **Start Quake Live** in the panel fires
`steam://rungameid/282440` if you want it to do that too.

When the game exits the panel stays open and re-arms, so the next
framerate cell does not need it restarted.

Log lands beside the DLL as `bin\ql_sndtap_<stamp>.log`.

`ql_sndtap_loader.exe` is the same injection with no window, for scripted
runs. It is not needed for normal use.

**Do not load the rega hood at the same time.** It patches the cgame
prediction lookback, which is the mechanism being measured.

## The panel

Live counters plus four knobs:

| Knob | Default | Effect |
|---|---|---|
| log `CG_EntityEvent` | on | event lines |
| log `S_StartSound` | on | sound lines |
| ledger dry run | off | counts what the ledger *would* suppress, changes nothing audible |
| ledger ACTIVE | off | actually drops replayed events — the candidate fix |

Plus the ledger window in microseconds.

## The ledger (fix F2)

`MAX_PS_EVENTS` is 2, so the client only has two slots of memory when
deciding "have I already played this?". Enlarging the real array is not
possible on shipped binaries — it is a networked, delta-compressed,
fixed-layout struct shared with the server. The ledger sidesteps that: a
256-entry client-local ring in the DLL, keyed on `(entity, masked event)`,
consulted before the event is allowed through.

**Use the dry run first.** Set the window from the measured gap
distribution, never by feel. Too wide and it eats the second shot of an LG
burst 50 ms after the first, which is a worse bug than the one being fixed
and a quieter one. The panel keeps `would_suppress` and `suppressed` as
separate counters for exactly this reason.

## Safety notes

- Signatures are matched with the embedded absolute addresses wildcarded.
  cgame is rebased at load and ASLR can move the engine, so raw byte
  matching would be wrong. A mismatch logs and refuses to patch.
- cgame is unloaded and reloaded on every map change and `vid_restart`. A
  watcher thread re-arms the hook; without it the tap would silently stop
  logging mid-session and produce a clean-looking but wrong dataset.
- Logging is double-buffered with a writer thread, and producers drop a
  line rather than block. At 1000 fps a synchronous write would change the
  frame timing being measured.
- Nothing is written to the Steam game directory.
