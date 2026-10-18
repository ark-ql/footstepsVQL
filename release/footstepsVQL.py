"""
footstepsVQL.py - audible footsteps at high client FPS, without the doubled sound.

A replacement for footsteps250: take footsteps250 out of qlx_plugins, put
footstepsVQL in, and restart or change map. Only one of the two can hold the
bobCycle site. If footsteps250's trampoline is still there - it leaves it in
place when it is unloaded at runtime - this plugin refuses rather than fight
for the site, says so, and takes over on the next map.

Runs unmodified on both minqlx and minqlxtended. The runtime is detected at
import and aliased to `engine`. Only five engine names are used - Plugin,
console_print, delay, next_frame and the stop-dispatch constant - and only the
constant is spelled differently between the forks. Syntax is kept to Python 3.5
so the same file loads in the Ubuntu 16 minqlx containers.
scripts/_plugin_review_check.py and scripts/_plugin_shim_test.py enforce all of
that.

What it does
------------
footsteps250 quantizes the networked playerState.bobCycle so a fast client's
walk cycle advances at the 250 fps rate. That makes the server's bobCycle
disagree with what the client's own pmove computes, and the client can only
de-duplicate a predicted event against the server's while the two agree. An
unpredicted footstep then enters the two-slot event ring and replays the jump or
shot sitting in the other slot: the doubled sound.

footstepsVQL leaves bobCycle vanilla and keeps its cadence in private server
memory. Each footstep a fast client's own pmove did not produce is written into
his event ring for other players' snapshots and taken back out before his own
snapshot is built. Other players hear a steady clock from anyone above ~355 fps,
one footstep per 476 ms (2.10/s), the tempo a real 250 fps client runs at; the
mover hears only what his own client predicts; nobody gets a replay or a
double. Walking with the walk key, crouching, wading and no-footsteps servers
stay silent, as in stock. 333 fps and slower is left exactly as stock.

Cvars
-----
  qlx_footstepsVQLEnabled  "1" re-applies the hook on every new map, "0" leaves
                           qagame vanilla. !fsvload / !fsvunload write it.
  qlx_footstepsVQLSo       absolute path to footstepsVQL_hook.so. Default: next
                           to this file, else in footstepsVQL_lib/ next to it
                           (where a QLSM repository install puts it).
  qlx_footstepsVQLVerbose  "1" also logs the routine per-map lines. Default "0":
                           silent while everything works. Load-time state, every
                           failure, every retry and every recheck are logged
                           either way.

Commands
--------
  !fsvstatus         (1) the version and build (the first 8 hex digits of each
                     file's sha256, as in RELEASE.md), what is loaded, what is
                     patched, what happens on next map, and the counters.
                     Rate-limited server-wide.
                     Alias !footstepsvql.
  !fsvstatus reset   (1) report, then zero the counters for a clean measurement.
  !fsvload           (2) apply the hook now and re-apply it on future maps.
  !fsvunload         (2) restore the vanilla bytes and stop re-applying.

  Every command answers with the state before it ran, what it did, and the
  state after.

The library keeps the _hook suffix and must NOT be named footstepsVQL.so. Both
engines import a plugin as <plugins-dir>.<name>, and Python tries extension
modules before .py, so that name would shadow this file and the plugin would
never load.

Patching happens on the game thread only - a next_frame task, a delay task or a
command handler - because it rewrites instructions the game thread may be about
to execute. The library is loaded through a copy in the temp directory named
after its content hash, because dlopen resolves by name against what is already
loaded and ctypes never dlcloses: without that, a !reload after replacing the
.so would keep running the previous build.
"""

import ctypes
import glob
import hashlib
import os
import sys
import tempfile
import time

# One file, both runtimes. Detection is by sys.modules rather than by trying an
# import: the engine imports its own package before any plugin, so sys.modules
# names the runtime that is actually running, where an import probe would be
# fooled by a stray install of the other package. minqlxtended's Return is a
# plain Enum, so minqlx's int 3 is not equal to Return.STOP_ALL there: the reply
# still goes out, but the command then passes through as well, with an
# "unknown return value" warning in the log on every use.
if "minqlxtended" in sys.modules:
    import minqlxtended as engine
    RET_STOP_ALL = engine.Return.STOP_ALL
