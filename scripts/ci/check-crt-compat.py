#!/usr/bin/env python3
"""Check the CRT / wide-character compatibility layer, in the two ways it can silently rot.

The compile counts in `docs/porting/ci-baselines/` already notice when a compat header stops
working. They do not notice these:

1. **Wrong language linkage.** A helper defined here with C++ linkage, where a vendor SDK declares
   the same name inside `extern "C"`, makes every translation unit that sees both fail with
   "different language linkage". That is not a hypothetical: `_strlwr` against the GameSpy SDK cost
   33 translation units before anyone spotted it, because the SDK header is only reached from some
   of them. Each linkage check below compiles the compat layer *together with* the vendor header
   that declares the same name, in both include orders.
2. **Headers that only work second.** A compat header that quietly depends on something a previous
   include dragged in compiles fine everywhere it is currently used and breaks the first time it is
   used on its own. Each header is compiled standalone.

Run with the same clang and the same fetched SDKs as `scripts/native-port-probe.py`; the flags come
from the probe itself so that the two cannot disagree.
"""
import argparse
import concurrent.futures
import pathlib
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
import native_probe_targets as targets  # noqa: E402

probe = targets.probe
REPO_ROOT = targets.REPO_ROOT
UTILITY_DIR = REPO_ROOT / "Dependencies" / "Utility"
SHIM_DIR = REPO_ROOT / "scripts" / "native-port-shims"

# Helpers the compat layer must keep providing, and how to reach each one. Call expressions rather
# than addresses, because several of these are macros on some platforms and functions on others --
# which is exactly as much as the consumers rely on.
PRESENCE = [
    ("_fpreset", "_fpreset();"),
    ("_statusfp", "unsigned int s = _statusfp(); (void)s;"),
    ("_controlfp", "unsigned int c = _controlfp(_RC_NEAR, _MCW_RC); (void)c;"),
    ("_wtoi", 'int v = _wtoi(L"12"); (void)v;'),
    ("iswascii", "int v = iswascii(L'a'); (void)v;"),
    ("_wcsicmp", 'int v = _wcsicmp(L"a", L"A"); (void)v;'),
    ("_mbsnccnt", 'size_t v = _mbsnccnt((const unsigned char *)"ab", 2); (void)v;'),
    ("_stricmp", 'int v = _stricmp("a", "A"); (void)v;'),
    ("_strlwr", 'char b[] = "A"; _strlwr(b);'),
    ("_strupr", 'char b[] = "a"; _strupr(b);'),
]

# Names a vendor SDK also declares, with the header that declares them and the linkage it uses.
# `extern "C"` here is an assertion, not a fix: if the compat layer disagrees, this fails to
# compile.
LINKAGE = [
    ("_strlwr", "gamespy/gsplatform.h", 'extern "C" char *_strlwr(char *);'),
    ("_strupr", "gamespy/gsplatform.h", 'extern "C" char *_strupr(char *);'),
]


def include_flags(deps_dir, with_shims):
    flags = []
    if with_shims:
        flags.append(f"-I{SHIM_DIR}")
    flags += [f"-I{d}" for d in probe.dep_includes(deps_dir)]
    flags += [f"-I{REPO_ROOT / d}" for d in targets.COMMON_INCLUDES]
    flags.append(f"-I{UTILITY_DIR}")
    return flags


def compile_snippet(name, source, flags, workdir):
    path = pathlib.Path(workdir) / f"{name}.cpp"
    path.write_text(source)
    cmd = [probe.CLANGXX, *probe.CLANG_FLAGS, *flags, str(path)]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    first = ""
    for line in proc.stderr.splitlines():
        if ": error: " in line:
            first = line.split(": error: ", 1)[1]
            break
    return name, proc.returncode == 0, first


def cases(deps_dir):
    """Every check, as (group, name, source, extra include flags)."""
    shimmed = include_flags(deps_dir, with_shims=True)
    unshimmed = include_flags(deps_dir, with_shims=False)

    for symbol, expression in PRESENCE:
        source = f'#include <Utility/compat.h>\nvoid use_{_ident(symbol)}() {{ {expression} }}\n'
        yield "presence", symbol, source, unshimmed

    for symbol, vendor, assertion in LINKAGE:
        # Both orders: a linkage clash is only diagnosed at the second of the two declarations, so
        # one order alone can pass while a consumer that includes them the other way round fails.
        for order, (first, second) in (("compat-first", ("<Utility/compat.h>", f"<{vendor}>")),
                                       ("vendor-first", (f"<{vendor}>", "<Utility/compat.h>"))):
            source = f"#include {first}\n#include {second}\n{assertion}\n"
            yield "linkage", f"{symbol} ({order})", source, unshimmed

    for header in sorted(UTILITY_DIR.glob("Utility/*.h")):
        source = f'#include <Utility/{header.name}>\n#include <Utility/{header.name}>\n'
        yield "standalone-compat", f"Utility/{header.name}", source, unshimmed

    for header in sorted(SHIM_DIR.rglob("*.h")):
        name = header.relative_to(SHIM_DIR).as_posix()
        source = f'#include <{name}>\n#include <{name}>\n'
        yield "standalone-shim", name, source, shimmed


def _ident(symbol):
    return symbol.strip("_").replace(".", "_")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--deps-dir", default=str(targets.DEFAULT_DEPS_DIR),
                    help="build tree holding the SDKs CMake fetches")
    args = ap.parse_args()

    deps_dir = pathlib.Path(args.deps_dir)
    if not deps_dir.is_dir():
        print(f"FAIL: {deps_dir} does not exist; run scripts/ci/fetch-probe-deps.sh first",
              file=sys.stderr)
        return 1

    print(f"compat layer checked with {probe.clang_version()}")
    failures = []
    with tempfile.TemporaryDirectory() as workdir:
        planned = list(cases(deps_dir))
        with concurrent.futures.ThreadPoolExecutor() as pool:
            futures = {}
            for index, (group, name, source, flags) in enumerate(planned):
                future = pool.submit(compile_snippet, f"case{index}", source, flags, workdir)
                futures[future] = (group, name)
            results = by_group(futures)

    for group, entries in results:
        ok = sum(1 for _, good, _ in entries if good)
        print(f"{group:20} {ok}/{len(entries)}")
        for name, good, message in entries:
            if not good:
                failures.append((group, name, message))

    if failures:
        print(f"\nFAIL: {len(failures)} compat check(s) do not compile:", file=sys.stderr)
        for group, name, message in failures:
            print(f"  - [{group}] {name}: {message}", file=sys.stderr)
        return 1
    print("OK: the CRT/wide-character compat layer is self-contained and linkage-clean")
    return 0


def by_group(futures):
    grouped = {}
    for future, (group, name) in futures.items():
        _, good, message = future.result()
        grouped.setdefault(group, []).append((name, good, message))
    return [(group, sorted(entries)) for group, entries in grouped.items()]


if __name__ == "__main__":
    sys.exit(main())
