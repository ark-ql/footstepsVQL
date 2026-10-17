#!/usr/bin/env python3
"""Check server/footstepsVQL.py against the minqlx-plugin-review checklist.

  https://github.com/dngrtech/minqlx-plugin-review

Ported from ql-footsteps-fix/scripts/_plugin_review_check.py, which gates the
footsteps250 release, and tightened for this plugin. One file serves both
runtimes, so the checks are:

* Forbidden patterns from the checklist's Quick Audit - HTTP without a thread,
  Redis scans, unbounded workers, eval/exec, broad excepts, console_command, a
  `frame` hook.
* Hooked event handlers and command handlers have the arity the engines call
  them with. minqlxtended checks this at registration and refuses the whole
  plugin on one mismatch.
* A single-string command name is lowercase. minqlx lowercases a tuple of names
  but not a single string, while it lowercases what the player typed, so an
  uppercase single name can never match there.
* Shim integrity: each engine package appears exactly once as a NAME token,
  inside its own `import <engine> as engine`, and both spellings of the stop
  constant are bound. Tokenised, not grepped - "minqlx" is a prefix of
  "minqlxtended" and appears legitimately in prose.
* Nothing that minqlxtended removed (RET_*, PRI_*, WP_*, removed setters, ...).
* Python 3.5 syntax only, because the minqlx Ubuntu 16 containers run 3.5: no
  f-strings, walrus, variable annotations, positional-only parameters, `match`,
  or underscores in numeric literals.

Exit status is 0 on pass, 1 otherwise.
"""
from __future__ import annotations

import argparse
import ast
import pathlib
import re
import sys
import tokenize

ROOT = pathlib.Path(__file__).resolve().parent.parent
PLUGIN = ROOT / "server" / "footstepsVQL.py"

ALIAS = "engine"
ENGINES = ("minqlx", "minqlxtended")

FORBIDDEN = [
    (r"\brequests\.(get|post|put|delete)\b", "HTTP call (step 1)"),
    (r"@(minqlx|minqlxtended|engine)\.thread\b", "background thread (steps 2, 5)"),
    (r"\bwhile\s+True\b", "unbounded worker loop (step 8)"),
    (r"\btime\.sleep\s*\(", "sleep loop (step 8)"),
    (r"\bthreading\.", "raw threading (step 8)"),
    (r"\bsocket\b", "socket (step 8)"),
    (r"\.keys\s*\(", "Redis keyspace scan (step 6)"),
    (r"\.smembers\s*\(|\.lrange\s*\(|\.zrange\s*\(", "large Redis read (step 6)"),
    (r"\.zadd\s*\(|\.hmset\s*\(", "version-sensitive redis API (step 7)"),
    (r"\beval\s*\(|\bexec\s*\(", "eval/exec (step 11)"),
    (r"\bexcept\s*:", "bare except (step 11)"),
    (r"\bexcept\s+Exception\b", "except Exception (step 11)"),
    (r"\bexcept\s+BaseException\b", "except BaseException (step 11)"),
    (r"console_command\s*\(", "console command sink (step 13)"),
    (r"add_hook\s*\(\s*[\"']frame[\"']", "frame hook (step 14)"),
]

REMOVED_ON_EXTENDED = [
    (r"\bRET_[A-Z_]+\b", "RET_* constant; use Return.*"),
    (r"\bPRI_[A-Z_]+\b", "PRI_* constant; use Priority.*"),
    (r"\bWP_[A-Z_]+\b", "WP_* constant; use Weapon.*"),
    (r"\b(red_score|blue_score)\b", "Game.red_score/blue_score; use team_scores"),
    (r"\bself\.kick\s*\(", "Plugin.kick does not exist on minqlxtended"),
    (r"\bset_(ammo|armor|flight|health|holdable|invulnerability|position|"
     r"powerups|privileges|score|velocity|weapon|weapons)\s*\(",
     "removed single-field setter; assign the property"),
    (r"\b(noclip|allow_single_player)\s*\(", "removed engine function"),
]

EVENT_ARITY = {"new_game": 0, "unload": 1}
COMMAND_ARITY = 3   # (player, msg, channel)

# Syntax newer than Python 3.5, as AST node types.
PY36_NODES = {
    "JoinedStr": "f-string (3.6)",
    "NamedExpr": "walrus operator (3.8)",
    "AnnAssign": "variable annotation (3.6)",
    "Match": "match statement (3.10)",
}


def strip_strings_and_comments(path: pathlib.Path) -> str:
    """Source with comments and string literals blanked, so prose in a docstring
    cannot trip a pattern rule and a comment naming a pattern does not report
    itself."""
    out = []
    with open(path, "rb") as fh:
        for tok in tokenize.tokenize(fh.readline):
            if tok.type in (tokenize.STRING, tokenize.COMMENT):
                out.append(" " * len(tok.string))
            elif tok.type == getattr(tokenize, "FSTRING_START", -1):
                out.append(" ")
            else:
                out.append(tok.string)
            out.append(" ")
    return "".join(out)


def engine_name_lines(path: pathlib.Path) -> dict[str, list[int]]:
    found = {name: [] for name in ENGINES}
    with open(path, "rb") as fh:
        for tok in tokenize.tokenize(fh.readline):
            if tok.type == tokenize.NAME and tok.string in found:
                found[tok.string].append(tok.start[0])
    return found


def numeric_underscores(path: pathlib.Path) -> list[int]:
    lines = []
    with open(path, "rb") as fh:
        for tok in tokenize.tokenize(fh.readline):
            if tok.type == tokenize.NUMBER and "_" in tok.string:
                lines.append(tok.start[0])
    return lines


