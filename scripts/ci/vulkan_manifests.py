#!/usr/bin/env python3
"""Vulkan ICD and layer manifests with absolute `library_path`s, for macOS.

Homebrew's `MoltenVK_icd.json` and `VkLayer_khronos_validation.json` name their dylibs by leaf
name, so the Vulkan loader only finds them if the dynamic loader can: in practice, with
`DYLD_LIBRARY_PATH` pointing at the kegs' `lib` directories. That does not survive on macOS.
`DYLD_*` is stripped from the environment whenever a SIP-protected binary is exec'd, and
`/bin/bash` and `/usr/bin/python3` both are -- so a `scripts/ci/*.py` gate launched from a
workflow step loses the variable before it ever reaches the spike binary, and the loader then
either fails with `VK_ERROR_LAYER_NOT_PRESENT` (-6) or, worse, quietly builds an instance with no
layer on it and the run reports `validation messages: 0` having validated nothing.
docs/porting/apple-silicon-verification.md 8.1 has the measurement.

The fix is to stop relying on the dynamic loader's search path: rewrite each manifest with an
absolute `library_path` into a scratch directory and point `VK_ICD_FILENAMES` / `VK_LAYER_PATH`
at the rewritten copies. Those two variables are not `DYLD_*` and are not stripped.

Used two ways:

  * imported by the `scripts/ci/check-*.py` gates, which build their child's environment with
    `child_environment()` so the layer survives the exec they perform;
  * run as `python3 scripts/ci/vulkan_manifests.py --github-env` in a workflow step, which
    writes the same variables to `$GITHUB_ENV` for the steps that launch a binary directly.

On anything other than macOS this is a no-op: the Linux packages install manifests whose
libraries are on the default loader path, and `--self-check` still exercises the rewriting there.
"""
import argparse
import json
import os
import pathlib
import shutil
import subprocess
import sys

# Where a Homebrew keg's dylib lives when the manifest names it by leaf name. Ordered: the
# formula's own lib directory is tried first, through `brew --prefix <formula>`.
FALLBACK_LIB_DIRS = ("/opt/homebrew/lib", "/usr/local/lib")

LAYER_FORMULA = "vulkan-validationlayers"
ICD_FORMULA = "molten-vk"

DEFAULT_OUT_DIR = pathlib.Path("build/vulkan-manifests")


def brew_prefix(formula=None):
    """-> pathlib.Path or None. `brew --prefix [formula]`, without raising when brew is absent."""
    if shutil.which("brew") is None:
        return None
    command = ["brew", "--prefix"] + ([formula] if formula else [])
    try:
        out = subprocess.run(command, capture_output=True, text=True, check=True).stdout.strip()
    except (subprocess.CalledProcessError, OSError):
        return None
    path = pathlib.Path(out) if out else None
    return path if path is not None and path.is_dir() else None


def find_manifest(prefix, leaf_name):
    """-> pathlib.Path or None. The one manifest named `leaf_name` under a keg.

    Kegs are not linked into `$(brew --prefix)/share`, and molten-vk has moved its manifest
    between releases, so the file is located rather than assumed.
    """
    if prefix is None:
        return None
    for candidate in sorted(prefix.rglob(leaf_name)):
        return candidate
    return None


def resolve_library(library_path, manifest_dir, extra_dirs=()):
    """-> pathlib.Path or None. The dylib a manifest's `library_path` names.

    Manifest `library_path`s are, per the loader's spec, either absolute, relative to the
    manifest, or a bare name to be handed to the dynamic loader. The third case is the one that
    breaks under SIP, and the one this resolves against real directories.
    """
    named = pathlib.Path(library_path)
    if named.is_absolute():
        return named if named.exists() else None
    beside = manifest_dir / named
    if beside.exists():
        return beside.resolve()
    for directory in list(extra_dirs) + list(FALLBACK_LIB_DIRS):
        candidate = pathlib.Path(directory) / named.name
        if candidate.exists():
            return candidate.resolve()
    return None


def rewrite_manifest(manifest, out_dir, extra_dirs=()):
    """Copy `manifest` to `out_dir` with an absolute `library_path`. -> the copy's path.

    Raises FileNotFoundError when the library it names cannot be found, which is the honest
    outcome: a manifest whose dylib is missing is exactly the silent-no-layer case.
    """
    manifest = pathlib.Path(manifest)
    document = json.loads(manifest.read_text())
    # An ICD manifest keys its object "ICD", a layer manifest keys it "layer", and a
    # multi-layer manifest keys "layers". Every one of them holds a library_path.
    if "ICD" in document:
        holders = [document["ICD"]]
    elif "layer" in document:
        holders = [document["layer"]]
    elif "layers" in document:
        holders = list(document["layers"])
    else:
        raise ValueError(f"{manifest}: neither an ICD nor a layer manifest")

    for holder in holders:
        named = holder.get("library_path")
        if named is None:
            continue
        resolved = resolve_library(named, manifest.parent, extra_dirs)
        if resolved is None:
            raise FileNotFoundError(f"{manifest}: cannot find the library it names, {named!r}")
        holder["library_path"] = str(resolved)

    out_dir = pathlib.Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    written = out_dir / manifest.name
    written.write_text(json.dumps(document, indent=2) + "\n")
    return written


