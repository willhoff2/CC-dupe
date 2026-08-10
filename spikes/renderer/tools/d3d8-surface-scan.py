#!/usr/bin/env python3
"""Enumerate the Direct3D 8 interface surface the Zero Hour engine actually uses.

Methodology (deliberately conservative, so the numbers can be audited):

 * Scope: Core/ + GeneralsMD/, excluding any */Tools/* tree (WorldBuilder, W3DView,
   ...) and Generals/ (the base game, which the port plan cuts).
 * Comments are stripped first, so dead code does not inflate the counts.
 * A `x->Method(` site is only counted when BOTH hold:
     - `x` is either a known device/interface accessor, or an identifier declared
       anywhere in scope with type IDirect3DDevice8*/LPDIRECT3DDEVICE8 (resp.
       IDirect3D8*/LPDIRECT3D8) -- a global pass, because the declarations live in
       headers and the uses in the .cpp, and
     - `Method` is an actual member of that interface (checked against the real
       vtable list below).
   The second condition is what keeps generic names like `device` from matching
   unrelated classes.
 * DX8CALL / DX8CALL_HRES / DX8CALL_D3D and their DX8CALL_RAW* counterparts (the
   non-asserting variants) are counted separately from direct `->` sites so the
   doc's original "45 call sites" claim can be compared against like for like.
 * Sites inside the render backend implementation (SEAM_FILES below) are counted as
   kind "backend", not "direct". That file *is* the D3D8 implementation of the
   RenderBackendClass seam: it is the one place that is supposed to hold an
   IDirect3DDevice8 and call methods on it. Everything above the seam reaches it
   through the DX8CALL macros, which now dispatch on the backend instead of on a
   device pointer, so the macro counts are unchanged by the seam refactor.
"""
import os, re, sys, collections, json

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))

DEVICE8_METHODS = set("""
TestCooperativeLevel GetAvailableTextureMem ResourceManagerDiscardBytes GetDirect3D
GetDeviceCaps GetDisplayMode GetCreationParameters SetCursorProperties
SetCursorPosition ShowCursor CreateAdditionalSwapChain Reset Present GetBackBuffer
GetRasterStatus SetGammaRamp GetGammaRamp CreateTexture CreateVolumeTexture
CreateCubeTexture CreateVertexBuffer CreateIndexBuffer CreateRenderTarget
CreateDepthStencilSurface CreateImageSurface CopyRects UpdateTexture GetFrontBuffer
SetRenderTarget GetRenderTarget GetDepthStencilSurface BeginScene EndScene Clear
SetTransform GetTransform MultiplyTransform SetViewport GetViewport SetMaterial
GetMaterial SetLight GetLight LightEnable GetLightEnable SetClipPlane GetClipPlane
SetRenderState GetRenderState BeginStateBlock EndStateBlock ApplyStateBlock
CaptureStateBlock DeleteStateBlock CreateStateBlock SetClipStatus GetClipStatus
GetTexture SetTexture GetTextureStageState SetTextureStageState ValidateDevice
GetInfo SetPaletteEntries GetPaletteEntries SetCurrentTexturePalette
GetCurrentTexturePalette DrawPrimitive DrawIndexedPrimitive DrawPrimitiveUP
DrawIndexedPrimitiveUP ProcessVertices CreateVertexShader SetVertexShader
GetVertexShader DeleteVertexShader SetVertexShaderConstant GetVertexShaderConstant
GetVertexShaderDeclaration GetVertexShaderFunction SetStreamSource GetStreamSource
SetIndices GetIndices CreatePixelShader SetPixelShader GetPixelShader
DeletePixelShader SetPixelShaderConstant GetPixelShaderConstant
GetPixelShaderFunction DrawRectPatch DrawTriPatch DeletePatch
""".split())

D3D8_METHODS = set("""
RegisterSoftwareDevice GetAdapterCount GetAdapterIdentifier GetAdapterModeCount
EnumAdapterModes GetAdapterDisplayMode CheckDeviceType CheckDeviceFormat
CheckDeviceMultiSampleType CheckDepthStencilMatch GetDeviceCaps GetAdapterMonitor
CreateDevice
""".split())


# The D3D8 implementation of the backend seam. Direct D3D8 calls here are the point of
# the file, so they are reported as "backend" rather than "direct"; this is deliberately a
# hard-coded list of implementation files and not a per-file numeric budget, so that adding
# a D3D8 call to any *other* file still fails scripts/ci/check-d3d8-surface.py.
SEAM_FILES = {
    "Core/Libraries/Source/WWVegas/WW3D2/d3d8renderbackend.cpp",
    "Core/Libraries/Source/WWVegas/WW3D2/d3d8renderbackend.h",
}


def in_scope(rel):
    if rel.startswith("Generals/"):
        return False
    if "/Tools/" in "/" + rel:
        return False
    return rel.startswith("Core/") or rel.startswith("GeneralsMD/")


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


MACRO_RE = re.compile(
    r"\bDX8CALL((?:_RAW)?(?:_D3D)?(?:_HRES)?)\s*\(\s*(?:res\s*=\s*)?([A-Za-z_]\w*)")
