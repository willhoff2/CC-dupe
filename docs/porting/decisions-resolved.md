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
