# The video path, executed off Windows at 64-bit

A probe slice, not an implementation. Everything below was measured on Linux/x86-64 with
`clang++-14` — the same toolchain and the same object files the native build ratchet produces — or
read out of the retail Zero Hour 1.04 `INIZH.big`. Where a claim is inference rather than
measurement it says so.

Two artefacts back it: `scripts/native-video-probe.py` (builds and runs the probe) and
`scripts/native-video-probe/video_decode_probe.cpp` (the harness). The probe links the engine's own
`FFmpegFile.cpp.o` and `File.cpp.o` out of `build/native`, so what it exercises is engine code
compiled by the real build, not a re-implementation of it.

## 0. The headline

The decision on record — use `RTS_BUILD_OPTION_FFMPEG` instead of Bink — survives contact with real
Bink files. FFmpeg has a native Bink decoder, the pinned build enables it, and the engine's
`FFmpegFile` opens and decodes Bink 1 through it at 64-bit with no seek support and no Windows.
"There is no decoder" is the wrong answer.

What is *not* true is that the native build plays movies. Two independent gaps, both of them small:

1. Nothing defined `RTS_HAS_FFMPEG`, so the linked binary constructed `NullVideoPlayer`.
   Fixed in this slice (one commit, two lines — see §5).
2. The frame *upload* is `IDirect3DTexture8::GetSurfaceLevel` + `IDirect3DSurface8::LockRect`, so it
   stops exactly where #87 stopped: the null render backend. That is the renderer slice's territory
   and is untouched here.

## 1. What Zero Hour actually ships (retail INI, not inference)

From `Data/INI/Video.ini` inside the retail `INIZH.big` (SHA-256 of the archive it came from is in
§7): **41 `Video` entries**, every one of them a `Filename` with no extension. The engine appends
the extension itself — `FFmpegVideoPlayer.cpp` has `#define VIDEO_EXT "bik"` — and looks in, in
order, `<mod>\Data\Movies`, `Data/<language>/Movies` and `Data\Movies`.

| group | count | internal names | files |
| --- | --- | --- | --- |
| campaign mission briefings | 15 | `MD_USA01`–`05`, `MD_China01`–`05`, `MD_GLA01`–`05` | `MD_*_0` |
| Generals Challenge general portraits | 20 | `Portrait<General><Left\|Right>` | `Comp_*Gen_[inv_]000` |
| Generals Challenge chrome | 2 | `GeneralsChallengeBackground`, `VSSmall` | `GC_Background`, `VS_small` |
| front-end intro | 4 | `EALogoMovie[640]`, `Sizzle[640]` | `EA_LOGO[640]`, `sizzle_review[640]` |

`Data/INI/Campaign.ini` confirms how they are reached: each `Mission` block carries an `IntroMovie`
label, and the three single-player campaigns (`USA`, `GLA`, `China`, 5 missions each) name exactly
the 15 briefing movies above. The nine `CHALLENGE_*` campaigns all name
`GeneralsChallengeBackground`.

A useful accident falls out of the same two files: **five movie labels that retail references are
not defined in retail's own `Video.ini`** — `TrainingCampaign`, `END`, and the three
`FinalVictoryMovie`s (`USACampaignVictory`, `GLACampaignVictory`, `ChinaCampaignVictory`).
`VideoPlayer::getVideo()` returns null for them, so `open()` returns null and the caller takes its
"no movie" branch. The shipped Windows game therefore exercises the missing-movie path on every
campaign completion. That is the oracle telling us the path is meant to be survivable.

### Container and codec

`.bik` is Bink, and FFmpeg decodes Bink 1 natively (`binkvideo`, `binkaudio_dct`,
`binkaudio_rdft`) — no SDK, no `BINKW32.DLL`. `scripts/ci/fetch-probe-deps.sh` builds the pinned
FFmpeg with all of those plus the `bink` and `binka` demuxers enabled, and the probe reports
libavformat 61.7.100 / libavcodec 61.19.101 / libswscale 8.3.100 from that build.

