# Apple Silicon verification: measuring the port on the machine it is for

Everything merged since #101 was measured on Linux against lavapipe, and §3.1 of
`renderer-first-frame.md` records that the window spike's presentation path had never presented at
all — `SPIKE_WITH_PLATFORM_WINDOW` was defined on the executable rather than on the library holding
`vulkan_backend.cpp`, so every earlier "presented N frames" line, *including* the arm64 ones,
measured a draw and a readback and not a flip. This document therefore inherits no figure from any
earlier document or session. Every number below was taken on one real Mac, in one sitting, from a
build made on that Mac.

Five questions, five verdicts. Section 7 is the table of what real Apple Silicon now verifies
against what is still Linux-only; section 8 lists every defect found, classified.

There is **no photograph of the desktop anywhere in this document.** `screencapture` and
`CGWindowListCreateImage` both need a Screen Recording grant this worker does not have and cannot
grant itself, so the pixel evidence is in-process `Read_Back_Color_Target()` and the Window Server's
own `CGWindowListCopyWindowInfo` record. Where a claim would need a photograph, it is not made.

## 0. The machine, as the tools report it

| Thing | Value | Read by |
|---|---|---|
| OS | macOS 26.6.1, build 25G76, `Darwin 25.6.0 arm64` | `sw_vers`, `uname -mrs` |
| Rosetta | not translated: `sysctl.proc_translated` = `0` | `sysctl -n sysctl.proc_translated` |
| Compiler | Apple clang 21.0.0 (clang-2100.1.1.101), host `arm64-apple-darwin25.6.0` | the build harness's own JSON |
| GPU | `Apple M1 Pro`, driver `/opt/homebrew/Cellar/molten-vk/1.4.2/lib/libMoltenVK.dylib` | Vulkan loader debug output of the engine run |
| Validation layer | `VK_LAYER_KHRONOS_validation` 1.4.357.0, **loaded** (see §2.3) | loader debug output |
| Display | 1728x1117 points, `backingScaleFactor` 2.00 | `CGWindowListCopyWindowInfo` + `NSScreen` |
| Retail data | the user's Steam depot, symlinked into a run directory outside the repo | — |

## 1. Q1 — does the tree still build here?

```sh
python3 scripts/native-build.py --level 1 --level 2 --level 3 --level 4 \
  --with-shims --strict-link
```

Raw tail of the harness:

```
== compiling 979 translation units
   979 objects, 0 failures
== re-running the probe over the same translation units
== re-building without the failed translation units
   dropped 5 test-tool entry points from the archives
== linking 14 archives (+5 support, zlib: yes, openal: yes, ffmpeg: yes, render backend: yes,
   window backend: platform_window_cocoa.mm)
   entry point from libgeneralsmd_code_main (`_main`, no stub main)
== linking strictly (no --warn-unresolved-symbols)
   strict link succeeded: 0 unresolved symbol(s), binary produced: yes
   build/native/native_strict_link: 26.4 MiB, Mach-O 64-bit arm64
objects 979/979, probe-clean 979, probe-clean-but-uncompilable 0, undefined symbols 0
  library-not-linked 0, cut-scope-not-linked 0, compile-blocked 0, harness-artefact 0,
  no-definition-anywhere 0
```

From the harness's JSON, the parts that only a Mac can answer:

| Field | Value |
|---|---|
| `objects` / `translation_units` | 979 / 979, `probe_clean` 979 |
| `undefined_total` | 0 |
| `strict_link.unresolved_total` | 0 |
| `strict_link.agrees_with_nm` | `true`, with `only_in_linker_report` and `only_in_nm_scan` both empty |
| `symbol_prefix` | `_` |
| `strict_link.linker_name_form` / `symbol_name_form` | `mangled` / `demangled` |
| `system_symbols_discounted` | 12756, from 50 `.tbd` sources |
| `strict_link.binary` | `Mach-O 64-bit executable arm64`, `lipo_archs: ["arm64"]`, `macho_filetype` 2 |
| `host_translated` | `false` |

and independently of the harness:

```
$ lipo -archs build/native/native_strict_link
arm64
$ file build/native/native_strict_link
build/native/native_strict_link: Mach-O 64-bit executable arm64
$ sysctl -n sysctl.proc_translated
0
```

**Verdict: yes.** The two fixes that were written on Linux and had never been compiled by a Mach-O
toolchain both work here and both are load-bearing:

- the `_` symbol prefix and the mangled-in-the-linker/demangled-in-`nm` name forms are what make
  `agrees_with_nm` true — the two independent views of the unresolved set agree exactly, which is
  the only reason `unresolved_total: 0` means anything;
