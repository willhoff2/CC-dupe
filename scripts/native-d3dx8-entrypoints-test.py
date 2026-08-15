#!/usr/bin/env python3
"""Build and run the behaviour tests for the D3DX 8 entry points WW3D2 needs off Windows.

On Windows these come from d3dx8.lib. Off Windows the port implements them in WW3D2:

    d3dx8fvf.cpp      D3DXGetFVFVertexSize   -- a vertex stride from an FVF bitmask
    d3dx8texture.cpp  D3DXFilterTexture      -- mip-chain generation
                      D3DXLoadSurfaceFromSurface -- surface blit with format conversion
    d3dx8shader.cpp   D3DXAssembleShader     -- a deliberate, loud refusal

None of these can be checked by building the game: a stride that is four bytes short, a box filter
that averages the wrong footprint, a 5-bit channel widened by 8 instead of by 255/31, and a shader
assembler that returns success with no bytecode all compile, link and start. They are checked by
asserting the values, which is what these three suites do -- see the header comment of each for
what is asserted and, for the choices D3DX does not document, what was asserted instead.

The suites are built with AddressSanitizer and UndefinedBehaviorSanitizer when the toolchain has
them, because the surfaces here are strided memory and a misaligned load or a write one byte past a
row is exactly the kind of defect that passes every value assertion. (It found one: the pixel
readers used to cast a row pointer to `unsigned *`, and a surface pitch is not guaranteed to be a
multiple of the pixel size.)

Usage:
    python3 scripts/native-d3dx8-entrypoints-test.py [--suite NAME] [--keep] [--verbose]
                                                     [--no-sanitizers]

Exits non-zero if any suite does not compile, does not link, or fails an assertion. `CLANGXX`
selects the compiler, as it does for native-build.py and the other standalone checks.
"""

import argparse
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
CLANGXX = os.environ.get("CLANGXX", "clang++")

WW3D2_DIR = "Core/Libraries/Source/WWVegas/WW3D2"
WWLIB_DIR = "Core/Libraries/Source/WWVegas/WWLib"

# The vendored DirectX 8 headers, which scripts/ci/fetch-probe-deps.sh puts here. These declare
# the entry points under test, so without them the test would be testing a different declaration
# set from the one the engine compiles against.
DX8_INCLUDE = "build/docker/_deps/dx8-src"

# Shims first, then the vendored SDK, then the repo -- the order native-port-probe.py uses.
INCLUDES = [
    "scripts/native-port-shims",
    DX8_INCLUDE,
    "Dependencies/Utility",
    "Core/Libraries/Include",
    "Core/Libraries/Source/WWVegas",
    WW3D2_DIR,
]

COMPILE_FLAGS = [
    "-std=c++20",
    "-m64",
    "-g",
    "-fms-extensions",
    "-include", "Utility/CppMacros.h",
    "-DWIN32_LEAN_AND_MEAN",
    "-D_REENTRANT",
    # The vendored headers and the shims disagree about TRUE/FALSE, and always.h redeclares the
    # global operator delete; both are the engine's own long-standing noise, not this test's.
    "-Wno-macro-redefined",
    "-Wno-implicit-exception-spec-mismatch",
    # The COM method declarations the fake surfaces override carry __stdcall, which is not a thing
    # on 64-bit; the engine is compiled the same way.
    "-Wno-ignored-attributes",
]

