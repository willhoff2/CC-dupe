#!/usr/bin/env python3
"""Measure the D3D8 *resource* interface surface: the part DX8Wrapper does not wrap.

`d3d8-surface-scan.py` counts calls on `IDirect3DDevice8` / `IDirect3D8`.  It says
nothing about the resource interfaces, because those are not device methods:
`DX8Wrapper` hands out raw `IDirect3DTexture8*` / `IDirect3DSurface8*` /
`IDirect3D{Vertex,Index}Buffer8*` and the engine stores them in `TextureClass`,
`DX8VertexBufferClass`, `SurfaceClass` and friends, then calls `LockRect`,
`GetSurfaceLevel`, `GetLevelDesc`, `AddRef`, `Release` on them directly.

This script counts those sites, so the decision of whether the renderer seam can
abstract resource handles is made against a number instead of a feeling.

Methodology is deliberately identical to `d3d8-surface-scan.py` so the two counts
can be compared:

  * scope is Core/ + GeneralsMD/, excluding */Tools/* and Generals/;
  * comments stripped before matching;
  * an `x->Method(` site counts only when `x` is an identifier (or a
    zero/one-argument accessor call) declared *anywhere in scope* as a pointer to
    one of the resource interfaces below, **and** `Method` is a real member of that
    interface (including the inherited `IDirect3DResource8` and `IUnknown` methods).

Both conditions matter: `Release()` and `Lock()` are far too generic to match on
their own, and the engine's own reference-counted classes use the same names.

Run:  python3 spikes/renderer/tools/d3d8-resource-scan.py
"""
import argparse
import collections
import json
import os
import re

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))

# IUnknown + IDirect3DResource8, inherited by every resource interface below.
UNKNOWN = set("QueryInterface AddRef Release".split())
RESOURCE = set("""GetDevice SetPrivateData GetPrivateData FreePrivateData SetPriority
GetPriority PreLoad GetType""".split())
BASETEXTURE = RESOURCE | set("SetLOD GetLOD GetLevelCount".split())

INTERFACES = {
    "IDirect3DBaseTexture8": BASETEXTURE,
    "IDirect3DTexture8": BASETEXTURE | set(
        "GetLevelDesc GetSurfaceLevel LockRect UnlockRect AddDirtyRect".split()),
    "IDirect3DCubeTexture8": BASETEXTURE | set(
        "GetLevelDesc GetCubeMapSurface LockRect UnlockRect AddDirtyRect".split()),
    "IDirect3DVolumeTexture8": BASETEXTURE | set(
        "GetLevelDesc GetVolumeLevel LockBox UnlockBox AddDirtyBox".split()),
    "IDirect3DVertexBuffer8": RESOURCE | set("Lock Unlock GetDesc".split()),
    "IDirect3DIndexBuffer8": RESOURCE | set("Lock Unlock GetDesc".split()),
    "IDirect3DSurface8": set(
        "GetDevice SetPrivateData GetPrivateData FreePrivateData GetContainer GetDesc "
        "LockRect UnlockRect".split()),
    "IDirect3DVolume8": set(
        "GetDevice SetPrivateData GetPrivateData FreePrivateData GetContainer GetDesc "
        "LockBox UnlockBox".split()),
    "IDirect3DSwapChain8": set("Present GetBackBuffer".split()),
}
for _iface in INTERFACES:
    INTERFACES[_iface] |= UNKNOWN

# LPDIRECT3DTEXTURE8-style typedefs map onto the same interfaces.
LP_ALIASES = {"LP" + name.upper()[1:]: name for name in INTERFACES}


def in_scope(rel):
    if rel.startswith("Generals/"):
        return False
    if "/Tools/" in "/" + rel:
        return False
    return rel.startswith("Core/") or rel.startswith("GeneralsMD/")


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


