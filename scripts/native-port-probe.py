#!/usr/bin/env python3
"""Probe how far the codebase is from a native 64-bit clang build.

Runs `clang++ -fsyntax-only` over each translation unit of a set of probe targets and
categorises the resulting diagnostics, so the effort of a native macOS/Linux port can be
estimated from data rather than guesswork.

Two modes, because they answer different questions:

* **native** (default) — nothing stands in for the Windows SDK. Answers "what compiles today
  on a machine with no Windows headers at all?"
* **shimmed** (`--with-shims`) — `scripts/native-port-shims/` supplies declaration-only
  stand-ins for the Win32 headers. Answers "once a platform layer exists, how much of the
  engine's own C++ is portable?" This is the number that sizes the port; the native number
  mostly measures how widely `windows.h` is included.

Translation unit lists come from the CMake source lists, not from `rglob`, so the probe
measures exactly what the real build compiles and does not report on dead files.

By default the renderer, audio, device and entry-point targets are excluded, because their
numbers are much worse and folding them into the headline figure would hide movement in the
engine proper. `--include-renderer` adds them and labels their contribution separately.

Usage:
    python3 scripts/native-port-probe.py [--with-shims] [--report report.md] [--jobs N]
                                        [--json results.json]
"""

import argparse
import collections
import concurrent.futures
import dataclasses
import json
import os
import pathlib
import re
import subprocess
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
SHIM_DIR = REPO_ROOT / "scripts" / "native-port-shims"

# Which clang to probe with. The counts are compiler-version dependent, so CI pins this and
# records the version alongside the results.
CLANGXX = os.environ.get("CLANGXX", "clang++")

# Third-party SDKs the CMake build fetches at configure time (min-dx8-sdk, gamespy, miles,
# lzhl). They are not in the repo, so the probe picks them up from a build tree when one
# exists and reports how many diagnostics are attributable to their absence when it does not.
DEFAULT_DEPS_DIR = REPO_ROOT / "build" / "docker" / "_deps"
FETCHED_DEP_INCLUDES = [
    "dx8-src",
    "gamespy-src/include",
    "lzhl-src",
    "miles-src",
    # The Miles headers live one level down; WWAudio includes them as <mss.h>.
    "miles-src/mss",
    # stb_image_write_impl.cpp includes <stb_image_write.h>.
    "stb-src",
    # The FFmpeg video path includes <libavcodec/avcodec.h> and friends. Headers only: nothing
    # here links the libraries. See docs/porting/video-and-harness-headers.md.
    "ffmpeg-src",
]

# Include dirs shared by every target.
COMMON_INCLUDES = [
    "Dependencies/Utility",
    "Core/Libraries/Include",
    "resources/gitinfo",
]

# NOTE: `Core/Libraries/Source` is deliberately absent from every include list. It contains
# `debug/` and `profile/` subdirectories, which shadow libstdc++'s internal `<debug/...>` and
# `<profile/...>` header directories and produce thousands of spurious errors inside the
# standard library. Per-library include paths, never a blanket one.
WWVEGAS_INCLUDES = [
    "Core/Libraries/Source/WWVegas",
    "Core/Libraries/Source/WWVegas/WWMath",
    "Core/Libraries/Source/WWVegas/WWLib",
    "Core/Libraries/Source/WWVegas/WWDebug",
    "Core/Libraries/Source/WWVegas/WWSaveLoad",
    "Core/Libraries/Source/Compression",
]

# The two GameEngine libraries are one logical library split across two directories: every
# translation unit of either is compiled with both include trees on the path and with
# `Include/Precompiled/PreRTS.h` reachable, exactly as the CMake targets arrange it.
GAMEENGINE_INCLUDES = WWVEGAS_INCLUDES + [
    "Core/GameEngine/Include",
    "GeneralsMD/Code/GameEngine/Include",
    "GeneralsMD/Code/GameEngine/Include/Precompiled",
    "GeneralsMD/Code/Libraries/Source/WWVegas",
]


