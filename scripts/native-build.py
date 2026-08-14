#!/usr/bin/env python3
"""Build the platform-independent libraries natively, 64-bit, and report what stops them.

Every native-port figure published before this script came from `clang++ -fsyntax-only`
(`scripts/native-port-probe.py`). No object file had ever been produced for a 64-bit non-Windows
target and no linker had ever run, so two whole classes of blocker were unmeasured:

* **codegen** — templates that only instantiate on emission, inline assembly, alignment/ABI
  problems. `-fsyntax-only` structurally cannot see them.
* **link** — every symbol the code declares and does not define. This is where the Win32 API,
  Direct3D, Miles, Bink and GameSpy dependencies actually bite.

This script measures both, and the divergence between them and the probe:

1. Generates one CMake fragment per library from the probe's own target definitions, so the
   translation-unit lists, include paths and flags are identical to what the probe measures.
2. Compiles objects with `cmake/native/CMakeLists.txt`, keeping going past failures, and records
   which translation units failed to *compile* despite the probe calling them clean.
3. Re-builds without the failed translation units so each library produces an archive, then runs
   the real linker over all of them and categorises every unresolved symbol.

A playable binary is not a goal. An honest, reproducible blocker list is.

Usage:
    python3 scripts/native-build.py [--level 1|2|3] [--with-shims]
                                    [--report docs/porting/native-build-report.md]
                                    [--json out.json] [--jobs N] [--build-dir DIR]
"""

import argparse
import collections
import concurrent.futures
import dataclasses
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import native_probe_targets as npt  # noqa: E402  (path shim must run first)

REPO_ROOT = npt.REPO_ROOT
NATIVE_CMAKE_DIR = REPO_ROOT / "cmake" / "native"
SHIM_DIR = REPO_ROOT / "scripts" / "native-port-shims"

CXX = os.environ.get("CLANGXX", "clang++")

# Build order, bottom-up. Level 1 is the set with no engine dependencies; level 2 adds the game
# engine proper, which depends on level 1; level 3 adds the device and entry-point layer.
#
# Level 3 is not expected to build cleanly and is not there to make the objects figure look better.
# It exists because excluding it made two whole categories of unresolved symbol uninterpretable:
# `TheKey_*` (104 symbols, instantiated only in GameEngineDevice's WorldHeightMap.cpp) and "defined
# in a layer not built here" (21) were artefacts of the level-1-2 scope, not port work. Including
# the layer converts each of them into either a resolved symbol or a compile failure attributable
# to a named file. The renderer libraries (WW3D2, WWAudio, WWDownload) are still out: they are the
# renderer seam's own measurement, and pulling them in here would mix two slices' figures.
LEVELS = {
    1: [
        "Core/Libraries/Source/Compression",
        "Core/Libraries/Source/WWVegas/WWMath",
        "Core/Libraries/Source/WWVegas/WWLib",
        "Core/Libraries/Source/WWVegas/WWDebug",
        "Core/Libraries/Source/WWVegas/WWSaveLoad",
    ],
    2: [
        "Core/GameEngine",
        "GeneralsMD/Code/GameEngine",
    ],
    3: [
        "Core/GameEngineDevice",
        "GeneralsMD/Code/GameEngineDevice",
        "GeneralsMD/Code/Main",
    ],
}

# Every target the probe knows about, in the probe's own definitions. Levels index into this.
ALL_TARGETS = npt.TARGETS + npt.RENDERER_TARGETS


def slug(target_name):
    return re.sub(r"[^A-Za-z0-9]+", "_", target_name).strip("_").lower()


# ---------------------------------------------------------------------------------------------
# Symbol categorisation
# ---------------------------------------------------------------------------------------------

# Win32 entry points are recognised from the declaration-only shim headers rather than from a
# hand-written list: the shims exist precisely because they enumerate what PreRTS.h pulls in, so
# they are the repo's own answer to "which of these names are Win32?". Anything they declare and
# nothing defines is a platform-layer gap, not a missing engine file.
WIN32_DECL_RE = re.compile(
    r"^\s*(?:extern\s+\"C\"\s+)?(?:WINBASEAPI\s+|WINUSERAPI\s+|WINADVAPI\s+)?"
    r"[A-Za-z_][A-Za-z0-9_ \t\*]*?\b(?:WINAPI|APIENTRY|__stdcall|__cdecl)?\s*"
    r"([A-Za-z_][A-Za-z0-9_]*)\s*\(",
    re.M,
)


# `extern char gcd_gamename[];`, `extern int foo;` -- the SDKs' global state, which is a symbol
# reference like any other and used to land in "Other / unclassified".
EXTERN_OBJECT_DECL_RE = re.compile(
    r"^\s*extern\s+(?:\"C\"\s+)?[A-Za-z_][\w \t\*]*?\b([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*;", re.M)

# `#define GenerateAuth GenerateAuthA` -- the SDKs' ANSI/wide selection macros. The engine calls
# `GenerateAuth`, so the name that reaches the linker is one no header ever *declares*, only
# defines a macro for. This is why `GenerateAuthA`, `SendGameSnapShotA`, `SetPersistDataValuesA`,
# `GetPersistDataValuesA` and `PreAuthenticatePlayerCDA` looked unclassifiable.
ALIAS_MACRO_RE = re.compile(r"^\s*#\s*define\s+(\w+)\s+(\w+)\s*$", re.M)

DECL_KEYWORDS = {"if", "for", "while", "switch", "return", "sizeof", "defined", "typedef"}


