#!/usr/bin/env python3
"""Measure the D3D8 fixed-function texture-stage cascade the engine can actually request.

`d3d8-surface-scan.py` counts *tokens* -- how often `D3DTOP_MODULATE` appears. That is not
the number a renderer backend needs. What a backend needs is the set of distinct
**(op, arg1, arg2, arg0) tuples per stage** that can be programmed into the cascade, because
each distinct tuple is a distinct combiner the shader must be able to evaluate. D3D8 defines
26 colour ops and 2^6 argument encodings; the question is which of those ~10^5 combinations
the game can reach.

Two independent measurements, because the engine reaches the cascade two ways:

  A. STATIC. Every literal `Set_DX8_Texture_Stage_State(stage, D3DTSS_*, value)` /
     `SetTextureStageState(...)` call site, replayed in source order per function, snapshotted
     at the end of each contiguous run of stage-state writes. This catches the hand-programmed
     cascades: TerrainTex, W3DShaderManager, the water and shadow passes, render2d.

  B. ENUMERATED. `shader.cpp`'s `ShaderClass::Apply()` is not a set of literals -- it is a
     function from the 32-bit `ShaderBits` word to a cascade program, and every `ShaderClass`
     the game constructs (from W3D material chunks, from the ~90 preset shaders, from
     WorldBuilder-authored assets) goes through it. So its contribution is enumerated by
     evaluating the transcribed logic over the full cross product of the four fields that
     reach the cascade:
        Texturing (2) x PriGradient (6) x DetailColorFunc (13) x DetailAlphaFunc (4) = 624
     Every one of those 624 is reachable: the fields are independent bitfields, `Apply()`
     branches on each independently, and W3D asset files set them independently.

Both are reported separately and unioned. The union is the set a backend must implement.

Caps dependence, stated because it changes the answer: `Apply()` consults
`D3DTEXOPCAPS_*` and falls back when an op is unsupported. Under `--caps=all` (the default,
and the only case that matters for a Vulkan/MoltenVK target, where the backend advertises
whatever it implements) the preferred op is always taken. `--caps=minimal` reports the
fallback set instead, which is what a Voodoo-era card would have got.

The Voodoo3 path in `Apply()` (`Get_Vendor()==VENDOR_3DFX && Get_Device()==DEVICE_3DFX_VOODOO_3`)
shuffles the primary stage onto stage 2. It is unreachable on any Vulkan device -- there is no
3dfx Vulkan driver -- and is reported separately under `--voodoo3` rather than folded into the
required set.

Usage:
    texture-stage-scan.py [--caps all|minimal] [--voodoo3] [--json out.json] [--scope ...]
"""

import argparse
import collections
import json
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))

# --------------------------------------------------------------------------------------
# D3D8 vocabulary. Values from d3d8types.h; names are what the report prints.
# --------------------------------------------------------------------------------------

TEXTUREOP = {
    "D3DTOP_DISABLE": 1, "D3DTOP_SELECTARG1": 2, "D3DTOP_SELECTARG2": 3,
    "D3DTOP_MODULATE": 4, "D3DTOP_MODULATE2X": 5, "D3DTOP_MODULATE4X": 6,
    "D3DTOP_ADD": 7, "D3DTOP_ADDSIGNED": 8, "D3DTOP_ADDSIGNED2X": 9,
    "D3DTOP_SUBTRACT": 10, "D3DTOP_ADDSMOOTH": 11,
    "D3DTOP_BLENDDIFFUSEALPHA": 12, "D3DTOP_BLENDTEXTUREALPHA": 13,
    "D3DTOP_BLENDFACTORALPHA": 14, "D3DTOP_BLENDTEXTUREALPHAPM": 15,
    "D3DTOP_BLENDCURRENTALPHA": 16, "D3DTOP_PREMODULATE": 17,
    "D3DTOP_MODULATEALPHA_ADDCOLOR": 18, "D3DTOP_MODULATECOLOR_ADDALPHA": 19,
    "D3DTOP_MODULATEINVALPHA_ADDCOLOR": 20, "D3DTOP_MODULATEINVCOLOR_ADDALPHA": 21,
    "D3DTOP_BUMPENVMAP": 22, "D3DTOP_BUMPENVMAPLUMINANCE": 23,
    "D3DTOP_DOTPRODUCT3": 24, "D3DTOP_MULTIPLYADD": 25, "D3DTOP_LERP": 26,
}

