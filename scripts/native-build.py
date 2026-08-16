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
    python3 scripts/native-build.py [--level 1|2|3|4] [--with-shims]
                                    [--report docs/porting/native-build-report.md]
                                    [--json out.json] [--jobs N] [--build-dir DIR]
"""

import argparse
import collections
import concurrent.futures
import dataclasses
import functools
import glob
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

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
# to a named file.
#
# Level 4 adds the renderer and audio libraries for exactly the same reason, one level up. Keeping
# them out was defended as "the renderer seam's own measurement", but the level-1-2-3 link put 272
# of its 457 unresolved symbols in "defined in a layer not built here (renderer / audio)" -- 60% of
# the total was a statement about this script's scope rather than about the code. A number that
# large cannot be read as portability, and no honest link attempt is possible while the layer that
# defines those symbols is absent. Building it converts each of them into a resolved symbol or a
# compile failure attributable to a named file, which is what the level-3 comment above claims as
# the whole point of the exercise.
#
# It is emphatically not expected to compile: these translation units are the D3D8, DirectSound and
# WinInet consumers, the code least likely to build off Windows. New compile failures here are the
# deliverable, not a regression. Level 4 has its own baseline file
# (`native-build-shimmed-level1-2-3-4.json`), so the smaller build's ratchet is untouched.
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
    4: [
        "Core/Libraries/Source/WWVegas/WW3D2",
        "Core/Libraries/Source/WWVegas/WWAudio",
        "Core/Libraries/Source/WWVegas/WWDownload",
        "GeneralsMD/Code/Libraries/Source/WWVegas",
    ],
}

# Every target the probe knows about, in the probe's own definitions. Levels index into this.
ALL_TARGETS = npt.TARGETS + npt.RENDERER_TARGETS

# The build configurations, spelled the way the real build spells them.
#
# `debug` is `RTS_BUILD_OPTION_DEBUG=ON` in cmake/config-build.cmake, which is the only
# configuration in which the engine's own `DEBUG_ASSERTCRASH`/`DEBUG_LOG` guards expand to
# anything: Debug.h derives `DEBUG_CRASHING` and `DEBUG_LOGGING` from `RTS_DEBUG` unless
# cmake/config-debug.cmake's DEFAULT is overridden, and WWDebug's `WWASSERT` derives from
# `WWDEBUG`. Measuring it is the point: every native figure published before it came from a build
# in which no assertion could fire, so no assertion had ever been compiled either.
#
# `release` deliberately adds nothing rather than adding the `RTS_RELEASE NDEBUG` the real build's
# else-branch adds. That is what every checked-in baseline was measured with, and changing it would
# move the release ratchet's numbers for a reason unrelated to any source change. It is recorded
# here as a known difference from the real build rather than silently fixed: see
# docs/porting/native-debug-build.md.
CONFIG_DEFINES = {
    "release": (),
    "debug": ("RTS_DEBUG", "WWDEBUG", "DEBUG"),
}


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
    # OpenAL is what the audio backend is implemented over. Unresolved `al*`/`alc*` means no
    # libopenal was found to link, which is a provisioning fact about the box, not port work --
    # and it is the backend's own dependency, never an engine call site.
    ("OpenAL (not linked here)", re.compile(r"^(al[A-Z]\w*|alc[A-Z]\w*)$")),
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
    # libavcodec/libavformat/libavutil/libswscale, the video playback backend
    # (GameEngineDevice/.../FFmpegFile.cpp). Present as headers, never linked here, so every
    # `av_*`/`avcodec_*`/`sws_*` call is unresolved: a missing link line, not a port blocker.
    ("FFmpeg (not linked here)", re.compile(
        r"^(av_|avcodec_|avformat_|avio_|avutil_|swr_|sws_|swscale_)")),
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
    if WELL_KNOWN_KEYS_RE.match(plain):
        return attribution.well_known_keys_category
    # EVIDENCE FIRST, NAME PATTERN LAST. A vendor prefix says what a symbol *is*; a file in this
    # tree that defines it says what would resolve it, and the second answer outranks the first.
    # `D3DXFilterTexture` and `D3DXAssembleShader` are the case that made this an ordering bug
    # rather than a preference: WW3D2 defines both, so at levels 1-3 -- where WW3D2 is not built --
    # the `D3DX\w+` pattern filed them as "Direct3D 8 / DirectX", whose pile is
    # `no-definition-anywhere`, i.e. remaining port work, when the definition is right there in a
    # layer this level selection leaves out. Anything else that shares a vendor prefix while living
    # in an unbuilt layer, a translation unit that failed to compile, or a disabled `#if` was
    # misfiled the same way; putting the four evidence lookups above the patterns fixes the whole
    # class, and scripts/native-build-categorise-test.py pins it.
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
    for name, pattern in CATEGORY_PATTERNS:
        if pattern.search(mangled) or pattern.search(base) or pattern.search(plain):
            return name
    if {mangled, plain, base} & attribution.win32_names:
        return "Win32 API"
    if {mangled, plain, base} & attribution.gamespy_names:
        return "GameSpy SDK (cut scope, not linked)"
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


def excluded_backend_sources(chosen=None):
    """`chosen` is the window backend this build compiled, which is therefore not excluded."""
    wanted = set(npt.probe.OPTIONAL_BACKENDS) | set(npt.probe.EXCLUSIVE_ALTERNATIVES)
    found = []
    for directory in EXCLUDED_BACKEND_DIRS:
        root = REPO_ROOT / directory
        if not root.is_dir():
            continue
        found.extend(p for p in root.rglob("*.cpp") if p.name in wanted)
        found.extend(root.rglob("*.mm"))
    return [p for p in found if p != chosen]


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
# Piles: how far each unresolved symbol is from an executable
# ---------------------------------------------------------------------------------------------

# The categories above say *what* a symbol is. They do not say what would make it go away, and the
# two are routinely confused: FFmpeg's 29 symbols and the renderer layer's 272 (before level 4)
# read as port work while both were only "the build does not include that". Level 4 settled the
# renderer case by building the layer; these piles settle the rest by naming, for every remaining
# symbol, the thing that resolves it.
#
# Only `no-definition-anywhere` is remaining port work. Everything else is a link line, a cut, or a
# build-configuration fact.
PILE_LIBRARY = "library-not-linked"
PILE_CUT = "cut-scope-not-linked"
PILE_COMPILE = "compile-blocked"
PILE_HARNESS = "harness-artefact"
PILE_PORT = "no-definition-anywhere"

PILE_MEANING = {
    PILE_LIBRARY: "A library defines it and this configuration links no such library. A link line, "
                  "not port work.",
    PILE_CUT: "A library defines it and this project will never link that library, because the "
              "feature is cut scope. Goes away by excising the call sites, not by defining it.",
    PILE_COMPILE: "An in-tree translation unit defines it in its source text but that unit does "
                  "not compile natively yet. The definition exists; the file is the blocker.",
    PILE_HARNESS: "An artefact of how this harness is configured: a build-time generated "
                  "definition it does not generate, one a disabled `#if` removed, or one in a "
                  "layer this level selection does not build.",
    PILE_PORT: "Nothing in the repository, the provisioned dependencies or a linkable library "
               "defines it. This is the remaining port work.",
}


@dataclasses.dataclass(frozen=True)
class Provider:
    """A library that defines some of the unresolved symbols, and why nothing links it here.

    `sources` and `headers` are the *evidence*: globs, repo-relative or relative to the provisioned
    dependency tree, whose definitions (or, for a declaration-only SDK, declarations) account for
    the symbols. They are provisioned by `scripts/ci/fetch-probe-deps.sh`, so the attribution is
    reproducible on any box. `libraries` is an extra check against a real shared library when the
    measuring machine happens to have one; it is deliberately *not* what decides the pile, because
    then the answer would depend on the box.
    """

    key: str
    label: str
    pile: str
    reason: str
    owner: str
    sources: tuple = ()
    headers: tuple = ()
    libraries: tuple = ()
    # Whether `Foo::bar` may be matched by the evidence defining a bare `bar`. Only safe where the
    # qualifier is a namespace the source scan cannot see; for a class member it produced a real
    # misattribution -- a 4700-name C SDK defines something called `Initialize`, and WWAudio's
    # `ListenerHandleClass::Initialize` is not it.
    match_unqualified: bool = False


PROVIDERS = (
    Provider(
        key="miles",
        label="Miles AIL_* API — the `milesstub`/OpenAL backend",
        pile=PILE_LIBRARY,
        reason="`cmake/openal.cmake` builds an OpenAL-backed implementation of the same AIL_* API, "
               "and the 32-bit Windows build links the fetched miles-sdk-stub. This harness now "
               "builds `Core/Libraries/Source/OpenALAudioDevice` as a support archive and links "
               "libopenal, so what is left here is the part of the Miles surface that backend does "
               "not implement rather than the whole API.",
        owner="platform/audio-device (the Miles/OpenAL link)",
        sources=("Core/Libraries/Source/OpenALAudioDevice/**/*.cpp",
                 "@deps/miles-src/*.c"),
        libraries=(r"libopenal\.so.*",),
    ),
    Provider(
        key="ffmpeg",
        label="FFmpeg (libavcodec / libavformat / libavutil / libswscale)",
        pile=PILE_LIBRARY,
        reason="The video path is the engine's own `RTS_BUILD_OPTION_FFMPEG` route. "
               "`fetch-probe-deps.sh` provisions the pinned headers so the code compiles, and "
               "nothing installs an FFmpeg runtime for the link.",
        owner="video/bink-excision-and-harness-headers",
        headers=("@deps/ffmpeg-src/libav*/**/*.h", "@deps/ffmpeg-src/libsw*/**/*.h"),
        libraries=(r"libav(codec|format|util)\.so\.\d+", r"libsw(scale|resample)\.so\.\d+"),
    ),
    Provider(
        key="window-backend",
        label="The window/input backend this configuration does not choose (SDL2, Cocoa)",
        pile=PILE_LIBRARY,
        reason="`probe.OPTIONAL_BACKENDS` keeps the SDL2 backend opt-in and the Cocoa backend is "
               "Objective-C++, so no target lists either. The definitions are in the tree; a "
               "configuration that picks one resolves all of them.",
        owner="platform/macos-window-compile and platform/window-seam-wiring",
        sources=("Core/Libraries/Source/WWVegas/WWLib/platform/platform_window_sdl2.cpp",
                 "Core/Libraries/Source/WWVegas/WWLib/platform/platform_window_cocoa.mm"),
        libraries=(r"libSDL2-2\.0\.so\.\d+",),
        # These two files define `WWPlatform::Window_Create` inside `namespace WWPlatform`, which
        # the column-zero source scan only ever sees unqualified.
        match_unqualified=True,
    ),
    Provider(
        key="gamespy",
        label="GameSpy SDK",
        pile=PILE_CUT,
        reason="Online matchmaking is permanently cut scope (docs/porting/native-port-plan.md). "
               "The SDK's own sources are provisioned and define these symbols, so this is a link "
               "refused rather than one that is missing: they disappear when the call sites go, "
               "which is `online/absent-menu-seam`'s work, and must not be stubbed to make a link "
               "pass.",
        owner="online/absent-menu-seam",
        sources=("@deps/gamespy-src/**/*.c", "@deps/gamespy-src/**/*.cpp"),
    ),
)

# Categories whose pile is a property of the category itself: no library is involved either way.
CATEGORY_PILES = {
    "Generated gitinfo (build-time, not a blocker)": PILE_HARNESS,
    DISABLED_IF_CATEGORY: PILE_HARNESS,
    "Defined in a translation unit that failed to compile": PILE_COMPILE,
    UNBUILT_LAYER_CATEGORY: PILE_HARNESS,
    EXCLUDED_BACKEND_CATEGORY: PILE_LIBRARY,
    "COM / OLE (browser embedding, cut scope)": PILE_CUT,
    "Bink video": PILE_CUT,
    "Third-party library not linked (lzhl, zlib)": PILE_LIBRARY,
}


def _expand(globs, deps_dir):
    """Resolve a provider's evidence globs. `@deps/` prefixes are relative to the deps tree."""
    paths = []
    for pattern in globs:
        if pattern.startswith("@deps/"):
            root, rest = deps_dir, pattern[len("@deps/"):]
        else:
            root, rest = REPO_ROOT, pattern
        paths.extend(p for p in root.glob(rest) if p.is_file())
    return paths


