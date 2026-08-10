# Renderer spike — `DX8Wrapper`-shaped abstraction on Vulkan

A standalone proof-of-concept for the Phase 4 renderer question: *can the engine's Direct3D 8
call pattern be served by Vulkan (and therefore MoltenVK)?*

Read `docs/porting/renderer-surface.md` for the measured D3D8 surface, the Vulkan mapping table,
the recommendation, and the revised Phase 4 estimate. This README covers only how to build and
run the code.

**This is not wired into the game.** It has its own CMake project, is not referenced by the
top-level build, and touches no engine code. It cannot affect the Windows build.

![the spike running](docs/spike-triangle.png)

800×600, read back from the Vulkan colour target and written as a PNG by the program itself:
a textured triangle (`D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1`, stage 0 set to
`COLOROP=MODULATE`, `COLORARG1=TEXTURE`, `COLORARG2=DIFFUSE`) and, in the corner, an
alpha-blended screen-space quad (`D3DFVF_XYZRHW | D3DFVF_DIFFUSE`, no texture,
`COLOROP=SELECTARG1/DIFFUSE`). Two draws, two different state blocks, two lazily created
`VkPipeline`s.

## What it is

| File | Role |
|---|---|
| `src/render_backend.h` | the abstraction. Method names, argument order and semantics deliberately mirror `Core/Libraries/Source/WWVegas/WW3D2/dx8wrapper.h` |
| `src/d3d8_subset.h` | the D3D8 enum vocabulary the engine uses, redeclared so the spike needs no Windows SDK |
| `src/state_translate.{h,cpp}` | D3D8 enum → Vulkan enum mapping, the `PipelineKey` that decides what has to be baked into a pipeline, and FVF → vertex-input decoding |
| `src/vulkan_backend.cpp` | the backend: device setup, pipeline cache, sampler cache, D3D8-style mutable shadow state, draw submission, readback |
| `shaders/fixedfunc.vert` | vertex stage, including the `D3DFVF_XYZRHW` pretransformed path Vulkan has no equivalent for |
| `shaders/fixedfunc.frag` | the `SetTextureStageState` combiner cascade, interpreted at runtime from a uniform block |
| `src/main.cpp` | driver. Issues the calls in the engine's order, then proves the result by readback |
| `src/caps_probe.cpp` | `zh-caps-probe`: formats, features and limits, queried. No rendering |
| `src/feature_probe.cpp` | `zh-feature-probe`: nine rendered cases (DXT, stencil, render targets, depth, blending, two stages, dynamic buffers), each asserted by reading pixels back |
| `tools/d3d8-surface-scan.py` | the measurement behind every number in `renderer-surface.md` |

