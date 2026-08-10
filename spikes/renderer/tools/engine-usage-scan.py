#!/usr/bin/env python3
"""Measure the *rest* of the D3D8 fixed-function surface the engine uses.

`texture-stage-scan.py` measures the texture-stage cascade. This measures everything
else a fixed-function backend has to implement, so that the spike implements what the
game asks for rather than what D3D8 defines:

  1. FVF        -- the vertex declarations `SetVertexShader(fvf)` is called with,
                   both the literal `D3DFVF_*` expressions and the named layouts in
                   `dx8fvf.h`, decoded into components.
  2. transforms -- which `D3DTS_*` the engine sets.
  3. lighting   -- light types, per-light state, and the D3DRS_* that control the
                   fixed-function lighting model.
  4. fog        -- vertex vs table fog, the modes, and the values written.
  5. alpha test -- the comparison functions and reference values.
  6. ZBIAS      -- the literal values, which decide the Vulkan depth-bias mapping.
  7. scissor    -- whether the engine scissors at all.
  8. formats    -- what the texture loaders can hand the backend.
  9. pipelines  -- an estimate of how many distinct VkPipeline objects the measured
                   state space needs, which is the number that decides whether an
                   uber-shader or shader permutations are viable on MoltenVK.

Everything printed is either a literal found in the source (with a count) or an
enumeration of an engine enum whose cardinality is read out of the header. Anything
that could not be resolved to a compile-time constant is reported as unresolved
rather than guessed at, exactly like the cascade scan.

Usage:
    engine-usage-scan.py [--json out.json] [--scope Core Generals GeneralsMD]
"""

import argparse
import collections
import json
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
DEFAULT_SCOPE = ["Core", "Generals", "GeneralsMD"]
SOURCE_SUFFIXES = (".cpp", ".h", ".inl")

# --------------------------------------------------------------------------------------
# D3D8 vocabulary (values from d3d8types.h)
# --------------------------------------------------------------------------------------

FVF_BITS = {
    "D3DFVF_XYZ": 0x002, "D3DFVF_XYZRHW": 0x004, "D3DFVF_XYZB1": 0x006,
    "D3DFVF_XYZB2": 0x008, "D3DFVF_XYZB3": 0x00a, "D3DFVF_XYZB4": 0x00c,
    "D3DFVF_XYZB5": 0x00e, "D3DFVF_NORMAL": 0x010, "D3DFVF_PSIZE": 0x020,
    "D3DFVF_DIFFUSE": 0x040, "D3DFVF_SPECULAR": 0x080,
    "D3DFVF_TEX0": 0x000, "D3DFVF_TEX1": 0x100, "D3DFVF_TEX2": 0x200,
    "D3DFVF_TEX3": 0x300, "D3DFVF_TEX4": 0x400, "D3DFVF_TEX5": 0x500,
    "D3DFVF_TEX6": 0x600, "D3DFVF_TEX7": 0x700, "D3DFVF_TEX8": 0x800,
    "D3DFVF_LASTBETA_UBYTE4": 0x1000,
}

# D3DFVF_TEXCOORDSIZEn(index) shifts a 2-bit code into the texture-format field.
TEXCOORDSIZE_CODE = {1: 3, 2: 0, 3: 1, 4: 2}

TRANSFORMSTATE = [
    "D3DTS_VIEW", "D3DTS_PROJECTION", "D3DTS_WORLD", "D3DTS_WORLD1",
    "D3DTS_WORLD2", "D3DTS_WORLD3", "D3DTS_TEXTURE0", "D3DTS_TEXTURE1",
    "D3DTS_TEXTURE2", "D3DTS_TEXTURE3", "D3DTS_TEXTURE4", "D3DTS_TEXTURE5",
    "D3DTS_TEXTURE6", "D3DTS_TEXTURE7",
]

LIGHT_RENDER_STATES = [
    "D3DRS_LIGHTING", "D3DRS_AMBIENT", "D3DRS_SPECULARENABLE",
    "D3DRS_COLORVERTEX", "D3DRS_LOCALVIEWER", "D3DRS_NORMALIZENORMALS",
    "D3DRS_DIFFUSEMATERIALSOURCE", "D3DRS_SPECULARMATERIALSOURCE",
    "D3DRS_AMBIENTMATERIALSOURCE", "D3DRS_EMISSIVEMATERIALSOURCE",
]

