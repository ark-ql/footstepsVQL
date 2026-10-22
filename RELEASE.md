# footstepsVQL 1.1.1 — `c96f49de` + `8fae5aa1`

**Released.** The native library `c96f49de` was built and gated on 2026-10-07
and has been in production since 7–11 October 2026 on four community servers,
with the 1.1 plugin (see *In production*). The public write-up is `README.md`.
This file is the full record: artifacts, install, every change since the
2026-09-27 code review, the gates, the live measurements and the known limits.

**Version 1.1.1 (2026-10-18): plugin `8fae5aa1`, library unchanged.** The
plugin also finds the library in `footstepsVQL_lib/` next to it, so it can be
installed from a QLSM plugin repository: QLSM ships a plugin's extra files only
in a subfolder. A library next to the plugin is still tried first, and nothing
else changed. `release/` now holds the library in `footstepsVQL_lib/` and a
`qlsm-repository.json`. See *Install* and *What changed*.

**Version 1.1 (2026-10-07): `.so` `c96f49de`, plugin `7ce3da41`.** Fixes a
failure seen once in production on 1.0: after a map change, qagame was mapped
2.26 GB from the hook, out of reach of site C's `call rel32`, and 1.0 refused
with `-5`, leaving that server stock until a later map change. Site C's call
now always lands on a small stub mapped next to qagame, which jumps to the
hook with a full 64-bit address. All offline gates pass, including a new one
that loads qagame more than 2 GB from the hook; 1.0 fails that one with `-5`.
See *What changed*.

**Version 1.0 (2026-10-04): `.so` `b6da4845`, plugin `351f501f`.** The plugin
names its version and build in `!fsvstatus` and in the load log. The two
builds before it added two rules that keep a clock footstep from silently
replacing another sound: injected footsteps are at least 350 ms apart
(`5a199d8d`; a missed footstep at the top of stairs, which a second human
listener heard), and nothing is injected within 50 ms after a jump, jump pad
or landing (`b6da4845`).

A replacement for `footsteps250` 2.1. One native library and one plugin that
runs unmodified on **minqlx and minqlxtended**. No modes: it always does what
was called s2 during development.

Builds are named by the first 8 hex digits of the `.so`'s sha256. File names
of logs below are the author's raw session logs, and some rig and analysis
helpers named below were not published; the hook, the plugin, the build and
gate scripts and the listener measurement tools are all in this repository.

## In production

Four community servers, one LG duel and three CA, checked read-only from each
server's journal and logs, last on 2026-10-17. All four run 1.1, patched.

| Server | footstepsVQL since | 1.1 since | Restarts | Map loads | Install failures | Footsteps delivered | Busiest hour |
|---|---|---|---|---|---|---|---|
| LG duel | 2026-09-28 (earlier builds, 1.0 from 10-05) | 2026-10-08 | 26 | 61 | 0 | ≥ 78,597 (2 not delivered) | 7.4 players |
| CA 1 | 2026-10-07, replacing footsteps250 | 2026-10-07 | 4 | 163 | 0 | ≥ 30,793 (0) | 12.9 |
| CA 2 | 2026-10-10, replacing footsteps250 | 2026-10-10 | 9 | 182 | 0 | ≥ 13,258 (1) | 16.5 |
| CA 3 | 2026-10-06 (1.0) | 2026-10-11 | 8 | 147 | 0 | ≥ 4,106 (0) | 15.4 |
| **All four** | | | **47** | **553** | **0** | **≥ 126,754 (3)** | |

- **Restarts, map loads and failures** are counted from the server's journal
  over the whole time footstepsVQL was loaded, except CA 3, whose row starts at
  its first 1.1 load. Its one 1.0 refusal is described below. A failure is any
  refusal or error line from the plugin; every map load re-installs the hook.
- **Footsteps delivered is a lower bound.** The counters reset at every
  restart, so only runs caught by a check are counted, each at the highest
  value seen. "Not delivered" is injected minus delivered at the moment of the
  check; a check during play can catch one still in flight.
- **Busiest hour** is the highest 60-minute average of connected humans
  (spectators in, bots out). The logs it comes from rotate after a few days,
  so the record only goes back to 8–12 October.
- **After each install** the file hashes were checked, the logs showed no
  failure line across map changes, and the hook reported itself patched. On two
  servers the live process memory was read to confirm that site C's call goes
  through the stub into the hook.

**Before 1.1, in the same servers' logs:**
- **The deferral works live.** A footstep waits one command when the engine
  has no room to ship it. The LG duel server counted 97 deferred with 1990 of
  1990 delivered (2026-10-05), and 596 deferred with 14,198 of 14,198 the next
  day. CA 3 counted 57 deferred with 14,411 of 14,411 (2026-10-06), and 1962
  stock footsteps stripped for listeners from players in the ~390–500 fps band.
- **Transitions:** on CA 3 on 2026-10-06, 17 map changes, 32 re-installs,
  12 match starts, 9 `map_restart` and one `!abort`, with no failure line.
- **The 1.0 refusal (`-5`), 2026-10-07 18:03, CA 3.** At a map change, qagame
  was mapped 2.26 GB below the hook library, out of reach of site C's
  `call rel32`. The re-patch refused and the server ran stock, safely: site A
  was rolled back, and all three sites read vanilla in process memory. It
  re-patched at a map change at 22:20, when qagame landed about 1.6 GB away.
  Fixed in 1.1, which every server now runs.

## Artifacts

| File | sha256 | Size | Built |
|---|---|---|---|
| `footstepsVQL_hook.so` | `c96f49decdef801a5945a5b81dd190b5a8a52e070f5647e67e2cae93fc8473cf` | 27520 B | test server, gcc 13.3.0, glibc 2.39 |
| `footstepsVQL.py` | `8fae5aa17743c2be191d1cfbcc30d4ea40469d2856b7949cdb7bf34c1b9b5597` | 25480 B | LF line endings, version 1.1.1 |

`scripts/_stage_release.py` refuses to stage anything that does not match these
two rows, and writes `release/SHA256SUMS.txt`.

The `.so` needs only `GLIBC_2.2.5` and `GLIBC_2.4` (measured with `objdump -T`),
so the same binary loads on Ubuntu 24.04, on Debian 12, and in the Ubuntu 16 minqlx
containers. It imports no file or I/O function. It finds its three targets by
byte signature, not by address, and refuses rather than patches if a signature is
missing or matches twice. All seven signatures are unique in `qagamex64.so`
`091d568f46a2…`. Before installing the bob hook it also checks the footstep gate
around the site (see *What changed*), and refuses with `-6` if it differs.

Supersedes `c96f49decdef…` / `7ce3da41db70…` (1.1), `b6da48457882…` / `351f501f6a1e…` (1.0), `b6da48457882…` / `5597190d1495…`, `5a199d8d5120…` / `5597190d1495…`, `d6d28bed98c5…` / `5597190d1495…`, `bd6c781b530e…` / `5597190d1495…`, `c62aba41f340…` / `5597190d1495…`, `e4573c6e00e3…` / `5597190d1495…`, `d7ea25e425ba…` / `f744e90f6489…`, `10b40ab62220…` / `167e2911d4d5…`, `e10ad5206f6f…` / `167e2911d4d5…`, `9464c3c4ddc2…` / `9b0d042caf48…`, `ccf69b7b7983…` / `ac3433768a24…`,
and the test-rig pair `118945eb2807…` / `49bdd8308a9d…`.

## What it does

footsteps250 quantizes the networked `bobCycle`, so the server and the client's
own prediction disagree. An unpredicted footstep then enters the two-slot event
ring and replays the jump or shot in the other slot: the doubled sound.

footstepsVQL leaves `bobCycle` vanilla and keeps a private clock: one footstep
per 476 ms (2.10/s), the tempo a real 250 fps client runs at. It does this with two
sites, installed together or not at all:

- **Site A**, the `bobCycle` store in `PM_Footsteps`: vanilla cycle, private clock.
- **Site C**, the call to `SendPendingPredictableEvents` in `ClientThink_real`.
  Each footstep a fast client's own pmove did not produce is written into his
  event ring for other players' snapshots and taken back out before his own
  snapshot is built.