TEXTUREARG = {
    "D3DTA_DIFFUSE": 0x00, "D3DTA_CURRENT": 0x01, "D3DTA_TEXTURE": 0x02,
    "D3DTA_TFACTOR": 0x03, "D3DTA_SPECULAR": 0x04, "D3DTA_TEMP": 0x05,
    "D3DTA_COMPLEMENT": 0x10, "D3DTA_ALPHAREPLICATE": 0x20,
}

TTFF = {
    "D3DTTFF_DISABLE": 0, "D3DTTFF_COUNT1": 1, "D3DTTFF_COUNT2": 2,
    "D3DTTFF_COUNT3": 3, "D3DTTFF_COUNT4": 4, "D3DTTFF_PROJECTED": 256,
}

TCI = {
    "D3DTSS_TCI_PASSTHRU": 0x00000,
    "D3DTSS_TCI_CAMERASPACENORMAL": 0x10000,
    "D3DTSS_TCI_CAMERASPACEPOSITION": 0x20000,
    "D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR": 0x30000,
}

SYMBOLS = {}
SYMBOLS.update(TEXTUREOP)
SYMBOLS.update(TEXTUREARG)
SYMBOLS.update(TTFF)
SYMBOLS.update(TCI)

OP_NAME = {v: k for k, v in TEXTUREOP.items()}
ARG_BASE_NAME = {v: k for k, v in TEXTUREARG.items() if v <= 0x05}


def arg_name(value):
    """Render an argument encoding the way d3d8types.h composes it."""
    if value is None:
        return "?"
    base = ARG_BASE_NAME.get(value & 0x0F, "0x%x" % (value & 0x0F))
    out = base
    if value & 0x10:
        out += "|COMPLEMENT"
    if value & 0x20:
        out += "|ALPHAREPLICATE"
    return out


def op_name(value):
    if value is None:
        return "?"
    return OP_NAME.get(value, "0x%x" % value).replace("D3DTOP_", "")


# Which arguments each op actually reads, per the D3D8 "Texture Blending" tables. Writing
# COLORARG2 for a SELECTARG1 stage is legal and common (the engine does it constantly) but
# has no effect, so two tuples that differ only in an unread argument are the *same*
# combiner. Normalising against this table is what turns "distinct tuples written" into
# "distinct combiners a shader has to be able to evaluate", which is the number that sizes
# the work.
ARG_USE = {
    "D3DTOP_DISABLE": (),
    "D3DTOP_SELECTARG1": ("arg1",),
    "D3DTOP_SELECTARG2": ("arg2",),
    "D3DTOP_MULTIPLYADD": ("arg0", "arg1", "arg2"),
    "D3DTOP_LERP": ("arg0", "arg1", "arg2"),
}


def arg_use(op):
    return ARG_USE.get(OP_NAME.get(op, ""), ("arg1", "arg2"))


def normalise(key):
    """Zero out the arguments the op does not read, so equivalent combiners collapse."""
    op, a1, a2, a0 = key
    used = arg_use(op)
    return (op,
            a1 if "arg1" in used else None,
            a2 if "arg2" in used else None,
            a0 if "arg0" in used else None)


# --------------------------------------------------------------------------------------
# Part A -- static extraction of literal call sites
# --------------------------------------------------------------------------------------

CALL_RE = re.compile(
    r"(?:Set_DX8_Texture_Stage_State(?:_Uncached)?|SetTextureStageState)\s*\("
    r"\s*([^,]+?)\s*,\s*(D3DTSS_[A-Z0-9_]+)\s*,\s*(.+?)\s*\)\s*\)?\s*;")

CHANNEL_STATES = {
    "D3DTSS_COLOROP": ("color", "op"),
    "D3DTSS_COLORARG1": ("color", "arg1"),
    "D3DTSS_COLORARG2": ("color", "arg2"),
    "D3DTSS_COLORARG0": ("color", "arg0"),
    "D3DTSS_ALPHAOP": ("alpha", "op"),
    "D3DTSS_ALPHAARG1": ("alpha", "arg1"),
    "D3DTSS_ALPHAARG2": ("alpha", "arg2"),
    "D3DTSS_ALPHAARG0": ("alpha", "arg0"),
}