def prepare(out_dir=DEFAULT_OUT_DIR, require_layer=False):
    """-> ({env var: value}, [note]). Empty off macOS, and never raises for a missing formula."""
    variables = {}
    notes = []
    if sys.platform != "darwin":
        return variables, notes

    layer_prefix = brew_prefix(LAYER_FORMULA)
    layer_manifest = find_manifest(layer_prefix, "VkLayer_khronos_validation.json")
    if layer_manifest is None:
        note = f"no VkLayer_khronos_validation.json under {LAYER_FORMULA}"
        if require_layer:
            raise FileNotFoundError(note)
        notes.append(note)
    else:
        lib_dirs = [str(layer_prefix / "lib")] if layer_prefix else []
        written = rewrite_manifest(layer_manifest, out_dir, lib_dirs)
        # VK_LAYER_PATH is a directory list, and the rewritten copy has to be the only entry:
        # with the original directory also listed the loader can pick the relative manifest
        # again, which is the bug.
        variables["VK_LAYER_PATH"] = str(written.parent.resolve())
        notes.append(f"layer manifest rewritten to {written}")

    icd_prefix = brew_prefix(ICD_FORMULA)
    icd_manifest = find_manifest(icd_prefix, "MoltenVK_icd.json")
    if icd_manifest is None:
        notes.append(f"no MoltenVK_icd.json under {ICD_FORMULA}")
    else:
        lib_dirs = [str(icd_prefix / "lib")] if icd_prefix else []
        written = rewrite_manifest(icd_manifest, out_dir, lib_dirs)
        variables["VK_ICD_FILENAMES"] = str(written.resolve())
        notes.append(f"ICD manifest rewritten to {written}")

    return variables, notes


def child_environment(env=None, out_dir=DEFAULT_OUT_DIR):
    """-> a copy of `env` a child can load the layer and driver from.

    Anything already set in the environment wins: a workflow that has resolved the manifests
    itself, or a developer pointing at a Vulkan SDK, is not overridden.
    """
    environment = dict(os.environ if env is None else env)
    try:
        variables, notes = prepare(out_dir)
    except (FileNotFoundError, ValueError, OSError) as error:
        print(f"note: {error}", file=sys.stderr)
        return environment
    for name, value in variables.items():
        if not environment.get(name):
            environment[name] = value
    for note in notes:
        print(f"note: {note}", file=sys.stderr)
    return environment


def self_check():
    """Rewrite a synthetic manifest whose library is named by leaf name only. -> exit status.

    Runs on every platform, which is the point: the rewriting is what the macOS gates depend on
    and Linux CI is where the scripts are actually exercised.
    """
    import tempfile

    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory)
        lib_dir = root / "lib"
        lib_dir.mkdir()
        library = lib_dir / "libVkLayer_khronos_validation.dylib"
        library.write_bytes(b"not a real dylib")
        manifest_dir = root / "share" / "vulkan" / "explicit_layer.d"
        manifest_dir.mkdir(parents=True)
        manifest = manifest_dir / "VkLayer_khronos_validation.json"
        manifest.write_text(json.dumps({
            "file_format_version": "1.2.0",
            "layer": {
                "name": "VK_LAYER_KHRONOS_validation",
                "library_path": "libVkLayer_khronos_validation.dylib",
                "api_version": "1.3.280",
            },
        }))

        out_dir = root / "out"
        written = rewrite_manifest(manifest, out_dir, [str(lib_dir)])
        got = json.loads(written.read_text())["layer"]["library_path"]
        if got != str(library.resolve()):
            print(f"FAIL: library_path came out {got!r}, expected {str(library.resolve())!r}",
                  file=sys.stderr)
            return 1

        # And a manifest naming a library that is not there has to say so rather than write a
        # manifest the loader will silently ignore.
        manifest.write_text(json.dumps({
            "file_format_version": "1.2.0",
            "layer": {"name": "VK_LAYER_KHRONOS_validation",
                      "library_path": "libNothingHere.dylib"},
        }))
        try:
            rewrite_manifest(manifest, out_dir, [str(lib_dir)])
        except FileNotFoundError:
            pass
        else:
            print("FAIL: a manifest naming a missing library was rewritten anyway",
                  file=sys.stderr)
            return 1

    print("OK: manifest rewriting resolves a leaf library name and rejects a missing one")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out-dir", default=str(DEFAULT_OUT_DIR),
                        help="where the rewritten manifests are written")
    parser.add_argument("--github-env", action="store_true",
                        help="append the variables to $GITHUB_ENV as well as printing them")
    parser.add_argument("--print-env", action="store_true",
                        help="print `export NAME=value` lines only, for eval in a shell")
    parser.add_argument("--require-layer", action="store_true",
                        help="fail when the validation layer manifest cannot be found")
    parser.add_argument("--self-check", action="store_true",
                        help="exercise the rewriting on a synthetic manifest and exit")
    args = parser.parse_args()

    if args.self_check:
        return self_check()

    try:
        variables, notes = prepare(args.out_dir, args.require_layer)
    except (FileNotFoundError, ValueError, OSError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1

    # Notes go to stderr so that --print-env's output is evaluable as it stands.
    for note in notes:
        print(f"note: {note}", file=sys.stderr)
    for name, value in variables.items():
        print(f"export {name}={value}" if args.print_env else f"{name}={value}")
    if args.github_env:
        github_env = os.environ.get("GITHUB_ENV")
        if not github_env:
            print("FAIL: --github-env outside a GitHub Actions step", file=sys.stderr)
            return 1
        with open(github_env, "a", encoding="utf-8") as handle:
            for name, value in variables.items():
                handle.write(f"{name}={value}\n")
    if sys.platform != "darwin":
        print("note: not macOS, so nothing to rewrite", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
