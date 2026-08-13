#!/usr/bin/env python3
"""Measure how much of the D3D8 surface the Vulkan backend actually implements.

`d3d8-surface-scan.py` measures *demand*: which D3D8 entry points and which render /
texture-stage states the Zero Hour engine reaches for. This measures *supply*: for each
of those, whether `spikes/renderer/` serves it, accepts and ignores it, or does not know
about it at all. The two together are the renderer port's scoreboard.

Everything is derived from source, so it cannot drift out of date the way a hand-written
table does:

  methods       the engine's measured method list comes from `d3d8-surface-scan.py`
                (`--json-out`); each is mapped to the `RenderBackend` entry point that
                serves it by `backend-coverage-map.json`, which is a *design* statement
                and the only hand-maintained input here. The status is measured: the
                entry point must be declared in `src/render_backend.h` and defined in
                `src/vulkan_backend.cpp` with a body that does something.
                  implemented  defined, and the body has a statement that is not a
                               `(void)` cast or a bare `return`
                  stubbed      defined but empty, or marked `COVERAGE-STUB:`
                  absent       declared but never defined, or not declared at all
                Methods whose map category is not `backend` are counted separately and
                are not part of the backend coverage figure: `platform` belongs to the
                window/display layer, `caps` reports a device's abilities rather than
                drawing with them, and `device-model` is a D3D8 device-model artefact
                (device loss, managed-pool eviction) with nothing to implement.

  render states a `D3DRS_*` is `implemented` when the backend *reads*
                `render_states_[D3DRS_X]` somewhere -- i.e. it reaches a pipeline key, a
                uniform or a dynamic-state call -- rather than merely storing it.
                An assignment `render_states_[D3DRS_X] = ...` (the device defaults) does
                not count. A state the backend deliberately does not serve must carry a
                `COVERAGE-IGNORE: D3DRS_X - reason` line, which is reported as `ignored`
                with its reason; anything else the engine sets is `absent`.

  stage states  a `D3DTSS_*` is `implemented` when the `Set_DX8_Texture_Stage_State`
                switch stores it into a `PerStage` field *and* that field is read
                somewhere else in the backend. Stored but never read is `stubbed`.

  cascade ops   a `D3DTOP_*` is implemented when `shaders/fixedfunc.frag` has a
                `case TOP_X:` for it in the colour combiner.

  primitives    a `D3DPT_*` is implemented when `To_Vk_Topology` translates it *and* the
                pipeline key's topology is assigned from something other than a literal --
                a backend that hardcodes `key.topology = D3DPT_TRIANGLELIST` covers exactly
                one primitive type however many the translator knows about.

Usage:
    backend-coverage-scan.py [--json out.json] [--sites-json sites.json]
                             [--states-json states.json] [--quiet]

`--sites-json` / `--states-json` reuse an existing `d3d8-surface-scan.py` payload instead
of re-running the scan, which takes about a minute over the whole tree.
"""

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
SPIKE = os.path.join(ROOT, "spikes", "renderer")
SURFACE_SCAN = os.path.join(SPIKE, "tools", "d3d8-surface-scan.py")
COVERAGE_MAP = os.path.join(SPIKE, "tools", "backend-coverage-map.json")

BACKEND_IMPL = os.path.join(SPIKE, "src", "vulkan_backend.cpp")
BACKEND_HEADER = os.path.join(SPIKE, "src", "render_backend.h")
STATE_TRANSLATE = [os.path.join(SPIKE, "src", "state_translate.h"),
                   os.path.join(SPIKE, "src", "state_translate.cpp")]
FRAG_SHADER = os.path.join(SPIKE, "shaders", "fixedfunc.frag")
VERT_SHADER = os.path.join(SPIKE, "shaders", "fixedfunc.vert")


def read(path):
    with open(path, errors="replace") as fh:
        return fh.read()


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


# --------------------------------------------------------------------------------------
# demand: what the engine asks for
# --------------------------------------------------------------------------------------