# D3D8 SetTextureStageState defaults (d3d8 docs, "Texture Stage States"): stage 0 defaults
# to MODULATE(TEXTURE, CURRENT) colour and SELECTARG1(TEXTURE) alpha; every other stage
# defaults to DISABLE. Arg defaults are ARG1=TEXTURE, ARG2=CURRENT, ARG0=CURRENT on all
# stages. Used to fill in an argument a call site never writes.
def stage_defaults(stage):
    if stage == 0:
        return {
            "color": {"op": TEXTUREOP["D3DTOP_MODULATE"],
                      "arg1": TEXTUREARG["D3DTA_TEXTURE"],
                      "arg2": TEXTUREARG["D3DTA_CURRENT"],
                      "arg0": TEXTUREARG["D3DTA_CURRENT"]},
            "alpha": {"op": TEXTUREOP["D3DTOP_SELECTARG1"],
                      "arg1": TEXTUREARG["D3DTA_TEXTURE"],
                      "arg2": TEXTUREARG["D3DTA_CURRENT"],
                      "arg0": TEXTUREARG["D3DTA_CURRENT"]},
        }
    return {
        "color": {"op": TEXTUREOP["D3DTOP_DISABLE"],
                  "arg1": TEXTUREARG["D3DTA_TEXTURE"],
                  "arg2": TEXTUREARG["D3DTA_CURRENT"],
                  "arg0": TEXTUREARG["D3DTA_CURRENT"]},
        "alpha": {"op": TEXTUREOP["D3DTOP_DISABLE"],
                  "arg1": TEXTUREARG["D3DTA_TEXTURE"],
                  "arg2": TEXTUREARG["D3DTA_CURRENT"],
                  "arg0": TEXTUREARG["D3DTA_CURRENT"]},
    }


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def eval_expr(expr):
    """Evaluate a D3D8 constant expression like `D3DTA_DIFFUSE | D3DTA_COMPLEMENT`.

    Returns an int, or None when the expression is not a compile-time constant (a variable,
    a function call, a loop induction variable). Non-constant sites are reported rather than
    guessed at."""
    expr = expr.strip()
    tokens = re.findall(r"[A-Za-z_]\w*|0x[0-9A-Fa-f]+|\d+|\||\(|\)", expr)
    if "".join(tokens) != re.sub(r"\s+", "", expr):
        return None
    out = 0
    for t in tokens:
        if t in ("|", "(", ")"):
            continue
        if t in SYMBOLS:
            out |= SYMBOLS[t]
        elif re.fullmatch(r"0x[0-9A-Fa-f]+", t):
            out |= int(t, 16)
        elif re.fullmatch(r"\d+", t):
            out |= int(t)
        else:
            return None
    return out


class Snapshot(collections.namedtuple(
        "Snapshot", "stage channel op arg1 arg2 arg0")):
    def key(self):
        return (self.op, self.arg1, self.arg2, self.arg0)

    def pretty(self):
        used = arg_use(self.op)
        parts = []
        if "arg0" in used:
            parts.append(arg_name(self.arg0))
        if "arg1" in used:
            parts.append(arg_name(self.arg1))
        if "arg2" in used:
            parts.append(arg_name(self.arg2))
        return "%s(%s)" % (op_name(self.op), ", ".join(parts))


