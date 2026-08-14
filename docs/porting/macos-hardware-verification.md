# macOS hardware verification: what a real Apple Silicon Mac says

Everything in the repo's macOS path was written on Linux and checked on a paravirtualised
`macos-15` GitHub runner with no display, scale 1 and no GPU. This document is the first run on a
real machine with a real display and a real Metal driver. It separates what was **measured** from
what is **inferred**, and it ends with what a Mac still did not verify.

Measured revision: `0ed283a4db5bc2bb19996cba3ac6b4730aaa8e54`
(`build(port): Measure the native build through level 3 and link lzhl and zlib (#44)`), i.e.
**before** PR #45, which is the file API seam and is *not* merged. Everything about file enumeration
below is therefore the pre-#45 state.

## 0. The machine and the toolchain, as reported by the tools that ran

| Thing | Value | How it was read |
|---|---|---|
| OS | macOS 26.6.1, build 25G76, Darwin 25.6.0 | `sw_vers`, `uname -a` |
| Architecture | `arm64`, target `arm64-apple-darwin25.6.0` | `clang++ -v` |
| Compiler | AppleClang 16.0.0.16000026, `/Applications/Xcode.app/.../usr/bin/clang++` | `clang++ --version` |
| CMake | 4.4.2 | `cmake --version` |
| GPU | Apple M1 Pro, Metal 4 | `MTLCreateSystemDefaultDevice()`, `system_profiler` |
| Vulkan | loader/headers 1.4.357, `/opt/homebrew/lib/libvulkan.dylib` | `vulkaninfo`, the spike |
| MoltenVK | 1.4.2, `driverVersion` 0.2.2210, `apiVersion` 1.1.357 | `VkPhysicalDeviceDriverProperties` in the spike |
| Display | built-in Liquid Retina XDR, 3456x2234 backing pixels, 1728x1117 points, `backingScaleFactor` 2.00 | `NSScreen` |

The Cocoa backend and the window check build natively for arm64 with this compiler, `-Wall
-Wextra`, no warnings from the backend:

```
spikes/renderer/tools/macos-window-check.sh                 # build + 240 frames + pixel check
```

## 1. The window (measured)

`spikes/renderer/src/macos_window_metrics.mm` (new, this slice) drives the real seam and prints
what AppKit, Core Animation, Vulkan and the Window Server say at each phase.

```
window     frame 800x628 at (464,357), contentRect 800x600, title "Zero Hour macOS metrics"
state      visible yes, key yes, main yes, level 0, styleMask 0xF, occlusion visible
ordering   orderedIndex 0 of 1 on-screen window(s), app active yes, menu bar visible yes
```

Ordering and occlusion come from `CGWindowListCopyWindowInfo`, i.e. the Window Server's own list,
not from the process's belief about itself: the window is on-screen, front of its app, and
`NSWindowOcclusionStateVisible`. The title is the string the seam was given. A
`Window_Set_Mode(1024x768)` resize is survived: `contentRect` becomes 1024x768, one `resize` event
is delivered, the swapchain is rebuilt and 240 further frames present.

**Not measured: a screenshot.** `screencapture` fails with `could not create image from display` —
the worker process has no Screen Recording (TCC) grant and cannot grant itself one. The prompt asked
for screenshots and there are none; the substitutes are the Window Server's ordering/occlusion
record above and the in-process `Read_Back_Color_Target()` PNGs in section 4, both of which are
pixels that came back off the GPU, and neither of which is a photograph of the desktop.

## 2. Retina: the classic half-resolution bug is not present (measured)

The runner is scale 1, so none of this arithmetic had ever executed. On a `backingScaleFactor` 2.00
display, in three sizes:

| Phase | Content points | `contentsScale` | `CAMetalLayer.drawableSize` | Vulkan `currentExtent` | backing/point |
|---|---|---:|---|---|---|
| windowed as created | 800x600 | 2.00 | 1600x1200 | 1600x1200 | 2.00 x 2.00 |
| after `Window_Set_Mode(1024x768)` | 1024x768 | 2.00 | 2048x1536 | 2048x1536 | 2.00 x 2.00 |
| borderless fullscreen | 1728x1117 | 2.00 | 3456x2234 | 3456x2234 | 2.00 x 2.00 |

