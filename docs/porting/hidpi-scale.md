# Points and pixels: the HiDPI rule, and the test that can fail at scale ≠ 1

The retail process on an M1 Pro rendered a quarter of the panel's pixels and let MoltenVK upscale the
result. `docs/porting/apple-silicon-verification.md` §8.4 measured it in the running game:

```
window client area (Cocoa bounds)   800 x 600    points
CAMetalLayer.drawableSize           1600 x 1200  pixels
swapchain_extent_                   1600 x 1200  pixels
width_ / height_ (colour target)    800 / 600    points
viewport_                           800 x 600    points
TheDisplay / TheMouse maxX,maxY     800 / 600    points
```

`Present` reconciled the two with a `VK_FILTER_LINEAR` `vkCmdBlitImage` from 800x600 to 1600x1200.
Input and GUI hit-testing were measured as consistently in points and were correct.

This slice fixes the colour target, states the rule, and — the part that keeps it fixed — adds an
assertion that runs at a backing scale no Linux display has.

## 1. The rule

**One seam converts. Points on the window side of it, pixels on the device side.**

| Quantity | Unit | Why |
|---|---|---|
| `NSWindow`/SDL window geometry, `Window_Client_Size` | points | what the window system sizes windows in |
| Mouse coordinates, `TheMouse`, GUI hit-testing, `TheDisplay` width/height | **points** | measured correct on hardware; scaling them would break every `.wnd` layout |
| D3D8 logical viewport and scissor, `Set_Viewport`, `Set_Scissor` | points | the engine's own space; the D3D8 surface does not know about scale |
| Vulkan swapchain extent, default colour and depth images, framebuffers, hardware viewport and scissor, render areas, clears, readback | **pixels** | what the drawable actually has |

The conversion is `Window_Backing_Scale()` (`WWLib/platform/platform_window.h`), the only place the
factor is read, and the default render target's size is

```
device_pixels = ceil(client_points * backing_scale)
```

`ceil`, not truncation: a 1.25 scale on a 100-point window is 125 pixels, and a target one pixel
short of the drawable leaves a column of the panel unwritten. The backend keeps both sizes —
`width_`/`height_` in points, `device_width_`/`device_height_` in pixels — and its D3D8-facing
surface descriptions keep advertising the point size, so nothing above the seam changes.

Implementations of the seam: `NSWindow.backingScaleFactor` (Cocoa), the drawable-size to
window-size ratio (SDL2), `1.0f` elsewhere.

### What the scale does *not* multiply

**Render-to-texture resources.** A 256x256 render target is 256x256 pixels on any display; the
window's scale is a property of the window, not of every image. `Surface_Render_Scale()` returns 1
for anything that is not the default target, so a `32x32` texture stays `32x32` at scale 2 — asserted
by the suite.

### Scale changes at runtime

Dragging a window between a Retina and a non-Retina display changes `backingScaleFactor` with no
resize. `Resize_Presentation()` therefore re-reads the window's scale before it rebuilds the
swapchain, and a changed scale waits for the device to go idle and rebuilds the default colour and
depth targets. The suite drives 2 → 1 → 2 and checks the target size after each transition.

### Why the stretch blit stays

Deleting it looks tempting once the target is the drawable's size, and is wrong. The blit is what
makes a **resized** window fill: between a resize and the swapchain rebuild — and whenever the
client area and the swapchain images disagree at all — a direct present would paint the old extent
into a corner of the new one. So the blit remains, with its source extent now the *pixel* size of
the default target rather than the point size. At scale 1 with a settled swapchain it is an identity
copy, which is exactly what it was before; at scale 2 it is now 1:1 instead of a 2x magnification.

### Readback when the two sizes differ

`Read_Back_Color_Target()` reads the default target and reports **pixels**: `device_width_ x
device_height_`, with a buffer sized to match. Callers that ask for point-space rectangles of a
scaled default back buffer are refused rather than served a plausible wrong answer —
`Resolve_Surface_Read()`, `Copy_Rects()` and the default back-buffer lock paths all fail when the
default target is scaled, because a point-space rect has no single correct interpretation there and
silently rounding one would be a fake result. Nothing in the engine takes those paths on the default
back buffer today; the spike suite asserts the refusal.

## 2. The test that can fail at scale ≠ 1

The defect survived CI because **Linux CI only ever asserted scale 1.00**, where source and
destination extents are equal and the blit is an identity copy: the pre-fix code passes every
existing gate at scale 1, and there is no Retina panel on any runner.

`spikes/renderer/src/hidpi_tests.cpp` (`zh-hidpi-tests`, gated by
`scripts/ci/check-hidpi-scale.py`) therefore *injects* the scale: `Set_Render_Scale()` before `Init`
puts the backend in the state a Retina window puts it in, with a headless surface sized independently
of any client area. It runs at 2.00, 1.00 (the transition back) and 1.25, and asserts, per scale:

* the default colour target is `ceil(points * scale)` pixels;
* the readback reports those pixel dimensions;
* a half-width **point-space** quad covers half the *pixel* target, with the coverage edge inside one
  pixel of the midpoint — a half-resolution render upscaled by the blit fails this;
