# The audio path, executed off Windows

`docs/porting/audio-device-seam.md` and `docs/porting/audio-surface.md` describe an OpenAL-backed
`AIL_*` layer that compiles and links. Neither describes it *running*. This document does: it records
what the layer answered when the same `AIL_*` sequences `MilesAudioManager.cpp` issues were driven at
it, on a 64-bit host, with the mix captured to a file and measured rather than trusted.

Three defects and two unimplemented paths came out of it. Together they mean that on the current tree
**a native Zero Hour would be almost entirely silent**: no music, no in-game speech, and — depending on
how retail encodes its sound effects, which is the one thing this probe could not read — possibly no
sound effects either. None of them were fixed here. Every one is a genuine failure of a path the game
uses on every mission, and each is classified below.

## 0. How this was measured, and what was synthetic

Two things are needed to make a claim about audio: the *engine's* requests, and the *layer's*
behaviour. They came from different places, and the difference matters for how far each finding
travels.

| | Source | Real? |
|---|---|---|
| What the game asks for | retail `INIZH.big` from `s3://cc-mac-game-data/zerohour104_gamedata_trimmed.7z` — 135 INI files, extracted read-only outside the repo | **retail** |
| What the layer does with it | `Core/Libraries/Source/OpenALAudioDevice/tests/openal_audio_probe.cpp`, driven by `scripts/native-audio-probe.py` | code under test is real; **the audio fed to it is synthesised**, §0.1 |
| Where the layer's limits are | the layer's own source | real |

Host: Linux x86-64, clang 14, OpenAL Soft 1.19.1 (`AL_VERSION` = `1.1 ALSOFT 1.19.1`) with the `wave`
backend, so every "does it play" answer is a measured RMS over captured samples, not a return code.
`AIL_startup` → `AIL_quick_startup(1, 0, 44100, 16, 2)` → `AIL_quick_handles` is the engine's own
sequence, from `MilesAudioManager::openDevice()`.

```
$ CLANGXX=clang++-14 python3 scripts/native-audio-probe.py --json audio.json
device            : OpenAL Soft
AIL_MSS_version   : OpenAL 1.1 ALSOFT 1.19.1
2D voices granted : 256
3D voices granted : 128
3D providers      : OpenAL
...
  17 ok, 0 failed
```

### 0.1 No retail audio was decoded, and why

The hosted archives are the *trimmed* replay-CI data. Their own packer says "No textures, audio or GUI
data", and the extracted tree confirms it: `INIZH.big`, `MapsZH.big`, `W3DZH.big`, `mss32.dll`,
`BINKW32.DLL`, `Data/Scripts`. There is no `AudioZH.big`, no `SpeechZH.big`, no `Data/Audio`. **Not one
retail sound was decoded or played for this report**, and no conclusion below rests on having done so.

Where audio bytes were needed, they were synthesised with ffmpeg and are labelled *synthetic*
throughout. Synthetic inputs are sufficient for the defects in §2 and §3 because each of those is a
property of the layer's own parsing and handoff, reproducible with any file of the format the retail
INI names — but they cannot establish that a *retail* file decodes correctly, and are not used to claim
it.

One data caveat, measured: the downloaded Zero Hour archive is 53,307,598 bytes as documented, but its
SHA-256 is `2d137f6cd51609b517345fc7ab780dfbf4547eca0dd689e7212d8506a36b1b13`, which does **not** match
the `EXPECTED_HASH_GENERALSMD` default in `.github/workflows/check-replays.yml`
(`6837FE1E…AC05E21`). Either the repository variable overrides it in CI or that default is stale; the
INI contents parse correctly, so this is flagged, not treated as corruption.

## 1. Initialisation: this part works

All of it, on the first try, with no shims and no fallback path taken.

| Question | Answer | Evidence |
|---|---|---|
| Does a device open off Windows? | yes | `alcOpenDevice(NULL)` → `OpenAL Soft`, context current |
| Does the engine's `AIL_quick_startup` path work? | yes | `AIL_quick_handles` returns a digital driver |
| Does the `AIL_waveOutOpen` path work? | yes | returns a driver; reports `emulated_ds = 0` |
| Are there voices? | yes | 256 of 512 requested 2D handles; retail asks for 4 (`SampleCount2D`) |
| Is there a 3D provider? | yes | one, named `OpenAL`; 128 3D handles; retail asks for 25 |
| Does a listener open? | yes | `AIL_open_3D_listener` non-null |