**The one thing I could not measure**: the retail movie *bytes*. The CI archives are trimmed to what
a headless replay reads and contain no `Data/Movies` at all (`scripts/ci/pack-gamedata.py`), and the
full install exists only on the user's Mac. So the retail files' exact Bink revision is still
inference: ZH 1.04 predates Bink 2 by years and ships `BINKW32.DLL`, which decodes Bink 1 only, so
Bink 1 is near-certain — but "near-certain" is not "measured". Four bytes per file settle it; see §8.

## 2. FFmpeg through the engine's own `FFmpegFile`, at 64-bit

Four distinct public Bink files (three `BIKi`, one `BIKb`; not retail assets — they are stand-ins
for the container, not for the inventory in §1). Each was pushed through the real
`FFmpegFile::open()`, `decodePacket()` and the pixel conversion `FFmpegVideoStream::frameRender()`
performs, with the engine's `File` abstraction underneath:

| file | magic | video | `getNumFrames()` | frames decoded | `getFrameTime()` | audio | decode cost |
| --- | --- | --- | --- | --- | --- | --- | --- |
| ActivisionLogo | `BIKi` | 720×486 yuv420p | 311 | 312 | 33 ms | 2ch 22500 Hz | 1.1 ms/frame |
| C1ab7 | `BIKb` | 200×116 yuv420p | 40 | 40 | 100 ms | none | 0.5 ms/frame |
| logo_legal | `BIKi` | 640×480 yuv420p | 266 | 266 | 33 ms | 2ch 44000 Hz | 0.9 ms/frame |
| logo_lucas | `BIKi` | 512×384 yuv420p | 280 | 280 | 40 ms | 2ch 44100 Hz | 0.7 ms/frame |

Everything that matters here worked unmodified:

- `avio_alloc_context` with the engine's `readPacket` and **no seek callback** is enough. Across all
  four files libavformat asked for zero seeks, so probing Bink over a non-seekable custom `AVIOContext`
  — which is what reading out of a `.big` archive would be — is not a problem the format creates.
- Both Bink audio variants decode; sample counts and rates come back sane.
- All four `VideoBuffer` destination formats convert through `sws_scale`: `TYPE_R8G8B8` → `rgb24`,
  `TYPE_X8R8G8B8` → `bgr0`, `TYPE_R5G6B5` → `rgb565le`, `TYPE_X1R5G5B5` → `rgb555le`, each
  producing non-zero pixels at the expected pitch.
- Decode runs 30–60× faster than playback, on one thread, with no SIMD tuning. Timing will not be
  the problem.

`--png` writes the converted `rgb24` buffer out as an image, which is how the conversions above were
checked by eye as well as by byte count. No decoded frame is committed: the samples are third-party
content and the retail assets are off limits.

## 3. Where a decoded frame goes, and where it stops

```
FFmpegFile::decodePacket()  ->  m_frameCallback  ->  FFmpegVideoStream::m_frame (av_frame_clone)
Display::update()          ->  frameDecompress()  (empty in this backend)
                           ->  frameRender(VideoBuffer*)
                                 sws_scale(AVFrame -> VideoBuffer::lock())
W3DVideoBuffer::lock()     ->  TextureClass::Get_Surface_Level(0)   [IDirect3DTexture8]
                           ->  SurfaceClass::Lock(&pitch)          [IDirect3DSurface8::LockRect]
```

- **`frameDecompress()` being empty is correct here**, not a hole: with Bink the SDK decompressed on
  demand, whereas `decodePacket()` has already produced a full `AVFrame` by the time
  `frameRender()` runs.
- **The upload is the renderer seam.** `W3DVideoBuffer::allocate()` constructs a `TextureClass` and
  immediately `lock()`s it; both go through `DX8Wrapper`, so with a null `RenderBackend` this dies at
  the same `dx8wrapper.cpp` point #87 reached. Reported, not touched.
