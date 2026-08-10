# Miles Sound System API surface used by the engine

Measured, not estimated. Enumerated by scanning every `.cpp`/`.h` in
`Core/Libraries/Source/WWVegas/WWAudio` and `Core/GameEngineDevice` for `AIL_*` identifiers and
Miles handle types, and cross-checking against the API actually declared by the fetched
`miles-sdk-stub` header (`build/docker/_deps/miles-src/mss/mss.h`, 298 lines).

## Headline numbers

| Measure | Count |
|---|---:|
| Distinct `AIL_*` identifiers referenced by the engine | **114** |
| ... of which are functions | **101** |
| ... of which are constants / compatibility macros | **13** |
| Total `AIL_*` reference sites | **232** |
| Functions declared by `miles-sdk-stub`'s `mss.h` | 104 |
| Source files that reference Miles | **10** |

These are counts over the file text as written, comments included. `scripts/audio-surface-scan.py`
reproduces them and also counts what the compiler actually sees (comments and string literals
stripped): **105** identifiers, **94** functions, **11** constants, **217** sites. The 9-identifier
difference is entirely names that survive only inside `Upgrades miles call from legacy …` comments,
i.e. entry points the engine no longer calls; see `docs/porting/audio-device-seam.md` §1. The site
total was previously given as 230 here because the per-file table below undercounted
`WWAudio/Utils.h` by 2 (both in a comment); every other per-file figure was exact.

The plan document's earlier characterisation of audio as "roughly ten call sites" is wrong by an
order of magnitude. There are 232 call sites across 101 distinct entry points. The surface is
still small *relative to Miles as a whole* — the engine uses no MIDI, no DLS, no ASI decoder
plumbing beyond one filter provider — but it is not a ten-line shim.

## Files that reference Miles

| File | `AIL_*` references |
|---|---:|
| `Core/GameEngineDevice/Source/MilesAudioDevice/MilesAudioManager.cpp` | 104 |
| `Core/Libraries/Source/WWVegas/WWAudio/WWAudio.cpp` | 43 |
| `Core/Libraries/Source/WWVegas/WWAudio/soundstreamhandle.cpp` | 25 |
| `Core/Libraries/Source/WWVegas/WWAudio/sound2dhandle.cpp` | 24 |
| `Core/Libraries/Source/WWVegas/WWAudio/sound3dhandle.cpp` | 17 |
| `Core/Libraries/Source/WWVegas/WWAudio/Sound3D.cpp` | 8 |
| `Core/Libraries/Source/WWVegas/WWAudio/FilteredSound.cpp` | 4 |
| `Core/Libraries/Source/WWVegas/WWAudio/Listener.cpp` | 2 |
| `Core/Libraries/Source/WWVegas/WWAudio/Utils.h` | 4 |
| `Core/Libraries/Source/WWVegas/WWAudio/SoundBuffer.cpp` | 1 |

Headers that pull in `mss.h`: `WWAudio/AudibleSound.h`, `WWAudio/SoundBuffer.h`,
`WWAudio/WWAudio.h`, `WWAudio/Utils.h` (all as `#include "mss.h"`) and
`Core/GameEngineDevice/Include/MilesAudioDevice/MilesAudioManager.h` (as `#include "mss/mss.h"`).

## Handle types

Miles handles are opaque pointers or ints. Both spellings appear in engine headers, so the
replacement must keep the same names and pointer-ness.