def declared_names_in(headers):
    """Function and object names a set of declaration-only headers declares."""
    names = set()
    for header in headers:
        try:
            text = header.read_text(errors="replace")
        except OSError:
            continue
        for match in WIN32_DECL_RE.finditer(text):
            names.add(match.group(1))
        for match in EXTERN_OBJECT_DECL_RE.finditer(text):
            names.add(match.group(1))
        for match in ALIAS_MACRO_RE.finditer(text):
            names.update(match.groups())
    return names - DECL_KEYWORDS


def win32_shim_symbols():
    if not SHIM_DIR.is_dir():
        return set()
    return declared_names_in(SHIM_DIR.rglob("*.h"))


# The GameSpy SDK is cut scope (see docs/porting/native-port-plan.md), so nothing links it and
# everything it declares is unresolved. Recognising those names from the fetched SDK's own headers
# rather than from a `^(gs|peer|qr2|...)` prefix regex is what moves `GenerateAuthA`,
# `PersistThink`, `NewGame` and the stats-connection cluster out of "Other / unclassified": they
# are ordinary-looking names that happen to be GameSpy's.
GAMESPY_INCLUDE_SUBDIRS = ("gamespy-src/include", "gamespy-src/include/gamespy")


def gamespy_symbols(deps_dir):
    if deps_dir is None:
        return set()
    headers = []
    for subdir in GAMESPY_INCLUDE_SUBDIRS:
        root = deps_dir / subdir
        if root.is_dir():
            headers.extend(root.rglob("*.h"))
    return declared_names_in(headers)


CATEGORY_PATTERNS = [
    # resources/gitinfo/gitinfo.cpp.in is configured at build time by git_watcher.cmake; this
    # project does not run it, so these are an artefact of the harness, not a port blocker.
    ("Generated gitinfo (build-time, not a blocker)", re.compile(
        r"^Git(Revision|Tag|ShortSHA1|CommitAuthorName|CommitTimeStamp|UncommittedChanges|"
        r"HasLocalChanges)$")),
    ("Third-party library not linked (lzhl, zlib)", re.compile(
        r"^(LZHL\w+|compress2?|uncompress|deflate\w*|inflate\w*|zlib\w*|crc32|adler32)$")),
    # `IID_IUnknown` / `IID_IBrowserDispatch`: the interface GUIDs the WOL/EABrowserDispatch
    # embedding references. On Windows they come out of the generated IDL stubs and uuid.lib.
    # Cut scope with the browser embedding itself, and a GUID constant either way, never code.
    # `_com_util::`/`_bstr_t` are MSVC's `comutil.h` helpers, reached from the same embedding.
    ("COM / OLE (browser embedding, cut scope)", re.compile(
        r"^(?:(?:IID|CLSID|LIBID|DIID)_\w+$|_com_util::|_bstr_t|_variant_t)")),
    ("Direct3D 8 / DirectX", re.compile(
        r"^(Direct3DCreate8|D3DX\w+|DirectDrawCreate\w*|DirectInput\w*|DirectSound\w*|"
        r"IDirect3D\w*)")),
    ("Miles Sound System", re.compile(r"^AIL_")),
    ("Bink video", re.compile(r"^Bink")),
    ("GameSpy SDK (cut scope, not linked)", re.compile(
        r"^(gs|ghttp|peer|qr2|sb|GT2|gt2|gp|ci|sc)[A-Z]\w*")),
    ("x86 assembly / MSVC intrinsics", re.compile(
        r"^(_Interlocked\w+|__rdtsc|_lrotl|_lrotr|_byteswap_\w+|__cpuid\w*|_BitScan\w+|"
        r"_mm_\w+|__debugbreak|_ReturnAddress)$")),
    ("STLport", re.compile(r"^_?_?stlp|^std::__\w+_STL", re.I)),
]


# DEFINE_KEY in Common/WellKnownKeys.h declares `TheKey_*` everywhere and defines them only in the
# translation unit that sets INSTANTIATE_WELL_KNOWN_KEYS. Macro-generated, so the source scan below
# cannot see them, and their cause depends entirely on what happened to that one file -- which is
# why they get their own lookup rather than a pattern.
WELL_KNOWN_KEYS_RE = re.compile(r"^TheKey_\w+$")
WELL_KNOWN_KEYS_MACRO = "INSTANTIATE_WELL_KNOWN_KEYS"


# `typeinfo for X`, `vtable for X`, `VTT for X`, `typeinfo name for X`, `non-virtual thunk to
# X::y()` -- the class, not the function, is what identifies these.
SYMBOL_OWNER_PREFIX_RE = re.compile(
    r"^(?:typeinfo(?: name)? for |vtable for |VTT for |construction vtable for |"
    r"(?:non-virtual|virtual) thunk to |guard variable for )")


def symbol_class(base):
    """The class a demangled symbol belongs to, or None if it is not a member of one."""
    name = SYMBOL_OWNER_PREFIX_RE.sub("", base).strip()
    if "::" not in name:
        return name if name != base else None
    return name.split("::", 1)[0]


@dataclasses.dataclass(frozen=True)
class Attribution:
    """Everything needed to say *why* a symbol is unresolved, gathered once per run."""

    win32_names: frozenset
    gamespy_names: frozenset
    uncompiled_names: frozenset
    unbuilt_layer_names: frozenset
    built_definition_names: frozenset
    excluded_backend_names: frozenset
    uncompiled_classes: frozenset
    unbuilt_layer_classes: frozenset
    well_known_keys_category: str


DISABLED_IF_CATEGORY = (
    "Defined in a built translation unit behind a disabled #if (build option / platform)")
EXCLUDED_BACKEND_CATEGORY = (
    "Defined only in a backend this configuration excludes (SDL2 / Cocoa)")
UNBUILT_LAYER_CATEGORY = "Defined in a layer not built here (renderer / audio)"