| | Result |
|---|---|
| Other players hear | a steady 476 ms clock from anyone above ~390 fps, and in practice from ~355 fps within seconds (see *Known limits*); below ~320 fps, stock |
| The mover hears | only what his own client predicts — nothing extra |
| Weapon/jump replay | none |
| Same footstep twice | none |

## Install as a footsteps250 substitute

1. Upload `footstepsVQL.py` and the `footstepsVQL_lib/` folder (it holds
   `footstepsVQL_hook.so`) into the plugin directory (`minqlx-plugins/` or
   `minqlxtended-plugins/`). The `.so` may also sit directly next to the
   plugin, as before 1.1.1. Keep the `_hook` suffix. A file named
   `footstepsVQL.so` would shadow the plugin on import.
2. In `qlx_plugins`, replace `footsteps250` with `footstepsVQL`. The footsteps250
   files can stay on disk.
3. Restart the server, or change map. If footsteps250 was only unloaded at
   runtime, its trampoline stays on the site until the next map. footstepsVQL
   logs `bobCycle site not found … takes over on the next map` and does exactly
   that.
4. Check the console for two lines:
   `[footstepsVQL] version 1.1.1: .so c96f49de, .py 8fae5aa1` and
   `[footstepsVQL] plugin load: hook installed (qagame base …, site …, ring …)`.
   In game, `!fsvstatus` must start with the same version line and say
   `now: patch is patched and active`.

**From a QLSM plugin repository** (QLSM 1.43.0 or newer): Settings →
Repositories → Add Repository, with the URL
`https://github.com/ark-ql/minqlx-plugins` (the shared plugins repository,
which carries the same files and manifest entry; `scripts/_sync_plugins_repo.py`
copies them there) or this repository's
`https://github.com/ark-ql/footstepsVQL/tree/main/release`. Tick footstepsVQL,
pick the runtime, and Download selected. QLSM fetches the plugin and
`footstepsVQL_lib/footstepsVQL_hook.so`, pushes them to the hosts, and adds a
settings form for the three cvars. Then tick footstepsVQL in the instance's
Plugins tab (and untick footsteps250), save and restart. Check step 4. If
footstepsVQL was uploaded to that instance by hand before, delete those
instance files (`footstepsVQL.py`, `footstepsVQL_hook.so`) first: an
instance's own files always win over QLSM's shared copy. Verified on the test
server on 2026-10-18: a fresh install on one instance, and on the other a
switch from a hand upload after deleting it; both load 1.1.1 from
`footstepsVQL_lib/` with the hook patched.

No cvar needs to be set. `qlx_footstepsVQLMode` and `qlx_footstepsVQLEmit` no
longer exist; if a config still sets them, they are ignored.

**Upgrading from `ccf69b7b` in place.** Replace both files, then `!reload
footstepsVQL` or change map. The plugin loads a content-hashed copy, so the new
bytes get a new mapping. The old commands `!vql*` are gone.

**QLSM note.** footsteps250's notes record that QLSM's `plugin_compat.py` matches
`import minqlx` and `RET_*`, and the runtime shim contains both. A direct
per-instance upload of footsteps250's identical shim passed untouched, and this
build's predecessor loaded that way on our test server. A preset save or deploy was
never tested.

**Rollback.** Put `footsteps250` back in `qlx_plugins` and change map.
`!fsvunload` restores vanilla bytes at both sites immediately.

## Commands and cvars

Named like footsteps250's, with an `fsv` prefix. Each command answers with the
state before it ran, what it did, and the state after.

