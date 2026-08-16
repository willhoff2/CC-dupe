# The music: decoding retail MP3 under the Miles `AIL_*` stream path

`docs/porting/audio-retail-validation.md` ends with a measured gap rather than a claim: every retail
music track is an MP3, the OpenAL backend had no MPEG decoder, and so a native build reached the main
menu **musically silent** while reporting no error anywhere. That is a missing feature, not polish,
and this document is what closed it and how it was proved.

Nothing below is quoted from an earlier document. Every number comes from
`scripts/audio-music-probe.py` run on this branch against the full retail object, and that script
prints them all.

## 1. Which decoder, and why not the one already in the tree

The obvious route was FFmpeg: `#92` already provisions and links it for video, and FFmpeg decodes
MP3. **Measured, it does not — not this build of it.** The pinned FFmpeg this project builds is
configured for exactly the video work it was added for:

```
--disable-everything --enable-decoder=bink,binkaudio_dct,binkaudio_rdft,pcm_s16le
--enable-demuxer=bink,binka,avi,wav
```

Against that library:

```
libavcodec 61.19.101
mp3 decoder            : (none)
mp2 decoder            : (none)
mp3 demuxer            : (none)
binkaudio_dct decoder  : binkaudio_dct
```

So taking the FFmpeg route means re-enabling MP3 decoding and demuxing in the pinned build, and it
also means the *music* would become conditional on `RTS_BUILD_OPTION_FFMPEG`, a **video** option that
defaults OFF, attached to `corei_gameenginedevice` rather than to the audio backend. A default build
would still have no music, which is the defect this slice exists to remove.

