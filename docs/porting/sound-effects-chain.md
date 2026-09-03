# Sound effects: the chain from a gameplay event to an OpenAL source, and the two links that were broken

Slice 1 of the playability ranking (`playability-probe.md` §9 item 2: "there are no sound effects,
ever"). This document traces the path an `AudioEventRTS` takes from the gameplay code that raises it to
the OpenAL source that plays it, names the first broken link and a second one directly behind it,
classifies both, and records the device-level evidence for the fix.

**Platform of every measurement below: Linux x86-64** (Ubuntu 22.04, `clang++-14`, the strict-link
binary from `scripts/native-build.py --level 1 --level 2 --level 3 --level 4 --with-shims
--strict-link`, SDL2 window on `DISPLAY=:0`, lavapipe for the renderer, **OpenAL Soft with the `wave`
backend writing to a file**). Nothing here was run on Apple Silicon; every Apple Silicon row is
`UNMEASURED` and is listed in §7. Nothing here was heard by a human: the OpenAL Soft `wave` driver
writes the mixed output to a `.wav`, so the device-level evidence is `SYNTHETIC-ONLY` in the sense
this project uses — it proves the engine submits and OpenAL mixes and consumes buffers, it does not
prove a speaker moved (§5.4).

Game data: `zerohour104_gamedata_full.7z` (SHA-256
`d9ddd8112449555fcd6b102053d9232a25f8a46b270e0c90dc9c9afb0cac1ae4`), both archive sets reachable
through the registry substitute (`base-game-install-path.md`). Skirmish entered through the real shell
with real SDL input (main menu → Skirmish → Start), player GLA, one AI.

## 0. Summary

| | |
|---|---|
| First broken link | `MilesAudioManager::openDevice` → `selectProvider(PROVIDER_ERROR)`: the retail preference `Miles Fast 2D Positional Audio` resolves to no provider, `selectProvider` sees `providerNdx == m_selectedProvider` (both `PROVIDER_ERROR`) and returns before opening anything, so `m_num2DSamples`/`m_num3DSamples` stay 0 and every later link has no voice to take. |
| Classification | **PORT DEFECT.** The backend, the enumeration, the data and the event path are all correct; the engine's provider bookkeeping cannot represent "no Miles provider exists, use the only one there is". |
| Second broken link (directly behind it) | The end-of-sample callbacks truncate the backend voice pointer to `UnsignedInt` before `notifyOfAudioCompletion`, so on a 64-bit build no completion is ever matched to a `PlayingAudio`: voices are never returned to the pool (4 2D + 25 3D exhausted in under a minute) and loops never restart. |
| Classification | **PORT DEFECT**, 32→64-bit, same family as `LegacyDDSURFACEDESC2::Surface` (#137). |
| Fix | Both fixed in this slice, 21 lines in `MilesAudioManager.cpp`, 3 declarations widened to `uintptr_t`. No `AIL_*` added, no music-path or renderer change. |
| Device-level evidence | During a real skirmish on Linux: `m_num2DSamples` 4, `m_num3DSamples` 25, `OpenALAudio::lib().objects` 26 (25 voices + listener), 5 2D and 234 3D `AIL_start_*` calls in 3.5 minutes, up to 1 sample and 7 object sources in `AL_PLAYING` at once, `AL_SAMPLE_OFFSET` advancing between samples on named events (`WorkerVoiceMove`, `CommandCenterGLASelect`, `IndustrialYardAmbientLoop`, …), 69 `alSourcePlay` calls seen by an `LD_PRELOAD` interposer, a 148 MB non-silent 32-bit-float stereo 44.1 kHz `.wav` from the `wave` backend. |
| Heard | **No.** Nobody listened; the device was a file. |
| Apple Silicon | **UNMEASURED** for sound effects. Music on the M1 Pro is measured and untouched (`playability-probe.md` §3). |

## 1. The chain, as read from source

Zero Hour raises sound effects from many hundreds of call sites; the ones used as named witnesses below
are unit voice responses (`WorkerVoiceMove`, `WorkerVoiceSelect` from the selection/command translators),
structure selection (`CommandCenterGLASelect`), rally-point placement (`RallyPointSet`) and the map's
ambient emitters (`Amb_*Loop`, `IndustrialYardAmbientLoop`, `TrainClicketyClack`). Weapon fire goes
through `FiringTracker::speedUp/coolDown` → `TheAudio->addAudioEvent(&weaponFired->getFireSound())`
with the firing object's ID; EVA speech goes through `Eva::playSound`. All of them end in the same
call.

```
gameplay                 AudioEventRTS ev("WorkerVoiceMove"); ev.setObjectID(id);
                         TheAudio->addAudioEvent(&ev)
AudioManager::addAudioEvent          (GameAudio.cpp)
    getInfoForAudioEvent  -> AudioEventInfo lookup in m_allAudioEventInfo (INI-declared)
    isOn(AudioAffect_Sound / Sound3D / Speech)        <- toggles
    shouldPlayLocally / getUninterruptible            <- locality
    volume < AudioSettings::m_minVolume               <- volume floor
    allocateAudioEventRTS(copy) ; TheSoundManager->addAudioEvent(copy)   (non-music, non-stream)
SoundManager::addAudioEvent          (GameSounds.cpp)
    canPlayNow(ev)
        violatesVoice / isPlayingLowerPriority / isPlayingAlready (duplicate) / voice limits
    allocateAudioRequest ; m_request = AR_Play ; TheAudio->appendAudioRequest
AudioManager::appendAudioRequest     -> m_audioRequests
MilesAudioManager::update (per frame)
    processRequestList -> processRequest -> playAudioEvent(ev)
        AudioEventInfo::m_soundType & ST_WORLD ?  3D : 2D
        getAvailable3DSample / getAvailable2DSample   <- pool (m_available3DSamples / m_availableSamples)
        playSample3D(ev, H3DSAMPLE) / playSample(ev, HSAMPLE)
            loadFileForRead -> AudioFileCache::openFile (.wav inside the .big)
            AIL_set_3D_sample_file / AIL_set_sample_file        milesstub: decode, alBufferData
            AIL_register_3D_EOS_callback(set3DSampleCompleted)
            AIL_start_3D_sample / AIL_start_sample              milesstub: alSourcePlay
    m_playing3DSounds / m_playingSounds  <- PlayingAudio bookkeeping
OpenALAudio service thread (10 ms)     alGetSourcei(AL_SOURCE_STATE) == AL_STOPPED
    -> endOfSample callback -> set3DSampleCompleted(H3DSAMPLE)
    -> TheAudio->notifyOfAudioCompletion(handle, PAT_3DSample)
    -> findPlayingAudioFrom(handle) -> loop restart (startNextLoop) or release voice to the pool
```

The pools themselves are built once, at device open:

```
MilesAudioManager::openDevice
    AIL_startup ; AIL_open_digital_driver
    enumerateDevices        AIL_enumerate_3D_providers -> m_provider3D[], m_providerCount
    selectProvider(TheAudio->getProviderIndex(m_pref3DProvider))
        AIL_open_3D_provider ; createListener ; initSamplePools
            m_num2DSamples = AudioSettings::m_sampleCount2D (4)
            m_num3DSamples = AudioSettings::m_sampleCount3D (25)
            AIL_allocate_sample_handle x4 ; AIL_allocate_3D_sample_handle x25
```

`milesstub` (`Core/Libraries/Source/OpenALAudioDevice/OpenALDriver.cpp`) enumerates exactly one 3D
provider, named `OpenAL`. `ListenerHandleClass` is not on this path (dead in Zero Hour, per the slice
constraints); the live listener is `AIL_open_3D_listener`.

## 2. Instrumentation

`scripts/linux-audio-chain-probe.py` is a gdb Python script attached to a live `zh`. It puts a
count-and-resume breakpoint on each link, records the `AudioEventRTS::m_eventName` at the links that
receive an event, records which OpenAL source each `AIL_start_*` handle owns, and once per interval
stops in `MilesAudioManager::update` to read the engine's provider/pool/`PlayingAudio` state and to call
`alGetSourcei(AL_SOURCE_STATE / AL_SAMPLE_OFFSET)` **in the inferior** on every voice — device state,
not engine bookkeeping. It changes no engine state. Its costs: every breakpoint hit stops all threads,
so with a busy map the logic rate drops to a few frames per second while attached (the OpenAL mixer
keeps real time, so short sounds can start and finish between two samples). Its `std::list` walk
assumes libstdc++'s `_List_node` layout (value after two pointers); this was checked by the fact that
the names read from the engine's `m_playing3DSounds` agree, source by source, with the names recorded at
`AIL_start_3D_sample` for the same AL source IDs.

Two other instruments were used and are not committed: an `LD_PRELOAD` interposer over
`alBufferData / alSourcei(AL_BUFFER) / alSourceQueueBuffers / alSourcePlay` that logs each call with the
source's state, and `dprintf` breakpoints on `setSampleCompleted / set3DSampleCompleted /
notifyOfAudioCompletion` and its not-found branch.

## 3. Where the count went to zero — measured

Counts are `AIL_*`-level breakpoint hits from process attach to detach, so they are lower bounds on a
run and only comparable within a row.

### 3.1 Before any fix (main at the branch point)

| link | count | note |
|---|---|---|
| `TheAudio->isOn` sound / 3D / speech | 1 / 1 / 1 | toggles are on |
| `m_providerCount` | 1 | `OpenAL` enumerated |
| `m_pref3DProvider` | `Miles Fast 2D Positional Audio` | from `Options.ini` / retail default |
| `getProviderIndex(m_pref3DProvider)` | `PROVIDER_ERROR` (4294967295) | |
| `m_selectedProvider` | `PROVIDER_ERROR` | never opened |
| `m_num2DSamples` / `m_num3DSamples` | **0 / 0** | pools never built |
| `OpenALAudio::lib().samples` / `.objects` | 0 / 0 | matches the M1 Pro measurement in `playability-probe.md` §3 |
| `AudioManager::addAudioEvent` | 13 in 30 s at the main menu, thousands per minute in a skirmish | events are raised |
| `SoundManager::canPlayNow` | same order | events reach the sound manager |
| `appendAudioRequest` (AR_Play) | 3 | |
| `playAudioEvent` | 5 | requests are processed |
| `playSample` / `playSample3D` | **0 / 5**, then `AL_PLAYING` 0 | see below |

So events are raised, looked up, pass the toggles/locality/volume filters, are queued and processed. The
`AudioEventInfo` lookup is not the problem (named events carry their INI names all the way to
`playSample3D`); the data is not the problem (`loadFileForRead` returned a buffer for every
`playSample3D`, `AIL_set_3D_sample_file` count equals `playSample3D` count in every run); the seam is not
the problem (once a voice exists, `alBufferData` and `alSourcePlay` follow). The first zero is the
**pool**: `getAvailable2DSample` / `getAvailable3DSample` have nothing to hand out because
`initSamplePools` never ran.

Why it never ran, read from `selectProvider` (`MilesAudioManager.cpp`):

```cpp
void MilesAudioManager::selectProvider( UnsignedInt providerNdx )
{
    if (!isOn(AudioAffect_Sound3D)) return;
    if (providerNdx == m_selectedProvider) return;        // <- PROVIDER_ERROR == PROVIDER_ERROR
    ...
    providerNdx = getProviderIndex( "Miles Fast 2D Positional Audio" );   // ignores the argument
    if( providerNdx >= m_providerCount )                  // (existing non-Miles fallback)
        providerNdx = ( m_providerCount > 0 ) ? 0 : PROVIDER_ERROR;
    success = AIL_open_3D_provider( m_provider3D[providerNdx].id ) == 0;
    ... createListener(); initSamplePools();
```

A fallback to provider 0 already exists further down, but it is unreachable on a non-Miles backend:
`openDevice` passes `getProviderIndex(m_pref3DProvider)`, which is `PROVIDER_ERROR`, and
`m_selectedProvider` is initialised to `PROVIDER_ERROR`, so the "already selected" guard fires first.
On Windows with Miles the preference always resolves, so this guard never sees two error sentinels. The
5 `playSample3D` hits with 0 pool are `friend_forcePlayAudioEventRTS`/attach-time noise; the
device-side `AL_PLAYING` was 0 for both `samples` and `objects` in every sample of the pre-fix run.

Classification: **PORT DEFECT** — a code path that should work (open the only provider the backend
offers) is skipped because the engine encodes "no provider selected" and "no provider found" with the
same sentinel. Not `UNIMPLEMENTED PATH` (the OpenAL provider, listener and pool paths exist and work
when reached), not `MISSING DATA` (the preference string is the retail default and the archives
resolve).

### 3.2 After the provider fix only (second build, before §3.3)

| link | value |
|---|---|
| `m_selectedProvider` | 0 (`OpenAL`) |
| `m_num2DSamples` / `m_num3DSamples` | 4 / 25 |
| `OpenALAudio::lib().samples` / `.objects` | 4 / 26 (25 voices + the listener) |
| `AIL_set_3D_sample_file` = `AIL_start_3D_sample` | yes, named events (`Amb_TemperateForestTreesLoop`, `TrainClicketyClack`, …) |
| `alSourcePlay` (interposer) | called, source state `AL_PLAYING` (0x1012) right after |
| after ~1 min in the skirmish: `m_availableSamples` / `m_available3DSamples` | **0 / 0** |
| `m_playingSounds` / `m_playing3DSounds` (engine) | 3 / 25 |
| sources in `AL_PLAYING` (device) | **0 / 0** |
| `set3DSampleCompleted` / `setSampleCompleted` callbacks | 26 |
| `notifyOfAudioCompletion` not-found branch | **26 of 26** |

Sound now started, and then the game went silent again within a minute: every voice the pool had was
handed out once, played to the end, and was never returned. The `dprintf` on the callback showed why —
`setSampleCompleted` received `ptr=0x7ffff2ae8f70` and `notifyOfAudioCompletion` received
`handle=0xf2ae8f70`:

```cpp
void AILCALLBACK set3DSampleCompleted( H3DSAMPLE sample3DCompleted )
{
    TheAudio->notifyOfAudioCompletion((UnsignedInt) sample3DCompleted, PAT_3DSample);
}
// virtual void notifyOfAudioCompletion( UnsignedInt audioCompleted, UnsignedInt flags );
```

`findPlayingAudioFrom` casts the truncated value back to `H3DSAMPLE` and compares it with the full
pointer stored in `PlayingAudio::m_3DSample`; on a 32-bit Windows build these are the same value, on any
LP64 build they never are. The consequences are exactly the two things the callback drives: voices are
never released to the pool, and looping sounds (`AC_LOOP`, restarted from inside
`notifyOfAudioCompletion` via `startNextLoop`) play once.

Classification: **PORT DEFECT**, 64-bit pointer truncation. It is the second link, not the first — with
empty pools no completion ever fires — but it is directly behind the first and was confirmed with its
own evidence rather than inferred, so it is fixed in the same slice. `notifyOfAudioCompletion` is part
of the `AudioManager` interface, so the widening touches `GameAudio.h`, `MilesAudioManager.h/.cpp` and
the non-Miles null manager's empty override; `uintptr_t` is `unsigned int` on the 32-bit Windows build,
so the Windows ABI of the virtual is unchanged.

### 3.3 After both fixes (final build, `audio-chain-fix2.json`, 3 min 30 s attached, logic frames 176 → 224 under the probe's slowdown)

Counts at detach:

| link | count |
|---|---|
| `AudioManager::addAudioEvent` (raised) | 491 |
| `SoundManager::canPlayNow` | 488 |
| `appendAudioRequest` (queued) | 12 |
| `playAudioEvent` (processed) | 11 |
| `playSample` / `playSample3D` / `playStream` | 5 / 234 / 0 |
| `AIL_set_sample_file` / `AIL_set_3D_sample_file` | 5 / 234 |
| `AIL_start_sample` / `AIL_start_3D_sample` / `AIL_start_stream` | 5 / 234 / 0 |

Read the shape: `playSample3D` (234) exceeds queued requests (12) because it is also called from
`notifyOfAudioCompletion` to restart loops — which is the §3.2 defect proven fixed from the engine
side. The 476 events rejected in `canPlayNow` are, by name, dominated by `TrainClicketyClack` (876 of
the name hits across the two event-bearing links) and the ambient loops that are already playing — the
duplicate/`isPlayingAlready` and voice-limit filters doing their job (this attribution of *which* filter
is from source, the count is measured). `playStream` is 0 because music was not started in this
window and speech was not triggered; that is not a defect in this slice's scope (music is measured
elsewhere).

Named events at each link (`event_names`, top entries): `TrainClicketyClack` 876,
`Amb_TemperateForestTreesLoop` 106, `TrainRunningLoop` 74, `Amb_TemperateTownDayAmbientLoop` 65,
`IndustrialYardAmbientLoop` 24, `Amb_TemperateMeadowBirdsLoop2` 17, `Amb_TemperateMeadowAmbientLoop` 16,
**`WorkerVoiceMove` 14, `RallyPointSet` 12, `WorkerVoiceSelect` 6, `CommandCenterGLASelect` 3**,
`Amb_TemperateForestBirds` 1, `Amb_DesertVillageDayDog` 1. The bold ones are player actions performed
during the run (select the Command Center, select a worker, order it to move, set a rally point).

## 4. The fix

`Core/GameEngineDevice/Source/MilesAudioDevice/MilesAudioManager.cpp`, `openDevice`:

```cpp
UnsignedInt providerNdx = TheAudio->getProviderIndex(m_pref3DProvider);
if (providerNdx == PROVIDER_ERROR && m_providerCount > 0 &&
        getProviderIndex("Miles Fast 2D Positional Audio") == PROVIDER_ERROR)
{
    providerNdx = 0;
}
selectProvider(providerNdx);
```

The guard is deliberately narrow: it only changes the argument when the preference *and* Miles'
software provider are both absent and at least one provider exists — i.e. only on a non-Miles backend.
On Windows with Miles, `Miles Fast 2D Positional Audio` always enumerates, so the condition is false and
`selectProvider` receives exactly what it did before. `selectProvider` itself is unchanged; its existing
`providerNdx >= m_providerCount` fallback then picks the same provider 0.

The callback widening: `notifyOfAudioCompletion(uintptr_t, UnsignedInt)` in `GameAudio.h`,
`MilesAudioManager.h/.cpp` (`findPlayingAudioFrom` likewise) and the null manager; the three
`AILCALLBACK` shims cast to `uintptr_t`.

Not changed: `selectProvider`'s Dolby/DirectSound branch (`#ifdef _WIN32`), the music/stream path, the
renderer, `milesstub`. No `AIL_*` entry point was added or altered.

## 5. Device-level evidence (Linux x86-64, OpenAL Soft `wave` backend — SYNTHETIC-ONLY for audibility)

### 5.1 Engine pools and provider (measured on the final build, every sample)

`provider 0/1`, `pools 2D=4 3D=25`, `OpenALAudio::lib().started` 1, `.samples` 4, `.objects` 26,
`m_listener` non-null, `sound_on`/`sound3d_on`/`speech_on` 1, `in_game` 1.

### 5.2 Live voices, named, with `AL_SOURCE_STATE` and `AL_SAMPLE_OFFSET` read from OpenAL

Representative probe lines (one per `MilesAudioManager::update` stop):

```
frame=206 ... AL_PLAYING {"samples": 1, "objects": 5} | engine-playing 2D=1 3D=7
   live [WorkerVoiceMove:src2@19968, Amb_TemperateMeadowBirdsLoop2:src28@..., ...]
frame=211 ... AL_PLAYING {"samples": 1, "objects": 7} | engine-playing 2D=1 3D=7
   live [CommandCenterGLASelect:src2@46080, Amb_TemperateMeadowBirdsLoop2:src28@28077,
         Amb_TemperateTownDayAmbientLoop:src26@36634, IndustrialYardAmbientLoop:src25@100864,
         Amb_TemperateTownDayAmbientLoop:src24@95802, Amb_TemperateMeadowAmbientLoop:src23@7625, ...]
```

Engine-list join for the same sources across consecutive stops (name, AL source, `AL_PLAYING`,
`AL_SAMPLE_OFFSET`):

```
frame 209: IndustrialYardAmbientLoop src25 True 1024      Amb_TemperateMeadowAmbientLoop src23 True 8342
frame 210: IndustrialYardAmbientLoop src25 True 47616     Amb_TemperateMeadowAmbientLoop src23 True 23835
frame 211: IndustrialYardAmbientLoop src25 True 100864    Amb_TemperateMeadowAmbientLoop src23 True 7625  (looped)
```

The offsets advance between stops on the same source and wrap when a loop restarts; a source that had
finished shows `AL_PLAYING False, offset 0` for at most one stop before the engine releases it
(`engine-playing 3D` 9 → 7, `available_3d` 16 → 18 across frames 213–216), which is the §3.2 fix seen
from the device side. In §3.2's run the same columns were `available 0/0`, `engine-playing 3/25`,
`AL_PLAYING 0/0` forever.

### 5.3 Buffer submission (interposer)

69 `alSourcePlay`, 214 buffer operations (`alBufferData`, `alSourcei(AL_BUFFER)`,
`alSourceQueueBuffers`) in one ~1 minute log; every `alSourcePlay` line reads back `state=0x1012`
(`AL_PLAYING`) with `queued=1`.

### 5.4 Mixed output — SYNTHETIC-ONLY

`ALSOFT_CONF` `drivers = wave` wrote `openal-out.wav`: RIFF/WAVE, format 0xFFFE (extensible), 2 channels,
44100 Hz, 32-bit float, 148,389,888 bytes ≈ 280 s of mix. Sampled non-zero content across many seconds,
peak |amplitude| ≈ 0.42. This proves OpenAL Soft mixed the voices into its output stream. It does
**not** prove a physical device played them: no ALSA/PulseAudio/PipeWire device was opened in this
session (there is none on the CI-style VM), so **nothing was heard**, and this is reported as
`SYNTHETIC-ONLY`. The `wave` backend's mixing is the same code that feeds a real backend, so the
remaining unknowns are device-open and output-format negotiation on a real device, which are OpenAL
Soft's responsibility and are exactly the things the M1 Pro music measurement already exercised.

### 5.5 Build

`scripts/native-build.py --level 1 --level 2 --level 3 --level 4 --with-shims --strict-link`:
980/980 objects, 0 unresolved, executable produced (83.3 MiB ELF x86-64);
`check-native-build-baseline.py` reports no regression, baseline JSON unchanged.

## 6. What remains, ranked (evidence, then cost)

1. **Apple Silicon confirmation of both fixes — UNMEASURED.** Both defects are platform-independent
   in mechanism (the sentinel collision needs only a non-Miles backend; the truncation needs only 64-bit
   pointers, and arm64 heap addresses exceed 32 bits like Linux's), so they are expected to reproduce
   and to be fixed identically, but that is inference. Cost: one M1 Pro session with
   `scripts/macos-playability-probe.py audio` reading `OpenALAudio::lib().samples/.objects`,
   `m_num3DSamples` and `AL_PLAYING` in a skirmish; and a human saying whether they heard a worker
   answer. This is the row `playability-probe.md` §9 item 2 needs before it can be closed.
2. **Nobody has heard a sound effect on any platform.** Linux evidence stops at the mixer (§5.4). Cost:
   run once with a real OpenAL Soft device (`drivers = pulse`/`alsa`) on a machine with speakers, or
   item 1.
3. **Speech and EVA were not exercised** (`playStream` 0; no speech event in the named list). The
   `ST_STREAM` path is the music path plus `AudioEventInfo` speech entries; speech uses
   `playSample`/`playSample3D` like effects when not streamed, and EVA goes through the same
   `addAudioEvent`. Nothing in the trace suggests a separate defect, but it is unmeasured. Cost: a
   skirmish long enough for an EVA line (building complete, under attack), ~10 minutes attached, with
   `Eva::playSound` added to the probe's `LINKS`.
4. **Weapon-fire sounds were not in the named list** — the run ended before combat; `FiringTracker`
   raises them through the same `addAudioEvent` and `combat-probe.md` shows combat resolves, so the
   remaining question is only voice-limit pressure (25 3D voices, 7 of them held by ambient loops on
   this map, so a firefight will hit `violatesVoice`/priority eviction). That is retail behaviour, not a
   port defect, but its *audible* result on a 25-voice pool is unmeasured. Cost: same run as item 3.
5. **Callback thread affinity.** `notifyOfAudioCompletion` now finds its entry and mutates
   `m_playing3DSounds` from the OpenAL service thread, which is also what Miles did (its callbacks ran
   on Miles' background thread), so this is original design rather than a new hazard; but the original
   relied on Miles' locking around `AIL_*` calls made from the main thread at the same time. No crash or
   list corruption occurred in ~10 minutes of probed play (the probe's list walks stayed consistent).
   Unmeasured under load. Cost: a TSan build of the audio libraries, or a long soak with
   `-fsanitize=thread` on `MilesAudioManager.cpp` + `OpenALDriver.cpp` only.
6. **Focus mute** (`m_muteReasonBits`, documented in `playability-probe.md` §3): unchanged, not a
   defect, but anyone measuring audio through a harness that steals focus will see silence and must
   click into the window first. Cost: nothing; documented here so it is not re-discovered.

## 7. What could not be measured, and what it would take

| item | status | needs |
|---|---|---|
| Sound effects on Apple Silicon (M1 Pro, CoreAudio) | UNMEASURED | the outpost, one skirmish, `macos-playability-probe.py audio` |
| Audibility on any platform | UNMEASURED (SYNTHETIC-ONLY mixer output only) | a machine with an audio device and a listener |
| Speech / EVA | UNMEASURED | longer skirmish, `Eva::playSound` in the probe |
| Weapon fire under voice-limit pressure | UNMEASURED | combat in the probed window |
| Windows build of the widened virtual | **built**: `scripts/docker-build.sh --game zh` (Wine/VC6) on the rebased head, 1363/1363 targets, `generalszh.exe` linked | — |
| Retail replay gate | not run locally (Wine is a differential check only) | the Windows CI replay job on the PR; audio is not on the CRC path, but the gate is the proof |
| Callback thread safety under load | UNMEASURED | TSan or a long soak |

## 8. Evidence taxonomy, for anyone reading the numbers

- **Real device evidence**: an `AL_PLAYING` source with an advancing offset on a hardware-backed
  context, plus a listener. None in this slice for sound effects.
- **OpenAL Soft evidence, `wave` backend (this slice, SYNTHETIC-ONLY)**: `AL_PLAYING`, advancing
  `AL_SAMPLE_OFFSET`, `alSourcePlay` observed, mixed float output on disk. Proves engine → seam → mixer.
- **Engine bookkeeping**: pool sizes, `PlayingAudio` lists, breakpoint counts. Proves the engine *tried*;
  used here only alongside the device reads.
- **Decoder-only evidence** (#102/#106 style): a `.wav`/`.mp3` decoded to PCM. Proves nothing about
  playback and is not used as proof anywhere in this document.
- **Source inference**: which `canPlayNow` filter rejected an event, why arm64 will behave like x86-64.
  Marked as such where it appears.
