#!/usr/bin/env python3
"""Assert the native build was compiled with the flags that keep replays in sync.

At -O2, Apple clang's default -fno-math-errno lets the backend fuse `sinf(x)` and `cosf(x)` into one
`__sincosf_stret` call whose last bits differ, and a replay goes out of sync within 700 frames
(docs/porting/optimised-native-build.md). This gate reads the build directory
scripts/native-build.py produced and checks two things:

  * every compile command carries `-ffp-contract=off` and `-fmath-errno`, and the optimisation the
    configuration asked for (`-O2 -fno-strict-aliasing`, or no `-O` at all);
  * no archive imports a fused sine/cosine (`sincosf`, `__sincosf_stret`, ...).

`--self-check` compiles a sine and a cosine of one angle at -O2 with and without -fmath-errno and
requires the import scan to flag the first and pass the second, so a pass means the scan can still
see the defect on this host. Retail replays cannot run in CI; this is the part of their guard
that can.

    python3 scripts/ci/check-native-build-flags.py --build-dir build/native --expect-optimised
    python3 scripts/ci/check-native-build-flags.py --build-dir build/native-debug
    python3 scripts/ci/check-native-build-flags.py --self-check
"""

import argparse
import json
import os
import pathlib
import shlex
import shutil
import subprocess
import sys
import tempfile

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
ALWAYS = ("-ffp-contract=off", "-fmath-errno")
OPTIMISED = ("-O2", "-fno-strict-aliasing")
# Leading underscores stripped: Mach-O prefixes one, and Apple's fused forms are `__sincos*_stret`.
FUSED_SINCOS = {"sincos", "sincosf", "sincosl", "sincos_stret", "sincosf_stret"}
CONTROL_SOURCE = """
#include <math.h>
float rotate(float angle, float *cosine) { *cosine = cosf(angle); return sinf(angle); }
"""


def nm_tool():
    for name in ("llvm-nm-14", "llvm-nm", "nm"):
        found = shutil.which(name)
        if found:
            return found
    raise SystemExit("no nm on PATH, so the archives cannot be read")


def fused_imports(path):
    """The fused sine/cosine symbols `path` (an object or archive) leaves undefined."""
    proc = subprocess.run([nm_tool(), "-u", str(path)], capture_output=True, text=True, check=True)
    names = {line.split()[-1] for line in proc.stdout.splitlines() if line.strip()
             and not line.endswith(":")}
    return sorted(name for name in names if name.lstrip("_") in FUSED_SINCOS)


def flag_problems(compile_commands, expect_optimised):
    problems = []
    for entry in compile_commands:
        arguments = entry.get("arguments") or shlex.split(entry["command"])
        wanted = ALWAYS + (OPTIMISED if expect_optimised else ())
        missing = [flag for flag in wanted if flag not in arguments]
        levels = [arg for arg in arguments if arg.startswith("-O")]
        unexpected = [] if expect_optimised else levels
        if missing or unexpected:
            problems.append(f"{entry['file']}: missing {missing}, unexpected {unexpected}")
    return problems


def self_check():
    compiler = os.environ.get("CLANGXX", "clang++")
    with tempfile.TemporaryDirectory() as scratch:
        source = pathlib.Path(scratch) / "control.cpp"
        source.write_text(CONTROL_SOURCE)
        verdicts = {}
        for errno_flag in ("-fno-math-errno", "-fmath-errno"):
            obj = pathlib.Path(scratch) / f"control{errno_flag}.o"
            subprocess.run([compiler, "-O2", errno_flag, "-c", str(source), "-o", str(obj)],
                           check=True)
            verdicts[errno_flag] = fused_imports(obj)
    print(f"control at -O2 -fno-math-errno imports {verdicts['-fno-math-errno']}, "
          f"at -O2 -fmath-errno {verdicts['-fmath-errno']}")
    if not verdicts["-fno-math-errno"] or verdicts["-fmath-errno"]:
        print("FAIL: the scan does not separate the fused control from the unfused one",
              file=sys.stderr)
        return 1
    print("OK: the scan flags the fused control and passes the unfused one")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", type=pathlib.Path, default=REPO_ROOT / "build" / "native")
    parser.add_argument("--expect-optimised", action="store_true",
                        help="require -O2 -fno-strict-aliasing (the release configuration); "
                             "without it, require no -O flag at all (debug, or --unoptimised)")
    parser.add_argument("--self-check", action="store_true")
    args = parser.parse_args()

    if args.self_check:
        return self_check()

    database = args.build_dir / "compile_commands.json"
    archives = sorted(args.build_dir.rglob("*.a"))
    if not database.is_file() or not archives:
        sys.exit(f"{args.build_dir} has no compile_commands.json or no archive: "
                 "run scripts/native-build.py first")

    problems = flag_problems(json.loads(database.read_text()), args.expect_optimised)
    for archive in archives:
        fused = fused_imports(archive)
        if fused:
            problems.append(f"{archive.relative_to(args.build_dir)} imports {', '.join(fused)}")

    expected = "-O2 -fno-strict-aliasing" if args.expect_optimised else "-O0"
    print(f"{args.build_dir}: {expected} with {' '.join(ALWAYS)}, {len(archives)} archives scanned")
    if problems:
        for problem in problems[:20]:
            print(f"FAIL: {problem}", file=sys.stderr)
        print(f"{len(problems)} problem(s)", file=sys.stderr)
        return 1
    print("OK: every compile command has the expected flags and no archive imports a fused sincos")
    return 0


if __name__ == "__main__":
    sys.exit(main())