def categorise_symbol(mangled, demangled, attribution):
    base = demangled.split("(", 1)[0].strip().removesuffix("[abi:cxx11]")
    plain = mangled.lstrip("_")
    unqualified = base.rsplit("::", 1)[-1]
    for name, pattern in CATEGORY_PATTERNS:
        if pattern.search(mangled) or pattern.search(base) or pattern.search(plain):
            return name
    if WELL_KNOWN_KEYS_RE.match(plain):
        return attribution.well_known_keys_category
    if {mangled, plain, base} & attribution.win32_names:
        return "Win32 API"
    if {mangled, plain, base} & attribution.gamespy_names:
        return "GameSpy SDK (cut scope, not linked)"
    if base in attribution.uncompiled_names:
        return "Defined in a translation unit that failed to compile"
    # A name a *built* translation unit defines in its source text and that is still unresolved was
    # preprocessed away: an `#if defined(RTS_DEBUG)` or a `#ifdef _WIN32` this configuration does
    # not take. That is a build-configuration fact, not missing code, and it used to be filed as
    # "engine C++ not built at this level", which was wrong -- the file *was* built.
    if base in attribution.built_definition_names:
        return DISABLED_IF_CATEGORY
    # The unqualified fallback is only used here, where the file set is three files and the
    # namespace-versus-file mismatch is real: the backends define `Window_Create` inside
    # `namespace WWPlatform`, which the column-zero source scan sees unqualified.
    if attribution.excluded_backend_names & {base, unqualified}:
        return EXCLUDED_BACKEND_CATEGORY
    if base in attribution.unbuilt_layer_names:
        return UNBUILT_LAYER_CATEGORY
    # Compiler-generated symbols -- `typeinfo for X`, `vtable for X`, thunks -- and members the
    # source scan misses because their return type sits on the previous line, or because they are
    # generated by a macro. No text scan can find them by name, but the class they belong to has
    # methods defined somewhere, and that file is the answer.
    owner = symbol_class(base)
    if owner:
        if owner in attribution.uncompiled_classes:
            return "Defined in a translation unit that failed to compile"
        if owner in attribution.unbuilt_layer_classes:
            return UNBUILT_LAYER_CATEGORY
    if mangled.startswith("_Z") or "::" in demangled:
        return "Engine C++ not built at this level"
    return "Other / unclassified"


# A definition in this codebase starts at column zero: `void Foo::Bar(...)`, `int baz(...)`,
# `GameLogic *TheGameLogic = NULL;`.
FUNCTION_DEFINITION_RE = re.compile(
    r"^[A-Za-z_][\w\s\*&:<>,]*?\b([A-Za-z_]\w*(?:::[A-Za-z_~]\w*)*)\s*\(", re.M)
OBJECT_DEFINITION_RE = re.compile(
    r"^[A-Za-z_][\w\s\*&:<>,]*?\b([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*(?:=[^=]|;)", re.M)


def defined_names_in(sources):
    """Names -- functions and file-scope objects -- defined by the given translation units.

    A symbol can be unresolved because the file defining it failed to compile, or because that
    file belongs to a layer this build deliberately does not include. Those are different facts,
    and neither is "the code is missing". Telling them apart properly would need the object files
    that by definition do not exist, so this reads names out of the source text instead: coarse,
    but it never attributes a cause to a symbol none of those files mentions.
    """
    names = set()
    for source in sources:
        try:
            text = source.read_text(errors="replace")
        except OSError:
            continue
        names.update(FUNCTION_DEFINITION_RE.findall(text))
        names.update(OBJECT_DEFINITION_RE.findall(text))
    return names


# `void W3DTankDraw::doDrawModule(...)`, `W3DTankDraw::~W3DTankDraw()` -- a member definition, at
# any indentation, but never a call: a qualified call is a statement and ends in a semicolon.
CLASS_MEMBER_DEFINITION_RE = re.compile(
    r"^[ \t]*(?:[\w:<>&*\[\]]+[ \t]+)*([A-Za-z_]\w*)::~?[A-Za-z_]\w*[ \t]*\([^;]*$", re.M)


def defined_classes_in(sources):
    """Classes with at least one member defined by the given translation units."""
    names = set()
    for source in sources:
        try:
            text = source.read_text(errors="replace")
        except OSError:
            continue
        names.update(CLASS_MEMBER_DEFINITION_RE.findall(text))
    return names


# Sources the probe and this build both exclude from every target: the opt-in SDL2 window backend,
# the mutually exclusive memory implementation, and the Objective-C++ Cocoa backend (never a .cpp,
# so no target ever listed it). Whatever they alone define is unresolved because of the
# configuration, which is a different statement from "this layer is not built".
EXCLUDED_BACKEND_DIRS = ("Core", "Generals", "GeneralsMD")


def excluded_backend_sources():
    wanted = set(npt.probe.OPTIONAL_BACKENDS) | set(npt.probe.EXCLUSIVE_ALTERNATIVES)
    found = []
    for directory in EXCLUDED_BACKEND_DIRS:
        root = REPO_ROOT / directory
        if not root.is_dir():
            continue
        found.extend(p for p in root.rglob("*.cpp") if p.name in wanted)
        found.extend(root.rglob("*.mm"))
    return found


def well_known_keys_owner():
    """The translation unit that instantiates the `TheKey_*` Dict keys, if any."""
    for directory in EXCLUDED_BACKEND_DIRS:
        root = REPO_ROOT / directory
        for source in root.rglob("*.cpp"):
            if WELL_KNOWN_KEYS_MACRO in source.read_text(errors="replace"):
                return source
    return None


