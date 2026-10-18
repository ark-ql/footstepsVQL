#!/usr/bin/env python3
"""Prove server/footstepsVQL.py loads and behaves under both runtimes, offline.

The plugin picks its runtime from sys.modules, so the runtime can be faked:
inject a stub engine under the name the real one has, import the plugin against
it, and drive it. Two things make the stubs worth trusting:

* minqlxtended's Return is modelled as a plain enum.Enum, exactly as the real
  engine defines it. footsteps250's version of this test used IntEnum, under
  which a bare int 3 compares equal to Return.STOP_ALL - the very bug this
  plugin shipped with would have passed.
* the minqlxtended stub checks handler signatures at registration, the way the
  real engine does, so a wrong arity fails here rather than on the server.

Each runtime is run twice: once with no library (the load-failure path must be
loud and the commands must still answer), and once with a fake native library
standing in for footstepsVQL_hook.so, which exercises load, new_game,
!fsvstatus and its reset, !fsvunload / !fsvload, a refusal that must not retry,
and that nothing queued before an unload can patch qagame after it. Both runs
check that the load line and !fsvstatus name the version and the plugin's hash.

Exit status 0 when both runtimes pass, 1 otherwise.
"""
from __future__ import annotations

import enum
import hashlib
import importlib.util
import inspect
import pathlib
import sys
import types

PLUGIN = pathlib.Path(__file__).resolve().parent.parent / "server" / "footstepsVQL.py"

COMMANDS = ("fsvstatus", "footstepsvql", "fsvload", "fsvunload")
RETIRED_COMMANDS = ("vqlstatus", "vqlmode", "vqlemit", "vqlload", "vqlunload")
RETIRED_CVARS = ("qlx_footstepsVQLMode", "qlx_footstepsVQLEmit")


# ---------------------------------------------------------------- stub engines

def make_plugin_base(engine, check_signatures):
    class StubPlugin:
        _cvars = {}

        def __init__(self):
            self.hooks = {}
            self.commands = {}

        @property
        def name(self):
            return type(self).__name__

        def set_cvar_once(self, name, value, flags=0):
            if name not in StubPlugin._cvars:
                StubPlugin._cvars[name] = str(value)
                return True
            return False

        def set_cvar(self, name, value, flags=0):
            StubPlugin._cvars[name] = str(value)
            return True

        def get_cvar(self, name, return_type=str):
            return StubPlugin._cvars.get(name)

        def add_hook(self, event, handler, priority=0):
            if check_signatures:
                want = {"new_game": 0, "unload": 1}[event]
                got = len(inspect.signature(handler).parameters)
                if got != want:
                    raise TypeError("Event '{}' passes {} arguments, handler takes {}".format(
                        event, want, got))
            self.hooks[event] = handler

        def add_command(self, name, handler, permission=0, channels=None,
                        exclude_channels=(), priority=0, client_cmd_pass=False,
                        client_cmd_perm=5, prefix=True, usage=""):
            if check_signatures:
                params = list(inspect.signature(handler).parameters)
                if params != ["player", "msg", "channel"]:
                    raise TypeError("A command handler must take (player, msg, channel), "
                                    "got {}".format(params))
            names = name if isinstance(name, (list, tuple)) else (name,)
            for n in names:
                self.commands[n.lower() if isinstance(name, (list, tuple)) else n] = handler

    return StubPlugin


def stub_engine(name):
    m = types.ModuleType(name)
    m.printed = []
    m.queued = []           # delay tasks, run only when a test says time passed
    m.frames = []           # next_frame tasks, run by flush()
    m.console_print = lambda text: m.printed.append(text)

    def next_frame(fn):
        def schedule(*a, **k):
            m.frames.append(fn)
        return schedule

    m.next_frame = next_frame

    def delay(seconds):
        def deco(fn):
            def schedule(*a, **k):
                m.queued.append(fn)
            return schedule
        return deco

    m.delay = delay
    if name == "minqlx":
        m.RET_STOP_ALL = 3
        m.Plugin = make_plugin_base(m, check_signatures=False)
        m.stop_all = m.RET_STOP_ALL
    else:
        class Return(enum.Enum):
            NONE = 0
            STOP = 1
            STOP_EVENT = 2
            STOP_ALL = 3
            USAGE = 4
        m.Return = Return
        m.Plugin = make_plugin_base(m, check_signatures=True)
        m.stop_all = Return.STOP_ALL
    return m


