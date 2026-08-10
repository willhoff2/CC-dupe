#!/usr/bin/env python3
"""Gate the D3D8 surface: no direct IDirect3DDevice8 / IDirect3D8 call outside the allowlist.

PR #8 routes every direct D3D8 call through `DX8Wrapper`'s `DX8CALL*` macros, taking the direct
count from 376 to 0. Nothing stops it drifting back one convenient `D3DDevice->SetTexture(...)`
at a time, which is what this job is for.

The allowlist (spikes/renderer/tools/d3d8-direct-allowlist.json) is a per-file budget of
*direct* call sites -- call sites that bypass the wrapper macros. The check is exact in both
directions:

  * more direct sites in a file than the allowlist permits -> the wrapper was bypassed, fail;
  * fewer -> the allowlist is stale and must be tightened in the same PR that earned it, fail.

Both are fixed by reviewing the diff and running:

    python3 scripts/ci/check-d3d8-surface.py --update

Sites reached through DX8CALL / DX8CALL_HRES / DX8CALL_D3D (and the DX8CALL_RAW* variants) are
not counted here at all: those *are* the wrapper.
"""
import argparse
import collections
import json
import pathlib
import subprocess
import sys
import tempfile

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
SCANNER = REPO_ROOT / "spikes" / "renderer" / "tools" / "d3d8-surface-scan.py"
ALLOWLIST = REPO_ROOT / "spikes" / "renderer" / "tools" / "d3d8-direct-allowlist.json"


def scan():
    """Run the surface scanner and return {file: count} of direct (non-wrapper) call sites."""
    with tempfile.TemporaryDirectory() as tmp:
        out = pathlib.Path(tmp) / "sites.json"
        proc = subprocess.run([sys.executable, str(SCANNER), "--json-out", str(out)],
                              cwd=REPO_ROOT, capture_output=True, text=True)
        if proc.returncode != 0:
            sys.stderr.write(proc.stderr)
            raise SystemExit("the D3D8 surface scanner failed")
        sites = json.loads(out.read_text())
    direct = collections.Counter()
    detail = collections.defaultdict(list)
    for method, entries in sites.items():
        for rel, line, kind in entries:
            if kind != "direct":
                continue
            direct[rel] += 1
            detail[rel].append((line, method))
    return direct, detail


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--update", action="store_true",
                    help="rewrite the allowlist from the current tree")
    args = ap.parse_args()

    direct, detail = scan()
    total = sum(direct.values())

    if args.update:
        existing = json.loads(ALLOWLIST.read_text()) if ALLOWLIST.is_file() else {}
        reasons = {f: e.get("reason", "") for f, e in existing.get("files", {}).items()}
        payload = {k: v for k, v in existing.items() if k.startswith("_")}
        payload.update({
            "total": total,
            "files": {f: {"allowed": n, "reason": reasons.get(f, "TODO: justify this exception")}
                      for f, n in sorted(direct.items())},
        })
        ALLOWLIST.write_text(json.dumps(payload, indent=2) + "\n")
        print(f"wrote {ALLOWLIST.relative_to(REPO_ROOT)}: "
              f"{total} direct call sites across {len(direct)} files")
        return 0

    allow = json.loads(ALLOWLIST.read_text())
    budgets = {f: e["allowed"] for f, e in allow["files"].items()}

    failures = []
    for path in sorted(set(budgets) | set(direct)):
        got, want = direct.get(path, 0), budgets.get(path, 0)
        if got == want:
            continue
        if got > want:
            lines = ", ".join(f"{ln} ({m})" for ln, m in sorted(detail[path])[:8])
            failures.append(f"{path}: {got} direct D3D8 call sites, allowlist permits {want}"
                            f"\n      at lines: {lines}")
        else:
            failures.append(f"{path}: {got} direct D3D8 call sites, allowlist still permits "
                            f"{want} -- tighten the allowlist in this PR")

    print(f"direct (non-wrapper) D3D8 call sites: {total}, allowlist total: {allow['total']}")
    for path, n in sorted(direct.items(), key=lambda kv: -kv[1])[:15]:
        print(f"  {n:4d}  {path}")

    if failures:
        print()
        print(f"FAIL: the D3D8 direct call surface does not match "
              f"{ALLOWLIST.relative_to(REPO_ROOT)}", file=sys.stderr)
        for line in failures:
            print(f"  - {line}", file=sys.stderr)
        print("\nRoute the call through DX8Wrapper (DX8CALL / DX8CALL_HRES / DX8CALL_D3D), or, "
              "if the change is intentional:\n"
              "  python3 scripts/ci/check-d3d8-surface.py --update", file=sys.stderr)
        return 1

    print("\nOK: direct D3D8 call surface matches the allowlist exactly")
    return 0


if __name__ == "__main__":
    sys.exit(main())
