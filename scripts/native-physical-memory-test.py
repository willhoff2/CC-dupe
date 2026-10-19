#!/usr/bin/env python3
"""Check that the engine's total physical memory is the operating system's, off Windows.

`CPUDetectClass::Init_Memory()` was a `#warning` off Windows, so the total stayed 0, and
GameLODManager's `m_memPassed` (at least 256 MB) was false on every machine. That picked the 640
intro movies, turned trees off at the preset detail levels, skipped the Generals campaign briefing
video and auto-detected the detail level as if there were no memory. See
docs/porting/physical-memory.md.

This links `Core/GameEngine/Source/Common/System/tests/physical_memory_test.cpp`, compiled with
cpudetect.cpp's own flags, against WWLib's archive from the native build. CPUDetectClass's static
initialiser runs as it does in the game, and the printed total must equal the OS figure
(`sysctl hw.memsize` on macOS, `sysconf` on Linux) and pass the game's RAM test.

Usage:
    python3 scripts/native-build.py --level 1 --level 2 --level 3 --level 4 \\
        --with-shims --strict-link          # must run first: this uses its archives
    python3 scripts/native-physical-memory-test.py [--keep] [--verbose]
"""

import argparse
import importlib.util
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
HARNESS = REPO_ROOT / "Core/GameEngine/Source/Common/System/tests/physical_memory_test.cpp"
FLAG_DONOR = "cpudetect.cpp"
# cpudetect.cpp's archive and what it calls into; linked as ordinary archives, so only the members
# the harness reaches come in, and CPUDetectClass's static initialiser with them.
ARCHIVES = ("libcore_libraries_source_wwvegas_wwlib.a",
            "libcore_libraries_source_wwvegas_wwdebug.a")
# GameLOD.cpp: `(Real)(m_numRAM)/(Real)(256*1024*1024) >= PROFILE_ERROR_LIMIT` (0.94).
GAME_RAM_MINIMUM = 0.94 * 256 * 1024 * 1024
TOTAL_RE = re.compile(r"^total physical memory: (\d+)$", re.MULTILINE)


def load_render_runner():
    """The render harness's script, imported: it owns the compile recipe."""
    path = REPO_ROOT / "scripts" / "native-render-backend-run.py"
    spec = importlib.util.spec_from_file_location("native_render_backend_run", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def operating_system_total():
    """The machine's physical memory in bytes, from the platform's own interface."""
    if sys.platform == "darwin":
        output = subprocess.run(["sysctl", "-n", "hw.memsize"], capture_output=True, text=True,
                                check=True).stdout
        return int(output.strip())
    return os.sysconf("SC_PHYS_PAGES") * os.sysconf("SC_PAGE_SIZE")


def link(harness_object, binary, build_dir):
    archives = [build_dir / name for name in ARCHIVES]
    missing = [a.name for a in archives if not a.is_file()]
    if missing:
        sys.exit(f"{', '.join(missing)} missing from {build_dir}: run scripts/native-build.py "
                 "--level 1 --level 2 --level 3 --level 4 --with-shims --strict-link first")
    runtime = ["-lc++"] if sys.platform == "darwin" else ["-lstdc++", "-lpthread"]
    command = [os.environ.get("CLANGXX", "clang++"), "-std=c++20", "-o", str(binary),
               str(harness_object), *[str(a) for a in archives], *runtime, "-lm"]
    proc = subprocess.run(command, capture_output=True, text=True)
    if proc.returncode != 0:
        sys.stdout.write(proc.stdout + proc.stderr)
        sys.exit("the harness did not link")
    return proc.stdout + proc.stderr


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--keep", action="store_true", help="keep the scratch directory")
    parser.add_argument("--verbose", action="store_true", help="echo the compiler output")
    args = parser.parse_args()

    if sys.platform.startswith("win"):
        print("Windows reads the total through GlobalMemoryStatusEx(); this measures the "
              "platform layer that replaces it")
        return 0

    runner = load_render_runner()
    scratch = pathlib.Path(tempfile.mkdtemp(prefix="physical-memory-test-"))
    try:
        harness_object = scratch / "physical_memory_test.o"
        binary = scratch / "physical_memory_test"
        output = runner.compile_harness(harness_object, harness=HARNESS, donor=FLAG_DONOR)
        output += link(harness_object, binary, runner.BUILD_DIR)
        if args.verbose and output.strip():
            print(output)

        proc = subprocess.run([str(binary)], capture_output=True, text=True)
        sys.stdout.write(proc.stdout)
        match = TOTAL_RE.search(proc.stdout)
        if proc.returncode != 0 or match is None:
            sys.stdout.write(proc.stderr)
            print(f"FAILED: the harness exited {proc.returncode} without a total")
            return 1

        engine_total = int(match.group(1))
        os_total = operating_system_total()
        print(f"operating system total: {os_total}")
        failures = []
        if engine_total != os_total:
            failures.append(f"the engine's total {engine_total} is not the operating system's "
                            f"{os_total}")
        if engine_total < GAME_RAM_MINIMUM:
            failures.append(f"{engine_total} bytes fails GameLODManager's RAM test "
                            f"(>= {GAME_RAM_MINIMUM:.0f}), so m_memPassed would be false")
        for failure in failures:
            print(f"FAILED: {failure}")
        if failures:
            return 1
        print(f"\nOK: CPUDetectClass records {engine_total / 2**30:.1f} GiB, the operating "
              "system's figure, and GameLODManager's memory test passes")
        return 0
    finally:
        if args.keep:
            print(f"scratch directory kept at {scratch}")
        else:
            shutil.rmtree(scratch, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