| Command | Perm | |
|---|---|---|
| `!fsvstatus` | 1 | the version and build (first 8 hex digits of each file's sha256), what is loaded and patched, what happens on the next map, the counters. Alias `!footstepsvql`. Rate-limited to one reply per 5 s server-wide |
| `!fsvstatus reset` | 1 | report, then zero the counters for a clean measurement (audit line in the log) |
| `!fsvload` | 2 | apply now and re-apply on future maps |
| `!fsvunload` | 2 | restore vanilla bytes at both sites and stop re-applying |

| Cvar | Default | |
|---|---|---|
| `qlx_footstepsVQLEnabled` | `1` | `0` leaves qagame vanilla. `!fsvload` / `!fsvunload` write it |
| `qlx_footstepsVQLSo` | empty | absolute path override for the `.so` |
| `qlx_footstepsVQLVerbose` | `0` | `1` adds routine per-map lines |

**Logging.** A healthy server writes one console line at plugin load and nothing
else. Every failure, retry and recheck is logged. Per-map re-patch lines appear
only with `qlx_footstepsVQLVerbose 1`. No secrets are ever logged.

## Server cost

Measured on 2026-10-03 on `d6d28bed`, on our test server (Xeon Icelake), with
`server/perf_bench.c`. It runs in its own process against a private `dlopen`
of the real qagame, with the bob site, the ring write, the undo and the strip
all live:

| | Cost |
|---|---|
| Hook body per command (site A + site C before/after) | 28–33 ns, flat for 1, 8, 16 and 32 players and 1–4 ms commands |
| Assembly trampoline save/restore (9 GPRs, 16 xmm, flags), timed as a replica | ~20 ns |
| **Per command, upper bound** (site A runs only on grounded, moving commands) | **~50 ns** |
| 16 players all at 1000 fps | 0.8 ms CPU per second, 0.08% of one core |
| 32 players all at 1000 fps | ~0.16% of one core |
| First `fsvql_patch()`: self-test 9.2 ms + signature scan and install | 14.9 ms, once per library load, on the game thread |
| Later map changes (scan and install, no self-test) | ~6 ms, inside the map load |

The hot path makes no system calls and takes no locks. The `.so` imports only
`dl_iterate_phdr`, `mprotect`, `mmap`, `munmap`, `memset` and `strstr`
(`mmap` and `munmap` since 1.1, to place the stub page): nothing that does I/O.
The plugin has no per-frame hooks. It prints on load, map change and unload
only, and the routine lines only with `qlx_footstepsVQLVerbose 1`. Its file I/O
is staging the `.so` copy at load. The counters stay in the build: they cost a
few ns of the 30 and are what `!fsvstatus` and the rig read. Not measured: the
cost of qagame's own Pmove per command, which a high-fps player already
multiplies whether or not footstepsVQL is loaded.

## Survivability

The engine reloads or reinitialises qagame on a map change, `map_restart`, match
start and match abort, and every hook site goes with it. Both engines dispatch
`new_game` for all of those, on minqlxtended as `handle_new_game(is_restart)`,
and on the next server frame the plugin installs both sites again. A 5 s recheck
catches a late remap. A server restart installs at plugin load. The gap is at
most one server frame of vanilla behaviour, and each player's clock restarts.
`!fsvunload` survives everything except a process restart, because the cvar is not
saved; put `set qlx_footstepsVQLEnabled 0` in `server.cfg` to make it stick.

Measured on `9464c3c4`, test server, 2026-09-27. This build installs and
re-installs through the same code. Evidence per transition: the
engine log shows no footstepsVQL failure, retry or recheck line, and the recorder
shows footsteps injected and delivered again after it. Log:
`steps-live-9464c3c4-below-transitions-20260927.tsv`.

| Transition (log time) | First delivery after it |
|---|---|
| `!rcon map_restart` 22:38:28, then map change 22:38:41 | 22:39:01, 2 of 2 (no movement in between, so the restart itself shows only as no error) |
| server restart 22:39:14 | load line; 22:41:48, 2 of 2 |
| server restart 22:42:04 | load line; then the map change below |
| map change to `ca` 22:42:36 | 22:43:11, 1 of 1 |
| match start 22:43:01 | 22:43:11 onward |
| `!abort` 22:43:39 | 22:43:49, 1 of 1 |
| match start 22:44:10 | 22:44:28, 1 of 1 |
| map change to `servitude` 22:47:45 | 22:48:14, 2 of 2 |

After the last transition, `verify_patch.py` found both sites ours in the live
mapping and the `SendPendingPredictableEvents` entry vanilla. With
`qlx_footstepsVQLVerbose 1`, each transition also logs `new_game: hook installed
(qagame base …)`.

## Live — `d6d28bed`, test server, 2026-10-03 19:08–19:40

Loaded through the QLSM UI with minqlxtended: pid `577207`, only
`/tmp/footstepsVQL_hook_d6d28bed98c5.so` mapped, `hook installed (qagame base
0x7127dee00000, site …58570, ring …5a1d9)`.

**Method.** One human mover, the author, on `thunderstruck nlg` (`devmap`). Four
paused bots were the listeners. `server/botsnap.py` read every snapshot built
for each bot out of process memory and replayed cgame's event rules on it, so
the numbers are what a listener in each bot's place is sent and would play.
Network loss is not modelled. The runs were 30 s each, marked from game chat by
`server/conductor.py`. The added round trip (netem) was 50 ms for runs 1–4 and
0 ms after that (ping about 6). In every run, all four bots had the mover in
view for all 29 whole seconds and received identical streams. Logs: `botsnap-session-d6d28bed-20261003-1907.tsv`
and `runs-session-d6d28bed-20261003-1907.tsv`.

**Stairs, before and after** (the same staircase, a single pass each):

| | Build | fps | Footsteps/s | 1/2/3/4+ per s | Near-doubles <150 ms | Holes >700 ms | Injected / stripped |
|---|---|---|---|---|---|---|---|
| 18:25 | `bd6c781b` | 250 | 3.000 | 0/41/21/34% | 14% (four 25 ms pairs) | 0% | 18 / 0 |
| 19:08 | `d6d28bed` | 250 | 3.000 | 0/41/24/34% | 0% | 1% | 1 / 0 |
| 19:11 | `d6d28bed` | 990 | 2.966 | 3/24/34/34% | 1% (one 25 ms pair) | 0% | 33 / 0 |

On `bd6c781b`, every second with an injection also had command gaps over 32
ms: the air-time bug. On `d6d28bed`, the 250 fps run has none over 32 ms
(`fs_note_command`), and its one injection fell on a grounded stretch. The
990 fps run injects only in the flat stretches and keeps the stair rate within
1% of 250 fps. `bd6c781b`'s long stair pass was 2.770/s against 2.843/s for
stock 250. Logs for the 18:25 run: `botsnap-session-bd6c781b.tsv`,
`runs-session.tsv` (run 2).

**Flat, by framerate.** Going up is runs 5–13, from 250; coming down is runs
14–25, from 500. All at 0 ms added:

| fps (command interval) | Path | Footsteps/s | On exactly 2 | Median gap | Near-doubles | Holes | Injected / stripped |
|---|---|---|---|---|---|---|---|
| 250 (3.82 ms) | stock | 2.103 | 83% | 475 | 0% | 0% | 0 / 0 |
| 300 (3.28) | stock | 2.241 | 69% | 450 | 0% | 0% | 0 / 0 |
| 333 (3.00) | stock | 2.379 | 52% | 400 | 0% | 0% | 0 / 0 |
| 360 going up (2.80) | stock, latched to the clock after 22 s | 1.897 | 83% | 475 | 0% | 2% | 11 / 12 |
| 400 (2.53) | clock | 2.034 | 86% | 475 | 0% | 0% | 60 / 45 |
| 450 (2.27) | clock | 1.931 | 86% | 475 | 0% | 0% | 56 / 25 |
| 500 (2.05) | clock | 2.034 | 90% | 475 | 0% | 0% | 59 / 10 |
| 750 (1.36) | clock | 2.034 | 90% | 475 | 0% | 0% | 61 / 2 |
| 990 (1.04) | clock | 2.069 | 86% | 475 | 0% | 0% | 59 / 0 |
| 500 → 400 → 380 → 360 coming down (two passes) | clock, hysteresis holds | 2.000–2.103 | 83–90% | 475 | 0% | 0% | 57–61 / 6–58 |
| 325 coming down (3.06) | clock for its first second, then stock | 2.207 | 52% | 425 | 0% | 0% | 1 / 1 |
| 310 (3.19) | stock | 2.207 | 62% | 425 | 0% | 0% | 0 / 0 |
| 250 at the end (3.88) | stock | 1.862 | 86% | 475 | 0% | 0% | 0 / 0 |

At 50 ms added (runs 3 and 4), 990 fps flat was 2.034/s and 250 fps flat was
2.069/s, both 0% near-doubles and 0% holes, the same as at 0 ms. Stock 250 varies
1.86–2.10/s between passes, depending on the path and the stops. Runs 17 and 18
(360 fps) were discarded: the paused listener bots dropped mid-run with `Server
command overflow`, a rig limit, not a
footstepsVQL one.

**Read.** No near-doubles anywhere on flat ground. Below the switch, stock is
untouched. From 400 fps up, and coming down to 360, listeners get the clock:
median 475 ms, 86–90% of seconds on two. The one cost is the moment the clock
takes over from stock, arriving from below. It produced the only flat-ground
hole (*Known limits*).

**Stairs, by framerate (19:44–20:05, runs 26–54).** Up and down the same
staircase, at 0 ms added and again at 50 ms:

| fps | 0 ms added: footsteps/s | 50 ms added: footsteps/s |
|---|---|---|
| 250 | 3.207, 3.069 | 2.862, 3.207 |
| 300 / 320 / 325 | 3.034 / 3.172 / 3.241 | — / — / 3.172 |
| 360 | 3.000, 2.862 | 2.862, 2.931 |
| 380 | 3.103, 2.897, 2.966 | 2.931 |
| 400 | 3.172, 3.000, 2.966 | 2.966 |
| 450 / 500 / 990 | 3.172 / 3.069, 2.966 / 3.069 | — / 3.034 / — |

- **0% near-doubles in all 25 valid stair runs**, and holes over 700 ms in 0–2% of gaps.
- **The clock path matches stock:** about 2.9–3.2 footsteps/s from 360 to 990 fps, against 2.9–3.2 for stock at 250–325. On stairs it injects only on the ground, about 1 per second.
- **The added delay changes nothing measurable:** the 50 ms runs fall within the same pass-to-pass spread (about ±5%), with the same command intervals.

**Movement styles (20:10–20:21, runs 55–63).** For each style, the share of
the 29 s spent on the ground, then the footsteps heard per ground second:

| Style | 250 fps (stock) | 400 fps | 990 fps |
|---|---|---|---|
| Bunnyhop | 9.7 s, 1.954 | 7.5 s, 1.862 (fell, jumped stairs; not a clean pass) | 9.1 s, 1.980 |
| Strafe-jump | 5.5 s, 1.999 | 4.9 s, 2.239 | 6.2 s, 2.113 |
| Run, jump and fire | 13.7 s, 2.120 | 12.5 s, 2.081 | 10.1 s, 1.789 |
| Jump pad, repeated (20:33, fresh session `…-2031`) | 3.6 s, 1.942 (7 footsteps) | 5.3 s, 1.498 (8) | 3.5 s, 1.982 (7) |
| Flat running, for reference | 27.8 s, 2.192 | — | 28.3 s, 2.121 |

- **0% near-doubles throughout.**
- **The clock does not run in the air:** the 990 fps bunnyhop injected 19 times, all on ground time. As a whole-run rate, bunnyhopping comes out at 0.66/s on stock 250 and 0.62/s at 990; strafe-jumping at 0.38 and 0.45.
- **Shots on the overflow path:** at 990 fps, run-jump-fire sent 1 of 18 footsteps per bot through the overflow temp entity. Stock 250 sent 8.

**The mover's ears.** The author listened as the mover through every run of
the session, 19:08–20:36: 250–990 fps, flat ground, stairs, bunnyhop,
strafe-jump, run-jump-fire, jump pad and CA, at 0 and 50 ms added. He heard
nothing unusual or unexpected.

**Mover check, measured (20:42–20:52).** `bin\ql_sndtap_ui.exe` hooked into
the author's own client, with default logging and the ledger off. One tap
session covered 250, 360, 400 and 990 fps, with continuous fire, jumps, stairs
and the jump pad. `scripts/_replay_signature.py` was run on each block (cut out
with `scripts/_tap_slice.py`) and on the whole log. A true replay is the same
event played again within 40 ms of its first play and within 100 µs of another
event in the same drain.

| Block | Local events | Fire / footstep / jump / fall short | True replays | Hook in the block (injected / stripped) | Verdict |
|---|---|---|---|---|---|
| 250 fps | 1133 | 990 / 72 / 38 / 25 | 0 | ~0 / 0 (stock) | PASS |
| 360 fps | 1491 | 1331 / 63 / 47 / 39 | 0 | ~89 / ~50 | PASS |
| 400 fps | 1196 | 1061 / 59 / 44 / 29 | 0 | 46 / 38 | PASS |
| 990 fps | 1879 | 1757 / 23 / 54 / 38 | 0 | 91 / 0 | PASS |
| **Session** | **5699** | 5139 / 217 / 183 / 131 | **0** | | **PASS** |

footsteps250 2.1 gives 9 true replays in 689 local events
(`ql_sndtap_20260920-230302.log`). Here, 14 shots drained in the same batch
as a footstep, all 47–56 ms after the previous shot: fire cadence, not replays.
Server side, the hook's counters for the whole load stood at 2133 injected,
2133 delivered, 2133 undone, 0 skipped, 0 missed. The few own footsteps at
990 fps are stock QL: at that rate the mover's own `bobCycle` barely advances,
and the hook adds nothing to what the mover hears. Logs:
`ql_sndtap_20261003-204241.log`, with the per-block cuts in
`tap-20261003-{360,400,990}fps.log`.

**CA with unpaused bots (20:24–20:28, `server/rig_ca.sh`).** Thunderstruck
CA, 5 v 5 (the mover plus 9 bots), a couple of rounds at each framerate:

| fps | Bot-seconds in view | Footsteps/s | Near-doubles | Holes >700 ms | Entity / overflow | Injected / stripped |
|---|---|---|---|---|---|---|
| 250 | 231 | 1.039 | 0% | 38% | 136 / 104 | 2 / 0 |
| 400 | 232 | 1.069 | 0% | 33% | 184 / 64 | 20 / 15 |
| 990 | 230 | 1.143 | 0% | 37% | 207 / 56 | 28 / 0 |

- **Hook counters over the CA match:** 121 injected, 121 delivered, 121 undone, 0 skipped, 0 missed, **0 deferred**.
- **Whole load, 19:06–20:28, all of the above:** 1892 injected and 1892 delivered, 0 skipped, 0 missed, 0 deferred. 1 command gap over 32 ms in 662,773.
- **Rounds, deaths and air time** account for the low rate and the long gaps, the same at every framerate. The overflow share is combat: shots and pain hold the entity slot.
- Logs: `botsnap-ca-d6d28bed-20261003-2023.tsv`, `runs-ca-d6d28bed-20261003-2023.tsv`.

## Live — `9464c3c4`, test server, 2026-09-27 22:13–22:18

Loaded through the QLSM UI with minqlxtended: pid `1489820`, mapped
`/tmp/footstepsVQL_hook_9464c3c4ddc2.so`. The load line was `hook installed
(qagame base 0x75eff4600000, site …58570, ring …5a1d9)`, and `!fsvstatus` answered.
One human mover, counters read from process memory by `server/steps.py`. The log
is `steps-live-9464c3c4-20260927.tsv`.

| | 1000 fps | 400 fps | 360 fps | 250 fps |
|---|---|---|---|---|
| Delivered fps / moving time | ~972 / 57.6 s | ~388 / 58.6 s | ~357 / 47.0 s | 250 / 40.0 s |
| Listeners were sent | 1.945/s | 1.946/s | 1.956/s | 2.051/s (stock, nothing added) |
| Mover's own client played | 0 | 1.724/s | 2.105/s | 2.051/s |
| Vanilla stripped from listeners | 0 | 101 of 101 | 99 of 99 | 0 |
| Injected → delivered | 112 → 112 | 114 → 114 | 92 → 92 | none |

Per full second of movement: 95% of seconds on exactly two (sd 0.22) in every
band above 300 fps, and 92% at 250 fps. No second was silent. The session total
was 318 injected and 318 delivered, none left in the mover's ring, 200 vanilla
stripped, all 318 re-tagged.

The 360 fps run came straight after 400 fps, so the replace hysteresis kept it on
the clock (see *Known limits*).

## Live — previous candidate `ccf69b7b`, test server, 2026-09-27 21:36–21:50

Same rig, same mover, log `steps-live-ccf69b7b-20260927.tsv`. The ring
path is unchanged in this build, which only removes the modes and the s5 emitter
and makes the install all-or-nothing.

| | 1000 fps | 392 fps | 250 fps |
|---|---|---|---|
| Moving time | 31.7 s | 26.2 s | 11.8 s |
| Listeners were sent | 1.957/s | 1.947/s | 2.114/s (stock, nothing added) |
| Mover's own client played | 0 | 1.679/s | 2.114/s |
| Vanilla stripped from listeners | 0 | 44 of 44 | 0 |
| Seconds with exactly 2 steps | 97% | 91–93% | 91% |
| Silent seconds | 0 | 0 | 0 |

The session total was 118 footsteps injected and 118 delivered. None was left in
the mover's ring. All 118 were re-tagged. For comparison, the pre-replace band at
360–500 fps was 53% of seconds on two, with sd 0.69.

The 1000 and 400 fps clock is about 8% slower than a real 250 fps walk: 1.95/s
against the 2.114/s measured here. That is a tempo difference, not unevenness,
and it is the open retarget question.

## What changed, from the 2026-09-27 code review

### Version 1.1.1: plugin `8fae5aa1`, since 1.1

- **Why:** to install from a QLSM plugin repository. QLSM (1.43.0 and newer)
  fetches extra files of a plugin only through the manifest's
  `package_files`, and only at least one folder deep, so the library can't
  arrive next to the plugin. Read in QLSM's source (`ui/plugin_repositories.py`,
  `ansible/playbooks/tasks/sync_common_plugins.yml`): files are written byte
  for byte, the pool reaches the hosts by `rsync -a` with no filter, and each
  instance is refreshed from it at restart.
