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
| Apple Silicon | **Engine state MEASURED, Waves 11–12** (pools, started voices, OpenAL device/context; §7). The Wave 11 **PORT DEFECT** (SIGSEGV in the completion callback after 16.2 min) is **FIXED and RE-MEASURED**: Wave 12 soak on `c6fd1bd7c`, ~29 running min + 265 s paused, 20 220 callbacks, 0 off the main thread, no crash (§6 item 1). **HUMAN AUDIBILITY MEASURED, Mac / live**: music and ambient SFX audible, with a **constant crackle/pop — new PORT DEFECT** (§6 item 0), mechanism INFERRED. Recorded output UNMEASURED (no loopback). **Wave 13 (Linux)**: shim data path PROVEN clean (rendered PCM, retail music sample-identical to ffmpeg); one real-time mechanism MEASURED on a PulseAudio sink — a >1 s service-thread scheduling stall drained the 4-buffer stream queue and forced a restart (a pop) — fixed by an 8-buffer queue with deterministic red/green PCM evidence; a *constant* crackle NOT REPRODUCED; shim instrumented (`OPENAL_AUDIO_DIAG`) and `ALC_FREQUENCY=44100` requested explicitly; Mac audibility after it UNMEASURED. |
| Third broken link (found by the M1 Pro crash) | The OpenAL shim delivered end-of-sample callbacks from its own service thread; the engine's handlers (`notifyOfAudioCompletion` → `startNextLoop` → `playSample3D`) edit `m_playingSounds`/`PlayingAudio`/`AudioEventRTS` with no lock, as they did against retail Miles. Fixed below the `AIL_*` surface (§4.1): the shim now queues completions and delivers them on the thread that calls `AIL_*`, as that thread's `AIL_*` calls return. Linux soak 22 min, 16 641 callbacks, 0 off the main thread; Mac soak ~29 running min, 20 220 callbacks, 0 off the main thread, no crash (§6 item 1). |

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
    -> Voice::completionPending = true                      (nothing called back from here; §4.1)
main thread, next AIL_* call returning  OpenALAudio::deliverCompletions
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

### 4.1 Completion callbacks: delivered on the API thread, not the service thread

The crash in §6 item 1 is a threading contract the shim broke, not an engine bug, so the fix is in
`Core/Libraries/Source/OpenALAudioDevice/` only (the shim does not compile on Windows; the engine and
the Windows binary are untouched).

**What retail Miles did — INFERRED, not proven from a vendor quote.** The Miles header this build is
pinned to (`build/docker/_deps/miles-src/mss/mss.h`, the `OmniBlade` stub of the `mss32.dll` subset the
engine imports) declares `AIL_register_EOS_callback` / `AIL_register_3D_EOS_callback` /
`AIL_register_stream_callback` and the timer API (`HTIMER`, `AIL_stop_timer`,
`AIL_get_timer_highest_delay`) but carries no documentation of the thread an EOS callback runs on, and
the repo has no Miles 6 manual. What the *engine* proves: `rg 'AIL_serve|AIL_lock|AIL_unlock'` over
`MilesAudioManager.cpp` (and all of `GeneralsMD/Code`) finds nothing — `MilesAudioManager::update()`
pumps nothing, and `notifyOfAudioCompletion`/`startNextLoop`/`playSample3D` walk and rewrite
`m_playingSounds`, `PlayingAudio` and `AudioEventRTS` with no synchronisation of any kind. That code
shipped and ran for hours against retail Miles, so either Miles ran the EOS callbacks on the caller's
thread inside its own `AIL_*` calls, or its background thread's callbacks happened to serialise with the
main thread in a way this code never had to think about. Either way the engine was written to, and only
works under, an effectively single-threaded contract: **completions are observed between the engine's
own `AIL_*` calls, on the thread that makes them.** The shim now provides exactly that contract, which
is safe regardless of which of the two Miles actually did — it is the stricter of the two.