- the `.tbd` discount list accounts for 12756 system symbols from 50 `.tbd` files. Without it those
  would be counted as unresolved port work, and the level 1..4 configuration would report a
  five-figure backlog rather than zero.

## 2. Q2 — does MoltenVK present the frame #101 measured on lavapipe?

### 2.1 The archive that the engine actually links

```sh
$ python3 scripts/ci/check-swapchain-compiled.py --archive build/native/libsupport_renderbackend.a
OK: libsupport_renderbackend.a references vkCreateSwapchainKHR, vkAcquireNextImageKHR,
vkQueuePresentKHR -- the backend the engine links has a swapchain
```

This is the check that §3.1's defect motivated: the presentation entry points are in the archive the
link consumes, not merely in a translation unit some other target compiled. The harness prints the
same three symbols when it links the run:

```
   swapchain compiled in: vkCreateSwapchainKHR, vkAcquireNextImageKHR, vkQueuePresentKHR
```

### 2.2 The frame

`scripts/native-render-backend-run.py` drives the engine's own `DX8Wrapper`/`RenderBackendClass`
seam — `WW3D::Set_Render_Device`, then `Begin_Scene`/`Clear`/`End_Scene` with a flip — on this
machine, with the ICD pointed at MoltenVK:

```
== WW3D::Set_Render_Device -> CreateDevice + one-time inits
WW3D::Set_Render_Device                    ok

== 3 frames of Begin_Scene / Clear / End_Scene with a flip
frame 0: submitted
frame 1: submitted
frame 2: submitted
frames submitted                           ok
frames actually presented                  ok  (FrameCount advanced 3, device lost no)

== what was in the frame (read back from the colour target)
800x600, 480000/480000 pixels within 2 of the clear colour 26,51,115
centre pixel rgba = 25,51,114,0; channel range r 25..25 g 51..51 b 114..114

== the unimplemented-call ledger (each entry is a finding, not a fallback)
(empty: every D3D8 entry point the engine reached is implemented)

validation messages: 0
validation layer silent                    ok
```

Against #101's lavapipe numbers: same geometry (800x600), same matching-pixel count
(480000/480000 within 2 of 26,51,115), same centre pixel `25,51,114,0`, same per-channel range
`r 25..25 g 51..51 b 114..114`. **The delta is zero — not "within tolerance", identical.** The 26 →
25 and 115 → 114 offsets are the sRGB-write rounding that #101 also recorded, and they land on the
same values on Metal as on lavapipe.

### 2.3 The layer was really loaded

The skill's warning is that an unloaded validation layer makes a silent run prove nothing. From the
loader's own debug output for that run:

```
[Vulkan Loader] INFO | LAYER:   Insert instance layer "VK_LAYER_KHRONOS_validation"
    (/opt/homebrew/Cellar/vulkan-validationlayers/1.4.357.0/lib/libVkLayer_khronos_validation.dylib)
[Vulkan Loader] INFO | LAYER:   Inserted device layer "VK_LAYER_KHRONOS_validation" (...)
[Vulkan Loader] DRIVER:                Using "Apple M1 Pro" with driver:
    "/opt/homebrew/Cellar/molten-vk/1.4.2/lib/libMoltenVK.dylib"
```

### 2.4 The spike's own gates, on this GPU

The `renderer-spike-macos` CI job runs against a paravirtualised Metal device and asserts only that
`vkCreateInstance` succeeds and the readback is loosely within tolerance. Here the same gates ran
against a real M1 Pro:

```
$ python3 scripts/ci/check-spike-render.py --binary build/spike-mac/zh-renderer-spike \
    --reference spikes/renderer/docs/spike-triangle.png --out spike-mac.png
centre pixel rgba = 131,152,170,255
validation messages: 0
readback vs spikes/renderer/docs/spike-triangle.png: 0/480000 pixels (0.0000%) differ by more
than 2/255; worst channel delta 1 at (400, 76)
OK: spike rendered the reference image with the validation layer active and silent

$ build/spike-mac/zh-feature-probe                     -> 0 case(s) failed, validation messages: 0
$ build/spike-mac/zh-fixedfunc-tests --validation      -> 0 failed, 6 pending, 0 validation msgs
$ ZH_SPIKE_NO_VIEW_SWIZZLE=1 ... zh-fixedfunc-tests    -> 0 failed, 6 pending, 0 validation msgs
$ build/spike-mac/zh-resource-lock-tests --validation  -> 0 failed, 0 skipped, 0 validation msgs
$ ZH_SPIKE_NO_VIEW_SWIZZLE=1 ... zh-resource-lock-tests-> 0 failed, 0 skipped, 0 validation msgs
$ python3 scripts/ci/check-staging-cost.py --binary build/spike-mac/zh-staging-workload --self-check
    staging reuse rate                     0.9831       0.9000  ok
    device: Apple M1 Pro
  self-check: pre-pool (retain) mode -> the ceiling rejects it on: staging_allocations,
    staging_resident_bytes, staging_peak_bytes, staging_steady_state_bytes
OK: staging cost within the committed ceiling in both swizzle modes
$ python3 scripts/ci/check-d3d8-surface.py    -> OK: direct D3D8 call surface matches the allowlist
$ python3 scripts/ci/check-backend-coverage.py-> OK: backend coverage matches the committed baseline
$ python3 spikes/renderer/tools/d3d8-lock-scan.py --check     -> OK (100 sites, 9 classes)
$ python3 spikes/renderer/tools/surface-lock-audit.py --check -> OK: matches the committed audit
```