def provider_definitions(deps_dir):
    """Provider key -> the names its evidence files define or declare, with what was scanned."""
    definitions = {}
    for provider in PROVIDERS:
        sources = _expand(provider.sources, deps_dir)
        headers = _expand(provider.headers, deps_dir)
        names = defined_names_in(sources) | declared_names_in(headers)
        definitions[provider.key] = (names, len(sources) + len(headers))
    return definitions


def provider_library_symbols(provider, search_dirs):
    """Symbols a real shared library on this box exports, for cross-checking the evidence scan.

    Informational only: the CI container has no FFmpeg or SDL2 runtime, so making the pile depend
    on this would make the pile split depend on the machine.
    """
    names = set()
    found = []
    for directory in search_dirs:
        root = pathlib.Path(directory)
        if not root.is_dir():
            continue
        for pattern in provider.libraries:
            matcher = re.compile(pattern + r"$")
            for path in root.iterdir():
                if matcher.match(path.name):
                    found.append(str(path))
                    names |= nm_symbols(path, "-D", "--defined-only", "--extern-only")
    return names, sorted(set(found))


LIBRARY_SEARCH_DIRS = (
    "/usr/lib/x86_64-linux-gnu", "/lib/x86_64-linux-gnu", "/usr/lib/aarch64-linux-gnu",
    "/usr/lib", "/usr/local/lib", "/opt/homebrew/lib",
)


def symbol_keys(demangled, unqualified=True):
    """The names a symbol could be matched by: fully qualified, bare, and optionally unqualified."""
    base = demangled.split("(", 1)[0].strip().removesuffix("[abi:cxx11]")
    stripped = SYMBOL_OWNER_PREFIX_RE.sub("", base).strip()
    keys = {base, stripped}
    if unqualified:
        keys.add(stripped.rsplit("::", 1)[-1])
    return keys


def provider_for(demangled, provider_definitions_by_key):
    """The first provider whose evidence accounts for this symbol, or None."""
    for provider in PROVIDERS:
        names, _ = provider_definitions_by_key.get(provider.key, (frozenset(), 0))
        if symbol_keys(demangled, provider.match_unqualified) & names:
            return provider
    return None


def pile_for(category, provider):
    if provider is not None:
        return provider.pile
    # `well_known_keys_category` names the one translation unit that decides those 104 symbols, so
    # the category text is built per run and cannot be a dictionary key.
    if category.startswith("Well-known Dict keys"):
        return PILE_COMPILE if "failed to compile" in category else PILE_HARNESS
    return CATEGORY_PILES.get(category, PILE_PORT)


# ---------------------------------------------------------------------------------------------
# Steps
# ---------------------------------------------------------------------------------------------

# The GameSpy SDK's own headers include their siblings unqualified (`gt2/gt2.h` does
# `#include "gscommon.h"`), so the directory the probe puts on the path, `gamespy-src/include`, is
# not enough: `include/gamespy` has to be there too. On Windows the MSVC build never noticed
# because it adds the directory anyway. Adding it here rather than in the probe keeps the probe's
# published baselines stable; the divergence measurement is unaffected because the probe pass in
# this script uses the same include set as the build it is compared against.
# stb-src for the same reason: <stb_truetype.h> is what WWLib's GDI text entry points rasterise
# glyphs with off Windows (docs/porting/gdi-font-seam.md), and the real CMake build gets it from
# the `stb` interface target that cmake/stb.cmake fetches.
EXTRA_DEP_INCLUDES = ["gamespy-src/include/gamespy", "stb-src"]

# `<mss.h>` has two candidates in this tree's include path and only one of them is the real build's.
# Off 32-bit Windows the Miles SDK is not fetched at all and `milesstub`
# (Core/Libraries/Source/OpenALAudioDevice) supplies the AIL_* API, header included; the probe puts
# the fetched `miles-src/mss` on the path so the audio code compiles wherever it runs, and that
# vendor header was winning here. It is not a harmless substitution: the SDK's mss.h defines
# `#define AIL_startup() (MSS_auto_cleanup(), AIL_startup())`, so every audio call site referenced
# `MSS_auto_cleanup`, a function the OpenAL backend does not implement and the real off-Windows
# build never calls. Putting the backend's own header directory first is what the real build does.
MSS_INCLUDE_DIRS = ["Core/Libraries/Source/OpenALAudioDevice/mss",
                    "Core/Libraries/Source/OpenALAudioDevice"]

# Headers a CMake target writes into its own binary directory at configure time, reproduced here
# verbatim from the `file(WRITE ...)` calls that produce them. Without them the entry point cannot
# compile at all, and "'BuildVersion.h' file not found" is a harness gap rather than a port blocker:
# the real build has these headers, so measuring their absence measures nothing.
GENERATED_HEADERS = {
    "GeneralsMD/Code/Main": {
        # GeneralsMD/Code/Main/CMakeLists.txt, `file(WRITE ... BuildVersion.h)`.
        "BuildVersion.h": "#pragma once\n\n"
                          "#define VERSION_MAJOR 1\n"
                          "#define VERSION_MINOR 4\n"
                          "#define VERSION_BUILDNUM 601\n",
        # GeneralsMD/Code/Main/CMakeLists.txt, `file(WRITE ... GeneratedVersion.h)`.
        "GeneratedVersion.h": (
            "#pragma once\n\n"
            "#define VERSION_LOCALBUILDNUM 0\n"
            "#define VERSION_BUILDUSER \"\"\n"
            "#define VERSION_BUILDLOC \"\"\n"),
    },
}


def write_generated_headers(build_dir):
    """Materialise the build-time headers. Returns target name -> include directory."""
    dirs = {}
    for target_name, files in GENERATED_HEADERS.items():
        directory = build_dir / "generated" / slug(target_name)
        directory.mkdir(parents=True, exist_ok=True)
        for name, text in files.items():
            (directory / name).write_text(
                f"// Generated by scripts/native-build.py from the file(WRITE) in "
                f"{target_name}/CMakeLists.txt. Do not edit.\n{text}")
        dirs[target_name] = directory
    return dirs


def includes_for(target, deps_dir, with_shims, generated_dirs=None):
    includes = npt.target_includes(target, deps_dir)
    extra = [str(deps_dir / d) for d in EXTRA_DEP_INCLUDES if (deps_dir / d).is_dir()]
    mss = [str(REPO_ROOT / d) for d in MSS_INCLUDE_DIRS if (REPO_ROOT / d).is_dir()]
    includes = mss + extra + includes
    generated = (generated_dirs or {}).get(target.name)
    if generated is not None:
        includes.insert(0, str(generated))
    if with_shims:
        # Shims first, so they win over anything else on the path -- the ordering the probe's
        # shimmed mode uses.
        includes.insert(0, str(SHIM_DIR))
    return includes


def write_manifests(targets, manifest_dir, deps_dir, skip=None, with_shims=False,
                    generated_dirs=None, extra_defines=()):
    """One CMake fragment per library. `skip` maps target name -> set of source paths to omit."""
    skip = skip or {}
    manifest_dir.mkdir(parents=True, exist_ok=True)
    written = {}
    for target in targets:
        sources = [s for s in npt.target_sources(target) if s not in skip.get(target.name, set())]
        includes = includes_for(target, deps_dir, with_shims, generated_dirs)
        lines = [
            f"# Generated by scripts/native-build.py for {target.name}. Do not edit.",
            "set(NATIVE_SOURCES",
            *[f'    "{s}"' for s in sources],
            ")",
            "set(NATIVE_INCLUDES",
            *[f'    "{i}"' for i in includes],
            ")",
        ]
        defines = list(target.defines) + list(extra_defines)
        if defines:
            lines += ["set(NATIVE_DEFINES", *[f'    "{d}"' for d in defines], ")"]
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
#
# macOS ships zlib in the dyld shared cache, so `/usr/lib/libz.dylib` is not a file on disk and the
# SDK offers only a `.tbd` text stub, which `nm` refuses ("File format has no dynamic symbol
# table"). The definitions this measurement needs therefore come from a real dylib -- Homebrew's
# keg -- while the link itself uses the same library the SDK resolves `-lz` to.
ZLIB_CANDIDATES = (
    "/lib/x86_64-linux-gnu/libz.so.1",
    "/usr/lib/x86_64-linux-gnu/libz.so.1",
    "/usr/lib/aarch64-linux-gnu/libz.so.1",
    "/usr/lib/libz.dylib",
    "/opt/homebrew/opt/zlib/lib/libz.dylib",
    "/usr/local/opt/zlib/lib/libz.dylib",
)

# The audio backend. `AIL_*` was never an unported surface: `cmake/openal.cmake` supplies
# `milesstub` from Core/Libraries/Source/OpenALAudioDevice off 32-bit Windows, and every audio
# consumer links that target, so the OpenAL backend is the engine's real audio device. This harness
# simply did not build it, which is the same harness-scope artefact level 4 removed for the
# renderer: the symbols were reported as unresolved because nothing here defined them.
#
# Built as a *support* archive for the same reason lzhl is: it is a dependency of the measured
# libraries, not one of them, so it stays out of the objects/translation-unit denominators, which
# must remain comparable with the probe's. Its file list is read out of its own CMakeLists.txt so
# the harness cannot drift from the real build.
AUDIO_BACKEND_DIR = "Core/Libraries/Source/OpenALAudioDevice"
AUDIO_BACKEND_SLUG = "support_openalaudiodevice"

# OpenAL itself is a system package (`libopenal-dev`) on Linux and a framework on macOS. The
# headers are also provisioned by scripts/ci/fetch-probe-deps.sh (openal-soft at the tag
# cmake/openal.cmake pins), so the backend compiles on a box that has no OpenAL installed; without
# the library it still cannot be *linked*, and then `al*`/`alc*` are reported as unresolved rather
# than silently counted as the engine's problem.
OPENAL_HEADER_SUBDIR = "openal-src/include"
# minimp3, the MPEG decoder OpenALMpeg.cpp compiles for retail music (cmake/minimp3.cmake pins it,
# fetch-probe-deps.sh provisions it at the same commit). Not optional: without the header the audio
# backend does not compile, and check-audio-backend-linked.py then fails by name rather than the
# music quietly going missing.
MINIMP3_HEADER_SUBDIR = "minimp3-src"
# openal-soft is keg-only in Homebrew, because macOS ships its own deprecated OpenAL.framework, so
# it is never symlinked into <prefix>/lib and only the keg path finds it.
OPENAL_CANDIDATES = (
    "/lib/x86_64-linux-gnu/libopenal.so.1",
    "/usr/lib/x86_64-linux-gnu/libopenal.so.1",
    "/usr/lib/aarch64-linux-gnu/libopenal.so.1",
    "/usr/local/lib/libopenal.dylib",
    "/opt/homebrew/lib/libopenal.dylib",
    "/opt/homebrew/opt/openal-soft/lib/libopenal.dylib",
    "/usr/local/opt/openal-soft/lib/libopenal.dylib",
)
OPENAL_SYSTEM_HEADER_DIRS = ("/usr/include", "/usr/local/include", "/opt/homebrew/include")