@dataclasses.dataclass(frozen=True)
class Target:
    """A probe target: a set of translation units plus the include paths they need."""

    name: str
    includes: tuple
    # Either a CMake list to read the translation units from...
    cmake_lists: str = None
    cmake_root: str = None
    # ...or directories to walk.
    source_dirs: tuple = ()
    defines: tuple = ()


TARGETS = [
    Target(
        name="Core/Libraries/Source/Compression",
        includes=tuple(WWVEGAS_INCLUDES),
        source_dirs=("Core/Libraries/Source/Compression",),
    ),
    Target(
        name="Core/Libraries/Source/WWVegas/WWMath",
        includes=tuple(WWVEGAS_INCLUDES),
        source_dirs=("Core/Libraries/Source/WWVegas/WWMath",),
    ),
    Target(
        name="Core/Libraries/Source/WWVegas/WWLib",
        includes=tuple(WWVEGAS_INCLUDES),
        source_dirs=("Core/Libraries/Source/WWVegas/WWLib",),
    ),
    Target(
        name="Core/Libraries/Source/WWVegas/WWDebug",
        includes=tuple(WWVEGAS_INCLUDES),
        source_dirs=("Core/Libraries/Source/WWVegas/WWDebug",),
    ),
    Target(
        name="Core/Libraries/Source/WWVegas/WWSaveLoad",
        includes=tuple(WWVEGAS_INCLUDES),
        source_dirs=("Core/Libraries/Source/WWVegas/WWSaveLoad",),
    ),
    Target(
        name="Core/Libraries/Source/debug",
        includes=tuple(WWVEGAS_INCLUDES),
        source_dirs=("Core/Libraries/Source/debug",),
    ),
    Target(
        name="Core/Libraries/Source/profile",
        includes=tuple(WWVEGAS_INCLUDES),
        source_dirs=("Core/Libraries/Source/profile",),
    ),
    Target(
        name="Core/GameEngine",
        includes=tuple(GAMEENGINE_INCLUDES),
        cmake_lists="Core/GameEngine/CMakeLists.txt",
        cmake_root="Core/GameEngine",
        defines=("RTS_ZEROHOUR=1",),
    ),
    Target(
        name="GeneralsMD/Code/GameEngine",
        includes=tuple(GAMEENGINE_INCLUDES),
        cmake_lists="GeneralsMD/Code/GameEngine/CMakeLists.txt",
        cmake_root="GeneralsMD/Code/GameEngine",
        defines=("RTS_ZEROHOUR=1",),
    ),
]

# Renderer, audio, device and entry-point code. Kept out of the default target list on purpose:
# the numbers above are tracked over time, and silently widening their scope would make the
# history meaningless. Enable with --include-renderer and read the figure as its own thing.
#
# These translation units have never been compiled off Windows. They are the code most likely to
# be unportable (Direct3D 8, DirectSound/Miles, Bink, WinMain), so a bad number here is expected
# and is still worth having measured.

# `corei_ww3d2` and the other Core renderer/device libraries are CMake INTERFACE libraries: their
# sources are compiled as part of the game-specific targets, with the game-specific include tree
# ahead of the Core one (that is how the game copies of e.g. `w3d_file.h`, which Core has no copy
# of, get found). The probe mirrors that ordering rather than compiling Core in isolation.
GAME_RENDERER_INCLUDES = [
    "GeneralsMD/Code/Libraries/Source/WWVegas",
    "GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2",
    "GeneralsMD/Code/Libraries/Source/WWVegas/WWAudio",
    "GeneralsMD/Code/Libraries/Source/WWVegas/WWDownload",
]

RENDERER_INCLUDES = GAME_RENDERER_INCLUDES + WWVEGAS_INCLUDES + [
    "Core/Libraries/Source/WWVegas/WW3D2",
    "Core/Libraries/Source/WWVegas/WWAudio",
    "Core/Libraries/Source/WWVegas/WWDownload",
]