The reference PNG was rendered on lavapipe. Real Metal reproduces it with **no pixel differing by
more than 2/255 and a worst single-channel delta of 1** — so the fixed-function emulation is not
merely "close enough" on Apple Silicon, it is bit-near-identical to the Linux reference.

**Verdict: yes**, with one caveat that belongs to Q4: what is presented is a linear upscale of an
800x600 image into a 1600x1200 drawable (§4.2), so the flip is real but the resolution is not.

### 2.5 The one failure in this section

Running the gates through a Python wrapper on macOS silently loses the validation layer:

```
$ DYLD_LIBRARY_PATH=<layer dir>:<moltenvk dir> python3 scripts/ci/check-staging-cost.py ...
vulkan_backend.cpp:840: vkCreateInstance(&ci, nullptr, &instance_) failed with VkResult -6
FAIL: build/spike-mac/zh-staging-workload --frames 12 exited 1
```

`-6` is `VK_ERROR_LAYER_NOT_PRESENT`. The binary run directly from the shell with the same
environment succeeds. The cause is SIP: `DYLD_*` is stripped when a protected interpreter such as
`/usr/bin/python3` execs, so the recipe in `native-build.md` and in the `renderer-spike-verify`
skill — "set `DYLD_LIBRARY_PATH` to the validation-layer and MoltenVK lib directories" — works for a
directly launched binary and cannot work for anything the CI scripts launch. The fix that made every
gate above run is to give the loader an absolute library path instead of a `DYLD` hint:

```sh
# Homebrew's manifest names its dylib relatively, which is why DYLD_LIBRARY_PATH was needed at all.
python3 - <<'PY'
import json
d = json.load(open('/opt/homebrew/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json'))
d['layer']['library_path'] = '/opt/homebrew/Cellar/vulkan-validationlayers/1.4.357.0/lib/libVkLayer_khronos_validation.dylib'
json.dump(d, open('<some dir>/VkLayer_khronos_validation.json', 'w'), indent=2)
PY
export VK_LAYER_PATH=<some dir>            # no DYLD_LIBRARY_PATH needed
```

Verified to load through the wrapper afterwards:

```
[Vulkan Loader] DEBUG | LAYER:  Loading layer library
    /opt/homebrew/Cellar/vulkan-validationlayers/1.4.357.0/lib/libVkLayer_khronos_validation.dylib
```

Classified as **port defect (in the documented procedure, not in the engine)**; see §8.1. It is worth
fixing because in its failure mode the layer is absent, and a spike run with no layer reports
`validation messages: 0` for the same reason a run with a silent layer does.

## 3. Q3 — does the retail main menu render here?

### 3.1 The retail binary runs

```sh
cd <run dir>   # retail depot symlinked in; nothing retail inside the repo
VK_ICD_FILENAMES=/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json \
  ./zh -win -noshellmap -nologo -xres 800 -yres 600
```

It reaches the game loop and stays there for as long as it is left alone (90 s, then a second
process for the audio work in §5 that lived for the whole of that section). The Window Server sees a
real window:

```
window id=3138 owner=zh pid=89570 layer=0 alpha=1 onscreen=1
  bounds(points) x=464 y=131 w=800 h=632
  windows in front of it (all apps, on-screen, excluding desktop): 26
screen frame(points) 1728x1117 backingScaleFactor=2.00
```

Total diagnostic output of a full run, deduplicated:

```
   1 !!! GetSystemDirectoryA() is not implemented off Windows: there is no Windows system directory
   2 Decode_Fvf: unsupported FVF 0x0
   1 Copy_Rects: source and destination formats differ
```

### 3.2 The real `MainMenu.wnd` is live, not a synthetic stand-in