- **Timing is `std::chrono::system_clock`** — portable, and already free of `timeGetTime`. But
  `getFrameTime()` is `1000u / fps` in integer arithmetic, so 29.97 fps becomes 33 ms instead of
  33.37: measured on the 312-frame sample that is ~114 ms of drift by the end, i.e. playback runs
  ~1.1 % fast and would desync against audio the moment audio exists. `system_clock` is also the
  wrong clock for an interval — it is not monotonic and steps with NTP. Both are pre-existing
  behaviour, both are cheap to fix, neither is a port defect.
- **`getNumFrames()` is `duration × avg_frame_rate`, truncated**, and came back one frame short on
  the 312-frame sample. `Display::update()` stops the movie at `frameCount() - 1`, so a movie ends
  one frame early. Cosmetic. The sharper edge of the same code: if `duration` were ever
  `AV_NOPTS_VALUE`, `frameCount()` would be 0, `frameIndex() != -1` would never fail, and the movie
  would never stop — and `Intro::update()` and `GameLogic::update()`'s `startNewGame` gate both wait
  on `isMoviePlaying()`, so that would hang the front end. It did not happen on any sample here; it
  is a risk to keep in view, not an observed failure.
- **Movie audio is dead code in every configuration this tree can build.** Every audio branch in
  `FFmpegVideoPlayer.cpp` is behind `RTS_USE_OPENAL`, and *nothing in the repository defines
  `RTS_USE_OPENAL`* — not CMake, not `cmake/openal.cmake`, not the probe. The OpenAL include at the
  top of the same file is behind a differently-spelled `RTS_HAS_OPENAL`, which is also undefined.
  So `onFrame()` decodes audio frames and drops them: movies will play silent, on Windows and off it
  alike. Unimplemented path, and *not* something to "fix" by defining the macro blind — the branch
  calls `TheAudio->getHandleForBink()` and `OpenALAudioStream::bufferData`, which needs the audio
  slice's opinion.
- **Path resolution is already portable.** `FFmpegVideoPlayer::open()` builds `Data\Movies\X.bik`
  with backslashes, and `StdLocalFileSystem::openFile()` translates them and then retries
  case-insensitively component by component. Nothing to do for macOS here.

## 4. Is video on the single-player critical path?

Mostly no — with one exception that will crash.

- **The intro is skippable by construction.** `Intro::update()` advances its state machine whenever
  `!TheDisplay->isMoviePlaying()`, and `Display::playMovie()` returns quietly on a null stream. With
  no video backend the intro steps straight through to `doPostIntro()`.
- **Campaign start does not wait on video.** `GameLogic::update()` calls `startNewGame()` when
  `m_startNewGame && !TheDisplay->isMoviePlaying()`; a movie that never opens satisfies that
  immediately.
- **Mission briefings degrade.** `LoadScreen`'s campaign path hides the progress bar and returns when
  `open()` yields null.
- **Scripted movies cannot block progress.** `ScriptActions` plays movies, and the
  `VIDEO_HAS_COMPLETED` script condition reads `ScriptEngine::isVideoComplete()` — but *both* call
  sites of `notifyOfCompletedVideo()`, in `Display::stopMovie()` and `InGameUI::stopMovie()`, are
  commented out in retail-derived code ("removing sync error source -MDC"). The condition can never
  become true on Windows either, so no mission can be gated behind it.
- **The exception, and it is single-player Zero Hour content:**
  `ChallengeLoadScreen::init()` (`Core/GameEngine/Source/GameClient/GUI/LoadScreen.cpp`, ~line 951)
  opens `GeneralsChallengeBackground` and then calls `m_videoStream->width()` **without a null
  check** — unlike the six other call sites in the tree, which all guard. Generals Challenge is
  single-player, and this is reached on every Challenge mission load. With no video backend, or with
  a movie that fails to open, that is a null dereference. Note this is *engine* behaviour, not a port
  defect: retail Windows crashes the same way if the file is missing. Left unfixed on purpose —
  it is a two-line guard, but it is a behaviour change to shared `Core` code and belongs in a
  reviewed commit of its own, not smuggled into a probe.