- **The plugin looks in `footstepsVQL_lib/` too.** The candidates are the
  `qlx_footstepsVQLSo` path, then those of `footstepsVQL_hook.so` next to the
  plugin and `footstepsVQL_lib/footstepsVQL_hook.so` that exist, in that order.
  A default that isn't there is not tried, so neither install style adds a
  warning line; with neither present, both are named in the "no hook library
  loaded" line. Not `footstepsVQL/`: a folder named like the module is a
  namespace-package candidate for the same import. `VERSION = "1.1.1"`.
- **`release/`:** the library moved to `release/footstepsVQL_lib/`, next to a
  new `qlsm-repository.json` (written by `scripts/_stage_release.py`). Its
  `sha256` values are of the LF-normalized bytes, as QLSM compares them; for
  the `.so`, which has 33 lone CR bytes and no CRLF, that differs from the
  file's real hash, which `SHA256SUMS.txt` keeps.
- **Gated:** `_plugin_review_check.py` (Python 3.5 syntax, shim, handlers) and
  `_plugin_shim_test.py` on both runtimes, with a new *library lookup* case:
  only the subfolder copy, both, neither, and a cvar path. The `.so` is
  unchanged, so its gates still hold.
- **Installed through QLSM on the test server (2026-10-18),** minqlxtended, from
  the repository URL of a test branch. Port 27960, a fresh install: the pool and
  the instance held the plugin, `footstepsVQL_lib/footstepsVQL_hook.so` and the
  settings sidecar, and the load line read `version 1.1.1: .so c96f49de, .py
  8fae5aa1` with `hook installed` before and after a restart; status 1 read
  from process memory. Port 27961 kept loading its hand-uploaded 1.1 copy until
  that was deleted, then loaded 1.1.1 the same way (status 1). Not run: QLSM on
  minqlx.

### Version 1.1: `c96f49de` + `7ce3da41`, since 1.0

- **Site C goes through a stub that is always in reach.** 1.0 repointed the
  rel32 of ClientThink_real's `call SendPendingPredictableEvents` straight at
  the thunk in the hook library, which only reaches ±2 GB. qagame is mapped
  afresh on every map change, and on 2026-10-07 18:03 it landed 2.26 GB from
  the hook on a production CA server; the re-patch refused with `-5` and the server ran
  stock until a later map change (see *In production*). Now the
  rel32 always targets a 14-byte stub, `jmp [rip+0]` plus the thunk's 64-bit
  address, on its own anonymous page mapped near qagame (`stub_near`: the
  kernel's own choice first, then hints in 16 MB steps up to ±1 GB, every
  result checked for reach). The page is written, then made read+exec only.
  A jump, not a call, so the thunk sees the stack exactly as before. A stub is
  reused while qagame keeps landing in reach of it; pages are never unmapped
  (qagame is dlclosed without notice), at most 8.