def scan_static(files, gap=8):
    """Replay literal stage-state writes per function and snapshot each cascade program.

    The unit of measurement is a "run": the writes to one (stage, channel) that together
    program it once. A run ends when

      * a field it has already written is written again -- i.e. the source has moved on to
        the next program, which is the reliable signal, because a cascade is only ever
        programmed by writing each of {op, arg1, arg2, arg0} at most once. This is what
        separates the two arms of an `if`/`else` that each program the same stage;
      * more than `gap` source lines pass with no stage-state write at all;
      * the enclosing function ends (a `}` in column 0).

    Fields the run never writes take the D3D8 stage default, and the run is only recorded
    at all if it wrote something. Call sites often write arguments an op does not read
    (`COLORARG2` for a `SELECTARG1` stage); `normalise()` collapses those afterwards, so
    the gap heuristic can only affect the *raw* tuple count, not the normalised one, unless
    it merges two runs that write disjoint fields -- which the rewrite rule above prevents
    for everything except a run that sets only args and a following run that sets only the
    op. No such pair exists in the engine (checked: every literal program writes its op).
    """
    combos = collections.defaultdict(list)      # (channel, key) -> [(file, line, stage)]
    per_stage = collections.Counter()
    other_states = collections.defaultdict(collections.Counter)
    nonconst = collections.defaultdict(list)
    max_stage = -1

    for path, rel in files:
        text = strip_comments(open(path, errors="replace").read())
        lines = text.splitlines()
        runs = {}           # (stage, channel) -> {"vals": {...}, "written": set, "line": n}
        last_line = None

        def emit(stage, channel, run):
            nonlocal max_stage
            vals = run["vals"]
            snap = Snapshot(stage, channel, vals["op"], vals["arg1"],
                            vals["arg2"], vals["arg0"])
            combos[(channel, snap.key())].append((rel, run["line"], stage))
            per_stage[stage] += 1
            max_stage = max(max_stage, stage)

        def flush_all():
            for (stage, channel), run in sorted(runs.items()):
                emit(stage, channel, run)
            runs.clear()

        for i, line in enumerate(lines, 1):
            if line.startswith("}") and runs:
                flush_all()
                last_line = None
            m = CALL_RE.search(line)
            if not m:
                continue
            stage_expr, tss, value_expr = m.group(1), m.group(2), m.group(3)
            stage = eval_expr(stage_expr)
            value = eval_expr(value_expr)
            if tss not in CHANNEL_STATES:
                other_states[tss][value_expr.strip() if value is None else value] += 1
                continue
            if stage is None or stage > 7:
                nonconst[rel].append((i, "stage=%s" % stage_expr.strip()))
                continue
            if value is None:
                nonconst[rel].append((i, "%s=%s" % (tss, value_expr.strip())))
                continue
            if last_line is not None and i - last_line > gap:
                flush_all()
            last_line = i
            channel, field = CHANNEL_STATES[tss]
            run = runs.get((stage, channel))
            if run is not None and field in run["written"]:
                emit(stage, channel, run)
                run = None
            if run is None:
                run = {"vals": dict(stage_defaults(stage)[channel]),
                       "written": set(), "line": i}
                runs[(stage, channel)] = run
            run["vals"][field] = value
            run["written"].add(field)
        if runs:
            flush_all()

    return combos, per_stage, other_states, nonconst, max_stage


# --------------------------------------------------------------------------------------
# Part B -- enumeration of ShaderClass::Apply()
#
# Transcribed from GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/shader.cpp, the
# `if(diff & pri_mask)` / `if(diff & sec_mask)` blocks (lines ~535-905 at the commit this
# was written against). Kept structurally line-for-line with the C++ so it can be diffed
# against it by eye; that fidelity is the whole point, so do not "simplify" it.
# --------------------------------------------------------------------------------------

TEXTURING = ["TEXTURING_DISABLE", "TEXTURING_ENABLE"]
PRIGRADIENT = ["GRADIENT_DISABLE", "GRADIENT_MODULATE", "GRADIENT_ADD",
               "GRADIENT_BUMPENVMAP", "GRADIENT_BUMPENVMAPLUMINANCE", "GRADIENT_MODULATE2X"]
DETAILCOLOR = ["DETAILCOLOR_DISABLE", "DETAILCOLOR_DETAIL", "DETAILCOLOR_SCALE",
               "DETAILCOLOR_INVSCALE", "DETAILCOLOR_ADD", "DETAILCOLOR_SUB",
               "DETAILCOLOR_SUBR", "DETAILCOLOR_BLEND", "DETAILCOLOR_DETAILBLEND",
               "DETAILCOLOR_ADDSIGNED", "DETAILCOLOR_ADDSIGNED2X", "DETAILCOLOR_SCALE2X",
               "DETAILCOLOR_MODALPHAADDCOLOR"]
DETAILALPHA = ["DETAILALPHA_DISABLE", "DETAILALPHA_DETAIL", "DETAILALPHA_SCALE",
               "DETAILALPHA_INVSCALE"]

T = TEXTUREOP
A = TEXTUREARG


