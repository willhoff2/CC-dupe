# Retail audio: what it is, what was broken, and what now plays

`docs/porting/audio-path-probe.md` found three defects and two unimplemented paths in the OpenAL
`AIL_*` layer, and said plainly that **every input it used was synthetic** — the archives it could
reach held no audio at all. So it could measure the layer's behaviour but only *infer* the blast
radius ("hits all 2568 retail `DialogEvent`s"), and could not claim that any retail file decodes.

The full game-data object (`s3://cc-mac-game-data/zerohour104_gamedata_full.7z`, sha256
`d9ddd811…0bac1ae4`, 2.23 GB unpacked) removes that limit. This document is the retail half: a
survey of every audio asset the game ships, the fixes those measurements justify, and the decoded
samples of real retail assets that show each fix working. Two new scripts do the work and neither is
a CI gate, because neither can run without non-redistributable data:

| Script | What it does |
|---|---|
| `scripts/audio-retail-survey.py` | reads the retail `.big`s and INI, classifies every audio entry, resolves the engine's own event definitions onto them, and counts what each defect broke |
| `scripts/audio-retail-probe.py` | extracts one real asset per codec/layout class outside the repo and runs each through the engine's own `AIL_*` sequence, capturing and measuring the mix |
| `scripts/audio_retail_assets.py` | the shared BIG/WAV/MPEG parser and an independent IMA ADPCM decoder, deliberately written from the format specs rather than from the C++ under test |

No retail bytes are committed, and nothing below quotes a number that was not produced by one of
those two scripts on this branch.

## 1. What retail Zero Hour actually ships

```
$ python3 scripts/audio-retail-survey.py --data ~/gamedata/full/GeneralsMD
archives read      : 20
audio entries      : 3530
unparsed entries   : 0
```

3530 audio entries, all parsed, in five archives: `SpeechEnglishZH.big` 2430, `AudioEnglishZH.big`
794, `AudioZH.big` 287, `SpeechZH.big` 12, `MusicZH.big` 7.

| Codec, layout | Count |
|---|---|
| IMA ADPCM, mono, 44100 Hz, 4-bit, block 1024 | 2521 |
| PCM, mono, 22050 Hz, 16-bit | 904 |
| PCM, stereo, 22050 Hz, 16-bit | 35 |
| IMA ADPCM, stereo, 44100 Hz, 4-bit, block 2048 | 28 |
| IMA ADPCM, mono, 22050 Hz, 4-bit, block 512 | 23 |
| PCM, mono, 44100 Hz, 16-bit | 10 |
| **MP3, stereo, 44100 Hz** | **7** |
| PCM, stereo, 44100 Hz, 16-bit | 2 |

By the code path the engine uses for each (`AudioRoot = Data\Audio`, `SoundsFolder = Sounds`,
`StreamingFolder = Speech`, `MusicFolder = Tracks`):

| Engine path | Assets |
|---|---|
| one-shot / sample (`AIL_set_sample_file`) | 900 PCM + **181 IMA ADPCM** = 1081 |
| stream (`AIL_open_stream`) | **2391 IMA ADPCM** + 51 PCM = 2442 |
| music (stream) | **7 MP3** |

And what the INI asks for, resolved onto those entries: 2569 `DialogEvent` definitions → 2429
distinct files (2392 ADPCM + 43 PCM references); 1410 `AudioEvent` definitions in the parsed INI set
→ 899 distinct files (820 PCM + 206 ADPCM references); 69 `MusicTrack` definitions → the 7 MP3s.

Two notes on those counts, since the probe report quotes different ones:

- The probe counted **per INI file** ("743 `AudioEvent` in `SoundEffects.ini`, 666 in `Voice.ini`");
  this counts **every `AudioEvent` in every INI the archives contain**, which is 1410. Both are
  right about different sets. `DialogEvent` is 2569 here against the probe's 2568 — the probe read
  `Speech.ini` from the trimmed object, this reads all INI in the full one.
- Unresolved references (133 dialogue, 4272 sound-effect, 60 music) are mostly base-game assets:
  Zero Hour INI inherits Generals events, and the survey was pointed at `GeneralsMD` only. They are
  reported rather than hidden, and none of them changes a codec count.