FOG_RENDER_STATES = [
    "D3DRS_FOGENABLE", "D3DRS_FOGCOLOR", "D3DRS_FOGTABLEMODE",
    "D3DRS_FOGVERTEXMODE", "D3DRS_FOGSTART", "D3DRS_FOGEND",
    "D3DRS_FOGDENSITY", "D3DRS_RANGEFOGENABLE",
]

# ww3dformat.h's WW3D_FORMAT_*: every format the engine's own format enum can name,
# i.e. everything a loader is allowed to hand to the texture-creation path.
WW3D_FORMAT_HEADER = "Core/Libraries/Source/WWVegas/WW3D2/ww3dformat.h"

SHADER_HEADER = "GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/shader.h"


def source_files(scope):
    for top in scope:
        base = os.path.join(ROOT, top)
        for dirpath, _dirnames, filenames in os.walk(base):
            for name in filenames:
                if name.endswith(SOURCE_SUFFIXES):
                    yield os.path.join(dirpath, name)


def relative(path):
    return os.path.relpath(path, ROOT)


# --------------------------------------------------------------------------------------
# 1. FVF
# --------------------------------------------------------------------------------------

FVF_EXPR = re.compile(
    r"(D3DFVF_[A-Z0-9_]+(?:\([0-9]+\))?"
    r"(?:\s*\|\s*D3DFVF_[A-Z0-9_]+(?:\([0-9]+\))?)*)")
TEXCOORDSIZE = re.compile(r"D3DFVF_TEXCOORDSIZE([1-4])\(\s*([0-9]+)\s*\)")


def eval_fvf(expr):
    """Fold an FVF expression to its bit value; None if a token is unknown."""
    value = 0
    for token in [t.strip() for t in expr.split("|")]:
        size = TEXCOORDSIZE.fullmatch(token)
        if size:
            components, index = int(size.group(1)), int(size.group(2))
            value |= TEXCOORDSIZE_CODE[components] << (index * 2 + 16)
            continue
        if token in FVF_BITS:
            value |= FVF_BITS[token]
            continue
        return None
    return value


def describe_fvf(value):
    """Decode an FVF bitfield the way a vertex-input decoder has to."""
    position = {
        0x002: "XYZ", 0x004: "XYZRHW", 0x006: "XYZB1", 0x008: "XYZB2",
        0x00a: "XYZB3", 0x00c: "XYZB4", 0x00e: "XYZB5",
    }.get(value & 0x00e, "none")
    parts = [position]
    if value & 0x1000:
        parts.append("LASTBETA_UBYTE4")
    if value & 0x010:
        parts.append("NORMAL")
    if value & 0x020:
        parts.append("PSIZE")
    if value & 0x040:
        parts.append("DIFFUSE")
    if value & 0x080:
        parts.append("SPECULAR")
    sets = (value & 0xf00) >> 8
    if sets:
        sizes = []
        for i in range(sets):
            code = (value >> (i * 2 + 16)) & 0x3
            sizes.append({0: 2, 1: 3, 2: 4, 3: 1}[code])
        parts.append("TEX%d(%s)" % (sets, ",".join("uv%d" % s for s in sizes)))
    return " | ".join(parts)


def scan_fvf(scope):
    literal = collections.Counter()
    unresolved = collections.Counter()
    for path in source_files(scope):
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            text = handle.read()
        for match in FVF_EXPR.finditer(text):
            expr = re.sub(r"\s+", "", match.group(1))
            # Skip the declaration of the constants themselves.
            value = eval_fvf(expr)
            if value is None:
                unresolved[expr] += 1
            elif value & 0x00e:  # has a position: an actual vertex declaration
                literal[value] += 1
    return literal, unresolved


def scan_named_layouts():
    """The DX8_FVF_* enum in dx8fvf.h: the layouts the engine's own vertex classes use."""
    path = os.path.join(ROOT, "Core/Libraries/Source/WWVegas/WW3D2/dx8fvf.h")
    named = {}
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            match = re.match(r"\s*(DX8_FVF_[A-Z0-9_]+)\s*=\s*(.+?),?\s*(?://.*)?$", line)
            if not match:
                continue
            expr = re.sub(r"\s+", "", match.group(2)).rstrip(",")
            expr = re.sub(r"^\(|\)$", "", expr)
            value = eval_fvf(expr)
            if value is not None:
                named[match.group(1)] = value
    return named


# --------------------------------------------------------------------------------------
# generic: literal render-state writes
# --------------------------------------------------------------------------------------