| Type | Meaning | Used in |
|---|---|---|
| `HDIGDRIVER` | digital output device | `WWAudio.h`, `MilesAudioManager.h`, `soundhandle.h` |
| `HSAMPLE` | 2D sample voice | `WWAudio.h`, `sound2dhandle.h`, `soundstreamhandle.h`, `soundhandle.h` |
| `H3DSAMPLE` / `H3DPOBJECT` | 3D sample voice / positional object (listener) | `WWAudio.h`, `sound3dhandle.h` |
| `HSTREAM` | streaming voice | `soundstreamhandle.h`, `MilesAudioManager.h` |
| `HPROVIDER` | 3D provider or pipeline-filter provider | `WWAudio.h`, `FilteredSound.h`, `MilesAudioManager.h` |
| `HTIMER` | Miles timer | `WWAudio.h` |
| `HAUDIO` | "quick" one-shot audio (speech) | `MilesAudioManager.cpp` |
| `HPROENUM` | provider enumeration cursor | `WWAudio.cpp`, `MilesAudioManager.cpp` |
| `AILSOUNDINFO` | parsed WAV/ADPCM description | `SoundBuffer.cpp`, `MilesAudioManager.cpp` |
| `AILLPDIRECTSOUND` | raw DirectSound escape hatch | `MilesAudioManager.cpp` (Bink handoff) |

`INVALID_MILES_HANDLE` (engine-side, `soundhandle.h`) is compared against all of these, so all
handle types must remain castable from an integer sentinel.

## Functional grouping

The 101 functions fall into nine groups. This grouping is what the OpenAL implementation is
organised around.

### 1. Library lifecycle and preferences (7)
`AIL_startup`, `AIL_shutdown`, `AIL_quick_startup`, `AIL_quick_handles`, `AIL_set_preference`,
`AIL_set_redist_directory`, `AIL_last_error`

Only two preferences are ever set: `AIL_LOCK_PROTECTION` and `DIG_USE_WAVEOUT`. Neither has an
OpenAL analogue; both become no-ops that report success.

### 2. Digital driver / device (3)
`AIL_waveOutOpen`, `AIL_waveOutClose`, `AIL_get_DirectSound_info`

`WWAudio.cpp` opens the 2D driver via `AIL_waveOutOpen` twice — once to probe whether the driver
is DirectSound-emulated (it reads `HDIGDRIVER::emulated_ds`) and once for real. So `HDIGDRIVER`
cannot be fully opaque: the `emulated_ds` member must exist. `AIL_get_DirectSound_info` exists
only to hand a DirectSound buffer to Bink; on OpenAL it must return null pointers, and the
Bink path is already Windows-only.

### 3. Voice allocation (6)
`AIL_allocate_sample_handle`, `AIL_release_sample_handle`, `AIL_allocate_3D_sample_handle`,
`AIL_release_3D_sample_handle`, `AIL_init_sample`, `AIL_set_sample_processor`

The engine preallocates a fixed pool of voices at startup (`MilesAudioManager.cpp` allocates
`m_num2DSamples` 2D and `m_num3DSamples` 3D handles) and never allocates during play. This maps
cleanly onto a preallocated pool of OpenAL sources.

### 4. 2D sample control (18)
`AIL_set_named_sample_file`, `AIL_set_sample_file`, `AIL_start_sample`, `AIL_stop_sample`,
`AIL_resume_sample`, `AIL_end_sample`, `AIL_sample_volume`, `AIL_set_sample_volume`,
`AIL_sample_pan`, `AIL_set_sample_pan`, `AIL_sample_volume_pan`, `AIL_set_sample_volume_pan`,
`AIL_sample_loop_count`, `AIL_set_sample_loop_count`, `AIL_sample_ms_position`,
`AIL_set_sample_ms_position`, `AIL_sample_playback_rate`, `AIL_set_sample_playback_rate`

Note the two volume conventions: `AIL_sample_volume` is integer 0..127, `AIL_set_sample_volume_pan`
takes float 0..1 volume and 0..1 pan (0.5 = centre). Both are used; the replacement must honour
both.

### 5. 3D sample control (15)
`AIL_set_3D_sample_file`, `AIL_start_3D_sample`, `AIL_stop_3D_sample`, `AIL_resume_3D_sample`,
`AIL_end_3D_sample`, `AIL_3D_sample_volume`, `AIL_set_3D_sample_volume`,
`AIL_set_3D_sample_distances`, `AIL_set_3D_sample_effects_level`, `AIL_set_3D_sample_occlusion`,
`AIL_3D_sample_loop_count`, `AIL_set_3D_sample_loop_count`, `AIL_3D_sample_offset`,
`AIL_set_3D_sample_offset`, `AIL_3D_sample_length`, `AIL_3D_sample_playback_rate`,
`AIL_set_3D_sample_playback_rate`