def measure_demand(sites_json, states_json):
    """Return (methods, render_states, stage_states, cascade_ops) with use counts."""
    if sites_json is None or states_json is None:
        with tempfile.TemporaryDirectory() as tmp:
            sites_json = sites_json or os.path.join(tmp, "sites.json")
            states_json = states_json or os.path.join(tmp, "states.json")
            proc = subprocess.run(
                [sys.executable, SURFACE_SCAN, "--json-out", sites_json,
                 "--states-json-out", states_json],
                cwd=ROOT, capture_output=True, text=True)
            if proc.returncode != 0:
                sys.stderr.write(proc.stderr)
                raise SystemExit("the D3D8 surface scanner failed")
            sites = json.loads(read(sites_json))
            states = json.loads(read(states_json))
    else:
        sites = json.loads(read(sites_json))
        states = json.loads(read(states_json))

    methods = {}
    for key, entries in sites.items():
        interface = "IDirect3D8" if key.startswith("IDirect3D8::") else "IDirect3DDevice8"
        # Keep the scanner's own key: GetDeviceCaps exists on both interfaces.
        methods[key] = {"interface": interface, "sites": len(entries)}
    render_states = dict(states.get("SetRenderState", {}))
    for state, count in states.get("GetRenderState", {}).items():
        render_states[state] = render_states.get(state, 0) + count
    return (methods, render_states, dict(states.get("SetTextureStageState", {})),
            dict(states.get("D3DTOP", {})), dict(states.get("D3DPT", {})))


# --------------------------------------------------------------------------------------
# supply: what the backend implements
# --------------------------------------------------------------------------------------

IGNORE_RE = re.compile(r"COVERAGE-IGNORE:\s*(D3D\w+)\s*-\s*(.+?)\s*$", re.M)
STUB_RE = re.compile(r"COVERAGE-STUB:\s*(.+?)\s*$", re.M)


def backend_sources():
    """The backend implementation, with comments kept (the markers live in them)."""
    return {path: read(path) for path in [BACKEND_IMPL, BACKEND_HEADER, FRAG_SHADER,
                                          VERT_SHADER] + STATE_TRANSLATE}


def declared_entry_points(header_text):
    return set(re.findall(r"virtual\s+[\w:<>*&\s]+?\b(\w+)\s*\(", header_text))


def definition_bodies(impl_text):
    """{entry point: body} for every VulkanBackend member defined in the .cpp.

    Both spellings: out-of-line `VulkanBackend::Name(...) {` and the short overrides
    defined inside the class body as `Name(...) override {`.
    """
    bodies = {}
    patterns = [r"\bVulkanBackend::(\w+)\s*\([^;{]*?\)\s*(?:const\s*)?\{",
                r"\b(\w+)\s*\([^;{]*?\)\s*(?:const\s*)?override\s*\{"]
    for pattern in patterns:
        for match in re.finditer(pattern, impl_text, re.S):
            name = match.group(1)
            if name in bodies:
                continue
            depth = 0
            start = impl_text.index("{", match.end() - 1)
            for index in range(start, len(impl_text)):
                if impl_text[index] == "{":
                    depth += 1
                elif impl_text[index] == "}":
                    depth -= 1
                    if depth == 0:
                        bodies[name] = impl_text[start + 1:index]
                        break
    return bodies


def body_status(body):
    """implemented / stubbed, measured from what the body actually does."""
    if STUB_RE.search(body):
        return "stubbed"
    code = strip_comments(body).strip()
    # Drop the statements that do nothing: `(void)x;` silences an unused parameter and a
    # bare `return;` / `return false;` / `return 0;` is what an unimplemented override
    # looks like.
    code = re.sub(r"\(void\)\s*\w+\s*;", "", code)
    code = re.sub(r"return\s*(?:false|true|0|nullptr|\{\})?\s*;", "", code)
    return "implemented" if code.strip() else "stubbed"


def measure_methods(methods, coverage_map, sources):
    header = sources[BACKEND_HEADER]
    declared = declared_entry_points(header)
    bodies = definition_bodies(sources[BACKEND_IMPL])

    out = {}
    unmapped = []
    for name, info in sorted(methods.items()):
        entry = coverage_map["methods"].get(name)
        if entry is None:
            unmapped.append(name)
            continue
        category = entry["category"]
        points = entry.get("entry", [])
        statuses = []
        for point in points:
            if point not in declared:
                statuses.append("absent")
            elif point not in bodies:
                statuses.append("absent")
            else:
                statuses.append(body_status(bodies[point]))
        if not points:
            status = "none-needed"
        elif "absent" in statuses:
            status = "absent"
        elif "stubbed" in statuses:
            status = "stubbed"
        else:
            status = "implemented"
        out[name] = {"interface": info["interface"], "sites": info["sites"],
                     "category": category, "entry": points, "status": status,
                     "note": entry.get("note", "")}
    if unmapped:
        raise SystemExit(
            "these D3D8 methods are reached by the engine but are not in "
            f"{os.path.relpath(COVERAGE_MAP, ROOT)}: {', '.join(unmapped)}\n"
            "Add them (with a category and, for backend methods, the RenderBackend entry "
            "point that serves them) so the scoreboard stays complete.")
    return out


