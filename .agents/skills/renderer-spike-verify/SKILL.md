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

On a box whose blueprint already provisioned the pinned headers, `$VULKAN_HEADERS_INCLUDE` points at
them and passing them as a flag configures and builds the same way:

```sh
cmake -S spikes/renderer -B build/spike -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DSPIKE_USE_SDL2=OFF -DCMAKE_CXX_COMPILER=clang++-14 \
  -DCMAKE_CXX_FLAGS="-I$VULKAN_HEADERS_INCLUDE"
cmake --build build/spike
```

On jammy the lavapipe manifest is `lvp_icd.x86_64.json`, not `lvp_icd.json`:
`VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json`.

Two things that make the command above fail on a box where it should work:

- `$VULKAN_HEADERS_INCLUDE` may be empty even where the headers are provisioned; then
  `-I$VULKAN_HEADERS_INCLUDE` expands to a bare `-I` and configure dies in `find_package(Vulkan)`.
  Echo it first, and fall back to the checkout itself (`$HOME/vulkan-headers/include`).
- Configure the spike with **`/usr/bin/cmake`** explicitly. The `native-port-measure` skill has you
  `pip install --user 'cmake==4.1.2'` for the OpenAL symbol gate, and that puts a CMake 4 ahead of
  the system one on `PATH`; its `FindVulkan` then reports
  `Could NOT find Vulkan (missing: Vulkan_LIBRARY) (found version "1.3.204")` on jammy even with
  `libvulkan-dev` installed. CMake 3.22 configures the same tree without a flag change. Delete
  `build/spike` before retrying so no CMake 4 cache is left behind.

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
python3 scripts/ci/check-hidpi-scale.py --binary build/spike/zh-hidpi-tests
python3 scripts/ci/check-backend-coverage.py
```

`check-hidpi-scale.py` is the HiDPI rule: the default colour target, the viewport, the scissor and
the readback follow the window's backing scale in pixels while the mouse and the GUI stay in points
(`docs/porting/hidpi-scale.md`). It injects scales of 2.00, 1.00 and 1.25 because no Linux display
has one — at scale 1 the presentation blit is an identity copy and the pre-fix half-resolution code
passes. **The injected scale is not a Retina answer.** On a Mac with a Retina panel run the real
thing, which reads `NSWindow.backingScaleFactor` off the actual display:

```sh
spikes/renderer/tools/macos-window-check.sh          # includes zh-hidpi-tests-cocoa --window
build/spike/zh-hidpi-tests-cocoa --window --min-scale 2.0    # or on its own
```

The spike also carries a measured ceiling and two derived counts, all three gated in the
`renderer-spike-linux` job and all three part of a measurement sweep. The staging gate needs
`XDG_RUNTIME_DIR` set to a private directory and no `DISPLAY`, and its `--self-check` additionally
proves the ceiling *rejects* the pre-pool per-resource-staging behaviour, so a pass means the gate
can still catch a regression:

```sh
mkdir -p /tmp/xdgrt && chmod 700 /tmp/xdgrt
XDG_RUNTIME_DIR=/tmp/xdgrt DISPLAY= python3 scripts/ci/check-staging-cost.py \
  --binary build/spike/zh-staging-workload --self-check
python3 spikes/renderer/tools/d3d8-lock-scan.py --check
python3 spikes/renderer/tools/surface-lock-audit.py --check
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
python3 scripts/ci/check-backend-coverage.py
```

Run **both**. One new D3D8 call site is recorded in three separate checked-in files, and each one
fails on its own push if you only run the first gate: the direct-call allowlist
(`spikes/renderer/tools/d3d8-direct-allowlist.json`), the classification map
(`backend-coverage-map.json`, which requires every reached method to have a category) and the
coverage baseline (`backend-coverage-baseline.json`, which counts sites per method). Adding
`d3dx8texcreate.cpp` in #86 cost three CI round trips exactly this way.

The direct-call budget is enforced in both directions: a new unbudgeted direct
`D3DDevice->Method(...)` fails the gate, and so does a stale allowlist entry. D3D8 calls belong only
in the `DX8CALL*` macros, inside `WW3D2/d3d8renderbackend.cpp` (the D3D8 side of the
`RenderBackendClass` seam), and in `WW3D2/d3dx8texcreate.cpp`, which is d3dx8.lib's own creation
entry points off Windows and is budgeted with that reason.

## macOS / MoltenVK

The `renderer-spike-macos` CI job runs on `macos-15` — `macos-14` aborts inside
`MVKPhysicalDevice::initMetalFeatures` against the runner's paravirtualised Metal device. Locally:
build with `-DSPIKE_USE_SDL2=OFF`, then

```sh
eval "$(python3 scripts/ci/vulkan_manifests.py --require-layer --print-env)"
```

which writes copies of Homebrew's validation-layer and MoltenVK manifests with **absolute**
`library_path` values under `build/vulkan-manifests/` and prints the `VK_LAYER_PATH` and
`VK_ICD_FILENAMES` that select them. `--require-layer` fails loudly when the layer is not installed.

`DYLD_LIBRARY_PATH` is not the recipe and must not be used as one: SIP strips `DYLD_*` from the
environment when it execs `/bin/bash` or `/usr/bin/python3`, so every `scripts/ci/*.py` gate that
launches a spike binary lost the layer, and Homebrew's manifest naming its dylib relatively then
either failed with `VK_ERROR_LAYER_NOT_PRESENT` (-6) or, worse, ran unvalidated and reported
`validation messages: 0`. `VK_LAYER_PATH`/`VK_ICD_FILENAMES` survive the exec, and the Python gates
call `vulkan_manifests.child_environment()` themselves so a direct `python3 scripts/ci/...` run is
already correct. `validation messages: 0` is only evidence when the same run also printed
`validation layer: loaded` — the spikes print it, and the gates require it.

**The CI macOS runner has a paravirtualised GPU.** Its check asserts that `vkCreateInstance` succeeds
against a portability driver and that the readback is within a loose tolerance. It does **not** prove
rasterisation parity or performance on real Apple Silicon. Never present it as hardware verification.
