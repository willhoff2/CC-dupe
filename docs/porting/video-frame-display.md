# Retail video, actually displayed: decode → texture → draw

#92 proved that FFmpeg decodes retail Bink. It did not prove that a decoded frame reaches a
framebuffer, because the frames had never been driven through a texture. This slice closes that gap
and reports what is left.

Everything below was measured on Linux/x86-64, Vulkan through **lavapipe** (`llvmpipe`, LLVM 15.0.7),
`clang++-14`, with the objects and archives `scripts/native-build.py --level 1..4 --with-shims
--strict-link` produced. **Nothing here is evidence about Apple Silicon** — §7 says what a Mac
session has to run to make the same claims there.

Retail movie bytes came out of `zerohour104_movies.7z` in the project's S3 bucket (SHA-256
`ec124a788e988d73aec28059810d5945fa4272b4cc503104fe69b008672fc3d2`, 604,804,334 bytes, 70 entries).
No retail byte, decoded frame or PNG is committed.

## 1. The path, end to end

The only new code is the harness. The path is the engine's own:

```
FFmpegVideoPlayer::createStream(File*)          the #92 decode path, real File abstraction
  FFmpegFile::open / decodePacket               libavformat 61.7.100 / libavcodec 61.19.101
FFmpegVideoStream::frameRender(VideoBuffer*)
  sws_scale(AVFrame -> VideoBuffer::lock())     libswscale 8.3.100
W3DVideoBuffer::lock()
  TextureClass::Get_Surface_Level(0)            IDirect3DTexture8::GetSurfaceLevel
  SurfaceClass::Lock(&pitch)                    IDirect3DSurface8::LockRect      [lock class C1]
W3DDisplay::drawVideoBuffer()
  Render2DClass::Add_Quad + DX8Wrapper          the engine's own video quad
VulkanRenderBackendClass                        the D3D8-shaped backend
  colour-target readback -> PNG
```

`W3DDisplay::drawVideoBuffer` is the shipped draw. The harness reaches it through `WW3D::Init` +
`WW3D::Set_Render_Device` and the same `Render2DClass` quad, and then reads the colour target back
instead of presenting — headless equivalence is exactly "the same submission, `Present` replaced by
a readback".

No second upload path was added. The existing seam serves video once the defect in §2 is fixed.

## 2. The defect that made "the texture was written" a lie

`W3DVideoBuffer::lock()` locks **level 0 of a texture** through the surface handle
`GetSurfaceLevel(0)` returns. The Vulkan backend routed *every* `IDirect3DSurface8::LockRect`
through `Surface_Bits()`, which is the read-back/persistent mapping for a surface's own staging
memory. So every byte `sws_scale` wrote was accepted, could be read back through the same funnel —
and was never uploaded into the texture's `VkImage`. The draw sampled an untouched image, i.e. black,
while every check on the write side said the write had happened. That is precisely the failure mode
the slice was told to watch for, and it is why the pixel comparison is the assertion and a plausible
picture is not.

Fix, in `Core/Libraries/Source/WWVegas/WW3D2/vulkanrenderbackend.cpp`:

```cpp
bool VulkanD3DSurfaceClass::Locks_Through_Texture() const
{ return Container != NULL && Container->Is_Lockable(); }        // lockable == not a render target

HRESULT VulkanD3DSurfaceClass::LockRect(...)
{ if (Locks_Through_Texture()) return Container->LockRect(Level, locked_rect, rect, flags); ... }
```

A texture-owned surface that is lockable locks through *the texture's own existing level funnel*
(`Lock_Texture`/`Unlock_Texture`); render targets and standalone surfaces keep `Surface_Bits()`.
This is the classification the renderer spike already draws (`spikes/renderer/src/resource_lock_tests.cpp`
separates `Lock_Texture` from `Surface_Bits`); the engine backend was the copy that conflated them.
No new lock class: the video lock stays **C1 whole-surface write** and both audits still match
(`d3d8-lock-scan.py --check`, `surface-lock-audit.py --check`, 100/100 sites, 9 classes).