### Formats the retail set does **not** contain

- **No MS ADPCM.** 3523 WAV files, 0 with `WAVE_FORMAT_ADPCM` (0x02). The probe's §2.1 "MS ADPCM is
  refused loudly, whether retail uses it is unknown" is now answered: retail does not use it, so the
  refusal is correct and MS ADPCM support is **cut, with evidence**.
- **No MP2.** All 7 music tracks are MPEG-1 **layer III**. MP2 is likewise cut.
- **No 8-bit PCM**, and no sample rate other than 22050 and 44100.

## 2. What each defect actually broke — measured, not inferred

```
What each probe defect actually broke, measured over the archives
  ADPCM handoff  : 181 of 1081 one-shot assets were IMA ADPCM
  stream window  : 2442 of 2442 streams have payload past byte 1024; 0 would have fitted
  stream metadata: 0 streams keep metadata past byte 1024 (largest header 60 bytes)
  mirrored pan   : 1081 assets can play on a 3D voice (codec-independent, at the backend seam)
```

The stream line **corrects the probe's diagnosis while confirming its conclusion**. The probe
described a "1024-byte header window" too small for the header. It was never too small for a
*header*: the largest retail stream header is **60 bytes**. What broke was that the parser required
the whole `data` **payload** to be resident in the same window, so all 2442 retail streams — 100%,
covering the 2429 files the 2569 `DialogEvent`s resolve to — parsed as failures and were then
swallowed into a valid-looking zero-length silent handle. Enlarging the constant would have fixed
nothing; the fix is that metadata parsing no longer needs the payload at all (§3.2).

## 3. The fixes, and why they are shaped this way

### 3.1 `AIL_decompress_ADPCM` returns a WAV image again — port defect, fixed

`AudioFileCache::openFile()` caches whatever `AIL_decompress_ADPCM` returns and
`MilesAudioManager::playSample` later hands that same pointer to `AIL_set_sample_file`, which parses
a RIFF container. Miles returned a PCM **WAV image**; this layer returned bare PCM, so every ADPCM
one-shot was rejected. `AIL_decompress_ADPCM` now decodes and then wraps the PCM in a 44-byte
RIFF/WAVE image (`buildWaveImage`), which is what the engine's next call expects; `AIL_mem_free_lock`
still frees it, so the allocator pair stays matched.

Two silent-failure sites in the engine were closed at the same time, in
`Core/GameEngineDevice/Source/MilesAudioDevice/MilesAudioManager.cpp`: `AIL_WAV_info`'s result was
ignored with `soundInfo` uninitialised, and `AIL_decompress_ADPCM`'s result was ignored so a failed
decode cached a null buffer. Both now `DEBUG_CRASH` and refuse to cache. This is a real behaviour
change on Windows too: Miles never failed these calls for a file the engine could open, so the
oracle's behaviour is unchanged for valid data, and for invalid data an assertion is strictly better
than a silent null.

### 3.2 Stream metadata and stream payload are separate questions — port defect, fixed

`parseWaveInfo(..., requirePayload)` splits the two: `parseWaveHeader` (payload resident, used by
`AIL_WAV_info` and the sample paths) keeps requiring it, and `parseWaveMetadata` reports
`data_len` plus the payload's **offset** with `data_ptr` left null. `readWaveMetadata` then starts
with a 1024-byte read and **doubles the window until the `data` chunk header is found**, bounded at
64 KB purely so a corrupt file cannot pull the whole file into memory. The payload is read later,
in chunks, through the engine's file callbacks — which is what streaming means.

The stream path also learned IMA ADPCM, because 2391 of 2442 retail streams are ADPCM and a PCM-only
stream path plays essentially no retail dialogue. Reads are rounded to **whole blocks** in both
directions (`alignToCodecBoundary`) since each block carries the predictor state its samples decode
from; seeking is therefore exact to a block, and `AIL_set_stream_loop_block`, previously a no-op, now
stores block-aligned loop bounds.

### 3.3 The handedness conversion, stated — port defect, fixed