The swapchain extent tracks the **backing store**, not the point size, in every phase, and
`convertRectToBacking:` agrees with `drawableSize` to the pixel. So it is neither a
half-resolution nor a quarter-area render; it is a full-resolution one.

**But `Window_Client_Size()` reports points** — 800x600, 1024x768, 1728x1117 — while the
framebuffer the renderer draws into is twice that in each axis. On Windows, client pixels, the
back-buffer and the mouse coordinate space are one and the same. Here they are not, and section 3
shows the consequence.

## 3. Input (measured, with synthetic events)

`kVK_*` -> set-1 translation, checked against the repo's own `KeyScanCodes.h` constants at runtime
in the same process that received the event (not against a table copied into the test):

| Key | `kVK` | Set-1 scan code received | Expected |
|---|---|---|---|
| A | 0x00 | 0x1E | 0x1E |
| Z | 0x06 | 0x2C | 0x2C |
| 1 | 0x12 | 0x02 | 0x02 |
| Space | 0x31 | 0x39 | 0x39 |
| Return | 0x24 | 0x1C | 0x1C |
| Tab | 0x30 | 0x0F | 0x0F |
| Left arrow | 0x7B | 0xCB | 0xCB |
| Up arrow | 0x7E | 0xC8 | 0xC8 |
| F1 | 0x7A | 0x3B | 0x3B |
| Escape | 0x35 | 0x01 | 0x01 |
| Shift+A | 0x00, flags 0x20000 | 0x1E, modifiers 0x1 | 0x1E, shift set |

Key-up is delivered for each, and `A` also produced `text U+0061`, so the text path is live.

Mouse, in client coordinates with the engine's top-left origin:

| Injected | Received |
|---|---|
| client (10,10) in the 800x600 window | (10,10) |
| client (400,300) plus a left click | (400,300), `mouse-down` then `mouse-up`, button 0 |
| client (799,599), the bottom-right pixel | (799,599) |
| client (512,384) and (1023,767) after the resize to 1024x768 | (512,384), (1023,767) |
| client (10,10) and (1727,10) in fullscreen | (10,10), (1727,10) |
| a `windowNumber == 0` event at screen (664,807), which stands for client (200,150) | (200,150) |
| scroll wheel | delta 120, i.e. one Win32 `WHEEL_DELTA` |

**These are synthetic `CGEvent`/`NSEvent` injections into the app's own window, not a human
pressing keys.** They exercise the same `Translate_Event` code a real event does — the seam cannot
tell the difference — but a physical keyboard and mouse remain untried, and so does a non-US layout.

Two things were wrong and are fixed in this branch (both cheap, both clearly wrong):

1. **`windowNumber == 0` mouse events were misplaced.** `locationInWindow` is in *screen*
   coordinates for a window-less event, and the seam treated it as window-local. A deterministic
   event standing for client (200,150) arrived as (664,-207) — off-screen, negative. Fixed by
   converting through `convertRectFromScreen:` when `[event window] == nil`, then measured again:
   (200,150).
2. **fullscreen did not actually cover anything** — see section 5.

**Structural, not fixed here: mouse coordinates are points, the framebuffer is pixels.** In the
800x600 window the mouse space is 0..799 x 0..599 while the render target is 1600x1200. Every
cursor hit-test, UI click and drag box the game computes in "client pixels" is therefore at half
the scale of what it drew, on any Retina display. Three ways out, none free:

* scale mouse coordinates by `backingScaleFactor` in the seam. One line, and it makes the mouse
  space match the framebuffer — but then `Window_Client_Size()` still reports points, so half the
  call sites still disagree;
* make `Window_Client_Size()` report backing pixels, so points never leave the backend. This is the
  Windows-equivalent model and the one I would pick, but it means auditing every existing consumer
  of the size (window creation, mode change, the renderer's viewport, the UI's layout) rather than
  one function;
* set `contentsScale = 1` and render at point resolution — a blurry 1x game on a 2x display, cheap
  and wrong.

Which one is right depends on the renderer slice's coordinate contract, so it is written up rather
than forced. Nothing in the engine is running yet (section 6), so no gameplay observation supports
or contradicts the size of this problem.

## 4. Real Metal, not lavapipe (measured)

Surface path, on the real driver (`spikes/renderer/src/metal_surface_probe.mm`):