## 3. The pixel evidence

`scripts/native-video-frame-run.py` compares the frame the engine *drew* against a reference decoded
by a **different** FFmpeg (the system `ffmpeg` binary, not the pinned libraries the engine links),
and again against a YUV→RGB conversion redone here in integer BT.601, so a swscale-side colour bug
cannot cancel itself out. Three controls run against every frame; each of them is what one specific
wrong-but-plausible result would look like.

`Data/English/Movies/MD_USA01_0.bik`, 800×600, frames 301-304, `X8R8G8B8`, validation layer loaded
and silent:

| comparison | mean abs delta | max channel delta |
| --- | ---: | ---: |
| drawn vs system-ffmpeg `rgb24` | **0.51** | 3 |
| drawn vs independent integer BT.601 | **1.09** | 3 |
| control: channels swapped (R↔B) | 51.69 | 255 |
| control: flipped vertically | 64.27 | 255 |
| control: row skew (wrong row pitch) | 62.43 | 255 |

`Data/Movies/GC_Background.bik`, 800×600, frames 61-64: drawn vs system ffmpeg **0.10** / max 2,
vs integer BT.601 **0.20** / max 2, controls 8.88 / 13.28 / 3.29 — 33-133× the straight comparison.

The residual of 0.5/1.1 with a max of 3 is the two conversions' own rounding (swscale's fixed-point
YUV→RGB against integer BT.601), not a path error: it is bounded, uniform, and the same on both
movies. What it is *not* is any of the four failure modes:

- **wrong colour space** — the drawn frame is closer to two independently computed conversions than
  those conversions' controls are to each other.
- **wrong channel order** — the R↔B control is 101× worse.
- **flipped origin** — the vertical-flip control is 126× worse.
- **wrong row pitch** — the row-skew control is 122× worse. (A plain one-row shift is too weak a
  control on video, whose adjacent rows are similar; the control rolls row *y* by *y* pixels, which
  is what a pitch mismatch actually does.)
- **stale texture contents** — consecutive drawn frames differ (mean 2.7, max ~200) by the same
  amount the movie's own consecutive frames differ, so the upload happens every frame. A texture
  uploaded once would match frame 0 forever, and did exactly that before §2's fix.

Mip staleness cannot arise on this path: `W3DVideoBuffer::allocate()` constructs its `TextureClass`
with `MIP_LEVELS_1` (`W3DVideoBuffer.cpp:135`), so there is exactly one level to go stale, and the
lock the video path takes is level 0 of it.

Reproduce (needs retail bytes, so it is not a CI gate):

```sh
CLANGXX=clang++-14 python3 scripts/native-build.py --level 1 --level 2 --level 3 --level 4 \
    --with-shims --strict-link
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json \
python3 scripts/native-video-frame-run.py --movie .../MD_USA01_0.bik \
    --frames 4 --start-frame 300 --validation
```

The first frames of several retail movies are pure black (a fade-in), which is why `--start-frame`
exists: a black frame passes every comparison and proves nothing.

## 4. `ChallengeLoadScreen::init()`: the null dereference, fixed, with a negative control

`Core/GameEngine/Source/GameClient/GUI/LoadScreen.cpp` opened the Challenge background movie and
then read `m_videoStream->width()` unconditionally. Six other call sites in the tree guard; this one
did not, so a movie label the player cannot open crashed Generals Challenge instead of bringing the
screen up without its movie. Retail Windows crashes the same way — this is an engine defect the port
exposes rather than a port defect.

The guard wraps only the buffer creation, so exactly one behaviour changes: a **null** stream now
continues into the rest of `init()`. A stream that opens and then cannot allocate or display its
buffer still tears down and returns early, as before, because Windows is the oracle for that case.

`scripts/native-loadscreen-video-test.py` is the negative control and it does three things a
hand-written unit test would not:

1. It **classifies the real file**: it parses `ChallengeLoadScreen::init()` out of `LoadScreen.cpp`
   and reports `guarded` / `unguarded (first unguarded use: ...)`. Reverting the fix makes the
   classifier say `unguarded`, so the test tracks the production body rather than a copy of it.
2. It compiles `Core/GameEngine/Source/GameClient/tests/loadscreen_video_null_test.cpp`, which
   models both shapes over fake `VideoStream`/`VideoBuffer`/`Display`, and runs the **fixed** shape
   over a null stream (must survive) and both shapes over a stream that opens across all eight
   combinations of `memPassed`/`allocate`/`display` (must be indistinguishable — 8/8 agree).
3. It runs the **pre-fix** shape over the same null stream in a child process and requires it to die
   on `SIGSEGV`. It does. That is the crash this slice fixes, reproduced on demand:

```
ChallengeLoadScreen::init() as it stands: guarded
   the pre-fix body: unguarded (first unguarded use: if (m_videoBuffer == nullptr || !m_videoBuffer->allocate( m_videoStream->width(),)
ok: a null video stream is survived
ok: both shapes agree over a working stream (8 combinations)
ok: the pre-fix shape died on SIGSEGV, which is the crash this slice fixes
failures: 0
```

## 5. What the video path still lacks, measured over all 41 retail movies

`scripts/video-path-gaps.py` drives the engine's own `FFmpegFile`/`FFmpegVideoStream` over every
retail `GeneralsMD` movie (41 files) and cross-checks each number against `ffprobe -count_frames`
from a different FFmpeg. Ranked by consequence; none of these is fixed here.

1. **`frameGoto()` does not seek — `unimplemented path`.** On *all 41* movies, `frameGoto(n)` leaves
   `frameIndex()` at 1 and the stream then stops delivering frames (four `frameNext()` calls do not
   move the index). `FFmpegFile::seekFrame()` computes its timestamp as
   `time_base × frame_idx × avg_frame_rate` — `time_base` and `avg_frame_rate` are reciprocals, so
   the target collapses to roughly `frame_idx` in seconds-times-fps units, i.e. the wrong quantity —
   and it seeks every stream with `AVSEEK_FLAG_ANY` without flushing the decoder. Its own comment
   says "not tested, since not used ingame", which is nearly true: `LoadScreen.cpp` calls
   `frameGoto(frameCount())` to *end* a movie, where landing nowhere is harmless. Any real use
   (rewind, skip) is broken today.
2. **Movie audio is compiled out — `unimplemented path`.** 20 of 41 retail movies carry audio (2ch,
   44.1 kHz; the EA logos 48 kHz), and every audio branch in `FFmpegVideoPlayer.cpp` is behind
   `RTS_USE_OPENAL`, which the real build does not define — verified by reading the actual compile
   command for `FFmpegVideoPlayer.cpp` out of `build/native/compile_commands.json`, not by grepping
   CMake. Every briefing plays silent. The remaining 21 movies (the 20 Challenge portraits and
   `VS_small`) have no audio track at all, so they are silent by design.
3. **There is no audio clock, so A/V sync does not exist — `unimplemented path`.**
   `FFmpegVideoStream::isFrameReady()` compares wall-clock elapsed time against
   `getFrameTime() × frameIndex()`. Nothing reads the audio stream's position. Whatever video-clock
   error exists is uncorrected by construction, so item 4 becomes desync the moment item 2 lands.
4. **`getFrameTime()` truncates to integer milliseconds — `pre-existing engine defect`.** All 41
   movies are affected. 30 fps → 33 ms instead of 33.3333 (+0.3333 ms/frame); 15 fps → 66 instead of
   66.6667 (+0.6667 ms/frame). Measured drift over the whole movie: **+0.64 s on `MD_USA01_0.bik`**
   (1919 frames), +0.65 s on `sizzle_review.bik` (1961), +0.06 s on `GC_Background.bik` (180). The
   pacing gate really does release frames that fast — measured mean interval 33.00 ms (min 32,
   max 34) over 30 frames, against a true period of 33.3333 ms.
   `EA_LOGO.BIK` is genuinely fractional (30.0013 fps), so it cannot be fixed by a different integer.