# The window/input/event-loop backend. Both backends were already in the tree and neither was ever
# built here, so the seam's 25 entry points were reported as unresolved: a configuration fact, and
# after #85 wired the user32 shim onto the seam, the single largest thing between this harness and
# an executable. This build now *chooses* one, the way the real build does
# (`CORE_WWLIB_WINDOW_BACKEND` in Core/Libraries/Source/WWVegas/WWLib/CMakeLists.txt): SDL2
# everywhere but Apple, the Cocoa backend on macOS, because those are the two the tree defines and
# the two the spike exercises. See docs/porting/decisions-resolved.md.
#
# A support archive, like the audio backend, for the same reason: it is a platform dependency of
# the measured libraries rather than one of the libraries whose portability is being counted, and
# the objects/translation-unit denominators have to stay comparable with the probe's.
WINDOW_BACKEND_DIR = "Core/Libraries/Source/WWVegas/WWLib/platform"
WINDOW_BACKEND_SLUG = "support_windowbackend"
WINDOW_BACKEND_SDL2 = "platform_window_sdl2.cpp"
WINDOW_BACKEND_COCOA = "platform_window_cocoa.mm"

# The render backend. Off Windows `DX8Wrapper`'s RenderBackendClass is
# Core/Libraries/Source/WWVegas/WW3D2/vulkanrenderbackend.cpp, which is a translation layer over the
# renderer in spikes/renderer -- the one the spike ladder verifies pixel-for-pixel. Those two
# sources are the `zh-render-backend` library the spike's own CMakeLists.txt now builds; this
# harness compiles the same two files, so the engine and the ladder cannot diverge.
#
# A support archive, like the window and audio backends: a platform dependency of the measured
# libraries, not one of them, so it stays out of the objects/translation-unit denominators.
RENDER_BACKEND_DIR = "spikes/renderer/src"
RENDER_BACKEND_SLUG = "support_renderbackend"
# png_write.cpp is here for the frame proof: VulkanRenderBackendClass::Measure_Frame reads the
# colour target back and writes it out, so a run can show what was IN the frame rather than quoting
# Present's HRESULT. It is the spike's own writer (no external dependency), not a copy.
RENDER_BACKEND_SOURCES = ("state_translate.cpp", "vulkan_backend.cpp", "png_write.cpp")
# The backend loads its SPIR-V from SPIKE_SHADER_DIR at device-creation time, so the shaders are
# compiled here with the same compiler and the same flags spikes/renderer/CMakeLists.txt uses.
RENDER_BACKEND_SHADERS = ("fixedfunc.vert", "fixedfunc.frag", "probe.vert", "probe.frag")
# Ubuntu jammy's Vulkan headers (1.3.204) predate VK_KHR_portability_enumeration, which the backend
# needs for the MoltenVK opt-in, so the spike is built against pinned headers there (see
# .agents/skills/renderer-spike-verify/SKILL.md). The same search order applies here, and the
# portability macro -- not merely the presence of <vulkan/vulkan.h> -- is what makes a directory
# usable, so a too-old system header is reported rather than silently failing to compile.
# build/docker/_deps/vulkan-headers-src/include comes first because scripts/ci/fetch-probe-deps.sh
# provisions it at a pinned tag: a CI run must not depend on what happens to be installed.
VULKAN_HEADER_DIRS = ("build/docker/_deps/vulkan-headers-src/include",
                      "$VULKAN_SDK/include", "$HOME/vk-headers/include",
                      "$HOME/vulkan-headers/include", "/usr/local/include",
                      "/opt/homebrew/include", "/usr/include")
VULKAN_PORTABILITY_MACRO = "VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME"
VULKAN_CANDIDATES = (
    "/usr/lib/x86_64-linux-gnu/libvulkan.so.1",
    "/lib/x86_64-linux-gnu/libvulkan.so.1",
    "/usr/lib/aarch64-linux-gnu/libvulkan.so.1",
    "/usr/local/lib/libvulkan.1.dylib",
    "/opt/homebrew/lib/libvulkan.1.dylib",
)

SDL2_HEADER_DIRS = ("/usr/include/SDL2", "/usr/local/include/SDL2", "/opt/homebrew/include/SDL2")
SDL2_CANDIDATES = (
    "/usr/lib/x86_64-linux-gnu/libSDL2-2.0.so.0",
    "/lib/x86_64-linux-gnu/libSDL2-2.0.so.0",
    "/usr/lib/aarch64-linux-gnu/libSDL2-2.0.so.0",
    "/usr/local/lib/libSDL2-2.0.dylib",
    "/opt/homebrew/lib/libSDL2-2.0.dylib",
)

# FFmpeg. `fetch-probe-deps.sh` builds the shared libraries from the tag vcpkg-lock.json pins into
# the deps tree, so the link uses the same version the headers come from; the system's FFmpeg is
# deliberately *not* a fallback, because Ubuntu 22.04's libavcodec 58 exports every name the pinned
# libavcodec 61 headers declare and agrees with none of their struct layouts.
FFMPEG_LIB_SUBDIR = "ffmpeg-lib/lib"
FFMPEG_LIB_STEMS = ("libavcodec", "libavformat", "libavutil", "libswscale", "libswresample")


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


def openal_library():
    return next((pathlib.Path(p) for p in OPENAL_CANDIDATES if pathlib.Path(p).exists()), None)


def openal_include_dir(deps_dir):
    """Where <AL/al.h> lives: the provisioned openal-soft headers, else the system's."""
    fetched = deps_dir / OPENAL_HEADER_SUBDIR
    if (fetched / "AL" / "al.h").is_file():
        return fetched
    for candidate in OPENAL_SYSTEM_HEADER_DIRS:
        if (pathlib.Path(candidate) / "AL" / "al.h").is_file():
            return pathlib.Path(candidate)
    return None


def audio_backend_sources():
    """The backend's translation units, read out of its own CMakeLists.txt so the two cannot drift.

    Its file list is a flat `set()` of names relative to the backend directory, not the
    `Source/...` layout `probe.cmake_sources` parses, so it gets its own two-line reader.
    """
    text = (REPO_ROOT / AUDIO_BACKEND_DIR / "CMakeLists.txt").read_text()
    listed = re.findall(r"^\s+(\S+\.cpp)\s*$", text, re.M)
    return sorted({p for name in listed
                   if (p := REPO_ROOT / AUDIO_BACKEND_DIR / name).is_file()})


def write_audio_backend_manifest(manifest_dir, deps_dir):
    """CMake fragment for the OpenAL audio backend. Its slug, or None without its headers."""
    include_dir = openal_include_dir(deps_dir)
    if include_dir is None:
        return None
    minimp3_dir = deps_dir / MINIMP3_HEADER_SUBDIR
    if not (minimp3_dir / "minimp3.h").is_file():
        print(f"   warning: no minimp3.h under {minimp3_dir}; the audio backend cannot compile "
              "its MPEG decoder. Run scripts/ci/fetch-probe-deps.sh.")
        return None
    root = REPO_ROOT / AUDIO_BACKEND_DIR
    sources = audio_backend_sources()
    if not sources:
        return None
    manifest_dir.mkdir(parents=True, exist_ok=True)
    lines = [
        "# Generated by scripts/native-build.py for the OpenAL audio backend. Do not edit.",
        "set(NATIVE_SOURCES",
        *[f'    "{s}"' for s in sources],
        ")",
        "set(NATIVE_INCLUDES",
        # WWAudio includes "mss.h" and GameEngineDevice "mss/mss.h", exactly as in the real build.
        f'    "{root}"',
        f'    "{root / "mss"}"',
        f'    "{include_dir}"',
        f'    "{minimp3_dir}"',
        # The project force-includes Utility/CppMacros.h into every target, this one included.
        f'    "{REPO_ROOT / "Dependencies" / "Utility"}"',
        ")",
    ]
    (manifest_dir / f"{AUDIO_BACKEND_SLUG}.cmake").write_text("\n".join(lines) + "\n")
    return AUDIO_BACKEND_SLUG


def vulkan_include_dir():
    """Where a new enough <vulkan/vulkan.h> lives, which is how the backend includes it."""
    for candidate in VULKAN_HEADER_DIRS:
        expanded = pathlib.Path(os.path.expandvars(candidate))
        if "$" in str(expanded):
            continue
        if not expanded.is_absolute():
            expanded = REPO_ROOT / expanded
        core = expanded / "vulkan" / "vulkan_core.h"
        if not (expanded / "vulkan" / "vulkan.h").is_file() or not core.is_file():
            continue
        if VULKAN_PORTABILITY_MACRO in core.read_text(errors="ignore"):
            return expanded
    return None


def vulkan_library():
    return next((pathlib.Path(p) for p in VULKAN_CANDIDATES if pathlib.Path(p).exists()), None)


def compile_spike_shaders(build_dir):
    """Compile the backend's four shaders to SPIR-V. Returns (directory, None) or (None, why)."""
    compiler = shutil.which("glslangValidator") or shutil.which("glslc")
    if compiler is None:
        return None, "no glslangValidator or glslc on PATH"
    source_dir = REPO_ROOT / "spikes" / "renderer" / "shaders"
    out_dir = build_dir / "generated" / "spike-shaders"
    out_dir.mkdir(parents=True, exist_ok=True)
    for shader in RENDER_BACKEND_SHADERS:
        source = source_dir / shader
        if not source.is_file():
            return None, f"{source.relative_to(REPO_ROOT)} is missing"
        out = out_dir / f"{shader}.spv"
        if compiler.endswith("glslc"):
            cmd = [compiler, str(source), "-o", str(out)]
        else:
            cmd = [compiler, "-V", str(source), "-o", str(out)]
        proc = subprocess.run(cmd, capture_output=True, text=True)
        if proc.returncode != 0 or not out.is_file():
            return None, f"{shader}: {(proc.stdout or proc.stderr).strip()[:200]}"
    return out_dir, None


def write_render_backend_manifest(manifest_dir, build_dir):
    """CMake fragment for the Vulkan render backend. Returns (slug, None) or (None, why)."""
    root = REPO_ROOT / RENDER_BACKEND_DIR
    sources = [root / name for name in RENDER_BACKEND_SOURCES]
    missing = [s for s in sources if not s.is_file()]
    if missing:
        return None, f"{missing[0].relative_to(REPO_ROOT)} is missing"
    include_dir = vulkan_include_dir()
    if include_dir is None:
        return None, "no <vulkan/vulkan.h>; install the Vulkan headers"
    shader_dir, why = compile_spike_shaders(build_dir)
    if shader_dir is None:
        return None, why
    manifest_dir.mkdir(parents=True, exist_ok=True)
    lines = [
        "# Generated by scripts/native-build.py for the Vulkan render backend. Do not edit.",
        "set(NATIVE_SOURCES",
        *[f'    "{s}"' for s in sources],
        ")",
        "set(NATIVE_INCLUDES",
        f'    "{root}"',
        f'    "{include_dir}"',
        # `platform/platform_window.h`, which the backend includes under SPIKE_WITH_PLATFORM_WINDOW.
        f'    "{REPO_ROOT / WINDOW_BACKEND_DIR}/.."',
        # This project force-includes Utility/CppMacros.h into every target, this one included.
        f'    "{REPO_ROOT / "Dependencies" / "Utility"}"',
        ")",
        "set(NATIVE_DEFINES",
        f'    "SPIKE_SHADER_DIR=\\"{shader_dir}\\""',
        # Without this the backend's surface and swapchain paths compile to `return true` and
        # Present() returns success having presented nothing -- measured on the outpost, where
        # `nm -um` on the archive showed no vkCreateSwapchainKHR at all. The window seam it needs
        # is WWLib's, and its implementations are already in the window support archive.
        '    "SPIKE_WITH_PLATFORM_WINDOW"',
        ")",
    ]
    (manifest_dir / f"{RENDER_BACKEND_SLUG}.cmake").write_text("\n".join(lines) + "\n")
    return RENDER_BACKEND_SLUG, None