def flush(engine):
    """Run the next server frame's tasks."""
    while engine.frames:
        engine.frames.pop(0)()


def dispatch_ok(engine, res):
    """What each engine's handle_input does with a command handler's return."""
    if engine.__name__ == "minqlx":
        return res == engine.RET_STOP_ALL
    return res == engine.Return.STOP_ALL


class Player:
    def __init__(self):
        self.told = []

    def tell(self, msg):
        self.told.append(msg)


# ------------------------------------------------------------ fake native lib

class FakeFn:
    def __init__(self, impl):
        self.impl = impl
        self.argtypes = None
        self.restype = None

    def __call__(self, *args):
        return self.impl(*args)


class FakeLib:
    """Just enough of footstepsVQL_hook.so's contract: fsvql_patch installs both
    sites or neither and returns patch_rc, fsvql_status reports it, a refusal is
    sticky the way g_status is, and fsvql_unpatch restores."""

    def __init__(self):
        self.status = 0
        self.patch_rc = 1
        self.calls = []

    def remap(self):
        """A map change: qagame is remapped and every site goes vanilla."""
        self.status = 0

    def _patch(self):
        self.status = self.patch_rc
        return self.patch_rc

    def _unpatch(self):
        was = self.status
        self.status = 0
        return 1 if was == 1 else 0

    def __getattr__(self, name):
        impls = {
            "fsvql_status": lambda: self.status,
            "fsvql_patch": self._patch,
            "fsvql_unpatch": self._unpatch,
            "fsvql_dt_bucket": lambda i: 0,
        }
        if name in impls:
            impl = impls[name]
        elif name.startswith("fsvql_"):
            impl = lambda *a: 0  # noqa: E731
        else:
            raise AttributeError(name)

        def recorded(*args):
            self.calls.append(name)
            return impl(*args)

        fn = FakeFn(recorded)
        self.__dict__[name] = fn
        return fn


# ------------------------------------------------------------------ harness

def load_under(engine):
    saved = {n: sys.modules.pop(n, None) for n in ("minqlx", "minqlxtended")}
    sys.modules.pop("footstepsVQL", None)
    sys.modules[engine.__name__] = engine
    try:
        spec = importlib.util.spec_from_file_location("footstepsVQL", PLUGIN)
        mod = importlib.util.module_from_spec(spec)
        sys.modules["footstepsVQL"] = mod
        spec.loader.exec_module(mod)
        return mod
    finally:
        sys.modules.pop("footstepsVQL", None)
        sys.modules.pop(engine.__name__, None)
        for n, v in saved.items():
            if v is not None:
                sys.modules[n] = v


def _raise_oserror(path):
    raise OSError("no such file: {}".format(path))


def run_no_lib(engine_name):
    problems = []
    engine = stub_engine(engine_name)
    try:
        mod = load_under(engine)
    except Exception as e:  # the test reports any import failure as a failure
        return ["import failed: {}: {}".format(type(e).__name__, e)]

    if mod.engine is not engine:
        problems.append("aliased the wrong engine")
    if mod.RET_STOP_ALL is not engine.stop_all:
        problems.append("stop constant is {!r}, expected {!r}".format(
            mod.RET_STOP_ALL, engine.stop_all))

    mod.ctypes.CDLL = _raise_oserror
    engine.Plugin._cvars.clear()
    try:
        plugin = mod.footstepsVQL()
    except Exception as e:
        return problems + ["__init__ raised: {}: {}".format(type(e).__name__, e)]

    for event in ("new_game", "unload"):
        if event not in plugin.hooks:
            problems.append("did not hook {}".format(event))
    for cmd in COMMANDS:
        if cmd not in plugin.commands:
            problems.append("did not register !{}".format(cmd))
    for cmd in RETIRED_COMMANDS:
        if cmd in plugin.commands:
            problems.append("still registers the retired !{}".format(cmd))
    cv = type(plugin)._cvars
    for name, want in (("qlx_footstepsVQLEnabled", "1"), ("qlx_footstepsVQLVerbose", "0"),
                       ("qlx_footstepsVQLSo", "")):
        if cv.get(name) != want:
            problems.append("{} defaults to {!r}, expected {!r}".format(name, cv.get(name), want))
    for name in RETIRED_CVARS:
        if name in cv:
            problems.append("still registers the retired {}".format(name))
    if not any("no hook library loaded" in t for t in engine.printed):
        problems.append("library-missing path printed nothing; it has to be loud")

    player = Player()
    for cmd in ("fsvstatus", "fsvload", "fsvunload"):
        res = plugin.commands[cmd](player, ["!" + cmd], None)
        if not dispatch_ok(engine, res):
            problems.append("!{} returned {!r}, which this engine does not read as "
                            "STOP_ALL".format(cmd, res))
    if not player.told:
        problems.append("commands said nothing with no library loaded")
    problems += version_problems(mod, engine, player)
    return problems