* a scissor rectangle given in points clips the corresponding scaled pixels;
* a `32x32` render-to-texture target is still `32x32`;
* the scaled default back buffer refuses the ambiguous point-space copy and lock paths;
* `Set_Render_Scale(0.0)` is refused, and the validation layer was loaded and silent.

Linux, lavapipe, `35 checks, 0 failed`. Reverting the target creation to `device_width_ = width_` —
i.e. the pre-fix code — fails it: Vulkan validation reports the framebuffer dimensions disagreeing
with the render area, the coverage-edge checks fail, and a run got as far as `double free or
corruption (!prev)`. The gate also refuses a run that quietly stops exercising the scaled paths: it
requires the 2.00, 1.00 and 1.25 assertions to be present, at least 20 checks, and
`validation layer: loaded`.

The same binary runs in the `renderer-spike-macos` job, so the arithmetic is exercised through
MoltenVK as well as lavapipe.

## 3. What is Linux-only, and what a Mac session must run

**Since confirmed on real hardware.** An M1 Pro run of every gate below, with the numbers, is in
[`base-game-install-path.md`](base-game-install-path.md) §4: `backingScaleFactor` 2.00 read off the
panel, a 100x80 point client rendering at 200x160 pixels, `9 checks, 0 failed`, and the retail menu's
800x600 default at 1600x1200 pixels rather than upscaled.

**Everything above was measured on Linux against lavapipe, plus MoltenVK on a display-less CI
runner.** An injected scale is not a Retina display: it proves the arithmetic and the plumbing, and
it cannot prove that `NSWindow.backingScaleFactor` reaches the backend on real hardware, that
`CAMetalLayer.drawableSize` agrees with `ceil(points * scale)`, or that the panel shows a
full-resolution image.

The standalone check for that is in the tree and needs a Mac with a Retina panel:

```sh
spikes/renderer/tools/macos-window-check.sh        # step 6 is the full-resolution check
build/spike/zh-hidpi-tests-cocoa --window --min-scale 2.0
```

`--window` creates a real `NSWindow` through the same `platform_window_cocoa.mm` the engine uses,
reads `backingScaleFactor` off the display it opened on, initialises a real swapchain, and then
asserts the same things the injected run does — plus that the scale it found is at least
`--min-scale`, so a run on a non-Retina display **fails** instead of reporting a pass that answers a
different question. `macos-window-check.sh --allow-no-display` downgrades it to a skip and says the
full-resolution claim is unverified.

A Linux equivalent exists for the plumbing only: `zh-hidpi-tests-sdl2 --window --min-scale 1.0`
passes on an X display at scale 1.00 (`9 checks, 0 failed`), which shows the window-to-backend path
works, at the one scale that cannot distinguish the bug.

## 4. Classification

| Finding | Class |
|---|---|
| Default colour target sized in points on a scale-2 display | **port defect**, fixed here |
| Linux CI asserting scale 1.00 only | **synthetic-only** coverage gap, closed by `check-hidpi-scale.py` |
| Full-resolution rendering on a Retina panel | **unverified**: needs the Mac check above |

## 5. The macOS validation-layer recipe, and making its absence fatal (§8.1)

Two defects, and the second is the dangerous one.

**The recipe.** Homebrew's `VkLayer_khronos_validation.json` names its dylib *relatively*, so the
loader only finds it if the library directory is on the dynamic loader's path — and the repo's recipe
was `DYLD_LIBRARY_PATH`. SIP strips every `DYLD_*` variable when it execs a protected binary such as
`/usr/bin/python3` or `/bin/bash`, so every `scripts/ci/*.py` gate that launched a spike lost the
layer: either `VK_ERROR_LAYER_NOT_PRESENT` (-6), or a run with no layer at all.

`scripts/ci/vulkan_manifests.py` replaces it. It finds Homebrew's validation-layer and MoltenVK
manifests, resolves each relative `library_path` to the real dylib on disk, writes copies carrying
**absolute** paths under `build/vulkan-manifests/`, and hands back `VK_LAYER_PATH` and
`VK_ICD_FILENAMES` — neither of which SIP touches. It refuses to write a manifest whose library it
could not find, and it is a no-op off macOS. Every gate that starts a spike binary now builds its
child environment through `vulkan_manifests.child_environment()`, so the layer survives the exec
chain, and the macOS CI job resolves the manifests once into `$GITHUB_ENV`. `--self-check` exercises
the rewriting on a synthetic manifest, including the rejection of a missing library, and runs on
Linux:

```
$ python3 scripts/ci/vulkan_manifests.py --self-check
OK: manifest rewriting resolves a leaf library name and rejects a missing one
```

**The silent success.** `validation messages: 0` from a run with no layer loaded is not evidence of
anything, and that is precisely what the broken recipe produced. The backends now report whether the
layer was actually loaded and the debug messenger actually installed
(`RenderBackend::Validation_Active()`), the spikes print `validation layer: loaded`, and a run asked
for `--validation` that did not get it **fails**. A validation result now requires all three: the
layer was loaded, the messenger was installed, and the message count is zero.

Classification: **port defect** in the repo's own tooling — a gate that could report success without
having tested anything.

## 6. The two smaller fixes' siblings

The exit-time stack overflow (§8.5) is a lifetime fix in the same "only a real Mac could see it" set
and is recorded, with its harness output and its control, in
`docs/porting/memory-shutdown-order.md`.