```
PASS metal-device MTLCreateSystemDefaultDevice(): Apple M1 Pro
PASS surface vkCreateMetalSurfaceEXT: VK_SUCCESS
PASS physical-device 1 device(s); first is Apple M1 Pro
PASS surface-support queue family 0 presents; 60 surface format(s), 2 present mode(s)
PASS swapchain vkCreateSwapchainKHR: VK_SUCCESS
PASS present acquired image 0 and vkQueuePresentKHR: VK_SUCCESS
```

The render gate, with the Khronos validation layer loaded (confirmed with `VK_LOADER_DEBUG=layer`,
not assumed from a zero exit status):

```
python3 scripts/ci/check-spike-render.py \
  --binary build/spike/zh-renderer-spike \
  --reference spikes/renderer/docs/spike-triangle.png \
  --out /Users/willhoff/devin-work/artifacts/spike-macos-hw.png \
  --tolerance 8 --max-differing 0.01
```

```
device: Apple M1 Pro
centre pixel rgba = 131,152,170,255
validation messages: 0
readback vs spikes/renderer/docs/spike-triangle.png:
0/480000 pixels (0.0000%) differ by more than 8/255; worst channel delta 1 at (400, 76)
OK: spike rendered the reference image with the validation layer active and silent
```

The gate's tolerance is 8/255, which on its own would let a visibly different image through, so the
same PNG was also compared strictly and channel by channel:

| Measure | Value |
|---|---|
| worst channel delta, any pixel | **1**/255, at (400,76) |
| mean absolute channel delta | **0.0136**/255 |
| pixels differing at all | 15462 / 480000 (3.22%), all by exactly 1 |
| re-run with `--tolerance 2 --max-differing 0` | passes |

So MoltenVK on an M1 Pro reproduces the Linux/lavapipe reference to within one least-significant
bit on 3% of pixels and exactly elsewhere. That is a rasterisation-parity statement about *this
scene* (two draws, 2 pipelines) on *this* GPU, not about the renderer in general.

Companion probes on the real driver: `zh-feature-probe` 0 failures, `zh-fixedfunc-tests` 0 failures
(6 pending), `zh-resource-lock-tests` 0 failures 0 skipped, `zh-caps-probe` exit 0.

## 5. Borderless fullscreen, the menu bar, the Dock and the notch (measured)

The game's fullscreen is a screen-sized `WS_POPUP`. What the seam did before this branch, measured:
the window took the screen's frame but stayed at `NSNormalWindowLevel` with default presentation
options, so **the Dock and the menu bar drew on top of it** — the Window Server listed `Dock id 57
layer 20 ... overlaps ours, in front` while our window sat at layer 0.

Fixed here by doing what a borderless full-screen app has to do explicitly: hide the Dock and the
menu bar through `NSApp.presentationOptions`, and raise the window above the menu bar level. After
the fix, in fullscreen:

```
window     frame 1728x1117 at (0,0), contentRect 1728x1117
state      visible yes, key yes, main yes, level 25, styleMask 0x0
ordering   orderedIndex 0 of 1 on-screen window(s), menu bar visible no
presentation  NSApp presentationOptions 0xA   (HideDock | HideMenuBar)
drawableSize / vk currentExtent  3456x2234
```

and returning to windowed restores level 0, styleMask 0xF, `presentationOptions 0x0` and a
1600x1200 drawable — the restore path is measured, not assumed, because leaving the Dock hidden
after a mode change would be a user-visible bug in its own right. Window destruction restores the
presentation options too, so a crash-free exit does not leave the desktop without a Dock.

**The notch.** In fullscreen the window's frame is the *whole* screen, 1728x1117 at (0,0), and the
window's own `safeAreaInsets` are all 0 — borderless windows get none. The screen, meanwhile,
reports `safeAreaInsets.top = 32` and `auxiliaryTopLeftArea 771x32 at (0,1085)`. So the top 32
points of the game's framebuffer, across the middle 771 points, are **behind the camera housing**
on this display. The engine draws its top-of-screen UI there. This is measured geometry, and the
inference is the obvious one: on a notched Mac a screen-sized borderless window loses that strip
unless the game either insets to the safe area or opts into the "content fills the notch area"
behaviour deliberately. Not fixed: which of those is right is a UI-layout decision, not a seam bug.

## 6. Retail assets on the pre-#45 seam (measured)