Walking `TheWindowManager`'s live window list in the running process (LLDB's Python `SBValue`
traversal, so no expression-evaluation artefacts) finds **207 windows** with retail names,
identifiers, rectangles and label strings. The six the user actually clicks, with the hit-test
result at each rectangle's own centre:

```
windows in the live MainMenu.wnd tree: 207
ButtonSinglePlayer  screenpos=(540,116) size=208x36 centre=(644,134) hit(centre)=true
ButtonMultiplayer   screenpos=(540,156) size=208x36 centre=(644,174) hit(centre)=true
ButtonLoadReplay    screenpos=(540,196) size=208x35 centre=(644,213) hit(centre)=true
ButtonOptions       screenpos=(540,236) size=208x36 centre=(644,254) hit(centre)=true
ButtonCredits       screenpos=(540,276) size=208x36 centre=(644,294) hit(centre)=true
ButtonExit          screenpos=(540,316) size=208x36 centre=(644,334) hit(centre)=true
```

These are the retail control names and the retail 640x480-authored layout scaled to the 800x600
window, read out of the live tree — not a fixture.

### 3.3 What is actually in the frame

`VulkanRenderBackendClass::Measure_Frame` was called in-process on the running game and wrote the
colour target to a PNG (the PNG is retail-derived and is **not** committed):

```
(bool) $0 = true
$proof = (Width = 800, Height = 600, Pixels = 480000, Matching = 610,
          MinRGB = {0,0,0}, MaxRGB = {255,255,255}, CentreRGBA = {216,5,218,129})
Validation_Message_Count() = 0
```

Pixels exist, they are not the clear colour (610 of 480000 are still background), and the frame is
recognisably the Zero Hour main menu — and it is **wrong**. What the readback shows:

- the "Command & Conquer Generals — Zero Hour" logo is **correct**: right colours, right position,
  clean edges. The texture path works for it;
- the six main-menu buttons are in the right places with the right blue frames, but every label is
  drawn **twice, horizontally offset** ("SOLO SOLO / PLAY PLAY", "EXIT EXIT / GAME GAME");
- everything behind the buttons is **magenta**, in rectangular blocks and bands, with speckle noise
  over part of it. The centre pixel `(216,5,218)` is that magenta. Magenta blocks plus noise is the
  signature of sampling an image whose contents were never written, or written in another format;
- the validation layer is silent throughout, so no *Vulkan* rule is being broken. Whatever is wrong
  is legal Vulkan being asked to do the wrong thing.

The two diagnostics in §3.1 name the mechanism precisely enough to fix without redoing this
measurement: `Decode_Fvf: unsupported FVF 0x0` (twice) is a vertex declaration the backend does not
translate, and `Copy_Rects: source and destination formats differ` is exactly the 2D blit the shell
uses to compose the background and the text — a `CopyRects` that declines to copy leaves the
destination as it was, which is the magenta-and-noise field, and a text pass that goes through it
twice at two offsets is how one label becomes two.

**Verdict: the retail menu loads and rasterises on this machine, and it does not render correctly.**
Q3 is a *no* on the render, a *yes* on everything upstream of it: retail archives, INI, WND parsing,
the 207-window shell, fonts, the logo texture and the button geometry all work. Classified in §8.2
and §8.3.

### 3.4 What could not be measured: a click

Synthetic input does not reach this window. `CGWarpMouseCursorPosition` moves the real cursor and
`CGEventPost` returns success, but the game's `MouseIO` never changes, `Window_Is_Active` stays
false, and the Window Server reports 26 windows in front of `zh`. A background, non-activated window
does not receive posted events, and this worker has no way to activate it (activation needs either
Accessibility/Screen Recording grants or a real user). So:

- the button rectangles hit-test correctly at their centres and reject `(400,300)` and doubled
  coordinates — measured, §4.3;
- **no menu callback was executed by a click.** Not verified, and not claimed. §8.6.

## 4. Q4 — points versus pixels at `backingScaleFactor` 2.00

### 4.1 What each layer holds

Read out of the running retail process:

| Quantity | Value | Unit |
|---|---|---|
| Window client area (Cocoa `bounds`) | 800 x 600 | **points** |
| `backingScaleFactor` | 2.00 | — |
| `CAMetalLayer.drawableSize` (`layer.contentsScale` = scale) | 1600 x 1200 | **pixels** |
| `VulkanBackend::swapchain_extent_` | `(width = 1600, height = 1200)` | **pixels** |
| `VulkanBackend::width_` / `height_` (the colour target) | 800 / 600 | **points** |
| `target_width_` / `target_height_` | 800 / 600 | **points** |
| `viewport_` | `(x=0, y=0, width=800, height=600, min_z=0, max_z=1)` | **points** |
| `scissor_enabled_` / `scissor_` | `false` / `{0,0},{0,0}` (so the viewport-sized default is used) | — |
| `TheDisplay->getWidth()` / `getHeight()` | 800 / 600 | **points** |
| `TheMouse->m_maxX` / `m_maxY` | 800 / 600 | **points** |
| `TheMouse->m_currMouse.pos` | `(358, 600)` | **points** |