def shim_imports(tree: ast.Module) -> dict[str, int]:
    out = {}
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            for a in node.names:
                if a.name in ENGINES and a.asname == ALIAS:
                    out[a.name] = node.lineno
    return out


def calls_to(tree: ast.Module, attr: str) -> list[ast.Call]:
    return [n for n in ast.walk(tree)
            if isinstance(n, ast.Call) and isinstance(n.func, ast.Attribute)
            and n.func.attr == attr]


def handler_arity(tree: ast.Module, method: str) -> int | None:
    for node in ast.walk(tree):
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)) and node.name == method:
            args = [a.arg for a in node.args.args]
            return len(args) - 1 if args and args[0] == "self" else len(args)
    return None


def check(path: pathlib.Path = PLUGIN) -> list[str]:
    problems = []
    if not path.is_file():
        return ["missing file {}".format(path)]

    source = path.read_text(encoding="utf-8")
    code = strip_strings_and_comments(path)
    try:
        tree = ast.parse(source, filename=str(path))
    except SyntaxError as e:
        return ["does not parse: {}".format(e)]

    for pattern, why in FORBIDDEN:
        if re.search(pattern, code):
            problems.append("forbidden: {}  [{}]".format(why, pattern))

    # --- Python 3.5 ----------------------------------------------------------
    for node in ast.walk(tree):
        why = PY36_NODES.get(type(node).__name__)
        if why:
            problems.append("not Python 3.5: {} at line {}".format(
                why, getattr(node, "lineno", "?")))
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.Lambda)) \
                and getattr(node.args, "posonlyargs", None):
            problems.append("not Python 3.5: positional-only parameters (3.8) at "
                            "line {}".format(node.lineno))
    for line in numeric_underscores(path):
        problems.append("not Python 3.5: underscore in a numeric literal (3.6) at "
                        "line {}".format(line))

    # --- shim integrity -----------------------------------------------------
    imports = shim_imports(tree)
    for name in ENGINES:
        if name not in imports:
            problems.append("shim does not `import {} as {}`; the file would not run "
                            "on that runtime".format(name, ALIAS))
    for name, lines in engine_name_lines(path).items():
        stray = sorted(set(lines) - {imports.get(name)})
        if stray:
            problems.append("`{}` used as a NAME outside the shim, line(s) {} - route "
                            "it through `{}`".format(name, stray, ALIAS))
        if len(lines) > 1:
            problems.append("`{}` appears {} times as a NAME; the shim should be its "
                            "only occurrence".format(name, len(lines)))
    if "Return.STOP_ALL" not in source:
        problems.append("shim never reads Return.STOP_ALL (minqlxtended)")
    if "engine.RET_STOP_ALL" not in source:
        problems.append("shim never reads engine.RET_STOP_ALL (minqlx)")
    if re.search(r"getattr\s*\(\s*engine\s*,\s*[\"']RET_", code):
        problems.append("stop constant read through getattr with a default; on "
                        "minqlxtended that yields a plain int, which is not "
                        "Return.STOP_ALL")

    for pattern, why in REMOVED_ON_EXTENDED:
        hits = [h for h in re.findall(pattern, code) if h != "RET_STOP_ALL"]
        if hits:
            problems.append("not on minqlxtended: {}  {}".format(why, sorted(set(hits))))

    # --- handlers -----------------------------------------------------------
    events = {}
    for call in calls_to(tree, "add_hook"):
        if len(call.args) >= 2 and isinstance(call.args[0], ast.Constant) \
                and isinstance(call.args[1], ast.Attribute):
            events[call.args[0].value] = call.args[1].attr
    for event, handler in sorted(events.items()):
        if event not in EVENT_ARITY:
            problems.append("hooks '{}', which this checker has no arity for - verify "
                            "it against both engines' _events.py".format(event))
            continue
        arity = handler_arity(tree, handler)
        if arity != EVENT_ARITY[event]:
            problems.append("'{}' handler {} takes {} args, expected {}".format(
                event, handler, arity, EVENT_ARITY[event]))
    if "unload" not in events:
        problems.append("no unload hook; delayed tasks would outlive the plugin (step 8)")

    for call in calls_to(tree, "add_command"):
        if len(call.args) < 2:
            continue
        name, handler = call.args[0], call.args[1]
        # Tuples are lowercased by both engines; a single string only by
        # minqlxtended.
        if isinstance(name, ast.Constant) and isinstance(name.value, str) \
                and name.value != name.value.lower():
            problems.append("command name '{}' is not lowercase; minqlx cannot "
                            "match a single-string name that is not".format(name.value))
        if isinstance(handler, ast.Attribute):
            arity = handler_arity(tree, handler.attr)
            if arity != COMMAND_ARITY:
                problems.append("command handler {} takes {} args, expected {} "
                                "(player, msg, channel)".format(handler.attr, arity,
                                                                COMMAND_ARITY))
    return problems


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--quiet", action="store_true", help="only print failures")
    ap.add_argument("--path", type=pathlib.Path, default=PLUGIN)
    args = ap.parse_args()

    problems = check(args.path)
    if problems:
        print("FAIL  {}".format(args.path))
        for p in problems:
            print("        - {}".format(p))
        print("\nplugin review: FAILED")
        return 1
    if not args.quiet:
        print("ok    {}: {} forbidden patterns absent, Python 3.5 syntax, shim intact, "
              "handler arities and command names ok".format(args.path.name, len(FORBIDDEN)))
        print("\nplugin review: pass, both runtimes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