def version_problems(mod, engine, player):
    """The load line and !fsvstatus name the version and this file's sha256."""
    if not getattr(mod, "VERSION", None):
        return ["no VERSION constant"]
    want = "version {}: .so ".format(mod.VERSION)
    py = ".py " + hashlib.sha256(PLUGIN.read_bytes()).hexdigest()[:8]
    problems = []
    if not any(want in t and py in t for t in engine.printed):
        problems.append("no load-time line with {!r} and {!r}".format(want, py))
    if not any(want in t and py in t for t in player.told):
        problems.append("!fsvstatus did not report {!r} and {!r}".format(want, py))
    return problems


def fresh(engine_name):
    engine = stub_engine(engine_name)
    mod = load_under(engine)
    lib = FakeLib()
    mod.ctypes.CDLL = lambda path: lib
    engine.Plugin._cvars.clear()
    return engine, mod, lib


def run_with_lib(engine_name):
    problems = []
    engine, mod, lib = fresh(engine_name)

    plugin = mod.footstepsVQL()
    if lib.status != 1:
        problems.append("plugin load did not install the hook")
    if not any("plugin load: hook installed" in t for t in engine.printed):
        problems.append("no unconditional load-time line")

    # A map change: every site vanilla, then new_game, then the next frame.
    lib.remap()
    plugin.hooks["new_game"]()
    flush(engine)
    if lib.status != 1:
        problems.append("new_game did not re-install the hook")

    player = Player()
    res = plugin.commands["fsvstatus"](player, ["!fsvstatus"], None)
    if not dispatch_ok(engine, res):
        problems.append("!fsvstatus returned {!r}".format(res))
    if not any("patched and active" in t for t in player.told):
        problems.append("!fsvstatus did not report the hook as active")
    if "fsvql_reset_counters" in lib.calls:
        problems.append("a plain !fsvstatus reset the counters")
    problems += version_problems(mod, engine, player)

    # The cooldown answers, and does not run the report again.
    before = len(lib.calls)
    plugin.commands["fsvstatus"](player, ["!fsvstatus"], None)
    if not any("ask again" in t for t in player.told) or len(lib.calls) != before:
        problems.append("the !fsvstatus cooldown did not hold")

    plugin._last_status -= mod.STATUS_COOLDOWN_S
    res = plugin.commands["fsvstatus"](player, ["!fsvstatus", "reset"], None)
    if not dispatch_ok(engine, res) or "fsvql_reset_counters" not in lib.calls:
        problems.append("!fsvstatus reset did not zero the counters")
    if not any("counters reset" in t for t in engine.printed):
        problems.append("!fsvstatus reset left no audit line")

    # Unload restores vanilla, and nothing queued before it may patch afterwards:
    # neither the 5 s recheck from the last ensure, nor a new_game whose
    # next_frame task is still pending when the plugin goes away. Both would
    # re-patch without the guard, because they see status 0.
    engine.queued.clear()
    plugin.ensure("test")
    if not engine.queued:
        problems.append("ensure queued no recheck, so the unload guard is untested")
    lib.remap()
    plugin.hooks["new_game"]()
    if not engine.frames:
        problems.append("new_game queued no frame task, so the unload guard is untested")
    plugin.hooks["unload"]("footstepsVQL")
    before = lib.calls.count("fsvql_patch")
    flush(engine)
    for fn in list(engine.queued):
        fn()
    if lib.calls.count("fsvql_patch") != before:
        problems.append("a task queued before unload patched qagame after it")
    if lib.status != 0:
        problems.append("unload left the hook patched")

    # !fsvunload / !fsvload write the intent cvar, as !fsunload / !fsload do.
    engine, mod, lib = fresh(engine_name)
    p2 = mod.footstepsVQL()
    res = p2.commands["fsvunload"](Player(), ["!fsvunload"], None)
    if not dispatch_ok(engine, res):
        problems.append("!fsvunload returned {!r}".format(res))
    if lib.status != 0 or type(p2)._cvars.get("qlx_footstepsVQLEnabled") != "0":
        problems.append("!fsvunload did not restore vanilla and turn auto-reapply off")
    lib.remap()
    p2.hooks["new_game"]()
    flush(engine)
    if lib.status != 0:
        problems.append("new_game re-applied the hook with auto-reapply off")
    told = Player()
    res = p2.commands["fsvload"](told, ["!fsvload"], None)
    if not dispatch_ok(engine, res):
        problems.append("!fsvload returned {!r}".format(res))
    if lib.status != 1 or type(p2)._cvars.get("qlx_footstepsVQLEnabled") != "1":
        problems.append("!fsvload did not re-apply the hook and turn auto-reapply on")
    if not any("before" in t for t in told.told) or not any("now" in t for t in told.told):
        problems.append("!fsvload did not answer with before and now")

    # A build without the ring site: refused once, loudly, and not retried.
    engine, mod, lib = fresh(engine_name)
    lib.patch_rc = -5
    mod.footstepsVQL()
    if not any("event-ring site is not in this qagame build" in t for t in engine.printed):
        problems.append("a -5 refusal was not reported")
    if engine.queued:
        problems.append("a deterministic refusal queued a retry")
    return problems