def sdl2_include_dir():
    """Where <SDL.h> lives. The backend includes it unqualified, exactly as SDL2::SDL2 implies."""
    for candidate in SDL2_HEADER_DIRS:
        if (pathlib.Path(candidate) / "SDL.h").is_file():
            return pathlib.Path(candidate)
    return None


def sdl2_library():
    return next((pathlib.Path(p) for p in SDL2_CANDIDATES if pathlib.Path(p).exists()), None)


def window_backend_source():
    """The backend this platform chooses, or None when the file is missing."""
    name = WINDOW_BACKEND_COCOA if sys.platform == "darwin" else WINDOW_BACKEND_SDL2
    path = REPO_ROOT / WINDOW_BACKEND_DIR / name
    return path if path.is_file() else None


def write_window_backend_manifest(manifest_dir):
    """CMake fragment for the chosen window backend. Returns (slug, source) or (None, reason)."""
    source = window_backend_source()
    if source is None:
        return None, "neither backend source is present"
    includes = [REPO_ROOT / WINDOW_BACKEND_DIR,
                REPO_ROOT / "Core" / "Libraries" / "Source" / "WWVegas",
                REPO_ROOT / "Dependencies" / "Utility"]
    if sys.platform != "darwin":
        include_dir = sdl2_include_dir()
        if include_dir is None:
            return None, "no SDL2 headers (<SDL.h>); install libsdl2-dev"
        includes.insert(0, include_dir)
    manifest_dir.mkdir(parents=True, exist_ok=True)
    lines = [
        "# Generated by scripts/native-build.py for the window backend. Do not edit.",
        "set(NATIVE_SOURCES",
        f'    "{source}"',
        ")",
        "set(NATIVE_INCLUDES",
        *[f'    "{i}"' for i in includes],
        ")",
    ]
    (manifest_dir / f"{WINDOW_BACKEND_SLUG}.cmake").write_text("\n".join(lines) + "\n")
    return WINDOW_BACKEND_SLUG, source


# resources/gitinfo/gitinfo.cpp is `configure_file`d by resources/gitinfo/git_watcher.cmake at
# build time, and this harness never ran that step, so six symbols the version reporting reads --
# `GitRevision`, `GitShortSHA1`, `GitCommitTimeStamp`, `GitCommitAuthorName`, `GitTag`,
# `GitUncommittedChanges` -- were unresolved for no reason except that. Running the real generator
# (`cmake -P`, the same command the real build's `check_git` target runs) rather than writing the
# values here keeps the harness's copy identical to the build's, including the fields nothing in the
# engine reads yet.
GITINFO_DIR = "resources/gitinfo"
GITINFO_SLUG = "support_gitinfo"


def generate_gitinfo(build_dir, manifest_dir):
    """Run git_watcher.cmake and write a manifest for its output.

    Returns (slug, cpp) or (None, why).
    """
    watcher = REPO_ROOT / GITINFO_DIR / "git_watcher.cmake"
    template = REPO_ROOT / GITINFO_DIR / "gitinfo.cpp.in"
    if not watcher.is_file() or not template.is_file():
        return None, f"{GITINFO_DIR} has no generator"
    git = shutil.which("git")
    if git is None:
        return None, "no git on PATH"
    out_dir = build_dir / "generated" / GITINFO_SLUG
    out_dir.mkdir(parents=True, exist_ok=True)
    generated = out_dir / "gitinfo.cpp"
    proc = subprocess.run([
        "cmake",
        "-D_BUILD_TIME_CHECK_GIT=TRUE",
        f"-DGIT_WORKING_DIR={REPO_ROOT}",
        f"-DGIT_EXECUTABLE={git}",
        f"-DGIT_STATE_FILE={out_dir / 'git-state-hash'}",
        f"-DGIT_PRE_CONFIGURE_FILE={template}",
        f"-DGIT_POST_CONFIGURE_FILE={generated}",
        "-P", str(watcher),
    ], capture_output=True, text=True)
    if proc.returncode != 0 or not generated.is_file():
        return None, f"git_watcher.cmake failed: {(proc.stderr or proc.stdout).strip()[:200]}"
    manifest_dir.mkdir(parents=True, exist_ok=True)
    lines = [
        "# Generated by scripts/native-build.py for gitinfo. Do not edit.",
        "set(NATIVE_SOURCES",
        f'    "{generated}"',
        ")",
        "set(NATIVE_INCLUDES",
        f'    "{REPO_ROOT / GITINFO_DIR}"',
        f'    "{REPO_ROOT / "Dependencies" / "Utility"}"',
        ")",
    ]
    (manifest_dir / f"{GITINFO_SLUG}.cmake").write_text("\n".join(lines) + "\n")
    return GITINFO_SLUG, generated


def ffmpeg_libraries(deps_dir):
    """The pinned FFmpeg shared libraries fetch-probe-deps.sh built, or [] when absent."""
    if deps_dir is None:
        return []
    root = deps_dir / FFMPEG_LIB_SUBDIR
    if not root.is_dir():
        return []
    found = []
    for stem in FFMPEG_LIB_STEMS:
        # The versioned file, never the `.so` symlink: the link records the soname either way and
        # naming the real file keeps the report's "what was linked" honest.
        matches = sorted(p for p in root.glob(f"{stem}.so.*") if not p.is_symlink())
        matches += sorted(p for p in root.glob(f"{stem}.*.dylib") if not p.is_symlink())
        if matches:
            found.append(matches[0])
    return found if len(found) == len(FFMPEG_LIB_STEMS) else []


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

    # Attribute each diagnostic to the translation unit that printed it: either the error line
    # names the .cpp, or clang's "In file included from" chain does, its head being the unit. Only
    # accepting the first form recorded an empty diagnostic for every error inside a header, which
    # is most of them (56 of 72 in the shimmed level 1-3 build). Ninja prints each edge's output
    # atomically, so the chain and the error it introduces stay adjacent.
    diagnostics = {}
    by_name = {source.name: source for source in failed}
    include_chain = re.compile(r"^In file included from (\S+?):\d+")
    current = None
    for line in log.splitlines():
        chain = include_chain.match(line)
        if chain:
            # The head of the chain is the translation unit; deeper lines are its headers.
            head = by_name.get(pathlib.Path(chain.group(1)).name)
            if head is not None:
                current = head
            continue
        if ": error:" not in line and ": fatal error:" not in line:
            continue
        named = next((source for name, source in by_name.items()
                      if line.startswith(("/", ".")) and name in line.split(":", 1)[0]), None)
        source = named or current
        if source is not None and source not in diagnostics:
            diagnostics[source] = line.split(": error:", 1)[-1] \
                .split(": fatal error:", 1)[-1].strip()
    return failed, diagnostics


def probe_sources(sources, target_by_source, deps_dir, jobs, with_shims=False,
                  generated_dirs=None, extra_defines=()):
    """Run the probe's -fsyntax-only check over the same translation units, for comparison."""
    flags = [f for f in npt.probe.CLANG_FLAGS]

    def run(source):
        target = target_by_source[source]
        includes = includes_for(target, deps_dir, with_shims, generated_dirs)
        cmd = [CXX, *flags]
        cmd += [f"-D{d}" for d in list(target.defines) + list(extra_defines)]
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

# ---------------------------------------------------------------------------------------------
# One symbol namespace
#
# An object file's symbol table does not spell a C name the way the source does: Mach-O prefixes
# every C symbol with an underscore (`main` is `_main`, `_ZN...` is `__ZN...`), ELF does not. #87's
# Darwin run is what this abstraction is for. The harness compared names from four sources -- `nm`
# over the archives, `nm` over the system libraries, the linker's diagnostics, and a hard-coded
# `"main"` -- and only ELF spells all four the same way, so on macOS the entry-point scan matched
# nothing and the checker's `agrees_with_nm` was false.
#
# The fix is not a per-platform table of spellings, which is a special case waiting for the next
# platform. Every name entering this script is converted once, at the boundary, into a single
# prefix-free namespace, and the prefix itself is *measured* from the toolchain being used rather
# than inferred from `sys.platform`.
# ---------------------------------------------------------------------------------------------

SYMBOL_PREFIX_PROBE_NAME = "native_build_symbol_prefix_probe"


@functools.cache
def symbol_prefix():
    """What this toolchain puts in front of a C symbol name: `""` under ELF, `"_"` under Mach-O.

    Measured by compiling a one-line translation unit and reading the name back out of the object
    file, so a platform whose convention this script has never seen is handled by asking it. There
    is deliberately no fallback: every symbol comparison here, the entry-point scan included,
    depends on this answer, and guessing it wrong is how a link of a generated stub gets reported
    as a link of the game.
    """
    with tempfile.TemporaryDirectory() as tmp:
        source = pathlib.Path(tmp) / "symbol_prefix_probe.c"
        obj = pathlib.Path(tmp) / "symbol_prefix_probe.o"
        source.write_text(f"void {SYMBOL_PREFIX_PROBE_NAME}(void) {{}}\n")
        compile_proc = subprocess.run([CXX, "-x", "c", "-c", str(source), "-o", str(obj)],
                                      capture_output=True, text=True)
        if compile_proc.returncode != 0 or not obj.exists():
            raise SystemExit(
                f"FAIL: cannot determine this toolchain's C symbol prefix: {CXX} could not compile "
                f"a one-line probe.\n{compile_proc.stderr.strip()}")
        proc = subprocess.run([NM, "--defined-only", "--extern-only", str(obj)],
                              capture_output=True, text=True)
        for line in proc.stdout.splitlines():
            parts = line.split()
            if parts and parts[-1].endswith(SYMBOL_PREFIX_PROBE_NAME):
                return parts[-1][:-len(SYMBOL_PREFIX_PROBE_NAME)]
    raise SystemExit(
        f"FAIL: cannot determine this toolchain's C symbol prefix: `{NM}` did not report "
        f"`{SYMBOL_PREFIX_PROBE_NAME}` in an object file that defines it. Every symbol comparison "
        "in this script depends on knowing it, so the run stops rather than measuring the wrong "
        "names.")


def canonical_symbol(name):
    """A symbol table name in this script's namespace: exactly one platform prefix removed.

    One, not `lstrip("_")`: `__ZN...` is the Mach-O spelling of the mangled name `_ZN...` and
    `___cxa_throw` of `__cxa_throw`, so stripping greedily corrupts both and no two lists of names
    can then be compared.
    """
    prefix = symbol_prefix()
    if prefix and name.startswith(prefix):
        return name[len(prefix):]
    return name


def platform_symbol(name):
    """The inverse: how this platform's symbol table spells `name`."""
    return symbol_prefix() + name


# Libraries a native build may legitimately link against. What they export is resolved, not a
# blocker, and must not show up in the report as one.
#
# Globbed per architecture rather than listed for x86-64 alone: on any other Linux multiarch triple
# the list matched no file, and an empty discount list silently turns every libc symbol into a
# reported blocker.
SYSTEM_LIBRARY_GLOBS = [
    "/lib/*-linux-gnu/lib[cm].so.6",
    "/lib/*-linux-gnu/libpthread.so.0",
    "/lib/*-linux-gnu/libdl.so.2",
    "/lib/*-linux-gnu/libgcc_s.so.1",
    "/lib/*-linux-gnu/libstdc++.so.6",
    "/usr/lib/*-linux-gnu/libstdc++.so.6",
]

