# Untyped (FVF 0) vertex buffers and the D3D8 vertex-declaration path

Measured on Linux x86-64 (lavapipe, `VK_LAYER_KHRONOS_validation` loaded) with the native binary
from `scripts/native-build.py --level 1..4 --with-shims --strict-link` (980/980 objects,
0 unresolved) at the commit this document ships with. The enforced numbers are produced by
`scripts/ci/check-untyped-vertex-buffer.py` on every CI run (Linux/lavapipe and macOS
arm64/MoltenVK); nothing quoted here is the source of truth.

Every result below carries one of the project's four labels: **PORT DEFECT**, **UNIMPLEMENTED
PATH**, **MISSING DATA**, **SYNTHETIC-ONLY**.

## 0. Summary

- `Decode_Fvf: unsupported FVF 0x0`, printed twice per run, was the backend refusing two *valid*
  D3D8 requests: `CreateVertexBuffer(..., FVF=0)` from `W3DProjectedShadowManager` (32768 decal
  vertices, 49152 bytes) and `W3DVolumetricShadowManager` (4096 volume vertices, 786432 bytes).
  FVF 0 is legal D3D8: the layout of an untyped buffer is supplied later, by the FVF that is
  current at draw time or by the `D3DVSD_*` declaration of the bound vertex shader.
- The failure was **silent at the frame level and fatal underneath** (§1): the engine discards
  the `ReAcquireResources()` result, keeps a null buffer, and the frame is reported as
  presented with no shadows. The first time the shadow path actually runs it dereferences the
  null buffer (`shadowVertexBufferD3D->Lock`, `W3DVolumetricShadow.cpp:1412`) and the process
  dies with SIGSEGV. That is the same defect class as the 64-draw cap: a "successful" frame
  without the geometry. **PORT DEFECT** (in the backend), now fixed and made loud.
- The engine's declaration set is **bounded**: three distinct `D3DVSD_*` shapes from six source
  sites (§2). All use one stream, `FLOAT2/FLOAT3/D3DCOLOR` only, no `SKIP`, no tessellator.
- The backend now (§3) creates FVF-0 buffers, resolves their layout at draw time (declaration
  of the bound program, else the fixed-function FVF), honours the explicit `SetStreamSource`
  stride, keys pipelines on `{fvf, declaration hash, stride}`, and **refuses and counts** any
  untyped draw for which no decodable layout exists.
- It is **proven to draw** (§4) by a readback-classified spike workload: 7 layouts x 64 tiles,
  every tile's identity read back from the framebuffer, the layout-less draw refused and
  counted, validation silent, and two negative controls under which the gate fails.
  **SYNTHETIC-ONLY.**
- In the real game (§5) both buffers are now created and, when the shadow path executes, it
  issues up to 866 untyped draws per frame through the new path with 0 dropped. That is draw
  accounting, not framebuffer proof: no retail frame with shadows has been read back. Those
  measurements predate #137, which independently removed the raw `IDirect3DDevice8*` gate
  that had kept the shadow path from running natively (§6).

## 1. What was silently not drawn (measured)

### 1.1 What the backend did

`VulkanRenderBackendClass::Create_Vertex_Buffer` called `Decode_Fvf(0)`, which has no case for 0,
printed `Decode_Fvf: unsupported FVF 0x0` and returned failure; the D3D8 bridge returned
`D3DERR_INVALIDCALL`. Loud at the resource level, once per manager, at map load.

### 1.2 What the engine did with the failure

```
BaseHeightMap.cpp:457    TheW3DShadowManager->ReAcquireResources();      // result discarded
W3DShadow.cpp:160-170    W3DShadowManager::ReAcquireResources()          // FALSE if either fails
W3DVolumetricShadow.cpp  shadowVertexBufferD3D      stays nullptr
W3DProjectedShadow.cpp   shadowDecalVertexBufferD3D stays nullptr
```

There is no fallback path: the manager objects exist, `renderStencilShadows()` and
`flushDecals()` are still called every frame, and they will `Lock()` the null buffer as soon as
there is a shadow to draw. Silent at the frame level (`draws/frame ... dropped 0`), no
"missing geometry" accounting anywhere.

### 1.3 Measured, three configurations of the same map (GDB, `zh-run/shadow-ab.gdb`)