def well_known_keys_category(owner, failed, built_sources):
    """Why `TheKey_*` is unresolved, named after the one file that decides it.

    Before level 3 these 104 symbols were reported as "well-known Dict keys", which said nothing:
    the only thing that matters is what happened to the single translation unit that instantiates
    them. If it built, they resolve and this is never used.
    """
    if owner is None:
        return f"Well-known Dict keys: no translation unit sets {WELL_KNOWN_KEYS_MACRO}"
    relative = owner.relative_to(REPO_ROOT)
    if owner in failed:
        return f"Well-known Dict keys: `{relative}` failed to compile"
    if owner not in built_sources:
        return f"Well-known Dict keys: `{relative}` is not in the built levels"
    return f"Well-known Dict keys: `{relative}` built but did not define them"


# ---------------------------------------------------------------------------------------------
# Steps
# ---------------------------------------------------------------------------------------------

# The GameSpy SDK's own headers include their siblings unqualified (`gt2/gt2.h` does
# `#include "gscommon.h"`), so the directory the probe puts on the path, `gamespy-src/include`, is
# not enough: `include/gamespy` has to be there too. On Windows the MSVC build never noticed
# because it adds the directory anyway. Adding it here rather than in the probe keeps the probe's
# published baselines stable; the divergence measurement is unaffected because the probe pass in
# this script uses the same include set as the build it is compared against.
EXTRA_DEP_INCLUDES = ["gamespy-src/include/gamespy"]


def includes_for(target, deps_dir, with_shims):
    includes = npt.target_includes(target, deps_dir)
    extra = [str(deps_dir / d) for d in EXTRA_DEP_INCLUDES if (deps_dir / d).is_dir()]
    includes = extra + includes
    if with_shims:
        # Shims first, so they win over anything else on the path -- the ordering the probe's
        # shimmed mode uses.
        includes.insert(0, str(SHIM_DIR))
    return includes


def write_manifests(targets, manifest_dir, deps_dir, skip=None, with_shims=False):
    """One CMake fragment per library. `skip` maps target name -> set of source paths to omit."""
    skip = skip or {}
    manifest_dir.mkdir(parents=True, exist_ok=True)
    written = {}
    for target in targets:
        sources = [s for s in npt.target_sources(target) if s not in skip.get(target.name, set())]
        includes = includes_for(target, deps_dir, with_shims)
        lines = [
            f"# Generated by scripts/native-build.py for {target.name}. Do not edit.",
            "set(NATIVE_SOURCES",
            *[f'    "{s}"' for s in sources],
            ")",
            "set(NATIVE_INCLUDES",
            *[f'    "{i}"' for i in includes],
            ")",
        ]
        if target.defines:
            lines += ["set(NATIVE_DEFINES", *[f'    "{d}"' for d in target.defines], ")"]
        (manifest_dir / f"{slug(target.name)}.cmake").write_text("\n".join(lines) + "\n")
        written[target.name] = sources
    return written


# ---------------------------------------------------------------------------------------------
# Third-party libraries the engine actually calls into
# ---------------------------------------------------------------------------------------------

# `LZHLCompress`, `compress2` and friends were reported as unresolved for one reason only: nothing
# linked lzhl or zlib. That is a dependency, not port work, and counting it as a blocker inflated
# every figure quoted from this build by nine symbols.
#
# lzhl is built from the sources cmake/lzhl.cmake pins and scripts/ci/fetch-probe-deps.sh fetches,
# with cmake/lzhl.cmake's own file list (Lzhl_tcp.cpp is commented out there, so it is out here).
# It is a *support* archive: deliberately not part of the objects/translation-unit denominators,
# which measure the engine's own portability and must stay comparable with the probe's.
LZHL_SUBDIR = "lzhl-src/CompLibHeader"
LZHL_SOURCES = ("Huff.cpp", "Lz.cpp", "Lzhl.cpp")
LZHL_SLUG = "thirdparty_lzhl"

# zlib is a system package (`zlib1g-dev`), so it is linked with -lz rather than built.
ZLIB_CANDIDATES = (
    "/lib/x86_64-linux-gnu/libz.so.1",
    "/usr/lib/x86_64-linux-gnu/libz.so.1",
    "/usr/lib/aarch64-linux-gnu/libz.so.1",
    "/usr/lib/libz.dylib",
)


def write_lzhl_manifest(manifest_dir, deps_dir):
    """CMake fragment for lzhl. Returns its slug, or None when the sources are not provisioned."""
    root = deps_dir / LZHL_SUBDIR
    sources = [root / name for name in LZHL_SOURCES if (root / name).is_file()]
    if len(sources) != len(LZHL_SOURCES):
        return None
    manifest_dir.mkdir(parents=True, exist_ok=True)
    lines = [
        "# Generated by scripts/native-build.py for lzhl. Do not edit.",
        "set(NATIVE_SOURCES",
        *[f'    "{s}"' for s in sources],
        ")",
        "set(NATIVE_INCLUDES",
        f'    "{root}"',
        f'    "{root.parent}"',
        # The project force-includes Utility/CppMacros.h into every target, this one included.
        f'    "{REPO_ROOT / "Dependencies" / "Utility"}"',
        ")",
    ]
    (manifest_dir / f"{LZHL_SLUG}.cmake").write_text("\n".join(lines) + "\n")
    return LZHL_SLUG


def zlib_library():
    return next((pathlib.Path(p) for p in ZLIB_CANDIDATES if pathlib.Path(p).exists()), None)