`AIL_set_3D_sample_distances(sample, max, min)` — max first — maps to `AL_MAX_DISTANCE` /
`AL_REFERENCE_DISTANCE`. Occlusion and effects level have no core-OpenAL equivalent; they are
approximated with a gain reduction and documented as approximate.

### 6. Positional / listener (5)
`AIL_set_3D_position`, `AIL_set_3D_orientation`, `AIL_set_3D_velocity_vector`,
`AIL_open_3D_listener`, `AIL_close_3D_listener`

`AIL_set_3D_position` and `AIL_set_3D_orientation` take an `H3DPOBJECT`, which is *either* a 3D
sample or the listener — the same call is used for both. The implementation therefore needs a
tagged object, not two separate types.

### 7. Providers and filters (7)
`AIL_enumerate_3D_providers`, `AIL_open_3D_provider`, `AIL_close_3D_provider`,
`AIL_set_3D_speaker_type`, `AIL_enumerate_filters`, `AIL_set_filter_sample_preference`,
plus the `AIL_3D_*_SPEAKER` / `AIL_3D_HEADPHONE` / `AIL_3D_SURROUND` constants

Only one filter is ever looked up by name: the reverb filter in `WWAudio.cpp`, and a "Mono Delay"
filter in `MilesAudioManager.cpp` (`"Mono Delay Time"`, `"Mono Delay"`, `"Mono Delay Mix"`).
OpenAL Soft's EFX could implement these; the initial port enumerates a single synthetic provider
and accepts filter preferences without applying them.

### 8. Streaming (16)
`AIL_open_stream`, `AIL_open_stream_by_sample`, `AIL_close_stream`, `AIL_start_stream`,
`AIL_pause_stream`, `AIL_stream_volume`, `AIL_set_stream_volume`, `AIL_stream_pan`,
`AIL_set_stream_pan`, `AIL_stream_volume_pan`, `AIL_set_stream_volume_pan`,
`AIL_stream_loop_count`, `AIL_set_stream_loop_count`, `AIL_set_stream_loop_block`,
`AIL_stream_ms_position`, `AIL_set_stream_ms_position`, `AIL_stream_playback_rate`,
`AIL_set_stream_playback_rate`

Streams are opened *by filename* (`AIL_open_stream(driver, "path", 0)`), and Miles reads the file
through the callbacks installed by `AIL_set_file_callbacks`. That indirection is how the engine
streams music out of its own archive layer, so the OpenAL implementation must route stream I/O
through the same callbacks rather than calling `fopen` directly.

`MilesAudioManager.cpp` also uses `AIL_open_stream` purely to *measure* a track's length
(`AIL_stream_ms_position(stream, &total, nullptr)` then close), so the total-length field must be
populated at open time, before playback.

### 9. Callbacks, file I/O, decoding, misc (8)
`AIL_set_file_callbacks`, `AIL_register_EOS_callback`, `AIL_register_3D_EOS_callback`,
`AIL_register_stream_callback`, `AIL_WAV_info`, `AIL_decompress_ADPCM`, `AIL_mem_free_lock`,
`AIL_sample_user_data` / `AIL_set_sample_user_data` / `AIL_3D_user_data` /
`AIL_set_3D_user_data` (aliased as `AIL_3D_object_user_data` / `AIL_set_3D_object_user_data`),
`AIL_lock` / `AIL_unlock`, `AIL_stop_timer` / `AIL_release_timer_handle`,
`AIL_get_timer_highest_delay`, `AIL_MSS_version`, `AIL_quick_load_and_play`,
`AIL_quick_set_volume`, `AIL_quick_unload`

Two important semantics:

- **EOS callbacks are called from Miles' mixer thread**, with `AIL_lock`/`AIL_unlock` guarding
  engine state. The OpenAL implementation therefore needs its own service thread that polls
  source state and dispatches end-of-sample callbacks, plus a real mutex behind
  `AIL_lock`/`AIL_unlock`.