Retail Zero Hour data is present on this machine at `~/devin-work/zh-data`, treated as read-only
input. `spikes/assets/src/path_probe.cpp` (new) compiles the *real*
`WWLib/platform/platform_path.cpp` — the `FindFirstFile`/`FindNextFile` seam — and points it at
that directory.

```
./build/assets/zh-path-probe ~/devin-work/zh-data
```

| Call | Result |
|---|---|
| `Enumerate(dir, "*")` | 53 entries |
| `Enumerate(dir, "*.big")` | 20 |
| `Enumerate(dir, "*.BIG")` | 20 — same set; Win32 patterns are case-insensitive and `FNM_CASEFOLD` reproduces that |
| `Enumerate(dir, "*big")` | 20 (superset of `*.big`, coincidentally equal here) |
| `Enumerate(dir, "Speech*.big")` | 2 — a prefixed pattern narrows correctly |
| `Has_Match(dir, "*.big")` / `"*.BIG"` | true / true |
| `Has_Match(dir, "*.no-such-extension")` | false |
| `Exists()` on exact / UPPER / lower / backslash-separated spellings of an archive | all four resolve |
| `Resolve("<dir>\Data")` | `<dir>/Data` |
| `Enumerate("<dir>\Data\Cursors", "*.ani")` | 52 entries — a backslash-separated directory works |
| `Enumerate("<dir>\Data\Cursors", "*.no-such-extension")` | false, matching `FindFirstFile`'s `INVALID_HANDLE_VALUE` for an empty match set |

All 12 checks pass. **The caveat is the important part:** this volume is APFS
case-*insensitive* (verified directly — `AbC.txt` and `abc.txt` collapse to one file), and the probe
reports it:

```
volume: case insensitive - the literal spelling opens, so Resolve()'s case folding walk is never reached
```

So the shouty and lower-case spellings above prove that *macOS* tolerates them, and prove nothing
about `Resolve()`'s component-by-component case-folding fallback, which is the code Linux depends
on. A case-sensitive APFS volume would exercise it; this Mac cannot.

The archives themselves also settle the case question for their contents. Reading all 20 with
`zh-asset-inspect`:

* 20/20 archives parse, entry table sums match file sizes, `ok` for each (12,280 entries total);
* **0 entry names collide only by case** across all 12,280, so extracting the retail tree onto a
  case-sensitive filesystem loses nothing;
* 12,277 of 12,280 entry names use `\` as a separator, none use `/` — the separator translation is
  the engine's job on every platform, not something the OS ever does for it;
* 14 names appear in more than one archive, which is the patch-precedence behaviour, not a defect.

**This is archive parsing and path lookup, not gameplay.** Nothing here says the game can load a
map, and nothing here should be read as saying so.

## 7. What a launch attempt actually does (measured)

There is no native executable to launch, and that is the measured boundary rather than a limitation
of the attempt. `scripts/native-build.py` builds the platform-independent libraries natively and
runs the real linker; its own docstring says "a playable binary is not a goal". Running it on this
Mac through level 3 with the shims is the closest thing to a launch that exists at this revision:

```
arch -arm64 /usr/bin/python3 scripts/native-build.py --level 1 --level 2 --level 3 --with-shims \
    --jobs 8 --build-dir build/native-macos-arm64 \
    --json  /Users/willhoff/devin-work/artifacts/native-build-macos-arm64.json \
    --report /Users/willhoff/devin-work/artifacts/native-build-macos-arm64.md
```

```
== compiling 829 translation units
   739 objects, 90 failures
   no archive for: GeneralsMD/Code/Main (every unit failed)