DECL_RE = re.compile(
    r"\b(?P<type>IDirect3D(?:Base|Cube|Volume)?(?:Texture|Surface|Volume|VertexBuffer|"
    r"IndexBuffer|SwapChain)8|LPDIRECT3D(?:BASE|CUBE|VOLUME)?(?:TEXTURE|SURFACE|VOLUME|"
    r"VERTEXBUFFER|INDEXBUFFER|SWAPCHAIN)8)\s*(?P<stars>\**)\s*(?P<name>\w+)")
# `obj->Method(`, where obj is a plain identifier or an accessor call with a simple
# argument list (`Peek_D3D_Texture()`, `Get_D3D_Surface(0)`).
CALL_RE = re.compile(r"([A-Za-z_]\w*)\s*(?:\([^()]*\))?\s*->\s*([A-Za-z_]\w*)\s*\(")


def collect(sources):
    """name -> interface, from declarations anywhere in scope."""
    aliases = {}
    for text in sources.values():
        for m in DECL_RE.finditer(text):
            typename = m.group("type")
            iface = LP_ALIASES.get(typename, typename)
            # `IDirect3DTexture8 x` with no star is a by-value declaration, which
            # cannot happen for a COM interface, so treat everything as a pointer.
            aliases[m.group("name")] = iface
    # Names too generic to attribute even with the method whitelist.
    for generic in ("texture", "surface", "buffer", "tex", "surf", "p", "ptr"):
        aliases.pop(generic, None)
    return aliases


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--json-out", help="write the per-site detail to this file")
    args = ap.parse_args()

    sources = {}
    for dirpath, _, filenames in os.walk(ROOT):
        if "/.git" in dirpath:
            continue
        for f in filenames:
            if not f.endswith((".cpp", ".h", ".hpp", ".inl")):
                continue
            rel = os.path.relpath(os.path.join(dirpath, f), ROOT)
            if in_scope(rel):
                with open(os.path.join(dirpath, f), errors="replace") as fh:
                    sources[rel] = strip_comments(fh.read())

    aliases = collect(sources)

    per_iface = collections.Counter()
    per_method = collections.defaultdict(collections.Counter)
    per_file = collections.Counter()
    lifetime = collections.Counter()      # AddRef/Release/QueryInterface only
    sites = collections.defaultdict(list)

    for rel, text in sorted(sources.items()):
        for i, line in enumerate(text.splitlines(), 1):
            for obj, method in CALL_RE.findall(line):
                iface = aliases.get(obj)
                if iface is None or method not in INTERFACES[iface]:
                    continue
                per_iface[iface] += 1
                per_method[iface][method] += 1
                per_file[rel] += 1
                if method in UNKNOWN:
                    lifetime[method] += 1
                sites[f"{iface}::{method}"].append((rel, i))

    total = sum(per_iface.values())
    print(f"D3D8 resource-interface call sites: {total}")
    print(f"  of which reference counting (AddRef/Release/QueryInterface): "
          f"{sum(lifetime.values())}")
    print(f"  of which real resource operations: {total - sum(lifetime.values())}")
    print()
    print("| interface | sites | distinct methods |")
    print("| --- | ---: | ---: |")
    for iface, n in sorted(per_iface.items(), key=lambda kv: (-kv[1], kv[0])):
        print(f"| `{iface}` | {n} | {len(per_method[iface])} |")
    print()
    for iface, n in sorted(per_iface.items(), key=lambda kv: (-kv[1], kv[0])):
        methods = ", ".join(f"{m} ({c})" for m, c in
                            sorted(per_method[iface].items(), key=lambda kv: (-kv[1], kv[0])))
        print(f"{iface}: {methods}")
    print()
    print("top files by resource call-site count:")
    for rel, n in per_file.most_common(25):
        print(f"  {n:4d}  {rel}")
    print()
    print(f"files touching D3D8 resource interfaces: {len(per_file)}")

    if args.json_out:
        with open(args.json_out, "w") as fh:
            json.dump({k: v for k, v in sites.items()}, fh, indent=1)


if __name__ == "__main__":
    main()
