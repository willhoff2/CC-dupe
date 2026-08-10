# The fixed-function subset Zero Hour actually needs, measured

Companion to `docs/porting/renderer-surface.md`. That document measured the *interface* — 63
D3D8 methods, who calls them, and how each maps to Vulkan. This one measures the *semantics*
behind the two methods that carry almost all of the risk, `SetTextureStageState` and
`SetVertexShader(FVF)`, and records what the spike now implements against that measurement.

Everything here is produced by two scanners in the repo and every number can be re-derived:

```sh
python3 spikes/renderer/tools/texture-stage-scan.py   # section 1
python3 spikes/renderer/tools/engine-usage-scan.py    # sections 2-7
```

Both scan `Core/`, `Generals/` and `GeneralsMD/` (2,872 files). They are deliberately literal:
they report only what they can resolve at compile time and list every call site whose stage or
value is an expression, so each number below is a **lower bound with its own uncertainty
attached**, not an estimate.

---

## 1. The texture-stage cascade

D3D8's cascade is the part of the API with no Vulkan analogue at all: up to 8 stages, each with
a colour and an alpha combiner chosen from 26 ops over 6 argument sources with 2 modifiers, all
mutable one state at a time. The question is not "can it be emulated" (it can — an uber-shader
reading a uniform block) but **how big the vocabulary the engine can actually request is**,
because that decides whether the shader is 100 lines or a combinatorial trap.

Measured, the engine reaches the cascade two ways, and both have to be counted:

**(a) Literal call sites.** 77 direct/macro `SetTextureStageState` sites with constant
arguments, folded into per-stage snapshots:

| | distinct combiners | from raw tuples |
|---|---:|---:|
| colour | 18 | 22 |
| alpha | 12 | 17 |

**(b) `ShaderClass::Apply()`.** `shader.cpp` translates a packed 32-bit shader word into stage
state at runtime, so its call sites are not literal. Enumerating the fields that reach the
cascade — `Texturing`(2) × `PriGradient`(6) × `DetailColorFunc`(13) × `DetailAlphaFunc`(4) =
**624 reachable shader states** — and running the translation gives:

| | distinct combiners |
|---|---:|
| colour | 20 |
| alpha | 7 |

**Union — what a backend must implement:**

```
colour combiners                  32
alpha combiners                   14
both channels                     46   (34 ignoring the colour/alpha split)
distinct D3DTOP_* ops             17   of D3D8's 26
distinct argument encodings        8   of D3D8's 6 sources x 4 modifier combinations
highest stage index written        7
distinct stage0+stage1 programs   314
```

The 17 ops:

```
DISABLE  SELECTARG1  SELECTARG2  MODULATE  MODULATE2X  ADD  ADDSIGNED  ADDSIGNED2X
SUBTRACT  ADDSMOOTH  BLENDTEXTUREALPHA  BLENDCURRENTALPHA  MODULATEALPHA_ADDCOLOR
BUMPENVMAP  BUMPENVMAPLUMINANCE  DOTPRODUCT3  MULTIPLYADD
```

The 8 argument encodings:

```
DIFFUSE  CURRENT  TEXTURE  TFACTOR
TFACTOR|COMPLEMENT  DIFFUSE|ALPHAREPLICATE  TFACTOR|ALPHAREPLICATE
DIFFUSE|COMPLEMENT|ALPHAREPLICATE
```

Literal call sites per stage — the reason "two stages" is not enough:

| stage | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| snapshots | 60 | 67 | 22 | 8 | 6 | 4 | 4 | 4 |

Other stage state, by value:

- `TEXCOORDINDEX`: `TCI_PASSTHRU` ×33, `TCI_CAMERASPACEPOSITION` ×31, explicit index 1 ×21,
  `TCI_CAMERASPACENORMAL` ×11, `TCI_CAMERASPACEREFLECTIONVECTOR` ×9, indices 2–7 ×20.
  So both the "which UV set" and all three camera-space generators are live.