elif "minqlx" in sys.modules:
    import minqlx as engine
    RET_STOP_ALL = engine.RET_STOP_ALL
else:
    raise ImportError(
        "footstepsVQL found neither minqlx nor minqlxtended loaded; it has to be "
        "imported by one of those engines, not standalone")

VERSION = "1.1.1"

# QLSM ships a plugin's extra files only in a subfolder (package_files), so the
# library may sit one level down. Not "footstepsVQL/": a folder named like the
# module is a namespace-package candidate for the same import.
LIB_NAME = "footstepsVQL_hook.so"
LIB_DIR = "footstepsVQL_lib"

# Mirrors the FSVQL_* constants in footstepsVQL_hook.c.
STATUS_TEXT = {
    1: "^2patched and active^7",
    0: "^3not patched^7 - vanilla bytes are in place",
    -1: "^1refused^7 - cadence selftest failed",
    -2: "^1not patched^7 - bobCycle site not found (is footsteps250 holding it?)",
    -3: "^1refused^7 - byte pattern matched more than once",
    -4: "^1not patched^7 - could not make the code page writable",
    -5: "^1not patched^7 - the event-ring site is not in this qagame build",
    -6: "^1not patched^7 - PM_Footsteps' footstep gate is not where this build expects it",
    -7: "^1not patched^7 - no memory for the event-ring stub within 2 GB of this qagame mapping",
}

UNPATCH_TEXT = {
    1: "^2vanilla bytes restored^7",
    0: "^3nothing to restore^7 - the hook was not installed",
    -4: "^1failed^7 - could not make the code page writable",
}

NO_PATTERN_HINT = (
    "[footstepsVQL] {}: bobCycle site not found. footsteps250's trampoline is "
    "probably still on it - it stays until the next map after footsteps250 is "
    "unloaded. Remove footsteps250 from qlx_plugins; footstepsVQL takes over on "
        "the next map\n")


def _sha8(path):
    try:
        with open(path, "rb") as fh:
            return hashlib.sha256(fh.read()).hexdigest()[:8]
    except (OSError, TypeError):
        return "?"


DT_LABELS = (
    ("no-prev", "first command"),
    ("1ms", "~1000fps"),
    ("2ms", "~500fps"),
    ("3ms", "~333fps"),
    ("4ms", "250fps"),
    ("5-8ms", "125-200fps"),
    ("9-32ms", "31-111fps"),
    (">32ms", "gaps or bots"),
)

RETRY_S = (0.1, 0.5, 2.0, 5.0)
VERIFY_S = 5.0
# !fsvstatus is permission 1 and every call walks the loader's link map, so the
# limit is server-wide: the cost is on the server whoever asks.
STATUS_COOLDOWN_S = 5.0

INT_EXPORTS = ("fsvql_patch", "fsvql_status", "fsvql_unpatch", "fsvql_selftest")
U64_EXPORTS = ("fsvql_hits", "fsvql_vanilla_flips", "fsvql_injections_due",
               "fsvql_move_ms", "fsvql_qagame_base", "fsvql_site", "fsvql_site_c",
               "fsvql_ring_written", "fsvql_ring_drained", "fsvql_ring_undone",
               "fsvql_ring_skipped", "fsvql_ring_missed",
               "fsvql_ring_suppressed", "fsvql_ring_retagged", "fsvql_ring_deferred",
               "fsvql_quiet")