The sign flip lives in exactly one place, `milesToAlZ()` in `OpenALAudioInternal.h`, applied at the
backend seam (`OpenAL3DSample.cpp`) to every position, orientation vector and velocity. Nothing above
the seam changes, so the Windows Miles path is untouched at runtime.

The reasoning, written down because a sign flip that makes one test pass is not a fix: Miles' 3D
frame is left-handed (+X right, +Y up, +Z away from the listener), OpenAL's is right-handed with the
same X and Y and +Z *towards* the listener. They differ by the sign of Z alone. The consequence is
which side is "right", because each derives it by a cross product of opposite chirality: OpenAL
computes `at × up`, Miles `up × forward`. Zero Hour's listener is
`AIL_set_3D_orientation(listener, facing.x, facing.y, facing.z, 0, 0, -1)` with world coordinates
passed straight through, so with facing = world north, un-negated triples gave OpenAL
`(0,1,0) × (0,0,-1) = (-1,0,0)` where Miles computes `(0,0,-1) × (0,1,0) = (+1,0,0)` — a source to
the world east came out of the left speaker, exactly inverted. Negating Z makes east right in both.

### 3.4 Undecodable streams fail instead of playing silence — unimplemented, now loud

`AIL_open_stream` used to keep a handle for anything it could not parse and report zero length, which
is indistinguishable from a working stream at every call site in `MilesAudioManager`. It now returns
null and sets a reportable error, naming MPEG specifically when the bytes look like an MPEG
elementary stream, and closes the callback file it opened. `getFileLengthMS`, which opens a stream
purely to measure it, gets 0 from a failure it can now see rather than from a success that lies.

## 4. Real retail assets, decoded and measured

```
$ CLANGXX=clang++-14 python3 scripts/audio-retail-probe.py --data ~/gamedata/full/GeneralsMD
assets selected : 12 (one per codec/layout class)
  ...
  25 ok, 0 failed
```

One real asset per class, extracted from the archives to a scratch directory outside the repo, run
through the path the engine would use, with OpenAL Soft's wave writer capturing the mix. The
assertion is the samples: a decode that returns success and plays silence fails.

### 4.1 One-shots (`AIL_set_sample_file`)

| Retail asset | Codec | Length / header | Rate, channels | Captured RMS |
|---|---|---|---|---|
| `Sounds\English\baiasela.wav` | IMA ADPCM block 512 | 2582 / 2583 ms | 22050, 1 | `[0.112, 0.112]` |
| `Sounds\English\isabotab.wav` | IMA ADPCM block 1024 | 1527 / 1527 ms | 44100, 1 | `[0.059, 0.059]` |
| `Sounds\cbusroll.wav` | IMA ADPCM block 2048 | 5461 / 5461 ms | 44100, 2 | `[0.198, 0.136]` |
| `Sounds\English\aangr01a.wav` | PCM 16-bit | 2693 / 2694 ms | 22050, 1 | `[0.097, 0.097]` |
| `Sounds\ubutton2.wav` | PCM 16-bit | 511 / 511 ms | 44100, 1 | `[0.071, 0.071]` |
| `Sounds\English\ucheergl.wav` | PCM 16-bit | 3767 / 3767 ms | 22050, 2 | `[0.153, 0.164]` |
| `Sounds\uboarder.wav` | PCM 16-bit | 859 / 860 ms | 44100, 2 | `[0.168, 0.270]` |

These seven all pass on the pre-fix backend too, and that is worth saying: `AIL_set_sample_file`
handed an ADPCM WAV directly always worked. What was broken is the *engine's* sequence, §4.2 — which
is the only one the game actually uses for these files.

### 4.2 The engine's own ADPCM handoff (§3.1), on retail bytes

`AIL_decompress_ADPCM` → cache → `AIL_set_sample_file`, the exact sequence
`AudioFileCache::openFile` and `playSample` perform:

| Retail asset | Decompressed | RIFF/WAVE? | `AIL_set_sample_file` | Captured RMS |
|---|---|---|---|---|
| `baiasela.wav` | 113,948 B | yes | 1 | `[0.141, 0.141]` |
| `isabotab.wav` | 134,750 B | yes | 1 | `[0.060, 0.060]` |
| `cbusroll.wav` | 963,396 B | yes | 1 | `[0.235, 0.167]` |