**The change.** `OpenALAudio::serviceLoop` (`OpenALDriver.cpp`) still polls `AL_SOURCE_STATE`
and refills streams on its thread, but on a voice running dry it only sets `Voice::completionPending`.
Every `AIL_*` entry point that touches a sample, 3D object or stream constructs an `ApiCall`; when the
outermost `ApiCall` on the API thread (the thread that called `AIL_startup`) is destroyed,
`deliverCompletions()` collects the pending `(callback, handle)` pairs under the library lock, clears the
flags, and invokes the callbacks **with the lock released** (the handlers re-enter `AIL_*`:
`startNextLoop` → `AIL_start_3D_sample`, and may release the handle they were told about). Nested
`AIL_*` calls made from inside a callback do not drain again (`apiDepth`). Starting, resuming or ending
a voice clears its pending flag, so a voice the engine reused before the drain never reports a stale
completion. The comment that claimed "the engine's handlers take `AIL_lock` themselves" was false and is
gone.

**Red/green at the shim level** — `scripts/native-audio-callback-test.py` builds
`Core/Libraries/Source/OpenALAudioDevice/tests/openal_callback_thread_test.cpp` against the shim,
drives 2D/3D/stream voices to completion through the public `AIL_*` surface with synthetic WAVE data
and records on which thread, and whether inside an `AIL_*` call, each callback arrived:

| check | `--shim-rev e1f8de610` (before) | working tree (after) |
|---|---|---|
| callbacks while the API thread is idle (out of the library) | 4 (all loop callbacks; service thread) | 0 |
| 2D / 3D / stream completion on the API thread, inside an `AIL_*` call | 0 / 0 / 0 of 1 each | 1 / 1 / 1 |
| callback after the voice was retired (unregistered) | 1 delivered | 0 |
| handler restarting its own 3D voice from the callback (3 restarts) | 4 callbacks, 0 on the API thread | 4 callbacks, 4 on the API thread, inside an `AIL_*` call |
| verdict | defect observed | pass (5 consecutive runs) |

**Live game, Linux, 22 minutes** — `scripts/linux-audio-soak-probe.py` attached with gdb to a
lavapipe skirmish (China vs GLA AI, sound effects on, OpenAL Soft `wave`), main thread identified from
the first `MilesAudioManager::update`, breakpoints on the three engine EOS callbacks and on
`notifyOfAudioCompletion`/`startNextLoop` (`docs/porting/ci-baselines/audio-callback-soak.json`):
1320 s wall, no exit; `setSampleCompleted` 9, `set3DSampleCompleted` 16 632, `setStreamCompleted` 0;
all 16 641 callbacks entered from `OpenALAudio::deliverCompletions`, none from anywhere else, **0 off
the main thread**; `notifyOfAudioCompletion` 16 640, `startNextLoop` (looping sound restarted) 16 631,
both 0 off the main thread. Under gdb's breakpoint overhead the game ran ~8 fps (5:31 of game time in
the 22 wall minutes), which is why the M1 Pro's 16.2-minute figure is not comparable. Apple Silicon:
re-measured in Wave 12 on `c6fd1bd7c` — `ci-baselines/audio-callback-soak-macos-arm64.json`, §6
item 1.

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

