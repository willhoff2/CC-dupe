# Is it a game yet? Playing a skirmish on Apple Silicon and measuring everything that is not pixels

Every probe before this one measured a subsystem. This one plays: a real retail shell driven with real
OS input on the M1 Pro, a real Zero Hour skirmish, and then eight questions answered from engine state,
OS state and crash reports rather than from the viewport (which a parallel slice owns,
`docs/porting/draws-per-frame.md` §5.1).

**Headline: it is a running game that simulates for hours, and it is not yet a game a person would
play.** A skirmish survived 21.8 minutes of continuous probing and a second process survived 2 h 41 min
without dying. Inside that, four defects are the difference between "runs" and "playable": the game has
no sound effects at all (music only, because no sample voice is ever allocated), a save can be written
but never appears in the load list, the renderer leaks a texture *and* a surface handle several times a
second forever, and quitting from the main menu still ends in `SIGSEGV`. One session also wedged: the
pause menu stopped accepting any input while the process kept rendering.

Nothing here was fixed. The only code in this PR is the measurement script,
`scripts/macos-playability-probe.py`.

## 0. The machine, the binary, the data

| Thing | Value |
|---|---|
| Machine | Apple M1 Pro, 16-core GPU, Metal 4, macOS 26.6.1 (Darwin 25.6.0 arm64) |
| Renderer | Vulkan through MoltenVK; 800x600 point client at `backingScaleFactor` 2.0 → 1600x1200 pixels |
| Binary | `scripts/native-build.py --level 1..4 --with-shims --strict-link`: 980 objects, 0 compile failures, 0 unresolved symbols, `link_clean` true, Mach-O 64-bit **arm64**, Apple clang 21.0.0, `host_translated` false, `proc_translated` 0 |
| Run | `./zh -win` from a disposable run directory of symlinks into a retail install |
| Harness | `scripts/macos-input-drive.py` (real `CGEventPost` input + LLDB state) and `scripts/macos-playability-probe.py` (new here: long-lived LLDB attach that samples and resumes) |
| Base | `main` at `0e52ccd4b` |

Every number below was measured on this machine in this session. Retail-derived screenshots and the
save file stay out of the repo; they are attached to the session. **This is one machine**: the frame
times are an M1 Pro result, not an Apple Silicon result.

## 1. Does it survive? Yes — 21.8 min under the probe, 2 h 41 min unattended

`macos-playability-probe.py run --minutes 20 --interval 30`, 40 samples, `GAME_SKIRMISH` (2) throughout,
`m_gamePaused` 0 in every sample:

| | value |
|---|---|
| Wall clock probed | 21.83 min |
| Logic frames | 8737 → 31049 (22,312 frames ≈ 12.4 min of game time at 30 Hz) |
| Deaths | none; the process was alive at the end of the run and `died` is empty |

A second process (a GLA skirmish) then ran **2 h 41 min** in-mission without crashing; it ended because
this probe sent it `SIGTERM`, not because it died. Survival is not the problem.

**Classification: no defect.** Long-run survival is real.

### 1.1 …but memory grows without bound, and it is a leak, not growth — PORT DEFECT

RSS over the 21.8-minute run: **32.5 MB → 184.3 MB**, peaking at 276.3 MB. That alone is only "growth".
The mechanism was then measured directly, and it is worse than a slow leak:

While the game sat **paused** in the in-game menu (no simulation at all), RSS went **82 MB → 227 MB in
30 seconds**, and the live handle vectors inside the Vulkan backend grew monotonically:

| `spike::VulkanBackend` member | first read | ~17 min later (still paused) | rate |
|---|---|---|---|
| `owned_surfaces_` | 268,686 | 405,299 | ≈ +134/s |
| `owned_textures_` | 134,523 | 202,830 | ≈ +67/s |

Two `SIGSTOP` backtraces taken minutes apart both landed in the same place:

```text
spike::VulkanBackend::Get_Surface_Level(...) at vulkan_backend.cpp:4508
VulkanD3DTextureClass::GetSurfaceLevel(...) at vulkanrenderbackend.cpp:722
TextureClass::Get_Surface_Level(...) at texture.cpp:983
Render2DSentenceClass::Build_Textures(...) at render2dsentence.cpp:362
Render2DSentenceClass::Render(...)
W3DDisplayString::draw(x=379, y=446, ...)
drawStaticTextText -> W3DGadgetStaticTextDraw -> GameWindowManager::drawWindow -> winRepaint
W3DInGameUI::draw -> W3DDisplay::draw -> GameClient::update
```