def measure_render_states(render_states, sources):
    impl = sources[BACKEND_IMPL]
    ignored = {name: reason for name, reason in IGNORE_RE.findall(impl)}
    code = strip_comments(impl)
    reads = set()
    stores = set()
    for line in code.splitlines():
        for state in re.findall(r"render_states_\[(D3DRS_\w+)\]", line):
            # `render_states_[X] = ...` on the left of an assignment is a store (the
            # device defaults, and Set_DX8_Render_State itself); everything else -- a
            # pipeline key field, a uniform, a dynamic-state argument -- is a read.
            if re.match(r"\s*render_states_\[" + state + r"\]\s*=[^=]", line):
                stores.add(state)
            else:
                reads.add(state)
    out = {}
    for state, uses in sorted(render_states.items()):
        if state in reads:
            status, reason = "implemented", ""
        elif state in ignored:
            status, reason = "ignored", ignored[state]
        elif state in stores:
            status, reason = "stubbed", "stored in the shadow state, never read"
        else:
            status, reason = "absent", ""
        out[state] = {"uses": uses, "status": status, "reason": reason}
    return out


def measure_stage_states(stage_states, sources):
    impl = sources[BACKEND_IMPL]
    ignored = {name: reason for name, reason in IGNORE_RE.findall(impl)}
    code = strip_comments(impl)
    # `case D3DTSS_X: s.field = ...` -- the field is what the rest of the backend reads.
    stored = dict(re.findall(r"case\s+(D3DTSS_\w+)\s*:\s*s\.(\w+)", code))
    out = {}
    for state, uses in sorted(stage_states.items()):
        field = stored.get(state)
        if field is None:
            status = "ignored" if state in ignored else "absent"
        else:
            # Read anywhere other than the switch line that stores it.
            others = [line for line in code.splitlines()
                      if re.search(r"\b" + field + r"\b", line)
                      and not re.search(r"case\s+D3DTSS_\w+\s*:", line)]
            status = "implemented" if others else "stubbed"
        out[state] = {"uses": uses, "status": status,
                      "field": field or "", "reason": ignored.get(state, "")}
    return out


def measure_cascade_ops(cascade_ops, sources):
    frag = sources[FRAG_SHADER]
    handled = {"TOP_" + name for name in re.findall(r"case\s+TOP_(\w+)\s*:", frag)}
    out = {}
    for op, uses in sorted(cascade_ops.items()):
        token = "TOP_" + op[len("D3DTOP_"):]
        # DISABLE is not a combiner op: it ends the cascade, which the shader's loop does.
        implemented = token in handled or op == "D3DTOP_DISABLE"
        out[op] = {"uses": uses, "status": "implemented" if implemented else "absent"}
    return out


def measure_primitive_types(primitive_types, sources):
    translate = strip_comments(sources[STATE_TRANSLATE[1]])
    impl = strip_comments(sources[BACKEND_IMPL])
    body = re.search(r"To_Vk_Topology\s*\([^)]*\)\s*\{(.*?)\n\}", translate, re.S)
    translated = set(re.findall(r"case\s+(D3DPT_\w+)\s*:", body.group(1) if body else ""))
    # Assignments to the *pipeline key's* topology; the Vulkan struct's own
    # `input_assembly.topology = To_Vk_Topology(...)` is the translation, not the choice.
    assignments = [value.strip() for value in re.findall(r"\.topology\s*=\s*([^;]+);", impl)
                   if not value.strip().startswith(("To_Vk_Topology", "VK_"))]
    dynamic = any(not re.fullmatch(r"D3DPT_\w+", value) for value in assignments)
    literals = {value for value in assignments if re.fullmatch(r"D3DPT_\w+", value)}
    out = {}
    for primitive, uses in sorted(primitive_types.items()):
        if primitive not in translated:
            status = "absent"
        elif dynamic or primitive in literals:
            status = "implemented"
        else:
            status = "stubbed"
        out[primitive] = {"uses": uses, "status": status,
                          "reason": "" if status == "implemented" else
                          "translated, but the pipeline key's topology is fixed"}
    return out


# --------------------------------------------------------------------------------------
# report
# --------------------------------------------------------------------------------------

def summarise(items, statuses=("implemented", "stubbed", "ignored", "absent",
                               "none-needed")):
    counts = {status: 0 for status in statuses}
    for item in items.values():
        counts[item["status"]] = counts.get(item["status"], 0) + 1
    return counts