DEVICE_INCLUDES = GAME_RENDERER_INCLUDES + GAMEENGINE_INCLUDES + [
    "Core/Libraries/Source/WWVegas/WW3D2",
    "Core/Libraries/Source/WWVegas/WWAudio",
    "Core/GameEngineDevice/Include",
    "GeneralsMD/Code/GameEngineDevice/Include",
]

RENDERER_TARGETS = [
    Target(
        name="Core/Libraries/Source/WWVegas/WW3D2",
        includes=tuple(RENDERER_INCLUDES),
        source_dirs=("Core/Libraries/Source/WWVegas/WW3D2",),
    ),
    Target(
        name="Core/Libraries/Source/WWVegas/WWAudio",
        includes=tuple(RENDERER_INCLUDES),
        source_dirs=("Core/Libraries/Source/WWVegas/WWAudio",),
    ),
    Target(
        name="Core/Libraries/Source/WWVegas/WWDownload",
        includes=tuple(RENDERER_INCLUDES),
        source_dirs=("Core/Libraries/Source/WWVegas/WWDownload",),
    ),
    Target(
        name="GeneralsMD/Code/Libraries/Source/WWVegas",
        includes=tuple(RENDERER_INCLUDES),
        source_dirs=("GeneralsMD/Code/Libraries/Source/WWVegas",),
    ),
    Target(
        name="Core/GameEngineDevice",
        includes=tuple(DEVICE_INCLUDES),
        cmake_lists="Core/GameEngineDevice/CMakeLists.txt",
        cmake_root="Core/GameEngineDevice",
        defines=("RTS_ZEROHOUR=1",),
    ),
    Target(
        name="GeneralsMD/Code/GameEngineDevice",
        includes=tuple(DEVICE_INCLUDES),
        cmake_lists="GeneralsMD/Code/GameEngineDevice/CMakeLists.txt",
        cmake_root="GeneralsMD/Code/GameEngineDevice",
        defines=("RTS_ZEROHOUR=1",),
    ),
    Target(
        name="GeneralsMD/Code/Main",
        includes=tuple(DEVICE_INCLUDES) + ("GeneralsMD/Code/Main",),
        source_dirs=("GeneralsMD/Code/Main",),
        defines=("RTS_ZEROHOUR=1",),
    ),
]

CLANG_FLAGS = [
    "-fsyntax-only",
    "-std=c++20",
    "-m64",
    "-ferror-limit=0",
    # Keeps `__int64`, `__forceinline` and friends available; the codebase relies on them
    # pervasively and replacing them is a separate mechanical pass.
    "-fms-extensions",
    # Mirrors the `/FIUtility/CppMacros.h` force-include the MSVC build uses.
    "-include",
    "Utility/CppMacros.h",
    "-DWIN32_LEAN_AND_MEAN",
    "-D_REENTRANT",
]

# Ordered: the first pattern that matches a diagnostic wins.
CATEGORIES = [
    ("Missing Win32 headers", re.compile(
        r"'(windows|windef|winbase|winsock2?|wtypes|objbase|ocidl|oleauto|mmsystem|tchar|io|"
        r"direct|excpt|process|crtdbg|dbghelp|shlobj|shlguid|shellapi|snmp|winreg|wingdi|"
        r"winuser|winerror|wininet|winsvc|imagehlp|lmcons|ddraw|dinput|dsound|vfw|atlbase|"
        r"new|basetsd|intrin|malloc)\.h' file not found", re.I)),
    ("Missing fetched SDK headers (dx8 / gamespy / miles / lzhl)", re.compile(
        r"'(d3d\w*|dx\w*|mss\w*|gamespy/[\w/]+|CompLibHeader/\w+)\.h' file not found", re.I)),
    ("Missing generated headers (IDL / build-time)", re.compile(
        r"'(EABrowserDispatch/\w+|BrowserEngine/\w+|gitinfo)\.h' file not found", re.I)),
    ("Missing project/vendor headers", re.compile(r"file not found")),
    ("Inline x86 assembly", re.compile(r"__asm|inline assembly|asm-specifier|expected \(")),
    ("MSVC calling conventions / declspec", re.compile(
        r"__stdcall|__cdecl|__fastcall|__declspec|__forceinline|calling convention")),
    ("64-bit size/layout assumptions", re.compile(
        r"cast (to|from) .*pointer .*(different|smaller) size|"
        r"loses precision|"
        r"static_assert failed.*(size|SIZE)|"
        r"'(int|long|unsigned int|DWORD)' (to|from) '[^']*\*'")),
    ("Win32 types undeclared", re.compile(
        r"unknown type name '(HWND|HANDLE|DWORD|HINSTANCE|LPCSTR|LPSTR|BOOL|WORD|BYTE|UINT|"
        r"LRESULT|WPARAM|LPARAM|HDC|HRESULT|FARPROC|CRITICAL_SECTION|LARGE_INTEGER|FILETIME|"
        r"HMODULE)'")),
    ("Win32 / MSVC identifiers undeclared", re.compile(
        r"use of undeclared identifier '(_|[A-Z]{2,}_)")),
    ("Non-conforming template/name lookup", re.compile(
        r"use of undeclared identifier|no template named|missing 'typename'|"
        r"dependent name|must use 'template'|no member named")),
    ("MSVC pragmas / extensions", re.compile(r"#pragma|unknown pragma|__int64|__int32|__based")),
    ("STL / STLport mismatch", re.compile(r"stlport|std::|namespace 'std'", re.I)),
]


