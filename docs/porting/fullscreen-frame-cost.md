# Fullscreen frame cost on a Retina Mac: what it costs, and why the report was not reproduced

User report (2026-09-30): *"Good when smaller screen, but full screen it's slow."* This slice
measures windowed against fullscreen before changing anything. **Result: the extra pixels cost
something, and the cost grows with pixel count. But in the two scenes measured, fullscreen still held
the 30 fps cap and had fewer late frames than the user's windowed setup.** The slowdown was not
reproduced, so no fix is landed here. A candidate fix (render fullscreen at one pixel per point) is
written, and §5 gives the conditions for landing it.

Evidence categories are kept apart: **MEASURED** (method named), **INFERRED** (from source, not
run), **UNMEASURED**.

## 1. Set-up

| | |
|---|---|
| Machine | Apple M1 Pro, macOS 26.6.1, built-in panel 3456x2234 pixels = 1728x1117 points at backing scale 2 |
| Binary | `origin/main` `ec61b5c07` + this branch's `ZH_RENDER_FRAME_LOG` hook only; `native-build.py --level 1..4 --with-shims --strict-link`, 981/981 objects, 0 unresolved, `lipo -archs` = `arm64`, `sysctl.proc_translated` = 0 |
| Data | retail Zero Hour 1.04 (`~/devin-work/zh-data`), symlinked run directory; `Options.ini` `StaticGameLOD = Low` (the user's own) |
| Launch lines | the user's own, from shell history: windowed `-win -xres 1024 -yres 768`, fullscreen `-xres 1728 -yres 1117` (as `docs/working/running-on-macos.md` instructs) |
| Harness | `scripts/macos-fullscreen-scale-probe.py`. It launches the game and makes real `CGEventPost` clicks through Single Player -> Skirmish -> Play Game, with coordinates scaled from the 800x600 WND layout to each case's resolution. After 45 s of load it reads 20 s (~600 frames) of the backend's per-frame CSV. `--scene shellmap` stays on the main menu's 3D shell map instead |
| Results | `ci-baselines/fullscreen-frame-cost-skirmish-macos-arm64.json`, `ci-baselines/fullscreen-frame-cost-shellmap-macos-arm64.json` |

`ZH_RENDER_FRAME_LOG=<path>` (in `spikes/renderer/src/vulkan_backend.cpp`) writes one row per
presented frame. Each row has these fields:

- `interval_ms`: present to present.
- `scene_ms`: `Begin_Scene` to the frame's submit.
- `acquire_ms`, `present_ms`: the image acquire and the queue-idle wait plus the blit.
- The colour-target, swapchain and back-buffer sizes, and the scale.

The backend is synchronous: every one-shot command buffer ends in `vkQueueWaitIdle`. So GPU time
appears in this log as CPU wall time, in `scene_ms` when the wait happens mid-scene and in
`present_ms` otherwise. *Busy* below is `scene_ms + acquire_ms + present_ms`: the renderer's share of
the frame. The engine's logic and scene traversal before `Begin_Scene` are not in it, but they are
in `interval_ms`.

## 2. MEASURED: skirmish (start of a skirmish, ~55 s of game time, the player's base in view)

| case | colour target (px) | Mpx | fps (median interval) | interval p95 ms | frames > 40 ms | busy median ms | busy p95 ms | stretched |
|---|---|---:|---:|---:|---:|---:|---:|---|
| windowed 1024x768 (the user's "good") | 2048x1536 | 3.15 | 29.4 | 41.3 | 8.3 % | 18.8 | 23.4 | no |
| windowed 1600x1000 (control: pixels, not mode) | 3200x2000 | 6.40 | 29.9 | 38.9 | 2.3 % | 21.5 | 25.6 | no |
| **fullscreen 1728x1117 (the user's "slow")** | 3456x2234 | 7.72 | **30.0** | **37.0** | **2.0 %** | 22.8 | 26.5 | no |
| fullscreen, no `-xres` (retail default 800x600) | 1600x1200 | 1.92 | 29.7 | 41.0 | 6.8 % | 15.6 | 20.6 | no |

## 3. MEASURED: main-menu shell map

| case | Mpx | fps | busy median ms | busy p95 ms | present wait median ms |
|---|---:|---:|---:|---:|---:|
| windowed 1024x768 | 3.15 | 30.0 | 11.3 | 13.4 | 10.2 |
| windowed 1600x1000 | 6.40 | 30.0 | 15.9 | 19.8 | 13.7 |
| fullscreen 1728x1117 | 7.72 | 29.9 | 17.3 | 19.5 | 14.5 |
| fullscreen, no `-xres` | 1.92 | 30.0 | 8.3 | 10.2 | 7.0 |

## 4. What the numbers say

- **Renderer time does scale with pixels**:
  - roughly 1.2 ms per megapixel in the skirmish (15.6 -> 22.8 ms over 1.9 -> 7.7 Mpx);
  - roughly 1.5 ms per megapixel at the shell map.
  - The large *window* costs about what fullscreen costs at a similar pixel count (21.5 vs 22.8 ms),
    so what costs frame time is the pixel count, not fullscreen as such.
- **It is not enough to break the frame rate in these scenes.** Fullscreen's renderer share is
  about 4 ms (skirmish) and 6 ms (shell map) above the user's windowed 1024x768, and still well
  inside the 33.3 ms budget of the 30 fps cap. Fullscreen had the *best* interval p95 and the fewest
  late frames of the four skirmish cases. The windowed cases' extra late frames are unexplained
  (window-server compositing of a titled window is one possibility, INFERRED, unmeasured).
- **The hypothesis "fullscreen is slow because it renders 4x the pixels" is therefore not
  confirmed.** The arithmetic premise holds (7.7 Mpx vs 1.9 Mpx for 800x600, or 3.1 Mpx for the
  user's 1024x768 window). But the measured cost of those pixels does not produce a slowdown at the
  start of a skirmish or at the menu.
- **No aspect stretch** in any case. The engine's back buffer and the swapchain have the same aspect
  ratio, because `-xres 1728 -yres 1117` is the screen's own point size.
- **Fullscreen without `-xres/-yres` is still an 800x600 window**: window-server bounds `[0, 32,
  800, 600]`, top-left under the menu-bar strip. This re-measures residual 3 of
  `next-slice-scope.md`, unchanged.
- **`-noFPSLimit` does nothing under a `StaticGameLOD` preset**: the uncapped cases also ran at 30.0
  fps (MEASURED). The mechanism is INFERRED from `GameLOD.cpp`: applying the static LOD preset
  copies its `m_useFpsLimit = TRUE` back into `TheGlobalData` after the command line was parsed. So
  the harness cannot measure headroom by uncapping. The *busy* column is the headroom measure
  instead.

## 5. The candidate fix, and when to land it

Written and headless-tested, but **not committed on this branch**; it is kept as a local branch
(`perf/macos-fullscreen-render-scale-candidate`, not pushed):

- a pure policy in `WWLib/platform/platform_render_scale.h`:
  - a window renders at the backing scale;
  - fullscreen renders at 1 pixel per point;
  - `ZH_RENDER_SCALE=<0..4>` overrides either, and a bad value fails window creation;
- a new seam call `Window_Render_Scale()`, which the renderer reads instead of
  `Window_Backing_Scale()`;
- the Cocoa backend sizes `CAMetalLayer.drawableSize` and `contentsScale` from it, and lets Core
  Animation upscale.

The headless `zh-hidpi-tests` gained 17 policy checks (52/52 passing with the 2->1->2 suite). A
`--window` fullscreen round trip is written, but was not run here because the fix is not landed.

INFERRED by extrapolating from §2 (not run): fullscreen at 1 px/pt is 1.9 Mpx, so its
renderer share would drop from ~22.8 ms to about the 15.6 ms of the 800x600 row. That is ~7 ms of
headroom, paid for with a 2x compositor upscale (softer image, UNMEASURED by eye).

**Land it only if** a measurement shows fullscreen at 1728x1117 missing the 30 fps budget
(interval median > 33.4 ms, or busy close to 33 ms) in a scene the user actually plays, and the same
scene at 1 px/pt meeting it. Until then it trades sharpness for headroom nobody has shown is needed.

## 6. Is there a better way to do fullscreen?

For **cost**, the options rank as follows (INFERRED from §4 and from the design, not measured
against each other):

1. **Borderless, screen-covering window (what exists)** is right for this engine. There is no
   display-mode switch on macOS. AppKit's own fullscreen (`toggleFullScreen:`) moves the window to a
   separate Space with an animation, and renders the same number of pixels.
2. **Render scale** (§5) is the one knob that changes cost, and it is independent of how fullscreen
   is entered.
3. What is actually wrong with fullscreen today is **usability**, not speed. It needs
   `-xres/-yres` to cover the screen; without them it is an 800x600 window in the corner (§4). The
   right fix is for fullscreen to default the engine resolution to the screen's point size. That is
   residual 3 and a separate slice.

## 7. UNMEASURED, and what is needed to reproduce the report

- **What "slow" meant.** This could be frame rate, the camera or edge-scroll speed at a larger view,
  or input latency. The engine draws its own FPS counter at the top-left (`29[30]` = measured
  [target]). The user reading that number in the slow moment would settle it.
- **Heavier scenes**: late-game skirmish with many units, particles, shadows and water. Also a
  campaign mission, and `StaticGameLOD` above Low. Only the start of a skirmish and the shell map
  were measured.
- Engine CPU time outside the renderer, per frame. The log sees it only as part of `interval_ms`.
- The candidate fix's picture quality in fullscreen at 1 px/pt (needs a human look).