# The `(` is allowed to sit on the next line: this codebase wraps long D3D8 argument lists
# that way, and requiring `(` on the same line hid real call sites (e.g. the two
# `CreateTexture` calls in DX8Wrapper::_Create_DX8_ZTexture).
CALL_RE = re.compile(r"([A-Za-z_]\w*(?:\(\))?)\s*->\s*([A-Za-z_]\w*)\s*(?:\(|$)", re.M)
DECL_RE = re.compile(
    r"\b(?:IDirect3D(?P<a>Device8|8)\s*\*+|LPDIRECT3D(?P<b>DEVICE8|8))\s*(?P<name>\w+)")


def decl_aliases(text, dev_names, d3d_names):
    for m in DECL_RE.finditer(text):
        kind = m.group("a") or m.group("b")
        is_device = kind.lower() == "device8"
        (dev_names if is_device else d3d_names).add(m.group("name"))

# ---- pass 1: collect declaration-derived aliases across the whole scope -------
in_scope_files = []
for dirpath, _, filenames in os.walk(ROOT):
    if "/.git" in dirpath:
        continue
    for f in filenames:
        if not f.endswith((".cpp", ".h", ".hpp", ".inl")):
            continue
        p = os.path.join(dirpath, f)
        rel = os.path.relpath(p, ROOT)
        if in_scope(rel):
            in_scope_files.append((p, rel))

DEV_NAMES = {"_Get_D3D_Device8()", "D3DDevice"}
D3D_NAMES = {"_Get_D3D8()", "D3DInterface"}
SOURCES = {}
for p, rel in in_scope_files:
    text = strip_comments(open(p, errors="replace").read())
    SOURCES[rel] = text
    decl_aliases(text, DEV_NAMES, D3D_NAMES)
# `Device` etc. would be too generic to trust even with the method whitelist
DEV_NAMES -= {"Device", "device", "m_device"}

dev = collections.Counter()
dev_macro = collections.Counter()
dev_backend = collections.Counter()
d3d = collections.Counter()
d3d_macro = collections.Counter()
d3d_backend = collections.Counter()
sites = collections.defaultdict(list)
per_file = collections.Counter()
d3dx = collections.Counter()

for rel, text in SOURCES.items():
        dev_names, d3d_names = DEV_NAMES, D3D_NAMES
        for i, line in enumerate(text.splitlines(), 1):
            if "#define DX8CALL" in line:
                continue
            for suffix, method in MACRO_RE.findall(line):
                if "_D3D" in suffix:
                    if method in D3D8_METHODS:
                        d3d[method] += 1
                        d3d_macro[method] += 1
                        sites["IDirect3D8::" + method].append((rel, i, "DX8CALL" + suffix))
                        per_file[rel] += 1
                elif method in DEVICE8_METHODS:
                    dev[method] += 1
                    dev_macro[method] += 1
                    sites[method].append((rel, i, "DX8CALL" + suffix))
                    per_file[rel] += 1
            if "DX8CALL" in line:
                continue
            kind = "backend" if rel.replace(os.sep, "/") in SEAM_FILES else "direct"
            for obj, method in CALL_RE.findall(line):
                if obj in dev_names and method in DEVICE8_METHODS:
                    dev[method] += 1
                    if kind == "backend":
                        dev_backend[method] += 1
                    sites[method].append((rel, i, kind))
                    per_file[rel] += 1
                elif obj in d3d_names and method in D3D8_METHODS:
                    d3d[method] += 1
                    if kind == "backend":
                        d3d_backend[method] += 1
                    sites["IDirect3D8::" + method].append((rel, i, kind))
                    per_file[rel] += 1
        for m in re.finditer(r"\b(D3DX[A-Za-z_]\w*)\s*\(", text):
            d3dx[m.group(1)] += 1

dev_macro_total = sum(dev_macro.values())
d3d_macro_total = sum(d3d_macro.values())
dev_backend_total = sum(dev_backend.values())
d3d_backend_total = sum(d3d_backend.values())
dev_total = sum(dev.values())
d3d_total = sum(d3d.values())

print(f"IDirect3DDevice8: {len(dev)} distinct methods, {dev_total} call sites "
      f"({dev_macro_total} via DX8CALL/_HRES, {dev_backend_total} in the backend implementation, "
      f"{dev_total - dev_macro_total - dev_backend_total} direct)")
print(f"IDirect3D8:       {len(d3d)} distinct methods, {d3d_total} call sites "
      f"({d3d_macro_total} via DX8CALL_D3D, {d3d_backend_total} in the backend implementation, "
      f"{d3d_total - d3d_macro_total - d3d_backend_total} direct)")
print(f"TOTAL:            {len(dev) + len(d3d)} distinct methods, {dev_total + d3d_total} call sites "
      f"({dev_macro_total + d3d_macro_total} macro, "
      f"{dev_backend_total + d3d_backend_total} backend, "
      f"{dev_total + d3d_total - dev_macro_total - d3d_macro_total - dev_backend_total - d3d_backend_total} direct)")
