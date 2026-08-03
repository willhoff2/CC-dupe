#!/usr/bin/env python3
"""Probe how far the platform-independent libraries are from a native 64-bit clang build.

Runs `clang++ -fsyntax-only` over each translation unit in a set of candidate libraries and
categorises the resulting diagnostics, so the effort of a native macOS/Linux port can be
estimated from data rather than guesswork.

Usage:
    python3 scripts/native-port-probe.py [--report report.md] [--jobs N]
"""

import argparse
import collections
import concurrent.futures
import dataclasses
import os
import pathlib
import re
import subprocess
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent

# Libraries with no inherent dependency on Direct3D, Miles or the Win32 GUI, and therefore the
# cheapest slice to make portable first.
CANDIDATE_DIRS = [
    "Core/Libraries/Source/Compression",
    "Core/Libraries/Source/WWVegas/WWMath",
    "Core/Libraries/Source/WWVegas/WWLib",
    "Core/Libraries/Source/WWVegas/WWDebug",
    "Core/Libraries/Source/WWVegas/WWSaveLoad",
    "Core/Libraries/Source/debug",
    "Core/Libraries/Source/profile",
]

# NOTE: `Core/Libraries/Source` is deliberately absent. It contains `debug/` and `profile/`
# subdirectories, which shadow libstdc++'s internal `<debug/...>` and `<profile/...>` header
# directories and produce thousands of spurious errors inside the standard library.
INCLUDE_DIRS = [
    "Dependencies/Utility",
    "Core/Libraries/Include",
    "Core/Libraries/Source/WWVegas",
    "Core/Libraries/Source/WWVegas/WWMath",
    "Core/Libraries/Source/WWVegas/WWLib",
    "Core/Libraries/Source/WWVegas/WWDebug",
    "Core/Libraries/Source/WWVegas/WWSaveLoad",
    "Core/Libraries/Source/Compression",
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
        r"'(windows|windef|winbase|winsock2?|wtypes|objbase|mmsystem|tchar|io|direct|excpt|"
        r"process|crtdbg|dbghelp|shlobj|winreg|wingdi|winuser|imagehlp|ddraw|d3d\w*|dinput|"
        r"dsound|mss\w*|basetsd|intrin|malloc)\.h' file not found", re.I)),
    ("Missing project/vendor headers", re.compile(r"file not found")),
    ("Inline x86 assembly", re.compile(r"__asm|inline assembly|asm-specifier|expected \(")),
    ("MSVC calling conventions / declspec", re.compile(
        r"__stdcall|__cdecl|__fastcall|__declspec|__forceinline|calling convention")),
    ("64-bit pointer/int assumptions", re.compile(
        r"cast (to|from) .*pointer .*(different|smaller) size|"
        r"loses precision|"
        r"'(int|long|unsigned int|DWORD)' (to|from) '[^']*\*'")),
    ("Win32 types undeclared", re.compile(
        r"unknown type name '(HWND|HANDLE|DWORD|HINSTANCE|LPCSTR|LPSTR|BOOL|WORD|BYTE|UINT|"
        r"LRESULT|WPARAM|LPARAM|HDC|HRESULT|CRITICAL_SECTION|LARGE_INTEGER|FILETIME|HMODULE)'")),
    ("Non-conforming template/name lookup", re.compile(
        r"use of undeclared identifier|no template named|missing 'typename'|"
        r"dependent name|must use 'template'|no member named")),
    ("MSVC pragmas / extensions", re.compile(r"#pragma|unknown pragma|__int64|__int32|__based")),
    ("STL / STLport mismatch", re.compile(r"stlport|std::|namespace 'std'", re.I)),
]


@dataclasses.dataclass
class FileResult:
    path: str
    ok: bool
    diagnostics: list  # list of (category, message)


def include_flags():
    return [f"-I{REPO_ROOT / d}" for d in INCLUDE_DIRS]


def categorise(message):
    for name, pattern in CATEGORIES:
        if pattern.search(message):
            return name
    return "Other"


def probe(source):
    cmd = ["clang++", *CLANG_FLAGS, *include_flags(), str(source)]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    diagnostics = []
    for line in proc.stderr.splitlines():
        if ": error:" not in line and ": fatal error:" not in line:
            continue
        message = line.split(": error:", 1)[-1].split(": fatal error:", 1)[-1].strip()
        diagnostics.append((categorise(line), message))
    rel = str(source.relative_to(REPO_ROOT))
    return FileResult(rel, proc.returncode == 0, diagnostics)


def collect_sources():
    sources = []
    for directory in CANDIDATE_DIRS:
        sources.extend(sorted((REPO_ROOT / directory).rglob("*.cpp")))
    return sources


def render_report(results):
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

    lines = [
        "# Native 64-bit clang probe — platform-independent libraries",
        "",
        f"Compiled {total} translation units with "
        f"`clang++ {' '.join(CLANG_FLAGS)}` (no Windows SDK, no Wine).",
        "",
        f"- Translation units that already compile clean: **{len(clean)} / {total}** "
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

    lines += ["", "## Per-library breakdown", "", "| Library | Clean | Total |", "|---|---:|---:|"]
    per_lib = collections.defaultdict(lambda: [0, 0])
    for result in results:
        for directory in CANDIDATE_DIRS:
            if result.path.startswith(directory):
                per_lib[directory][1] += 1
                if result.ok:
                    per_lib[directory][0] += 1
                break
    for directory, (ok, count) in sorted(per_lib.items()):
        lines.append(f"| {directory} | {ok} | {count} |")

    lines += ["", "## Translation units already clean", ""]
    lines += [f"- `{r.path}`" for r in clean] or ["- (none)"]
    lines.append("")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", default="native-port-probe.md")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    args = parser.parse_args()

    sources = collect_sources()
    if not sources:
        print("No sources found", file=sys.stderr)
        return 1

    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        results = list(pool.map(probe, sources))

    report = render_report(results)
    pathlib.Path(args.report).write_text(report)
    print(report)
    return 0


if __name__ == "__main__":
    sys.exit(main())
