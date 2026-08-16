# Resolved decisions

`review-and-decisions.md` §2 lists the questions the port could not answer for itself. This file
records the ones the project owner has now answered, so a slice does not reopen them, and states the
consequence each answer has for code that has not been written yet.

Answers are recorded verbatim where they were short, because the wording carries the intent.

## 1. The C8 GPU-read hazard — **GPU-dirty bit**

**Question** (raised by the staging-pool slice, #43, and sharpened by #46): 21 `SurfaceClass::Lock`
sites sit outside the 19 that were audited, and **5 of them read a surface the GPU can have written**
(screenshot and movie capture). Unconditional readback is always correct and always slow; a
GPU-dirty bit is fast but is more state; splitting `Lock` into read and write variants is cleanest
but edits call sites, which the seam pattern exists to avoid.

**Answer: the GPU-dirty bit.**

Consequences for the implementer:

* The bit is set where the GPU can become the writer. #46 established that those set-points already
  funnel through `Set_Render_Target` and `Copy_Rects`, so this is a small number of places rather
  than an audit of every draw call.
* A read lock on a dirty surface performs the readback and clears the bit; a read lock on a clean
  surface must not. **That "must not" is the whole point of the decision** — if the fast path is not
  actually taken, this is unconditional readback with extra bookkeeping, so it needs a test that
  fails when a clean-surface read triggers a transfer, not just one that proves pixels are correct.
* Call sites do not change. `SurfaceClass::Lock` keeps its signature.
* The 5 reading sites are the ones that must be correct; the other 16 are write-only and are
  unaffected.

**Implemented** in the spike's Vulkan backend: `renderer-resource-seam.md` §4.4. Two notes on what
the answer above assumed, both from doing it:

* `Set_Render_Target` and `Copy_Rects` were **not** the whole funnel list. Binding a target is not
  the write; the draw is. The enumerated set is `Set_Render_Target`, `Clear`, `Prepare_Draw`,
  `Copy_Rects` and `Update_Texture`, and `Prepare_Draw` is the one #46's reasoning would have missed.
* The bit belongs on the **image**, not on the surface handle, because a render-target texture and a
  `Get_Surface_Level` view of it are two handles onto the same pixels.

The "must not" is enforced by `C8 surface read hazard` in `zh-resource-lock-tests`, which asserts on
the backend's `readback_stalls` counter across each read, so a clean read that transfers fails even
with correct pixels.

## 2. Zero-on-acquire in the pooled staging path — **match Windows; generalise only where nothing depends on it**

**Question**: pooled staging publishes zeroes where the old per-lock staging happened to preserve
the previous contents. `W3DRadar`, `W3DShroud` and `Render2DSentenceClass` lock a whole level and
then draw a few pixels, so they would see the difference. Flag those sites as preserve-contents, or
declare "locks do not preserve" as an explicit contract?

**Answer, verbatim:** *"match whatever standard windows build has as closely as possible. if it
doesnt need it, make it as generalizeable as you can"*

Consequences:

* The Windows build is the oracle. A D3D8 `Lock` without `D3DLOCK_DISCARD` exposes the existing
  contents, so **the pooled path must preserve contents by default** — the previous behaviour was
  not an accident the port is free to drop, it is what the shipped game runs on.
* Where a site demonstrably overwrites every byte it locks, the preserve can be skipped as an
  optimisation, but the burden of proof is on the optimisation, not on the default.
* "Zero on acquire" is therefore **not** an acceptable contract, and the pool must not rely on
  callers tolerating it.
* Anywhere the Windows behaviour is genuinely unspecified rather than merely undocumented, prefer
  the more general implementation over one that encodes the quirk.

**Implemented**: `renderer-resource-seam.md` §4.4 and §4.1.1. `Acquire_Staging` no longer zeroes; a
lock without `D3DLOCK_DISCARD` reads the level back. The only skips are `D3DLOCK_DISCARD` (D3D8's own
proof), a level nothing has written yet (D3D8 leaves it undefined) and a level the staging block
still holds with no GPU write since (no transfer needed to honour the contract). No call site was
flagged, so nothing is encoded per site.

The cost, measured, is the part worth carrying forward: **residency and the 98.3% block reuse are
unchanged**, and preservation buys that with bandwidth — 53.9 MB of image→buffer copies per 12 frames
of the representative workload, against 0 before. The dirty bit (decision 1) hands 108 read-back
stalls per 12 frames back, so the two decisions partly pay for each other.

## 3. Retina points versus framebuffer pixels — **points**

**Question** (new, from the Apple Silicon run in `macos-hardware-verification.md` §3):
`Window_Get_Client_Size` and the mouse coordinates are Cocoa **points**, while the drawable and the
Vulkan `currentExtent` are **pixels**. At scale 2, a click at the bottom-right of an 800x600 window
reports `(799, 599)` for a pixel at `(1598, 1198)`.

**Answer, verbatim:** *"point is fine if easier"*

Consequences:

* The engine's UI, hit-testing and mouse coordinates stay in **points**, which is also the space the
  existing WND GUI layout numbers were authored in, so no GUI data changes.
* The conversion therefore belongs at the **renderer boundary**: the swapchain and viewport are sized
  in pixels from the backing store, and anything the engine is told about "the window" is points.
* This is not free and should not be described as free: a full-screen-quad or post-process effect that
  assumes framebuffer dimensions equal client dimensions is wrong at scale 2, and screenshot capture
  produces a 2x image. Those are the two places to check first.
* It does mean the seam must not quietly report pixels from `Window_Get_Client_Size` — the hardware
  run showed it currently reports points, so the existing behaviour is the intended one and should be
  documented rather than changed.

## 4. The wide-character representation — **still deferred, and deliberately abstracted**

**Answer, verbatim:** *"yes fine, wrap in if it doesnt matter either way"*

Consequences:

* The decision between UTF-16 `char16_t`/`WideChar` and 4-byte `wchar_t` stays open. No slice may
  change the representation.
* Where a slice needs to widen or narrow, it goes through a named helper rather than an inline cast,
  so the eventual decision is a change in one place. The four `Copy_Out_Wide`/`Narrow` helpers in
  `platform_win32_locale.cpp` are the existing precedent and should be reused rather than duplicated.
* This is only correct while the output is ASCII, which is all these functions currently produce.
  `widechar-fallout.md` remains the place where the real decision gets made.

## 5. A macOS measurement baseline — **add one, but keep the Linux gate authoritative until it has proven itself**

**Question**: `check-probe-baseline.py` correctly refuses to compare AppleClang 16 figures against
clang-14 baselines, so macOS has no ratchet of its own.

**Answer, verbatim:** *"eh yes probably worth it but lets not turn off the old one until its
successful"*

Consequences:

* A second, macOS-specific baseline file is added and gated **advisory-only** at first: it reports and
  uploads, and does not fail the build.
* The Linux clang-14 baselines stay the blocking gate. Nothing about them is relaxed.
* The macOS gate is promoted to blocking only after it has been green across enough runs to show it is
  measuring the tree rather than the runner. Until then, a macOS regression is a message, not a stop.
* The baselines must record the compiler and SDK, because the existing refusal-to-compare behaviour is
  the feature that caught this in the first place.

## 6. The window/input backend the build selects — **SDL2 off Apple, Cocoa on macOS**

**Question** (forced by the slice that made `--strict-link` produce an executable): both backends
were in the tree and the native harness built **neither**, so 24 window-seam entry points were
unresolved as a matter of configuration. A link line cannot stay undecided once it has to produce a
file: something has to define `WWPlatform::Window_Create`.

**Answer: the harness chooses the backend the real build chooses — `platform_window_sdl2.cpp`
everywhere except Apple, `platform_window_cocoa.mm` on macOS.** This is not a new arrangement; it is
`CORE_WWLIB_WINDOW_BACKEND` in `Core/Libraries/Source/WWVegas/WWLib/CMakeLists.txt`, which the
measurement harness was the only build not honouring.

Consequences:

* The measured platforms differ in *which* backend is exercised, so a seam function that compiles
  only under SDL2 is not proven by the Linux gate. That asymmetry is real and is the reason both
  backends keep their own scan-code and seam-wiring gates (`check-window-scancodes.py`,
  `check-window-seam-wiring.py`), which compare them against each other rather than against a build.
* The Linux gate now needs `libsdl2-dev` present, and the harness links `libSDL2-2.0.so.0` **by
  path**: a box without SDL2 gets the honest unresolved list instead of a silently different link.
* Cocoa is Objective-C++, so the harness compiles `.mm` and CMake's `OBJCXX` language is enabled
  only when a `.mm` source is actually selected — a Linux configuration must not require an
  Objective-C++ compiler to exist.
* macOS keeps a second, advisory backend gate (decision 5): the SDL2 path is what the blocking
  Linux gate measures, and the Cocoa path is measured on the macOS runners.
* SDL2 is a dependency of an executable on Linux, not a shipping decision for macOS: the intended
  Apple Silicon artefact is the Cocoa one, and nothing here makes SDL2 a runtime requirement of it.

## 7. FFmpeg: link it, or compile the video path out — **link it, at the version the headers come from**

**Question**: the video path (`RTS_BUILD_OPTION_FFMPEG`, default `OFF` in
`cmake/config-build.cmake`) compiles in the harness and left 29 `av_*`/`sws_*`/`swr_*` symbols
unresolved, because nothing provided the libraries. Link them, or exclude the video path?

**Answer: link them.** The video path is the engine's own route and the reason the Bink SDK was not
needed; excluding it would make the executable a different program from the one the port is aiming
at, and would move the 29 symbols out of the measurement without answering anything.

Consequences:

* `fetch-probe-deps.sh` builds the shared libraries **from the tag `vcpkg-lock.json` pins** (n7.1.1),
  not from the distribution's packages. Ubuntu 22.04 ships libavcodec 58 against headers the engine
  compiled at 61: that link resolves every symbol and then disagrees on struct layouts, which is a
  measurement lie of exactly the kind this slice must not produce.
* The harness's configuration is deliberately narrow — Bink and `.binka` decoders, the demuxers the
  game's media needs, `swscale`/`swresample` — and this is a property of the *harness*, not of the
  port: the real build gets vcpkg's full-featured FFmpeg. `--disable-x86asm` (used when no assembler
  is installed) costs decode speed and changes no API.
* Decoding a real `.bik` is not proven by linking, and no figure in `docs/porting/` should be read as
  claiming it. `startability.md` says what the executable does and does not demonstrate.
* `SKIP_FFMPEG_LIBS=1` skips the build for the CI jobs that compile without linking; the link then
  reports the 29 symbols honestly rather than pretending they are resolved.

## Consequence for the font seam (not itself a listed decision)

The 18 `HFONT` compile failures have no Windows behaviour to "match" in the sense of decision 2 —
there is no GDI off Windows. Applying the same principle: the *output* must match GDI closely enough
that the WND GUI's authored layout numbers still line up (glyph advance widths and line height are
what the layout consumes), while the *implementation* should be the generalisable one rather than one
native API per platform. That points at a single portable rasteriser used on every non-Windows
platform, with GDI's metrics as the reference to compare against, and it means the acceptance
criterion is a metrics comparison against the Windows build — not "text appears".

That is what was built: `docs/porting/gdi-font-seam.md`, one stb_truetype rasteriser under the GDI
names themselves, gated by `scripts/ci/check-font-metrics.py` against recorded GDI metrics.