- `TEXTURETRANSFORMFLAGS`: `D3DTTFF_COUNT2` ×56, `D3DTTFF_DISABLE` ×38, `259` (=
  `COUNT3|PROJECTED`) ×4.
- `BUMPENVMAT00/01/10/11`, `BUMPENVLSCALE`, `BUMPENVLOFFSET`: written, with a rotation matrix
  (`c`/`-s`/`s`/`c`) — the bump ops are not vestigial.
- `MINFILTER`/`MAGFILTER`/`MIPFILTER`: `LINEAR`/`POINT`/`NONE` only. `MAXANISOTROPY` at one site.
- `ADDRESSU/V`: `WRAP` ×26, `CLAMP` ×22. `ADDRESSW` `CLAMP` ×2.

### Negative findings, stated rather than worked around

- **9 of D3D8's 26 ops are dead** — no call site, literal or generated, can request
  `MODULATE4X`, `BLENDDIFFUSEALPHA`, `BLENDFACTORALPHA`, `BLENDTEXTUREALPHAPM`, `PREMODULATE`,
  `MODULATECOLOR_ADDALPHA`, `MODULATEINVALPHA_ADDCOLOR`, `MODULATEINVCOLOR_ADDALPHA` or `LERP`.
  (The spike implements `LERP` anyway: it costs one `mix()`.)
- **`D3DTSS_RESULTARG` is written by exactly one engine site, and that site is commented out.**
  The spike implements it (D3D8 defaults it to `CURRENT`, and getting the default wrong silently
  corrupts every cascade) but no shipped code path exercises it. Implemented, not needed.
- The scanner's fourth tuple component is printed as `arg0`; that is D3D8's `COLORARG0` /
  `ALPHAARG0`, the third input `MULTIPLYADD` and `LERP` need.
- 30 call sites compute their stage index (`stage`, `i`, `m_stageOfSet`). They are listed
  individually by the scanner rather than guessed at; they are why the per-stage table is a
  lower bound.

---

## 2. Vertex declarations (FVF)

**21 distinct literal declarations**, of which 13 are the named `DX8_FVF_*` layouts in
`dx8fvf.h`:

```
XYZ                                        XYZRHW
XYZ | NORMAL                               XYZRHW | DIFFUSE
XYZ | DIFFUSE                              XYZRHW | TEX1
XYZ | TEX1                                 XYZRHW | DIFFUSE | TEX1
XYZ | NORMAL | TEX1                        XYZRHW | TEX2
XYZ | NORMAL | DIFFUSE                     XYZRHW | DIFFUSE | TEX2
XYZ | DIFFUSE | TEX1
XYZ | NORMAL | DIFFUSE | TEX1              XYZ | NORMAL | TEX3(uv1,uv4,uv2)
XYZ | TEX2                                 XYZ | NORMAL | DIFFUSE | TEX4(uv2,uv3,uv3,uv3)
XYZ | NORMAL | TEX2                        XYZB4
XYZ | DIFFUSE | TEX2
XYZ | NORMAL | DIFFUSE | TEX2
```

Consequences for the vertex-input decoder: **4 texture-coordinate sets maximum**, coordinate
**sizes 1, 2, 3 and 4** all occur (`DX8_FVF_XYZNUV2DMAP` is uv1/uv4/uv2), normals are used, the
pretransformed `XYZRHW` path is used by 8 declarations, and blend weights appear in one
(`D3DFVF_XYZB4 | D3DFVF_LASTBETA_UBYTE4`).

**Negative finding on `XYZB4`:** the spike decodes it and accepts vertices in that layout, and
the pixel test asserts that a `XYZB4|LASTBETA_UBYTE4` quad rasterises. That is *declaration
compatibility*, not skinning: no bone-matrix palette is uploaded and no blending is performed,
because D3D8 indexed vertex blending also needs `D3DRS_VERTEXBLEND` and
`D3DTS_WORLDMATRIX(n)`, and the engine's use of them is not measured here. Do not read the
passing test as "skinning works".

---