The retail `AudioSettings.ini` asks for `Preferred3DHW1 = "Creative Labs EAX (TM)"`,
`Preferred3DHW2 = "Aureal A3D Interactive (TM)"`, `Preferred3DSW = "Miles Fast 2D Positional Audio"` —
none of which this backend advertises. `MilesAudioManager::selectProvider` already falls back to
provider 0 when the hardcoded Miles names are absent, so the `OpenAL` provider is selected and
`PROVIDER_ERROR` is not hit. That fallback is load-bearing for the port and is worth keeping in mind
if anyone "cleans it up".

Initialisation is therefore **not** the wall. The wall is one layer up, and the game reaches it on
every asset it loads.

## 2. What Zero Hour actually asks for — read out of retail INI

| Category | Definitions | Path the engine builds | Format |
|---|---|---|---|
| Music | 68 `MusicTrack` in `Music.ini` | `Data\Tracks\<name>` | **67 of 67 filenames are `.mp3`** |
| Dialogue / EVA | 2568 `DialogEvent` in `Speech.ini` | `Data\Speech\<lang>\<name>.wav` | `.wav`, played as a **stream** (`AT_Streaming`) |
| Sound effects | 743 `AudioEvent` in `SoundEffects.ini`, 666 in `Voice.ini`, 4071 distinct sample names | `Data\Sounds\<name>.wav` (`SoundsExtension = wav`) | `.wav`, played from a **whole-file image** |

(`AudioRoot = Data\Audio`, folders `Sounds` / `Tracks` / `Speech`, `OutputRate = 44100`,
`OutputBits = 16`, `OutputChannels = 2`, `SampleCount2D = 4`, `SampleCount3D = 25`, `StreamCount = 3`.)

So the port needs exactly three things to make noise: an MP3 decoder for streams, a working WAV
*stream* path, and a working WAV *image* path. It has one and a half of the three.

### 2.1 The sample (whole-file image) path decodes what it claims to — *synthetic*

`AIL_set_sample_file` was given synthesised WAVs and the mix captured; RMS is over the captured
stereo file.

| Synthetic asset | `AIL_set_sample_file` | Captured RMS |
|---|---|---|
| PCM 16-bit mono 22050 | 1 | `[0.0509, 0.0509]` |
| PCM 8-bit stereo 11025 | 1 | `[0.0606, 0.0606]` |
| PCM 16-bit stereo 44100 | 1 | `[0.0605, 0.0605]` |
| IMA ADPCM mono 22050 | 1 | `[0.0488, 0.0488]` |
| IMA ADPCM stereo 22050 | 1 | `[0.0603, 0.0603]` |
| MS ADPCM mono 22050 | **0**, `"unsupported sample format (expected PCM or IMA ADPCM WAV)"` | silence |

The IMA ADPCM decoder also tracks ffmpeg's decode of the same bytes: 12246 samples compared,
max absolute difference 26, RMS difference 8.1 on a 16-bit scale, decoded lengths identical (22374
samples, max 30, RMS 8.4 for stereo). That is a rounding-order difference, not a broken decoder.

MS ADPCM is refused loudly, which is the correct behaviour for an unimplemented format. Whether retail
uses it is unknown — see §5.

## 3. The three defects

### 3.1 The engine's own ADPCM handoff produces silence — **port defect**

This one is invisible to any test that drives the layer directly, which is why it survived: the layer's
`AIL_set_sample_file` and `AIL_decompress_ADPCM` both work, and the defect is in how the engine
composes them.

`AudioFileCache::openFile` decompresses an ADPCM asset and caches *the decompressed buffer*
(`MilesAudioManager.cpp:3074`); `MilesAudioManager::playSample` then hands **that buffer** to
`AIL_set_sample_file` (`:2698`) and never looks at the return value. Miles' `AIL_decompress_ADPCM`
produced a PCM **WAV image**, so this worked. This layer's returns bare PCM:

```cpp
int AIL_decompress_ADPCM(const AILSOUNDINFO* info, void** outdata, unsigned long* outsize)
{
	return OpenALAudio::decodeImaAdpcm(*info, outdata, outsize) ? 1 : 0;   // no RIFF header
}
```