So: **video is a polish item for campaign and skirmish, and a hard blocker for Generals Challenge
only until either that null check lands or the FFmpeg backend works.**

## 5. The one fix in this slice, stated loudly

`scripts/native-port-probe.py`: added `RTS_HAS_FFMPEG` to the `defines` of the two
`GameEngineDevice` probe targets — two lines. Without it the linked binary compiled the FFmpeg
translation units, linked libavcodec, and then constructed `NullVideoPlayer` anyway, which made
every downstream question unanswerable. Measured after the change:

- `objdump` of `W3DGameClient::createVideoPlayer()` now calls `FFmpegVideoPlayer::FFmpegVideoPlayer()`
  (it called `NullVideoPlayer::NullVideoPlayer()` before).
- Levels 1–4 with `--strict-link`: 977/977 objects, 0 failures, **0 unresolved symbols**, binary
  produced. Unchanged from #86/#87.
- Both probe baselines unchanged (671/760 native, 716/760 shimmed) — the defines land on renderer
  targets, which the tracked totals do not include.

## 6. Classification

| finding | kind |
| --- | --- |
| Bink 1 video + audio decode through `FFmpegFile` at 64-bit | works, measured |
| `sws_scale` into all four `VideoBuffer` formats | works, measured |
| `Data\Movies` path resolution on a POSIX case-sensitive FS | works, measured |
| `RTS_HAS_FFMPEG` never defined → `NullVideoPlayer` | build-configuration defect (fixed here) |
| texture upload via `IDirect3DTexture8`/`IDirect3DSurface8` | unimplemented path — renderer slice |
| movie audio behind an undefined `RTS_USE_OPENAL` | unimplemented path — audio slice |
| `getFrameTime()` integer truncation, `system_clock` for intervals | pre-existing defect, cheap |
| `getNumFrames()` truncation; `frameCount() == 0` would hang the intro | pre-existing defect, cheap |
| `ChallengeLoadScreen::init()` null dereference | pre-existing engine defect, in scope, unfixed |
| retail movie bytes never opened | data problem |

## 7. Reproducing it

```sh
./scripts/ci/fetch-probe-deps.sh                      # builds the pinned FFmpeg with Bink enabled
CLANGXX=clang++-14 python3 scripts/native-build.py \
    --level 1 --level 2 --level 3 --level 4 --with-shims --strict-link
python3 scripts/native-video-probe.py path/to/movie.bik --png /tmp/frame.png
```

The probe reuses the compile flags recorded in `build/native/compile_commands.json` for
`FFmpegFile.cpp`, so it cannot drift from the real build's configuration without failing.

Retail INI in §1 came from `zerohour104_gamedata_trimmed.7z` in the CI bucket, SHA-256
`2d137f6cd51609b517345fc7ab780dfbf4547eca0dd689e7212d8506a36b1b13`. That does **not** match the
`EXPECTED_HASH_GENERALSMD` default in `.github/workflows/check-replays.yml`
(`6837FE1E…`), so either the repository variable overrides it or the object has been repacked;
worth resolving before anyone trusts that gate. Nothing from the archive is committed here.

## 8. What the next slice needs

1. **Four bytes per retail movie.** `head -c 4` over `Data/Movies/*.bik` on the Mac, plus the
   directory listing, closes §1's only inference. If any file is `KB2*`, that file is Bink 2 and
   FFmpeg cannot decode it.
2. **One retail movie**, ideally `MD_USA01_0.bik` and `GC_Background.bik`, through
   `scripts/native-video-probe.py`. That turns "the container works" into "the assets work".
3. **The renderer seam first.** The remaining unknown on this path is whether a `TextureClass` lock
   under the Vulkan backend gives `sws_scale` a writable linear surface with a sane pitch. Nothing
   about video can be proven end-to-end before that lands.
4. Then, in order and each small: the `ChallengeLoadScreen` null guard; `getFrameTime()` /
   `getNumFrames()` in floating point off a monotonic clock; and movie audio, which is the audio
   slice's `RTS_USE_OPENAL` question, not this one's.