| configuration | result |
| --- | --- |
| `main` backend, `W3DDisplay::draw` gate as committed (§6) | `Decode_Fvf: unsupported FVF 0x0` x2; 91-100 draws/frame, 0 untyped; `renderStencilShadows`/`flushDecals` hit every frame (2170 each), `RenderMeshVolume`/`RenderDynamicMeshVolume` hit 0 times; frame reported presented; **no shadows, silently** |
| `main` backend, gate lifted so `updateViews()` runs | **SIGSEGV** in `W3DVolumetricShadow::RenderDynamicMeshVolume` at `W3DVolumetricShadow.cpp:1412` (`shadowVertexBufferD3D->Lock` on null), frame 175 |
| this branch's backend, gate lifted | `untyped vertex buffer: 49152 bytes created with FVF 0`, `... 786432 bytes ...`, `untyped vertex buffer layout: FVF 0x2 stride 12`; `draws/frame: requested 1772 issued 1772 dropped 0 untyped 866/0`; `RenderMeshVolume` hit 19184 times, `RenderDynamicMeshVolume` 79732 times over 462 frames; no SIGSEGV |

The volumetric shadow path draws through `SHADOW_DYNAMIC_VOLUME_FVF` (= `D3DFVF_XYZ`, stride 12),
which is why the runtime layout line says `FVF 0x2 stride 12` and not a declaration: the shadow
managers create *untyped* buffers but draw them with fixed-function FVFs. The declaration
consumers are elsewhere (§2).

### 1.4 Loud now

- A draw from an untyped buffer with no resolvable layout is refused and counted
  (`untyped_draws_dropped`, reported in `draws/frame ... untyped issued/dropped`), never
  issued with a guessed layout.
- A declaration the decoder does not support is refused at `Create_Vertex_Shader` with the
  reason (`Create_Vertex_Shader: refused, the declaration has <reason>`), so the program is
  never bound and its draws land in the counted refusal above.
- `ZH_RENDER_NO_UNTYPED_VB=1` and `ZH_RENDER_NO_VERTEX_DECLARATION=1` disable the two paths
  explicitly so the gate has a negative control (§4).

## 2. Enumeration of the engine's vertex declarations

### 2.1 From source (`Core/GameEngineDevice`, `GeneralsMD/Code`, WW3D2)

Six `D3DVSD_*` declaration streams, three distinct shapes:

| shape | stride | sites |
| --- | --- | --- |
| `STREAM(0) REG(0,FLOAT3) REG(1,D3DCOLOR) REG(2,FLOAT2) END` | 24 | `W3DShaderManager.cpp:294`, `:1952`, `W3DWater.cpp:898` |
| `STREAM(0) REG(0,FLOAT3) REG(1,D3DCOLOR) REG(2,FLOAT2) REG(3,FLOAT2) END` | 32 | `W3DShaderManager.cpp:2244`, `:3500` |
| `STREAM(0) REG(0,FLOAT3) REG(1,FLOAT3) REG(2,D3DCOLOR) REG(7,FLOAT2) END` | 36 | `W3DTreeBuffer.cpp:1169` |

Tokens used: `D3DVSD_STREAM`, `D3DVSD_REG`, `D3DVSD_END`, data types `FLOAT2`, `FLOAT3`,
`D3DCOLOR`. Not used anywhere: `D3DVSD_SKIP`, `D3DVSD_STREAM_TESS`, multiple streams, `UBYTE4`,
`SHORT2`, `SHORT4`, `FLOAT1`, `FLOAT4`.

FVF-0 `CreateVertexBuffer` sites (the buffers whose layout is deferred):

| site | consumer | drawn with |
| --- | --- | --- |
| `W3DProjectedShadow.cpp:288` (`SHADOW_DECAL_VERTEX_SIZE` x 24) | decals | fixed-function `SHADOW_DECAL_FVF` |
| `W3DVolumetricShadow.cpp:3741` (`SHADOW_VERTEX_SIZE` x 16) | volumes | fixed-function `SHADOW_DYNAMIC_VOLUME_FVF` (`XYZ`) / `SV_DEBUG` (`XYZ\|DIFFUSE`) |
| `W3DWater.cpp:666` static-water branch (`fvf=0; // DX8 Docs confusing on this`) | water mesh | the 24-byte declaration above |

Everything else in the engine passes a non-zero FVF. **Bounded**, so the path was implemented
(§3) rather than reported.

### 2.2 At runtime

Confirmed at runtime on the mission map used above: both shadow buffers are created with FVF 0
and drawn with FVF `0x2` (§1.3). The declaration-backed consumers (`W3DShaderManager` terrain
shaders, `W3DTreeBuffer`, static water) did **not** appear in the runs sampled: no
`vertex declaration <hash>:` line was printed. Whether that is because the sampled map/shader
mode never takes those branches, or because the native build's shader-capability detection
routes them to the fixed-function fallbacks, was not isolated in this slice. Retail confirmation
of the three declaration shapes is therefore **MISSING DATA**; their runtime confirmation is
synthetic (§4), against the byte-exact shapes from §2.1.