@dataclasses.dataclass
class FileResult:
    target: str
    path: str
    ok: bool
    diagnostics: list  # list of (category, message)


def categorise(message):
    for name, pattern in CATEGORIES:
        if pattern.search(message):
            return name
    return "Other"


def dep_includes(deps_dir):
    """Include dirs for the SDKs CMake fetches, for whichever of them are present."""
    if deps_dir is None:
        return []
    return [str(deps_dir / d) for d in FETCHED_DEP_INCLUDES if (deps_dir / d).is_dir()]


def probe(job):
    target, source, extra_includes, with_shims = job
    # Shims first so they take precedence over anything a build tree happens to provide, and
    # the fetched SDKs next, ahead of the repo's own trees.
    includes = []
    if with_shims:
        includes.append(str(SHIM_DIR))
    includes.extend(extra_includes)
    includes.extend(str(REPO_ROOT / d) for d in COMMON_INCLUDES)
    includes.extend(str(REPO_ROOT / d) for d in target.includes)

    cmd = [CLANGXX, *CLANG_FLAGS]
    cmd += [f"-D{d}" for d in target.defines]
    cmd += [f"-I{d}" for d in includes]
    cmd.append(str(source))
    proc = subprocess.run(cmd, capture_output=True, text=True)
    diagnostics = []
    for line in proc.stderr.splitlines():
        if ": error:" not in line and ": fatal error:" not in line:
            continue
        message = line.split(": error:", 1)[-1].split(": fatal error:", 1)[-1].strip()
        diagnostics.append((categorise(line), message))
    rel = str(source.relative_to(REPO_ROOT))
    return FileResult(target.name, rel, proc.returncode == 0, diagnostics)


CMAKE_SOURCE_RE = re.compile(r"^\s+(Source/\S+\.cpp)\s*$")

# Backends that are opt-in in CMake because they need a dependency this probe deliberately does
# not have on its include path (SDL2). They are not part of "how much of the engine compiles
# natively": the answer for them is "only with their dependency present", which the spike's own
# build answers instead. See docs/porting/window-event-loop.md.
OPTIONAL_BACKENDS = {"platform_window_sdl2.cpp"}

# Sources CMake compiles only in the *other* branch of a mutually exclusive option, and which
# therefore cannot compile in the configuration being measured. GameMemoryNull.cpp is the whole
# list: cmake/config-memory.cmake defaults RTS_GAMEMEMORY_ENABLE to ON, so the build compiles
# GameMemory.cpp, and GameMemoryNull.h redefines DynamicMemoryAllocator, MemoryPoolFactory and
# MemoryPoolObject -- classes PreRTS.h has already supplied through GameMemory.h. Counting it as a
# port blocker measured the harness rather than the code; it appeared as one from the first probe
# run until this exclusion.
EXCLUSIVE_ALTERNATIVES = {"GameMemoryNull.cpp"}