The decoder linked instead is **[minimp3](https://github.com/lieff/minimp3)**, pinned in
`cmake/minimp3.cmake` at commit `7b590fdcfa5a79c033e76eacc05d0c3e4c79f536` the way the other vendor
deps are pinned, and provisioned for the offline probe by `scripts/ci/fetch-probe-deps.sh`:

* single header, CC0/public domain, no runtime shared library and no new link-time surface beyond the
  audio backend itself — the backend's only other dependency stays OpenAL;
* decodes MPEG-1/2/2.5 Layer I/II/III to interleaved 16-bit PCM, which is what OpenAL wants and what
  the existing `StreamCodec` path already produces for PCM and IMA ADPCM;
* frame-at-a-time, which fits the callback-backed `readChunk` seam exactly: the engine's own file
  callbacks stay the only way bytes are read, so tracks inside `.big` archives work unchanged.

## 2. What the retail files actually are

Measured by an independent frame walk (`mpeg_facts` in `scripts/audio_retail_assets.py`, written from
the format spec, not from the C++ under test), over all 56 tracks — 7 in `MusicZH.big`, 49 in the base
game's `Music.big`:

| Property | Measured |
|---|---|
| Layer | III, all 56 |
| 55 tracks | MPEG-1, stereo, 44100 Hz |
| 1 track (`Silence60.mp3`) | MPEG-2, **mono, 22050 Hz** |
| Bitrates | 192 kbps (53), 256, 160, 64 |
| VBR | none; no Xing or VBRI header in the set |
| ID3v2 at the front | 54 of 56, 1024 bytes |
| Total playing time | **2 h 13 m 4 s** across 305,632 frames |
| `USA_09.mp3` | final frame header claims 523 bytes with 87 in the file: **truncated** |

The implementation is nonetheless frame-indexed rather than bitrate-scaled, because "all CBR" is a
property of this retail set and not of the format, and the failure mode of bitrate maths on a VBR
file is drifting loop points and mistimed events with no error anywhere — the exact defect class this
project exists to stop.

## 3. How it works

`OpenALStream.cpp` gained a third `StreamCodec`, alongside `Pcm` and `ImaAdpcm`:

* **`readMpegMetadata`** runs when `readWaveMetadata` finds no RIFF header and the first bytes are an
  ID3 tag or a frame sync. It skips the ID3v2 tag by its own length, then walks frame headers to the
  end of the file, recording each frame's payload offset, byte length and sample count. Headers only:
  no audio is decoded, so it is one sequential pass.
* **Duration** is the sum of the indexed frames' sample counts, not `bytes * 8 / bitrate` and not
  anything derived from the file size. `getFileLengthMS` and `AIL_stream_ms_position` both read it,
  which is what makes looping and event timing right.
* **A truncated final frame is not indexed.** `USA_09.mp3`'s last header promises 436 bytes that do
  not exist; the scan stops there, so nothing tries to half-decode it and the reported length is the
  length of the audio that is really present.
* **`decodeMpegChunk`** decodes whole indexed frames into about a buffer's worth of PCM, reading the
  compressed bytes through the engine's installed file callbacks. It checks every frame's decoded
  sample count, rate and channel count against what the index said; a disagreement stops the stream
  with an error rather than queueing whatever is in the buffer.
* **Seeking and looping prime the decoder.** Layer III frames depend on the bit reservoir and the
  filterbank overlap of the frames before them, so decoding cold at a midstream frame produces
  nothing. `MPEG_PRIME_FRAMES` (4) frames before the target are decoded into scratch and discarded
  first. This was not theoretical: the first working build drained all 56 tracks correctly and failed
  **every** seek assertion until the priming was added.
* **Failure stays loud.** A stream that cannot be indexed fails to open, with an error naming what was
  wrong with the file. Nothing in the MPEG path can turn an undecodable track into a zero-length
  silent stream that reports success.

Windows is untouched: real Miles still provides `AIL_*` there, and none of this compiles into it.

## 4. Proof, on samples rather than return codes

`scripts/audio-music-probe.py` extracts all 56 tracks outside the repository and runs each one
through the engine's own sequence — `AIL_open_stream` with the engine's file callbacks installed, then
the real `readChunk` — asserting on the PCM that comes out:

```
$ CLANGXX=clang++-14 python3 scripts/audio-music-probe.py \
      --data ~/gamedata/full/GeneralsMD --data ~/gamedata/full/Generals
tracks found   : 56
tracks decoded : 56 of 56
Assertions: 450 ok, 0 failed
```

1,400 MB of PCM decoded, and per track:

| Assertion | Result over 56 tracks |
|---|---|
| opens as an MPEG stream, every frame indexed | 56/56, index length equals the independent walk |
| rate, channels, 16-bit are the file's own | 56/56, including the MPEG-2 mono 22050 Hz track |
| reported duration matches the frame walk | 56/56 within **0.98 ms** worst case |
| drains to the end of the payload, exact PCM frame count | 56/56, no truncation |
| audible, not a silent success | 55/56; `Silence60.mp3` is asserted to *be* silence, since it is |
| seek lands within a frame and still decodes | 56/56 |
| **samples agree with an independent decoder** | 56/56 |

That last row is the one that turns "plausible PCM" into "the file's audio". The system `ffmpeg`
decodes each track and the two waveforms are compared: worst correlation over the whole set is
**0.99999976**, worst mean absolute difference **1.0e-07** of full scale, and the largest single
sample disagreement anywhere is **2 LSB of 16 bits** — decoder rounding, not a difference in content.
The same comparison is made *after a seek*, at the exact PCM frame the stream reports it landed on,
so a seek that misses by a frame cannot pass: worst correlation there is **0.99999982**.

(The `ffmpeg` used as the oracle is whatever the machine has, not the `--disable-everything` build
this project links — which, as section 1 measured, has no MP3 decoder at all. It is a second opinion,
and a machine without it degrades the run to a reported gap rather than a pass.)

Two tracks are additionally played in **real time** through OpenAL with the mix captured off the
device, so the assertion is not only about the decoder in isolation but about the queueing path:
`C_Chi01` and `C_Chi01b` each advance to ~2995 ms of position in 3 s of wall clock and capture
132,096 stereo frames of non-silent audio (RMS 0.082 / 0.140).

## 5. The whole path, both roots mounted

Decoding is necessary but not sufficient: the tracks also have to be *reachable*. The probe re-runs
the INI resolution with each root alone and with both, and names what is left over:

| Mounted | `MusicTrack` definitions | Resolved | Unresolved |
|---|---|---|---|
| `GeneralsMD` only | 69 | 8 | **60** |
| `Generals` only | 58 | 56 | 1 |
| **both** | 127 | **122** | **3** |

The three that remain with both roots mounted are named rather than counted: `NoFilename` twice (a
placeholder in each game's INI, one per root, which names no file at all) and `USA05Test.mp3`, which
no retail archive ships. Neither is a port defect and neither is a decoder failure — and 55 of the 56
tracks are referenced by some `MusicTrack` definition, so the set the engine can reach and the set
that decodes are the same set.

## 6. What is still not proved

* **This is Linux x86-64 with OpenAL Soft, not an M1 with CoreAudio.** minimp3 is portable C with no
  x86 intrinsics required, and the same code path is what the arm64 build compiles, but the decode has
  not yet been run on the Mac. Nothing here claims otherwise.
* **Not a CI gate.** It needs the full retail object, which is not redistributable; CI keeps the source
  gates (`check-openal-symbols.py`, `check-audio-backend-linked.py`) and the level 1-4 strict link.
* **The music is not yet wired into a running game frame.** The engine still stops in
  `Do_Onetime_Device_Dependent_Inits()` (see `docs/porting/renderer-integration-arm64.md`), so what is
  proved is the audio path on retail bytes, not music heard during play.
