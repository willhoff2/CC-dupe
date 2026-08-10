# MoltenVK on Apple Silicon — measured

Everything below was run on real hardware. Where something is inference rather than measurement it
says so. The three programs that produced these numbers are in `spikes/renderer/`:
`zh-renderer-spike` (the existing two-draw PoC), `zh-caps-probe` (queries), and `zh-feature-probe`
(nine rendered cases, each checked by reading pixels back).

`docs/porting/review-and-decisions.md` §2.1 predicted the spike would fail at device enumeration on
macOS, and risk #2 called MoltenVK "unverified, possibly fatal to option A". Both are now settled.

## Verdict up front

| Question | Answer |
|---|---|
| Did the unmodified spike run on MoltenVK? | **No.** `vkCreateInstance` → `VK_ERROR_INCOMPATIBLE_DRIVER` (-9). The review's prediction was exactly right. |
| Does the ~20-line portability fix settle it? | **Yes.** The spike now renders on `Apple M1 Pro`, 2 pipelines for 2 draws, **0 validation messages**, and the readback is pixel-identical (±1/255) to the Linux/llvmpipe reference. |
| Do Apple GPUs lack BC/DXT, as feared? | **No — the fear is wrong on Apple Silicon Macs.** `textureCompressionBC = yes`; BC1/BC2/BC3 sample correctly, including a mip chain and BC3 alpha. See below, this is the biggest single finding. |
| Is option A (hand-written Vulkan behind `DX8Wrapper`) viable on Apple Silicon? | **Yes.** Nothing measured invalidates it. Every engine-critical capability probed works, with two format-level caveats that cost hours, not months. |

## 1. The machine

| | |
|---|---|
| Chip | Apple M1 Pro (`arm64`), integrated GPU, `vendorID 0x106b` |
| macOS | 26.6.1 (build 25G76) |
| MoltenVK | 1.4.2 (Homebrew `molten-vk`), driverVersion 0.2.2210, conformance 1.4.4.0 |
| Vulkan loader | Khronos 1.4.357.0 (Homebrew `vulkan-loader`), instance version 1.4.357 |
| Device API version | 1.1.357 — MoltenVK reports Vulkan **1.1**, which is what the spike targets |
| Validation layers | `VK_LAYER_KHRONOS_validation` 1.4.357, present and **active for every run below** |
| Shader compiler | `glslangValidator` (Homebrew `glslang` 16.5.0) |
| Metal-level check | `MTLDevice.supportsBCTextureCompression = true`, families `apple7`/`mac2`/`metal3` |

Install: `brew install cmake molten-vk vulkan-headers vulkan-loader vulkan-tools glslang
vulkan-validationlayers`. Two macOS-specific gotchas, both environmental, neither a code problem:

- Homebrew's SDL2 headers are x86-only in this install and break the build on arm64
  (`immintrin.h`); configure with `-DSPIKE_USE_SDL2=OFF`. Headless is the default path anyway.
- The Homebrew validation layer's manifest names its dylib relatively, so the loader cannot find
  it unless `DYLD_LIBRARY_PATH=/opt/homebrew/lib` is set. Without it the run silently proceeds
  **without validation**; with it, validation is genuinely on. Every "0 validation messages" below
  was produced with it set.

## 2. Baseline, before any change

```
$ cmake -S spikes/renderer -B build-spike-baseline -DSPIKE_USE_SDL2=OFF && cmake --build build-spike-baseline
$ ./build-spike-baseline/zh-renderer-spike --out baseline.png
spikes/renderer/src/vulkan_backend.cpp:343: vkCreateInstance(&ci, nullptr, &instance_) failed with VkResult -9
backend Init failed
```