0. **PORT DEFECT, HUMAN AUDIBILITY, Mac / live, 2026-09-03, `main` `c6fd1bd7c`: a constant
   crackle/pop under all audio.** The project's owner listened at the M1 Pro during the Wave 12 run:
   main-menu music is audible, skirmish ambient SFX (birds) are audible, and both carry a constant
   crackle/pop. The crackle is present in the main menu with music alone, so it is not a
   voice-count/CPU effect of 25 3D voices. Not fixed here (measurement slice). Cheap evidence for
   the mechanism, none of it from inside the game or from the debugger calling OpenAL:
   - Output device (standalone `AudioObjectGetPropertyData` tool): `MacBook Pro Speakers`, nominal
     44 100 Hz, 2 channels, Float32, HAL buffer 512 frames (11.6 ms; allowed 14–4096), device
     latency 65 frames, safety offset 74. The shim opens `alcOpenDevice(nullptr)` +
     `alcCreateContext(device, nullptr)` — no `ALC_FREQUENCY`/`ALC_REFRESH` attributes — so the
     context rate is alsoft's default and the effective rate is **UNMEASURED** (reading it is an
     `alcGetIntegerv`, forbidden from the debugger; Wave 13 can log it once at `AIL_startup`).
   - Stream refill (source, `OpenALStream.cpp` `serviceStream`, `OpenALAudioInternal.h`):
     `BUFFER_COUNT = 4` buffers (Wave 12 state; 8 after Wave 13) of up to `BUFFER_FRAMES = 8192`
     PCM frames each — 186 ms per buffer at 44.1 kHz (372 ms at 22.05 kHz), 0.74–1.5 s queued.
     Refill runs **only on the shim's service thread** (`serviceLoop` polls every 10 ms under the library lock; `serviceStream` has
     no other caller), never inside an `AIL_*` call on the engine thread, so a late render frame
     cannot starve the music queue. The shim mirror's `framesQueued` read 0 throughout while
     `framesPlayed` advanced (the field is not maintained by the refill path), so the live queue
     depth is **UNMEASURED**.
   - The 10 ms poll does hold `Library::lock` for every pass over 25 3D voices + 4 samples + the
     stream (`alGetSourcei` each); the engine's `AIL_*` calls take the same recursive lock.
   - Recorded output: **UNMEASURED** — no `ffmpeg`, BlackHole or aggregate loopback on this Mac; a
     ScreenCaptureKit system-audio capture proved itself on a system sound (−26.7 dBFS peak) but
     recorded the game as digital silence (−120 dBFS in every 0.5 s window, PID-filtered and
     unfiltered), so it never reached the game's CoreAudio client. Linux lavapipe soak audio:
     **UNMEASURED** (the `wave` backend writes a file nobody listened to; no output route).
   Candidate mechanisms, each **INFERRED** until Wave 13 measures it: (a) alsoft mixer underrun on
   the CoreAudio IOProc (period 512 frames, 11.6 ms) — alsoft's `renderSamples` runs on its own
   IO thread and takes alsoft's source/context locks, which the shim's 10 ms poll (25 `alGetSourcei`
   per pass) and the engine's `alSource*` calls also take; whether the IOProc ever misses its
   deadline needs an underrun counter, not a guess; (b) device/context
   sample-rate mismatch (22.05 kHz retail assets → alsoft context → 44.1 kHz device) with resampler
   artefacts; (c) alsoft period/buffer size too small for MoltenVK-loaded main thread jitter (an
   `ALC_REFRESH` hint or `alsoft.conf period_size`); (d) 3D-sample restart gaps — each of the
   ~20 000 EOS callbacks per soak restarts a loop via `startNextLoop` → `playSample3D`, and a gap
   between `AL_STOPPED` (seen by the 10 ms poll) and the restart on the next `AIL_*` call is
   audible as a click on a looping ambient (the birds). (d) would not explain the menu, where
   only the stream plays, unless the menu also loops a 2D sample (48 `setSampleCompleted` per
   soak; 42 of them before the mission). Cost: `alsoft` `ALSOFT_LOGLEVEL=3` + a one-line
   `alcGetIntegerv(ALC_FREQUENCY/ALC_REFRESH)` log at startup, and a loopback device.

   **Wave 13 (Linux, this slice): the data path is ruled out, the real-time path is instrumented,
   one starvation mechanism is MEASURED live and fixed with deterministic red/green evidence, and
   a *constant* crackle did NOT reproduce on Linux.** The
   shim now has opt-in diagnostics (`OPENAL_AUDIO_DIAG=stderr|<path>`, `OpenALDriver.cpp`
   `diagnostics()`): implementation and effective context attributes at `AIL_startup`, and
   counters for stream starvation (`stream_queue_emptied`: every buffer processed before a refill;
   `stream_stopped_with_data`: `AL_STOPPED` with data queued → forced restart), `stream_queued_min`,
   `stream_service_gap_max_us`, one-shot/3D `*_restarts_while_playing`, `alBufferData` declared
   format vs decoded bytes (`buffer_data_mismatches`), gain/position write counts, and lock hold /
   wait maxima on the service and API threads. (Wave 14: `Diagnostics` is a heap-allocated,
   never-freed singleton and a file log is closed by `~Library` via `diagnosticsClose()`, so the
   service thread and the destructor can use it at any point of static destruction; a
   `static destruction: AIL_shutdown was not called` line means the engine quit route skipped
   `AIL_shutdown` — see `memory-shutdown-order.md`, third mechanism, and
   `ci-baselines/quit-path-static-destruction.json`.) Measured with them, all on `main` `b905296b3`'s
   shim unless stated (`ci-baselines/audio-render-discontinuity.json`):
   - **Which OpenAL the M1 Pro links — PROVEN from the Wave 11 Mac backtrace**
     (`renderer-integration-arm64.md`: `libopenal.1.dylib std::basic_ofstream::~basic_ofstream`):
     a C++ `libopenal.1.dylib` is OpenAL Soft (Homebrew keg, `scripts/native-build.py`
     `OPENAL_CANDIDATES`), not Apple's `OpenAL.framework`. Candidate (iv) is closed; no pin needed.
   - **Data path clean — PROVEN, rendered PCM, Linux x86-64, OpenAL Soft 1.19.1 `wave`
     (file backend, SYNTHETIC, cannot underrun).** Retail shell music `USA_11.mp3` (full-archive
     `Data/Audio/Tracks`) rendered through `AIL_open_stream`/minimp3 for 45 s is sample-identical
     to an ffmpeg decode of the same file (0 discontinuities beyond the 9 the source material has
     at a 0.3 jump threshold; both files report the same 9). A retail bird one-shot rendered
     through `AIL_set_sample_file`/`AIL_start_sample` has 0 jumps and 0 gaps. Synthetic 440 Hz
     tones through all three paths (stream, one-shot, EOS-callback loop) have 0 jumps at a 0.1
     threshold, on `main` and after the fix alike (`scripts/native-audio-render-test.py`).
     `buffer_data_mismatches=0` across every upload. Candidates (b: format/decl) and (d: restart
     cutting a playing source) do not occur: `sample_restarts_while_playing=0`,
     `object_restarts_while_playing=0` over 2 514 3D starts in game.
   - **Real-time path, Linux, PulseAudio null sink with a real clock (OpenAL Soft `pulse`
     backend), lavapipe `zh`, main menu + skirmish, pre-fix shim (4 buffers):**
     `stream_queue_emptied=1`, `stream_stopped_with_data=1`, `stream_queued_min=0`,
     `stream_service_gap_max_us≈1 140 000` against 743 ms of queued music, `al_errors=0`. **That
     is the one starvation mechanism MEASURED in this slice:** the service thread (the shim's
     10 ms poller) was not scheduled for >1 s — longer than the whole queued stream — the queue
     drained, OpenAL Soft reported the source `AL_STOPPED` with buffers still queued, and the shim
     force-restarted it: a gap in the music, i.e. a pop. Lock contention is NOT the cause of the
     stall: `service_hold_max_us=3 999`, `api_hold_max_us=3 638`, `service_wait_max_us=3 536`,
     `api_wait_max_us=638` — milliseconds, two orders of magnitude below the queue — so
     candidate (ii)'s "poll under the lock" part is **REFUTED on Linux** and decode stays under the
     lock. The stall is host scheduling (a software-rasterised game at 100 % CPU on a shared box).
     The stall is intermittent (once in ~10 min here), which is a **pop, not a constant crackle**:
     a constant crackle on the Mac is NOT explained by this counter unless the Mac counters show it
     firing continuously. Human audibility of the Linux capture: UNMEASURED (no listener).
   - **Fix shipped for that mechanism: stream queue depth 4 → 8 (`StreamVoice::BUFFER_COUNT`,
     1.49 s of queued 44.1 kHz audio instead of 0.74 s).** Red/green, deterministic, Linux, rendered
     PCM (`scripts/native-audio-render-test.py` case `stall`, `OPENAL_AUDIO_DIAG_STALL=1000:1200`
     makes the service thread sleep 1.2 s once, 1 s into a 6 s synthetic stream, `wave` backend):
     4 buffers → `stream_queue_emptied=1 stream_stopped_with_data=1 stream_queued_min=0`,
     `rendered_pcm_jumps=2 rendered_pcm_gaps=1` (534 ms of silence), **FAIL**; 8 buffers → all
     counters 0, `rendered_pcm_jumps=0 rendered_pcm_gaps=0`, **pass**, same input and same
     injected stall (`service_pass_gap_max_us` 1.21 s in both). Live re-run after the fix
     (pulse null sink, lavapipe, menu + skirmish, ~13 min): a natural stall of
     `service_pass_gap_max_us=1 128 259` occurred again and this time
     `stream_queue_emptied=0 stream_stopped_with_data=0 stream_queued_min=1`, `al_errors=0`,
     `buffer_data_mismatches=0`, `*_restarts_while_playing=0`. The service thread also asks for a
     best-effort priority raise (`SCHED_RR` min / `QOS_CLASS_USER_INTERACTIVE` on macOS, logged
     `accepted`/`refused`, refused unprivileged on Linux); it never fails startup.
   - **Effective context — MEASURED, Linux:** `alcCreateContext(device, nullptr)` gave
     `ALC_FREQUENCY=44100 ALC_REFRESH=43` (OpenAL Soft's default when the backend reports
     44.1 kHz). With `alsoft.conf frequency=48000` the same call, and the explicit request
     alike, give `48000/23`: alsoft's config file outranks the attribute list, so that probe
     shows the default follows the backend, not that the request is powerless. **On the Mac this
     is still UNMEASURED** (the log line now exists; nobody has run it there). **INFERRED from
     alsoft source** (`alc/backends/coreaudio.cpp` `CoreAudioPlayback::reset`, tag 1.24.3): the
     CoreAudio backend overwrites `mDevice->mSampleRate` with the output unit's stream-format
     rate regardless of the request, so on the M1 Pro's 44.1 kHz speakers the context was
     already 44.1 kHz and the fix below is expected to be a no-op there.
   - **Also shipped (candidate (i), defensive):** `AIL_startup` now
     requests `ALC_FREQUENCY = 44100` (`Library::MIXER_RATE`, the rate the engine's
     `AIL_quick_startup` asks Miles for), falls back to the attribute-less context if the
     implementation rejects the list, and reads back the effective `ALC_FREQUENCY`/`ALC_REFRESH`
     (`Library::contextFrequency/contextRefresh`). `ALC_REFRESH` is deliberately not requested:
     where alsoft honours it, it shrinks the device period and only makes real-time underruns more
     likely. Linux: attribute list accepted, effective 44 100 / 43, renders unchanged (they were
     already at 44.1 kHz here). It protects the Linux/PulseAudio and 48 kHz-DAC cases and makes
     the rate observable; **it is NOT claimed to fix the Mac crackle** — see the CoreAudio
     inference above; the new startup line is what confirms or refutes that.
   - **Still INFERRED, and the Mac measurement that decides it:** run `OPENAL_AUDIO_DIAG=stderr
     ./zh` on the M1 Pro through the menu and a 10-minute skirmish, then read (1) the
     `implementation`/`context` lines — a `frequency` ≠ 44100 before this fix or an
     `attributes_accepted=0` names the rate path; (2) `stream_queue_emptied` /
     `stream_stopped_with_data` / `service_pass_gap_max_us` — non-zero starvation, or a service
     gap approaching 1.49 s, names the scheduling mechanism measured on Linux (a count that grows
     steadily is the first thing that could explain a *constant* crackle);
     (3) if both are clean and the crackle persists, the mechanism is below the shim (candidate
     (a): alsoft's `renderSamples` missing the 11.6 ms CoreAudio IOProc deadline), and the next
     lever is `alsoft.conf` `period_size`/`periods` or alsoft's own `ALSOFT_LOGLEVEL=3` output,
     not shim code. Human audibility after the fix, Mac: **UNMEASURED**, owed to the next Mac
     slot. #153 regression on this slice: `scripts/native-audio-callback-test.py` green (all
     completions on the API thread inside an `AIL_*` call, 0 idle deliveries); a live 10-minute
     callback soak did not run here because this session's lavapipe skirmish ended in an instant
     "You are victorious" screen (no starting units placed — an unrelated, unexplained Linux
     run-directory issue), so the live callback count is **UNMEASURED for this slice**.
