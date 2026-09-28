#!/usr/bin/env python3
"""Build and run the null-string test for WW3D2's particle emitter copy paths.

WW3D2 duplicates its owned C strings with `::_strdup`, and six call sites -- in three classes --
passed a pointer that is routinely null, one of them always. `_strdup(nullptr)` reaches `strlen`,
which reads address zero: on Apple's libc and on glibc that is a SIGSEGV; the 32-bit MSVC CRT
returned nullptr, so the defect was invisible on Windows and has been latent since the source
release (3d0ee53a0). It is an upstream bug, not a port regression. The measured crash was the
fog-of-war ghost path cloning a dying unit's emitters:

    _platform_strlen <- strdup <- ParticleEmitterClass::ParticleEmitterClass(const &)
    <- ParticleEmitterClass::Clone() <- W3DRenderObjectSnapshot::update
    <- W3DGhostObject::snapShot <- Object::getShroudedStatus <- GameClient::update

This gate has two halves, because the three classes that carry the defect are not equally reachable.

  * A COMPILED test (tests/particle_emitter_strdup_test.cpp) links the real part_ldr.cpp and
    actually copies a `ParticleEmitterDefClass`, whose `operator=` feeds its own null m_pName and
    m_pUserString back into Set_Name / Set_User_String. That copy is an unconditional segfault
    before the fix. `ParticleEmitterClass` itself cannot be driven this way -- its constructor
    needs a ParticleBufferClass, and a standalone link would have to stub 89 symbols including
    two full render-object vtables -- so it is covered by the other half.

  * A SOURCE SCAN over the six files in SCANNED, asserting that every `::_strdup` argument is
    either a string literal, null-guarded on the spot, or one of the few expressions that
    cannot be null by construction (named below, with the reason). This is what pins the two
    call sites in ParticleEmitterClass's copy constructor, and AggregateDefClass::Set_Name in
    the shared tree, which carries the same defect by the same route (a null m_pName handed
    back to Set_Name by operator=, which Clone() calls) -- neither of which a standalone link
    reaches.

See docs/porting/particle-emitter-strdup.md.

Usage:
    python3 scripts/native-particle-emitter-strdup-test.py [--keep] [--verbose]

Exits non-zero if the sources do not compile, do not link, an assertion fails, or an unguarded
`::_strdup` of a nullable pointer reappears. `CLANGXX` selects the compiler, as it does for
native-build.py and the other standalone checks.
"""

import argparse
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
CLANGXX = os.environ.get("CLANGXX", "clang++")

ZH_WW3D2 = "GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2"
GEN_WW3D2 = "Generals/Code/Libraries/Source/WWVegas/WW3D2"

SOURCES = [
    "Core/Libraries/Source/WWVegas/WW3D2/tests/particle_emitter_strdup_test.cpp",
    # The real code under test. Zero Hour's copy, because Zero Hour is what the replay gate
    # replays and what the measured crash came from; the Generals copy is byte-identical in these
    # call sites and is covered by the source scan below.
    f"{ZH_WW3D2}/part_ldr.cpp",
    # Create_Randomizer builds a real Vector3SolidBoxRandomizer for a zeroed definition
    # (CLASSID_SOLIDBOX is 0), so the copy path needs the real randomizers rather than stubs --
    # stubbing them would mean hand-writing their vtables. v3_rnd.cpp is 173 lines and needs only
    # Random3Class, which random.cpp supplies.
    "Core/Libraries/Source/WWVegas/WWMath/v3_rnd.cpp",
    "Core/Libraries/Source/WWVegas/WWLib/random.cpp",
]

# The vendored DirectX 8 headers, which scripts/ci/fetch-probe-deps.sh puts here: part_ldr.h
# reaches the render-object headers, which reach d3d8.h.
DX8_INCLUDE = "build/docker/_deps/dx8-src"

# Shims first, then the vendored SDK, then the game's WW3D2 ahead of Core's -- the order the real
# build uses, and it matters: shader.h and part_emt.h exist only under the game's tree.
INCLUDES = [
    "scripts/native-port-shims",
    DX8_INCLUDE,
    "Dependencies/Utility",
    "Core/Libraries/Include",
    "GeneralsMD/Code/Libraries/Source/WWVegas",
    ZH_WW3D2,
    "Core/Libraries/Source/WWVegas",
    "Core/Libraries/Source/WWVegas/WWLib",
    "Core/Libraries/Source/WWVegas/WWDebug",
    "Core/Libraries/Source/WWVegas/WWMath",
    "Core/Libraries/Source/WWVegas/WWSaveLoad",
    "Core/Libraries/Source/WWVegas/WW3D2",
]

COMPILE_FLAGS = [
    "-std=c++20",
    "-m64",
    "-g",
    "-O0",
    "-fms-extensions",
    "-include", "Utility/CppMacros.h",
    "-DWIN32_LEAN_AND_MEAN",
    "-D_REENTRANT",
    "-DRTS_ZEROHOUR=1",
    # The engine's own long-standing noise, not this test's: the vendored headers and the shims
    # disagree about TRUE/FALSE, always.h redeclares the global operator delete, and the COM
    # declarations carry __stdcall, which is not a thing on 64-bit.
    "-Wno-macro-redefined",
    "-Wno-implicit-exception-spec-mismatch",
    "-Wno-ignored-attributes",
    # part_ldr.cpp and the WWMath sources predate any of this; the port compiles them the same way.
    "-Wno-deprecated-declarations",
    "-Wno-writable-strings",
    "-Wno-unused-value",
    # `memcpy` over a Vector3 array in Copy_Emitter_Property_Struct, and wwmath.h's `!(x & y)`
    # float truncation trick. Both are the engine's, both predate the port.
    "-Wno-nontrivial-memcall",
    "-Wno-logical-not-parentheses",
]