def run_lib_paths(engine_name):
    """Where the library is looked for: the cvar, then whichever default exists,
    next to the plugin first, then footstepsVQL_lib/ (a QLSM repository install).
    A default that isn't there is not tried, so it can't add a warning line."""
    problems = []
    engine, mod, lib = fresh(engine_name)
    plugin = mod.footstepsVQL()
    here = str(PLUGIN.parent)
    beside = mod.os.path.join(here, "footstepsVQL_hook.so")
    sub = mod.os.path.join(here, "footstepsVQL_lib", "footstepsVQL_hook.so")
    real_isfile = mod.os.path.isfile
    try:
        for present, cvar, want in (
                ({sub}, "", [sub]),
                ({beside, sub}, "", [beside, sub]),
                (set(), "", [beside, sub]),
                ({sub}, "/opt/x/footstepsVQL_hook.so", ["/opt/x/footstepsVQL_hook.so", sub])):
            mod.os.path.isfile = lambda p, present=present: p in present
            type(plugin)._cvars["qlx_footstepsVQLSo"] = cvar
            got = plugin._candidates()
            if got != want:
                problems.append("present={} cvar={!r}: candidates {}, expected {}".format(
                    sorted(mod.os.path.relpath(p, here) for p in present), cvar, got, want))
    finally:
        mod.os.path.isfile = real_isfile
    return problems


def main():
    global PLUGIN
    if len(sys.argv) > 1:
        PLUGIN = pathlib.Path(sys.argv[1]).resolve()
    failed = False
    for name in ("minqlx", "minqlxtended"):
        for label, fn in (("no library", run_no_lib), ("fake library", run_with_lib),
                          ("library lookup", run_lib_paths)):
            problems = fn(name)
            if problems:
                failed = True
                print("FAIL  {:13} {}".format(name, label))
                for p in problems:
                    print("        - {}".format(p))
            else:
                print("ok    {:13} {}".format(name, label))

    saved = {n: sys.modules.pop(n, None) for n in ("minqlx", "minqlxtended")}
    try:
        spec = importlib.util.spec_from_file_location("footstepsVQL_bare", PLUGIN)
        m = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(m)
        print("FAIL  no-engine     imported cleanly with neither engine loaded")
        failed = True
    except ImportError as e:
        print("ok    no-engine     refused: {}".format(str(e).split(";")[0]))
    finally:
        for n, v in saved.items():
            if v is not None:
                sys.modules[n] = v

    print("\nshim test: " + ("FAILED" if failed else "both runtimes pass"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