### 4.2 The defect this exposes

The swapchain is in pixels (1600x1200) and everything the engine draws with is in points
(800x600) — and the two are reconciled in `Present`:

```cpp
blit.srcOffsets[1] = {int32_t(width_), int32_t(height_), 1};              // 800 x 600
// Stretched to the window, so a resized window is filled rather than painted in one corner.
blit.dstOffsets[1] = {int32_t(swapchain_extent_.width),
                      int32_t(swapchain_extent_.height), 1};             // 1600 x 1200
vkCmdBlitImage(..., VK_FILTER_LINEAR);
```

So on a 2.00-scale display the game renders at **one quarter of the panel's pixels** and MoltenVK
linearly upscales the result to the drawable. This is the classic half-resolution HiDPI bug, and it
is invisible to CI by construction: Linux asserts scale 1.00, where `srcOffsets == dstOffsets` and
the blit is an identity copy. It is not a crash and not a wrong-coordinates bug — geometry, cursor
and hit-testing are all self-consistent in points — it is purely a resolution loss. §8.4.

The stretch is also deliberate for another purpose (it is what makes a resized window fill), so the
fix is not "delete the blit": the colour target has to be created at
`client size x backingScaleFactor` with the viewport following it, and the mouse/GUI kept in points.
That is more than a small in-scope fix, so it is reported rather than attempted.

### 4.3 Mouse and GUI hit-testing are consistently in points

Hit-testing every main-menu button at its own centre, at a point outside it, at the window centre
`(400,300)`, and at **double** its centre coordinates — the last being what a points/pixels
confusion in the input path would need to succeed:

```
ButtonSinglePlayer  hit(centre)=true  hit(outside)=false  hit(400,300)=false  hit(centre*2)=false
ButtonMultiplayer   hit(centre)=true  hit(outside)=false  hit(400,300)=false  hit(centre*2)=false
ButtonLoadReplay    hit(centre)=true  hit(outside)=false  hit(400,300)=false  hit(centre*2)=false
ButtonOptions       hit(centre)=true  hit(outside)=false  hit(400,300)=false  hit(centre*2)=false
ButtonCredits       hit(centre)=true  hit(outside)=false  hit(400,300)=false  hit(centre*2)=false
ButtonExit          hit(centre)=true  hit(outside)=false  hit(400,300)=false  hit(centre*2)=false
```

`m_maxX`/`m_maxY` are 800/600 and the live cursor position is clamped inside that box, so
`MouseIO`, `TheDisplay` and the GUI all agree with the Cocoa `bounds` in points. **Verdict: the only
points/pixels error on this machine is the render resolution; the input and GUI coordinate spaces are
correct.** The residual risk that a click lands in the wrong place is *not* excluded, because no
click was ever delivered (§3.4) — hit-testing was driven directly, one layer above the OS event.

## 5. Q5 — does CoreAudio play retail audio and the retail MP3s?

#102 and #106 proved the decoder on Linux by decoding to samples. This section only asks whether
**this machine's audio device consumes retail content**, which is a different claim.

### 5.1 The device is real

`OpenAL Soft` in the retail process, with `ALSOFT_LOGLEVEL=3`:

```
[ALSOFT] (II) Initialized backend "core"
[ALSOFT] (II) Opening default playback device
[ALSOFT] (II) Got device type 'ispk'
[ALSOFT] (II) Created device 0x81b810020, "MacBook Pro Speakers"
[ALSOFT] (II) Pre-reset:  Stereo, Float32, 48000hz, 512 / 1536 buffer
[ALSOFT] (II) Post-reset: Stereo, Float32, 44100hz, 512 / 1411 buffer
[ALSOFT] (II) Stereo rendering
[ALSOFT] (II) Created context 0x81aa19e20
```

and CoreAudio, asked from outside the process which of its process objects are producing output
(`AudioObjectGetPropertyData` over `kAudioHardwarePropertyProcessObjectList`,
`kAudioProcessPropertyIsRunningOutput`):

```
default output device id=83 name="MacBook Pro Speakers" nominal_rate=44100 IsRunningSomewhere=1
CoreAudio process objects: 32
obj=126 pid=89570 running=1 runningOutput=1 bundle=- cmd=./zh
```