The interesting parts, if you only read two: the pipeline cache in `vulkan_backend.cpp`
(D3D8 mutates one state at a time, Vulkan wants an immutable pipeline — this is the reconciliation)
and `shaders/fixedfunc.frag` (D3D8's texture-stage cascade has no Vulkan analogue at all).

## Prerequisites

- CMake ≥ 3.20, Ninja or Make, a C++17 compiler
- Vulkan headers + loader, and either `glslangValidator` or `glslc`
- optional: SDL2, for a real window instead of headless rendering
- optional: `VK_LAYER_KHRONOS_validation`, on by default when present

On Debian/Ubuntu:

```sh
sudo apt-get install -y cmake ninja-build libvulkan-dev vulkan-validationlayers \
                        glslang-tools vulkan-tools libsdl2-dev
# a CPU Vulkan device, if the machine has no GPU (this is how it was verified):
sudo apt-get install -y mesa-vulkan-drivers
```

On macOS (verified on an M1 Pro — see `docs/porting/moltenvk-findings.md`):

```sh
brew install cmake molten-vk vulkan-headers vulkan-loader vulkan-tools glslang \
             vulkan-validationlayers
```

The LunarG SDK works too. Two macOS quirks, neither of them a code problem:

- configure with `-DSPIKE_USE_SDL2=OFF`; Homebrew's SDL2 headers pull in `immintrin.h` and do not
  compile on arm64. Headless is the default path anyway.
- `export DYLD_LIBRARY_PATH=/opt/homebrew/lib`, or the loader cannot find the validation layer's
  dylib and the run proceeds *without* validation while still printing `validation messages: 0`.

## Build

```sh
cmake -S spikes/renderer -B /tmp/zh-renderer-build -G Ninja
cmake --build /tmp/zh-renderer-build
```

The shaders are compiled to SPIR-V as part of the build; there is nothing to do by hand.

## Run

Headless is the default and needs no display server:

```sh
/tmp/zh-renderer-build/zh-renderer-spike --out spike-triangle.png
```

Expected output:

```
device: llvmpipe (LLVM 15.0.7, 256 bits)
wrote spike-triangle.png (800x600), 2 VkPipeline(s) created for 2 draws
centre pixel rgba = 132,151,170,255
validation messages: 0
OK
```

It exits non-zero if nothing rasterised at the centre of the target, or if the validation layer
reported anything — so it is usable as a CI check, not just a demo.

On a headless box with no GPU, select the Mesa CPU device explicitly:

```sh
unset DISPLAY
export XDG_RUNTIME_DIR=/tmp/xdgrt && mkdir -p "$XDG_RUNTIME_DIR" && chmod 700 "$XDG_RUNTIME_DIR"
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json \
  /tmp/zh-renderer-build/zh-renderer-spike --out spike-triangle.png
```

(`XDG_RUNTIME_DIR` and unsetting `DISPLAY` are needed because the loader otherwise tries the X11
surface path and fails with `BadMatch` on a headless display.)

With a window, if SDL2 was found at configure time — renders 240 frames, Esc or close to quit:

```sh
/tmp/zh-renderer-build/zh-renderer-spike --window
```

Options: `--window`, `--out <file.png>`, `--no-validation`, `--help`.

The committed `docs/spike-triangle.png` was run through `optipng`; the program's own output is the
same image but larger, since its built-in PNG writer uses stored (uncompressed) deflate blocks to
avoid a zlib dependency.

## The probes

Two extra binaries, built by the same project, answer what the two draws above cannot:

```sh
/tmp/zh-renderer-build/zh-caps-probe      # formats, features, limits, portability subset
/tmp/zh-renderer-build/zh-feature-probe   # nine rendered cases, each checked pixel by pixel
```

`zh-feature-probe` covers depth test, alpha blending, BC1/BC3 (with alpha and with a mip chain),
two texture stages in one draw, stencil write + test, render-to-texture, and dynamic buffer
suballocation. Both exit non-zero on failure. Results on Apple Silicon:
`docs/porting/moltenvk-findings.md`.

## Reproduce the measurements

```sh
python3 spikes/renderer/tools/d3d8-surface-scan.py
```

Prints the `IDirect3DDevice8` / `IDirect3D8` method tables with per-method macro vs. direct call
counts, the `D3DX*` entry points, the top files by call-site count, the `D3DRS_*` / `D3DTSS_*` /
`D3DTOP_*` / `D3DFVF_*` / `D3DFMT_*` / `D3DPOOL_*` / `D3DLOCK_*` usage histograms, and the `dx8*`
line count. Add `--json` to dump every individual call site. Methodology and its known limitations
are documented in `docs/porting/renderer-surface.md`.

## What this does and does not prove

Verified working on `llvmpipe (LLVM 15.0.7)`, Vulkan 1.1, validation layer enabled, zero
validation messages:

- D3D8's one-state-at-a-time mutable model reconciled with immutable `VkPipeline`s, via a shadow
  state block hashed into a pipeline cache and materialised lazily at draw time
- `SetTextureStageState`'s combiner cascade interpreted in a fragment shader from a uniform block:
  14 of the 17 `D3DTOP_*` ops the engine uses, plus the `COMPLEMENT`/`ALPHAREPLICATE` modifiers
- FVF bitfields decoded into `VkVertexInputAttributeDescription`s, including `D3DFVF_XYZRHW`
- `Clear` as `vkCmdClearAttachments` inside the pass, preserving D3D8's mid-frame semantics
- alpha test as a shader `discard`, since Vulkan has none
- sampler cache keyed on D3D8 *stage* filter/address state, which in Vulkan is *object* state
- correctness asserted by reading the colour target back, not by looking at it

Also verified on `Apple M1 Pro` / macOS 26.6.1 / MoltenVK 1.4.2, validation layer enabled, zero
validation messages: the readback is pixel-identical to the Linux image above to within 1/255 on
every one of the 480,000 pixels (`docs/spike-triangle-macos.png`). This needed the
`VK_KHR_portability_enumeration` / `VK_KHR_portability_subset` opt-in that the first version of the
backend lacked — without it `vkCreateInstance` returns `VK_ERROR_INCOMPATIBLE_DRIVER`. The full
measurement, including what MoltenVK does *not* provide, is in `docs/porting/moltenvk-findings.md`.

Also not implemented *in the backend*, and none of it should be assumed: fixed-function
lighting/fog/material, `SetRenderTarget` and render-to-texture, ps.1.1/vs.1.1 translation, texture
stages beyond the first two, texture transform matrices, `BUMPENVMAP`, mipmaps, DXT/cube/volume
textures, `CopyRects`, `DrawPrimitiveUP`, `ProcessVertices`, dynamic buffer rings,
`D3DPOOL_MANAGED` shadow copies, and device loss. (`zh-feature-probe` shows the *driver* handles
render targets, DXT, mipmaps and dynamic buffers correctly — that is a different claim from the
backend implementing them.) There is a hard cap of 64 draws per frame. The full list, with the
reasoning, is in `docs/porting/renderer-surface.md` §2.