## 3. Transforms, lighting, fog

| | measured |
|---|---|
| transforms | `WORLD` (90 refs), `VIEW` (67), `PROJECTION` (24), `TEXTURE0..3` (52/8/5/1) |
| light types | `DIRECTIONAL`, `POINT`, `SPOT` — all three, 14 `SetLight` sites, 2 `LightEnable` |
| material sources | `DIFFUSEMATERIALSOURCE` ∈ {`COLOR1`, `MATERIAL`, dynamic}, `AMBIENT`/`EMISSIVE` `MATERIAL`, `SPECULAR` `COLOR2` |
| `NORMALIZENORMALS` | both `TRUE` and `FALSE` |
| fog | `FOGVERTEXMODE = D3DFOG_LINEAR`, `FOGTABLEMODE = D3DFOG_NONE`, colour/start/end written, `RANGEFOGENABLE` gated on `D3DPRASTERCAPS_FOGRANGE` |

So: **vertex fog, linear, is the only mode the engine turns on**. Table fog is explicitly
disabled at the one site that sets it. The spike implements both (`EXP`/`EXP2` included) because
the cost is four lines of shader, but only the linear-vertex path is a measured requirement.

---

## 4. Alpha test, depth bias, scissor

- **Alpha test** is live: `NOTEQUAL` ×5, `LESSEQUAL` ×5, `GREATEREQUAL` ×4, with refs
  `0x00`, `0x60`, `0x7B`, `0x80`, `0x84`, `0xff` and two computed. Vulkan has no alpha test, so
  the spike does it as a `discard` in the fragment shader.
- **`D3DRS_ZBIAS`**: the engine writes `0` (14 sites), `7` (8), `8` (2), plus `1` and `4` in
  the wider source scan. D3D8 never defined a unit for ZBIAS — "closer to the eye", magnitude
  driver-defined — so there is no correct mapping, only a defensible one. The spike uses
  `depthBiasConstantFactor = -zbias`, i.e. one ZBIAS unit = one *r* (the smallest resolvable
  depth difference for the attachment format), which is the smallest bias that is guaranteed to
  change a comparison and cannot silently do nothing. The pixel test asserts the ordering
  behaviour (ZBIAS 0 hides a coplanar polygon, ZBIAS 7 shows it), not a magnitude, because the
  magnitude is not knowable. **Visual parity with the Windows build will need this re-tuned
  against screenshots.**
- **Scissor: 0 engine call sites.** The engine never scissors. The spike implements
  `Set_Scissor` anyway — it is one `vkCmdSetScissor` and the stencil test needs it to write a
  sub-rectangle — but it is not a port requirement.

---

## 5. Texture formats

The engine's own `WW3DFormat` enum can name 25 formats:

```
UNKNOWN R8G8B8 A8R8G8B8 X8R8G8B8 R5G6B5 X1R5G5B5 A1R5G5B5 A4R4G4B4 R3G3B2 A8
A8R3G3B2 X4R4G4B4 A8P8 P8 L8 A8L8 A4L4 U8V8 L6V5U5 X8L8V8U8 DXT1 DXT2 DXT3 DXT4 DXT5
```

The spike uploads and pixel-asserts: `A8R8G8B8`, `X8R8G8B8`, `R8G8B8`, `A4R4G4B4`, `A1R5G5B5`,
`R5G6B5`, `L8`, `A8L8`, `V8U8`, `P8`, `A8` (alpha only — see below), and DXT1–DXT5 through
`zh-feature-probe`.

Findings worth more than a workaround:

- **`A4R4G4B4` is sampleable but not renderable/blendable on MoltenVK**
  (`docs/porting/moltenvk-findings.md`). Textures are only ever sampled, so this is fine — but
  it forecloses using it as a render target on macOS.
- **`P8`/`A8P8` are palettised and Vulkan has no palette.** The spike expands them on the CPU at
  upload, which is what the backend will have to do; the engine's loader asserts against
  palettised textures at runtime anyway.