### 5.2 Retail MP3 music actually plays

Opened through the engine's own Miles-shaped stream path (`AIL_open_stream`, which reads via
`TheFileSystem` → `.big` archive → the shim's MPEG decoder → OpenAL → CoreAudio), then started, then
watched from outside:

```
stream voice at 0xb418b0500
fields: {rate: 44100, channels: 2, bits: 16, totalFrames: 8416512, codec: Mpeg, mpegFrames: 7306,
         playing: false, framesPlayed: 0}
AIL_set_stream_volume_pan -> ok        AIL_start_stream -> ok
after start: {playing: true, framesPlayed: 0, readCursor: 20062}
coreaudio: obj=126 pid=89570 running=1 runningOutput=1 cmd=./zh

t+2s  framesPlayed=119808  readCursor=85264   playing=true  exhausted=false
t+4s  framesPlayed=331776  readCursor=200620  playing=true  exhausted=false
t+6s  framesPlayed=635904  readCursor=366132  playing=true  exhausted=false
```

The file is `Data\Audio\Tracks\USA_11.mp3` out of `MusicZH.big` — 7306 MPEG frames, 8416512 frames of
44.1 kHz stereo. `framesPlayed` only advances when OpenAL's mixer *retires* queued buffers, and
OpenAL's mixer only retires them when the CoreAudio HAL pulls them, so this is the device consuming
decoded retail MP3 in real time, not a decode.

### 5.3 A retail one-shot actually plays

Same idea through the sample path (`AIL_allocate_sample_handle` / `AIL_set_sample_file` /
`AIL_start_sample`) with a retail WAV read out of `AudioZH.big`:

```
openFile(Data\Audio\Sounds\addnwi1a.wav) -> 0x81ac481b0
readEntireAndClose -> 0x820298020
AIL_allocate_sample_handle -> 0xb418e7820   AIL_init_sample -> ok
AIL_set_sample_file -> 1                    AIL_set_sample_volume_pan -> ok
AIL_start_sample -> ok      total ms: 3285
t+1s  ms pos: 1857
t+2s  ms pos: 0        (finished, source rewound)
t+3s  ms pos: 0
coreaudio (throughout): obj=126 pid=89570 running=1 runningOutput=1 cmd=./zh
```

A 3285 ms retail sample played to completion in about the wall-clock time it should take.

### 5.4 What is *not* proven, and the two findings underneath it

- **No recording of the analogue output exists.** There is no loopback device and no microphone
  grant on this worker, so "the speaker moved" is inferred from the mixer retiring frames and
  CoreAudio reporting the process as producing output. That is much stronger than a decode, and it
  is still not a recording. Stated as such.
- **The game never played anything by itself.** At the retail main menu, with retail data,
  `m_playingStreams`, `m_playingSounds`, `m_playing3DSounds` and `m_audioRequests` were all empty
  until §5.2/§5.3 started a voice by hand. Two reasons found:
  1. the engine was muted the whole time — `TheAudio->m_muteReasonBits = 1`, i.e.
     `MuteAudioReason_WindowFocus`, with `m_musicVolume`/`m_soundVolume`/`m_speechVolume` all 0.
     The window never became active (§3.4), so focus mute never lifted. After
     `unmuteAudio(MuteAudioReason_WindowFocus)` the configured volumes appeared
     (music 0.55, sound 0.72, from `AudioSettings.ini`) — so this is the focus path working as
     written, downstream of the activation problem, not an audio defect;
  2. even unmuted, `addAudioEvent("End_USA_Failure")` returned handle 16 and created no voice.
     `AudioManager::addAudioEvent` only *queues* a request; `MilesAudioManager::playAudioEvent` is
     what opens the stream, on a later engine update. A hand-built event injected from a debugger
     does not satisfy the same path as a real one, so this is an evidence-path limitation, not a
     measurement of the engine. §8.7.
- **The 49 base-game MP3s are not on this machine.** `Music.big` in this depot is 786724 bytes and
  contains a single entry, `generalsa.sec`; the real base-game `Music.big` is hundreds of MB.
  `MusicZH.big` has all 7 Zero Hour tracks and they play (§5.2). So `End_USAf.mp3`, the first track
  tried, failed at `AIL_open_stream` because it is genuinely absent — **missing data**, not a defect.
  §8.8.

**Verdict: yes for the paths that could be reached** — CoreAudio opens the real device, retail MP3
from `MusicZH.big` and a retail WAV from `AudioZH.big` both play through it in real time — **and the
game does not yet play audio of its own accord on this machine**, for the two reasons above, neither
of which is in the audio backend.