def configure(build_dir, manifest_dir, targets, extra_slugs=()):
    slugs = ";".join([slug(t.name) for t in targets] + list(extra_slugs))
    cmd = [
        "cmake", "-S", str(NATIVE_CMAKE_DIR), "-B", str(build_dir),
        f"-DCMAKE_CXX_COMPILER={CXX}",
        f"-DNATIVE_MANIFEST_DIR={manifest_dir}",
        f"-DNATIVE_TARGETS={slugs}",
        "-DCMAKE_BUILD_TYPE=Debug",
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
    ]
    if shutil.which("ninja"):
        cmd += ["-G", "Ninja"]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        sys.stderr.write(proc.stdout + proc.stderr)
        raise SystemExit("cmake configure failed")


def build(build_dir, jobs):
    """Build everything, keeping going past failures.

    Returns (failed_sources, first diagnostic per failed source).
    """
    cmd = ["cmake", "--build", str(build_dir), "--parallel", str(jobs)]
    # Keep going so one bad translation unit does not hide the other 700.
    cmd += ["--", "-k", "0"] if shutil.which("ninja") else ["--", "-k"]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    log = proc.stdout + proc.stderr

    # Map object paths back to translation units through the compile database.
    obj_to_source = {}
    db_path = build_dir / "compile_commands.json"
    if db_path.is_file():
        for entry in json.loads(db_path.read_text()):
            command = entry.get("command", "") or " ".join(entry.get("arguments", []))
            match = re.search(r"-o\s+(\S+\.o)", command)
            if match:
                obj_to_source[match.group(1)] = entry["file"]

    # Which object files exist, rather than which lines the build tool printed: Ninja announces a
    # failure as "FAILED: <obj>" and GNU make as "make[3]: *** [<obj>] Error 1", so scraping one
    # generator's wording silently reports zero failures under the other. A missing object is a
    # failure under both. (macOS has no ninja by default, which is where this first showed up: 87
    # translation units produced no object and the run still claimed "0 failures".)
    failed = set()
    for obj, source in obj_to_source.items():
        obj_path = pathlib.Path(obj)
        if not obj_path.is_absolute():
            obj_path = build_dir / obj_path
        if not obj_path.is_file():
            failed.add(pathlib.Path(source))

    diagnostics = {}
    lines = log.splitlines()
    for source in failed:
        for line in lines:
            if source.name not in line:
                continue
            if ": error:" in line or ": fatal error:" in line:
                diagnostics[source] = line.split(": error:", 1)[-1] \
                    .split(": fatal error:", 1)[-1].strip()
                break
    return failed, diagnostics


def probe_sources(sources, target_by_source, deps_dir, jobs, with_shims=False):
    """Run the probe's -fsyntax-only check over the same translation units, for comparison."""
    flags = [f for f in npt.probe.CLANG_FLAGS]

    def run(source):
        target = target_by_source[source]
        includes = includes_for(target, deps_dir, with_shims)
        cmd = [CXX, *flags]
        cmd += [f"-D{d}" for d in target.defines]
        cmd += [f"-I{i}" for i in includes]
        cmd.append(str(source))
        proc = subprocess.run(cmd, capture_output=True, text=True)
        return source, proc.returncode == 0

    results = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        for source, ok in pool.map(run, sources):
            results[source] = ok
    return results


NM = os.environ.get("NM", "nm")

# Libraries a native build may legitimately link against. What they export is resolved, not a
# blocker, and must not show up in the report as one.
SYSTEM_LIBRARIES = [
    "/lib/x86_64-linux-gnu/libc.so.6",
    "/lib/x86_64-linux-gnu/libm.so.6",
    "/lib/x86_64-linux-gnu/libpthread.so.0",
    "/lib/x86_64-linux-gnu/libdl.so.2",
    "/lib/x86_64-linux-gnu/libgcc_s.so.1",
    "/lib/x86_64-linux-gnu/libstdc++.so.6",
    "/usr/lib/x86_64-linux-gnu/libstdc++.so.6",
]

# Supplied by the CRT startup files or the unwinder rather than by a library nm can be pointed at.
TOOLCHAIN_SYMBOL_RE = re.compile(
    r"^(__gmon_start__|_ITM_\w+|_Unwind_\w+|__cxa_\w+|__gxx_\w+|__dso_handle|"
    r"_GLOBAL_OFFSET_TABLE_|__stack_chk_\w+)$")


def nm_symbols(path, *flags):
    proc = subprocess.run([NM, *flags, str(path)], capture_output=True, text=True)
    names = set()
    for line in proc.stdout.splitlines():
        parts = line.split()
        if parts:
            # Dynamic symbol tables carry a version suffix (`acosf@@GLIBC_2.2.5`), which is not
            # part of the name an archive references.
            names.add(parts[-1].split("@", 1)[0])
    return names


def system_symbols():
    names = set()
    for path in SYSTEM_LIBRARIES:
        if pathlib.Path(path).exists():
            names |= nm_symbols(path, "-D", "--defined-only", "--extern-only")
    return names


def unresolved_symbols(archives, support_archives=(), extra_libraries=()):
    """Symbols the engine archives reference that nothing linked into the binary defines.

    Read out of the archives with `nm` rather than scraped from linker diagnostics: ld demangles
    in its messages, which throws away the mangled name, and it reports only what its own archive
    ordering happened to require.

    `support_archives` (lzhl) and `extra_libraries` (zlib) contribute definitions only: they are
    dependencies, so what *they* reference is not this measurement's business.
    """
    defined = set()
    referenced_from = collections.defaultdict(set)
    for archive in list(archives) + list(support_archives):
        defined |= nm_symbols(archive, "--defined-only", "--extern-only")
    for library in extra_libraries:
        defined |= nm_symbols(library, "-D", "--defined-only", "--extern-only")
    for archive in archives:
        for name in nm_symbols(archive, "--undefined-only"):
            referenced_from[name].add(archive.stem)

    system = system_symbols()
    return {
        name: sorted(archives_using) for name, archives_using in referenced_from.items()
        if name not in defined and name not in system and not TOOLCHAIN_SYMBOL_RE.match(name)
    }