The same probe run against the pre-fix backend (a worktree at the branch point, `8c6dfaab0`, with only
these scripts copied in) reports for all three files `riff=False set_sample_file=0
rms=[1.5e-05, 1.5e-05]` — the decode was correct, the container was missing, and the engine's next
call rejected it in silence. That whole-probe before/after is `13 ok, 12 failed` → `25 ok, 0 failed`,
and it is the source of every "before" figure below.

### 4.3 The decoded samples are checked against two other decoders

The C++ decode of each retail ADPCM file is compared **sample for sample** with
`audio_retail_assets.decode_ima_adpcm`, an independent Python implementation of the IMA reference
algorithm, and separately with ffmpeg's:

| Retail asset | Samples | vs. reference decoder | vs. ffmpeg (max, RMS) |
|---|---|---|---|
| `baiasela.wav` | 56,952 | **0 mismatched** | 38, 10.2 |
| `isabotab.wav` | 67,353 | **0 mismatched** | 81, 18.5 |
| `cbusroll.wav` | 481,676 | **0 mismatched** | 67, 13.1 |

Decoded lengths equal what each file's own header implies, to the sample. The ffmpeg deltas are a
rounding convention, not drift: ffmpeg's step update is `((2*nibble+1)*step)>>3` where the reference
is shift-and-add, and the Python reference decoder reproduces the same deltas against ffmpeg that the
C++ does — 81 on `isabotab.wav`, identically. That is why exact agreement is asserted against the
reference decoder and only a bound (max ≤ 256, RMS ≤ 64 on a 32768 scale, i.e. ≥ 54 dB below full
scale) against ffmpeg.

One measured difference from the file's own `fact` chunk: whole-block decoding yields up to one block
more than `fact` declares (67,353 frames against 65,639 for `isabotab.wav`; 56,952 against 56,063 and
240,838 against 240,128 for the other two). Both this decoder and ffmpeg do that, and the surplus is
padding: its RMS is 6.96, 0.00 and 0.12 on a 32768 scale — at worst −73 dBFS, i.e. silence. Left as
is, matching ffmpeg; the alternative is trimming to `fact`, which is a change Miles' own output would
have to arbitrate.

### 4.4 Streams (`AIL_open_stream`) — the 1024-byte defect, on retail files

| Retail asset | Codec | Length / header | Rate | Captured RMS |
|---|---|---|---|---|
| `Speech\English\dxxoc001.wav` (379 KB) | IMA ADPCM block 1024 | 17124 / 17124 ms | 44100 | `[0.095, 0.095]` |
| `Speech\cjettaka.wav` | IMA ADPCM block 2048 | 4720 / 4721 ms | 44100 | `[0.319, 0.330]` |
| `Speech\English\t_gui001.wav` | PCM 16-bit mono | 1575 / 1576 ms | 22050 | `[0.117, 0.117]` |
| `Speech\English\ucheer.wav` | PCM 16-bit stereo | 3935 / 3935 ms | 22050 | `[0.168, 0.171]` |

Every one of these is far past the old 1024-byte window, and on the pre-fix backend every one
reported `length_ms=0 high_water_ms=0 rms=[1.5e-05, 1.5e-05]` from a handle that looked valid —
including the two PCM files, which also came back claiming 44100 Hz for 22050 Hz data because the
failed parse left the format defaulted. The position high-water mark above is where the probe stopped
polling (≈3 s), not where the stream ended.

### 4.5 3D direction, on retail assets

A mono retail one-shot on a 3D voice with the engine's own listener setup, at `x = ±20`:

| Retail asset | `x = -20` RMS | `x = +20` RMS |
|---|---|---|
| `baiasela.wav` | `[0.183, 0.000015]` | `[0.000015, 0.185]` |
| `isabotab.wav` | `[0.098, 0.000015]` | `[0.000015, 0.097]` |
| `aangr01a.wav` | `[0.160, 0.000015]` | `[0.000015, 0.159]` |
| `ubutton2.wav` | `[0.116, 0.000015]` | `[0.000015, 0.116]` |

Negative x on the left, positive x on the right — the Miles convention. The pre-fix backend produced
the same magnitudes with the channels swapped (`x=-20 rms=[1.5e-05, 0.183314]`), which is the
mirroring, measured on retail assets rather than inferred.