## 6. The exit crash

Every harness run that renders correctly still dies on the way out:

```
harness exit code: -11
```

after `DX8Wrapper::Shutdown` reported ok. The stack is a recursion through
`std::recursive_mutex::lock` → `CriticalSection::enter` → `DynamicMemoryAllocator::allocateBytes`,
entered from OpenAL Soft's **static destructors** running after the engine's `GameMemory` has been
torn down: the allocator tries to allocate while shutting down, takes a lock it holds, and overflows
the stack. Everything measured above happens before it. §8.5.

## 7. Verified on real Apple Silicon versus still Linux-only

| Claim | Real Apple Silicon | Linux/CI only | Notes |
|---|---|---|---|
| Level 1..4 build, 979/979 objects, 0 unresolved | **yes** | — | §1 |
| Strict link agrees with `nm` (Mach-O `_` prefix, name forms) | **yes** | — | first time compiled by a Mach-O toolchain |
| `.tbd` system-symbol discount list (12756 symbols, 50 sources) | **yes** | — | ditto |
| Thin arm64, no Rosetta | **yes** | — | `lipo`, `proc_translated` |
| Swapchain in the linked archive (`check-swapchain-compiled.py`) | **yes** | — | closes §3.1 of `renderer-first-frame.md` |
| `vkQueuePresentKHR` reached, `FrameCount` advanced, device not lost | **yes** | — | 3 frames |
| Frame equals #101's lavapipe frame | **yes, delta 0** | — | centre pixel and channel ranges identical |
| Spike reference image on real Metal | **yes**, worst channel delta 1 | — | 0 pixels differ by >2/255 |
| Fixed-function/resource-lock suites, both swizzle modes, layer silent | **yes** | — | validation layer proven loaded |
| Staging-cost ceiling incl. self-check | **yes** (reuse 0.9831) | — | on `Apple M1 Pro` |
| Retail archives, INI, `MainMenu.wnd`, 207-window shell | **yes** | — | §3.2 |
| Retail menu renders *correctly* | **no** — magenta field, doubled labels | — | §3.3, §8.2/§8.3 |
| A menu button responds to a click | **not measured** | — | window cannot be activated, §3.4 |
| GUI hit-test, mouse, `TheDisplay` all in points at scale 2.00 | **yes** | — | §4.3 |
| Render resolution at scale 2.00 | **no** — 800x600 upscaled to 1600x1200 | Linux is scale 1.00, cannot see it | §4.2, §8.4 |
| CoreAudio device open at 44.1 kHz | **yes** | — | §5.1 |
| Retail MP3 played through the device | **yes** | decode-only was #106 | §5.2 |
| Retail one-shot played through the device | **yes** | decode-only was #102 | §5.3 |
| Analogue output recorded | **no** — no loopback/mic grant | — | §5.4 |
| Game starts audio unprompted at the menu | **no** — focus-muted, §5.4 | — | §8.6/§8.7 |
| 49 base-game MP3s | **absent from this depot** | — | §8.8 |
| Windows build / replay gate | **not run here** | Windows/CI only | out of this slice; no engine code changed |

## 8. Defects and limitations, classified

### 8.1 The macOS validation-layer recipe cannot work through a Python wrapper — port defect (procedure)
SIP strips `DYLD_*` when `/usr/bin/python3` execs, so every `scripts/ci/*.py` gate that launches a
spike binary loses the layer and fails with `VK_ERROR_LAYER_NOT_PRESENT` (-6). Fix: a layer manifest
with an absolute `library_path` plus `VK_LAYER_PATH` (§2.5), and update the recipe in
`native-build.md` / the `renderer-spike-verify` skill. Dangerous because the same absence reads as
`validation messages: 0`.

**Fixed** by `scripts/ci/vulkan_manifests.py`, which rewrites both manifests with absolute paths and
hands the gates `VK_LAYER_PATH`/`VK_ICD_FILENAMES`; a `--validation` run that did not load the layer
now fails instead of reporting zero messages. `docs/porting/hidpi-scale.md` §5.

### 8.2 `Copy_Rects: source and destination formats differ` — unimplemented path
The shell's 2D composition blit declines when the formats differ, leaving the destination
unwritten — which is the magenta/noise field behind the retail menu, and (drawn twice at two
offsets) the doubled labels. Reproduce: run the retail binary with `-win -noshellmap -nologo`,
`Measure_Frame` the colour target, and count the `Copy_Rects` line. One occurrence per run.