- **`AIL_decompress_ADPCM` allocates, and the caller frees with `AIL_mem_free_lock`** — see
  `MilesAudioManager.cpp`'s comment at the release site. The allocator pair must stay matched.

`AIL_MSS_version` is already a no-op macro in the stub header.

## Audio formats that must be decoded

`AIL_WAV_info` / `AIL_decompress_ADPCM` and `AIL_set_*_sample_file` are handed raw file images, so
the replacement owns WAV parsing:

- `WAVE_FORMAT_PCM` (8- and 16-bit, mono and stereo) — the bulk of the game's sound effects.
- `WAVE_FORMAT_IMA_ADPCM` (0x11) — decoded to 16-bit PCM by `AIL_decompress_ADPCM`.
- Streamed music is MP2/MP3 in retail Zero Hour. Miles decoded this internally. Core OpenAL
  cannot; this is called out as a gap below.

## Chosen replacement strategy

Reimplement the `AIL_*` API on top of OpenAL rather than rewriting `WWAudio` and
`MilesAudioManager` against `alSource*` directly.

Rationale:

- The call sites are spread over 10 files with intertwined Miles-specific semantics (voice
  pools keyed by user data, `H3DPOBJECT` doubling as listener and sample, EOS callbacks on the
  mixer thread, stream I/O funnelled through Miles' file callbacks). Rewriting them in place means
  a several-thousand-line diff to gameplay-adjacent code with no way to test it — and would break
  the Windows build, which must keep working.
- The seam already exists in the build system: `cmake/miles.cmake` is only included for
  32-bit Windows, and every consumer links an abstract `milesstub` target. Supplying a different
  implementation of that target on other platforms is a drop-in substitution.
- The result is interface-complete by construction: it either provides every entry point the engine
  calls or it does not link.

So the port adds `Core/Libraries/Source/OpenALAudioDevice`, which provides an API-compatible
`mss/mss.h` and an OpenAL-backed implementation, and is wired in as `milesstub` on every
configuration where the real Miles stub is not fetched. Windows 32-bit builds are byte-for-byte
unaffected; nothing in `WWAudio` or `MilesAudioManager` changes behaviour there.

## What is NOT implemented / NOT tested

Stated plainly, because none of this can be verified without retail game data:

- **No audio output has been heard.** What was verified: every translation unit compiles clean at
  64-bit with `clang++ -std=c++20 -Wall`, and `nm` over the resulting archive shows 101 defined
  `AIL_*` symbols against 101 declared in the header — no missing entry points and no extras. It
  has never produced a sample of sound, because that requires the retail `.big` archives.
- **MP2/MP3 music streaming is not decoded.** `AIL_open_stream` parses and streams WAV. For
  compressed music a decoder must be plugged in (the repo already has an optional FFmpeg
  dependency for video — that is the obvious place to route it). Until then, streams of
  unsupported formats open successfully, report zero length, and play silence rather than
  crashing.
- **Filters are accepted but not applied.** `AIL_set_filter_sample_preference` and
  `AIL_set_sample_processor` record their arguments and return success. Reverb and mono-delay are
  not audible. OpenAL Soft EFX is the intended route.
- **Occlusion and 3D effects level are approximations**, applied as gain attenuation rather than
  filtering.
- **`AIL_get_DirectSound_info` returns nulls.** The only caller is the Bink video handoff, which
  is Windows-only anyway.
- **Speaker-type selection is recorded, not honoured.** OpenAL Soft picks its own output
  configuration; `AIL_set_3D_speaker_type` stores the request so that queries stay consistent.
- **`AIL_set_stream_loop_block` is a no-op.** Sub-region looping is unused by Zero Hour's own
  data as far as the call site indicates, but this is an assumption, not a measurement.
- **Playback-rate changes are implemented as pitch scaling** (`AL_PITCH`), which also changes
  duration. Miles resampled. For the engine's use (pitch-shift variation on effects) this is the
  intended effect.