- **One path everywhere:** the stub is used even when the thunk would be in
  reach, so the path every server runs is the tested one. Cost: one extra
  indirect jump per command.
- **New status `-7`**: no stub page could be placed within 2 GB of the call.
  Deterministic for that mapping, so the plugin does not retry it; the next map
  change tries again. New export `fsvql_stub_pages()`.
- **Plugin `7ce3da41`:** `VERSION = "1.1"`, the `-7` text, and `-7` among the
  statuses it does not retry.
- **Gated (test server, 2026-10-07):** `release_build.sh` exit 0; 56 cadence
  cells pass; signatures unique; ABI complete including `fsvql_stub_pages`;
  glibc needs unchanged (2.2.5, 2.4). `selftest_emit.py` 187 checks, 0
  failures (165 before): the call lands on an anonymous r-x stub that jumps to
  the thunk, the stub is reused by a second patch and by the reinstall, and
  the seccomp unpatch still fails closed through it. A new child run
  (`selftest_emit.py --far`) reserves 3 GB below the hook and then loads
  qagame, 3.01 GB away: the candidate installs through a stub 16 MB from the
  call, unpatch restores `e8 92 f8 ff ff`, and a re-patch reuses the stub.
  **Control: 1.0 (`b6da4845`) in the same run returns `-5`**, the production
  failure (`server/far_control.sh`). `_plugin_review_check.py` and the shim
  test pass. `verify_patch.py` follows the stub.
- **Live on the test server since 2026-10-07 22:22:55** (uploaded by hand;
  the new `.so` ran with the 1.0 plugin for 46 s before the new `.py` arrived,
  and installed both times). `version 1.1: .so c96f49de, .py 7ce3da41`, hook
  installed, 0 errors. `verify_patch.py` on the live process: site A jumps into
  the 1.1 library; site C's call lands on an anonymous stub, which jumps to the
  thunk. Three map loads over rcon (`server/map_cycle_verify.sh`) each moved
  qagame 400 MB lower and re-patched live through the same stub, with no
  failure lines. Not covered live: a mapping beyond 2 GB (the offline `--far`
  run covers it).
- **Played live on 1.1, 2026-10-07 22:54–22:59** (the author at ~1000 fps, 4
  paused listener bots, rig adding 5 ms; log
  `botsnap-session-c96f49de-20261007-2244.tsv`, split with
  `session_split.py`): flat 2.08/s while running, 92% of seconds on exactly
  2, gaps 450/475/500 ms, 0 holes; stairs 3.12/s, 0 near-doubles; pads with
  and without LG, 0 near-doubles; under LG fire the known stock-like losses.
  Injected = delivered in every phase (259 in all), which also proves the
  thunk runs through the stub. The same as 1.0 within run-to-run spread. Not
  run: the mover check with `ql_sndtap`.
- **Rolled out** to four production servers from 2026-10-07 to 2026-10-11
  (see *In production*).

### Version 1.0: plugin `351f501f`, since `5597190d`

- **The plugin names its version and build.** `VERSION = "1.0"` in
  `footstepsVQL.py`. `!fsvstatus` starts with
  `version 1.0: .so <sha256[:8]>, .py <sha256[:8]>`, and the plugin logs the
  same line on every load, whatever `qlx_footstepsVQLVerbose` says. The `.so`
  hash is taken from the copy actually loaded, the `.py` hash from the plugin
  file. Both match the artifact rows above. With no library loaded, it says
  `.so not loaded`.
- **Gated:** `_plugin_review_check.py`, the 3.5 syntax parse, and
  `_plugin_shim_test.py`, which now checks the version line in the load log
  and in `!fsvstatus` on both runtimes, with and without a library. It fails
  on `5597190d` ("no VERSION constant"). The `.so` is unchanged, so the VPS
  gates still hold.

### `b6da4845`, since `5a199d8d`

- **50 ms hold after a jump, jump pad or landing event**
  (`FS_EVENT_GUARD_MS`). Covers `EV_JUMP` (10), `EV_JUMP_PAD` (9) and
  `EV_FALL_SHORT/MEDIUM/FAR` (6/7/8), QL's event numbers, measured during development. An
  injection that drains into `ent->s.event` within the same snapshot interval
  after one of these replaces it, and listeners never hear it. Measured on
  `5a199d8d` (run F in *Known limits*): 1 of 73 falls and jump pads was lost
  with a clock injection in the same ~2 ms.
- **Two scans:**
  - The hook scans the ring at site A, as before. That catches what Pmove
    added before `PM_Footsteps`: landings and jumps.
  - It now also scans at the end of each command in `fsvql_ring_undo`. That
    catches what came after Pmove: `EV_JUMP_PAD` from `G_TouchTriggers`.
- **Why 50 ms and not the 200 ms used after landing footsteps:** a bunnyhop's
  ground contact is often under 100 ms, and the clock must still fit in it.
- **Deliberately not covered:**
  - **Shots:** LG fires every 50 ms, so the clock would never fit between
    them.
  - **Pain:** an `externalEvent`, which takes priority on the entity slot,
    and `ring_room` already sends an injection by the overflow path then.
- **Gated:**
  - **`test_cadence.c`:** 6 new cells (*landings as events*: 60/120/400 ms on
    the ground, 300–700 ms in the air, at 400 and 1000 fps). With the guard,
    0 injections within 50 ms of an event; without it, 34–56 per 300 s. The
    injected rate is identical with and without (0.22–0.93/s), so the guard
    only shifts footsteps, never drops them. All 56 cells pass.
  - **`selftest_emit.py`:** a new check against the real `.so`. A jump pad
    added after Pmove, a few ms before the clock is due, holds the next clock
    footstep exactly 50 ms. 165 checks, 0 failures. Signatures unique.
- **Re-tested live (2026-10-04 18:20–19:15)**, uploaded through the QLSM UI:
  - **Stairs at 1000 fps:** 153 footsteps heard by a bot. 0 holes or
    near-doubles in running stretches. The one gap over 650 ms that ended
    while running contained 287 ms of air. Clock spacing p5 355 ms, median
    474 ms, 0 under 300 ms.
  - **Pad drops:** without fire, 60 of 61 events reached listeners (the one
    loss is stock). With constant LG, 99 of 124 did. None of the losses had
    an injection within ±25 ms (runs G, H in *Known limits*).

### `5a199d8d`, since `d6d28bed`

- **Injected footsteps at least 350 ms apart** (`FS_MIN_INJECT_GAP_MS`). Two
  injections with no real event between them reach listeners with the same
  entityState value, and cgame plays an entity event only when it changes. The
  server clears the slot `EVENT_VALID_MSEC` (300 ms) after it was set, on a
  frame, and a snapshot must show it cleared before the next one. Below that,
  listeners silently drop the second footstep.
