#!/usr/bin/env python3
"""Find `FALSE`/`TRUE` used where a pointer is expected.

`Lib/BaseTypeCore.h` defines `FALSE` as `false` unless something already defined it. While
`PreRTS.h` force-included `<windows.h>` into every GameEngine translation unit, `FALSE` was
`windows.h`'s `0`, so `return FALSE;` from a pointer-returning function and `ptr != FALSE`
silently compiled. Now that the Win32 block is gone, `FALSE` is a genuine `bool` and those are
hard errors -- but only under a compiler that enforces it. VC6 accepts the `bool` literal
`false` as a null pointer constant, so the `vc6` presets cannot see this class at all and the
`win32` presets report it one file at a time.

This runs clang over every GameEngine translation unit and reports only bool/pointer
diagnostics, so the whole class is found in one pass. Everything else clang complains about
off Windows is ignored: the point is not that the tree compiles natively (it does not), it is
that no bool ever stands in for a pointer.

The Win32 shims are used, minus their `TRUE`/`FALSE` definitions -- with those in place the
shims would paper over exactly what is being looked for.

Usage:
    python3 scripts/ci/check-bool-pointer.py [--jobs N]
"""

import argparse
import concurrent.futures
import dataclasses
import importlib.util
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]

spec = importlib.util.spec_from_file_location(
    "native_port_probe", REPO_ROOT / "scripts" / "native-port-probe.py")
probe_mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe_mod)

# Debug and logging configurations compile code the release presets do not, and that code is
# where the surviving instances of this class hide.
EXTRA_DEFINES = ("RTS_DEBUG", "DEBUG_LOGGING", "DEBUG_CRASHING")

# clang's spellings for "a bool turned up where a pointer belongs" and the reverse.
BOOL_POINTER = re.compile(
    r"cannot initialize return object of type '[^']*\*'[^\n]*with an rvalue of type 'bool'|"
    r"comparison between pointer and (integer|'bool')|"
    r"assigning to '[^']*\*' from incompatible type 'bool'|"
    r"cannot initialize a (parameter|variable) of type '[^']*\*' with an rvalue of type 'bool'|"
    r"invalid operands to binary expression \('[^']*\*' and 'bool'\)|"
    r"invalid operands to binary expression \('bool' and '[^']*\*'\)")


def shim_dir_without_bool_macros(tmp):
    """A copy of the Win32 shims with `#define TRUE 1` / `#define FALSE 0` removed."""
    dst = pathlib.Path(tmp) / "shims"
    shutil.copytree(REPO_ROOT / "scripts" / "native-port-shims", dst)
    header = dst / "windows.h"
    text = header.read_text()
    text = re.sub(r"^#define\s+(TRUE|FALSE)\s+[01]\s*$", "", text, flags=re.M)
    header.write_text(text)
    return dst


def run(job):
    target, source, shim_dir = job
    includes = [str(shim_dir)]
    includes += [str(REPO_ROOT / d) for d in probe_mod.COMMON_INCLUDES]
    includes += [str(REPO_ROOT / d) for d in target.includes]

    cmd = [probe_mod.CLANGXX, *probe_mod.CLANG_FLAGS]
    cmd += [f"-D{d}" for d in tuple(target.defines) + EXTRA_DEFINES]
    cmd += [f"-I{d}" for d in includes]
    cmd.append(str(source))
    proc = subprocess.run(cmd, capture_output=True, text=True)
    hits = [line for line in proc.stderr.splitlines()
            if ": error:" in line and BOOL_POINTER.search(line)]
    return hits


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--jobs", type=int, default=0)
    args = parser.parse_args()

    generals = dataclasses.replace(
        [t for t in probe_mod.TARGETS if t.name == "GeneralsMD/Code/GameEngine"][0],
        name="Generals/Code/GameEngine",
        cmake_lists="Generals/Code/GameEngine/CMakeLists.txt",
        cmake_root="Generals/Code/GameEngine",
        defines=("RTS_GENERALS=1",))
    targets = [t for t in probe_mod.TARGETS if t.name.endswith("GameEngine")] + [generals]

    with tempfile.TemporaryDirectory() as tmp:
        shim_dir = shim_dir_without_bool_macros(tmp)
        jobs = [(t, s, shim_dir) for t in targets for s in probe_mod.cmake_sources(t)]
        print(f"Checking {len(jobs)} translation units...", file=sys.stderr)
        hits = []
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs or None) as pool:
            for result in pool.map(run, jobs):
                hits.extend(result)

    for hit in sorted(set(hits)):
        print(hit)
    print(f"\n{len(set(hits))} bool/pointer diagnostics")
    return 1 if hits else 0


if __name__ == "__main__":
    sys.exit(main())