1. **PORT DEFECT, measured on the M1 Pro, FIXED in the shim (§4.1), RE-MEASURED on the Mac
   (`ci-baselines/audio-callback-soak-macos-arm64.json`): SIGSEGV in
   `MilesAudioManager::initFilters3D` on the OpenAL service thread.** 16.2 resumed
   minutes into a real-input skirmish, the end-of-sample callback delivered by
   `OpenALAudio::serviceLoop` (then `OpenALDriver.cpp:138`) restarted a looping 3D
   sound (`notifyOfAudioCompletion` → `startNextLoop` → `playSample3D` → `initFilters3D`) and read
   `event->getAudioEventInfo()->m_lowPassFreq` through a null info pointer (fault address `0xd8` =
   `offsetof(AudioEventInfo, m_lowPassFreq)`, verified with `expr` on the binary).
   `getAudioEventInfo()` returns null when the cached info's name no longer matches the event, so the
   `PlayingAudio`'s event had been reassigned/torn down by the main thread under the callback.
   `MilesAudioManager` takes no `AIL_lock`; nothing serialises the two threads. macOS wrote
   `zh-2026-09-03-020314.ips`; no debugger expression was running. Full frames and mechanism in
   `playability-probe.md` §1.3. Fix: the shim queues completions and delivers them on the API thread
   as its `AIL_*` calls return (§4.1); Miles' own EOS thread affinity stays INFERRED (no vendor doc in
   the pinned deps), the fix is the stricter contract so it is safe either way. Linux: shim test red
   on `e1f8de610`, green after; 22-minute lavapipe skirmish soak, 16 641 callbacks, 16 631 loop
   restarts, 0 off the main thread, no crash. **Mac, 2026-09-03, `main` `c6fd1bd7c`**: a real-input
   Alpine Assault skirmish (USA vs one Easy AI) ran ~29 running minutes plus a 265 s paused stretch
   (in-game clock 00:23:16 at quit), 20 220 EOS callbacks in the probed 18:40–19:06 window
   (48 `setSampleCompleted`, 20 171 `set3DSampleCompleted`, 1 `setStreamCompleted`), **0 off the
   main thread**; all 78 sampled stacks ran `setXSampleCompleted ← deliverCompletions ←
   ApiCall::~ApiCall ← AIL_set_3D_orientation / AIL_stream_loop_count` on the engine thread; no
   SIGSEGV, DiagnosticReports 37 → 37; quit from the skirmish exited 0. Cost remaining: none for
   the crash.