- **Found live.** A second human listener (2026-10-04 16:38–16:45) heard
  "a couple of steps missing" at the top of the stairs. Bot snapshots showed
  750–775 ms gaps exactly at the stairs-to-flat transition at 1000 fps, while
  stock 250 fps had none there. `server/slottrace.py` read the player's clock
  state every ~2 ms and showed the sequence:
  1. a footstep owed on the stairs, held by the 200 ms post-landing guard, went
     out 200 ms after the last landing (injection #2468);
  2. the next regular one came 293 ms later (#2469), at the same event
     sequence 38, with no real event between;
  3. bots never played #2469.

  The 200 ms gap to any footstep, and the 476 ms rhythm, are unchanged.
  Cancelled injections (silent command, deferral) restore the new timestamp
  too.
- **The trace also confirmed:** QL's `PM_CrashLand` resets `bobCycle` to 0 on
  landing, as in Q3.
- **Gated.** `test_cadence.c` has 12 new *stairs then flat* cells: 6 stair
  steps, then 3 s of running, at 400/500/1000 fps, with stock and replace
  modes and 120/200 ms ground stretches. Every cell also checks that no two
  injections fall inside 350 ms (`LISTENER_PAIR_MS`, fixed in the test).
  Built with the rule off (`-DFS_MIN_INJECT_GAP_MS=0`), 10 of the 12 new
  cells fail, with 4–174 such pairs in 300 s. With it, all 50 cells pass, the
  injected rate is unchanged (1.65–1.81/s in those cells), and every earlier
  cell reads the same. `selftest_emit.py`: 163 checks, 0 failures. Signatures
  unique. `perf_bench`: 30–46 ns per command for the hook body (a noisier
  host, with a second QL server busy), against 28–33 ns on `d6d28bed`.
- **Re-tested live (2026-10-04 17:26–17:29).** It was uploaded
  through the QLSM UI, and the watchdog re-armed the rig after the restart.
  Then 1000 fps passes up the stairs, running on at the top:
  - `trace_gaps.py`: 0 gaps over 650 ms in 137 footsteps heard by a bot. On
    `d6d28bed` the same transition had a 775 ms hole.
  - `inj_spacing.py`: 58 injections; consecutive spacing min 346 ms (the
    tracer samples every ~2 ms), p5 351 ms, median 475 ms, 0 under 300 ms.
  - `humans_report.py`: 2.28 footsteps per running second, 0 near-doubles,
    0 holes, on the clock 100%.

### `d6d28bed`, since `bd6c781b`

Measured live afterwards: *Live — `d6d28bed`*.

- **Air time is no longer credited to the clock.** The bob site only runs on
  grounded, moving commands, so its `dt` used to span any air time before it.
  Gaps up to 250 ms were credited to the reference cycle, while vanilla
  `bobCycle` does not move in the air. Measured on `bd6c781b` with a stock
  250 fps client on stairs: 18 injections in 30 s, and four 25 ms doubles next
  to `PM_CrashLand` landing footsteps. Every second that had an injection
  also had command gaps over 32 ms. Now `fsvql_ring_pre`, which runs on every
  `ClientThink_real`, records the command time (`fs_note_command`), so `dt` is
  the command's own length.
- **A landing footstep counts toward the minimum gap.** `PM_CrashLand` runs
  before `PM_Footsteps`, so a footstep id in the ring at site A, added since
  the last undo, is a landing. The clock keeps `FS_MIN_STEP_GAP_MS` (200 ms)
  off it (`fs_note_footstep`).
- **Gated.** `test_cadence.c` has 16 new stairs cells (120 and 400 ms on the
  ground, drops of 40–160 ms, a landing each). Stock 250/333 get 0 injections,
  against 0.85/s on the old hook. At 1000 fps with 400 ms stretches, the clock
  runs at 1.683/s, its rate times the ground fraction. `selftest_emit.py` has a
  stairs section: 0 written over 30 s of 250 fps stairs, and the clock
  footstep after a landing comes 200 ms later. Every earlier cell is unchanged.
- **Known gap.** A landing on a command that never reaches the bob site (no
  movement keys held) is not seen. It costs at most one clock footstep closer
  than 200 ms to that landing.

### `bd6c781b`, since `c62aba41`

- **A stripped vanilla footstep no longer erases the clock footstep under it.**
  At 333–400 fps the mover's vanilla flips come about every 385 ms. When one
  drains into `ent->s.event` within the same snapshot interval as a clock
  footstep, the engine overwrites the clock footstep, and the strip then left
  the slot at 0, so neither reached listeners. Measured on `c62aba41` at
  333–400 fps, flat, firing: 1.778/s played against 2.17/s injected, 15% of gaps
  over 700 ms. Now the ring hook records `ent->s.event` at the end of every
  command, dated by when that value appeared, and the strip puts it back if it
  appeared within `STRIP_RESTORE_MS` (50 ms, two snapshots at `sv_fps 40`).
  An older value is not put back, so a listener coming into view cannot be sent
  a stale event; a recent one cannot double, because cgame plays an entity's
  event only when its value differs from the last one it saw. Counted by
  `fsvql_ring_suppressed_restored()`.

### `c62aba41`, since `e4573c6e`

- **Vanilla footsteps no longer leak through the overflow path.** In replace
  mode the listener hears only the clock, so each vanilla bob footstep is
  stripped from what listeners receive. It was stripped only from
  `ent->s.event`, and only on the command it was made. When another event was
  ahead of it in the ring, typically a shot, the entity slot took that event and
  the footstep left as the overflow temp entity, or waited in the ring for a
  later command; either way it reached listeners on top of the clock. Measured
  on `e4573c6e` at 400 fps, flat, while firing: 2.467/s played, 33% of seconds
  on exactly two, 17% of gaps under 150 ms, and 37 strips in 45 s where roughly
  60 were due. 1000 fps was unaffected: it makes no vanilla footsteps. Now the
  footstep is tracked by its ring sequence until a listener channel drains it.
  Just before `SendPendingPredictableEvents`, a new call in the site C thunk
  (`fsvql_ring_pre`) steps `entityEventSequence` past it if the overflow is
  about to ship it, so the overflow ships the next event or none. Only the
  listeners' drain pointer moves: `events[]` and `eventSequence`, which the
  mover predicts against, are untouched, and `entityEventSequence` is never sent
  to a client. If it drains into the entity slot instead, it is cleared by
  matching its exact sequence tag, which also means a `PM_CrashLand` footstep in
  that slot can no longer be stripped by mistake. Counted by
  `fsvql_ring_suppressed_overflow()`, part of `fsvql_ring_suppressed()`. Still
  not covered: a footstep left in a backed-up ring until `ClientEndFrame` drains
  it outside `ClientThink_real`.

### `e4573c6e`, since `d7ea25e4`

- **The listener clock runs at a real 250 fps walk's tempo.** The old reference,
  1.953/s, is what a client with perfectly even 4 ms commands produces. Real 250
  fps clients run faster, because truncation of uneven commands works in their
  favour: measured at 2.05-2.15/s on flat ground and 2.37/s on stairs, where
  `PM_CrashLand` adds step footsteps. In replace mode, above ~355 fps, other
  players hear only the clock, so they heard a fast player about 8% slower than
  a 250 fps one. That was the open retarget question. The clock in replace mode
  now advances at 269 units/s (`FS_CLOCK_UNITS_PER_SEC`), 2.102 footsteps/s, one
  per 476 ms. The deficit filler below the replace line keeps 250 units/s, so a
  stock 250 or 333 fps player is still never injected into: at the same rate
  bobCycle and the clock stay phase-locked. Gated: the host test asserts 2.100/s
  at 400-1000 fps and still 0 injections at 250 and 333; `fsvql_selftest()`
  asserts the clock within 0.05/s at 1 and 2 ms and at alternating 2/3 ms.

### `d7ea25e4`, since `10b40ab6`

- **No footsteps where vanilla plays none.** `PM_Footsteps` reaches the
  `bobCycle` store from four branches: running, walk key, crouched, and a QL
  slow-velocity band. Only the running one sets its `footstep` flag (`r13d`).
  After the store, vanilla plays a footstep only if that flag is set,
  `pm->noFootsteps` is clear and `pm->waterlevel` is 0 (the gate at `0x58910`).
  The injector checked none of it, so at high fps, where walk and crouch
  commands truncate to 0 bob units, the clock owed and sent a footstep every
  512 ms for a sneaking player. Measured on `10b40ab6` at ~990–1000 fps through
  bot snapshots: 1.875/s with the walk key, 1.812/s crouched, against 0 in
  stock QL and 0 from bots doing the same. A server with no-footsteps
  `dmflags`, or a player in water, would have been injected too. Now the
  trampoline passes `r13d` and `pm` (`r12`), and on a command where vanilla is
  silent the hook injects nothing, strips nothing (a footstep there can only be
  `PM_CrashLand`'s), and clears the owed debt. Running again, the clock picks up
  within one reference interval. Those commands are counted as
  `fsvql_quiet()`, shown by `!fsvstatus`. Before installing, the hook checks the
  bytes that make `r12` and `r13d` mean that: `mov rax,[r12]` just before the
  store, `mov edi,[r12+0xd4]; test; je` after it, and at that `je`'s target
  `test r13d,r13d; je; mov esi,[r12+0x2c]`. Any difference refuses with the new
  status `-6`, which the plugin reports once and does not retry.

### `10b40ab6`, since `e10ad520`

- **An unpatch that cannot write the ring site back changes nothing.** The ring
  site is the repointed call into our thunk, and the thunk forwards through a
  pointer to the real `SendPendingPredictableEvents`. If `mprotect` refused while
  restoring that call, `e10ad520` and `9464c3c4` still restored the bob site,
  which clears the forwarding pointer while the call still lands in the thunk:
  the next player command would jump to address 0 and take the server down. It
  also reported "vanilla bytes restored". Now `fsvql_unpatch()` returns `-4` and
  leaves both sites and every pointer as they were, still patched and active.
  `!fsvunload` and plugin unload then report "could not make the code page
  writable". Reachable only on `!fsvunload` or plugin unload, and only if
  `mprotect` fails on a page where it already succeeded at install; never seen.

### `e10ad520`, since `9464c3c4`

- **An injected footstep waits a command when the engine has no room for it.**
  The engine ships at most two ring events per command to other players: one
  into the player's public copy (`ent->s.event`), unless `externalEvent` (pain,
  pickups) holds that slot, and one overflow temp entity. Ours is written last.
  When the events already pending fill that, `9464c3c4` wrote ours anyway, and
  the undo then took it back out before anyone received it. Live in CA with
  bots, that was 10 of 1168 (0.9%), shown by `!fsvstatus` as "reached nobody".
  With two already pending, the write could also overwrite the older one in the
  two-slot ring. Now the step goes back to the cadence (debt and last-step time
  restored) and goes out on the next command with room, 1 ms later at 1000 fps.
  The new counter `fsvql_ring_deferred()` is shown by `!fsvstatus` as "waited a
  command".

### `9464c3c4`, since `ccf69b7b`

- **Modes removed.** The s5 temp-entity emitter (a trampoline on the entry of
  `SendPendingPredictableEvents`, calling `G_TempEntity`) and measure mode are
  gone from the C and from the plugin, and so are `fsvql_set_mode`,
  `fsvql_set_emit` and their state probes.
- **All-or-nothing install.** If the ring site cannot be armed, the bob hook
  comes back out and the status is `-5`, "the event-ring site is not in this
  qagame build". Previously site A alone would have counted footsteps and sent
  none while reporting itself patched.
- **Commands renamed** to `!fsvstatus [reset]`, `!fsvload` and `!fsvunload`, as
  footsteps250 has them. `!fsvstatus reset` is new.

### Earlier in the review, carried in

| Found | Fixed |
|---|---|
| The stop constant was `getattr(engine, "RET_STOP_ALL", 3)`, a plain `3` on minqlxtended. There `Return` is a plain `Enum`, so every command reply was followed by the command passing through plus an "unknown return value" warning | the footsteps250 shim: `engine.Return.STOP_ALL` / `engine.RET_STOP_ALL` |
| A `new_game` task still pending when the plugin unloaded re-patched qagame afterwards (reproduced offline) | `_alive` guard on every deferred path |
| `except Exception` around every native call | narrowed to `AttributeError`, a missing export |
| Deterministic refusals were retried for 7.6 s | reported once |
| After the ring rollback, the next real event drains with the same `(seq & 3) << 8` tag the injected footstep carried. cgame compares the full value against `previousEvent`; this was measured in `cgamex86.dll`, `cmp eax,[ebx+0x1e4]; je` at `1004c1cf`. A CrashLand or vanilla footstep within 300 ms was therefore silently not played for listeners | the injected footstep's tag is moved two on, which differs from both neighbours. Live: 118 of 118 re-tagged |
| The stripped-vanilla count was neither exported nor reset | `fsvql_ring_suppressed()`, `fsvql_ring_retagged()` |
| `undo_pending` survived slot reuse | cleared in `slot_for` |
| The install-time self-test never ran the replace path | it asserts bobCycle identity, the listener rate, no engagement at ≤333 fps, and full stripping at 400 fps |
| `selftest_emit.py` called the two-argument `fsvql_ring_undo` with one argument | passes a synthetic gentity; tests the re-tag, a foreign gentity, and the 400 fps strip |

## Gates run, 2026-10-03, on these bytes

Rerun for `d6d28bed` on the test server with `server/stair_gate.sh`, which wraps
`release_build.sh`. All pass, and every earlier row below holds. The host
self-test now has 38 cells, the 16 new ones being stairs (see *What changed*).
`selftest_emit.py` has 163 checks with 0 failures; the 7 new ones are the stairs
section. Signatures are still all unique in `091d568f…`. The rows below are the
`bd6c781b` run.

| Gate | Where | Result |
|---|---|---|
| `scripts/_plugin_review_check.py` | dev box | pass |
| `scripts/_plugin_shim_test.py` | dev box | pass on both runtimes. It covers load, `new_game`, `!fsvstatus` with its cooldown and `reset`, `!fsvunload`/`!fsvload`, a `-5` refusal that is not retried, and the unload race. Since version 1.0 (2026-10-04, `351f501f`), also the version line in the load log and in `!fsvstatus` |
| `ast.parse(feature_version=(3, 5))` | dev box | parses |
| `server/release_build.sh` → `probe_sigs.sh` | test server | qagame `091d568f46a2…`, 7 of 7 signatures unique |
| → `build.sh` host self-test | test server | 22 of 22 cells pass. Stock path unchanged (250 fps 1.950/s and 333 fps 2.600/s, no injection); replace mode 2.100/s at 400, 450, 500 and 1000 fps |
| → `validate.sh` | test server | ABI complete, including `fsvql_ring_deferred`, with no stale emitter or mode exports. `fsvql_selftest() = 0`. `selftest_emit.py`: both sites in one call, second call a no-op, ring arithmetic and listener paths, the deferral for pain plus one queued event and for two queued events, no injection and no debt over 1400 1 ms commands with the footstep flag 0, with waterlevel 1 and with noFootsteps set, then one footstep within 512 ms of running again, the vanilla footstep queued behind a shot at 400 fps (overflow ships nothing on that command, the shot stays in the entity slot, the mover's ring keeps the footstep, one skip counted, a CrashLand footstep in the entity slot left alone), the strip putting back a 3 ms old clock footstep and not a 60 ms old event, unpatch with `mprotect` denied by seccomp in a forked child (returns `-4`, both sites and both pointers kept), unpatch restores both sites and drops every pointer, reinstall. Install passed the footstep-gate check against the real qagame. 156 checks, 0 failures |

Nothing above ran patched code inside a game. `selftest_emit.py` patches only its
own `dlopen` of `qagamex64.so` and reads the bytes back.

## Not validated, and known limits

- **minqlx, live, is an earlier build only.** The production LG duel server
  ran `9464c3c4` / `9b0d042caf48…` on minqlx from 2026-09-28 (qagame
  `091d568f…`). Read-only check 2026-10-03: six daily restarts, `hook
  installed` on every load, no error. 1.0 and 1.1 have run live on
  minqlxtended; on minqlx they are covered by the plugin shim test.
- **The deferral was proven offline first, and is now seen working in
  production** (see *In production*). On `9464c3c4`, CA with bots
  lost 10 of 1168 injected footsteps (0.9%) to a full command; the mover was
  unaffected, with 1168 undone and 0 skipped or missed. This build holds such a
  footstep a command instead; `selftest_emit.py` covers pain plus one queued
  event, and two queued events. Confirm with one CA session: "reached nobody"
  should stay at 0 while "waited a command" counts up. **2026-10-03 on
  `d6d28bed`:** a short CA match (5 v 5 with bots, 250/400/990 fps) gave 121
  injected, 121 delivered, 0 deferred. That proves no loss, but not the
  deferral: at `9464c3c4`'s rate (0.9%), 121 injections would give about one.
  About 10 minutes of CA at 990 fps (about 1000 injections) would settle it.
  In production since then: 97, 596 and 57 deferred on three server-days,
  with every injected footstep delivered.
  Without the deferral working, the cost is an occasional missing footstep in
  heavy combat, never a double. The cause itself is
  inferred from the two-events-per-command rule, not traced per event; the new
  counter is what measures it.
- **In practice the clock takes over from about 355 fps.** The switch compares a
  short running average of command intervals (EMA, 1/16 per command, about 45 ms
  at these rates) against 2.58 ms to turn on, and against 3.125 ms (~320 fps) to
  turn off. Standing still does not reset it. The 2.58 ms line nominally means
  ~387 fps, but a burst of 1–2 ms commands crosses it at lower framerates. Measured
  on `9464c3c4` at 22:33, arriving at 360 from 250: stock for 5.3 s of movement
  (11 vanilla steps reached listeners), then latched onto the clock for the rest
  of the run (107 of 108 vanilla stripped, 99 injected). Per-second means were
  2.76–2.84 ms the whole time. Coming down from 400 it is on the clock from the
  start. Either way listeners get a steady stream (95% of seconds on exactly
  two); the cost is tempo, 1.95/s against the player's own ~2.09/s at 357 fps.
  The band itself stays, deliberately: modelled on this session's measured
  interval mix, a single cut at 2.58 ms would switch 45–49 times per second for a
  ~388 fps player, against once a minute with the band. Log:
  `steps-live-9464c3c4-below-20260927.tsv`.
- **The switch onto the clock, arriving from below, costs one gap.** Measured on
  `d6d28bed`, 360 fps arriving from 250: stock for 22 s at about 2/s, then the
  clock took over (second 1009). In that second two vanilla footsteps were
  already stripped while the clock had not fired yet, so there was one gap over
  700 ms. The run averaged 1.897/s. Once per switch, never on the way down
  (hysteresis). A possible fix, not made: schedule the clock's first step
  relative to the last vanilla step when it takes over.
- **A clock footstep just before a landing.** The 200 ms guard only covers the
  time after a `PM_CrashLand` footstep. The clock cannot know a landing is about
  to come, so at high fps on stairs a clock footstep can land one snapshot
  before it. Measured once in 30 s of 990 fps stairs (one 25 ms pair in 87
  footsteps). A landing on a command that never reaches the bob site (no
  movement keys) is not seen at all.
- **The all-or-nothing refusal (`-5`)** is covered by the shim test, not by a real
  qagame without the ring site.
- **An injection stripped with the vanilla flip that follows it.** In the
  390–500 fps band, a stripped flip that lands in the same snapshot interval as
  an injection (≤25 ms at `sv_fps 40`) zeroes the injection before listeners
  receive it. Estimated at vanilla rate × 25 ms, about 4% at 392 fps and 0 at
  500+. Server counters cannot see it.
- **A vanilla footstep can leak through the overflow temp entity.** If the same
  command also carried a jump, shot, CrashLand step or `externalEvent`, listeners
  hear one vanilla footstep on top of the clock. Rare, not measured.
- **Two injections under 300 ms apart with no real event between them** share one
  value, so the second does not play. **Fixed in `5a199d8d`** (350 ms minimum
  between injections). On `d6d28bed` it was not rare: it happened at the top
  of stairs at 1000 fps, measured 2026-10-04.
- **Stock QL drops some fall and jump-pad sounds for listeners under
  sustained LG fire. footstepsVQL keeps the rate the same, and rarely adds one
  by overwriting.** Measured on
  2026-10-04: repeated jump pad into a drop, with and without LG fire, four
  listener bots, with `server/slottrace.py` as the record of every event the
  server produced, and `server/fall_check.py` comparing it with the bots.

  | Run | fps | footstepsVQL | LG shots | FALL_SHORT lost | FALL_MEDIUM lost | JUMP_PAD lost | Total lost |
  |---|---|---|---|---|---|---|---|
  | A | 1000 | loaded (clock on) | 577 | 0/8 | 2/14 | 1/16 | 3/38 (8%) |
  | B | 1000 | unloaded | 1561 | 7/24 | 7/17 | 11/30 | 25/71 (35%) |
  | C | 1000 | unloaded | 0 | 0/18 | 0/12 | 0/23 | 0/53 |
  | D | 250 | stock path | 0 | 0/21 | 0/17 | 0/28 | 0/66 |
  | E | 250 | stock path | 1004 | 8/14 | 0/12 | 0/20 | 8/46 (17%) |
  | F | 1000 | `5a199d8d`, constant fire to match B | 1529 | 8/21 | 9/21 | 12/31 | 29/73 (40%) |
  | G | 1000 | `b6da4845` (event guard), no fire | 0 | 1/24 | 0/13 | 0/24 | 1/61 |
  | H | 1000 | `b6da4845` (event guard), constant fire | 2675 | 5/30 | 9/38 | 11/56 | 25/124 (20%) |

  - **No fire, no losses:** without LG fire nothing is lost at either fps
    (C, D). With it, stock loses some at both 250 and 1000 fps (B, E).
  - **The same rate with footstepsVQL:** with comparable fire (B: 22.0 shots
    per fall or pad; F: 20.9), stock lost 35% and footstepsVQL 40%. That is
    the same within noise (about ±8 points at ~70 events each). Run A's 8%
    came from sparser fire, not from the plugin.
  - **The 28 stock-like losses** in F had no other event within one snapshot,
    and three of the four bots had the player in full view at every loss. The
    mechanism inside stock delivery is not investigated.
  - **One loss in F (1 in 73 events) may be ours:** a JUMP_PAD with a clock
    injection in the same ~2 ms trace sample. If the pad event drained into
    `ent->s.event` on one command and the clock footstep on the next, inside
    one snapshot, listeners got the footstep instead.
  - **Fixed in `b6da4845`:** a 50 ms hold after `JUMP_PAD`, `FALL_*` and
    `JUMP` (see *What changed*). Re-tested in G and H: 0 of 26 losses had an
    injection within ±25 ms, across 100 injections. The stock losses under LG
    fire remain (H: 20% at 21.6 shots per event; read as run-to-run spread,
    not an improvement), and footstepsVQL can't fix them.
  - **G's single loss is stock:** a landing and a jump in the same command
    (event sequence +2 in one step: FALL_SHORT, JUMP). Only the JUMP reached
    listeners. No injection; the clock was owed 1 and held by the guard.
- **Footsteps are lost for listeners under sustained LG fire, in stock QL and
  on the clock alike.** The entity carries one event per snapshot, the last
  one drained in the server frame (25 ms at `sv_fps 40`). LG fires every
  ~50 ms, so a shot later in the same frame as a footstep replaces it.
  footstepsVQL doesn't fix this; we decided on 2026-10-04 to leave
  it as is. Measured on `b6da4845`, 2026-10-04 21:48–21:53: flat running on
  thunderstruck, firing LG at a wall, four paused listener bots, 0 ms added
  delay. `server/slottrace.py` recorded every footstep the server produced,
  and `server/fs_loss.py` checked each one against the bots.

  | Runs | fps | Footsteps | Shots | Lost | Shot later in the same frame | Nothing else in the frame |
  |---|---|---|---|---|---|---|
  | 3 × 30 s, firing | 250 (stock path) | 167 | 1714 | 36 (22%) | 36 | 0 |
  | 2 × 30 s, firing | 500 (clock) | 125 | 1142 | 40 (32%) | 34 | 6 |
  | 30 s, no fire | 250 | 60 | 0 | 0 | — | — |
  | 30 s, no fire | 500 | 64 | 0 | 0 | — | — |

  - **Stock loses them too:** every stock loss at 250 fps had a shot 0–17 ms
    after the footstep in the same frame. Bots heard 1.24–1.62 footsteps/s
    while firing against 2.03/s without.
  - **The clock loses somewhat more:** 1.28–1.41/s while firing against
    2.07/s without. Counting only shot overwrites, 27% against 22%, within
    noise at these counts.
  - **6 clock footsteps were lost with nothing else in their frame.** The
    cause is unknown; the trace doesn't show it.
  - **Seen live first** in a second human's session (~490 fps, 21:30–21:36): ground-running gaps of 950–975 ms, one clock step
    missing, in 12.8% of gaps with LG fire against 2.6% without. All four bots
    missed the same footsteps.
  - **A possible fix, not built:** when the predicted `ps.weaponTime` says a
    shot lands later in the same frame, send the clock footstep through the
    overflow temp entity, which the engine never overwrites. It would help
    only players on the clock.
- **Spectators following a player** hear what that player hears, not the clock.
- **Other hosts.** Hashing `qagamex64.so` on a new box before loading is cheap
  insurance, even though the hook fails safe by signature. `verify_patch.py` and
  `selftest_emit.py` carry offsets that only hold for `091d568f…`.