5. **`isFrameReady()` uses `std::chrono::system_clock` — `pre-existing engine defect`.** Not
   monotonic: an NTP step or a user clock change during a movie moves the deadline for every
   remaining frame. `steady_clock` is the whole fix.
6. **`frameCount()` truncates, so a movie ends one frame early — `pre-existing engine defect`.**
   `getNumFrames()` is `duration × avg_frame_rate` truncated. It is one short on **7 of 41** movies
   (`MD_GLA03`, `MD_GLA04`, `MD_USA03`, `MD_USA05`, `MD_China05`, `Comp_AirGen_inv_000`,
   `Comp_LaserGen_inv_000`); the playback loops stop at `frameCount() - 1`, so the last frame is
   never shown. The engine's own decoder delivered the full ffprobe count on all 41, so the file is
   fine and the arithmetic is not.
7. **`frameCount() / FRAME_FUDGE_ADD` divides by zero on 20 of 41 retail movies — latent
   `pre-existing engine defect`.** `LoadScreen.cpp` (both the campaign path at line 540 and the
   Challenge path at line 1057) computes `Int progressUpdateCount = m_videoStream->frameCount() / 30`
   and then evaluates `frame % progressUpdateCount`. Every Challenge portrait movie is 13 frames, so
   the divisor is 0. It is **not reachable with retail data**, because the movies those two loops play
   are the 15 briefings and `GC_Background` (180+ frames) while the 13-frame portraits are played by
   `WindowVideoManager`, not by this loop — but a mod or a replacement movie shorter than 30 frames
   turns a load screen into an integer division by zero. Left unfixed deliberately: it is a second
   behaviour change to shared `Core` code and deserves its own review, not a ride-along.
8. **Subtitles: nothing exists to lack.** There is no subtitle path on the video side at all — no
   subtitle stream is opened, and `FFmpegFile` only ever matches `AVMEDIA_TYPE_VIDEO` and
   `AVMEDIA_TYPE_AUDIO`. Retail did not put subtitles in the movies either: none of the 41 files
   carries a subtitle stream (ffprobe). Briefing text is separate game data, not a movie feature.
   `missing feature, not a port gap`.
9. **Movie-to-texture lifetime is unowned across a device reset — `unimplemented path`, Linux-only
   reasoning.** `W3DVideoBuffer`'s texture is constructed procedurally (`IsProcedural=true`,
   `MIP_LEVELS_1`, `POOL_MANAGED`) and is not registered with the asset manager, so
   `WW3D::_Invalidate_Textures()` never sees it and nothing re-uploads it. On D3D8 the managed pool
   restores contents itself, so Windows is safe. On the Vulkan backend
   `VulkanRenderBackendClass::Reset` rebuilds only the swapchain and records
   `IDirect3DDevice8::Reset(new back buffer size)` as unimplemented, so a resolution change mid-movie
   is not covered by any measurement here; the next uploaded frame would fix the contents in any case,
   which is why this ranks below the rest.
10. **Zero-frame completion is a real hang shape, not observed on retail data — `measurement`.** If
    `frameCount()` were ever 0 (a movie with no duration), `Display::update()`'s
    `frameIndex() < frameCount() - 1` loop and `isMoviePlaying()` gate never complete. All 41 retail
    movies report a non-zero count, so it stays a risk, not a finding.

Raw data: `scripts/video-path-gaps.py --movie-dir <extracted movies> --json out.json` regenerates the
whole table, including the per-movie release timestamps behind the pacing figures.

## 6. Gates run for this change, on this tree