# macOS has no such files to point `nm` at. Since Big Sur the system dylibs exist only inside the
# dyld shared cache -- `/usr/lib/libSystem.B.dylib` is not on disk -- so the glob above found
# nothing, the discount list came out EMPTY, and #87's Darwin run reported libc and libc++ symbols
# as unresolved while ld64 resolved every one of them. That is the whole of `agrees_with_nm` being
# false against a clean link, and relaxing the check would have hidden it.
#
# What ld64 itself links against is on disk: the SDK's text-based stubs (`.tbd`), which list the
# exports of each system dylib. Only the libraries this script actually links are read -- the
# libSystem umbrella's re-exported members, libc++ and libobjc -- because discounting every SDK
# library would hide a real unresolved symbol from a library nothing put on the link line.
SDK_STUB_LIBRARIES = (
    "usr/lib/libSystem.tbd",
    "usr/lib/libc++.tbd",
    "usr/lib/libc++abi.tbd",
    "usr/lib/libobjc.tbd",
)
SDK_STUB_GLOBS = ("usr/lib/system/*.tbd",)
# `dyld_info` reads the shared cache directly, and is the fallback when no SDK is installed.
DYLD_INFO_LIBRARIES = (
    "/usr/lib/libSystem.B.dylib",
    "/usr/lib/libc++.1.dylib",
    "/usr/lib/libc++abi.dylib",
    "/usr/lib/libobjc.A.dylib",
)

# Supplied by the CRT startup files, the unwinder or the dynamic loader rather than by a library nm
# can be pointed at. Written in this script's namespace, i.e. with the platform prefix already off:
# Mach-O's `___cxa_throw` arrives here as `__cxa_throw`, and `__mh_execute_header` -- which the
# linker synthesises rather than any library exporting -- as `_mh_execute_header`.
TOOLCHAIN_SYMBOL_RE = re.compile(
    r"^(__gmon_start__|_ITM_\w+|_Unwind_\w+|__cxa_\w+|__gxx_\w+|__dso_handle|"
    r"_GLOBAL_OFFSET_TABLE_|__stack_chk_\w+|_mh_execute_header|dyld_stub_binder)$")


def nm_symbols(path, *flags):
    """Symbol names `nm` reports for `path`, in this script's prefix-free namespace."""
    proc = subprocess.run([NM, *flags, str(path)], capture_output=True, text=True)
    names = set()
    for line in proc.stdout.splitlines():
        parts = line.split()
        if parts:
            # Dynamic symbol tables carry a version suffix (`acosf@@GLIBC_2.2.5`), which is not
            # part of the name an archive references.
            names.add(canonical_symbol(parts[-1].split("@", 1)[0]))
    return names


def exported_symbol_flags():
    """`nm` flags for the exports of a shared library.

    `-D` selects ELF's dynamic symbol table and has no Mach-O counterpart; llvm-nm rejects it for a
    dylib, which would have made every such read come back empty.
    """
    if sys.platform == "darwin":
        return ("--defined-only", "--extern-only")
    return ("-D", "--defined-only", "--extern-only")


# `symbols: [ _foo, _bar ]`, possibly wrapped over several lines, in a .tbd stub.
TBD_SYMBOLS_RE = re.compile(r"\bsymbols:\s*\[(.*?)\]", re.S)


def tbd_symbols(path):
    """Exports listed in a text-based dylib stub, in this script's namespace."""
    names = set()
    text = pathlib.Path(path).read_text(errors="replace")
    for match in TBD_SYMBOLS_RE.finditer(text):
        for name in match.group(1).replace("\n", " ").split(","):
            name = name.strip().strip("'\"")
            if name:
                names.add(canonical_symbol(name))
    return names


@functools.cache
def sdk_path():
    proc = subprocess.run(["xcrun", "--show-sdk-path"], capture_output=True, text=True)
    if proc.returncode != 0 or not proc.stdout.strip():
        return None
    path = pathlib.Path(proc.stdout.strip())
    return path if path.is_dir() else None


def dyld_info_exports(path):
    """Exports of a dylib that lives only in the dyld shared cache, in this script's namespace."""
    if not shutil.which("dyld_info"):
        return set()
    proc = subprocess.run(["dyld_info", "-exports", str(path)], capture_output=True, text=True)
    names = set()
    for line in proc.stdout.splitlines():
        # `0x00012345  _foo` / `0x00012345  _foo [re-export from libsystem_c]`
        for field in line.split():
            if field.startswith(symbol_prefix() or "_") and re.fullmatch(r"[\w$.]+", field):
                names.add(canonical_symbol(field))
                break
    return names


def darwin_system_symbols():
    """(exports, sources read) of the macOS libraries this script's link line pulls in."""
    names, sources = set(), []
    sdk = sdk_path()
    if sdk is not None:
        stubs = [sdk / name for name in SDK_STUB_LIBRARIES]
        stubs += [pathlib.Path(p) for pattern in SDK_STUB_GLOBS
                  for p in sorted(glob.glob(str(sdk / pattern)))]
        for stub in stubs:
            if stub.is_file():
                found = tbd_symbols(stub)
                if found:
                    names |= found
                    sources.append(str(stub))
    if not names:
        for dylib in DYLD_INFO_LIBRARIES:
            found = dyld_info_exports(dylib)
            if found:
                names |= found
                sources.append(f"dyld_info -exports {dylib}")
    return names, sources


@functools.cache
def system_symbols():
    """(exports of the platform's own libraries, what was read to get them).

    Empty is not an acceptable answer: it would discount nothing, so every `memcpy` and every
    libc++ symbol would be reported as an unresolved blocker and the totals would describe this
    function rather than the port. The run stops instead.
    """
    if sys.platform == "darwin":
        names, sources = darwin_system_symbols()
    else:
        names, sources = set(), []
        for pattern in SYSTEM_LIBRARY_GLOBS:
            for path in sorted(glob.glob(pattern)):
                names |= nm_symbols(path, *exported_symbol_flags())
                sources.append(path)
    if not names:
        raise SystemExit(
            "FAIL: could not read the exports of a single system library on this platform "
            f"({sys.platform}), so libc and libc++ symbols would be counted as unresolved port "
            "work. That is a measurement bug, not a result, so nothing is written. On macOS this "
            "means neither an SDK (`xcrun --show-sdk-path`) nor `dyld_info` was usable; on Linux, "
            f"that none of {SYSTEM_LIBRARY_GLOBS} matched.")
    return frozenset(names), sources


def framework_symbols(link_args):
    """Exports of the `-framework X` libraries on the link line.

    The Cocoa backend's frameworks are passed as `-framework`, not as paths, so they were invisible
    to the scan while ld64 resolved everything they define -- the same class of disagreement as the
    system libraries above, one link-line spelling further out.
    """
    names = set()
    if sys.platform != "darwin":
        return names
    sdk = sdk_path()
    wanted = [b for a, b in zip(link_args, link_args[1:]) if a == "-framework"]
    for framework in wanted:
        stub = sdk / "System" / "Library" / "Frameworks" / f"{framework}.framework" / \
            f"{framework}.tbd" if sdk is not None else None
        if stub is not None and stub.is_file():
            names |= tbd_symbols(stub)
        else:
            names |= dyld_info_exports(
                f"/System/Library/Frameworks/{framework}.framework/{framework}")
    return names


def unresolved_symbols(archives, support_archives=(), extra_libraries=(), extra_link_args=()):
    """Symbols the engine archives reference that nothing linked into the binary defines.

    Read out of the archives with `nm` rather than scraped from linker diagnostics: ld demangles
    in its messages, which throws away the mangled name, and it reports only what its own archive
    ordering happened to require.

    `support_archives` (lzhl) and `extra_libraries` (zlib) contribute definitions only: they are
    dependencies, so what *they* reference is not this measurement's business. `extra_link_args`
    contributes the `-framework` libraries for the same reason.
    """
    defined = set()
    referenced_from = collections.defaultdict(set)
    for archive in list(archives) + list(support_archives):
        defined |= nm_symbols(archive, "--defined-only", "--extern-only")
    for library in extra_libraries:
        defined |= nm_symbols(library, *exported_symbol_flags())
    defined |= framework_symbols(list(extra_link_args))
    for archive in archives:
        for name in nm_symbols(archive, "--undefined-only"):
            referenced_from[name].add(archive.stem)

    system, _ = system_symbols()
    return {
        name: sorted(archives_using) for name, archives_using in referenced_from.items()
        if name not in defined and name not in system and not TOOLCHAIN_SYMBOL_RE.match(name)
    }


# The entry point, in this script's namespace. `platform_symbol()` spells it for the symbol table
# being read, so nothing here has to know that Mach-O calls it `_main`.
ENTRY_SYMBOL = "main"


def archives_defining_main(archives):
    """Archives that define `main`, i.e. that carry the game's real entry point."""
    return [a for a in archives
            if ENTRY_SYMBOL in nm_symbols(a, "--defined-only", "--extern-only")]


# The one library whose `main()` is the game's: GeneralsMD/Code/Main. Everything else that defines
# one is a standalone test tool that happens to live inside a measured directory
# (WWLib/platform/tests/gdi_font_metrics_dump.cpp, win32_file_api_test.cpp), so it is compiled and
# counted as an object but must not be linked into the game binary.
GAME_ENTRY_TARGET = "GeneralsMD/Code/Main"


def objects_defining_main(archive):
    """Member object names inside `archive` that define `main`."""
    proc = subprocess.run([NM, "-A", "--defined-only", "--extern-only", str(archive)],
                          capture_output=True, text=True)
    # `libfoo.a:bar.cpp.o:0000000000000000 T main`
    pattern = re.compile(r"^.*?:([^:]+\.o):\s*\S+\s+\S+\s+"
                         + re.escape(platform_symbol(ENTRY_SYMBOL)) + r"$")
    return [m.group(1) for m in (pattern.match(line) for line in proc.stdout.splitlines())
            if m is not None]


def drop_rival_entry_points(archives, keep):
    """Remove other `main()` definitions from the archives, so the game's is the only one.

    A test tool's entry point is a duplicate definition of `main` the moment the game target
    produces an object, and the linker then reports that rather than the port's own unresolved
    symbols. Returns the archive member names removed.
    """
    removed = []
    for archive in archives:
        if archive == keep:
            continue
        for member in objects_defining_main(archive):
            subprocess.run(["ar", "d", str(archive), member], capture_output=True, text=True)
            removed.append(f"{archive.stem}({member})")
    return sorted(removed)


def entry_point_anchor(entry_archives, game_target_built):
    """The source of the link's anchor translation unit, and why it is what it is.

    Returns (text, stub_used). A generated `int main() { return 0; }` is legitimate in exactly one
    situation: the entry-point *target* is not in the selected levels, as in a levels 1-2 build,
    which measures libraries and has no entry point of its own to link.

    Once `GeneralsMD/Code/Main` is in the build and no archive defines the entry point, a stub is a
    lie in the shape of a measurement -- the link succeeds, `binary_produced` goes true, and the
    file is a program that returns 0 while every figure still carries the game's name. That is
    precisely what happened on Darwin in #87, where the scan looked for the ELF spelling of `main`
    in a Mach-O archive: the substitution was silent and the only visible symptom was 17 duplicate
    symbols from the test tools whose entry points had not been dropped. So it stops the run.
    """
    if entry_archives:
        return ("// Generated by scripts/native-build.py: empty on purpose. The game target's own\n"
                "// main() anchors this link, so a stub entry point would collide with it.\n"), \
            False
    if game_target_built:
        raise SystemExit(
            f"FAIL: {GAME_ENTRY_TARGET} is in this build, but no archive defines the entry point "
            f"`{platform_symbol(ENTRY_SYMBOL)}`, so there is nothing to link the game through. "
            "Refusing to substitute a generated stub main(): that would report a link of a "
            "three-line program as a link of the game, which is the failure mode this harness "
            "exists to catch. Look for a compile failure in that target (PlatformMain.cpp), or for "
            "a symbol-table spelling this script's namespace does not cover -- "
            f"`{NM} --defined-only --extern-only` over the archives says which.")
    return ("// Generated by scripts/native-build.py: an entry point so the linker has something\n"
            "// to anchor to, because the game's own entry-point target is not in the selected\n"
            f"// levels ({GAME_ENTRY_TARGET} is level 3). The archives are pulled in whole, so\n"
            "// what matters is what they reference.\n"
            "int main() { return 0; }\n"), True