Driving the engine's exact sequence (`engine-adpcm` stage) on a synthetic IMA ADPCM file:

```
decompressed_bytes=24492 decompressed_is_riff_wave=False set_sample_file_result=0
last_error='unsupported sample format (expected PCM or IMA ADPCM WAV)' rms=[1.5e-05, 1.5e-05]
```

The sound is dropped, the error is set and nobody reads it. Consequence: **every ADPCM-compressed
sound effect and unit voice line is silent**, and nothing anywhere reports a problem. How much of
retail is ADPCM is the open question of §5; the code path itself is unambiguous.

### 3.2 Any stream longer than ~1 KB becomes a silent zero-length stream — **port defect**

`OpenALStream.cpp` reads the first 1024 bytes of a stream and parses that buffer as a WAV header. But
the parser requires the whole `data` payload to be inside the buffer it was given:

```cpp
} else if (isData) {
	if (!haveFormat || !r.has(chunkSize)) {   // OpenALWaveFile.cpp:144
		return false;
	}
```

so with a 1024-byte window, any WAV with more than ~980 bytes of audio — about 5 ms at 44.1 kHz stereo
— fails to parse, and the failure is swallowed into a valid-looking handle:

```cpp
} else {
	stream.decodable = false;   // OpenALStream.cpp
	stream.totalFrames = 0;     // opens, reports zero length, plays silence
```

Measured, synthetic, one control and one subject:

| Synthetic stream | `AIL_open_stream` | length | position | captured RMS |
|---|---|---|---|---|
| PCM 16-bit mono, 20 ms (fits the window) | handle | 20 ms | advances to 20 ms | `[0.0242, 0.0242]` |
| PCM 16-bit stereo, 500 ms | handle | **0 ms** | **never advances** | `[1.5e-05, 1.5e-05]` |

Every one of the 2568 retail `DialogEvent`s is a `.wav` played through `AIL_open_stream`, and every one
is longer than 5 ms. So **all in-game speech and EVA dialogue is silent**, from a header-window bug
rather than a missing decoder. `MilesAudioManager::getFileLengthMS` also opens a stream purely to read
a duration, so it returns 0 for every real file — anything timed off audio length (subtitles, briefing
pacing) is affected too.

Mission-briefing speech goes through `AIL_quick_load_and_play` instead, which reads the whole file, so
that path is not hit by this.

### 3.3 3D panning is mirrored — **port defect**

`MilesAudioManager::setDeviceListenerPosition` passes Miles' left-handed listener vectors straight
through (`AIL_set_3D_orientation(m_listener, …, 0, 0, -1)` at `:2537`), and the OpenAL layer passes them
straight to `alListenerfv(AL_ORIENTATION, …)`, which is right-handed. Nothing converts. A mono voice at
`x = -20` with the engine's own listener setup comes out on the **right**:

```
x=-20 rms=[1.5e-05, 0.083792]
x=+20 rms=[0.083791, 1.5e-05]
```

Positional audio is otherwise alive — hard panning, `AL_LINEAR_DISTANCE_CLAMPED`, distances per sample
— it is just handed as a mirror image of what Windows produces. This is small and specific: the seam
needs a documented handedness conversion, not new machinery.

## 4. The unimplemented paths, and the approximations

| Path | State | Measured |
|---|---|---|
| MP3 / MP2 streams | **unimplemented**, and *silently*: the handle opens, length is 0, no error is set, the mix is silence | `handle=True length_ms=0 last_error=None rms=[1.5e-05, …]` |
| MS ADPCM images | unimplemented, refused loudly | §2.1 |
| `AIL_set_sample_processor` / `AIL_set_filter_sample_preference` (`Delay` in INI) | recorded, never applied; `AIL_enumerate_filters` advertises one filter so `initDelayFilter` succeeds | source |
| `AIL_set_3D_sample_effects_level` (reverb) | recorded only, no EFX slot | source |
| Occlusion | **approximation**: gain × `1 - occlusion*0.75`, where Miles low-passed | `ratio=0.25` at occlusion 1.0, matching the formula |
| 2D pan of a stereo voice | inert — OpenAL will not position a multi-channel source | `pan=0 rms=[0.0605, 0.0605]`, identical channels |
| `AIL_get_DirectSound_info` (Bink) | returns nulls by design | `object=False buffer=False` |