## 3. What was implemented

Seam preserved: consumers call `DX8Wrapper`; the native bridge (`vulkanrenderbackend.cpp`)
translates D3D8 into `spike::RenderBackendClass`; no new direct `IDirect3DDevice8` call site.
Windows is untouched: `d3d8renderbackend` already hands declarations to the real API, and no
Windows-compiled source changes in this slice. (The raw `_Get_D3D_Device8()` null checks in the
shadow managers that kept the path from running natively were replaced by
`DX8Wrapper::Has_Render_Device()` in #137, on which this slice now sits.)

Backend (`spikes/renderer/src`):

- `Create_Vertex_Buffer(FVF 0)` succeeds and records the buffer as untyped
  (`untyped vertex buffer: N bytes created with FVF 0`).
- `Set_Fixed_Function_Fvf(fvf)` and `Set_Vertex_Buffer(vb, stream, stride)` were added to
  `RenderBackendClass` so the bridge can forward `SetVertexShader(FVF)` and the explicit
  `SetStreamSource` stride (entered in `check-d3d8-surface.py` / `check-backend-coverage.py` /
  `backend-coverage-map.json` together).
- `Decode_Vertex_Declaration()` (`state_translate.cpp`) translates a `D3DVSD_*` token stream
  into the same `VertexLayout` the FVF path feeds into
  `VkPipelineVertexInputStateCreateInfo`: one stream, stream 0, `FLOAT1..4` and `D3DCOLOR`,
  offsets and stride accumulated in declaration order, register numbers returned for the
  program's inputs, FNV-1a hash for pipeline keying. Everything else (`SKIP`, tessellator,
  second stream, `UBYTE4`/`SHORT*`, register before stream, too many inputs) is refused with a
  reason. Elements are mapped to `VertexLayout` attribute slots by component count in
  declaration order; that is sufficient for the three shapes in §2.1 and is the **supported
  subset**, not a general declaration compiler.
- `Create_Vertex_Shader(declaration, ...)` decodes and stores the layout on the program and
  logs it (`vertex declaration <hash>: N input(s) stride S [v0:FLOAT3@0 ...]`).
- At draw time `Resolve_Draw_Layout()` picks, in order: the buffer's own layout (typed
  buffers); the bound program's declaration (untyped buffer, program bound,
  declarations enabled); the current fixed-function FVF (untyped buffer, no program); otherwise
  **refuse and count**. Resolved layouts are re-strided to the `SetStreamSource` stride when it
  is larger than the natural stride (`Apply_Stream_Stride`) and cached per
  `{layout, stride}`.
- `PipelineKey` gained `declaration` and `vertex_stride`, so an FVF-0 buffer drawn at stride 24
  and again at stride 32 gets two pipelines, and a declaration never aliases an FVF pipeline.
- The bridge's `SetVertexShader(program handle)` clears the fixed-function FVF before binding,
  so a program whose declaration was refused cannot be silently drawn with a stale FVF; it hits
  the counted refusal instead.

## 4. Proof that it draws (SYNTHETIC-ONLY)

`spikes/renderer/src/untyped_vertex_buffer.cpp`, modelled on `draw_capacity.cpp`: one FVF-0
vertex buffer, eight cases, each case 64 draws into its own 4x4-tile region of a 512x512 target,
each tile's colour encoding its case and tile id so the readback classifies every tile
individually (correct / wrong / dropped). The gate is
`scripts/ci/check-untyped-vertex-buffer.py`.

| case | layout source | stride | result |
| --- | --- | --- | --- |
| 0 | FVF `XYZ` (shadow volume) | 12 | 64/64 correct |
| 1 | FVF `XYZ\|DIFFUSE` (SV_DEBUG volume) | 16 | 64/64 |
| 2 | FVF `XYZ\|DIFFUSE\|TEX1` (shadow decal) | 24 | 64/64 |
| 3 | FVF `XYZ\|DIFFUSE\|TEX1` at explicit stride 32 | 32 | 64/64 |
| 4 | declaration `FLOAT3 D3DCOLOR FLOAT2` (water / shader manager) | 24 | 64/64 |
| 5 | declaration `FLOAT3 FLOAT3 D3DCOLOR FLOAT2` (trees, reg 7) | 36 | 64/64 |
| 6 | declaration `FLOAT3 FLOAT2 D3DCOLOR` (an order no FVF can express) | 24 | 64/64 |
| 7 | nothing bound | - | 0 drawn, 64 refused and counted |