== linking 9 archives (+1 third-party, zlib: no)
objects 739/829, probe-clean 739, probe-clean-but-uncompilable 0, undefined symbols 794
```

Host `arm64-apple-darwin25.6.0`, `Apple clang version 16.0.0`, archives confirmed arm64 with `lipo`.
The boundary is the *link*, not a crash: `link_binary_produced` is `false`, no `main` exists, and the
first thing a launch would need is a device layer (renderer, audio, video), which is other slices'
work. Per the instruction not to chase this, no crash was fixed and no partial entry point was
hacked together to produce a launch-shaped artefact.

### 7.1 The harness reported "0 failures" on macOS, and that was wrong (measured, then fixed)

The first run of this on the Mac printed `829 objects, 0 failures` and `linking 3 archives`, and
`build/native-macos-arm64` contained 742 object files and no archive for WWLib, `Core/GameEngine` or
`GeneralsMD/Code/GameEngine`. `build()` in `scripts/native-build.py` detected failures by scraping
Ninja's `FAILED: <obj>` lines from the build log. This Mac has no `ninja`, so CMake used the Unix
Makefiles generator, whose failures read `make[3]: *** [<obj>] Error 1` — the regex matched nothing,
every compile failure was invisible, and the run reported a clean compile it had not achieved. The
fix in this branch asks the question generator-independently: a translation unit whose object file
is absent after the build failed. Any macOS number produced by this script *before* this fix should
be distrusted; the numbers above are from after it.

### 7.2 macOS-only compile failures against the checked-in baseline (measured)

`docs/porting/ci-baselines/native-build-shimmed-level1-2-3.json` (Linux, clang 14) versus this run:

| | Linux baseline | this Mac |
|---|---|---|
| translation units | 829 | 829 |
| objects | 748 | 739 |
| compile failures | 81 | 90 |
| archives | 9 | 9 |
| undefined symbols | 393 | 794 |
| link produced a binary | yes | no |

Every one of the 81 Linux failures also fails here, plus **9 macOS-only** ones. They are not random:

| translation unit | diagnostic |
|---|---|
| `WWLib/FastAllocator.cpp` | `'malloc.h' file not found` |
| `WWLib/TARGA.cpp` | `'malloc.h' file not found` |
| `WWLib/ini.cpp` | `'malloc.h' file not found` |
| `WWDebug/wwmemlog.cpp` | via `WWLib/FastAllocator.h:42`, `'malloc.h' file not found` |
| `WWDebug/wwprofile.cpp` | via `WWLib/FastAllocator.h:42`, `'malloc.h' file not found` |
| `WWLib/regexpr.cpp` | `unknown type name 'reg_syntax_t'` |
| `Core/GameEngine/Source/Common/INI/INI.cpp` | `call to deleted function 'from_chars'` |
| `W3DDevice/.../Draw/W3DModelDraw.cpp` | not captured by the harness |
| `GeneralsMD/.../Shadow/W3DBufferManager.cpp` | not captured by the harness |

Three distinct causes, none of which a Linux run can see:

1. `<malloc.h>` is glibc-only. macOS has `<malloc/malloc.h>`, and `memalign`/`malloc_usable_size`
   have different spellings there. Five of the nine collapse into one include in
   `WWLib/FastAllocator.h:42`;
2. `reg_syntax_t` is a GNU `<regex.h>` extension. BSD/macOS `<regex.h>` has POSIX only, so
   `regexpr.cpp` needs either a bundled GNU regex or a POSIX rewrite;
3. `std::from_chars` on libc++ 16 rejects the argument type libstdc++ 14 accepted.

**Inference, not measurement:** (1) looks like a one-line platform guard per site plus an allocator
shim for the glibc-only entry points; (2) and (3) are real work, not spellings. None of this is in
this slice's seam and none of it was touched here — the count above is the deliverable. The two
failures whose diagnostic the harness did not record are listed as unrecorded rather than guessed:
reconstructing their command line outside the build produced a *different* error (`'d3d8.h' file not
found`) which is an artefact of the reconstruction, so it is not reported as their cause.

## 8. The rest of the verification ladder, run on this Mac (measured)

| Gate | Result on this machine |
|---|---|
| `flake8 --max-line-length=100 scripts/` | 914 violations, **0** of them in a file this branch touches; the touched files are clean |
| `actionlint .github/workflows/*.yml` | non-zero, all pre-existing: ShellCheck `SC2086`-class notes plus an undefined `needs.detect-changes` in `ci.yml`. No workflow was edited here |
| `./scripts/ci/fetch-probe-deps.sh` | provisions `dx8-src`, `gamespy-src`, `lzhl-src`, `miles-src` — after the BSD-grep fix below; it could not run on macOS at all before |
| `native-port-probe.py` native / shimmed | runs; 637/744 and 679/744. `check-probe-baseline.py` refuses the comparison, correctly — see section 9 item 8 |
| `check-d3d8-surface.py` | pass |
| `check-openal-symbols.py` | 101 declared, **101 defined**, pass — after the Mach-O fix below. Needed `brew install openal-soft` and `-I$(brew --prefix openal-soft)/include`, since macOS ships `OpenAL/al.h`, not `AL/al.h` |
| `audio-surface-scan.py --check` | pass, 101 entry points/prototypes, 10 unreferenced definitions |
| `native-layout-test.py` | pass, all three checks: LP64 reference clean, the ILP32 (`-m32`, `arm-apple-darwin`) reference clean, negative control fails in 221 layout assertions |
| `xfer-blob-audit.py` | exit 0 |
| `window-input-scan.py --check` | pass, 656 references across 24 `HWND` files |
| `check-spike-render.py` | pass on the real Metal driver with the validation layer loaded and silent — section 4 |

The `check-openal-symbols.py` result was a **failure until a portability fix**: it reported all 101
AIL_* entry points undefined, because Mach-O prefixes every C symbol with `_`, so `nm` prints
`_AIL_startup` where ELF prints `AIL_startup`. Nothing was wrong with the audio backend; the gate
was ELF-shaped. This is the same class of bug as section 7.1 and is the third one found this way.

## 9. What this hardware still did NOT verify

1. **screenshots of anything** — no Screen Recording grant for the worker process
   (`screencapture` -> `could not create image from display`). Window Server ordering and GPU
   read-backs are what stand in;
2. **human input.** Every keystroke and mouse movement above was injected by the test process.
   A physical keyboard and mouse, and any non-US layout, are untried;
3. **`Resolve()`'s case-folding fallback**, masked by this volume being case-insensitive;
4. **the mouse-points-versus-framebuffer-pixels mismatch's actual gameplay effect** — there is no
   running game to observe it in;
5. **an external or non-Retina display, a scale change, a monitor change** and
   `VK_ERROR_OUT_OF_DATE_KHR` from the compositor;
6. **cursor clipping semantics.** `Window_Set_Cursor_Clip` uses
   `CGAssociateMouseAndMouseCursorPosition`; nothing here checked what that feels like to a user;
7. **performance.** Not one frame-time number in this document is a claim about playability;
8. **the probe baselines.** `check-probe-baseline.py` correctly refuses to compare: the checked-in
   baselines were measured with clang 14 and this machine has AppleClang 16 (native 637/744 vs
   baseline 643/744, shimmed 679/744 vs 687/744). The differences are AppleClang-16 and
   macOS-SDK effects — missing `malloc.h`, `commctrl.h`, `gscommon.h` — and **not** a regression
   measurement. Comparing macOS to those baselines needs a macOS baseline file, which is a decision
   for whoever owns the CI gate;
9. **the two macOS-only compile failures in section 7.2 whose diagnostic was not captured**
   (`W3DModelDraw.cpp`, `W3DBufferManager.cpp`) — measured as failing, cause unrecorded;
10. **anything about gameplay, Wine, the tools, online play or replays** — all out of this slice's
    scope.

## 10. Honest note on scope

The verification playbook says to fix nothing and hand back. The task overrode that for things
"cheap and clearly wrong in the seam", so this is not a pure pre-change verification: the
`windowNumber == 0` mouse fix and the fullscreen presentation/level fix are in the same branch as
the measurements, and the measurements in sections 3 and 5 were taken both before and after
(`metrics-before-fix.log`, `metrics-after-fix.log`). Two verification-harness portability fixes are
also here, and are not engine changes: `fetch-probe-deps.sh` used GNU-only `grep -oP`, which BSD
grep rejects, and `native-layout-test.py`'s negative control looked for clang 14's exact
`static_assert failed` wording, which AppleClang 16 spells `static assertion failed due to
requirement`. Both made a gate fail for a reason that had nothing to do with the code under test.
`native-build.py` has two more of the same kind: its link probe passed GNU `ld` flags
(`--whole-archive`, `-lstdc++`, `-ldl`) that `ld64` does not accept, so the link could not run on
macOS at all, and its failure detection is the bug in section 7.1. `check-openal-symbols.py`
compared ELF-shaped symbol names against Mach-O's underscore-prefixed ones (section 8). `-m32` on
this host retargets to `arm-apple-darwin` and the ILP32 reference compiles there, so the layout gate
needed only the diagnostic-wording fix to pass on macOS.