2. **Apple Silicon `AL_PLAYING` by function evaluation — probe defect, still open in the checked-in
   script.** The checked-in `SOURCE_STATE_COUNT` calls `alGetSourcei` from LLDB while all threads
   are stopped and twice wedged OpenAL Soft's source mutex. Waves 11 and 12 read the shim's own
   bookkeeping by memory instead: `Voice::started && !paused` counted 7–17 started 3D voices of 25
   across the Wave 12 samples (0 while paused), started 2D samples 0, pools `.samples=4`,
   `.objects=26`, `m_num2DSamples=4`, `m_num3DSamples=25`, non-null OpenAL device/context, music
   `framesPlayed` 672 768 → 7 759 872. Cost: replace `SOURCE_STATE_COUNT` with the memory read or
   read alsoft's source struct.
3. **Sound effects have now been heard on one platform — with a defect (item 0).** Mac: music and
   ambient SFX audible to a human, with constant crackle. Linux evidence still stops at the mixer
   (§5.4). Weapon-fire and unit-acknowledgement classes were playing by the engine mirror (17
   started 3D voices mid-attack) but the listener report names only music and birds; per-class
   audibility beyond those two is UNMEASURED.
4. **Speech and EVA were not exercised** (`playStream` 0; no speech event in the named list). The
   `ST_STREAM` path is the music path plus `AudioEventInfo` speech entries; speech uses
   `playSample`/`playSample3D` like effects when not streamed, and EVA goes through the same
   `addAudioEvent`. Nothing in the trace suggests a separate defect, but it is unmeasured. Cost: a
   skirmish long enough for an EVA line (building complete, under attack), ~10 minutes attached, with
   `Eva::playSound` added to the probe's `LINKS`.