SANITIZER_FLAGS = ["-O1", "-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
PLAIN_FLAGS = ["-O0"]

SUITES = {
    # dx8fvf.cpp is linked in as the second oracle: it derives the same layout from the same bits
    # without calling the function under test. wwstring.cpp is what dx8fvf.cpp's debug-only
    # Get_FVF_Name() needs to link.
    "fvf": {
        "description": "the FVF vertex size matches the documented D3D8 layout",
        "sources": [
            f"{WW3D2_DIR}/d3dx8fvf.cpp",
            f"{WW3D2_DIR}/dx8fvf.cpp",
            f"{WWLIB_DIR}/wwstring.cpp",
            f"{WW3D2_DIR}/tests/d3dx8fvf_test.cpp",
        ],
    },
    "texture": {
        "description": "the texture filter and surface blit produce the right pixels",
        "sources": [
            f"{WW3D2_DIR}/d3dx8texture.cpp",
            f"{WW3D2_DIR}/tests/d3dx8texture_test.cpp",
        ],
    },
    "shader": {
        "description": "the shader assembler refuses loudly and cannot be mistaken for success",
        "sources": [
            f"{WW3D2_DIR}/d3dx8shader.cpp",
            f"{WW3D2_DIR}/tests/d3dx8shader_test.cpp",
        ],
    },
}


def run(command, verbose, **kwargs):
    if verbose:
        print("+", " ".join(str(part) for part in command))
    return subprocess.run(command, **kwargs)


def sanitizers_work(build_dir, verbose):
    """Whether this toolchain can build and run a sanitized binary at all."""
    source = build_dir / "sanitizer-probe.cpp"
    source.write_text("int main() { return 0; }\n")
    binary = build_dir / "sanitizer-probe"
    command = [CLANGXX] + SANITIZER_FLAGS + [str(source), "-o", str(binary)]
    if run(command, verbose, capture_output=True).returncode != 0:
        return False
    return run([str(binary)], verbose, capture_output=True).returncode == 0


def build_and_run(name, suite, build_dir, flags, verbose):
    binary = build_dir / f"d3dx8_{name}_test"
    command = [CLANGXX] + COMPILE_FLAGS + flags
    for include in INCLUDES:
        command += ["-I", str(REPO_ROOT / include)]
    command += [str(REPO_ROOT / source) for source in suite["sources"]]
    command += ["-o", str(binary)]

    if run(command, verbose, cwd=REPO_ROOT).returncode != 0:
        print(f"FAILED: the {name} suite did not compile or link")
        return False

    if run([str(binary)], verbose).returncode != 0:
        print(f"FAILED: {suite['description']} -- it does not")
        return False

    print(f"OK: {suite['description']}")
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--suite", choices=sorted(SUITES), action="append",
                        help="run only this suite (repeatable); default is all of them")
    parser.add_argument("--keep", action="store_true",
                        help="keep the build directory instead of removing it")
    parser.add_argument("--no-sanitizers", action="store_true",
                        help="build without AddressSanitizer and UndefinedBehaviorSanitizer")
    parser.add_argument("--verbose", action="store_true", help="echo the commands")
    args = parser.parse_args()

    if sys.platform == "win32":
        print("On Windows these entry points are d3dx8.lib's and the implementations here are "
              "empty; build WW3D2/tests/ against d3dx8.lib instead.")
        return 1

    if not (REPO_ROOT / DX8_INCLUDE).is_dir():
        print(f"FAILED: {DX8_INCLUDE} is missing. Run ./scripts/ci/fetch-probe-deps.sh first: "
              "without the vendored DirectX 8 headers these entry points have no declarations.")
        return 1

    build_dir = pathlib.Path(tempfile.mkdtemp(prefix="d3dx8-entrypoints-test-"))
    try:
        if args.no_sanitizers:
            flags = PLAIN_FLAGS
        elif sanitizers_work(build_dir, args.verbose):
            flags = SANITIZER_FLAGS
        else:
            flags = PLAIN_FLAGS
            print("NOTE: this toolchain cannot build sanitized binaries, so the strided-memory "
                  "checks are the guard bytes only")

        failures = 0
        for name in (args.suite or sorted(SUITES)):
            if not build_and_run(name, SUITES[name], build_dir, flags, args.verbose):
                failures += 1

        return 1 if failures else 0
    finally:
        if args.keep:
            print(f"build directory kept at {build_dir}")
        else:
            shutil.rmtree(build_dir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