def link_probe(build_dir, archives, support_archives=(), link_zlib=False, openal_path=None,
               extra_libraries=(), extra_link_args=(), game_target_built=False):
    """Run the linker over every archive, so "no linker has ever run" stops being true.

    `--whole-archive` forces every object in, since a trivial main() otherwise pulls in nothing,
    and `--warn-unresolved-symbols` lets it produce a binary anyway so the outcome is a result
    rather than a wall of errors. The third-party archives follow without `--whole-archive`: they
    are dependencies, so only what the engine actually calls needs to come in.

    The stub entry point is used only while the game's own entry-point target is outside the
    selected levels; see `entry_point_anchor`, which refuses to generate one otherwise.
    """
    entry_point = archives_defining_main(archives)
    stub = build_dir / "native_link_probe.cpp"
    text, stub_used = entry_point_anchor(entry_point, game_target_built)
    stub.write_text(text)
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
    # The audio backend is one of the support archives, and it calls OpenAL. Linked by path rather
    # than with -lopenal: the runtime library alone (libopenal.so.1) is enough to resolve the
    # symbols, and requiring the -dev package's development symlink would make the link fail on a
    # box where the backend can perfectly well be linked.
    if openal_path is not None:
        cmd.append(str(openal_path))
    # FFmpeg and SDL2, by path for the same reason, plus the Cocoa frameworks on macOS.
    cmd += [str(p) for p in extra_libraries]
    cmd += list(extra_link_args)
    proc = subprocess.run(cmd, capture_output=True, text=True)
    return (proc.returncode == 0, out.exists(), proc.stdout + proc.stderr,
            [a.stem for a in entry_point], stub_used)


UNDEFINED_REFERENCE_RE = re.compile(r"undefined reference to `([^']+)'")
# `libcore_gameengine.a(PlatformWindowHost.cpp.o): in function `X::y()':` -- the archive member that
# references the symbols named on the lines that follow.
REFERENCING_MEMBER_RE = re.compile(r"^(?:\S*ld:\s*)?(\S+\.a)\(([^)]+\.o)\)")
# ld64 says `  "_AIL_startup", referenced from:` and then `      _f in libfoo.a(bar.o)`. It reports
# mangled names with the Mach-O leading underscore where GNU ld reports demangled ones, so which
# form the list is in depends on the platform and is recorded alongside it.
MACH_UNDEFINED_RE = re.compile(r'^\s*"([^"]+)", referenced from:')
MACH_REFERENCED_FROM_RE = re.compile(r"\bin ([^\s(]+\.a)\(([^)]+\.o)\)")
STRICT_LINK_NAME_FORM = "mangled" if sys.platform == "darwin" else "demangled"


def parse_strict_link_log(log):
    """symbol -> first archive member that references it, from a failed link's diagnostics."""
    referenced_by = {}
    member = None
    pending = None
    for line in log.splitlines():
        found = REFERENCING_MEMBER_RE.match(line.strip())
        if found:
            member = f"{pathlib.Path(found.group(1)).name}({found.group(2)})"
        for match in UNDEFINED_REFERENCE_RE.finditer(line):
            referenced_by.setdefault(match.group(1), member)
        mach = MACH_UNDEFINED_RE.match(line)
        if mach:
            pending = canonical_symbol(mach.group(1))
            referenced_by.setdefault(pending, None)
            continue
        site = MACH_REFERENCED_FROM_RE.search(line)
        if site and pending is not None and referenced_by.get(pending) is None:
            referenced_by[pending] = f"{pathlib.Path(site.group(1)).name}({site.group(2)})"
    return referenced_by


def strict_link(build_dir, archives, support_archives=(), link_zlib=False,
                openal_path=None, extra_libraries=(), extra_link_args=(),
                game_target_built=False):
    """Link with no tolerance for unresolved symbols, i.e. attempt an actual executable.

    `link_probe` above passes `--warn-unresolved-symbols`, which is why it produces a file: the
    unresolved symbols are warnings and the "binary" it writes is not one the loader would accept.
    This runs the same link without that flag, so the linker's own verdict is the result. It is
    expected to fail; the deliverable is the list it fails with, fully attributed.

    Returns (linked, binary_produced, symbol -> first referencing archive member, log). The names
    are demangled under GNU ld and mangled under ld64; `STRICT_LINK_NAME_FORM` says which.

    Nothing is stubbed to make this pass, and nothing may be: a strict link made green with stubs is
    a worse measurement than the honest failure, because the stubs are invisible in every figure
    afterwards.
    """
    entry_point = archives_defining_main(archives)
    stub = build_dir / "native_strict_link.cpp"
    text, _ = entry_point_anchor(entry_point, game_target_built)
    stub.write_text("// Generated by scripts/native-build.py for the strict link.\n" + text)
    out = build_dir / "native_strict_link"
    if out.exists():
        out.unlink()
    if sys.platform == "darwin":
        cmd = [
            CXX, "-std=c++20", "-o", str(out), str(stub), "-Wl,-all_load",
            *[str(a) for a in archives], *[str(a) for a in support_archives], "-lc++", "-lm",
        ]
    else:
        cmd = [
            CXX, "-std=c++20", "-o", str(out), str(stub),
            "-Wl,--whole-archive", *[str(a) for a in archives], "-Wl,--no-whole-archive",
            *[str(a) for a in support_archives],
            "-lstdc++", "-lm", "-lpthread", "-ldl",
        ]
    if link_zlib:
        cmd.append("-lz")
    # By path, for the same reason link_probe does it: the OpenAL runtime library alone resolves the
    # backend's al*/alc* calls. Omitting it here would fail the strict link on ~90 symbols the
    # tolerant link resolves, which would read as port work rather than as a missing -l.
    if openal_path is not None:
        cmd.append(str(openal_path))
    # FFmpeg (the video path's own libraries, at the pinned version) and SDL2 or the Cocoa
    # frameworks (the chosen window backend's). Both are dependencies of code in this tree, so an
    # unresolved `av_*` or `SDL_*` is a missing -l, not port work -- but they are only linked when
    # they are actually present, so a box without them gets the honest unresolved list instead.
    cmd += [str(p) for p in extra_libraries]
    cmd += list(extra_link_args)
    proc = subprocess.run(cmd, capture_output=True, text=True)
    log = proc.stdout + proc.stderr
    # ld reports every reference site, so the same symbol appears many times; the first one is kept,
    # because a named object file is a better answer to "who needs this" than a count.
    return proc.returncode == 0, out.exists(), parse_strict_link_log(log), log


ELF_MACHINES = {0x3e: "x86-64", 0xb7: "aarch64", 0x28: "arm", 0x03: "i386"}
MACHO_MAGICS = {
    0xfeedfacf: ("Mach-O", 64),
    0xfeedface: ("Mach-O", 32),
}
MACHO_CPUS = {0x0100000c: "arm64", 0x01000007: "x86-64", 0x00000007: "i386"}
# A universal (fat) file, big-endian magic by definition. Worth naming rather than reporting as
# "unknown": an `arm64` slice inside a fat file is not the thin native binary this port measures,
# and the header of a fat file says nothing about the word size of what is inside it.
MACHO_FAT_MAGICS = (b"\xca\xfe\xba\xbe", b"\xca\xfe\xba\xbf")
# `lipo` spells the same CPU differently from the Mach-O header table above.
LIPO_ARCH_ALIASES = {"x86_64": "x86-64"}


def describe_binary(path):
    """Read the file's own header, so "is this a 64-bit executable" is measured, not assumed.

    `file(1)` is not present on every box this runs on and its wording is not stable, so the few
    header bytes that answer the question are decoded here instead: container, word size and
    machine. A file whose header says 32-bit or whose machine is not this host's would be a
    harness bug worth failing on, which is why CI gates on these fields rather than on the mere
    existence of a path.
    """
    path = pathlib.Path(path)
    if not path.exists():
        return None
    info = {"path": str(path.relative_to(REPO_ROOT) if path.is_relative_to(REPO_ROOT) else path),
            "bytes": path.stat().st_size, "format": "unknown", "word_size": None,
            "machine": None}
    with path.open("rb") as handle:
        head = handle.read(32)
    if head[:4] == b"\x7fELF":
        info["format"] = "ELF"
        info["word_size"] = 64 if head[4] == 2 else 32
        machine = int.from_bytes(head[18:20], "little" if head[5] == 1 else "big")
        info["machine"] = ELF_MACHINES.get(machine, f"0x{machine:x}")
        info["elf_type"] = {2: "EXEC", 3: "DYN"}.get(
            int.from_bytes(head[16:18], "little" if head[5] == 1 else "big"), "other")
    else:
        magic = int.from_bytes(head[:4], "little")
        if head[:4] in MACHO_FAT_MAGICS:
            info["format"] = "Mach-O universal"
            count = int.from_bytes(head[4:8], "big")
            info["architectures"] = count
        elif magic in MACHO_MAGICS:
            info["format"], info["word_size"] = MACHO_MAGICS[magic]
            cpu = int.from_bytes(head[4:8], "little")
            info["machine"] = MACHO_CPUS.get(cpu, f"0x{cpu:x}")
            info["macho_filetype"] = int.from_bytes(head[12:16], "little")
    # The independent answer to "is this really an arm64 binary": the header above is decoded by
    # this script, `lipo -archs` by the platform's own tool, and a slice list with anything but
    # this host's architecture in it means the build came out for another one. A build that
    # silently came out x86-64 under Rosetta would invalidate every conclusion drawn from it.
    lipo = shutil.which("lipo")
    if lipo:
        proc = subprocess.run([lipo, "-archs", str(path)], capture_output=True, text=True)
        if proc.returncode == 0:
            info["lipo_archs"] = [LIPO_ARCH_ALIASES.get(a, a) for a in proc.stdout.split()]
    tool = shutil.which("file")
    if tool:
        proc = subprocess.run([tool, "-b", str(path)], capture_output=True, text=True)
        if proc.returncode == 0:
            info["file_output"] = proc.stdout.strip()
    return info