# ---------------------------------------------------------------------------------------------
# The source scan.
# ---------------------------------------------------------------------------------------------

SCANNED = [
    f"{ZH_WW3D2}/part_emt.cpp",
    f"{ZH_WW3D2}/part_ldr.cpp",
    f"{GEN_WW3D2}/part_emt.cpp",
    f"{GEN_WW3D2}/part_ldr.cpp",
    # AggregateDefClass carries the same defect in the shared tree: a null m_pName on a
    # default-constructed definition, handed back to Set_Name by operator=, which Clone() calls.
    "Core/Libraries/Source/WWVegas/WW3D2/agg_def.h",
    "Core/Libraries/Source/WWVegas/WW3D2/agg_def.cpp",
]

# The argument, up to the matching close paren. `[^()]*` on purpose: every call site in the scanned
# files passes a plain identifier or member expression, and a nested call would be a new shape this
# gate should refuse to guess about rather than silently accept.
STRDUP_CALL = re.compile(r"::_?strdup\s*\(\s*([^()]*?)\s*\)")

# Arguments that cannot be null, with the reason. Everything else has to be a string literal or
# guarded on the spot.
NON_NULL_BY_CONSTRUCTION = {
    # W3dParticleEmitterInfoStruct::Name is a char array inside a POD read off disk, not a
    # pointer, so it decays to the address of the array and is never null.
    "header.Name": "a char array in a chunk header struct, not a pointer",
}


def guarded(line, argument):
    """True when `line` null-checks `argument` before duplicating it."""
    collapsed = re.sub(r"\s+", "", line)
    needle = re.sub(r"\s+", "", argument)
    return f"{needle}!=nullptr" in collapsed or f"{needle}!=NULL" in collapsed


def scan_sources(verbose):
    """Assert every ::_strdup of a nullable pointer in the scanned files is null-guarded."""
    findings = []
    call_sites = 0
    for relative in SCANNED:
        path = REPO_ROOT / relative
        if not path.is_file():
            findings.append(f"{relative}: missing")
            continue
        for number, line in enumerate(path.read_text().splitlines(), start=1):
            if "strdup" not in line or line.lstrip().startswith(("//", "*", "/*")):
                continue
            matches = STRDUP_CALL.findall(line)
            if not matches:
                findings.append(f"{relative}:{number}: ::_strdup call this gate cannot parse: "
                                f"{line.strip()}")
                continue
            for argument in matches:
                call_sites += 1
                if argument.startswith('"'):
                    verdict = "literal"
                elif guarded(line, argument):
                    verdict = "guarded"
                elif argument in NON_NULL_BY_CONSTRUCTION:
                    verdict = f"non-null ({NON_NULL_BY_CONSTRUCTION[argument]})"
                else:
                    findings.append(f"{relative}:{number}: ::_strdup({argument}) is not "
                                    f"null-guarded; _strdup(nullptr) faults on a POSIX libc")
                    continue
                if verbose:
                    print(f"    ok  {relative}:{number}: ::_strdup({argument}) -- {verdict}")

    if findings:
        print("FAILED: the source scan found unguarded string duplication")
        for finding in findings:
            print(f"  {finding}")
        return 1
    print(f"OK: all {call_sites} ::_strdup call sites in {len(SCANNED)} files are literal, "
          f"guarded, or provably non-null")
    return 0


def run(command, verbose, **kwargs):
    if verbose:
        print("+", " ".join(str(part) for part in command))
    return subprocess.run(command, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--keep", action="store_true",
                        help="keep the build directory instead of removing it")
    parser.add_argument("--verbose", action="store_true", help="echo the commands")
    args = parser.parse_args()

    if not (REPO_ROOT / DX8_INCLUDE).is_dir():
        print(f"FAILED: {DX8_INCLUDE} is missing; run scripts/ci/fetch-probe-deps.sh first")
        return 2

    build_dir = pathlib.Path(tempfile.mkdtemp(prefix="particle-emitter-strdup-test-"))
    try:
        binary = build_dir / "particle_emitter_strdup_test"
        command = [CLANGXX] + COMPILE_FLAGS
        for include in INCLUDES:
            command += ["-isystem", str(REPO_ROOT / include)]
        command += [str(REPO_ROOT / source) for source in SOURCES]
        command += ["-o", str(binary)]

        result = run(command, args.verbose, cwd=REPO_ROOT)
        if result.returncode != 0:
            print("FAILED: the particle emitter definition sources did not compile or link")
            return result.returncode

        result = run([str(binary)], args.verbose)
        if result.returncode != 0:
            print("FAILED: copying a particle emitter definition with null strings is not safe")
            return result.returncode

        print("OK: a definition with null strings copies without faulting")
        return scan_sources(args.verbose)
    finally:
        if args.keep:
            print(f"build directory kept at {build_dir}")
        else:
            shutil.rmtree(build_dir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