`-9` is `VK_ERROR_INCOMPATIBLE_DRIVER`. Same result with `--no-validation`, so it is not a layer
problem. It fails at `vkCreateInstance`, one step earlier than §2.1 guessed ("will not report it at
`vkEnumeratePhysicalDevices`") — with loader 1.4.357 the loader rejects instance creation outright
when the only ICD is a portability driver and the instance did not opt in.

For contrast, `vulkaninfo` from the same Homebrew loader enumerated the M1 Pro fine: the driver and
the ICD manifest were installed correctly all along. The blocker was the spike's own instance
creation, exactly as the review said.

## 3. The fix

In `spikes/renderer/src/vulkan_backend.cpp`, both halves, each conditional on the runtime
advertising the extension so the Linux path is byte-for-byte unchanged:

```cpp
if (Instance_Extension_Available(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
    extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    instance_flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
}
...
if (Device_Extension_Available(physical_, "VK_KHR_portability_subset")) {
    device_extensions.push_back("VK_KHR_portability_subset");
}
```

No `#ifdef __APPLE__`, no other change to the backend. Result:

```
$ ./build-spike/zh-renderer-spike --out macos-fixed.png
device: Apple M1 Pro
wrote macos-fixed.png (800x600), 2 VkPipeline(s) created for 2 draws
centre pixel rgba = 131,152,170,255
validation messages: 0
OK
```

**Not just an exit code.** Compared against the committed Linux/llvmpipe reference
`spikes/renderer/docs/spike-triangle.png`, pixel by pixel over all 480,000 pixels:

| | |
|---|---|
| max absolute per-channel difference | **1 / 255** |
| pixels differing by more than 1 | **0** |
| mean absolute difference | 0.0102 |

The centre pixel is `131,152,170` on MoltenVK versus `132,151,170` on llvmpipe — one LSB of
interpolation rounding. The textured triangle, the `D3DFVF_XYZRHW` screen-space quad, the alpha
blend, the `MODULATE` cascade and the sampler state all produce the same image on Metal as on the
CPU rasteriser. The macOS output is committed as `spikes/renderer/docs/spike-triangle-macos.png`.

## 4. Past the proof-of-concept

Risk #1 in the review is that two draws prove very little. `zh-feature-probe` renders nine cases
and asserts the resulting pixels; a case can only pass if the GPU produced the right colour.

```
device: Apple M1 Pro
depth-stencil format chosen: D32_SFLOAT_S8_UINT

  depth test (D3DRS_ZENABLE/ZFUNC)           PASS near-left=(0,0,255) far-right=(255,0,0)
  alpha blend (SRCALPHA/INVSRCALPHA)         PASS blended=(255,128,128) expected=(255,128,128)
  BC1 / D3DFMT_DXT1 sampled                  PASS sampled=(255,0,0,255) expected=(255,0,0,255)
  BC3 / D3DFMT_DXT5 sampled (incl. alpha)    PASS sampled=(0,255,0,128) expected=(0,255,0,128)
  BC1 mip chain (explicit LOD)               PASS sampled=(0,0,255,255) expected=(0,0,255,255)
  two texture stages in one draw             PASS modulated=(128,0,0) expected=(128,0,0)
  stencil write + test (shadow volumes)      PASS stencil==1=(0,255,255) stencil==0=(0,0,0)
  render target + sample the result          PASS sampled-from-RT=(255,0,255) expected=(255,0,255)
  dynamic buffer suballocation               PASS suballoc0=(0,255,0) suballoc1=(255,255,0)

validation messages: 0
0 case(s) failed
```

What each case is standing in for, and why it was chosen ahead of the rest:

| Case | Engine dependency it de-risks |
|---|---|
| depth test | `D3DRS_ZENABLE/ZFUNC/ZWRITEENABLE`, group (a) of the 370 render-state sites |
| alpha blend | `ALPHABLENDENABLE/SRCBLEND/DESTBLEND` — 99 sites, all of water and the shadow compositing |
| BC1 / BC3 / BC mips | the shipped asset format. Would have been fatal if absent — see §5 |
| two texture stages | the detail/cloud/shroud passes; `D3DTSS_*` stage 1 sampled in one draw |
| stencil | `W3DVolumetricShadow`'s entire method (the eight `D3DRS_STENCIL*` states, 75 sites) |
| render target | `SetRenderTarget`, 7 sites, called "the real cost" in `renderer-surface.md` §1.6 |
| dynamic buffers | the replacement for 11 `DrawPrimitiveUP` sites: one host-visible buffer, two draws at different offsets in one pass |

Zero validation messages across all nine, with the layer verified active.

## 5. The texture-format question — the fear is unfounded on Apple Silicon Macs

The concern was that Apple GPUs do not support BC/DXT and that the game's DXT assets would
therefore need runtime transcoding. **On this machine that is false, and it is false at the Metal
level, not merely papered over by MoltenVK.**

| Evidence | Result |
|---|---|
| `VkPhysicalDeviceFeatures.textureCompressionBC` | `yes` |
| `BC1_RGBA_UNORM_BLOCK` optimal-tiling features | `sampled + linear + blit-src` |
| `BC2_UNORM_BLOCK`, `BC3_UNORM_BLOCK` | `sampled + linear + blit-src` |
| Rendered BC1 texture (solid-endpoint block, exact expected value) | decoded correctly, `(255,0,0,255)` |
| Rendered BC3 texture including its alpha block | decoded correctly, `(0,255,0,128)` |
| Rendered BC1 **mip chain**, sampled at explicit LOD 1 | correct, so a `.dds`-shaped upload works |
| `MTLDevice.supportsBCTextureCompression` (Metal, bypassing Vulkan entirely) | `true` |

Scope of the claim, stated precisely: this is measured on an M1 Pro (`MTLGPUFamily.apple7`,
`mac2`) running macOS 26.6.1. BC support on Apple Silicon is a **macOS** capability — the same GPU
families on iOS/iPadOS do not expose BC — so this result covers the Mac target and says nothing
about a hypothetical iOS build. It should be re-checked on an M-series machine of a different
generation before being treated as universal, but there is no reason from the Metal family flags to
expect it to differ.

Consequence for the plan: **no DXT transcode step, no asset repacking, no ASTC/ETC2 conversion
pipeline.** The engine's `.dds`/DXT loading path can upload block data straight through
`vkCmdCopyBufferToImage`. ASTC and ETC2 are also supported if a transcode is ever wanted for size
reasons, but nothing forces it.

## 6. What actually does not work, or costs something

Small, and none of it structural.

**`D3DFMT_D24S8` does not exist.** The engine's preferred depth-stencil format reports *zero*
optimal-tiling features under MoltenVK, as does `D3DFMT_D24X8` (`X8_D24_UNORM_PACK32`). Metal on
Apple Silicon has no 24-bit depth. The fallback is `D32_SFLOAT_S8_UINT`, which works (the stencil
case above ran on it) and is strictly more precise, at 8 bytes per pixel instead of 4. The backend
must pick the format rather than hardcode it, and any code that assumes a 24-bit depth range or
reads depth back bit-exactly needs checking. The 13 `D3DFMT_D24S8` mentions in the engine are the
ones to audit.

**`D3DFMT_A4R4G4B4` cannot be a render target.** `B4G4R4A4_UNORM_PACK16` is `sampled + linear +
blit-src` only — no `colour-rt`, no blend. Fine as a texture format, not as a surface.

**Portability-subset features that are absent** (`VK_KHR_portability_subset`, advertised):
`pointPolygons` (`D3DFILL_POINT` is unavailable; the engine's 15 `D3DRS_FILLMODE` sites need
checking for it, but solid and wireframe are both fine), `samplerMipLodBias` (**`D3DTSS_MIPMAPLODBIAS`
cannot be set on the sampler — it has to move into the shader as an explicit-LOD sample**),
`tessellationIsolines`, `tessellationPointMode` (both irrelevant to a D3D8-era engine).

**Core features absent:** `depthBounds` (unused by the engine), `wideLines` (line width is fixed at
1.0; the debug-line rendering will look thinner, cosmetic only).

Everything else the engine needs is present: `independentBlend`, `dualSrcBlend`, `fillModeNonSolid`,
`depthClamp`, `depthBiasClamp`, `samplerAnisotropy` (max 16), `largePoints`, `shaderClipDistance`,
`occlusionQueryPrecise`, `triangleFans` (`D3DPT_TRIANGLEFAN` maps directly, which is not guaranteed
on portability drivers), `vertexAttributeAccessBeyondStride`, `mutableComparisonSamplers`,
`imageViewFormatSwizzle` (so the `L8`/`A8` → `R8_UNORM` swizzle fixups are available).

Limits are generous: `maxImageDimension2D` 16384, 8 bound descriptor sets, 16 samplers per stage
(the engine uses at most 8 texture stages), 31 vertex attributes, 4 KB push constants.

## 7. What is still unmeasured

Listed so the verdict is not read as broader than it is. None of this is *known* to be a problem;
it is simply not yet run:

- fixed-function lighting, fog and materials emulated in shaders (the largest uncosted item in
  Phase 4, and independent of MoltenVK)
- texture stages 3–8, texture transform matrices, `BUMPENVMAP`/`BUMPENVMAPLUMINANCE`
- cube and volume textures, `CopyRects`, `ProcessVertices`
- the ps.1.1/vs.1.1 translation of the 16 `.nvp`/`.nvv` sources
- `D3DPOOL_MANAGED` shadow copies and device-loss handling
- presentation: everything here is headless offscreen rendering. `VK_EXT_metal_surface` is
  advertised and the swapchain path is standard MoltenVK, but no window was opened on this machine
  (Homebrew's SDL2 is x86-only here)
- sustained performance. Nothing was benchmarked; MoltenVK's pipeline-compile cost and the
  SPIR-V→MSL conversion at first use are a known first-frame-hitch source and are unmeasured
- anything on an M2/M3/M4 or an Intel Mac

## 8. Verdict and recommendation

**Option A — hand-written Vulkan behind `DX8Wrapper` — is viable on Apple Silicon.** The single
blocker the review identified was real, was reproduced exactly, and was ~20 lines. With it fixed,
the spike produces an image indistinguishable from the Linux reference, and the nine capabilities
that would most plausibly have invalidated the approach — DXT, stencil, render targets, depth,
blending, multi-texture, dynamic buffers — all work with the validation layer clean. Risk #2 in
`review-and-decisions.md` should be closed, and the DXT worry that fed it should be struck rather
than carried forward.

The estimate does not move. Nothing here reduces Phase 4's ~1,000–1,700 h — that work is
`SetRenderState`, `SetTextureStageState`, lighting and fog, all of which is API-shape work that a
working MoltenVK does not do for you. What has changed is that the *probability* of the whole
approach being wrong has dropped a long way, and one of the two named renderer risks is gone.

**On spiking DXVK's `d3d8` frontend on MoltenVK: still worth the half day, but it is now clearly
the second priority, not the first.** The argument for it in §2.1 was partly that MoltenVK was
unverified and option A's foundation might not exist. That argument is spent. The remaining
argument — that DXVK covers far more of the 62 D3D8 methods on day one than a from-scratch backend
will in a year — is unaffected by anything measured here and is still a real argument. Worth
knowing before committing 1,000+ hours, but it is a comparison of two viable options rather than a
search for a viable one.

What should happen first is unchanged and unconditional: **route the 376 direct
`IDirect3DDevice8` call sites through `DX8Wrapper` while still on Windows and D3D8.** Options A and
B both need it, and it is the only renderer work that does not depend on the decision.

## Reproducing

```sh
brew install cmake molten-vk vulkan-headers vulkan-loader vulkan-tools glslang vulkan-validationlayers
cmake -S spikes/renderer -B build-spike -DSPIKE_USE_SDL2=OFF
cmake --build build-spike

export DYLD_LIBRARY_PATH=/opt/homebrew/lib   # so the validation layer actually loads
./build-spike/zh-renderer-spike --out macos.png
./build-spike/zh-caps-probe
./build-spike/zh-feature-probe
```

All three exit non-zero on failure, so they work as CI checks on a macOS runner. The raw captured
output from this machine is committed alongside the images:
`spikes/renderer/docs/caps-probe-m1pro.txt` and `spikes/renderer/docs/feature-probe-m1pro.txt`.