5. **Weapon-fire sounds were not in the named list** — the run ended before combat; `FiringTracker`
   raises them through the same `addAudioEvent` and `combat-probe.md` shows combat resolves, so the
   remaining question is only voice-limit pressure (25 3D voices, 7 of them held by ambient loops on
   this map, so a firefight will hit `violatesVoice`/priority eviction). That is retail behaviour, not a
   port defect, but its *audible* result on a 25-voice pool is unmeasured (the Mac run's peak was
   17 of 25 started). Cost: same run as item 3.
6. **Callback thread affinity**: item 1 was the crash this paragraph predicted; after §4.1 the shim's
   callbacks are on the API thread by construction (measured: 0 of 16 641 off it on Linux, 0 of
   20 220 off it on the M1 Pro). What
   Miles itself did stays unverified (INFERRED). Cost: a TSan build of `MilesAudioManager.cpp` +
   `OpenALDriver.cpp` would show whether any *other* shim→engine crossing remains (the service thread
   still reads `Voice` fields under the library lock, never engine state).
7. **Focus mute** (`m_muteReasonBits`, documented in `playability-probe.md` §3): unchanged, not a
   defect, but anyone measuring audio through a harness that steals focus will see silence and must
   click into the window first. Cost: nothing; documented here so it is not re-discovered.