RENDER_STATE_CALL = re.compile(
    r"Set_DX8_Render_State\s*\(\s*(D3DRS_[A-Z0-9_]+)\s*,\s*([^;]+?)\s*\)\s*;"
    r"|SetRenderState\s*\(\s*(D3DRS_[A-Z0-9_]+)\s*,\s*([^;]+?)\s*\)\s*;")


def scan_render_states(scope, wanted):
    """value -> count, per render state, over every literal write site."""
    values = collections.defaultdict(collections.Counter)
    for path in source_files(scope):
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            for line in handle:
                if "D3DRS_" not in line or line.lstrip().startswith("//"):
                    continue
                for match in RENDER_STATE_CALL.finditer(line):
                    state = match.group(1) or match.group(3)
                    value = (match.group(2) or match.group(4) or "").strip()
                    if state in wanted:
                        values[state][re.sub(r"\s+", " ", value)] += 1
    return values


def scan_tokens(scope, tokens):
    counts = collections.Counter()
    for path in source_files(scope):
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            text = handle.read()
        for token in tokens:
            hits = len(re.findall(r"\b%s\b" % re.escape(token), text))
            if hits:
                counts[token] += hits
    return counts


def scan_call_sites(scope, pattern):
    sites = []
    regex = re.compile(pattern)
    for path in source_files(scope):
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            for number, line in enumerate(handle, 1):
                if line.lstrip().startswith("//"):
                    continue
                if regex.search(line):
                    sites.append((relative(path), number, line.strip()))
    return sites


# --------------------------------------------------------------------------------------
# 8. texture formats
# --------------------------------------------------------------------------------------

def scan_ww3d_formats():
    path = os.path.join(ROOT, WW3D_FORMAT_HEADER)
    formats = []
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            match = re.match(r"\s*(WW3D_FORMAT_[A-Z0-9_]+)", line)
            if match and match.group(1) not in ("WW3D_FORMAT_COUNT",):
                if match.group(1) not in formats:
                    formats.append(match.group(1))
    return formats


# --------------------------------------------------------------------------------------
# 9. pipeline-count estimate
# --------------------------------------------------------------------------------------

def enum_cardinalities():
    """Read the ShaderClass field cardinalities out of shader.h rather than hardcoding."""
    path = os.path.join(ROOT, SHADER_HEADER)
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        text = handle.read()
    counts = {}
    for name in ("DepthCompareType", "DepthMaskType", "ColorMaskType",
                 "DstBlendFuncType", "SrcBlendFuncType", "CullModeType",
                 "AlphaTestType", "FogFuncType", "PriGradientType",
                 "SecGradientType", "TexturingType", "DetailColorFuncType",
                 "DetailAlphaFuncType"):
        block = re.search(r"enum\s+%s\s*\{(.*?)\}" % name, text, re.S)
        if not block:
            continue
        members = [m for m in re.findall(r"^\s*([A-Z][A-Z0-9_]*)\s*[=,]", block.group(1),
                                         re.M)]
        members = [m for m in members if not m.endswith("_MAX")]
        counts[name] = len(members)
    return counts


SHADE_CNST_FIELDS = [
    "depth_compare", "depth_mask", "color_mask", "src_blend", "dst_blend", "fog",
    "pri_grad", "sec_grad", "texture", "alpha_test", "cullmode", "post_det_color",
    "post_det_alpha",
]
# The subset of SHADE_CNST's fields Vulkan has to bake into a VkPipeline. The rest
# are interpreted by the uber-shader from a uniform and cost no pipelines.
# Alpha test is not in the list: the spike does it with `discard` in the fragment
# shader from a uniform, so it does not multiply the pipeline count.
BAKED_FIELDS = ["depth_compare", "depth_mask", "color_mask", "src_blend",
                "dst_blend", "cullmode"]


