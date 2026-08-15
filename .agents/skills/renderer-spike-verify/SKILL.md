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
ls /usr/share/vulkan/icd.d/lvp_icd.*.json                                     # lavapipe present
test -f /usr/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json    # layer present
cmake -S spikes/renderer -B build/spike -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/spike
```

What 22.04 cannot supply is jammy's *apt* Vulkan headers (1.3.204): they predate
`VK_KHR_portability_enumeration` and cannot compile the MoltenVK portability opt-in. The box itself
is fine — point the compiler at a newer headers checkout and both the build and the run work, which
is the difference between compile-checking a renderer fix and asserting on pixels (verified on
jammy, clang 14, lavapipe):

```sh
cmake -S spikes/renderer -B build/spike -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DSPIKE_USE_SDL2=OFF -DCMAKE_CXX_COMPILER=clang++-14 \
  -DCMAKE_CXX_FLAGS="-I$VULKAN_HEADERS_INCLUDE"
cmake --build build/spike
```

CI's `renderer-spike-linux` job still runs on 24.04 with clang 18; a local jammy run is a
cross-check, not a substitute for it.

## Assert on pixels

The ICD manifest on jammy is `lvp_icd.x86_64.json`, not `lvp_icd.json` — a wrong path here means no
lavapipe and a run that proves nothing.

```sh
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json
export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
python3 scripts/ci/check-spike-render.py \
  --binary build/spike/zh-renderer-spike \
  --reference spikes/renderer/docs/spike-triangle.png \
  --out spike.png
build/spike/zh-feature-probe
build/spike/zh-fixedfunc-tests
build/spike/zh-resource-lock-tests
```

The assertion is `0 case(s) failed` **and** `validation messages: 0`.

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