def apply_cascade(texturing, pri, det_color, det_alpha, caps_all=True):
    """Return {stage: {"color": (op,a1,a2), "alpha": (op,a1,a2)}} for one ShaderBits value."""
    # Defaults, shader.cpp "// Defaults"
    pric = [T["D3DTOP_SELECTARG1"], A["D3DTA_DIFFUSE"], A["D3DTA_DIFFUSE"]]
    pria = [T["D3DTOP_SELECTARG1"], A["D3DTA_DIFFUSE"], A["D3DTA_DIFFUSE"]]
    secc = [T["D3DTOP_DISABLE"], A["D3DTA_TEXTURE"], A["D3DTA_CURRENT"]]
    seca = [T["D3DTOP_DISABLE"], A["D3DTA_TEXTURE"], A["D3DTA_CURRENT"]]

    if texturing == "TEXTURING_ENABLE":
        if pri == "GRADIENT_DISABLE":
            pric = [T["D3DTOP_SELECTARG1"], A["D3DTA_TEXTURE"], A["D3DTA_CURRENT"]]
            pria = [T["D3DTOP_SELECTARG1"], A["D3DTA_TEXTURE"], A["D3DTA_CURRENT"]]
        elif pri == "GRADIENT_MODULATE":
            pric = [T["D3DTOP_MODULATE"], A["D3DTA_TEXTURE"], A["D3DTA_DIFFUSE"]]
            pria = [T["D3DTOP_MODULATE"], A["D3DTA_TEXTURE"], A["D3DTA_DIFFUSE"]]
        elif pri == "GRADIENT_ADD":
            pric = [T["D3DTOP_ADD"] if caps_all else T["D3DTOP_MODULATE"],
                    A["D3DTA_TEXTURE"], A["D3DTA_DIFFUSE"]]
            pria = [T["D3DTOP_MODULATE"], A["D3DTA_TEXTURE"], A["D3DTA_DIFFUSE"]]
        elif pri == "GRADIENT_BUMPENVMAP":
            if caps_all:
                pric = [T["D3DTOP_BUMPENVMAP"], A["D3DTA_TEXTURE"], A["D3DTA_DIFFUSE"]]
                pria = [T["D3DTOP_DISABLE"], A["D3DTA_TEXTURE"], A["D3DTA_CURRENT"]]
            else:
                pric = [T["D3DTOP_SELECTARG1"], A["D3DTA_DIFFUSE"], A["D3DTA_DIFFUSE"]]
                pria = [T["D3DTOP_SELECTARG1"], A["D3DTA_DIFFUSE"], A["D3DTA_DIFFUSE"]]
        elif pri == "GRADIENT_BUMPENVMAPLUMINANCE":
            if caps_all:
                pric = [T["D3DTOP_BUMPENVMAPLUMINANCE"], A["D3DTA_TEXTURE"], A["D3DTA_DIFFUSE"]]
                pria = [T["D3DTOP_DISABLE"], A["D3DTA_TEXTURE"], A["D3DTA_CURRENT"]]
            else:
                pric = [T["D3DTOP_SELECTARG1"], A["D3DTA_DIFFUSE"], A["D3DTA_DIFFUSE"]]
                pria = [T["D3DTOP_SELECTARG1"], A["D3DTA_DIFFUSE"], A["D3DTA_DIFFUSE"]]
        elif pri == "GRADIENT_MODULATE2X":
            # NB: shader.cpp tests `TextureOpCaps & D3DTOP_MODULATE2X` -- a D3DTOP_ enum
            # against a D3DTEXOPCAPS_ bitmask. D3DTOP_MODULATE2X == 5, so this tests caps
            # bits 0 and 2 (RESERVED|SELECTARG2). It is a bug in the engine, and it is
            # load-bearing here: on any card with SELECTARG2 the test passes and MODULATE2X
            # is used. Reproduced rather than corrected -- see the report.
            pric = [T["D3DTOP_MODULATE2X"], A["D3DTA_TEXTURE"], A["D3DTA_DIFFUSE"]]
            pria = [T["D3DTOP_MODULATE"], A["D3DTA_TEXTURE"], A["D3DTA_DIFFUSE"]]
    else:
        if pri == "GRADIENT_DISABLE":
            pric = [T["D3DTOP_DISABLE"], A["D3DTA_TEXTURE"], A["D3DTA_CURRENT"]]
            pria = [T["D3DTOP_DISABLE"], A["D3DTA_TEXTURE"], A["D3DTA_CURRENT"]]
        else:
            # GRADIENT_MODULATE, GRADIENT_ADD and (via `default:`) the three remaining
            # gradients all land here.
            pric = [T["D3DTOP_SELECTARG2"], A["D3DTA_TEXTURE"], A["D3DTA_DIFFUSE"]]
            pria = [T["D3DTOP_SELECTARG2"], A["D3DTA_TEXTURE"], A["D3DTA_DIFFUSE"]]

    if texturing == "TEXTURING_ENABLE":
        tex_cur = (A["D3DTA_TEXTURE"], A["D3DTA_CURRENT"])
        cur_tex = (A["D3DTA_CURRENT"], A["D3DTA_TEXTURE"])
        table = {
            "DETAILCOLOR_DISABLE": None,
            "DETAILCOLOR_DETAIL": (T["D3DTOP_SELECTARG1"],) + tex_cur,
            "DETAILCOLOR_SCALE": (T["D3DTOP_MODULATE"],) + tex_cur,
            "DETAILCOLOR_INVSCALE": (T["D3DTOP_ADDSMOOTH"] if caps_all else T["D3DTOP_ADD"],)
                                    + tex_cur,
            "DETAILCOLOR_ADD": (T["D3DTOP_ADD"],) + tex_cur,
            "DETAILCOLOR_SUB": (T["D3DTOP_SUBTRACT"],) + tex_cur,
            "DETAILCOLOR_SUBR": (T["D3DTOP_SUBTRACT"],) + cur_tex,
            "DETAILCOLOR_BLEND": (T["D3DTOP_BLENDTEXTUREALPHA"],) + tex_cur,
            "DETAILCOLOR_DETAILBLEND": (T["D3DTOP_BLENDCURRENTALPHA"],) + tex_cur,
            "DETAILCOLOR_ADDSIGNED": (T["D3DTOP_ADDSIGNED"] if caps_all else T["D3DTOP_ADD"],)
                                     + tex_cur,
            "DETAILCOLOR_ADDSIGNED2X": (T["D3DTOP_ADDSIGNED2X"] if caps_all
                                        else T["D3DTOP_ADD"],) + tex_cur,
            "DETAILCOLOR_SCALE2X": (T["D3DTOP_MODULATE2X"] if caps_all
                                    else T["D3DTOP_MODULATE"],) + tex_cur,
            "DETAILCOLOR_MODALPHAADDCOLOR": (T["D3DTOP_MODULATEALPHA_ADDCOLOR"],) + cur_tex
                                            if caps_all else
                                            (T["D3DTOP_ADD"],) + tex_cur,
        }
        entry = table[det_color]
        if entry is not None:
            secc = list(entry)

        atable = {
            "DETAILALPHA_DISABLE": None,
            "DETAILALPHA_DETAIL": (T["D3DTOP_SELECTARG1"],) + tex_cur,
            "DETAILALPHA_SCALE": (T["D3DTOP_MODULATE"],) + tex_cur,
            # NB: no `else` branch in shader.cpp -- if ADDSMOOTH is unsupported the alpha op
            # is left DISABLE, unlike the colour path which falls back to ADD.
            "DETAILALPHA_INVSCALE": (T["D3DTOP_ADDSMOOTH"],) + tex_cur if caps_all else None,
        }
        aentry = atable[det_alpha]
        if aentry is not None:
            seca = list(aentry)

    # "if color is enabled and alpha is disabled set to pass alpha through"
    if secc[0] != T["D3DTOP_DISABLE"] and seca[0] == T["D3DTOP_DISABLE"]:
        seca[0] = T["D3DTOP_SELECTARG2"]
        seca[2] = A["D3DTA_CURRENT"]
    elif secc[0] == T["D3DTOP_DISABLE"] and seca[0] != T["D3DTOP_DISABLE"]:
        secc[0] = T["D3DTOP_SELECTARG2"]
        secc[2] = A["D3DTA_CURRENT"]

    return {0: {"color": tuple(pric), "alpha": tuple(pria)},
            1: {"color": tuple(secc), "alpha": tuple(seca)}}