### 4.6 Retail MP3 fails loudly

```
retail MP3 music fails to open, loudly: Data\Audio\Tracks\CHI_10.mp3
  handle=False last_error='MPEG audio streams (MP1/MP2/MP3) are not implemented: no MPEG decoder
  is linked (retail Zero Hour music is MP3; see docs/porting/audio-retail-validation.md)'
```

A real 4.9 MB retail track, not a synthetic file. The pre-fix backend answered `handle=True
last_error=None` for the same file: a valid handle, zero length, silence, and nothing anywhere to
notice.

## 5. Where the audio path still stops

Classified, as the project requires:

| # | Item | Class | Consequence |
|---|---|---|---|
| 1 | **No MPEG decoder** | **unimplemented, and REQUIRED** — retail music is 7 MP3 tracks, so it cannot be cut | **all music is absent.** It now fails audibly-in-logs rather than silently. `RTS_BUILD_OPTION_FFMPEG` already exists for video, so this is a wiring and licensing decision, not research |
| 2 | Reverb (`AIL_set_3D_sample_effects_level`) recorded, not applied | unimplemented | no reverb; needs OpenAL Soft EFX |
| 3 | Filters (`AIL_set_sample_processor`, `Delay`) recorded, not applied | unimplemented | no mono-delay effect |
| 4 | Occlusion is gain, not low-pass; `LowPassCutoff` routed into occlusion | approximation | shrouded sounds get quieter where Windows makes them muffled |
| 5 | 2D pan of a stereo voice is inert | OpenAL limitation | multi-channel sources cannot be positioned |
| 6 | Playback rate is `AL_PITCH` | approximation | changes duration as well as pitch, unlike Miles' resampler |
| 7 | MS ADPCM, MP2 | **cut, with evidence** (§1) | refused loudly; absent from the retail set |

### What is still untestable here, and what it would take

- **The Mac's real audio device.** Everything above ran on Linux x86-64 with OpenAL Soft's `wave`
  writer, which is what makes the samples measurable. Device open, the `SampleCount3D` grant and the
  Core Audio backend on Apple Silicon are **real-device-only validation**; the decode and handedness
  logic is platform-independent and will reproduce anywhere.
- **The engine end to end.** The probe drives the `AIL_*` layer with the engine's own sequences, but
  `GameEngine::init()` still stops in the renderer (`docs/porting/renderer-integration-arm64.md`), so
  no frame has ever played a sound. `AudioEventRTS` filename generation and `.big` reads through
  `TheFileSystem` are read from source, not executed; the streams above were fed real retail bytes
  through the same callback shape the engine installs.
- **Numerical agreement with Miles' 3D attenuation.** Only the direction of the pan was asserted, not
  the distance curve. Comparing curves needs the Windows build as an oracle with a capture path,
  which does not exist yet.
- **Subtitle and briefing pacing.** `getFileLengthMS` now returns real durations (§4.4), so timings
  that depended on it should work; nothing exercises them without a running game.

## 6. Reproducing this

```sh
export AWS_ACCESS_KEY_ID=$R2_ACCESS_KEY_ID AWS_SECRET_ACCESS_KEY=$R2_SECRET_ACCESS_KEY
export AWS_DEFAULT_REGION=us-east-1
aws s3 cp s3://cc-mac-game-data/zerohour104_gamedata_full.7z /tmp/
7z x -o$HOME/gamedata/full /tmp/zerohour104_gamedata_full.7z

python3 scripts/audio-retail-survey.py --data ~/gamedata/full/GeneralsMD --json /tmp/survey.json
CLANGXX=clang++-14 python3 scripts/audio-retail-probe.py \
    --data ~/gamedata/full/GeneralsMD --json /tmp/probe.json
```

The synthetic probe, `scripts/native-audio-probe.py`, still runs without retail data and now gates
the fixed behaviour: the ADPCM handoff must produce RIFF/WAVE and be audible, a 500 ms stream must
report its real length, an undecodable stream must fail with a non-empty error, and `+x` must be
heard on the right. 23 gates, 0 failures.
