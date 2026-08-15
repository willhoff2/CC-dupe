---
name: renderer-spike-verify
description: Build and verify the D3D8-to-Vulkan renderer spike in spikes/renderer, including the pixel comparison, the fixed-function assertions, and the D3D8 call-surface gate. Use for any renderer or MoltenVK change.
---

# Verifying the renderer spike

The exit status of the spike is **not** the assertion. The framebuffer readback and a loaded, silent
validation layer are.

## Build (Linux, software Vulkan)

```sh
sudo apt-get install -y clang cmake ninja-build libvulkan-dev vulkan-validationlayers \
    glslang-tools mesa-vulkan-drivers
test -f /usr/share/vulkan/icd.d/lvp_icd.json                                  # lavapipe present
test -f /usr/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json    # layer present
cmake -S spikes/renderer -B build/spike -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/spike
```

Ubuntu 22.04 works, but **not** with jammy's Vulkan headers (1.3.204): they predate
`VK_KHR_portability_enumeration` and cannot compile the MoltenVK portability opt-in. Point CMake at
pinned headers instead of skipping the box:

```sh
git clone --depth 1 -b v1.3.280 https://github.com/KhronosGroup/Vulkan-Headers ~/vk-headers
cmake -S spikes/renderer -B build/spike -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=clang++-14 -DVulkan_INCLUDE_DIR=$HOME/vk-headers/include
```

On jammy the lavapipe manifest is `lvp_icd.x86_64.json`, not `lvp_icd.json`:
`VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json`.

## Assert on pixels

```sh
python3 scripts/ci/check-spike-render.py \
  --binary build/spike/zh-renderer-spike \
  --reference spikes/renderer/docs/spike-triangle.png \
  --out spike.png
build/spike/zh-feature-probe
build/spike/zh-fixedfunc-tests --validation
build/spike/zh-resource-lock-tests --validation
ZH_SPIKE_NO_VIEW_SWIZZLE=1 build/spike/zh-fixedfunc-tests --validation
ZH_SPIKE_NO_VIEW_SWIZZLE=1 build/spike/zh-resource-lock-tests --validation
python3 scripts/ci/check-backend-coverage.py
```

A validation message is a failure even when the pixels are right. The failure class to expect:
a texture's image and a `GetSurfaceLevel` surface viewing it are two names for one image, so any
path that transitions the image directly (`Update_Texture`, the CopyRects upload) must record the
new layout on both — otherwise a read either side of it barriers from a layout the image left, and
only the layer notices. Probe it as `read -> Update_Texture -> read`; each half alone is silent.

If the validation layer is not loaded, the run silently proceeds unvalidated and proves nothing —
check that it was.

## Keep the D3D8 surface contained

```sh
python3 scripts/ci/check-d3d8-surface.py
```

The direct-call budget is **0** and is enforced in both directions: a new direct
`D3DDevice->Method(...)` fails the gate, and so does a stale allowlist entry. D3D8 calls belong only
in the `DX8CALL*` macros and inside `WW3D2/d3d8renderbackend.cpp`, which is the D3D8 side of the
`RenderBackendClass` seam.

## macOS / MoltenVK

The `renderer-spike-macos` CI job runs on `macos-15` — `macos-14` aborts inside
`MVKPhysicalDevice::initMetalFeatures` against the runner's paravirtualised Metal device. Locally:
build with `-DSPIKE_USE_SDL2=OFF`, locate the ICD manifest rather than hard-coding it
(`find "$(brew --prefix molten-vk)/" -name MoltenVK_icd.json`), and set `DYLD_LIBRARY_PATH` to the
validation-layer and MoltenVK lib directories — Homebrew's layer manifest names its dylib relatively,
so without it the loader cannot find the layer.

**The CI macOS runner has a paravirtualised GPU.** Its check asserts that `vkCreateInstance` succeeds
against a portability driver and that the readback is within a loose tolerance. It does **not** prove
rasterisation parity or performance on real Apple Silicon. Never present it as hardware verification.