def scan_shader_presets():
    """The `#define SC_* ( SHADE_CNST(...) )` presets in shader.cpp.

    These are the shaders the engine ships with; W3D assets can name others, but this
    is the set the code itself constructs, so it bounds the *realistic* pipeline count
    from below the way the full cross product bounds it from above.
    """
    path = os.path.join(ROOT, "GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/shader.cpp")
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        text = handle.read()
    text = text.replace("\\\n", " ")
    presets = {}
    for match in re.finditer(r"#define\s+(SC_[A-Z0-9_]+)\s*\(\s*SHADE_CNST\((.*?)\)\s*\)",
                             text, re.S):
        args = [a.strip() for a in match.group(2).split(",")]
        if len(args) != len(SHADE_CNST_FIELDS):
            continue
        presets[match.group(1)] = dict(zip(SHADE_CNST_FIELDS, args))
    return presets


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--scope", nargs="*", default=DEFAULT_SCOPE)
    parser.add_argument("--json")
    args = parser.parse_args()

    scope = args.scope
    report = {}
    out = sys.stdout.write

    out("# D3D8 fixed-function usage outside the texture-stage cascade\n\n")
    out("scope: %s\n\n" % " ".join(s + "/" for s in scope))

    # --- 1. FVF ---------------------------------------------------------------
    literal, unresolved = scan_fvf(scope)
    named = scan_named_layouts()
    out("## 1. FVF: %d distinct literal vertex declarations\n" % len(literal))
    for value, count in sorted(literal.items(), key=lambda kv: -kv[1]):
        out("   0x%05x  %-58s %d site(s)\n" % (value, describe_fvf(value), count))
    out("\n## 1b. dx8fvf.h named layouts (DX8_FVF_*): %d\n" % len(named))
    for name, value in sorted(named.items(), key=lambda kv: kv[1]):
        out("   %-22s 0x%05x  %s\n" % (name, value, describe_fvf(value)))
    if unresolved:
        out("\n   FVF expressions that are not compile-time constant: %d\n" % len(unresolved))
        for expr, count in sorted(unresolved.items()):
            out("     %s x%d\n" % (expr, count))
    all_values = sorted(set(literal) | set(named.values()))
    max_sets = max(((v & 0xf00) >> 8) for v in all_values)
    out("\n   union: %d distinct declarations, max texture coordinate sets: %d,"
        " normals: %s, blend weights: %s\n"
        % (len(all_values), max_sets,
           any(v & 0x010 for v in all_values),
           any((v & 0x00e) in (0x006, 0x008, 0x00a, 0x00c, 0x00e) for v in all_values)))
    report["fvf_literal"] = {"0x%05x" % v: c for v, c in literal.items()}
    report["fvf_named"] = {k: "0x%05x" % v for k, v in named.items()}

    # --- 2. transforms --------------------------------------------------------
    transforms = scan_tokens(scope, TRANSFORMSTATE)
    out("\n## 2. transforms set (D3DTS_*)\n")
    for name in TRANSFORMSTATE:
        if transforms[name]:
            out("   %-16s %d reference(s)\n" % (name, transforms[name]))
    report["transforms"] = dict(transforms)

    # --- 3. lighting ----------------------------------------------------------
    out("\n## 3. lighting\n")
    light_types = scan_tokens(scope, ["D3DLIGHT_DIRECTIONAL", "D3DLIGHT_POINT",
                                      "D3DLIGHT_SPOT"])
    for name, count in sorted(light_types.items()):
        out("   %-22s %d reference(s)\n" % (name, count))
    set_light = scan_call_sites(scope, r"\bSetLight\s*\(|Set_DX8_Light\s*\(")
    light_enable = scan_call_sites(scope, r"\bLightEnable\s*\(")
    out("   SetLight/Set_DX8_Light call sites: %d\n" % len(set_light))
    out("   LightEnable call sites:            %d\n" % len(light_enable))
    states = scan_render_states(scope, set(LIGHT_RENDER_STATES))
    for name in LIGHT_RENDER_STATES:
        if name in states:
            out("   %-30s %s\n" % (name, ", ".join(
                "%s x%d" % (v, c) for v, c in states[name].most_common())))
    report["light_types"] = dict(light_types)
    report["light_states"] = {k: dict(v) for k, v in states.items()}

    # --- 4. fog ---------------------------------------------------------------
    out("\n## 4. fog\n")
    fog = scan_render_states(scope, set(FOG_RENDER_STATES))
    for name in FOG_RENDER_STATES:
        if name in fog:
            out("   %-22s %s\n" % (name, ", ".join(
                "%s x%d" % (v, c) for v, c in fog[name].most_common())))
    report["fog"] = {k: dict(v) for k, v in fog.items()}

    # --- 5. alpha test --------------------------------------------------------
    out("\n## 5. alpha test\n")
    alpha = scan_render_states(scope, {"D3DRS_ALPHATESTENABLE", "D3DRS_ALPHAFUNC",
                                       "D3DRS_ALPHAREF"})
    for name in ("D3DRS_ALPHATESTENABLE", "D3DRS_ALPHAFUNC", "D3DRS_ALPHAREF"):
        if name in alpha:
            out("   %-22s %s\n" % (name, ", ".join(
                "%s x%d" % (v, c) for v, c in alpha[name].most_common())))
    report["alpha_test"] = {k: dict(v) for k, v in alpha.items()}

    # --- 6. ZBIAS -------------------------------------------------------------
    out("\n## 6. depth bias (D3DRS_ZBIAS)\n")
    zbias = scan_render_states(scope, {"D3DRS_ZBIAS"})
    for value, count in zbias.get("D3DRS_ZBIAS", collections.Counter()).most_common():
        out("   value %-8s %d site(s)\n" % (value, count))
    report["zbias"] = dict(zbias.get("D3DRS_ZBIAS", {}))

    # --- 7. scissor -----------------------------------------------------------
    out("\n## 7. scissor\n")
    scissor = scan_call_sites(scope, r"SetScissorRect|SCISSORTESTENABLE|Set_Scissor")
    out("   call sites: %d%s\n" % (len(scissor),
                                   "" if scissor else "  (the engine never scissors)"))
    for path, number, line in scissor[:20]:
        out("     %s:%d  %s\n" % (path, number, line))
    report["scissor_sites"] = len(scissor)

    # --- 8. texture formats ---------------------------------------------------
    formats = scan_ww3d_formats()
    out("\n## 8. texture formats the engine's own enum can name: %d\n" % len(formats))
    out("   %s\n" % ", ".join(f.replace("WW3D_FORMAT_", "") for f in formats))
    report["ww3d_formats"] = formats

    # --- 9. pipeline estimate -------------------------------------------------
    cards = enum_cardinalities()
    out("\n## 9. VkPipeline count estimate\n")
    out("   ShaderClass field cardinalities, read from %s:\n" % SHADER_HEADER)
    for name, count in sorted(cards.items()):
        out("     %-22s %d\n" % (name, count))
    # Only the fields Vulkan bakes into a pipeline. The rest (gradients, detail
    # functions, texturing, fog func) are interpreted by the uber-shader from a
    # uniform, so they cost no pipelines.
    baked = ["DepthCompareType", "DepthMaskType", "ColorMaskType",
             "DstBlendFuncType", "SrcBlendFuncType", "CullModeType"]
    baked_product = 1
    for name in baked:
        baked_product *= cards.get(name, 1)
    fvf_count = len(all_values)
    out("\n   states Vulkan must bake in: %s\n" % " x ".join(
        "%s(%d)" % (n.replace("Type", ""), cards.get(n, 1)) for n in baked))
    out("   = %d state combinations, x %d vertex declarations = %d pipelines\n"
        % (baked_product, fvf_count, baked_product * fvf_count))
    interpreted = 1
    for name in ("FogFuncType", "PriGradientType", "SecGradientType", "TexturingType",
                 "DetailColorFuncType", "DetailAlphaFuncType"):
        interpreted *= cards.get(name, 1)
    out("   if the cascade were compiled into permutations instead of interpreted,\n"
        "   multiply by %d (%s) = %d pipelines\n"
        % (interpreted,
           " x ".join("%s(%d)" % (n.replace("Type", ""), cards.get(n, 1))
                      for n in ("FogFuncType", "PriGradientType", "SecGradientType",
                                "TexturingType", "DetailColorFuncType",
                                "DetailAlphaFuncType")),
           baked_product * fvf_count * interpreted))
    presets = scan_shader_presets()
    preset_keys = set()
    for fields in presets.values():
        preset_keys.add(tuple(fields[f] for f in BAKED_FIELDS))
    out("\n   the cross product is an upper bound. The engine's own shader presets\n"
        "   (#define SC_* in shader.cpp) are %d shaders, using %d distinct\n"
        "   pipeline-relevant state combinations; x %d vertex declarations =\n"
        "   %d pipelines for a frame that only uses presets.\n"
        % (len(presets), len(preset_keys), fvf_count, len(preset_keys) * fvf_count))
    report["shader_presets"] = {"count": len(presets),
                                "distinct_baked_states": len(preset_keys)}
    report["pipeline_estimate"] = {
        "preset_pipelines": len(preset_keys) * fvf_count,
        "baked_state_combinations": baked_product,
        "vertex_declarations": fvf_count,
        "uber_shader_pipelines": baked_product * fvf_count,
        "permutation_pipelines": baked_product * fvf_count * interpreted,
        "cardinalities": cards,
    }

    if args.json:
        with open(args.json, "w", encoding="utf-8") as handle:
            json.dump(report, handle, indent=2, sort_keys=True)
        out("\nwrote %s\n" % args.json)
    return 0


if __name__ == "__main__":
    sys.exit(main())