So the 2D text path rebuilds its textures every frame, every rebuilt texture is registered in
`owned_textures_` and gets a fresh entry in `owned_surfaces_`, and nothing is ever released. Frame 0 of
the backtrace is the *iterator increment* of the linear scan in `Get_Surface_Level`
(`for (SurfaceHandle* existing : owned_surfaces_)`), so the cost of every text draw grows with the
number of leaked surfaces: this is simultaneously the memory defect and the slowdown in §2.1.

**Classification: PORT DEFECT** — unbounded lifetime in the D3D8→Vulkan texture/surface seam, reachable
by simply leaving the game open.

## 2. Is it fast enough? Borderline, and the simulation silently runs ~5 % slow

`m_framesPerSecondLimit` 30 with `m_useFpsLimit` 1, i.e. the engine's intended rate is 30 (and
`LOGICFRAMES_PER_SECOND` is 30, `Core/GameEngine/Include/Common/GameCommon.h`).

Render side, from `W3DDisplay::updateAverageFPS`'s own `fpsHistory` ring read out of process memory
(1,200 frame samples across the run):

| metric | value |
|---|---|
| mean frame time | 33.61 ms |
| p50 | 33.34 ms |
| p99 | 44.70 ms |
| worst | 51.70 ms |
| render FPS mean / min / max | 29.81 / 19.34 / 34.68 |

Logic side, measured **against the time the process was actually running** (the probe freezes the game
to sample, so wall clock between samples is not the denominator — `ran_for` is), 39 intervals:

| metric | value |
|---|---|
| logic FPS mean | 28.38 |
| p50 | 28.38 |
| min / max | 26.48 / 29.21 |

The simulation never reached its intended 30 Hz in any interval. A player would experience this as the
whole game — build times, harvesting, timers — running about 5 % slow, uniformly, which is invisible
alone but is a real divergence from the Windows oracle for anything time-dependent.

**Classification: PORT DEFECT (small).** 30 Hz is the engine's intended rate and it is missed
consistently, on a machine that is not thermally or GPU limited at 800x600.

### 2.1 Performance degrades with uptime

The same process after ~2.7 h of uptime reported `fpsHistory` values of **14.99–26.00** (mean ≈ 18.7
at first sample, ≈ 23.8 thirty seconds later) *while paused with nothing to simulate*. That is the
`Get_Surface_Level` linear scan of §1.1, not a GPU limit.

**Classification: PORT DEFECT**, same root cause as §1.1.

## 3. Is the game audible? Music only. There is no sound-effect path at all — PORT DEFECT

Measured in all 40 samples of the long run, and again in the second process:

| audio state | value |
|---|---|
| `al_context` | non-null |
| `OpenALAudio::lib().streams` | 1, `playing` 1, source in `AL_PLAYING` |
| stream file | `Data\Audio\Tracks\CHI_02.mp3` → `CHI_04.mp3` → `CHI_05.mp3` (the track changes over the run) |
| `framesPlayed` | 3,170,304 → 8,478,720, advancing every sample |
| `OpenALAudio::lib().samples` | **0**, in every sample |
| `OpenALAudio::lib().objects` (3D voices) | **0**, in every sample |
| `OpenALAudio::lib().quickAudio` | **0**, in every sample |