class footstepsVQL(engine.Plugin):
    def __init__(self):
        super().__init__()

        self.set_cvar_once("qlx_footstepsVQLSo", "")
        self.set_cvar_once("qlx_footstepsVQLEnabled", "1")
        self.set_cvar_once("qlx_footstepsVQLVerbose", "0")

        self.lib = None
        self.lib_path = None
        self.lib_loaded = None
        self.py_hash = _sha8(os.path.abspath(__file__))
        self._seq = 0
        self._alive = True
        self._last_status = time.monotonic() - STATUS_COOLDOWN_S

        # Registered before the load attempt on purpose: if the .so is missing
        # or will not load, the commands still have to answer and say so.
        self.add_hook("new_game", self.handle_new_game)
        self.add_hook("unload", self.handle_unload)
        self.add_command(("fsvstatus", "footstepsvql"), self.cmd_status, 1)
        self.add_command("fsvload", self.cmd_load, 2)
        self.add_command("fsvunload", self.cmd_unload, 2)

        tried = []
        for path in self._candidates():
            tried.append(path)
            staged = self._stage(path)
            try:
                self.lib = ctypes.CDLL(staged)
            except OSError:
                continue
            self.lib_path = path
            self.lib_loaded = staged
            break

        self._log("[footstepsVQL] {}\n".format(self._build()))
        if self.lib is None:
            self._log("[footstepsVQL] no hook library loaded; tried: {}\n".format(
                ", ".join(tried)))
            return

        self._bind()
        self.ensure("plugin load")

    # ---------------------------------------------------------------- logging

    def _log(self, text):
        engine.console_print(text)

    def _build(self):
        return "version {}: .so {}, .py {}".format(
            VERSION, _sha8(self.lib_loaded) if self.lib_loaded else "not loaded", self.py_hash)

    def _vlog(self, text):
        try:
            verbose = int(self.get_cvar("qlx_footstepsVQLVerbose") or "0") != 0
        except ValueError:
            verbose = False
        if verbose:
            engine.console_print(text)

    # ---------------------------------------------------------------- library

    def _candidates(self):
        cvar = (self.get_cvar("qlx_footstepsVQLSo") or "").strip()
        here = os.path.dirname(os.path.abspath(__file__))
        defaults = [os.path.join(here, LIB_NAME), os.path.join(here, LIB_DIR, LIB_NAME)]
        present = [p for p in defaults if os.path.isfile(p)]
        return [p for p in [cvar] + (present or defaults) if p]

    def _stage(self, path):
        """Load through a content-tagged copy, so new bytes always get a new
        mapping while identical bytes reuse the existing one. Falls back to the
        original path if the copy cannot be made."""
        try:
            with open(path, "rb") as fh:
                data = fh.read()
        except OSError as e:
            self._log("[footstepsVQL] cannot read {} ({}); trying the next candidate\n".format(
                path, e))
            return path

        tag = hashlib.sha256(data).hexdigest()[:12]
        staged = os.path.join(tempfile.gettempdir(), "footstepsVQL_hook_{}.so".format(tag))
        if os.path.exists(staged):
            self._sweep(staged)
            return staged
        part = "{}.{}.part".format(staged, os.getpid())
        try:
            with open(part, "wb") as fh:
                fh.write(data)
            os.chmod(part, 0o755)
            os.replace(part, staged)
        except OSError as e:
            self._log("[footstepsVQL] could not stage a private copy of {} ({}); loading "
                      "it directly, so a reload will not pick up a new build\n".format(path, e))
            return path
        self._sweep(staged)
        return staged

    def _sweep(self, keep):
        """Best-effort removal of staged copies of builds we no longer run.
        Anything touched recently is left alone rather than raced against a
        sibling instance sitting between its exists() check and its CDLL."""
        cutoff = time.time() - 300
        for old in glob.glob(os.path.join(tempfile.gettempdir(), "footstepsVQL_hook_*.so")):
            if old == keep:
                continue
            try:
                if os.path.getmtime(old) > cutoff:
                    continue
                os.unlink(old)
            except OSError:
                pass

    def _bind(self):
        # Every export is optional here, so an older library degrades to zeros in
        # the status reply instead of taking the plugin down.
        for names, argtypes, restype in ((INT_EXPORTS, [], ctypes.c_int),
                                         (U64_EXPORTS, [], ctypes.c_uint64),
                                         (("fsvql_dt_bucket",), [ctypes.c_int], ctypes.c_uint64),
                                         (("fsvql_reset_counters",), [], None)):
            for name in names:
                try:
                    fn = getattr(self.lib, name)
                except AttributeError:
                    continue
                fn.argtypes = argtypes
                fn.restype = restype

    # ------------------------------------------------------------- primitives

    def _call(self, name, *args):
        """An export's return value as an int, or None if the library lacks it."""
        try:
            fn = getattr(self.lib, name)
        except AttributeError:
            return None
        return int(fn(*args))

    def _u64(self, name):
        value = self._call(name)
        return 0 if value is None else value

    def _status(self):
        value = self._call("fsvql_status")
        return 0 if value is None else value

    def _text(self, status):
        return STATUS_TEXT.get(status, "^1unknown status {}^7".format(status))

    def _enabled(self):
        try:
            return int(self.get_cvar("qlx_footstepsVQLEnabled") or "1") != 0
        except ValueError:
            return True

    def _intent(self, enabled):
        return "^2on^7" if enabled else "^3off^7"

    def _cancelled(self, seq):
        """True if a delayed task should give up: unloaded, superseded, the
        library gone, or auto-reapply switched off while it was waiting."""
        return (not self._alive or seq != self._seq or self.lib is None
                or not self._enabled())

    def _buckets(self):
        out = []
        for i, (interval, fps) in enumerate(DT_LABELS):
            n = self._call("fsvql_dt_bucket", i) or 0
            if n:
                out.append("{} ({}) {}".format(interval, fps, n))
        return out

    # ------------------------------------------------------------------ ensure

    def ensure(self, reason):
        """Install the hook if it is not live. Cheap when it already is."""
        if self.lib is None or not self._alive or not self._enabled():
            return None

        self._seq += 1
        seq = self._seq

        status = self._status()
        if status == 1:
            self._verify(seq, reason)
            return 1
        if status == -1:
            self._log("[footstepsVQL] {}: refused, cadence selftest failed\n".format(reason))
            return status

        status = self._call("fsvql_patch")
        if status == 1:
            line = "[footstepsVQL] {}: hook installed (qagame base 0x{:x}, site 0x{:x}, " \
                   "ring 0x{:x})\n".format(reason, self._u64("fsvql_qagame_base"),
                                           self._u64("fsvql_site"), self._u64("fsvql_site_c"))
            # The load-time line is the one proof the plugin came up working, so
            # it is unconditional; the per-map reinstall is routine.
            if reason == "plugin load":
                self._log(line)
            else:
                self._vlog(line)
            self._verify(seq, reason)
            return 1
        if status in (-1, -2, -3, -5, -6, -7):
            # Deterministic for this build or this mapping, so retrying cannot
            # help; new_game tries again against the next mapping.
            if status == -2:
                self._log(NO_PATTERN_HINT.format(reason))
            else:
                self._log("[footstepsVQL] {}: {}\n".format(reason, self._text(status)))
            return status

        self._retry(seq, 0, reason)
        return status

    def _retry(self, seq, index, reason):
        if index >= len(RETRY_S):
            self._log("[footstepsVQL] {}: still not installed, giving up ({})\n".format(
                reason, self._text(self._status())))
            return

        @engine.delay(RETRY_S[index])
        def again():
            if self._cancelled(seq):
                return
            status = self._status()
            if status == -1:
                return
            if status != 1:
                self._call("fsvql_patch")
                status = self._status()
            if status == 1:
                self._log("[footstepsVQL] {}: hook installed after {}s\n".format(
                    reason, RETRY_S[index]))
                self._verify(seq, reason)
                return
            self._retry(seq, index + 1, reason)

        again()

    def _verify(self, seq, reason):
        """One late confirmation that a second mmap did not wipe the patch."""
        base = self._u64("fsvql_qagame_base")

        @engine.delay(VERIFY_S)
        def check():
            if self._cancelled(seq):
                return
            status = self._status()
            now = self._u64("fsvql_qagame_base")
            if status == 1 and now == base:
                return
            self._log("[footstepsVQL] {}: recheck after {}s, status {}, base 0x{:x} -> 0x{:x}\n".format(
                reason, VERIFY_S, status, base, now))
            if status != 1:
                self.ensure("{} recheck".format(reason))

        check()

    # ------------------------------------------------------------------- hooks

    def handle_new_game(self):
        """qagame was re-initialised, so both hook sites went with the old mapping."""
        if self.lib is None or not self._alive:
            return
        if not self._enabled():
            self._log("[footstepsVQL] new_game: auto-reapply is off, leaving qagame vanilla\n")
            return

        self._vlog("[footstepsVQL] new_game: qagame base 0x{:x}, status {}\n".format(
            self._u64("fsvql_qagame_base"), self._status()))

        @engine.next_frame
        def go():
            if self._alive:
                self.ensure("new_game")

        go()

    def handle_unload(self, plugin):
        """Cancel queued tasks and restore the vanilla bytes, so unloading really
        leaves qagame untouched. Hooks and commands come off on their own; a task
        already in the scheduler does not on minqlx, which is what _alive and the
        sequence bump are for."""
        if str(plugin) != self.__class__.__name__:
            return
        self._alive = False
        self._seq += 1
        if self.lib is None:
            return
        rc = self._call("fsvql_unpatch")
        self._log("[footstepsVQL] unload: {}\n".format(
            UNPATCH_TEXT.get(rc, "^1unknown result {}^7".format(rc))))

    # ---------------------------------------------------------------- commands
    #
    # Every handler returns RET_STOP_ALL: each fully answers the command it was
    # registered for, so neither another plugin nor the engine should also reply.

    def _no_lib(self, player):
        player.tell("^7[footstepsVQL] {}".format(self._build()))
        player.tell("^1[footstepsVQL]^7 native hook is ^1not loaded^7; tried: {}".format(
            ", ".join(self._candidates())))
        player.tell("^7[footstepsVQL] nothing to report until a library loads.")
        return RET_STOP_ALL

    def _tell_state(self, player, label, status, enabled):
        player.tell("^7[footstepsVQL] {}: patch is {}, auto-reapply on new map is {}".format(
            label, self._text(status), self._intent(enabled)))

    def cmd_status(self, player, msg, channel):
        # Cooldown ahead of the library check, so the no-library path is limited too.
        now = time.monotonic()
        waiting = STATUS_COOLDOWN_S - (now - self._last_status)
        if waiting > 0:
            player.tell("^7[footstepsVQL] just reported; ask again in {}s.".format(
                int(waiting) + 1))
            return RET_STOP_ALL
        self._last_status = now

        if self.lib is None:
            return self._no_lib(player)

        status = self._status()
        enabled = self._enabled()
        player.tell("^7[footstepsVQL] {}".format(self._build()))
        self._tell_state(player, "^3now^7", status, enabled)
        player.tell("^7[footstepsVQL] library: {}".format(self.lib_path))
        if self.lib_loaded and self.lib_loaded != self.lib_path:
            player.tell("^7[footstepsVQL] loaded copy: {}".format(self.lib_loaded))
        player.tell("^7[footstepsVQL] qagame base 0x{:x}, bob site 0x{:x}, ring site 0x{:x}".format(
            self._u64("fsvql_qagame_base"), self._u64("fsvql_site"), self._u64("fsvql_site_c")))

        vanilla = self._u64("fsvql_vanilla_flips")
        stripped = self._u64("fsvql_ring_suppressed")
        written = self._u64("fsvql_ring_written")
        drained = self._u64("fsvql_ring_drained")
        player.tell("^7[footstepsVQL] {} bob updates; movers' own clients played {} footsteps, "
                    "{} were owed".format(self._u64("fsvql_hits"), vanilla,
                                          self._u64("fsvql_injections_due")))
        player.tell("^7[footstepsVQL] listeners were sent {}: {} injected ({} ^2delivered^7), "
                    "{} vanilla, {} vanilla replaced by the clock".format(
                        vanilla - stripped + drained, written, drained,
                        vanilla - stripped, stripped))
        quiet = self._u64("fsvql_quiet")
        if quiet:
            player.tell("^7[footstepsVQL] {} command(s) silent in vanilla (walk key, crouched, "
                        "water, footsteps off): nothing injected".format(quiet))
        deferred = self._u64("fsvql_ring_deferred")
        if deferred:
            player.tell("^7[footstepsVQL] {} injection(s) waited a command for the engine "
                        "to have room".format(deferred))
        # skipped and missed are footsteps left in a mover's ring unpredicted - the
        # one thing this exists to prevent - so they only appear when non-zero.
        left = self._u64("fsvql_ring_skipped") + self._u64("fsvql_ring_missed")
        if left:
            player.tell("^1[footstepsVQL]^7 {} footstep(s) were left in a mover's ring".format(left))
        if written > drained:
            player.tell("^1[footstepsVQL]^7 {} owed footstep(s) reached nobody".format(
                written - drained))

        buckets = self._buckets()
        if buckets:
            player.tell("^7[footstepsVQL] command intervals: {}".format(", ".join(buckets)))
        else:
            player.tell("^7[footstepsVQL] command intervals: none sampled yet "
                        "(walk on the ground to sample)")

        if msg is not None and len(msg) > 1 and msg[1].lower() == "reset":
            try:
                self.lib.fsvql_reset_counters()
            except AttributeError:
                player.tell("^1[footstepsVQL]^7 this library cannot reset counters.")
            else:
                player.tell("^7[footstepsVQL] action: counters above were the state "
                            "before; they are now ^2zeroed^7.")
                # A player mutated measurement state, so it keeps an audit line
                # regardless of the verbose cvar.
                self._log("[footstepsVQL] counters reset\n")
            return RET_STOP_ALL

        if status != 1 and enabled:
            player.tell("^7[footstepsVQL] use ^2!fsvload^7 to apply it now.")
        elif status == 1:
            player.tell("^7[footstepsVQL] use ^3!fsvunload^7 to restore vanilla, "
                        "or ^2!fsvstatus reset^7 before a measurement.")
        return RET_STOP_ALL

    def cmd_load(self, player, msg, channel):
        if self.lib is None:
            return self._no_lib(player)

        before, before_on = self._status(), self._enabled()
        self._tell_state(player, "^3before^7", before, before_on)

        self.set_cvar("qlx_footstepsVQLEnabled", "1")
        if before == 1:
            action = "already patched, left the code alone"
        else:
            self.ensure("!fsvload")
            after_try = self._status()
            if after_try == 1:
                action = "^2applied the patch^7"
            else:
                action = "could not apply it: {}".format(self._text(after_try))
        if not before_on:
            action += "; auto-reapply turned ^2on^7"

        player.tell("^7[footstepsVQL] action: {}".format(action))
        self._tell_state(player, "^3now^7", self._status(), self._enabled())
        return RET_STOP_ALL

    def cmd_unload(self, player, msg, channel):
        if self.lib is None:
            return self._no_lib(player)

        before, before_on = self._status(), self._enabled()
        self._tell_state(player, "^3before^7", before, before_on)

        # Cancels any retry or verify task still scheduled from an earlier apply.
        self._seq += 1
        self.set_cvar("qlx_footstepsVQLEnabled", "0")
        rc = self._call("fsvql_unpatch")
        player.tell("^7[footstepsVQL] action: {}; auto-reapply turned ^3off^7".format(
            UNPATCH_TEXT.get(rc, "^1unknown result {}^7".format(rc))))
        self._tell_state(player, "^3now^7", self._status(), self._enabled())
        player.tell("^7[footstepsVQL] the library stays mapped; ^2!fsvload^7 puts it back.")
        return RET_STOP_ALL