def apply_cascade_voodoo3(texturing, pri, det_color, det_alpha, caps_all=True):
    """The 3dfx Voodoo3 branch of Apply(): stage 0 becomes a pass-through and the primary
    op moves to stage 2. Unreachable on Vulkan (no 3dfx driver), reported for completeness."""
    base = apply_cascade(texturing, pri, det_color, det_alpha, caps_all)
    pric, pria = list(base[0]["color"]), list(base[0]["alpha"])
    secc, seca = list(base[1]["color"]), list(base[1]["alpha"])
    if not (pric[2] == A["D3DTA_DIFFUSE"] and
            (seca[0] != T["D3DTOP_DISABLE"] or secc[0] != T["D3DTOP_DISABLE"])):
        return base
    tex_arg = A["D3DTA_TEXTURE"] if texturing == "TEXTURING_ENABLE" else A["D3DTA_CURRENT"]
    out = {}
    if pric[0] == T["D3DTOP_SELECTARG1"] and pric[1] == A["D3DTA_DIFFUSE"]:
        out[0] = {"color": (T["D3DTOP_DISABLE"], A["D3DTA_TEXTURE"], A["D3DTA_CURRENT"]),
                  "alpha": (T["D3DTOP_DISABLE"], A["D3DTA_TEXTURE"], A["D3DTA_CURRENT"])}
        if secc[2] == A["D3DTA_CURRENT"]:
            secc[2] = A["D3DTA_DIFFUSE"]
        if seca[2] == A["D3DTA_CURRENT"]:
            seca[2] = A["D3DTA_DIFFUSE"]
        out[1] = {"color": tuple(secc), "alpha": tuple(seca)}
        if secc[0] != T["D3DTOP_DISABLE"] and seca[0] != T["D3DTOP_DISABLE"]:
            out[2] = {"color": (T["D3DTOP_SELECTARG1"], A["D3DTA_CURRENT"], A["D3DTA_CURRENT"]),
                      "alpha": (T["D3DTOP_SELECTARG1"], A["D3DTA_CURRENT"], A["D3DTA_CURRENT"])}
        else:
            out[2] = {"color": (T["D3DTOP_DISABLE"], A["D3DTA_TEXTURE"], A["D3DTA_CURRENT"]),
                      "alpha": (T["D3DTOP_DISABLE"], A["D3DTA_TEXTURE"], A["D3DTA_CURRENT"])}
    else:
        out[0] = {"color": (T["D3DTOP_SELECTARG1"], tex_arg, A["D3DTA_CURRENT"]),
                  "alpha": (T["D3DTOP_SELECTARG1"], tex_arg, A["D3DTA_CURRENT"])}
        out[1] = {"color": tuple(secc), "alpha": tuple(seca)}
        out[2] = {"color": (pric[0], A["D3DTA_CURRENT"], A["D3DTA_DIFFUSE"]),
                  "alpha": (pria[0], A["D3DTA_CURRENT"], A["D3DTA_DIFFUSE"])}
    return out