So: music is genuinely being pulled — an OpenAL source is in `AL_PLAYING` and its decoded frame counter
advances monotonically while the game runs, across a track change, which decode-only probes (#102/#106)
could not show. But **no 2D sample and no 3D voice is ever allocated**, for the entire life of the
process, in a skirmish with units firing. There are therefore no unit responses, no weapon sounds and no
EVA — not "we could not hear them", but "no voice object exists to play them".

The mechanism in the source is `MilesAudioManager::selectProvider`: the shim advertises exactly one 3D
provider named `OpenAL`, and if the preferred-provider string does not match it the manager ends at
`m_selectedProvider = PROVIDER_ERROR` and never opens a provider, leaving the sample pools empty even
though the settings ask for 4 2D and 25 3D samples. **The empty pools are measured; that this specific
mismatch is the cause is an inference from source** and wants a direct read of `m_selectedProvider` and
`m_pref3DProvider` on a fresh process to confirm.

**What could not be proven without a listener**: that the music is *audible* — no HAL-level check
(device sample-rate, underrun counters) was made. What was proven is stronger than decode and weaker
than sound: OpenAL reports the source playing and its frame counter advances.

**Also measured, and it matters for anyone testing audio**: losing window focus sets
`m_muteReasonBits` 0 → 1 and drops `m_musicVolume`/`m_soundVolume` to 0.0, as documented. Regaining it
needs a **real click into the window** — the Accessibility raise (`AXRaise`/`AXFrontmost` all returning
`kAXErrorSuccess`) left `Window_Is_Active` false with 21 windows in front, and the mute stayed on until
a posted click landed in the client area, after which `m_muteReasonBits` went back to 0.

**Classification: PORT DEFECT** (no sample/3D voice pool is ever created), plus an **UNIMPLEMENTED
PATH** note: nothing in the OpenAL device reports voice-pool creation failure to the player or the log.

## 4. Does the AI play? It builds and it earns. Nothing died in 12 minutes of game time

Enemy is player 3, `has_ai` true, side `China`, in a 2-player skirmish:

| frame | objects | units built | buildings built | money | money earned |
|---|---|---|---|---|---|
| 8737 | 14 | 8 | 6 | 15,000 | 11,400 |
| 20214 | 17 | 11 | 6 | 31,200 | 30,000 |
| 31049 | 22 | 13 | 9 | 32,170 | 36,220 |

So the AI builds structures, produces units, and harvests (money earned triples). The second, longer
process showed the same shape at a larger scale: the AI player held 157 objects with 14 units and 5
buildings built and 21,225 earned.

What did **not** happen: `units_lost` and `buildings_lost` were **0 for every player in every sample**,
across 22,312 logic frames. No unit and no building died in ~12.4 minutes of game time. The longer
process did show the UI message `Structure under attack` (so contact occurred there), yet its loss
counters were also 0 at the sampled frames.

**Classification: NOT MEASURABLE YET** for "does combat resolve and do units die" — the probe proves
build/gather/economy and proves nothing died in the observed window, which is not the same as proving
combat is broken. What would settle it: a scripted engagement (drive a unit group into the AI base with
real input, or start on a map where the bases are adjacent) and sample `units_lost` and
`Object::m_health`/destroy counts; that is a scoped follow-up, not a guess to make here.

## 5. Does video play where the game plays it? At the intro, yes — visibly

The retail intro movie renders on screen in a real run: full letterboxed frames of the retail intro
(screenshot attached to the session, not committed), while the run log shows FFmpeg colour conversion
for the same frames:

```text
[swscaler @ 0x780208000] No accelerated colorspace conversion found from yuv420p to rgb565le.
```

That is a player-visible frame, not a decoder return code.

**Correction to my own instrumentation, so nobody repeats it**: `((VideoPlayer*)TheVideoPlayer)->
m_firstStream` read **0 at the same moment the intro was visibly playing on screen**, and it read 0 in
all 40 samples of the skirmish run. That field is *not* a valid playback indicator, and "video_stream 0"
in the probe JSON must not be quoted as evidence that no video played.

**Mission briefing / load-screen video: NOT MEASURED.** A skirmish has neither, and the campaign path
was not re-driven in this session. What would unblock it: drive Single Player → USA → Easy (the path
`docs/porting/real-input-menu-drive.md` already establishes) and screenshot the load/briefing screen
while sampling the video subsystem.

## 6. Save and load: the save is written, and the game can never find it again — PORT DEFECT

Saving mid-skirmish works from the player's side: the in-game menu → `SAVE/LOAD` → save entry →
confirm produced `*** Game Saved ***` in the UI and a **2,023,468-byte** file on disk.

Where the file went:

```text
.../Command and Conquer Generals Zero Hour Data/Save\00000000.sav      <- the save, 2,023,468 bytes
.../Command and Conquer Generals Zero Hour Data/Save/                  <- the directory, EMPTY
```

`GameState::getSaveDirectory()` builds `<user data>` + `"Save\\"`, so on macOS the backslash becomes
part of the *file name* rather than a separator. The engine also creates the real `Save` directory
(`CreateDirectory`) and leaves it empty. The same defect is visible for screenshots
(`Screenshots\sshot_20260816_183356_873.png` beside an empty `Screenshots/`).

The consequence is not cosmetic. `GameState::iterateSaveFiles` does
`SetCurrentDirectory(getSaveDirectory().str())` and then enumerates `*.sav`, so enumeration cannot see
the file. Driven from a **fresh process** — Main Menu → Load/Replay → Load Game — the `SELECT GAME`
list came up **empty**, with `LOAD GAME` and `DELETE` greyed out, minutes after a successful save.

**Classification: PORT DEFECT.** Path-separator handling in the save path (the class of defect #110
fixed elsewhere) makes save/load unusable end-to-end.

**Self-consistency (does a restored game match): NOT MEASURABLE YET**, and blocked by exactly this
defect — the UI offers nothing to load. Pre-save state was captured for the comparison when it becomes
possible: frame 16621, `GAME_SKIRMISH`, player object counts 14 / 205 / 2 / 157, AI money 22,175.
What would unblock it: fix the separator (or hand-place a copy at the enumerated path) and re-run.

## 7. Quit: still a `SIGSEGV` at exit, in the same object pool — PORT DEFECT

Fresh process, no debugger attached, driven with real input: Main Menu → `Exit`. The process
disappeared, and macOS wrote a crash report (`zh-2026-09-02-033051.ips`):

```text
exception  EXC_BAD_ACCESS (SIGSEGV), KERN_INVALID_ADDRESS at 0x7ab5804c00000000
thread     com.apple.main-thread (triggered)
  zh                ObjectPoolClass<MultiListNodeClass, 256>::~ObjectPoolClass()
  zh                ObjectPoolClass<MultiListNodeClass, 256>::~ObjectPoolClass()
  libsystem_c       __cxa_finalize_ranges
  libsystem_c       exit
  dyld              start
```

This is the crash `docs/porting/draws-per-frame.md` §5.3 saw, in the same
`ObjectPoolClass<MultiListNodeClass>`, still present after #117 — now reached from a static destructor
run by `exit`/`__cxa_finalize_ranges` rather than from an explicit shutdown call. #117's fix to shutdown
ordering does not cover this path.

**Classification: PORT DEFECT.** Every quit is a crash report. (No user data was lost in the runs here,
but nothing proves the exit path finished flushing anything either.)

**Mechanism found and fixed** in `docs/porting/memory-shutdown-order.md` ("The second mechanism"):
`ObjectPoolClass::Allocate_Object_Memory()` skipped `sizeof(uint32)`, not `sizeof(uint32 *)`, past the
block's next-block pointer, so on LP64 the first object of every block overwrote the top half of it —
which is why the fault address has a zero low half. Reproduced and fixed on Linux x86-64; the three
Mac quit paths remain **UNMEASURED**.

## 8. What a player notices that the subsystem probes could not see

Measured with real `CGEventPost` input plus LLDB reads:

- **Camera** — arrow/scroll keys move the view: `W3DView::m_pos` changed in response to posted key
  events. Works.
- **Clicks reach the engine** — a posted click stops on a breakpoint in `Mouse::processMouseEvent` with
  `event_x`/`event_y` equal to the posted client coordinates. A drag-select left
  `TheInGameUI->m_selectCount` at 1 and `m_frameSelectionChanged` at 1386. Selection works.
- **Command bar and radar exist and are populated** — `ControlBar` context windows and `Radar`
  object lists were non-empty, and the radar renders in the screenshots.
- **Input latency: NOT MEASURABLE YET.** Nothing here is a latency number: the harness posts an event
  and reads state afterwards, with no common clock. What would unblock it: stamp `CGEventPost` against
  `MouseIO::time` (the seam already carries `event.Time_Ms`) and against the logic frame at which the UI
  reacted, over many events.
- **Hotkeys, build orders and unit commands: NOT MEASURED.** Camera keys and selection were exercised;
  issuing an actual build or attack order through the command bar was not.

### 8.1 One session wedged: rendering continued, all input stopped — PORT DEFECT (candidate)

After ~2.7 h of uptime, a save, and a focus loss/regain, the in-game pause menu stopped responding to
everything:

| observation | value |
|---|---|
| Rendering | continuing (`fpsHistory` 15–26, screenshots update, RSS climbing) |
| Mouse motion | delivered — `TheMouse->m_currMouse.pos` tracked posted moves to (400,347) |
| Click on `SAVE/LOAD`, then on `EXIT GAME` | no effect; the breakpoint in `Mouse::processMouseEvent` was hit at the right coordinates, `mouse_left_state` stayed 0 |
| `Escape` | no effect |
| `TheInGameUI->m_isQuitMenuVisible` | stayed 1 for > 20 min |
| `TheGameLogic->m_frame` | stayed 16621 for > 20 min (the menu pauses the game, and nothing could unpause it) |

The session was unrecoverable — a player would have to force-quit. Earlier in the same process the
identical clicks at the identical coordinates *did* work (they opened the save dialog and saved).

**Classification: PORT DEFECT (candidate).** Not reproduced from a fresh process and the mechanism is
unidentified, so it is not yet a named defect. What would unblock it: breakpoints on
`GameWindowManager::winProcessMouseEvent` and `Mouse::update` to see whether translated button events
are dequeued at all in that state, plus a repro attempt (long uptime + focus cycle + pause menu).

## 9. Ranked: what stands between this and a game a person would call playable

Ranked by what a player notices first, not by fix cost.

1. **The mission viewport is corrupt** (`draws-per-frame.md` §5.1, owned by a parallel slice, not
   measured here). Ranked first because it is the first thing anyone sees; listed only so the ranking is
   honest. Slice 5 (`block-compressed-textures.md`) names the mechanism behind the black/striped
   models and decals and fixes it on the seam; the real-game re-measurement on the Mac is still owed,
   so this item keeps its rank until it exists.
2. **There are no sound effects, ever** (§3, PORT DEFECT). Music plays, so the game sounds *nearly*
   right, which is worse: every click, every unit, every weapon and all of EVA are silent. No voice
   object is ever allocated.
3. **Saving is a trap** (§6, PORT DEFECT). The game says `*** Game Saved ***`, writes 2 MB, and the load
   list is empty forever. A player loses a session the first time they rely on it.
4. **Input can wedge permanently mid-game** (§8.1, PORT DEFECT candidate). Rendering continues, no key
   or click does anything, force-quit is the only exit. Rare in this session — but unrecoverable.
5. **The renderer leaks a texture and a surface several times a second** (§1.1/§2.1, PORT DEFECT).
   ~134 surfaces/s and ~67 textures/s *even while paused*, RSS 32 MB → 184 MB in 20 min, and the
   `Get_Surface_Level` linear scan drags render FPS from 30 to ~17 after a few hours. A long session
   degrades and eventually exhausts memory.
6. **Every quit is a crash** (§7, PORT DEFECT). `SIGSEGV` in
   `ObjectPoolClass<MultiListNodeClass>::~ObjectPoolClass` from `exit`. Mechanism named and fixed
   (LP64 block-header arithmetic, `docs/porting/memory-shutdown-order.md`); Mac confirmation owed.
7. **The simulation runs ~5 % slow** (§2, PORT DEFECT, small). 28.38 logic FPS against an intended 30,
   in every measured interval; a uniform time-base divergence from the Windows oracle.
8. **Combat is unproven** (§4, NOT MEASURABLE YET). The AI builds, produces and harvests, but nothing
   died in 12.4 minutes of game time. This is the highest-value *next measurement*, since a game where
   combat does not resolve is not a game — it is ranked below the defects above only because it is not
   yet known to be broken.
9. **Mission briefing / load-screen video is unmeasured** (§5, NOT MEASURED). The intro movie visibly
   plays, so the seam works somewhere; the campaign sites were not driven.
10. **Input latency is unquantified, and orders/hotkeys are unexercised** (§8). Camera, clicks and
    selection work; issuing a build or attack order through the command bar has never been measured.

## 10. Reproducing this

```sh
# build (arm64; the shell must not be running under Rosetta)
arch -arm64 python3 scripts/native-build.py --level 1 --level 2 --level 3 --level 4 \
    --with-shims --strict-link --report /tmp/l4.md --json /tmp/l4.json

# drive the shell into a skirmish with real OS input
arch -arm64 python3 scripts/macos-input-drive.py --pid <pid> --binary <run-dir>/zh buttons
arch -arm64 python3 scripts/macos-input-drive.py --pid <pid> --binary <run-dir>/zh click --window ButtonSkirmish

# then sample the running game for 20 minutes (LLDB needs Xcode's arm64 python)
export PYTHONPATH=$(lldb -P)
arch -arm64 /Applications/Xcode.app/Contents/Developer/usr/bin/python3 \
    scripts/macos-playability-probe.py --pid <pid> --binary <run-dir>/zh --out probe.json \
    run --minutes 20 --interval 30
```

`macos-playability-probe.py` keeps one LLDB attach for the whole run, samples and then resumes the
process, and records `ran_for` per interval so logic rate is computed against time the game was
actually running. Two cautions learned the hard way: only one debugger may be attached at a time (a
second attach fails with `tried to attach to process already being debugged`), and optimised
`std::vector`/`std::list` accessors are not callable in expressions — the script reads vector storage
pointers directly instead.