def is_measured_source(path):
    """Whether a source belongs to the configuration the probe measures."""
    return path.name not in OPTIONAL_BACKENDS and path.name not in EXCLUSIVE_ALTERNATIVES


def targets(include_renderer):
    return TARGETS + RENDERER_TARGETS if include_renderer else TARGETS


def cmake_sources(target):
    """Translation units CMake actually compiles, i.e. list entries that are not commented out."""
    text = (REPO_ROOT / target.cmake_lists).read_text().splitlines()
    root = REPO_ROOT / target.cmake_root
    sources = []
    for line in text:
        match = CMAKE_SOURCE_RE.match(line)
        if not match:
            continue
        path = root / match.group(1)
        if path.is_file() and is_measured_source(path):
            sources.append(path)
    return sorted(set(sources))


def collect_jobs(deps_dir, with_shims, include_renderer=False):
    extra = tuple(dep_includes(deps_dir))
    jobs = []
    for target in targets(include_renderer):
        if target.cmake_lists:
            sources = cmake_sources(target)
        else:
            sources = []
            for directory in target.source_dirs:
                sources.extend(sorted(path for path in (REPO_ROOT / directory).rglob("*.cpp")
                                      if is_measured_source(path)))
        jobs.extend((target, source, extra, with_shims) for source in sources)
    return jobs


def render_report(results, with_shims, deps_dir, deps_present, include_renderer=False):
    total = len(results)
    clean = [r for r in results if r.ok]
    by_category = collections.Counter()
    files_by_category = collections.defaultdict(set)
    examples = {}
    for result in results:
        for category, message in result.diagnostics:
            by_category[category] += 1
            files_by_category[category].add(result.path)
            examples.setdefault(category, f"{result.path}: {message}")

    mode = "shimmed" if with_shims else "native (no Windows SDK)"
    lines = [
        f"# Native 64-bit clang probe — {mode}",
        "",
        f"Compiled {total} translation units with "
        f"`clang++ {' '.join(CLANG_FLAGS)}` (no Windows SDK, no Wine, no MSVC).",
        "",
    ]
    if include_renderer:
        renderer_names = {t.name for t in RENDERER_TARGETS}
        renderer = [r for r in results if r.target in renderer_names]
        renderer_clean = sum(1 for r in renderer if r.ok)
        lines += [
            f"Scope: **extended**. The renderer/audio/device/entry-point targets "
            f"({', '.join(sorted(renderer_names))}) are included; they are excluded by default. "
            f"They contribute **{renderer_clean} / {len(renderer)}** of the totals below, and "
            "have never been compiled off Windows at all.",
            "",
        ]
    if with_shims:
        lines += [
            "Mode: **shimmed**. `scripts/native-port-shims/` supplies declaration-only "
            "stand-ins for the Win32 headers `PreRTS.h` pulls into every GameEngine "
            "translation unit, so the numbers below measure the engine's *own* C++ rather "
            "than the absence of `windows.h`.",
            "",
        ]
    else:
        lines += [
            "Mode: **native**. Nothing stands in for the Windows SDK.",
            "",
        ]
    if deps_present:
        lines += [
            f"Fetched SDK headers (dx8, gamespy, miles, lzhl) taken from `{deps_dir}`: "
            f"{', '.join(deps_present)}.",
            "",
        ]
    else:
        lines += [
            "The SDKs CMake fetches at configure time (min-dx8-sdk, gamespy, miles, lzhl) "
            "were not available, so diagnostics from their absence are counted separately. "
            "Configure a build tree, or pass `--deps-dir`, to fold them in.",
            "",
        ]

    lines += [
        f"- Translation units that compile clean: **{len(clean)} / {total}** "
        f"({len(clean) * 100 // max(total, 1)}%)",
        f"- Translation units with errors: **{total - len(clean)}**",
        f"- Total errors: **{sum(by_category.values())}**",
        "",
        "## Errors by category",
        "",
        "| Category | Errors | Files | Example |",
        "|---|---:|---:|---|",
    ]
    for category, count in by_category.most_common():
        example = examples[category].replace("|", "\\|")[:110]
        lines.append(f"| {category} | {count} | {len(files_by_category[category])} | `{example}` |")

    lines += ["", "## Per-target breakdown", "",
              "| Target | Clean | Total | Clean % |", "|---|---:|---:|---:|"]
    per_target = collections.OrderedDict((t.name, [0, 0]) for t in targets(include_renderer))
    for result in results:
        per_target[result.target][1] += 1
        if result.ok:
            per_target[result.target][0] += 1
    for name, (ok, count) in per_target.items():
        pct = ok * 100 // count if count else 0
        lines.append(f"| {name} | {ok} | {count} | {pct}% |")

    failing = [r for r in results if not r.ok]
    if failing:
        lines += ["", "## Translation units with errors", "",
                  "| Translation unit | Errors | First diagnostic |", "|---|---:|---|"]
        for result in sorted(failing, key=lambda r: (-len(r.diagnostics), r.path)):
            first = result.diagnostics[0][1].replace("|", "\\|")[:100] if result.diagnostics else ""
            lines.append(f"| `{result.path}` | {len(result.diagnostics)} | `{first}` |")

    lines.append("")
    return "\n".join(lines)