- **`A8`'s colour channels are pending, not asserted.** D3D8 does not define what RGB an
  alpha-only texture samples as. The backend produces `(0,0,0,A)`, matching D3D9/DXGI, but that
  is our choice, not a D3D8 fact, so the test asserts alpha and marks the colour **PENDING**
  rather than freezing our own behaviour into a golden value.
- **MoltenVK has no image-view component swizzle** (`imageViewFormatSwizzle` is `VK_FALSE` in
  `VkPhysicalDevicePortabilitySubsetFeaturesKHR`; Metal has no equivalent). `L8`, `A8`, `A8L8`
  and `X8R8G8B8` were being reproduced with a view swizzle, which is a validation error there.
  The backend now reads the portability feature and falls back to expanding those four to
  B8G8R8A8 on the CPU at upload. Linux CI runs the tests a second time with
  `ZH_SPIKE_NO_VIEW_SWIZZLE=1` so the expansion path is asserted against the swizzled result
  rather than only on the Mac.
- **MoltenVK forbids a vertex binding stride of zero** (`vertexAttributeAccessBeyondStride`).
  The spike feeds shader inputs an FVF omits from a one-element constant buffer, which the
  obvious way expresses as stride 0. It now binds that buffer at `VK_VERTEX_INPUT_RATE_INSTANCE`
  with a real stride instead; every vertex of the single instance still reads element 0. Any
  backend using the same trick for absent FVF components will hit this.
- **`D3DFMT_D24S8` does not exist on MoltenVK.** The backend now picks
  `D24_UNORM_S8_UINT` if the device has it and falls back to `D32_SFLOAT_S8_UINT` otherwise;
  before that it used `D32_SFLOAT`, which has no stencil at all and made the stencil path
  silently unimplementable.

---

## 6. Pipeline-cache pressure — the number that decides whether MoltenVK hurts

Vulkan bakes state into immutable pipelines; D3D8 mutates one state at a time. From the
`ShaderClass` field cardinalities in `shader.h`:

```
DepthCompare(8) x DepthMask(2) x ColorMask(2) x DstBlendFunc(6) x SrcBlendFunc(4) x CullMode(2)
  = 1,536 pipeline-relevant state combinations
  x 21 vertex declarations
  = 32,256 VkPipeline objects            <- upper bound, cascade interpreted in the shader
```

If the cascade were **compiled into shader permutations** instead of interpreted, multiply by
`FogFunc(4) × PriGradient(6) × SecGradient(2) × Texturing(2) × DetailColorFunc(13) ×
DetailAlphaFunc(4)` = 4,992:

```
  = 161,021,952 pipelines                <- why the uber-shader is not a stylistic choice
```

The cross product is an upper bound, and the realistic figure is far lower: the engine's own
presets (`#define SC_*` in `shader.cpp`) are **22 shaders using 12 distinct pipeline-relevant
state combinations**, so a frame that only uses presets needs **12 × 21 = 252 pipelines**.

Measured cost, Linux/lavapipe (`zh-throughput`, 200 frames × 64 draws, 64×64 target, validation
off):

```
draw calls            1,391 /sec   (719 us each; llvmpipe is a CPU rasteriser, this is not a GPU number)
state changes         4,113 /sec   (3 per draw, all pipeline-cache hits)
pipeline creation     59.33 ms each
                      1,914 s      to warm all 32,256 pipelines at that rate
```

**Read the pipeline-creation number, not the draw-call number.** 59 ms is llvmpipe compiling
SPIR-V on a CPU and will be far smaller on a real driver, but the shape of the problem is the
point: at 252 pipelines a preset-only frame costs ~15 s of warm-up at this rate, and the
32,256-pipeline upper bound is not warmable at startup on any driver. The mitigations are
therefore not optional: a serialised `VkPipelineCache` shipped with the build, plus
`vkCmdSetScissor`/`vkCmdSetDepthBias`-style dynamic state for everything that does not have to
be baked (the spike already keeps scissor, depth-bias amount and the whole cascade out of the
key — the cascade alone is the 4,992× factor). **This needs re-measuring on the M1 Pro before
Phase 4 commits to a warm-up strategy**; MoltenVK's compile is Metal's, not llvmpipe's, and
MoltenVK additionally converts SPIR-V to MSL first.

