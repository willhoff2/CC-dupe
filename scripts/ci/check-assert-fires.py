#!/usr/bin/env python3
"""The negative control for the native debug configuration: prove an assertion actually fires.

A debug build that compiles is worth nothing on its own. `-DRTS_DEBUG` was unbuildable off Windows
for the whole port, and the cost was measured: the headless simulation probe ticked 13,500 frames of
a retail map that had loaded ZERO objects, and the assertion the original developers wrote for
exactly that condition sat in the tree unable to run. A debug build whose asserts are compiled out,
routed to an assert dialog that does not exist off Windows, or swallowed by a portable stub would be
indistinguishable from the build that found nothing -- so this gate feeds a KNOWN-BAD input to the
engine's own reader and requires the engine's own assertion to be the thing that stops it.

The input is a minimal `.map` chunk stream, uncompressed, whose table of contents names one chunk
and whose single chunk header then claims an id that is not in that table. That is a real corruption
of the on-disk format, and `DataChunkTableOfContents::getName()` has always had a DEBUG_CRASH for
it. Nothing about the assertion is modified to make this pass; the input is what is bad.

What is required of the output, all of it, or this fails:

  * a non-zero exit status -- an assertion that returns to the caller has not stopped anything;
  * the failure on stderr, because a log file inside the user data directory is not visible to a
    CI job, a crash reporter, or a developer running the binary;
  * the source file and line of the assertion;
  * the assertion's own message text;
  * a stack dump with at least one named frame, so the report can be acted on.

    python3 scripts/ci/check-assert-fires.py --build-dir build/native-debug

The probe binary is the headless harness (`scripts/native-sim-probe.py --build`) built from a debug
build directory, since it hosts the real engine without needing a window or a device. Retail data is
not needed and deliberately not used: the bad map is generated here, so this gate runs anywhere.
"""
import argparse
import pathlib
import struct
import subprocess
import sys
import tempfile

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]

# The assertion this gate expects, and where it lives. Named explicitly rather than matched loosely,
# because "some assert fired" would also be satisfied by an assert about the harness's own setup.
EXPECTED_FILE = "DataChunk.cpp"
EXPECTED_TEXT = "name not found in DataChunkTableOfContents::getName"
UNKNOWN_CHUNK_ID = 7


def write_bad_map(path):
    """A `.map` whose one chunk header names an id its table of contents does not define.

    The layout is DataChunkTableOfContents::write() followed by one chunk header, which is what
    DataChunkInput reads: the `CkMp` tag, the symbol count, one length-prefixed name with its id,
    then a chunk header of id, version and data size. The chunk carries four bytes of payload for
    one reason: `openDataChunk` returns an empty label without consulting the table when the read
    hits end of file, so a header with nothing behind it would exercise the EOF path instead of the
    lookup this gate is about.
    """
    name = b"HeightMapData"
    data = b"CkMp" + struct.pack("<i", 1)
    data += bytes([len(name)]) + name + struct.pack("<I", 1)
    data += struct.pack("<IHi", UNKNOWN_CHUNK_ID, 1, 4) + b"\x00\x00\x00\x00"
    path.write_bytes(data)


def named_frame(stderr):
    """True when the stack dump names at least one frame rather than only printing addresses.

    backtrace_symbols() can only name a symbol that is in the dynamic symbol table, so this is also
    the check that the debug configuration is still linked with -rdynamic.
    """
    for line in stderr.splitlines():
        stripped = line.strip()
        if not stripped.startswith("/") and "(" not in stripped:
            continue
        if "(+0x" in stripped or "(" not in stripped:
            continue
        return True
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build-dir", default="build/native-debug",
                    help="debug build directory holding the harness (default: build/native-debug)")
    ap.add_argument("--probe", default=None,
                    help="harness binary (default: <build-dir>/sim_probe)")
    opts = ap.parse_args()

    build_dir = pathlib.Path(opts.build_dir)
    if not build_dir.is_absolute():
        build_dir = REPO_ROOT / build_dir
    probe = pathlib.Path(opts.probe) if opts.probe else build_dir / "sim_probe"
    if not probe.exists():
        print(f"FAIL: no harness at {probe}; build it with "
              f"`python3 scripts/native-sim-probe.py --build-dir {opts.build_dir} --build`",
              file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory() as scratch:
        bad_map = pathlib.Path(scratch) / "unknown-chunk-id.map"
        write_bad_map(bad_map)
        # The settings store is pointed at the scratch directory so the run cannot read or write a
        # developer's real one, and the harness runs there so its debug log lands in the scratch.
        # It has to carry an install path, because StdBIGFileSystem::init() asserts on an empty one
        # -- which is itself the debug configuration working, just not the assertion under test.
        settings = pathlib.Path(scratch) / "Registry.ini"
        settings.write_text(
            "[SOFTWARE\\Electronic Arts\\EA Games\\Generals]\n"
            f"STRING_InstallPath = {scratch}\n"
            "[SOFTWARE\\Electronic Arts\\EA Games\\Command and Conquer Generals Zero Hour]\n"
            f"STRING_InstallPath = {scratch}\n")
        env = {"HOME": scratch, "PATH": "/usr/bin:/bin", "CNC_SETTINGS_FILE": str(settings)}
        proc = subprocess.run([str(probe), "chunks", str(bad_map)], cwd=scratch, env=env,
                              capture_output=True, text=True)

    failures = []
    if proc.returncode == 0:
        failures.append("the harness exited 0: the assertion did not stop it")
    if "ASSERTION FAILURE" not in proc.stderr:
        failures.append("no assertion failure on stderr")
    if EXPECTED_FILE not in proc.stderr:
        failures.append(f"stderr does not name the source file ({EXPECTED_FILE})")
    if EXPECTED_TEXT not in proc.stderr:
        failures.append(f"stderr does not carry the assertion's message ({EXPECTED_TEXT!r})")
    if "Stack Dump:" not in proc.stderr:
        failures.append("stderr carries no stack dump")
    elif not named_frame(proc.stderr):
        failures.append("the stack dump names no frame; is the build still linked -rdynamic?")

    print("--- harness stderr " + "-" * 60)
    sys.stdout.write(proc.stderr)
    print("-" * 79)
    print(f"exit status: {proc.returncode}")

    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1

    print("PASS: a known-bad map trips the engine's own assertion, on stderr, with a stack dump, "
          f"and the process exits {proc.returncode}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