def enumerate_shader_class(caps_all=True, voodoo3=False):
    fn = apply_cascade_voodoo3 if voodoo3 else apply_cascade
    combos = collections.defaultdict(list)
    programs = set()
    for tex in TEXTURING:
        for pri in PRIGRADIENT:
            for dc in DETAILCOLOR:
                for da in DETAILALPHA:
                    prog = fn(tex, pri, dc, da, caps_all)
                    programs.add(tuple(sorted(
                        (s, c, v) for s, ch in prog.items() for c, v in ch.items())))
                    for stage, chans in prog.items():
                        for channel, (op, a1, a2) in chans.items():
                            key = (op, a1, a2, A["D3DTA_CURRENT"])
                            combos[(channel, key)].append(
                                ("shader.cpp:%s/%s/%s/%s" % (tex, pri, dc, da), 0, stage))
    return combos, programs


# --------------------------------------------------------------------------------------
# report
# --------------------------------------------------------------------------------------

def in_scope(rel, scopes):
    if "/Tools/" in "/" + rel:
        return False
    return any(rel.startswith(s) for s in scopes)


def collect_files(scopes):
    out = []
    for dirpath, _, filenames in os.walk(ROOT):
        if "/.git" in dirpath or "/spikes/" in dirpath:
            continue
        for f in filenames:
            if not f.endswith((".cpp", ".h", ".hpp", ".inl")):
                continue
            p = os.path.join(dirpath, f)
            rel = os.path.relpath(p, ROOT)
            if in_scope(rel, scopes):
                out.append((p, rel))
    return sorted(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--caps", choices=["all", "minimal"], default="all")
    ap.add_argument("--voodoo3", action="store_true")
    ap.add_argument("--json")
    ap.add_argument("--scope", nargs="*",
                    default=["Core/", "Generals/", "GeneralsMD/"])
    args = ap.parse_args()

    caps_all = args.caps == "all"
    files = collect_files(args.scope)
    static, per_stage, other_states, nonconst, max_stage = scan_static(files)
    enum, programs = enumerate_shader_class(caps_all, args.voodoo3)

    print("# D3D8 texture-stage cascade: the combination set the engine can request")
    print()
    print("scope: %s   files scanned: %d   caps: %s   voodoo3 path: %s"
          % (" ".join(args.scope), len(files), args.caps,
             "included" if args.voodoo3 else "excluded"))
    print()

    def collapse(combos):
        """(channel, raw key) -> sites  ==>  (channel, normalised key) -> sites"""
        out = collections.defaultdict(list)
        for (c, k), v in combos.items():
            out[(c, normalise(k))].extend(v)
        return out

    static_n, enum_n = collapse(static), collapse(enum)

    for title, combos, raw in (
            ("A. literal call sites", static_n, static),
            ("B. ShaderClass::Apply() enumerated over %d shader states"
             % (len(TEXTURING) * len(PRIGRADIENT) * len(DETAILCOLOR) * len(DETAILALPHA)),
             enum_n, enum)):
        for channel in ("color", "alpha"):
            keys = sorted((k for (c, k) in combos if c == channel),
                          key=lambda k: (k[0], tuple(-1 if a is None else a for a in k[1:])))
            raw_n = len({k for (c, k) in raw if c == channel})
            print("## %s -- %s: %d distinct combiners (from %d raw tuples)"
                  % (title, channel, len(keys), raw_n))
            for k in keys:
                sites = combos[(channel, k)]
                stages = sorted({s for _, _, s in sites})
                snap = Snapshot(0, channel, *k)
                print("   %-56s stages %-16s %d site(s)"
                      % (snap.pretty(), ",".join(str(s) for s in stages), len(sites)))
            print()

    union = collections.defaultdict(list)
    for src in (static_n, enum_n):
        for k, v in src.items():
            union[k].extend(v)

    print("## UNION -- what a backend must implement")
    total = 0
    for channel in ("color", "alpha"):
        keys = sorted(k for (c, k) in union if c == channel)
        total += len(keys)
        print("   %s: %d distinct combiners" % (channel, len(keys)))
    both = {k for (_, k) in union}
    print("   both channels: %d distinct combiners, %d ignoring the colour/alpha split"
          % (total, len(both)))
    ops = sorted({k[0] for (_, k) in union})
    print("   distinct ops: %d of D3D8's 26  (%s)"
          % (len(ops), ", ".join(op_name(o) for o in ops)))
    argvals = sorted({a for (_, k) in union for a in k[1:] if a is not None})
    print("   distinct argument encodings: %d of D3D8's 6x4  (%s)"
          % (len(argvals), ", ".join(arg_name(a) for a in argvals)))
    print("   highest stage index written by a literal call site: %d" % max_stage)
    print("   distinct ShaderClass cascade programs (stage0+stage1): %d" % len(programs))
    print()

    print("## literal call sites per stage")
    for s in sorted(per_stage):
        print("   stage %d: %d snapshot(s)" % (s, per_stage[s]))
    print()

    print("## the other stage states, by value")
    for tss in sorted(other_states):
        c = other_states[tss]
        rendered = []
        for v, n in sorted(c.items(), key=lambda x: (-x[1], str(x[0]))):
            if isinstance(v, int):
                name = ({vv: kk for kk, vv in TTFF.items()}.get(v)
                        if tss == "D3DTSS_TEXTURETRANSFORMFLAGS" else None)
                if name is None and tss == "D3DTSS_TEXCOORDINDEX":
                    name = {vv: kk for kk, vv in TCI.items()}.get(v)
                rendered.append("%s x%d" % (name or v, n))
            else:
                rendered.append("<%s> x%d" % (v, n))
        print("   %-32s %s" % (tss.replace("D3DTSS_", ""), ", ".join(rendered)))
    print()

    if nonconst:
        print("## call sites whose stage or value is not a compile-time constant")
        print("   (reported, not guessed at -- each is a place the measurement is a lower bound)")
        for rel in sorted(nonconst):
            for line, what in nonconst[rel]:
                print("   %s:%d  %s" % (rel, line, what))
        print()

    if args.json:
        payload = {
            "caps": args.caps,
            "voodoo3": args.voodoo3,
            "scope": args.scope,
            "max_literal_stage": max_stage,
            "shaderclass_programs": len(programs),
            "combos": [
                {"channel": c, "op": k[0], "op_name": op_name(k[0]),
                 "arg1": k[1], "arg2": k[2], "arg0": k[3],
                 "pretty": Snapshot(0, c, *k).pretty(),
                 "stages": sorted({s for _, _, s in union[(c, k)]}),
                 "sites": len(union[(c, k)]),
                 "from_static": (c, k) in static_n,
                 "from_shaderclass": (c, k) in enum_n}
                for (c, k) in sorted(
                    union, key=lambda x: (x[0], x[1][0],
                                          tuple(-1 if a is None else a for a in x[1][1:])))
            ],
        }
        with open(args.json, "w") as fh:
            json.dump(payload, fh, indent=1)
        print("wrote %s" % args.json)

    return 0


if __name__ == "__main__":
    sys.exit(main())