---

## 7. What the spike now implements, and what it does not

Implemented in `spikes/renderer/`, each with a pixel assertion in `zh-fixedfunc-tests`
(50 assertions; 0 failures; **0 validation-layer messages**; 6 marked PENDING):

| Area | Implemented |
|---|---|
| cascade | all 17 measured ops, all 8 argument encodings, `COMPLEMENT`/`ALPHAREPLICATE`, `COLORARG0`/`ALPHAARG0`, `RESULTARG`, 8 stages |
| cascade | `BUMPENVMAP`/`BUMPENVMAPLUMINANCE` with the 2×2 matrix, scale and offset |
| texcoords | 4 sets, sizes 1–4, `TEXCOORDINDEX` selection, `D3DTS_TEXTURE0..3` matrices, `D3DTTFF_COUNT*` |
| vertex | all 21 FVF declarations incl. `XYZRHW` and `XYZB4|LASTBETA_UBYTE4` (declaration only) |
| vertex | world/view/projection, `NORMALIZENORMALS`, material colour sources |
| lighting | ambient + directional + point (with the 1/(a0+a1·d+a2·d²) attenuation) + spot (theta/phi/falloff) |
| fog | vertex `LINEAR`, and table `LINEAR`/`EXP`/`EXP2` |
| raster | alpha test (all 8 `D3DCMP_*` compares, as a `discard`), `ZBIAS`, scissor, full stencil (func/ref/masks/fail/zfail/pass) |
| formats | A8R8G8B8, X8R8G8B8, R8G8B8, A4R4G4B4, A1R5G5B5, R5G6B5, L8, A8L8, A8, V8U8, P8, DXT1–5 |

Deliberately **not** implemented, and none of it should be assumed:

- vertex blending / bone palettes (`D3DRS_VERTEXBLEND`, `D3DTS_WORLDMATRIX(n)`) — the FVF is
  accepted, the transform is not applied;
- specular highlight computation from the light model (the `SPECULAR` FVF component is passed
  through, `D3DRS_SPECULARENABLE` gates its addition, but no per-light specular term is computed);
- ps.1.1/vs.1.1 shader translation (16 shaders, 158 lines of assembly — costed in
  `renderer-surface.md`, untouched here);
- cube and volume textures, `CopyRects`, `D3DPOOL_MANAGED` shadow copies, device loss;
- `DrawPrimitiveUP`, `ProcessVertices`, `SetClipPlane`, `SetGammaRamp`, point sprites;
- the 64-draws-per-frame cap in the spike's frame allocator is still there.

### Effect on the Phase 4 estimate

Three line items in `renderer-surface.md` §"Revised Phase 4 estimate" are now evidenced rather
than guessed:

| Line item | Was | Now | Why |
|---|---|---|---|
| Texture-stage cascade uber-shader | 60–100 h | **20–35 h** | done for all 17 ops and 8 stages in the spike; what remains is wiring, `D3DTTFF_PROJECTED`, and the 30 computed-stage call sites |
| Fixed-function lighting, fog, material | 80–140 h | **40–90 h** | directional/point/spot/ambient/fog implemented and pixel-asserted; the remaining spread is specular and visual parity, which is where the uncertainty always was |
| FVF decoding | 40–70 h | **15–30 h** | all 21 declarations decode; vertex blending is the only unimplemented piece |

That is roughly **95–155 h** off the 580–1,080 h range, i.e. **≈480–950 h remaining**, with the
same caveat as before: the last line item ("visual parity debugging against the Windows build")
is uncapped in practice and nothing here reduces it. What this work does reduce is the *risk*
that the cascade or the pipeline explosion makes the approach unworkable — both are now measured
numbers with working code behind them rather than open questions.
