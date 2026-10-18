#!/usr/bin/env python3
"""Pin `native-port-probe.py --game`: each game's targets must measure that game's tree only.

The probe's target lists are built per game from one template. The failure that template invites
is a half-substituted path or define: a Generals target that still reads a `GeneralsMD/` include
directory, or compiles with `RTS_ZEROHOUR=1`, compiles against the wrong headers and reports a
number for neither game. Nothing in the probe's own output would show it.

Run: python3 scripts/native-port-probe-game-test.py
"""
import importlib.util
import pathlib
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]

CHECKS = 0
FAILURES = []


def load(path, name):
    """Import a hyphenated script by path, the way no `import` statement can."""
    spec = importlib.util.spec_from_file_location(name, REPO_ROOT / path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def check(label, got, want):
    global CHECKS
    CHECKS += 1
    if got != want:
        FAILURES.append(f"{label}: got {got!r}, want {want!r}")
        print(f"FAIL {label}\n     got  {got!r}\n     want {want!r}")
    else:
        print(f"ok   {label}")


def target_paths(target):
    """Every repo-relative path a target reads: include dirs, CMake lists and walked dirs."""
    paths = list(target.includes) + list(target.cmake_dirs) + list(target.source_dirs)
    paths += [path for path in (target.cmake_lists, target.cmake_root) if path]
    return paths


probe = load("scripts/native-port-probe.py", "native_port_probe")

for game_name, game in sorted(probe.GAMES.items()):
    other_dirs = {other.directory for other in probe.GAMES.values() if other != game}
    other_defines = {other.define for other in probe.GAMES.values() if other != game}
    default, renderer = probe.game_targets(game)
    for target in default + renderer:
        paths = target_paths(target)
        foreign = [path for path in paths if path.split("/")[0] in other_dirs]
        check(f"{game_name}: {target.name} reads no other game's tree", foreign, [])
        missing = [path for path in paths if not (REPO_ROOT / path).exists()]
        check(f"{game_name}: {target.name} reads only paths that exist", missing, [])
        check(f"{game_name}: {target.name} has no other game's define",
              sorted(other_defines & set(target.defines)), [])

# The module-level lists are what CI's baselines and every importer measure.
check("TARGETS is Zero Hour's", probe.TARGETS, probe.game_targets(probe.GAMES["zerohour"])[0])
check("RENDERER_TARGETS is Zero Hour's", probe.RENDERER_TARGETS,
      probe.game_targets(probe.GAMES["zerohour"])[1])

print(f"\n{CHECKS - len(FAILURES)} / {CHECKS} checks passed")
sys.exit(1 if FAILURES else 0)