def link_probe(build_dir, archives, support_archives=(), link_zlib=False):
    """Run the linker over every archive, so "no linker has ever run" stops being true.

    `--whole-archive` forces every object in, since a trivial main() otherwise pulls in nothing,
    and `--warn-unresolved-symbols` lets it produce a binary anyway so the outcome is a result
    rather than a wall of errors. The third-party archives follow without `--whole-archive`: they
    are dependencies, so only what the engine actually calls needs to come in.
    """
    stub = build_dir / "native_link_probe.cpp"
    stub.write_text(
        "// Generated by scripts/native-build.py: an entry point so the linker has something to\n"
        "// anchor to. The archives are pulled in whole, so what matters is what they reference.\n"
        "int main() { return 0; }\n"
    )
    out = build_dir / "native_link_probe"
    if sys.platform == "darwin":
        # ld64 has none of the GNU spellings: -all_load is --whole-archive, -undefined warning is
        # --warn-unresolved-symbols, libSystem carries dlopen and pthreads, and -lstdc++ is a
        # deprecated shim rather than the C++ library. Without this branch the link cannot run on
        # macOS at all and the result is a flag error rather than a symbol list.
        cmd = [
            CXX, "-std=c++20", "-o", str(out), str(stub),
            "-Wl,-undefined,warning", "-Wl,-all_load",
            *[str(a) for a in archives], *[str(a) for a in support_archives],
            "-lc++", "-lm",
        ]
    else:
        cmd = [
            CXX, "-std=c++20", "-o", str(out), str(stub),
            "-Wl,--warn-unresolved-symbols",
            "-Wl,--whole-archive", *[str(a) for a in archives], "-Wl,--no-whole-archive",
            *[str(a) for a in support_archives],
            "-lstdc++", "-lm", "-lpthread", "-ldl",
        ]
    if link_zlib:
        cmd.append("-lz")
    proc = subprocess.run(cmd, capture_output=True, text=True)
    return proc.returncode == 0, out.exists(), proc.stdout + proc.stderr


def demangle(names):
    if not names:
        return {}
    tool = (shutil.which("llvm-cxxfilt-14") or shutil.which("llvm-cxxfilt")
            or shutil.which("c++filt"))
    if not tool:
        return {n: n for n in names}
    ordered = list(names)
    proc = subprocess.run([tool], input="\n".join(ordered), capture_output=True, text=True)
    lines = proc.stdout.splitlines()
    if len(lines) != len(ordered):
        return {n: n for n in ordered}
    return dict(zip(ordered, lines))


# ---------------------------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------------------------