| gate | result |
| --- | --- |
| `d3d8-lock-scan.py --check` | OK, 100/100 sites classified, matches baseline |
| `surface-lock-audit.py --check` | OK, matches the committed audit |
| `check-d3d8-surface.py` | OK, direct call surface matches the allowlist (4/4) |
| `check-backend-coverage.py` | OK, matches the committed baseline exactly |
| `check-spike-render.py` | OK, 0/480000 pixels differ, validation layer loaded and silent |
| `zh-feature-probe`, `zh-fixedfunc-tests`, `zh-resource-lock-tests` (both swizzle modes) | 0 failures, 0 validation messages |
| `check-staging-cost.py --self-check` | OK, within ceiling, ceiling still rejects the pre-pool behaviour |
| native probe, native / shimmed | 673/761 and 717/761 clean (was 672/760, 716/760) |
| native build, levels 1-3 / 1-4 / 1-4 debug | 843/843, 979/979, 979/979 objects; 0 unresolved at level 4 |
| the rest of the native sweep | layout, xfer, CRT, window scancodes/seam, bool-pointer, stackwalk, audio surface: OK |

Two denominators moved and both are the new test sources this slice adds, not the engine:
`Core/GameEngine` goes 210 → 211 translation units in the probe and 210 → 211 objects in the native
build (`loadscreen_video_null_test.cpp`, which follows the existing `tests/` convention, e.g.
`Common/System/tests/init_failure_test.cpp`), and the window/input scan's `mode_change` count goes
64 → 65 because the video harness calls `WW3D::Set_Render_Device`. Baselines regenerated, never
hand-edited.

`check-lanmessage-layout.py` fails its two 32-bit cases on this box for lack of 32-bit libraries; it
fails identically on unmodified `main` (verified in a clean worktree), so it is an environment gap
and not this change.

## 7. Linux-only, and what a Mac session must run

Everything in §3 is lavapipe. The colour values are software-rasterised, so what they prove is that
the *upload and sampling path* is right, not that MoltenVK agrees. A Mac session reproduces the same
claim by running exactly this, on Apple Silicon, not under Rosetta:

```sh
sysctl -n sysctl.proc_translated                    # must print 0
CLANGXX=clang++ python3 scripts/native-build.py --level 1 --level 2 --level 3 --level 4 \
    --with-shims --strict-link
lipo -archs build/native/native_strict_link         # must print exactly: arm64
python3 scripts/native-video-frame-run.py --movie .../MD_USA01_0.bik \
    --frames 4 --start-frame 300 --validation
python3 scripts/video-path-gaps.py --movie-dir .../GeneralsMD --json /tmp/gaps-macos.json
```

The comparison thresholds are absolute pixel deltas, not a reference image, so they are valid on any
backend. Two things are known to differ and are the reason the run is not a formality: MoltenVK
reports a different set of lockable formats, and a `CAMetalLayer`'s backing scale can make the
colour target larger than the movie — the harness compares at the size it read back, so a mismatch
shows up as a size error rather than as a silently resampled pass.

Do not overwrite `docs/porting/ci-baselines/*.json` from a Mac: they are the clang-14 Linux ratchet.

## 8. Classification summary

| finding | kind |
| --- | --- |
| retail Bink decoded, uploaded through the existing seam, drawn, and verified per pixel | works, measured (Linux/lavapipe) |
| texture level-0 surface locks used the read-back mapping and dropped every video write | port defect, fixed here |
| `ChallengeLoadScreen::init()` null video-stream dereference | pre-existing engine defect, fixed here |
| `frameGoto()`/seek lands nowhere and stalls the stream | unimplemented path |
| movie audio behind an undefined `RTS_USE_OPENAL`; no audio clock at all | unimplemented path |
| `getFrameTime()` / `frameCount()` integer truncation; `system_clock` for intervals | pre-existing engine defect |
| `frameCount() / 30` divides by zero for movies under 30 frames | pre-existing engine defect, latent on retail data |
| subtitles | missing feature, retail movies carry none |
| movie texture across a Vulkan device reset | unimplemented path, unmeasured |
| anything about Apple Silicon | not measured here — see §7 |
| retail movie bytes (the #92 open item) | missing data, now supplied |