## 7. What could not be measured, and what it would take

| item | status | needs |
|---|---|---|
| Sound-effect pools on Apple Silicon (M1 Pro) | **MEASURED, Mac / live / real input / engine state** (Wave 12, `c6fd1bd7c`, every 5-min sample): samples 4, objects 26, 2D 4, 3D 25 | — |
| `AL_PLAYING` for effects on Apple Silicon | **UNMEASURED by `alGetSourcei`** (probe defect: LLDB function evaluation wedges alsoft's source mutex); **MEASURED, engine state**, via the shim's `Voice::started` mirror: 7–17 of 25 started 3D voices across the Wave 12 samples (0 while paused), 0 started 2D samples, music `framesPlayed` advancing | replace `SOURCE_STATE_COUNT` with a memory read |
| Sound-effect completion callback on Apple Silicon | **MEASURED FAILURE (PORT DEFECT)** before §4.1: SIGSEGV in `initFilters3D` on the service thread at 16.2 min, `.ips` written. After §4.1: **MEASURED PASS, Mac / live / real input / engine state**: ~29 running min + 265 s paused, 20 220 callbacks in the probed window, 0 off the main thread, 78/78 sampled stacks via `deliverCompletions`, no SIGSEGV, DiagnosticReports 37 → 37 (`ci-baselines/audio-callback-soak-macos-arm64.json`) | — |
| Sound-effect completion callback on Linux, after §4.1 | **MEASURED, live / engine state**: 22 min, 16 641 callbacks all from `deliverCompletions` on the main thread, 16 631 loop restarts, no crash (`ci-baselines/audio-callback-soak.json`) | — |
| Miles 6 EOS callback thread (the retail oracle) | **INFERRED** (§4.1): the pinned `mss.h` is a stub without vendor prose; the engine takes no `AIL_lock`/`AIL_serve` | the Miles 6 SDK manual, if anyone has it; not needed for the fix |
| CoreAudio output open | **MEASURED, engine/OS state**: non-null OpenAL device/context, advancing music `framesPlayed`, and `com.apple.audio.IOThread.client` in `HALC_ProxyIOContext`; this proves the output client exists, not audibility | — |
| CoreAudio output device format (Mac) | **MEASURED, OS state, standalone tool**: `MacBook Pro Speakers`, 44 100 Hz nominal, 2 ch Float32, HAL buffer 512 frames (14–4096 allowed), latency 65 frames, safety offset 74 | — |
| OpenAL context format the shim actually got | **MEASURED, Linux** (Wave 13, `OPENAL_AUDIO_DIAG`): OpenAL Soft 1.19.1, `ALC_FREQUENCY=44100 ALC_REFRESH=43 ALC_SYNC=0`, 255 mono / 1 stereo sources; the shim now requests 44 100 explicitly and logs the readback. **UNMEASURED, Mac** | `OPENAL_AUDIO_DIAG=stderr ./zh` on the M1 Pro, read the `context` line |
| Which OpenAL the Mac binary links | **PROVEN, Mac backtrace** (Wave 11, `renderer-integration-arm64.md`): `libopenal.1.dylib` with a C++ static destructor = OpenAL Soft (Homebrew keg), not `OpenAL.framework` | — |
| Music stream queue depth / refill cadence | **MEASURED, live, Linux** (Wave 13, PulseAudio null sink, lavapipe): pre-fix 4 buffers, one >1.1 s service-thread stall → `stream_queue_emptied=1`, `stream_stopped_with_data=1` (forced restart), `stream_queued_min=0`; post-fix 8 buffers, ~13 min, `service_pass_gap_max_us=1 128 259` → `stream_queue_emptied=0`, `stream_stopped_with_data=0`, `stream_queued_min=1`. Deterministic red/green in `scripts/native-audio-render-test.py` (`stall` case). **UNMEASURED, Mac** | same run on the M1 Pro |
| Shim data path (decode → `alBufferData` → mix) | **PROVEN clean, rendered PCM, Linux `wave` (SYNTHETIC)**: retail `USA_11.mp3` sample-identical to ffmpeg; retail bird one-shot 0 jumps / 0 gaps; synthetic tones 0 jumps on `main` and fixed shim (`ci-baselines/audio-render-discontinuity.json`) | — |
| Library lock contention | **MEASURED, live, Linux** (post-fix run): `service_hold_max_us=3 999`, `api_hold_max_us=3 638`, `service_wait_max_us=3 536`, `api_wait_max_us=638`, two orders below the queue; not a starvation mechanism here | Mac numbers from the same diag run |
| Human-observed audibility — Mac | **MEASURED, HUMAN AUDIBILITY, Mac / live** (2026-09-03, owner at the M1 Pro): main-menu music audible; skirmish ambient SFX (birds) audible; **constant crackle/pop in both** — PORT DEFECT, §6 item 0. **After the Wave 13 fix: UNMEASURED** | a listener at the M1 Pro on a build with this slice, `OPENAL_AUDIO_DIAG=stderr` on |
| Crackle reproduction on Linux | **Constant crackle NOT REPRODUCED; one intermittent pop MEASURED** (Wave 13): `wave` renders have no shim-generated discontinuity and no restart cut; the PulseAudio real-time run showed one stream starvation + forced restart on a >1 s service-thread scheduling stall (pre-fix), none after the 8-buffer fix | Mac counters from the same diag run |
| Recorded (non-human) output — Mac | **UNMEASURED**: no `ffmpeg`/BlackHole/aggregate loopback on this Mac; ScreenCaptureKit system capture recorded a system sound (−26.7 dBFS) but the game as −120 dBFS silence in every 0.5 s window, so it does not tap the game's client | BlackHole or an aggregate device, then a 30 s capture and a discontinuity count |
| Human-observed audibility — Linux | **UNMEASURED** (lavapipe soak runs the `wave` backend to a file; no listener, no output route) | a Linux box with speakers or a `pulse` sink |
| Speech / EVA | UNMEASURED | longer skirmish, `Eva::playSound` in the probe |
| Weapon fire under voice-limit pressure | UNMEASURED | combat in the probed window |
| Windows build of the widened virtual | **built**: `scripts/docker-build.sh --game zh` (Wine/VC6) on the rebased head, 1363/1363 targets, `generalszh.exe` linked | — |
| Retail replay gate | not run locally (Wine is a differential check only) | the Windows CI replay job on the PR; audio is not on the CRC path, but the gate is the proof |
| Callback thread safety under load | **MEASURED FAILURE** on the M1 Pro before the fix (§6 item 1); **MEASURED PASS on Linux** after it (22 min); **MEASURED PASS on the M1 Pro** after it (~29 running min + 265 s paused, 0 of 20 220 callbacks off the main thread, no crash) | — |

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