def clang_version():
    """Major version of the clang the probe ran with, e.g. '14'."""
    try:
        out = subprocess.run([CLANGXX, "-dumpversion"], capture_output=True, text=True).stdout
    except OSError:
        return "unknown"
    return out.strip().split(".")[0] or "unknown"


def render_json(results, with_shims, deps_present, include_renderer=False):
    """Machine-readable summary, for the CI baseline gate."""
    per_target = collections.OrderedDict(
        (t.name, {"clean": 0, "total": 0}) for t in targets(include_renderer))
    for result in results:
        per_target[result.target]["total"] += 1
        if result.ok:
            per_target[result.target]["clean"] += 1
    return {
        "mode": ("shimmed" if with_shims else "native") + ("+renderer" if include_renderer else ""),
        "clang_major": clang_version(),
        "deps_present": sorted(deps_present),
        "clean": sum(1 for r in results if r.ok),
        "total": len(results),
        "targets": per_target,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", default="native-port-probe.md")
    parser.add_argument("--json", dest="json_out",
                        help="also write a machine-readable summary here (used by CI)")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("--with-shims", action="store_true",
                        help="put scripts/native-port-shims/ on the include path")
    parser.add_argument("--include-renderer", action="store_true",
                        help="also probe WW3D2, WWAudio, WWDownload, GameEngineDevice and Main, "
                             "which are excluded by default because they widen the scope of the "
                             "headline numbers")
    parser.add_argument("--deps-dir", default=str(DEFAULT_DEPS_DIR),
                        help="CMake FetchContent _deps directory to take dx8/gamespy/miles/lzhl "
                             "headers from")
    args = parser.parse_args()

    deps_dir = pathlib.Path(args.deps_dir) if args.deps_dir else None
    if deps_dir and not deps_dir.is_dir():
        deps_dir = None
    deps_present = [d for d in FETCHED_DEP_INCLUDES if deps_dir and (deps_dir / d).is_dir()]

    jobs = collect_jobs(deps_dir, args.with_shims, args.include_renderer)
    if not jobs:
        print("No sources found", file=sys.stderr)
        return 1

    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        results = list(pool.map(probe, jobs))

    report = render_report(results, args.with_shims, deps_dir, deps_present,
                           args.include_renderer)
    pathlib.Path(args.report).write_text(report)
    if args.json_out:
        pathlib.Path(args.json_out).write_text(
            json.dumps(render_json(results, args.with_shims, deps_present,
                                   args.include_renderer), indent=2) + "\n")
    clean = sum(1 for r in results if r.ok)
    print(f"{clean} / {len(results)} translation units clean; report written to {args.report}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
