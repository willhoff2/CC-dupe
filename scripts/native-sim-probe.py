#!/usr/bin/env python3
"""Build and run the headless simulation probe harness (spikes/sim).

The harness is not the game: it links the archives `scripts/native-build.py` already produced and
calls individual engine subsystems directly, so a subsystem's first genuine off-Windows failure can
be observed without the retail `.big` archives, without a renderer and without GameEngine::init().

Compile flags are taken from the native build's own `compile_commands.json` entry for an engine
translation unit, so the harness cannot drift from how the engine itself was compiled. The link uses
ordinary archive semantics (not --whole-archive) so only what the probe reaches is pulled in, and
the game's own main() in libgeneralsmd_code_main.a stays out.

The harness inherits the configuration of the build directory it is pointed at, because it takes its
compile flags from that directory's compile database: `--build-dir build/native-debug` produces a
probe compiled with `-DRTS_DEBUG`, and therefore one whose engine assertions are live.

Usage:
    python3 scripts/native-sim-probe.py --build
    python3 scripts/native-sim-probe.py --build-dir build/native-debug --build
    python3 scripts/native-sim-probe.py -- chunks path/to/map.map
"""

import argparse
import json
import os
import shlex
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DEFAULT_BUILD = REPO / "build" / "native"
BUILD = DEFAULT_BUILD
SOURCE = REPO / "spikes" / "sim" / "src" / "sim_probe.cpp"
OUT = BUILD / "sim_probe"
# The entry-point archive defines main(); linking it would either collide with the probe's main or
# drag the whole game start-up in.
EXCLUDED_ARCHIVES = {"libgeneralsmd_code_main.a"}
# An engine translation unit whose compile line is the reference for the probe's own.
REFERENCE_UNIT = "Core/GameEngine/Source/GameClient/MapUtil.cpp"


def reference_flags():
    """Return (compiler, flags) from the native build's command line for REFERENCE_UNIT."""
    db = BUILD / "compile_commands.json"
    if not db.exists():
        sys.exit(f"{db} is missing; run scripts/native-build.py --level 1 --level 2 "
                 "--level 3 --level 4 --with-shims --strict-link first")
    entries = json.loads(db.read_text())
    match = [e for e in entries if e["file"].endswith(REFERENCE_UNIT)]
    if not match:
        sys.exit(f"no compile command for {REFERENCE_UNIT} in {db}")
    argv = shlex.split(match[0]["command"])
    compiler = argv[0]
    flags = []
    skip = 0
    for i, arg in enumerate(argv[1:]):
        if skip:
            skip -= 1
            continue
        if arg in ("-o", "-c"):
            skip = 1
            continue
        flags.append(arg)
    # The device layer's own include root: the probe constructs StdLocalFileSystem, which the engine
    # normally only reaches through createLocalFileSystem() inside that layer.
    flags += ["-isystem", str(REPO / "Core" / "GameEngineDevice" / "Include")]
    # The game-specific device layer, for the modes that call a W3D class directly (the shroud's
    # world-position lookup). The reference unit is in Core, which does not see this root.
    flags += ["-isystem", str(REPO / "GeneralsMD" / "Code" / "GameEngineDevice" / "Include")]
    return compiler, flags


def archives():
    found = sorted(p for p in BUILD.glob("*.a") if p.name not in EXCLUDED_ARCHIVES)
    if not found:
        sys.exit(f"no archives in {BUILD}; run scripts/native-build.py first")
    return found


def extra_link_args():
    """The third-party libraries the engine archives depend on, as the native build links them."""
    args = ["-lstdc++", "-lm", "-lpthread", "-ldl", "-lz", "-lSDL2"]
    # The render backend archive is on the link because the engine archives reference it; the debug
    # configuration reaches more of it, so its Vulkan loader dependency has to be satisfied here as
    # the native build itself satisfies it.
    for prefix in ("/usr/lib/x86_64-linux-gnu", "/usr/lib", "/usr/local/lib"):
        if (Path(prefix) / "libvulkan.so.1").exists():
            args.append("-lvulkan")
            break
    for name in ("libopenal.so", "libopenal.so.1"):
        for prefix in ("/usr/lib/x86_64-linux-gnu", "/usr/lib", "/usr/local/lib"):
            candidate = Path(prefix) / name
            if candidate.exists():
                args.append(str(candidate))
                break
        else:
            continue
        break
    deps = REPO / "build" / "docker" / "_deps"
    for pattern in ("ffmpeg-build/lib/*.so", "ffmpeg-src/lib/*.so"):
        args += [str(p) for p in sorted(deps.glob(pattern))]
    return args


def build(verbose=False):
    compiler, flags = reference_flags()
    obj = BUILD / "sim_probe.o"
    compile_cmd = [compiler, *flags, "-c", str(SOURCE), "-o", str(obj)]
    if verbose:
        print(" ".join(compile_cmd))
    proc = subprocess.run(compile_cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        sys.stdout.write(proc.stdout + proc.stderr)
        sys.exit("probe compile failed")
    sys.stdout.write(proc.stdout + proc.stderr)

    link_cmd = [
        # -rdynamic for the same reason cmake/config-build.cmake passes it in the debug
        # configuration: an assertion's stack dump is backtrace_symbols(), which can only name
        # frames whose symbols are in the dynamic symbol table.
        compiler, "-std=gnu++20", "-g", "-rdynamic", "-o", str(OUT), str(obj),
        "-Wl,--start-group", *[str(a) for a in archives()], "-Wl,--end-group",
        *extra_link_args(),
    ]
    if verbose:
        print(" ".join(link_cmd))
    proc = subprocess.run(link_cmd, capture_output=True, text=True)
    sys.stdout.write(proc.stdout + proc.stderr)
    if proc.returncode != 0:
        sys.exit("probe link failed")
    print(f"built {OUT} ({OUT.stat().st_size} bytes)")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", action="store_true", help="build the harness and exit")
    ap.add_argument("--build-dir", default=str(DEFAULT_BUILD),
                    help="native build directory to take flags and archives from "
                         "(default: build/native); the probe inherits its configuration")
    ap.add_argument("--verbose", action="store_true", help="print the compile and link commands")
    ap.add_argument("args", nargs=argparse.REMAINDER,
                    help="arguments passed to the harness (after --)")
    opts = ap.parse_args()

    global BUILD, OUT
    BUILD = Path(opts.build_dir).resolve()
    OUT = BUILD / "sim_probe"

    if opts.build or not OUT.exists():
        build(opts.verbose)
    if opts.build:
        return 0

    argv = [a for a in opts.args if a != "--"]
    if not argv:
        return 0
    env = dict(os.environ)
    return subprocess.run([str(OUT), *argv], env=env).returncode


if __name__ == "__main__":
    sys.exit(main())