def host_translated():
    """Whether this process is a translated (Rosetta 2) one, or None where the question is absent.

    An arm64 Mac running an x86-64 Python and toolchain under Rosetta produces x86-64 objects while
    every other field in this file still says the host is an Apple Silicon machine, so a run taken
    that way is not the measurement it claims to be. The platform answers it directly.
    """
    if sys.platform != "darwin":
        return None
    proc = subprocess.run(["sysctl", "-n", "sysctl.proc_translated"],
                          capture_output=True, text=True)
    if proc.returncode != 0:
        return None
    return proc.stdout.strip() == "1"


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
        ("Configuration: **debug** — `-D"
         + " -D".join(data.get("config_defines") or [])
         + "`, the defines `cmake/config-build.cmake` adds for "
         "`RTS_BUILD_OPTION_DEBUG=ON`. The engine's own `DEBUG_ASSERTCRASH`, `DEBUG_LOG` and "
         "`WWASSERT` guards are compiled in this configuration and compiled out of the release "
         "one, so its figures are not comparable with the release build's and it has a baseline "
         "of its own."
         if data.get("config") == "debug" else
         "Configuration: **release** — the engine's assertions and debug logging are compiled "
         "out, as in every figure published before `--config debug` existed."),
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
        f"(binary produced: {'yes' if data['link_binary_produced'] else 'no'}; linker exited "
        f"{'0' if data['link_clean'] else 'non-zero'} -- unresolved symbols are warnings here, so "
        f"a file being produced does not mean it can run; entry point "
        f"`{data.get('entry_point_symbol', 'main')}` from: "
        + (", ".join(f"`{name}`" for name in data.get("link_entry_point_archives") or [])
           or "this script's stub `main()`, because the game's entry-point target is not in the "
              "selected levels — with it in the build, a missing entry point stops the run "
              "instead of being substituted")
        + (f"; {len(data['link_dropped_entry_points'])} standalone test-tool `main()` "
           f"object(s) removed from the archives first: "
           + ", ".join(f"`{name}`" for name in data["link_dropped_entry_points"])
           if data.get("link_dropped_entry_points") else "")
        + f"). **{data['undefined_total']} distinct symbols "
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
        "## 4. What would resolve them",
        "",
        "The causes above say what each symbol *is*. They do not say what makes it go away, and "
        "the two get confused: before level 4 the renderer's 272 symbols read as port work when "
        "they were only \"the build does not include that layer\". Each unresolved symbol is "
        "therefore also assigned to exactly one pile, and only one of the five is remaining port "
        "work.",
        "",
        "| Pile | Symbols | Meaning |",
        "|---|---:|---|",
    ]
    for pile, meaning in data["pile_meaning"].items():
        lines.append(f"| `{pile}` | {data['undefined_by_pile'].get(pile, 0)} | {meaning} |")
    lines += [
        "",
        "The libraries in the `library-not-linked` and `cut-scope-not-linked` piles, the evidence "
        "each attribution rests on, and the slice that owns it:",
        "",
        "| Library | Pile | Symbols | Evidence files | Why it is not linked | Owner |",
        "|---|---|---:|---:|---|---|",
    ]
    for provider in data["providers"].values():
        lines.append(
            f"| {provider['label']} | `{provider['pile']}` | {provider['symbols']} | "
            f"{provider['evidence_files']} | {provider['reason']} | {provider['owner']} |")
    lines += [
        "",
        "Evidence is the provisioned sources or headers that define the symbols, not a library "
        "found on the measuring machine: the CI container has no FFmpeg or SDL2 runtime, and a "
        "pile split that changed with the box would not be a measurement.",
        "",
    ]

    strict = data.get("strict_link") or {}
    lines += ["## 5. Strict link: is there an executable?", ""]
    if not strict.get("attempted"):
        lines += [
            "Not attempted in this run. `--strict-link` drops "
            "`--warn-unresolved-symbols` and reports the linker's own verdict; §3's file was "
            "produced *with* that tolerance and is not something the loader would accept.",
            "",
        ]
    else:
        lines += [
            f"`--strict-link` linked the same archives with no tolerance for unresolved symbols: "
            f"**{'succeeded' if strict['clean'] else 'failed'}**, "
            f"{strict['unresolved_total']} unresolved symbol(s), executable produced: "
            f"{'yes' if strict['binary_produced'] else 'no'}. Nothing is stubbed to make this "
            "pass, and nothing may be — a green strict link bought with stubs would hide exactly "
            "the work this number exists to count.",
            "",
            "The linker's list and §3's `nm` scan "
            + ("agree, so the categorised list above is the list standing between this build and "
               "an executable."
               if strict["agrees_with_nm"] else
               f"**disagree**: {len(strict['only_in_linker_report'])} symbol(s) only the linker "
               f"reports, {len(strict['only_in_nm_scan'])} only the scan does. Treat that as a bug "
               "in the scan, not as a smaller problem."),
            "",
            "Symbol resolution is necessary and not sufficient; "
            "`docs/porting/startability.md` defines what else a first launch needs.",
            "",
        ]
        binary = strict.get("binary")
        if binary:
            lines += [
                f"The file is `{binary['path']}`, "
                f"{binary['bytes'] / (1024 * 1024):.1f} MiB, "
                f"{binary['format']} {binary['word_size']}-bit {binary['machine']}"
                + (f", `lipo -archs`: {' '.join(binary['lipo_archs'])}"
                   if binary.get("lipo_archs") else "")
                + (", built under Rosetta 2" if data.get("host_translated") else "")
                + (f" (`file`: {binary['file_output']})" if binary.get("file_output") else "")
                + ". That it loads and runs is a separate question from whether it links, and one "
                "this run does not answer: see `docs/porting/startability.md`.",
                "",
            ]

    lines += [
        "## Reproducing",
        "",
        "```sh",
        "bash scripts/ci/fetch-probe-deps.sh",
        f"python3 scripts/native-build.py {' '.join(f'--level {n}' for n in data['levels'])}"
        + (" --with-shims" if data["with_shims"] else "")
        + (f" --config {data['config']}" if data.get("config", "release") != "release" else "")
        + (" --strict-link" if strict.get("attempted") else "")
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
    parser.add_argument("--config", choices=sorted(CONFIG_DEFINES), default="release",
                        help="build configuration, spelled as cmake/config-build.cmake spells it: "
                             "`debug` adds RTS_DEBUG/WWDEBUG/DEBUG, so the engine's own assertions "
                             "and debug logging are compiled (default: release)")
    parser.add_argument("--strict-link", action="store_true",
                        help="also attempt a link with no tolerance for unresolved symbols, i.e. "
                             "an actual executable, and exit non-zero when it fails")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("--examples", type=int, default=15,
                        help="how many example symbols to list per category")
    args = parser.parse_args()

    levels = sorted(set(args.level or [1]))
    config_defines = CONFIG_DEFINES[args.config]
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

    print(f"== generating manifests ({args.config} configuration"
          + (f": -D{' -D'.join(config_defines)}" if config_defines else "") + ")")
    generated_dirs = write_generated_headers(build_dir)
    sources_by_target = write_manifests(targets, manifest_dir, deps_dir,
                                        with_shims=args.with_shims,
                                        generated_dirs=generated_dirs,
                                        extra_defines=config_defines)
    target_by_source = {s: t for t in targets for s in sources_by_target[t.name]}
    all_sources = list(target_by_source)

    lzhl_slug = write_lzhl_manifest(manifest_dir, deps_dir)
    if lzhl_slug is None:
        print("   warning: lzhl sources are not provisioned; LZHL* symbols will stay unresolved")
    zlib_path = zlib_library()
    if zlib_path is None:
        print("   warning: no libz found; compress2/uncompress will stay unresolved")
    audio_slug = write_audio_backend_manifest(manifest_dir, deps_dir)
    if audio_slug is None:
        print("   warning: no <AL/al.h> found; the OpenAL audio backend cannot be built here, so "
              "AIL_* will stay unresolved")
    openal_path = openal_library()
    if openal_path is None:
        print("   warning: no libopenal found; the backend's own al*/alc* calls will stay "
              "unresolved")
    window_slug, window_detail = write_window_backend_manifest(manifest_dir)
    if window_slug is None:
        print(f"   warning: no window backend built ({window_detail}); the 25 WWPlatform::Window_* "
              "entry points will stay unresolved")
        window_source = None
    else:
        window_source = window_detail
        print(f"   window backend: {window_source.name}")
    sdl2_path = sdl2_library() if sys.platform != "darwin" else None
    if window_slug is not None and sys.platform != "darwin" and sdl2_path is None:
        print("   warning: no libSDL2 found; the backend's own SDL_* calls will stay unresolved")
    render_slug, render_detail = write_render_backend_manifest(manifest_dir, build_dir)
    if render_slug is None:
        print(f"   warning: no render backend built ({render_detail}); DX8Wrapper's non-Windows "
              "RenderBackendClass will have no renderer to call")
    vulkan_path = vulkan_library() if render_slug is not None else None
    if render_slug is not None and vulkan_path is None:
        print("   warning: no libvulkan found; the backend's own vk* calls will stay unresolved")
    gitinfo_slug, gitinfo_detail = generate_gitinfo(build_dir, manifest_dir)
    if gitinfo_slug is None:
        print(f"   warning: gitinfo was not generated ({gitinfo_detail});"
              " Git* will stay unresolved")
    else:
        print(f"   gitinfo generated by resources/gitinfo/git_watcher.cmake -> {gitinfo_detail}")
    ffmpeg_paths = ffmpeg_libraries(deps_dir)
    if not ffmpeg_paths:
        print("   warning: the pinned FFmpeg libraries are not built "
              f"({deps_dir / FFMPEG_LIB_SUBDIR}); re-run scripts/ci/fetch-probe-deps.sh, or the "
              "video path's av_*/sws_* calls stay unresolved")
    extra_slugs = [s for s in (lzhl_slug, audio_slug, window_slug, render_slug, gitinfo_slug) if s]

    print(f"== compiling {len(all_sources)} translation units")
    configure(build_dir, manifest_dir, targets, extra_slugs)
    failed, compile_diagnostics = build(build_dir, args.jobs)
    # The support archives are built alongside but are not part of the measurement, so a failure
    # there is a provisioning problem rather than a translation unit this port cannot compile.
    support_failed = {s for s in failed if s not in target_by_source}
    failed = {s for s in failed if s in target_by_source}
    if support_failed:
        print(f"   warning: {len(support_failed)} support-library sources failed to compile: "
              + ", ".join(sorted(str(s.relative_to(REPO_ROOT)) for s in support_failed)))
    print(f"   {len(all_sources) - len(failed)} objects, {len(failed)} failures")

    print("== re-running the probe over the same translation units")
    probe_results = probe_sources(all_sources, target_by_source, deps_dir, args.jobs,
                                  with_shims=args.with_shims, generated_dirs=generated_dirs,
                                  extra_defines=config_defines)

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
    write_manifests(link_targets, manifest_dir, deps_dir, skip=skip, with_shims=args.with_shims,
                    generated_dirs=generated_dirs, extra_defines=config_defines)
    configure(build_dir, manifest_dir, link_targets, extra_slugs)
    second_failed, _ = build(build_dir, args.jobs)
    second_failed = {s for s in second_failed if s in target_by_source}
    if second_failed:
        print(f"   warning: {len(second_failed)} further failures in the second pass")

    all_archives = sorted(build_dir.rglob("*.a"))
    support_slugs = (LZHL_SLUG, AUDIO_BACKEND_SLUG, WINDOW_BACKEND_SLUG, RENDER_BACKEND_SLUG,
                     GITINFO_SLUG)
    support_archives = [a for a in all_archives if a.stem.endswith(support_slugs)]
    archives = [a for a in all_archives if a not in support_archives]
    game_archive = next(
        (a for a in archives if a.stem == f"lib{slug(GAME_ENTRY_TARGET)}"), None)
    dropped_entry_points = (drop_rival_entry_points(archives, game_archive)
                            if game_archive is not None else [])
    if dropped_entry_points:
        print(f"   dropped {len(dropped_entry_points)} test-tool entry points from the archives")
    # The window backend's own platform libraries. SDL2 by path, like OpenAL; on macOS the Cocoa
    # backend needs the frameworks the real build's WWLib target links.
    platform_libraries = [p for p in (sdl2_path,) if p]
    platform_link_args = []
    if window_slug is not None and sys.platform == "darwin":
        for framework in ("AppKit", "QuartzCore", "Metal", "CoreGraphics", "Foundation"):
            platform_link_args += ["-framework", framework]
    link_extra = list(ffmpeg_paths) + platform_libraries + ([vulkan_path] if vulkan_path else [])
    print(f"== linking {len(archives)} archives "
          f"(+{len(support_archives)} support, zlib: {'yes' if zlib_path else 'no'}, "
          f"openal: {'yes' if openal_path else 'no'}, "
          f"ffmpeg: {'yes' if ffmpeg_paths else 'no'}, "
          f"render backend: {'yes' if render_slug else 'no'}, "
          f"window backend: {window_source.name if window_source else 'none'})")
    # Whether the game's own entry-point target is in this build at all. It decides whether a
    # generated stub `main()` is a legitimate anchor or a silent substitution the run must refuse:
    # see `entry_point_anchor`.
    game_target_built = any(t.name == GAME_ENTRY_TARGET for t in link_targets)
    link_ok, binary_produced, _, entry_archives, stub_main = link_probe(
        build_dir, archives, support_archives, link_zlib=zlib_path is not None,
        openal_path=openal_path, extra_libraries=link_extra,
        extra_link_args=platform_link_args, game_target_built=game_target_built)
    if entry_archives:
        print(f"   entry point from {', '.join(entry_archives)} "
              f"(`{platform_symbol(ENTRY_SYMBOL)}`, no stub main)")
    else:
        print(f"   entry point: this script's stub main(), because {GAME_ENTRY_TARGET} is not in "
              "this build")
    strict_referenced_by = None
    strict_ok = strict_binary = False
    strict_binary_info = None
    if args.strict_link:
        print("== linking strictly (no --warn-unresolved-symbols)")
        strict_ok, strict_binary, strict_referenced_by, strict_log = strict_link(
            build_dir, archives, support_archives, link_zlib=zlib_path is not None,
            openal_path=openal_path, extra_libraries=link_extra,
            extra_link_args=platform_link_args, game_target_built=game_target_built)
        # Kept, because a link can fail with zero *unresolved* symbols -- a duplicate definition, a
        # missing framework, a linker crash -- and then the parsed symbol list says nothing at all
        # about why there is no executable.
        strict_log_path = build_dir / "native_strict_link.log"
        strict_log_path.write_text(strict_log)
        if not strict_ok:
            print(f"   linker output: {strict_log_path}")
        strict_binary_info = describe_binary(build_dir / "native_strict_link") \
            if strict_binary else None
        print(f"   strict link {'succeeded' if strict_ok else 'failed'}: "
              f"{len(strict_referenced_by)} unresolved symbol(s), binary produced: "
              f"{'yes' if strict_binary else 'no'}")
        if strict_binary_info:
            print(f"   {strict_binary_info['path']}: "
                  f"{strict_binary_info['bytes'] / (1024 * 1024):.1f} MiB, "
                  f"{strict_binary_info['format']} {strict_binary_info['word_size']}-bit "
                  f"{strict_binary_info['machine']}")
    extra_libraries = [p for p in (zlib_path, openal_path) if p] + link_extra
    symbols = unresolved_symbols(archives, support_archives, extra_libraries,
                                 extra_link_args=platform_link_args)
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
        excluded_backend_names=frozenset(defined_names_in(excluded_backend_sources(window_source))),
        uncompiled_classes=frozenset(defined_classes_in(failed)),
        # A class whose members the failed files define is attributed to them first, so a class
        # split across a built and an unbuilt file is not reported as merely out of scope.
        unbuilt_layer_classes=frozenset(defined_classes_in(unbuilt_sources)),
        well_known_keys_category=well_known_keys_category(keys_owner, failed, target_by_source),
    )

    by_category = collections.Counter()
    per_category_names = collections.defaultdict(list)
    provider_defs = provider_definitions(deps_dir)
    by_pile = collections.Counter()
    per_pile_names = collections.defaultdict(list)
    provider_hits = collections.defaultdict(list)
    for symbol in sorted(symbols):
        name = demangled.get(symbol, symbol)
        category = categorise_symbol(symbol, name, attribution)
        by_category[category] += 1
        per_category_names[category].append(name)
        # Provider evidence outranks the category, which says what a symbol *is* rather than what
        # would resolve it: `MSS_auto_cleanup` categorises as "other / unclassified" and is
        # nonetheless defined by the Miles-API implementation this build does not include.
        provider = provider_for(name, provider_defs)
        pile = pile_for(category, provider)
        by_pile[pile] += 1
        per_pile_names[pile].append(name)
        if provider is not None:
            provider_hits[provider.key].append(name)

    providers = {}
    for provider in PROVIDERS:
        library_names, library_files = provider_library_symbols(provider, LIBRARY_SEARCH_DIRS)
        names, evidence_files = provider_defs[provider.key]
        providers[provider.key] = {
            "label": provider.label,
            "pile": provider.pile,
            "reason": provider.reason,
            "owner": provider.owner,
            # What the attribution rests on. Zero evidence files means the dependency was not
            # provisioned, so its symbols land in another pile: the run under-reports this
            # provider rather than guessing at it.
            "evidence_files": evidence_files,
            "evidence_names": len(names),
            "symbols": len(provider_hits[provider.key]),
            "symbol_names": sorted(provider_hits[provider.key]),
            # Informational: a runtime on this box that exports the same symbols. Never what
            # decides the pile, or the split would differ between CI and a developer's machine.
            "system_libraries": library_files,
            "system_library_symbols_matched": sorted(
                n for n in provider_hits[provider.key]
                if symbol_keys(n, provider.match_unqualified) & library_names),
        }

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
        # True when this process is running under Rosetta 2, i.e. when an "arm64" measurement was
        # taken by an x86-64 translation of the toolchain and the binary it produced is x86-64 too.
        # `sysctl.proc_translated` is the platform's own answer; absent everywhere else.
        "host_translated": host_translated(),
        "levels": levels,
        "with_shims": args.with_shims,
        # Which configuration this is, and the defines that make it one. Recorded because the two
        # configurations' figures are not comparable with each other and each has its own baseline:
        # the debug build compiles code -- assertions, debug logging, profiling hooks -- that the
        # release build never sees, so it has a larger surface and its own failure list.
        "config": args.config,
        "config_defines": list(config_defines),
        "compiled": compiled,
        "translation_units": len(all_sources),
        "objects": len(all_sources) - len(failed),
        "probe_clean": sum(1 for s in all_sources if probe_results.get(s)),
        "link_clean": link_ok,
        "link_binary_produced": binary_produced,
        # Which archives supplied `main`. Empty means the link was anchored by this script's stub
        # entry point, which is only permitted while the game's entry-point target is outside the
        # selected levels -- otherwise the run stops rather than substituting one.
        "link_entry_point_archives": sorted(entry_archives),
        # The symbol namespace every list here is in, measured from the toolchain rather than
        # assumed from the platform: the entry point is spelled `main` under ELF and `_main` under
        # Mach-O, and comparing the two spellings is what made #87's Darwin run substitute a stub.
        "symbol_prefix": symbol_prefix(),
        "entry_point_symbol": platform_symbol(ENTRY_SYMBOL),
        "link_entry_point_stub": stub_main,
        "game_entry_target_built": game_target_built,
        # What was discounted as "the platform provides this", and where that list came from. An
        # empty list is a measurement bug rather than a result, so the run refuses to produce one;
        # recording the sources is what makes the discount auditable per platform.
        "system_symbol_sources": system_symbols()[1],
        "system_symbols_discounted": len(system_symbols()[0]),
        # Standalone test tools inside a measured directory define a `main()` of their own; they
        # are removed from the archives before the link so the game's entry point is unique.
        "link_dropped_entry_points": dropped_entry_points,
        "archives": len(archives),
        # Libraries with no archive at all: every translation unit failed, so the link cannot see
        # them. Tracked separately because "one fewer archive" otherwise looks like progress.
        "libraries_without_archive": sorted(empty_targets),
        # Linked, but deliberately outside every other figure here: dependencies, not translation
        # units whose portability is being measured.
        "third_party_linked": sorted(
            [a.stem for a in support_archives]
            + (["z (system)"] if zlib_path else [])
            + (["openal (system)"] if openal_path else [])
            + (["vulkan (system)"] if vulkan_path else [])),
        "undefined_total": len(symbols),
        "undefined_by_category": dict(by_category.most_common()),
        # The categorised list is the deliverable, so it goes in the machine-readable output in
        # full; the report only quotes examples.
        "undefined_symbols": {
            category: sorted(per_category_names[category]) for category in by_category
        },
        # What would resolve each symbol, which the categories above do not say. Only
        # `no-definition-anywhere` is remaining port work; see PILE_MEANING.
        "undefined_by_pile": {pile: by_pile.get(pile, 0) for pile in PILE_MEANING},
        "undefined_by_pile_symbols": {
            pile: sorted(per_pile_names[pile]) for pile in PILE_MEANING if by_pile.get(pile)
        },
        "pile_meaning": PILE_MEANING,
        # Per omitted library: how many symbols it accounts for, why it is not linked, and which
        # slice owns linking it or excising the calls.
        "providers": providers,
        "compile_failures": {
            str(source.relative_to(REPO_ROOT)): compile_diagnostics.get(source, "")
            for source in sorted(failed)
        },
        "divergence": {
            "probe_clean_compile_failed": probe_clean_compile_failed,
            "probe_failed_compile_ok": probe_failed_compile_ok,
        },
    }

    if strict_referenced_by is not None:
        # The tolerant link's list comes from `nm` over the archives; the strict link's comes from
        # the linker itself. They should agree, and disagreement is the interesting result: it
        # would mean the categorised 412 is not the set standing between here and an executable.
        # The two lists have to be brought into one namespace before they can be compared at all:
        # GNU ld demangles in its diagnostics, ld64 reports the mangled Mach-O spelling, and the
        # `nm` scan has its own. `parse_strict_link_log` has already removed the platform prefix;
        # demangling both sides is the rest of it, and a no-op for a name GNU ld demangled.
        linker_demangled = demangle(sorted(strict_referenced_by))
        reported = {linker_demangled.get(s, s) for s in strict_referenced_by}
        expected = {demangled.get(s, s) for s in symbols}
        data["strict_link"] = {
            "attempted": True,
            "clean": strict_ok,
            "binary_produced": strict_binary,
            "unresolved_total": len(reported),
            # The form both lists are compared in, after normalisation. What the linker itself
            # printed is recorded separately, because that differs per platform.
            "symbol_name_form": "demangled",
            "linker_name_form": STRICT_LINK_NAME_FORM,
            "agrees_with_nm": reported == expected,
            "only_in_linker_report": sorted(reported - expected),
            "only_in_nm_scan": sorted(expected - reported),
            # Which archive member needs each symbol: the answer to "who calls this", which a bare
            # count does not give, and the thing a slice owner needs to excise or implement it.
            "referenced_by": {linker_demangled.get(name, name): member
                              for name, member in sorted(strict_referenced_by.items())},
            # What the file actually is, read out of its own header: the answer to "a binary of
            # what?", which `binary_produced: true` alone does not give. Size is recorded but not
            # gated -- it moves with the toolchain -- while format, word size and machine are.
            "binary": strict_binary_info,
        }
    else:
        data["strict_link"] = {"attempted": False}

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
    print("  " + ", ".join(f"{pile} {count}"
                           for pile, count in data["undefined_by_pile"].items()))

    if strict_referenced_by is not None and not strict_ok:
        # The honest failure is the deliverable, so it is reported as one: the tolerant link above
        # produces a file and exits 0, which is what made "there is no executable" easy to lose.
        print(f"FAIL: strict link: {len(strict_referenced_by)} unresolved symbol(s); no loadable "
              f"executable. {data['undefined_by_pile'][PILE_PORT]} of them are port work, the rest "
              "are a library this configuration does not link, a cut feature, a translation unit "
              "that does not compile yet, or a harness artefact.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