The MP3 case is the one to weigh: retail has 67 `.mp3` tracks and *all* music therefore plays as
silence with no diagnostic anywhere. `initFilters3D` also routes `LowPassCutoff` (used by 472
`shrouded` events) into `AIL_set_3D_sample_occlusion`, so off-screen sounds get *quieter* on this
backend where Windows makes them *muffled* — audible, wrong, and cheap to state precisely.

## 5. What could not be determined

- **Whether any retail audio file decodes.** No retail audio bytes exist on this box (§0.1). Everything
  in §2.1 is synthetic.
- **What fraction of retail `Data\Audio\Sounds\*.wav` is IMA ADPCM versus PCM.** This decides whether
  §3.1 silences most sound effects or none of them. It needs the retail archives, and it is the single
  highest-value unknown here.
- **Whether retail uses MS ADPCM anywhere.** Same reason.
- **macOS behaviour.** All of this ran on Linux with OpenAL Soft's `wave` writer. Device open, the
  `SampleCount3D` grant, and OpenAL Soft's Core Audio backend on Apple Silicon are unverified; §3.1,
  §3.2 and §3.3 are platform-independent logic and will reproduce anywhere.
- **The engine end-to-end.** Nothing here ran `GameEngine::init()`; the probe drives the `AIL_*` layer
  directly, so INI parsing, `AudioEventRTS` filename generation and `.big` reads through
  `TheFileSystem` are read from source, not executed. The stream path *does* read through the engine's
  file callbacks, so `.big`-backed streams will hit §3.2 identically.
- **Whether the 3D distance model matches Miles numerically.** Only the sign of the pan and the
  occlusion ratio were measured, not the attenuation curve.

## 6. `ListenerHandleClass` and the live 3D path

`ListenerHandleClass` needs nothing, because in Zero Hour it is dead code. It lives in WWAudio
(`Core/Libraries/Source/WWVegas/WWAudio/listenerhandle.{h,cpp}`), which the build compiles but which no
Zero Hour consumer instantiates — nothing outside WWAudio references `WWAudioClass` or `AudibleSound`
at all. Its `Initialize` declaration was removed in #85 and should stay removed; the class is not on the
path to audible sound.

The live listener is `MilesAudioManager::createListener` → `AIL_open_3D_listener`, updated every frame
by `setDeviceListenerPosition`. What *that* path needs is in §3.3 (a handedness conversion) and §4
(reverb, and a low-pass instead of gain for occlusion). It opens, it positions, it pans; it is the
closest thing to finished in this subsystem.

## 7. What the next slice should be scoped to

In dependency order, cheapest first, and deliberately not attempted here:

1. **Fix the ADPCM handoff (§3.1).** Either `AIL_decompress_ADPCM` returns a RIFF/WAVE image as Miles
   did, or `AIL_set_sample_file` learns to accept the cache's bare PCM. The first is closer to the
   oracle. Small, and it is the difference between sound effects and no sound effects.
2. **Fix the stream header window (§3.2).** The parser must be able to accept a `data` chunk whose
   payload extends past the buffer; the 1024-byte read is only the *header* read. Also: an
   undecodable stream must not present as a valid zero-length handle — that behaviour is what let both
   this and MP3 hide.
3. **Convert handedness at the 3D seam (§3.3),** with the conversion written down.
4. **Decide MP3/MP2 (§4).** The tree already builds FFmpeg for the video path
   (`RTS_BUILD_OPTION_FFMPEG`), so the decoder is likely already present; this is a wiring and
   licensing decision, not a research one. Until it lands, `AIL_open_stream` should fail loudly on a
   format it cannot decode.
5. **Then, and only then, repeat this probe against retail archives** — with `Data\Audio` present, on
   an Apple Silicon Mac, and against `AudioZH.big`/`SpeechZH.big`, checking a real ADPCM sound effect,
   a real speech stream and a real music track. `scripts/native-audio-probe.py` takes synthetic assets
   today; pointing it at a directory of extracted retail files is a small change to `make_assets`.

The harness is checked in with this report: `scripts/native-audio-probe.py` and
`Core/Libraries/Source/OpenALAudioDevice/tests/openal_audio_probe.cpp`. Its 17 gates are properties
that hold today and should keep holding; its observations are the list above, printed with their
evidence, and deliberately do not fail the run.