print()
print("| IDirect3DDevice8 method | sites | DX8CALL | backend | direct |")
print("| --- | --- | --- | --- | --- |")
for k, v in sorted(dev.items(), key=lambda x: (-x[1], x[0])):
    print(f"| `{k}` | {v} | {dev_macro.get(k, 0)} | {dev_backend.get(k, 0)} | "
          f"{v - dev_macro.get(k, 0) - dev_backend.get(k, 0)} |")
print()
print("| IDirect3D8 method | sites | DX8CALL_D3D | backend | direct |")
print("| --- | --- | --- | --- | --- |")
for k, v in sorted(d3d.items(), key=lambda x: (-x[1], x[0])):
    print(f"| `{k}` | {v} | {d3d_macro.get(k, 0)} | {d3d_backend.get(k, 0)} | "
          f"{v - d3d_macro.get(k, 0) - d3d_backend.get(k, 0)} |")
print()
print("D3DX entry points used:")
for k, v in sorted(d3dx.items(), key=lambda x: (-x[1], x[0])):
    print(f"  {k:34s} {v}")
print()
print("top files by D3D8 call-site count:")
for k, v in per_file.most_common(20):
    print(f"  {v:4d}  {k}")

if "--json" in sys.argv:
    json.dump({k: v for k, v in sites.items()}, sys.stdout, indent=1)

# Same payload, but to a file, so a caller can parse it without having to find the JSON
# in the middle of the human-readable report. Used by scripts/ci/check-d3d8-surface.py.
if "--json-out" in sys.argv:
    with open(sys.argv[sys.argv.index("--json-out") + 1], "w") as fh:
        json.dump({k: v for k, v in sites.items()}, fh, indent=1)


# ---------------------------------------------------------------------------
# Part 2: which D3D8 state tokens the engine actually uses.
# ---------------------------------------------------------------------------

RS = re.compile(r"(?:Set_DX8_Render_State|SetRenderState)\s*\(\s*(D3DRS_[A-Z0-9_]+)")
RSG = re.compile(r"(?:Get_DX8_Render_State|GetRenderState)\s*\(\s*(D3DRS_[A-Z0-9_]+)")
TSS = re.compile(r"(?:Set_DX8_Texture_Stage_State|SetTextureStageState)\s*\(\s*[^,]+,\s*(D3DTSS_[A-Z0-9_]+)")
TOP = re.compile(r"\b(D3DTOP_[A-Z0-9_]+)")
TS = re.compile(r"\b(D3DTS_[A-Z0-9_]+)")
FVF = re.compile(r"\b(D3DFVF_[A-Z0-9_]+)")
PRIM = re.compile(r"\b(D3DPT_[A-Z0-9_]+)")
FMT = re.compile(r"\b(D3DFMT_[A-Z0-9_]+)")
POOL = re.compile(r"\b(D3DPOOL_[A-Z0-9_]+)")
USAGE = re.compile(r"\b(D3DUSAGE_[A-Z0-9_]+)")
LOCK = re.compile(r"\b(D3DLOCK_[A-Z0-9_]+)")

buckets = collections.defaultdict(collections.Counter)
for rel, text in SOURCES.items():
    # Drop `case D3DRS_FOO:` lines: those are name-lookup switches in the debug
    # printers, not state the engine sets.
    text = "\n".join(l for l in text.splitlines() if not l.strip().startswith("case "))
    for name, rx in [("SetRenderState", RS), ("GetRenderState", RSG),
                     ("SetTextureStageState", TSS), ("D3DTOP", TOP),
                     ("D3DTS", TS), ("D3DFVF", FVF), ("D3DPT", PRIM),
                     ("D3DFMT", FMT), ("D3DPOOL", POOL), ("D3DUSAGE", USAGE),
                     ("D3DLOCK", LOCK)]:
        for m in rx.finditer(text):
            buckets[name][m.group(1)] += 1

for name in ["SetRenderState", "GetRenderState", "SetTextureStageState", "D3DTOP",
             "D3DTS", "D3DFVF", "D3DPT", "D3DFMT", "D3DPOOL", "D3DUSAGE", "D3DLOCK"]:
    c = buckets[name]
    print()
    print(f"### {name}: {len(c)} distinct, {sum(c.values())} uses")
    for k, v in sorted(c.items(), key=lambda x: (-x[1], x[0])):
        print(f"   {k:44s} {v}")

# ---------------------------------------------------------------------------
# Part 3: size of the dx8* wrapper layer.
# ---------------------------------------------------------------------------

dx8_dir = os.path.join(ROOT, "Core/Libraries/Source/WWVegas/WW3D2")
dx8_files = sorted(f for f in os.listdir(dx8_dir) if f.startswith("dx8"))
dx8_lines = 0
for f in dx8_files:
    with open(os.path.join(dx8_dir, f), errors="replace") as fh:
        dx8_lines += sum(1 for _ in fh)
print()
print(f"### dx8* wrapper layer: {len(dx8_files)} files, {dx8_lines} lines")