Backend accounting for the run: `requested 449 issued 448 dropped 1, untyped issued 448 dropped 1`;
`validation layer: loaded`, `validation messages: 0`. The three-frame run repeats the
classification every frame.

Negative controls, both run by the gate on every CI invocation, both required to **fail**:

- `ZH_RENDER_NO_UNTYPED_VB=1`: `Create_Vertex_Buffer(FVF 0)` is refused (loudly), the workload
  exits non-zero. Proves the FVF-0 path is the one under test.
- `ZH_RENDER_NO_VERTEX_DECLARATION=1`: cases 0-3 still draw 64/64, cases 4-6 come back
  64 dropped each and counted, the workload exits non-zero. Proves the declaration cases are
  drawn by the declaration path and not by some FVF that happens to coincide.

Case 5 initially failed 0/64: the case table carried a 28-byte stride for a 36-byte layout. The
readback caught it; the gate is one that fails.

What this does **not** prove: that retail shadow, water or tree geometry appears in a retail
frame. It proves the backend consumes an untyped buffer through both layout sources and puts
the right bytes in the right pixels.

## 5. What changed in the real game

- Before: `Decode_Fvf: unsupported FVF 0x0` x2 at map load, null shadow buffers, SIGSEGV the
  first time a shadow is actually rendered (§1.3).
- After: `untyped vertex buffer: 49152 bytes created with FVF 0` and
  `... 786432 bytes created with FVF 0` at map load; no `Decode_Fvf` refusal; when the shadow
  path executes (gate lifted experimentally), `draws/frame: requested 1772 issued 1772 dropped 0
  untyped 866/0`, stencil D24S8 selected under `-win`, no crash over 462 frames.
- With the tree as committed on `main` (gate in place, §6): 91-100 draws/frame, `untyped 0/0`,
  because the shadow managers are polled every frame but never given anything to draw.

No retail framebuffer was read back to confirm shadow pixels, and the sampled runs issued no
declaration-backed draws. Retail proof of visible shadows is **not established**.

## 6. What remains, classified

1. **`W3DDisplay::draw` and `W3DScene` gated on the raw `IDirect3DDevice8*` — PORT DEFECT,
   fixed independently by #137 (`DX8Wrapper::Has_Render_Device()`), not by this slice.**
   The measurements in §1.3 and §5 were taken before #137 landed, on a tree where
   `updateViews()` never ran natively (`Peek_D3D_Device8()` is `nullptr` on the Vulkan
   backend), which is why the as-committed row shows `untyped 0/0`: the shadow managers were
   polled but never populated. The "gate lifted" rows are the configuration `main` now has.
   With #137 merged the shadow path runs on every native mission frame, so the two FVF-0
   buffers are drawn — through this slice's path — rather than crashing on a null `Lock`
   (§1.3, row 2). The combined `main` + this branch state has not been re-measured in the
   retail game; that is the first thing to do before quoting a draws/frame number for it.
2. **Declaration decoder is a bounded subset — UNIMPLEMENTED PATH for anything outside §2.1.**
   Multiple streams, `D3DVSD_SKIP`, tessellator streams, `UBYTE4`/`SHORT*`, and semantic
   assignment beyond component-count order are refused with a reason. No engine source uses
   them; if a mod or later slice does, the refusal is loud and counted.
3. **Retail confirmation of the three declaration shapes — MISSING DATA.** No sampled run
   bound a `D3DVSD_*` program. Isolating which map/shader mode reaches `W3DShaderManager`'s
   shader-2.0 terrain paths, `W3DTreeBuffer`, or static water on the native build is a
   follow-up.
4. **Stencil format under fullscreen.** Under `-win` the device selects `D3DFMT_D24S8`
   (`Has_Stencil() == true`); an earlier fullscreen run selected `D3DFMT_D32` with no stencil,
   which would disable volumetric shadows through the engine's own `Has_Stencil()` check rather
   than through anything here. Not re-measured in this slice.

## 7. Gates run at this commit

`flake8`, `actionlint`, `check-spike-render.py` (delta 0), `check-draw-capacity.py --self-check`,
`check-d3d8-surface.py` + `check-backend-coverage.py` (baseline matches exactly),
`check-untyped-vertex-buffer.py` (PASS, both negative controls fail as required), strict native
build level 1-4 with shims (980/980, 0 unresolved, binary produced),
`check-generated-baselines.py`, `porting-status.py`. The Wine/VC6 Windows build
(`scripts/docker-build.sh --game zh`) was run (rc 0) on the pre-merge commit, when this branch
still carried its own shadow-manager edits; after merging #137 the branch changes no
Windows-compiled source. The retail replay gate was not run: nothing here touches
serialisation or simulation.