def print_report(payload):
    methods = payload["methods"]
    backend_methods = {k: v for k, v in methods.items() if v["category"] == "backend"}
    counts = summarise(backend_methods)
    print(f"D3D8 entry points reached by the engine: {len(methods)} "
          f"({sum(1 for v in methods.values() if v['interface'] == 'IDirect3DDevice8')} "
          f"IDirect3DDevice8, "
          f"{sum(1 for v in methods.values() if v['interface'] == 'IDirect3D8')} "
          "IDirect3D8)")
    for category in sorted({v["category"] for v in methods.values()}):
        n = sum(1 for v in methods.values() if v["category"] == category)
        print(f"  {category:13s} {n}")
    print(f"backend methods implemented: {counts['implemented']}/{len(backend_methods)}"
          f", stubbed {counts['stubbed']}, absent {counts['absent']}")
    print()

    for title, key in (("render states (D3DRS_*)", "render_states"),
                       ("texture-stage states (D3DTSS_*)", "stage_states"),
                       ("cascade ops (D3DTOP_*)", "cascade_ops"),
                       ("primitive types (D3DPT_*)", "primitive_types")):
        items = payload[key]
        counts = summarise(items)
        print(f"{title}: {len(items)} the engine sets, "
              f"{counts['implemented']} implemented, {counts['ignored']} ignored "
              f"by design, {counts['stubbed']} stubbed, {counts['absent']} absent")

    print()
    print("| D3D8 method | interface | sites | category | entry point | status |")
    print("| --- | --- | --- | --- | --- | --- |")
    for name, item in sorted(methods.items(),
                             key=lambda kv: (kv[1]["category"], kv[0])):
        entry = ", ".join(f"`{e}`" for e in item["entry"]) or "-"
        print(f"| `{name}` | {item['interface']} | {item['sites']} | "
              f"{item['category']} | {entry} | {item['status']} |")

    for title, key in (("Render states", "render_states"),
                       ("Texture-stage states", "stage_states"),
                       ("Cascade ops", "cascade_ops"),
                       ("Primitive types", "primitive_types")):
        print()
        print(f"| {title} | uses | status | note |")
        print("| --- | --- | --- | --- |")
        for name, item in sorted(payload[key].items(),
                                 key=lambda kv: (-kv[1]["uses"], kv[0])):
            print(f"| `{name}` | {item['uses']} | {item['status']} | "
                  f"{item.get('reason', '')} |")

    absent = [name for name, item in payload["render_states"].items()
              if item["status"] in ("absent", "stubbed")]
    absent += [name for name, item in payload["stage_states"].items()
               if item["status"] in ("absent", "stubbed")]
    absent += [name for name, item in payload["cascade_ops"].items()
               if item["status"] == "absent"]
    absent += [name for name, item in payload["primitive_types"].items()
               if item["status"] in ("absent", "stubbed")]
    print()
    print(f"states/ops the engine sets and the backend does not serve: {len(absent)}")
    for name in absent:
        print(f"  {name}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--json", help="write the machine-readable coverage payload here")
    ap.add_argument("--sites-json", help="reuse a d3d8-surface-scan.py --json-out payload")
    ap.add_argument("--states-json",
                    help="reuse a d3d8-surface-scan.py --states-json-out payload")
    ap.add_argument("--quiet", action="store_true", help="write the JSON, print nothing")
    args = ap.parse_args()

    coverage_map = json.loads(read(COVERAGE_MAP))
    demand = measure_demand(args.sites_json, args.states_json)
    methods, render_states, stage_states, cascade_ops, primitive_types = demand
    sources = backend_sources()
    payload = {
        "methods": measure_methods(methods, coverage_map, sources),
        "render_states": measure_render_states(render_states, sources),
        "stage_states": measure_stage_states(stage_states, sources),
        "cascade_ops": measure_cascade_ops(cascade_ops, sources),
        "primitive_types": measure_primitive_types(primitive_types, sources),
    }
    backend_methods = {k: v for k, v in payload["methods"].items()
                       if v["category"] == "backend"}
    payload["totals"] = {
        "methods_reached": len(payload["methods"]),
        "backend_methods": len(backend_methods),
        "backend_methods_implemented": summarise(backend_methods)["implemented"],
        "render_states_set": len(payload["render_states"]),
        "render_states_implemented": summarise(payload["render_states"])["implemented"],
        "stage_states_set": len(payload["stage_states"]),
        "stage_states_implemented": summarise(payload["stage_states"])["implemented"],
        "cascade_ops_used": len(payload["cascade_ops"]),
        "cascade_ops_implemented": summarise(payload["cascade_ops"])["implemented"],
        "primitive_types_used": len(payload["primitive_types"]),
        "primitive_types_implemented":
            summarise(payload["primitive_types"])["implemented"],
    }

    if args.json:
        with open(args.json, "w") as fh:
            json.dump(payload, fh, indent=1, sort_keys=True)
            fh.write("\n")
    if not args.quiet:
        print_report(payload)
    return 0


if __name__ == "__main__":
    sys.exit(main())