### 8.3 `Decode_Fvf: unsupported FVF 0x0` — unimplemented path
Twice per retail run. An FVF of 0 reaches the backend's vertex-declaration decoder, which has no
translation for it. Same reproduction as §8.2.

### 8.4 Half-resolution render at `backingScaleFactor` 2.00 — port defect
Colour target, viewport and scissor are sized from the client area in **points** (800x600) while the
swapchain is the drawable in **pixels** (1600x1200); `Present` reconciles them with a
`VK_FILTER_LINEAR` blit, so the game renders a quarter of the panel's pixels and is upscaled. Fix
direction and why it is not a small fix: §4.2. Invisible to CI because Linux asserts scale 1.00.

**Fixed** — the default colour target and everything device-side now follow the backing scale in
pixels while the mouse, the GUI and the D3D8 logical viewport stay in points, and the presentation
blit stays because it is what makes a resized window fill. The rule, the injected-scale CI gate that
fails on the pre-fix code, and the Retina check a Mac session still has to run are in
`docs/porting/hidpi-scale.md`. Full resolution on a real Retina panel remains **unverified** here.

### 8.5 Exit-time stack overflow in OpenAL's static destructors — port defect
`SIGSEGV` (`-11`) after a clean shutdown, recursing `CriticalSection::enter` →
`DynamicMemoryAllocator::allocateBytes` from OpenAL Soft's static destructors, after `GameMemory` is
gone. Affects exit status only; every measurement in this document precedes it. §6.

**Fixed** by a lifetime: the five sections the allocator takes are now immortal on the non-Windows
entry point, so static destructors that allocate find a live mutex. Evidence and the in-process
control: `docs/porting/memory-shutdown-order.md`. Exit status 0 on hardware is still to be confirmed.

### 8.6 A background window receives no synthetic input — measurement limitation
`CGEventPost`/`CGWarpMouseCursorPosition` succeed, `MouseIO` never changes, `Window_Is_Active` stays
false, 26 windows are in front. Needs Accessibility/Screen Recording grants or a real user; this
worker has neither. Consequence: menu callbacks and the focus-mute lift are unverified.

### 8.7 `addAudioEvent` from a debugger queues but does not play — measurement limitation
`AudioManager::addAudioEvent` returns a handle and enqueues; `MilesAudioManager::playAudioEvent`
opens the stream on a later update. A hand-built event does not exercise the same path, so §5 used
the Miles-shaped API directly instead. Not a finding about the engine.

### 8.8 The base game's 49 MP3s are missing from this depot — missing data
`Music.big` here is 786724 bytes with one entry (`generalsa.sec`). `MusicZH.big` has the 7 Zero Hour
tracks and they play. Any test that needs the other 49 needs a fuller install.

### 8.9 No photograph of the desktop — measurement limitation
No Screen Recording grant, and `CGWindowListCreateImage` is gone from the current SDK. All pixel
evidence in this document is in-process readback plus the Window Server's `CGWindowListCopyWindowInfo`
record.

### 8.10 Encountered but deliberately not touched — other slices' work
`GetSystemDirectoryA()` reports "not implemented off Windows" once per run (harmless here: callers
get an empty path). Retail asset paths are `\`-separated throughout, which the archive layer handles;
the loose-file and MapCache path-separator defects belong to the parallel slice and were routed
around, not fixed.

## 9. Reproducing this

```sh
python3 scripts/native-build.py --level 1 --level 2 --level 3 --level 4 --with-shims --strict-link
python3 scripts/ci/check-swapchain-compiled.py --archive build/native/libsupport_renderbackend.a
python3 scripts/ci/check-d3d8-surface.py && python3 scripts/ci/check-backend-coverage.py
python3 scripts/native-render-backend-run.py   # 3 frames on MoltenVK + readback + validation count

cmake -S spikes/renderer -B build/spike-mac -G Ninja -DCMAKE_BUILD_TYPE=Release -DSPIKE_USE_SDL2=OFF
cmake --build build/spike-mac
export VK_ICD_FILENAMES=$(find "$(brew --prefix molten-vk)/" -name MoltenVK_icd.json | head -1)
export VK_LAYER_PATH=<dir with an absolute-library_path validation manifest>   # see 8.1
python3 scripts/ci/check-spike-render.py --binary build/spike-mac/zh-renderer-spike \
  --reference spikes/renderer/docs/spike-triangle.png --out spike-mac.png
python3 scripts/ci/check-staging-cost.py --binary build/spike-mac/zh-staging-workload --self-check
```

The retail parts need a Zero Hour install symlinked into a run directory outside the repo, the
binary run from it, and LLDB attached to the live process; nothing retail belongs in the repository
and no path to it is recorded here.
