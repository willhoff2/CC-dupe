# Phase 4 renderer spike — the D3D8 surface, and whether Vulkan can serve it

Output of the time-boxed renderer spike. Three parts:

1. the exact Direct3D 8 interface surface the Zero Hour engine uses, measured, with each
   method mapped to Vulkan and the no-clean-mapping cases called out;
2. what a working Vulkan proof-of-concept through a `DX8Wrapper`-shaped interface actually
   renders — see `spikes/renderer/`;
3. a recommendation and a revised Phase 4 estimate.

> **Follow-up measurement.** `docs/porting/fixed-function-measurements.md` measures the
> *semantics* behind the two riskiest methods on this surface — the texture-stage cascade
> (17 of D3D8's 26 ops, 8 argument encodings, 8 stages, 314 distinct stage0+stage1 programs)
> and the FVF set (21 declarations) — records the pipeline-count estimate (252 for a
> preset-only frame, 32,256 upper bound, 161 M if the cascade were compiled rather than
> interpreted), and revises three line items of the estimate below downward by ~95–155 h.

## 0. Corrections to the previously stated numbers

> The counts in this section are the **pre-refactor** measurement (the state the spike found).
> They have since been acted on: see [0.5](#05-post-refactor-state-after-the-dx8wrapper-consolidation)
> for the current numbers. Everything from section 1 onward still describes the surface itself,
> which the refactor did not change — only where it is called from.

`docs/porting/next-slice-scope.md` said:

> All Direct3D 8 traffic goes through `DX8CALL(...)` in `dx8wrapper`: **13,517 LOC of `dx8*`
> files, 45 call sites, 34 distinct D3D8 device methods**.

Measured against the repo:

| Claim | Verdict | Measured |
|---|---|---|
| 13,517 LOC of `dx8*` files | **correct** | 13,517 (21 files, `Core/Libraries/Source/WWVegas/WW3D2/dx8*`) |
| 45 `DX8CALL` call sites | **wrong** | **82** macro sites (`DX8CALL` + `DX8CALL_HRES`); the raw grep that yields 90 includes the 3 macro definitions and non-call mentions |
| 34 distinct device methods | **wrong** | **52** `IDirect3DDevice8` methods, plus **10** `IDirect3D8` methods = **62** distinct |
| "all D3D8 traffic goes through `DX8CALL`" | **wrong, and this is the important one** | 82 of 458 call sites (18%) go through the macro. The other **376 (82%) bypass `DX8Wrapper` entirely** and call `IDirect3DDevice8` directly |

The "one wrapper to retarget" premise does not hold as stated. The macro is the tidy 18%;
the device-layer files above it reach around the wrapper and talk to D3D8 directly. Concretely,
the top offenders by direct call site:

| File | D3D8 call sites |
|---|---:|
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/Water/W3DWater.cpp` | 129 |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DShaderManager.cpp` | 103 |
| `Core/Libraries/Source/WWVegas/WW3D2/dx8wrapper.cpp` | 61 |
| `GeneralsMD/.../GameClient/Shadow/W3DVolumetricShadow.cpp` | 38 |
| `GeneralsMD/.../GameClient/Shadow/W3DProjectedShadow.cpp` | 25 |
| `Core/Libraries/Source/WWVegas/WW3D2/dx8wrapper.h` | 23 |
| `GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/shader.cpp` | 16 |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DTreeBuffer.cpp` | 14 |

Water, the shader manager and the two shadow systems are effectively a second, undocumented
renderer backend sitting beside `DX8Wrapper`. **They are the schedule risk, not `dx8wrapper.cpp`.**

This does not invalidate the plan's conclusion — 62 methods is still a small, fixed-function-era
slice of D3D8 and it is still worth retargeting rather than rewriting — but Phase 4 is not "port
one file", it is "port one file and then chase 376 call sites through five more".

### Reproducing

```sh
python3 spikes/renderer/tools/d3d8-surface-scan.py
```

Methodology, stated so the numbers can be argued with:

- Scope is `Core/` + `GeneralsMD/`, excluding any `*/Tools/*` tree (WorldBuilder, W3DView,
  GUIEdit, ImagePacker — all cut by the plan) and `Generals/` (base game, not Zero Hour).
- Comments are stripped before matching, so commented-out code does not inflate counts.
- An `x->Method(` site counts only if `x` is a known device accessor (`_Get_D3D_Device8()`,
  `D3DDevice`) or an identifier declared somewhere in scope as `IDirect3DDevice8*` /
  `LPDIRECT3DDEVICE8`, **and** `Method` is a real member of that interface. Both conditions are
  needed: aliases like `m_pDev`, `pDev`, `device` are used for non-D3D types too.
- The counts are **lower bounds**. The comment stripper is regex-based, and one unbalanced `/*`
  inside a string or `//` line can swallow live code; one such case is known
  (`W3DVolumetricShadow.cpp:3362`, a real `SetVertexShader` site the scanner misses). The error
  is ~0.2% at this scale.

## 0.5 Post-refactor state, after the `DX8Wrapper` consolidation

> Those 155 sites have since been moved onto an abstract backend interface, with D3D8 as the
> only implementation. See [`renderer-seam.md`](renderer-seam.md) for the interface, the
> resource-handle measurement, the hot-path reasoning and the lifecycle story.

Step 2 of "what to do next" (below) has been done: every `IDirect3DDevice8` / `IDirect3D8` call
site outside `dx8wrapper.{h,cpp}` now goes through `DX8Wrapper`, still on D3D8, verified by the
Windows build (`./scripts/docker-build.sh --game zh`).

| | Before | After |
|---|---:|---:|
| `IDirect3DDevice8` call sites | 439 (82 macro, **357 direct**) | 129 (129 macro, **0 direct**) |
| `IDirect3D8` call sites | 19 (0 macro, **19 direct**) | 26 (26 macro, **0 direct**) |
| Total call sites | 458 (82 macro, **376 direct**) | 155 (155 macro, **0 direct**) |
| Distinct methods reached | 62 | 63 |

The total drops from 458 to 155 because the duplicated call sites collapse: 129 hand-written
`SetTextureStageState`/`SetRenderState`/`Draw*` calls scattered across the water code become calls
to the handful of wrapper entry points that issue them. The distinct-method count is one higher, at 63: `GetViewport`
was called through a local `IDirect3DDevice8 *device` in `W3DProfilerFrameCapture.cpp`, and
`device` is on the scanner's deny-list of too-generic aliases, so it was never counted. It is a
`DX8Wrapper::Get_Viewport()` call now and shows up. No D3D8 method was lost or emulated away —
the surface is unchanged, only its call graph is.

Per-subsystem, direct sites eliminated:

| File | Before | After |
|---|---:|---:|
| `.../GameClient/Water/W3DWater.cpp` | 129 | 0 |
| `.../GameClient/W3DShaderManager.cpp` | 103 | 0 |
| `.../GameClient/Shadow/W3DVolumetricShadow.cpp` | 38 | 0 |
| `.../GameClient/Shadow/W3DProjectedShadow.cpp` | 25 | 0 |
| `.../GameClient/W3DTreeBuffer.cpp` | 14 | 0 |
| `.../GameClient/W3DMouse.cpp` | 7 | 0 |
| `.../GameClient/HeightMap.cpp`, `W3DSmudge.cpp` | 6, 6 | 0, 0 |
| `WW3D2/dx8caps.cpp`, `dx8vertexbuffer.cpp`, `dx8indexbuffer.cpp` | 4, 3, 2 | 0, 0, 0 |
| `.../GameClient/W3DSnow.cpp`, `W3DScene.cpp`, `W3DProfilerFrameCapture.cpp` | 3, 3, 3 | 0, 0, 0 |
| `BaseHeightMap.cpp`, `W3DShroud.cpp`, `W3DDisplay.cpp`, `ww3d.cpp` | 1 each | 0 |
| `WW3D2/dx8wrapper.cpp` (the wrapper itself) | 61 (26 direct) | 61 (0 direct) |

### What was added to `DX8Wrapper`

The wrapper already owned the cached state setters (`Set_DX8_Render_State` and friends), which
keep a shadow copy and skip redundant D3D8 calls. Most of the converted call sites deliberately
*bypassed* that cache — they set state behind the wrapper's back and restored it afterwards — so
routing them through the cached setters would have corrupted the shadow state and changed what is
rendered. They therefore call new pass-through entry points that do the D3D8 call and nothing
else:

`Set_DX8_Render_State_Uncached`, `Get_DX8_Render_State_Uncached`,
`Set_DX8_Texture_Stage_State_Uncached`, `Set_DX8_Texture_Uncached`, `Set_DX8_Transform_Uncached`,
`Set_DX8_Vertex_Shader_Uncached`, `Set_DX8_Pixel_Shader_Uncached`,
`Set_DX8_{Vertex,Pixel}_Shader_Constant_Uncached`, `Create_DX8_Texture_Uncached`.

Alongside them, entry points for the parts of D3D8 the wrapper simply did not expose: resource
creation (`Create_DX8_{Vertex,Index}_Buffer`, `Create_DX8_Image_Surface`,
`Create_DX8_{Vertex,Pixel}_Shader` and their deletes), submission (`Set_DX8_Stream_Source`,
`Set_DX8_Indices`, `Draw_DX8_Primitive`, `Draw_DX8_Indexed_Primitive`, `Draw_DX8_Primitive_UP`,
`Process_DX8_Vertices`), render targets (`Get/Set_DX8_Render_Target_Surface`,
`Get_DX8_Depth_Stencil_Surface`, `Get_Viewport`), device housekeeping
(`Test_Cooperative_Level`, `Validate_DX8_Device`, `Present_DX8_Device`,
`Get_DX8_Available_Texture_Mem`, `Discard_DX8_Resource_Manager_Bytes`), cursor and gamma, and the
whole `IDirect3D8` adapter/capability enumeration (`Get_DX8_Adapter_*`, `Check_DX8_Device_*`,
`Check_DX8_Depth_Stencil_Match`, `Get_DX8_Device_Caps`).

Two macros were added next to `DX8CALL`: `DX8CALL_RAW` / `DX8CALL_RAW_HRES` (and the `_D3D`
variants) issue the call and count it, but do **not** run `DX8_ErrorCode` on the result. They
exist because a converted call site either ignored the `HRESULT` or tested it itself; feeding it
to `DX8_ErrorCode` would turn a tolerated failure into an assert. The surface scanner was taught
about them, otherwise it silently stops counting those sites.

`DX8Caps` no longer takes an `IDirect3DDevice8*`: it only used it to toggle
`D3DRS_SOFTWAREVERTEXPROCESSING` while probing, which now goes through the wrapper.

### Honest exceptions — what is still not behind the wrapper

1. **`dx8wrapper.{h,cpp}` itself: 61 call sites.** This is the wrapper's own implementation, i.e.
   the chokepoint. They are all expressed through the `DX8CALL*` macros now (the 26 that still
   poked `D3DDevice` / `D3DInterface` by hand were converted), so a backend swap has exactly one
   file to rewrite — but they are, unavoidably, direct D3D8 calls.
2. **`DX8Wrapper::_Get_D3D_Device8()` survives as a null test in 23 places** (`if
   (!DX8Wrapper::_Get_D3D_Device8()) return;`, `DEBUG_ASSERTCRASH(...)`). These ask "is there a
   device yet", not "call D3D8"; they cost nothing to keep and a `Has_Device()` predicate can
   replace them mechanically when the backend changes. The scanner does not count them, correctly
   — no interface method is invoked.
3. **`dx8webbrowser.cpp:95`** hands the raw `IDirect3DDevice8*` to the embedded browser control
   (`pBrowser->Initialize(reinterpret_cast<long*>(...))`). The pointer leaves the engine and is
   consumed by a closed COM component, so there is nothing to abstract; this path is Windows-only
   and does not survive the port anyway.
4. **D3DX entry points are untouched** (`D3DXCreateTexture`, `D3DXFilterTexture`,
   `D3DXLoadSurfaceFromSurface`, the maths helpers). They are a separate library, not
   `IDirect3DDevice8`, and are already listed in section 3 as their own work item.

## 1. The surface, method by method

Sites are (macro + direct). Difficulty: **1:1** = a direct Vulkan call; **wrapper** = needs
bookkeeping in the backend but nothing invented; **hard** = no clean mapping, needs emulation or
a call-site change; **drop** = not a rendering concern, belongs to the platform layer.

### 1.1 Draw submission and frame control

| D3D8 method | Sites | Vulkan | Difficulty |
|---|---:|---|---|
| `BeginScene` | 1 (1+0) | `vkBeginCommandBuffer` + `vkCmdBeginRenderPass` | wrapper |
| `EndScene` | 1 (1+0) | `vkCmdEndRenderPass` + `vkEndCommandBuffer` + `vkQueueSubmit` | wrapper |
| `Present` | 2 (0+2) | `vkQueuePresentKHR` | wrapper |
| `Clear` | 1 (1+0) | **not** a render-pass `loadOp` — `vkCmdClearAttachments` | wrapper |
| `DrawIndexedPrimitive` | 11 (2+9) | `vkCmdDrawIndexed` | 1:1 |
| `DrawPrimitive` | 1 (0+1) | `vkCmdDraw` | 1:1 |
| `DrawPrimitiveUP` | 11 (0+11) | no equivalent: "draw from user pointer" needs a per-frame staging ring | **hard** |
| `ProcessVertices` | 2 (0+2) | no equivalent: CPU vertex transform into a VB. Either keep a CPU path or use a compute shader | **hard** |
| `SetStreamSource` | 15 (4+11) | `vkCmdBindVertexBuffers` | 1:1 |
| `SetIndices` | 12 (4+8) | `vkCmdBindIndexBuffer` — note D3D8's `BaseVertexIndex` becomes `vertexOffset` on the *draw*, not the bind | wrapper |

`Clear` deserves the emphasis. D3D8 `Clear` is a command issued mid-frame with per-call flags,
and the engine issues it more than once per frame in the render-to-texture paths. Folding it into
`VK_ATTACHMENT_LOAD_OP_CLEAR` changes semantics; `vkCmdClearAttachments` inside the pass is the
faithful mapping. The PoC does it this way.

`DrawPrimitiveUP` (11 sites, all in the shadow and water code) is a genuine annoyance: there is
no Vulkan concept of drawing straight from a CPU pointer. Every one of those sites needs a
dynamic buffer suballocated per frame.

### 1.2 Fixed-function state — the `SetRenderState` cascade

| D3D8 method | Sites | Vulkan | Difficulty |
|---|---:|---|---|
| `SetRenderState` | 65 (1+64) | see below — **no** single mapping | **hard** |
| `GetRenderState` | 4 (0+4) | shadow-state read; free once the backend keeps a shadow copy | wrapper |
| `SetTextureStageState` | 77 (16+61) | see 1.3 — **no** mapping at all | **hard** |
| `SetTransform` | 11 (6+5) | uniform/push-constant upload | wrapper |
| `GetTransform` | 2 (2+0) | shadow-state read | wrapper |
| `SetViewport` | 1 (1+0) | `VkViewport` (pipeline or dynamic state) | wrapper |
| `GetViewport` | 1 (0+1) | shadow-state read | wrapper |
| `SetClipPlane` | 1 (1+0) | no user clip planes in core Vulkan; emulate with a shader-side `gl_ClipDistance` or discard | **hard** |
| `SetMaterial` | 1 (1+0) | uniform upload, but only meaningful with fixed-function lighting (see below) | **hard** |
| `SetLight` / `LightEnable` | 1 (1+0) / 2 (2+0) | fixed-function lighting does not exist in Vulkan; must become shader code | **hard** |
| `ValidateDevice` | 1 (0+1) | no equivalent; `vkCreateGraphicsPipelines` failure is the closest signal | drop/stub |

The mismatch is structural, not per-enum: **D3D8 changes one piece of pipeline state at a time;
Vulkan bakes all of it into an immutable `VkPipeline`.** So the render states split into three
groups. Measured usage across the 370 `D3DRS_*` set-sites (53 distinct states — counting both
`SetRenderState` and `DX8Wrapper::Set_DX8_Render_State`):

**(a) bakes cleanly into a `VkPipeline`** — mechanical, needs a pipeline cache keyed on the
state block:

`ZENABLE`(5) `ZWRITEENABLE`(11) `ZFUNC`(23) `CULLMODE`(10) `FILLMODE`(15) `SHADEMODE`(2)
`ALPHABLENDENABLE`(33) `SRCBLEND`(34) `DESTBLEND`(32) `BLENDOP`(3) `COLORWRITEENABLE`(21)
`STENCILENABLE`(12) `STENCILFUNC`(11) `STENCILFAIL`(10) `STENCILZFAIL`(8) `STENCILPASS`(9)
`STENCILREF`(9) `STENCILMASK`(8) `STENCILWRITEMASK`(8) `DITHERENABLE`(1)

**(b) has no pipeline slot and must be emulated in a shader**:

| State | Sites | Why, and what it becomes |
|---|---:|---|
| `ALPHATESTENABLE` / `ALPHAFUNC` / `ALPHAREF` | 12 / 8 / 10 | alpha test was removed after D3D9. Becomes a `discard` in the fragment shader (the PoC does this) |
| `TEXTUREFACTOR` | 7 | a constant colour argument to the texture cascade; becomes a uniform |
| `LIGHTING` / `AMBIENT` / `COLORVERTEX` / `SPECULARENABLE` / `*MATERIALSOURCE` / `NORMALIZENORMALS` | 9 / 5 / 2 / 2 / 7 / 2 | fixed-function vertex lighting. Must be reimplemented as a vertex shader |
| `FOGENABLE` / `FOGCOLOR` / `FOGSTART` / `FOGEND` / `FOGTABLEMODE` / `FOGVERTEXMODE` / `RANGEFOGENABLE` | 3 / 1 / 1 / 1 / 1 / 1 / 1 | fixed-function fog. Vertex + fragment shader code |
| `WRAP0` | 2 | texture-coordinate wrapping for interpolation; shader-side |
| `CLIPPING` | 1 | no equivalent; usually ignorable |

**(c) no Vulkan analogue and no sane emulation** — needs a decision per site:

| State | Sites | Problem |
|---|---:|---|
| `ZBIAS` | 15 | D3D8's `ZBIAS` is a unitless integer 0–16 whose meaning was left to the driver. `depthBiasConstantFactor` is in depth units. **There is no correct conversion.** 15 sites will need tuning against reference screenshots — this is where "why does the shadow z-fight" bugs come from |
| `SOFTWAREVERTEXPROCESSING` | 3 | a D3D8 device mode. Drop |
| `POINTSPRITEENABLE`, `POINTSCALEENABLE`, `POINTSCALE_A/B/C`, `POINTSIZE`, `POINTSIZE_MIN/MAX` | 10 total | fixed-function point sprites (the snow system). Vulkan has `gl_PointSize` but no distance attenuation; the scale factors must be computed in the vertex shader |
| `PATCHSEGMENTS` | 1 | N-patch tessellation. Dead in practice; drop |

### 1.3 `SetTextureStageState` — the single worst item

77 device call sites, and 865 uses counting `DX8Wrapper::Set_DX8_Texture_Stage_State`, across
23 distinct `D3DTSS_*` states. **Vulkan has no equivalent of any of it.** The D3D8 texture-stage
cascade is a fixed-function combiner network; the only options are to interpret it in a shader at
runtime or to compile a shader permutation per state combination.

Measured usage:

| `D3DTSS_*` | Uses | Maps to |
|---|---:|---|
| `TEXCOORDINDEX` | 121 | which texcoord set feeds the stage → shader, or vertex-input remap |
| `COLOROP` / `ALPHAOP` | 93 / 92 | the combiner op → **shader interpretation** |
| `COLORARG1` / `COLORARG2` / `ALPHAARG1` / `ALPHAARG2` / `COLORARG0` | 64 / 62 / 40 / 32 / 2 | combiner operands → shader |
| `TEXTURETRANSFORMFLAGS` | 76 | fixed-function texcoord matrix, paired with `D3DTS_TEXTURE0..3` (46 uses) → shader |
| `MINFILTER` / `MAGFILTER` / `MIPFILTER` | 62 / 62 / 43 | `VkSampler` — but note this is *stage* state in D3D8 and *object* state in Vulkan, so it needs a sampler cache keyed on filter+address, not a sampler per texture |
| `ADDRESSU` / `ADDRESSV` / `ADDRESSW` / `BORDERCOLOR` | 48 / 48 / 2 / 1 | `VkSampler` |
| `MAXANISOTROPY` | 1 | `VkSampler` |
| `BUMPENVMAT00/01/10/11`, `BUMPENVLSCALE`, `BUMPENVLOFFSET` | 3/3/3/3/2/2 | `D3DTOP_BUMPENVMAP(LUMINANCE)` — a fixed-function bump path with no analogue. Must be hand-written as a shader |

The ops actually used, 17 distinct across 241 uses:

`DISABLE`(88) `MODULATE`(64) `SELECTARG1`(39) `ADD`(16) `SELECTARG2`(14) `MODULATE2X`(3)
`ADDSIGNED`(2) `ADDSMOOTH`(2) `BLENDCURRENTALPHA`(2) `DOTPRODUCT3`(2) `MULTIPLYADD`(2)
`SUBTRACT`(2) `ADDSIGNED2X`(1) `BLENDTEXTUREALPHA`(1) `BUMPENVMAP`(1)
`BUMPENVMAPLUMINANCE`(1) `MODULATEALPHA_ADDCOLOR`(1)

**This is the good news of the spike.** Five ops account for 221 of 241 uses, and the whole
vocabulary is 17 ops over 8 stages of which only the first two are heavily used. A single
uber-shader that interprets the cascade from a uniform block covers it. The PoC implements exactly
that — `spikes/renderer/shaders/fixedfunc.frag` — and it is ~120 lines of GLSL.

### 1.4 Vertex declarations — FVF through `SetVertexShader`

| D3D8 method | Sites | Vulkan | Difficulty |
|---|---:|---|---|
| `SetVertexShader` | 23 (2+21) | overloaded: an FVF bitfield *or* a shader handle | **hard** |
| `CreateVertexShader` / `DeleteVertexShader` | 1 (0+1) / 2 (0+2) | `vkCreateShaderModule` — but the input is vs.1.1 assembly | **hard** |
| `SetVertexShaderConstant` | 12 (1+11) | push constants or a uniform buffer | wrapper |

Of the 24 textual `SetVertexShader` sites, **20 pass an FVF bitfield** (literal, or a named
constant like `SHADOW_DYNAMIC_VOLUME_FVF` / `WATER_MESH_FVF` / `D3DFVF_POINTVERTEX`); only 3 pass
a real vertex-shader handle (`m_dwWaveVertexShader`, `m_dwTreeVertexShader`, and `DX8Wrapper`'s
own `Vertex_Shader` which holds either).

So the engine is overwhelmingly using D3D8's fixed-function vertex declaration. Vulkan has no
FVF; the bitfield must be decoded into explicit `VkVertexInputAttributeDescription`s. Measured
FVF vocabulary (20 distinct bits, 220 uses):

`XYZ`(40) `DIFFUSE`(38) `TEX1`(26) `NORMAL`(23) `TEX2`(21) `XYZRHW`(19) `SPECULAR`(5) `TEX3`(5)
`TEX4`(5) `TEXCOORDSIZE3`(5) `TEX5`(4) `TEX6`(4) `TEX7`(4) `TEX8`(4) `TEXCOORDSIZE2`(4)
`POINTVERTEX`(3) `TEXCOORDSIZE1`(3) `TEXCOORDSIZE4`(3) `LASTBETA_UBYTE4`(2) `XYZB4`(2)

Two specific traps:

- **`D3DFVF_XYZRHW` (19 uses)** means the position is *already in screen space* and D3D8 skips
  its whole transform pipeline. Vulkan has no such mode: the vertex shader must do the
  screen-space→clip-space conversion by hand. The PoC handles this case explicitly.
- **`D3DFVF_XYZB4` + `LASTBETA_UBYTE4` (2 uses)** is fixed-function skinning with a packed bone
  index. Becomes explicit shader code.

### 1.5 Programmable shaders — ps.1.1 / vs.1.1

| D3D8 method | Sites | Vulkan | Difficulty |
|---|---:|---|---|
| `SetPixelShader` | 22 (1+21) | `vkCmdBindPipeline` (the shader is *in* the pipeline, not bound separately) | wrapper |
| `CreatePixelShader` | 5 (0+5) | `vkCreateShaderModule` — input is ps.1.1 token stream | **hard** |
| `DeletePixelShader` | 15 (0+15) | `vkDestroyShaderModule` + pipeline eviction | wrapper |
| `SetPixelShaderConstant` | 7 (1+6) | push constants / UBO | wrapper |

`SetPixelShader` being separate from the pipeline is an impedance mismatch, not a difficulty:
in Vulkan the shader pair *is* part of the `VkPipeline`, so `SetPixelShader` becomes another key
field in the pipeline cache rather than a bind.

The shaders themselves are the surprise, and it is a pleasant one. `W3DShaderManager` loads
**13 pre-assembled shader binaries** from game data (`shaders\terrain.pso`, `fterrain.pso`,
`terrainnoise.pso`, `terrainnoise2.pso`, `roadnoise2.pso`, `monochrome.pso`, `fterrain0.pso`,
`fterrainnoise.pso`, `fterrainnoise2.pso`, `Trees.pso`, `Trees.vso`, `wave.pso`, `wave.vso`),
plus 4 `D3DXAssembleShader` calls on inline strings (water fallbacks, and the frame profiler).

But **the assembly sources are in the repo** — 16 `.nvp`/`.nvv` files under
`GeneralsMD/.../GameClient/Shaders/` and `Core/.../GameClient/Water/`. Total non-comment content:
**158 lines** across all 16, the largest being `wave.nvv` at 29 lines. No token-stream
disassembler is needed; these are hand-translatable to GLSL in a day. That removes what looked
like the second-largest risk item.

### 1.6 Resources — textures, buffers, surfaces

| D3D8 method | Sites | Vulkan | Difficulty |
|---|---:|---|---|
| `CreateTexture` | 2 (0+2) | `vkCreateImage` + `vkAllocateMemory` + view | wrapper |
| `CreateVertexBuffer` / `CreateIndexBuffer` | 2 (0+2) / 2 (0+2) | `vkCreateBuffer` | wrapper |
| `CreateImageSurface` | 4 (3+1) | a host-visible staging buffer, or a `VK_IMAGE_TILING_LINEAR` image | wrapper |
| `SetTexture` | 70 (5+65) | descriptor set update; **also needs a sampler**, since D3D8 keeps filter state on the stage | wrapper |
| `UpdateTexture` | 5 (5+0) | `vkCmdCopyBufferToImage` from the shadow copy | wrapper |
| `CopyRects` | 3 (2+1) | `vkCmdCopyImage` / `vkCmdBlitImage`, plus explicit layout transitions and a barrier | **hard-ish** |
| `SetRenderTarget` | 7 (5+2) | a different `VkFramebuffer`/render pass. **Cannot happen mid-pass** — this is the real cost | **hard** |
| `GetRenderTarget` / `GetDepthStencilSurface` | 4 (2+2) / 4 (2+2) | backend bookkeeping | wrapper |
| `GetBackBuffer` / `GetFrontBuffer` | 1 (1+0) / 1 (1+0) | swapchain image / readback copy | wrapper |
| `GetAvailableTextureMem` | 1 (0+1) | `VK_EXT_memory_budget`, or return a plausible number | wrapper |
| `ResourceManagerDiscardBytes` | 2 (1+1) | **no equivalent** — D3D8's managed pool evicted on request. A Vulkan backend owns its own residency | drop |

Two things here are more than mechanical:

**Resource pools.** 47 `D3DPOOL_*` uses: `MANAGED`(21), `DEFAULT`(19), `SYSTEMMEM`(7).
`D3DPOOL_MANAGED` means *the driver keeps a system-memory shadow copy and silently re-uploads it
after device loss*. Vulkan has no such contract, so the backend must implement the shadowing
itself — which is where 21 sites' worth of assumed behaviour lives. Likewise the 43 `D3DLOCK_*`
uses (`DISCARD`(23), `NOOVERWRITE`(14), `READONLY`(4), `NOSYSLOCK`(1), `NO_DIRTY_UPDATE`(1)) are
a rename-on-write contract the driver honours; `DISCARD` + `NOOVERWRITE` together are exactly the
dynamic-buffer ring a Vulkan backend has to build by hand.

**`SetRenderTarget` (7 sites).** In D3D8 this is a state change; in Vulkan you cannot change
attachments inside a render pass. Every render-target switch becomes a pass boundary, so the
backend has to defer `vkCmdBeginRenderPass` until the first draw and end the pass on a target
change. That is a real restructure of the frame, and it is the one place where the engine's call
pattern genuinely fights Vulkan's model.

**Palettised textures: not a risk.** The brief flagged `D3DFMT_P8`. It is not used at runtime —
all 10 occurrences are format-conversion tables, debug name strings, or
`WWASSERT(format != D3DFMT_P8)` (4 sites assert it *cannot* happen). Targa colour-mapped images
are expanded on load in `ww3dformat.cpp` before they reach the device. **Cross this off the risk
list.**

Texture formats actually used, 33 distinct over 130 uses: the interesting ones are `A8R8G8B8`,
`X8R8G8B8`, `R5G6B5`, `A1R5G5B5`, `A4R4G4B4`, `L8`, `A8L8`, `A8`, `DXT1..DXT5`, and the
bump-map formats `V8U8`, `L6V5U5`, `X8L8V8U8`. All have Vulkan equivalents. One trap: D3D8's
`A8R8G8B8` is B,G,R,A in memory, so the Vulkan format is `VK_FORMAT_B8G8R8A8_UNORM`, not
`R8G8B8A8`. Getting that wrong swaps red and blue everywhere; the PoC has it right and says so.

Depth formats used: `D16`, `D24S8`, `D24X8`, `D24X4S4`, `D32`, `D15S1`, `D16_LOCKABLE`. `D15S1`
and `D24X4S4` have no Vulkan equivalent and must be promoted — harmless, since they are only
probed for support, but the probe logic (`CheckDepthStencilMatch`) has to be rewritten as a
lookup over `vkGetPhysicalDeviceFormatProperties`.

### 1.7 Device lifecycle, and things that are not rendering at all

| D3D8 method | Sites | Vulkan | Difficulty |
|---|---:|---|---|
| `TestCooperativeLevel` | 7 (0+7) | no equivalent. Vulkan has no "device lost then restore" dance; `VK_ERROR_DEVICE_LOST` is terminal | drop/stub |
| `Reset` | 1 (1+0) | recreate the swapchain | wrapper |
| `CreateAdditionalSwapChain` | 1 (1+0) | a second `VkSwapchainKHR` | wrapper |
| `GetDeviceCaps` | 2 (2+0) | `VkPhysicalDeviceProperties`/`Features` — but the D3D8 caps bits do not correspond, so `dx8caps.cpp` needs rewriting against Vulkan limits | wrapper |
| `GetDisplayMode` | 2 (2+0) | surface capabilities, or the platform layer | drop |
| `SetGammaRamp` | 1 (0+1) | **no equivalent.** Vulkan has no gamma ramp. Becomes a post-process pass or is dropped | **hard** |
| `SetCursorProperties` / `SetCursorPosition` / `ShowCursor` | 2 / 1 / 4 (all direct) | not a renderer concern — belongs to the SDL platform layer (Phase 3) | drop |

`TestCooperativeLevel`'s 7 sites are worth flagging: they encode the D3D8 assumption that the
device can be lost on alt-tab and everything in `D3DPOOL_DEFAULT` must be recreated. On Vulkan
that entire code path is dead weight, but it is load-bearing in the engine's frame loop, so it
has to be stubbed to "always OK" rather than deleted.

### 1.8 `IDirect3D8` — adapter and device enumeration

19 sites, 10 methods, all direct (zero go through `DX8CALL_D3D`), almost all in `dx8caps.cpp`
and `dx8wrapper.cpp` init:

| Method | Sites | Vulkan |
|---|---:|---|
| `EnumAdapterModes` | 3 | `vkGetPhysicalDeviceSurfacePresentModesKHR` + platform display enumeration |
| `GetAdapterIdentifier` | 3 | `VkPhysicalDeviceProperties.deviceName`/`vendorID` |
| `CheckDeviceFormat` | 3 | `vkGetPhysicalDeviceFormatProperties` |
| `CheckDeviceType` | 2 | implicit |
| `CheckDeviceMultiSampleType` | 2 | `VkPhysicalDeviceLimits.framebufferColorSampleCounts` |
| `GetAdapterModeCount` | 2 | display enumeration |
| `CheckDepthStencilMatch` | 1 | format properties lookup |
| `GetAdapterCount` | 1 | `vkEnumeratePhysicalDevices` |
| `GetAdapterDisplayMode` | 1 | platform layer |
| `GetDeviceCaps` | 1 | `VkPhysicalDeviceProperties` |

All mappable; none is hard. But note this is where the engine's device-selection and
capability-tier logic lives (`dx8caps.cpp` drives the graphics LOD system), so it is not a
mechanical translation so much as a re-derivation of the tier heuristics against Vulkan limits.

### 1.9 D3DX

Not part of `IDirect3DDevice8`, but on the same critical path: 24 distinct `D3DX*` entry points.
The maths helpers (`D3DXMatrixInverse` ×20, `D3DXMatrixScaling` ×13, `D3DXMatrixTranslation` ×11,
`D3DXMatrixMultiply` ×6, `D3DXVECTOR4` ×31, …) are trivial to replace — the engine already has
its own `Matrix4x4`/`Vector4`. The ones that need real work:

| Entry point | Uses | Replacement |
|---|---:|---|
| `D3DXAssembleShader` | 4 | assembles ps/vs assembly at runtime. Replace with precompiled SPIR-V |
| `D3DXFilterTexture` | 5 | mipmap chain generation → `vkCmdBlitImage` loop |
| `D3DXCreateTexture` / `D3DXCreateCubeTexture` / `D3DXCreateVolumeTexture` | 4 / 4 / 2 | image creation with format/size fallback logic |
| `D3DXLoadSurfaceFromSurface` | 3 | format-converting blit; needs a CPU converter or a compute shader |
| `D3DXCreateTextureFromFileExA` | 1 | image file loading (DDS) |
| `D3DXGetFVFVertexSize` | 3 | trivial, but only once FVF decoding exists |

## 2. What the proof-of-concept actually does

`spikes/renderer/` — build and run instructions in its README. Standalone; deliberately not
wired into the game or the top-level CMake, so it cannot affect the Windows build.

The abstraction (`src/render_backend.h`) mirrors `DX8Wrapper`'s method names, argument order and
semantics, so the question it answers is the real one: can the *existing call pattern* be served
by Vulkan without changing the call sites?

### Verified working

Run on `llvmpipe (LLVM 15.0.7)` with `VK_LAYER_KHRONOS_validation` enabled, no validation errors:

![PoC output](../../spikes/renderer/docs/spike-triangle.png)

- **A textured triangle**, `D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1`, with the stage-0 cascade
  set to `COLOROP=MODULATE`, `COLORARG1=TEXTURE`, `COLORARG2=DIFFUSE`, `ALPHAOP=SELECTARG2` —
  i.e. the engine's most common material. The checkerboard is tinted by the per-vertex diffuse,
  which is the combiner actually running, not a hardcoded shader.
- **A second draw in the same frame with different state**: a `D3DFVF_XYZRHW | D3DFVF_DIFFUSE`
  screen-space quad, `ALPHABLENDENABLE=1`, `SRCBLEND=SRCALPHA`, `DESTBLEND=INVSRCALPHA`,
  `ZENABLE=0`, `COLOROP=SELECTARG1/DIFFUSE`, no texture bound. This is the shape of the engine's
  UI and shadow passes, including the pretransformed-position path.
- **The pipeline cache doing its job**: two draws with different state produce exactly
  2 `VkPipeline` objects, materialised lazily at draw time from a hash of the shadow state block.
  This is the mechanism that makes D3D8's one-state-at-a-time model work on Vulkan, and it is the
  thing the spike most needed to prove.
- **`Clear` as `vkCmdClearAttachments`** inside the render pass, preserving D3D8's mid-frame
  per-call-flags semantics.
- **Alpha test in-shader** via `discard`, since Vulkan has no alpha test.
- **The texture-stage cascade interpreted from a uniform block** at runtime
  (`shaders/fixedfunc.frag`), covering 14 of the 17 ops the engine uses — `DISABLE`,
  `SELECTARG1`, `SELECTARG2`, `MODULATE`, `MODULATE2X`, `MODULATE4X`, `ADD`, `ADDSIGNED`,
  `ADDSIGNED2X`, `SUBTRACT`, `ADDSMOOTH`, `BLENDTEXTUREALPHA`, `BLENDCURRENTALPHA`,
  `DOTPRODUCT3` — plus the `COMPLEMENT` and `ALPHAREPLICATE` argument modifiers and the
  `TFACTOR`/`CURRENT`/`TEXTURE`/`DIFFUSE` operands. `MULTIPLYADD`, `MODULATEALPHA_ADDCOLOR` and
  `BUMPENVMAP*` are not implemented (they need `COLORARG0` or bump matrices, neither plumbed).
- **FVF decoding** into `VkVertexInputAttributeDescription`s, including the `XYZRHW`
  screen-space path and substituting a constant dummy binding for attributes the FVF omits
  (Vulkan requires every shader input to be fed).
- **Correctness checked by readback, not by eye**: the PoC copies the colour target to host
  memory, asserts the centre pixel is not the clear colour, and writes a PNG. It exits non-zero if
  nothing rasterised.
- **Headless by default**, with an optional SDL2 window (`--window`). Headless is what makes it
  verifiable in CI.

### Explicitly NOT implemented

Stated plainly, because a spike that overstates itself is worthless:

- **Not run on macOS or MoltenVK.** This is the spike's biggest gap. It was written to be
  MoltenVK-compatible (Vulkan 1.1 core only, no extensions beyond swapchain, no features
  MoltenVK lacks) and it has no Linux-specific code, but *that is an argument, not a
  measurement*. Verified only on Linux/llvmpipe.
- Not wired into the game. No engine code was touched.
- No fixed-function **lighting**, **fog**, or **material** support — the states are accepted and
  ignored.
- No `SetRenderTarget` / render-to-texture, no pass restructuring. Single hardcoded render pass.
- No ps.1.1/vs.1.1 translation. None of the 16 engine shaders was ported.
- Only **2 of D3D8's 8 texture stages**. No `TEXTURETRANSFORMFLAGS`, no `D3DTS_TEXTURE0..3`
  matrices, no `BUMPENVMAP`.
- No mipmaps, no DXT/compressed formats, no cube or volume textures, no `CopyRects`,
  no `DrawPrimitiveUP`, no `ProcessVertices`.
- No dynamic vertex buffer ring; buffers are static, host-visible, and allocated one
  `vkAllocateMemory` each. Fine for a spike, unacceptable in the game.
- No `D3DPOOL_MANAGED` shadow-copy emulation, no device-loss handling.
- Hard cap of 64 draws per frame (preallocated descriptor sets).
- Wireframe/point fill modes, multiple streams, and stencil ops are mapped in the tables but
  never exercised.

The honest summary: **the PoC proves the architecture, not the coverage.** It demonstrates that
the two things that could have killed the approach — the mutable-state/immutable-pipeline
mismatch, and the texture-stage cascade having no Vulkan analogue — both have working,
non-exotic solutions. It does not demonstrate that the remaining 90% is cheap.

## 3. Recommendation

**Translate D3D8 → Vulkan behind a retargeted `DX8Wrapper`. Do not use a DXVK-style translation
layer. Do not write a native Metal backend.**

### Why not an existing translation layer

DXVK is the obvious temptation and it is the wrong tool here:

- **DXVK does not implement D3D8.** It covers D3D9/10/11. The D3D8 route is `d3d8to9` (which
  translates D3D8→D3D9 first, then DXVK D3D9→Vulkan) — two translation layers stacked, both
  designed for Windows binaries, neither maintained for this use case.
- Both are **COM/Win32-shaped**: they expect `IUnknown`, `HRESULT`, Windows calling conventions,
  and in DXVK's case a Wine or Windows host. The whole point of this port is *no Wine*. Getting
  d3d8to9 + DXVK running natively on macOS/arm64 means porting two more codebases, and their
  Windows-isms are exactly the thing being eliminated.
- Licensing and provenance: shipping a `.app` that bundles two reverse-engineered API
  reimplementations is a legal review nobody asked for.
- And critically: **82% of the engine's D3D8 calls bypass `DX8Wrapper` already.** A drop-in
  D3D8 implementation would have to be a *complete and correct* D3D8, because the engine reaches
  around the wrapper. Whereas a retargeted wrapper only has to implement the 62 methods the
  engine actually calls, with the semantics it actually relies on. **The measured narrowness of
  the surface is precisely what makes the bespoke route cheaper than the general one.**

### Why not a native Metal backend

MoltenVK is a thin, mature, Khronos-maintained Vulkan-on-Metal layer, and the engine is
fixed-function-era: it will never stress the features MoltenVK lacks (no geometry shaders, no
sparse resources, limited tessellation — none of which the engine uses). Writing Metal by hand
buys marginal performance for a renderer that ran on a GeForce 4, costs a second backend to
maintain, and abandons the Linux half of the target. If MoltenVK ever becomes the bottleneck, a
Metal backend can be added *behind the same abstraction* later — which is an argument for
building the abstraction now, not for skipping it.

### Why Vulkan behind the wrapper does work

The spike answered the two questions that mattered:

1. *Can D3D8's mutable, one-state-at-a-time model be served by immutable `VkPipeline`s?* Yes —
   shadow state block + lazy pipeline materialisation + hash cache. Working code, ~200 lines.
2. *Can `SetTextureStageState` be emulated?* Yes — a uniform-driven uber-shader. Working code,
   ~120 lines of GLSL, and the measured op vocabulary is small enough (5 ops = 92% of uses) that
   this is not a combinatorial trap.

Neither answer required anything exotic. That is the spike's real result.

### Revised Phase 4 estimate: **700–1,300 h** (was 600–1,200 h, revised in the last doc to 500–900 h)

The 500–900 h figure was based on "34 methods behind one wrapper". Both halves of that were
wrong, so it has to go up. But two risks came off the list, so not by much.

| Work item | Hours | Reasoning |
|---|---:|---|
| Backend skeleton: instance/device/swapchain, memory allocator, command/sync, resource lifetime | 60–100 | the spike is a third of this, but needs a real allocator, dynamic buffer rings, and device-loss paths |
| Pipeline cache + render-state translation (group (a), 20 states) | 40–70 | mechanical, proven by the spike |
| Texture-stage cascade uber-shader, all 8 stages, 17 ops, `TEXTURETRANSFORMFLAGS` + texcoord matrices | 60–100 | spike covers 2 stages and 14 ops; the rest is more of the same plus `BUMPENVMAP` |
| Fixed-function lighting, fog, material, point sprites as shader code | 80–140 | 30+ render-state sites, no reference implementation to copy, and getting it *visually identical* is the expensive part |
| FVF decoding, all 20 bits incl. `XYZB4`/`LASTBETA_UBYTE4` skinning, `TEXCOORDSIZE*` | 40–70 | spike covers the common cases |
| Texture/surface layer: formats, DXT, mipmaps, cube/volume, `CopyRects`, `D3DPOOL_MANAGED` shadowing, lock semantics | 80–140 | the `MANAGED`/`DISCARD`/`NOOVERWRITE` contracts are invisible assumptions across 90 sites |
| Render-target restructure: `SetRenderTarget` → pass boundaries, deferred pass begin, RTT paths | 60–110 | the one place the engine's model genuinely fights Vulkan's |
| Port the 16 ps.1.1/vs.1.1 shaders to GLSL/SPIR-V | 25–45 | **down from the original guess**: 158 lines of assembly total, sources in-repo, no disassembler needed |
| `dx8caps.cpp` rewrite: capability tiers and device selection against Vulkan limits | 40–70 | drives the graphics LOD system, so it is behaviour, not just plumbing |
| D3DX replacement (maths trivial; `FilterTexture`, `LoadSurfaceFromSurface`, texture creation) | 30–50 | |
| **Rework the 376 direct call sites** in W3DWater, W3DShaderManager, the two shadow systems, W3DTreeBuffer, shader.cpp to route through the wrapper | 120–220 | **this is the line item the previous estimate missed entirely** |
| `DrawPrimitiveUP` (11), `ProcessVertices` (2), `SetClipPlane`, `SetGammaRamp`, `ZBIAS` tuning | 50–90 | each needs a per-site decision; `ZBIAS` in particular has no correct answer, only a tuned one |
| Visual parity debugging against the Windows build | 100–200 | uncapped in practice. Everything above can be "done" and the game still looks wrong |
| **Total** | **785–1,405** | round to **700–1,300 h** |

Reasoning for the direction of the change, stated plainly:

- **Up**, because the "one wrapper" premise was false. 376 call sites bypass it, concentrated in
  four files that amount to a second renderer. That is the single biggest correction in this doc.
- **Up**, because fixed-function lighting/fog/material emulation was not costed before and has no
  reference implementation to copy — only a Windows binary to compare screenshots against.
- **Down**, because palettised textures are a non-issue (asserted against, never used at
  runtime).
- **Down**, because the shader port is 158 lines of in-repo assembly, not an opaque token-stream
  reverse-engineering exercise.
- **Unchanged in character**: the two architectural risks that could have made this a rewrite
  rather than a port — immutable pipelines and the texture-stage cascade — are both solved, with
  working code, at a total of ~320 lines. The approach is sound.

### What to do next, in order

1. **Run the PoC on macOS/arm64 through MoltenVK.** This is the one unverified assumption in the
   whole spike and it is a few hours of work. Until it is done, "and therefore MoltenVK" is an
   inference, not a result.
2. ~~**Route the 376 direct call sites through `DX8Wrapper` while still on D3D8**~~ — **done**,
   see [0.5](#05-post-refactor-state-after-the-dx8wrapper-consolidation). 376 direct sites → 0,
   verified by the Windows build. The renderer is now the "one wrapper" the plan assumed, so the
   120–220 h line item in the estimate above is spent and the remaining Phase 4 range should be
   read as **580–1,080 h**.
3. Only then build the backend for real, in the order of the table above.