def render_report(data, examples):
    compiled = data["compiled"]
    lines = [
        "# Native 64-bit build — objects and link",
        "",
        "Produced by `scripts/native-build.py`. Unlike every other number in `docs/porting/`,",
        "these come from real object files and a real linker invocation, not from",
        "`clang++ -fsyntax-only`.",
        "",
        f"Toolchain: `{data['compiler']}`, target `{data['host']}`, "
        f"levels built: {', '.join(str(x) for x in data['levels'])}.",
        "",
        ("Mode: **shimmed** — `scripts/native-port-shims/` supplies declaration-only stand-ins "
         "for the Win32 headers, so a missing platform layer shows up as an undefined symbol "
         "rather than as a failed compile. That is the point: it moves the blockers from §1 to "
         "§3, where they can be counted individually."
         if data["with_shims"] else
         "Mode: **native** — nothing stands in for the Windows SDK, so anything that includes a "
         "Win32 header fails to compile and never reaches the linker."),
        "",
        "## 1. Compilation",
        "",
        "| Library | Objects produced | Translation units | Probe-clean |",
        "|---|---:|---:|---:|",
    ]
    for name, stats in compiled.items():
        lines.append(
            f"| `{name}` | {stats['objects']} | {stats['total']} | {stats['probe_clean']} |")
    lines += [
        f"| **Total** | **{data['objects']}** | **{data['translation_units']}** | "
        f"**{data['probe_clean']}** |",
        "",
    ]

    if data["compile_failures"]:
        lines += [
            f"{len(data['compile_failures'])} translation units produced no object file:",
            "",
            "| Translation unit | First diagnostic |",
            "|---|---|",
        ]
        for path, diagnostic in data["compile_failures"].items():
            lines.append(f"| `{path}` | `{diagnostic.replace('|', chr(92) + '|')[:120]}` |")
        lines.append("")

    divergence = data["divergence"]
    lines += [
        "## 2. How much the probe over-reports",
        "",
        f"**{len(divergence['probe_clean_compile_failed'])} translation units that the probe "
        f"calls clean fail to compile**, out of {data['probe_clean']} probe-clean units "
        f"({len(divergence['probe_clean_compile_failed']) * 100 // max(data['probe_clean'], 1)}%). "
        "These are the codegen-class failures `-fsyntax-only` cannot see.",
        "",
    ]
    if divergence["probe_clean_compile_failed"]:
        lines += ["| Translation unit |", "|---|"]
        lines += [f"| `{p}` |" for p in divergence["probe_clean_compile_failed"]]
        lines.append("")
    if divergence["probe_failed_compile_ok"]:
        lines += [
            f"The reverse case, {len(divergence['probe_failed_compile_ok'])} units the probe "
            "rejects but that compile here, exists because the probe and this build differ in "
            "nothing but emission — treat any entry as a bug in one of the two.",
            "",
        ]
        lines += [f"- `{p}`" for p in divergence["probe_failed_compile_ok"]] + [""]

    third_party = ", ".join("`" + name + "`" for name in data["third_party_linked"]) or "none"
    lines += [
        "## 3. Undefined symbols",
        "",
        f"The {data['archives']} archives were linked into one binary with `--whole-archive`, "
        f"plus the third-party libraries the engine calls into: {third_party} "
        f"(binary produced: {'yes' if data['link_binary_produced'] else 'no'}; clean link: "
        f"{'yes' if data['link_clean'] else 'no'}). **{data['undefined_total']} distinct symbols "
        "are unresolved** once libc, libstdc++, libm, libpthread and the CRT/unwinder symbols are "
        "discounted. The full categorised list is in the JSON output; examples follow each count.",
        "",
        "| Cause | Symbols |",
        "|---|---:|",
    ]
    for category, count in data["undefined_by_category"].items():
        lines.append(f"| {category} | {count} |")
    lines.append("")

    for category, names in examples.items():
        lines += [f"### {category}", ""]
        lines += [f"- `{n}`" for n in names]
        if data["undefined_by_category"][category] > len(names):
            lines.append(f"- …and {data['undefined_by_category'][category] - len(names)} more")
        lines.append("")

    lines += [
        "## Reproducing",
        "",
        "```sh",
        "bash scripts/ci/fetch-probe-deps.sh",
        f"python3 scripts/native-build.py {' '.join(f'--level {l}' for l in data['levels'])}"
        + (" --with-shims" if data["with_shims"] else "")
        + " --report docs/porting/native-build-report.md --json native-build.json",
        "```",
        "",
    ]
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--level", action="append", type=int, choices=sorted(LEVELS),
                        help="build order level to include; repeatable (default: 1)")
    parser.add_argument("--build-dir", default=str(REPO_ROOT / "build" / "native"))
    parser.add_argument("--deps-dir", default=str(npt.DEFAULT_DEPS_DIR))
    parser.add_argument("--report")
    parser.add_argument("--json", dest="json_out")
    parser.add_argument("--with-shims", action="store_true",
                        help="put scripts/native-port-shims/ on the include path, as the probe's "
                             "shimmed mode does")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("--examples", type=int, default=15,
                        help="how many example symbols to list per category")
    args = parser.parse_args()

    levels = sorted(set(args.level or [1]))
    wanted = [name for level in levels for name in LEVELS[level]]
    # ALL_TARGETS, not npt.TARGETS: level 3's device and entry-point libraries live in the probe's
    # RENDERER_TARGETS list, which is where the probe keeps everything never compiled off Windows.
    targets = [t for t in ALL_TARGETS if t.name in wanted]
    missing = set(wanted) - {t.name for t in targets}
    if missing:
        raise SystemExit(f"probe has no target definition for: {', '.join(sorted(missing))}")

    # Absolute: the generated manifests are included by a CMake project in another directory, and
    # the compile database is read back from a different working directory again.
    deps_dir = pathlib.Path(args.deps_dir).resolve()
    if not deps_dir.is_dir():
        raise SystemExit(
            f"{deps_dir} does not exist. Run scripts/ci/fetch-probe-deps.sh first: without the "
            "fetched SDK headers the result is not comparable with the probe baselines.")

    build_dir = pathlib.Path(args.build_dir).resolve()
    manifest_dir = build_dir / "manifests"

    print("== generating manifests")
    sources_by_target = write_manifests(targets, manifest_dir, deps_dir,
                                        with_shims=args.with_shims)
    target_by_source = {s: t for t in targets for s in sources_by_target[t.name]}
    all_sources = list(target_by_source)

    lzhl_slug = write_lzhl_manifest(manifest_dir, deps_dir)
    if lzhl_slug is None:
        print("   warning: lzhl sources are not provisioned; LZHL* symbols will stay unresolved")
    zlib_path = zlib_library()
    if zlib_path is None:
        print("   warning: no libz found; compress2/uncompress will stay unresolved")
    extra_slugs = [lzhl_slug] if lzhl_slug else []

    print(f"== compiling {len(all_sources)} translation units")
    configure(build_dir, manifest_dir, targets, extra_slugs)
    failed, compile_diagnostics = build(build_dir, args.jobs)
    # The third-party archive is built alongside but is not part of the measurement, so a failure
    # there is a provisioning problem rather than a translation unit this port cannot compile.
    support_failed = {s for s in failed if s not in target_by_source}
    failed = {s for s in failed if s in target_by_source}
    if support_failed:
        print(f"   warning: {len(support_failed)} third-party sources failed to compile")
    print(f"   {len(all_sources) - len(failed)} objects, {len(failed)} failures")

    print("== re-running the probe over the same translation units")
    probe_results = probe_sources(all_sources, target_by_source, deps_dir, args.jobs,
                                  with_shims=args.with_shims)

    probe_clean_compile_failed = sorted(
        str(s.relative_to(REPO_ROOT)) for s in failed if probe_results.get(s))
    probe_failed_compile_ok = sorted(
        str(s.relative_to(REPO_ROOT)) for s in all_sources
        if not probe_results.get(s) and s not in failed)

    # Second pass without the failures, so every library yields an archive to link.
    print("== re-building without the failed translation units")
    skip = collections.defaultdict(set)
    for source in failed:
        target = target_by_source.get(source)
        if target:
            skip[target.name].add(source)
    # A library every one of whose translation units failed cannot become an archive at all --
    # CMake rejects a target with no sources -- so it is dropped from the link with its name
    # recorded, rather than being silently absent from the archive count.
    link_targets = [t for t in targets
                    if any(s not in failed for s in sources_by_target[t.name])]
    empty_targets = [t.name for t in targets if t not in link_targets]
    if empty_targets:
        print(f"   no archive for: {', '.join(empty_targets)} (every unit failed)")
    # A stale archive from an earlier run, or from a target just dropped, would be picked up by the
    # rglob below and linked, so the link would not describe this build.
    for stale in build_dir.rglob("*.a"):
        stale.unlink()
    write_manifests(link_targets, manifest_dir, deps_dir, skip=skip, with_shims=args.with_shims)
    configure(build_dir, manifest_dir, link_targets, extra_slugs)
    second_failed, _ = build(build_dir, args.jobs)
    second_failed = {s for s in second_failed if s in target_by_source}
    if second_failed:
        print(f"   warning: {len(second_failed)} further failures in the second pass")

    all_archives = sorted(build_dir.rglob("*.a"))
    support_archives = [a for a in all_archives if a.stem.endswith(LZHL_SLUG)]
    archives = [a for a in all_archives if a not in support_archives]
    print(f"== linking {len(archives)} archives "
          f"(+{len(support_archives)} third-party, zlib: {'yes' if zlib_path else 'no'})")
    link_ok, binary_produced, _ = link_probe(build_dir, archives, support_archives,
                                             link_zlib=zlib_path is not None)
    extra_libraries = [zlib_path] if zlib_path else []
    symbols = unresolved_symbols(archives, support_archives, extra_libraries)
    demangled = demangle(list(symbols))

    built_sources = [s for s in all_sources if s not in failed]
    # Everything the probe knows about but this build does not include: the renderer and audio
    # layers, plus any level not selected. A symbol they define is out of scope here, not missing.
    unbuilt = [t for t in ALL_TARGETS if t not in targets]
    unbuilt_sources = [s for t in unbuilt for s in npt.target_sources(t)]
    keys_owner = well_known_keys_owner()
    attribution = Attribution(
        win32_names=frozenset(win32_shim_symbols()),
        gamespy_names=frozenset(gamespy_symbols(deps_dir)),
        uncompiled_names=frozenset(defined_names_in(failed)),
        unbuilt_layer_names=frozenset(defined_names_in(unbuilt_sources)),
        built_definition_names=frozenset(defined_names_in(built_sources)),
        excluded_backend_names=frozenset(defined_names_in(excluded_backend_sources())),
        uncompiled_classes=frozenset(defined_classes_in(failed)),
        # A class whose members the failed files define is attributed to them first, so a class
        # split across a built and an unbuilt file is not reported as merely out of scope.
        unbuilt_layer_classes=frozenset(defined_classes_in(unbuilt_sources)),
        well_known_keys_category=well_known_keys_category(keys_owner, failed, target_by_source),
    )

    by_category = collections.Counter()
    per_category_names = collections.defaultdict(list)
    for symbol in sorted(symbols):
        name = demangled.get(symbol, symbol)
        category = categorise_symbol(symbol, name, attribution)
        by_category[category] += 1
        per_category_names[category].append(name)

    compiled = {}
    for target in targets:
        target_sources = sources_by_target[target.name]
        compiled[target.name] = {
            "objects": sum(1 for s in target_sources if s not in failed),
            "total": len(target_sources),
            "probe_clean": sum(1 for s in target_sources if probe_results.get(s)),
        }

    version = subprocess.run([CXX, "--version"], capture_output=True, text=True) \
        .stdout.splitlines()[0]
    clang_major = next((m.group(1) for m in [re.search(r"version (\d+)", version)] if m), None)
    data = {
        "compiler": version,
        # The counts are compiler-version dependent, so the baseline check refuses to compare
        # results measured with different majors.
        "clang_major": clang_major,
        "host": subprocess.run([CXX, "-dumpmachine"], capture_output=True,
                               text=True).stdout.strip(),
        "levels": levels,
        "with_shims": args.with_shims,
        "compiled": compiled,
        "translation_units": len(all_sources),
        "objects": len(all_sources) - len(failed),
        "probe_clean": sum(1 for s in all_sources if probe_results.get(s)),
        "link_clean": link_ok,
        "link_binary_produced": binary_produced,
        "archives": len(archives),
        # Libraries with no archive at all: every translation unit failed, so the link cannot see
        # them. Tracked separately because "one fewer archive" otherwise looks like progress.
        "libraries_without_archive": sorted(empty_targets),
        # Linked, but deliberately outside every other figure here: dependencies, not translation
        # units whose portability is being measured.
        "third_party_linked": sorted(
            [a.stem for a in support_archives] + (["z (system)"] if zlib_path else [])),
        "undefined_total": len(symbols),
        "undefined_by_category": dict(by_category.most_common()),
        # The categorised list is the deliverable, so it goes in the machine-readable output in
        # full; the report only quotes examples.
        "undefined_symbols": {
            category: sorted(per_category_names[category]) for category in by_category
        },
        "compile_failures": {
            str(source.relative_to(REPO_ROOT)): compile_diagnostics.get(source, "")
            for source in sorted(failed)
        },
        "divergence": {
            "probe_clean_compile_failed": probe_clean_compile_failed,
            "probe_failed_compile_ok": probe_failed_compile_ok,
        },
    }

    examples = collections.OrderedDict(
        (category, per_category_names[category][:args.examples])
        for category, _ in by_category.most_common())

    if args.report:
        pathlib.Path(args.report).write_text(render_report(data, examples))
        print(f"== wrote {args.report}")
    if args.json_out:
        pathlib.Path(args.json_out).write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")
        print(f"== wrote {args.json_out}")

    print(f"objects {data['objects']}/{data['translation_units']}, "
          f"probe-clean {data['probe_clean']}, "
          f"probe-clean-but-uncompilable {len(probe_clean_compile_failed)}, "
          f"undefined symbols {data['undefined_total']}")


if __name__ == "__main__":
    main()
