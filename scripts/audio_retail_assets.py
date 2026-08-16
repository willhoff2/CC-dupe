#!/usr/bin/env python3
"""Reader for the retail audio the game actually ships, inside its own .big archives.

Every audio measurement in docs/porting/ before this module was taken against synthetic ffmpeg
assets, because no probe had the retail archives. This module is the other half: it reads
`AudioZH.big`, `AudioEnglishZH.big`, `SpeechZH.big`, `SpeechEnglishZH.big` and `MusicZH.big`
directly, parses the container and codec of every audio entry, and resolves the engine's own INI
audio definitions onto those entries, so that "which retail assets does this defect break" is a
count rather than an inference.

It is a library: `audio-retail-survey.py` prints the survey and `native-audio-probe.py` extracts
representative retail assets through it. Nothing here writes retail bytes into the repository, and
callers must not either -- the archives are not redistributable.

BIGF layout, as read by Core/GameEngine/Source/Common/System/ArchiveFile*.cpp: a 4-byte "BIGF"
magic, big-endian archive size, big-endian entry count, big-endian offset of the first entry, then
one record per entry of big-endian offset, big-endian size and a NUL-terminated Windows-style path.
"""

import pathlib
import struct

# The subset of the audio surface Zero Hour uses. .bik video carries its own audio and is handled
# by the video seam (docs/porting/video-seam.md), not here.
AUDIO_EXTENSIONS = (".wav", ".mp3", ".mp2")

WAVE_FORMAT_PCM = 1
WAVE_FORMAT_MS_ADPCM = 2
WAVE_FORMAT_IMA_ADPCM = 17

WAVE_FORMAT_NAMES = {
    WAVE_FORMAT_PCM: "pcm",
    WAVE_FORMAT_MS_ADPCM: "ms_adpcm",
    WAVE_FORMAT_IMA_ADPCM: "ima_adpcm",
}

MPEG_RATES = {
    3: (44100, 48000, 32000),   # MPEG-1
    2: (22050, 24000, 16000),   # MPEG-2
    0: (11025, 12000, 8000),    # MPEG-2.5
}

MPEG1_BITRATES = {
    3: (0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448),   # layer I
    2: (0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384),      # layer II
    1: (0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320),       # layer III
}


class BigArchive:
    """One .big archive, opened lazily: entries are read on demand, never all at once."""

    def __init__(self, path):
        self.path = pathlib.Path(path)
        self.entries = {}
        self.order = []
        with self.path.open("rb") as handle:
            magic = handle.read(4)
            if magic not in (b"BIGF", b"BIG4"):
                raise ValueError(f"{self.path.name}: not a BIG archive (magic {magic!r})")
            handle.read(4)  # archive size, big-endian; not needed and not always accurate
            count, _first = struct.unpack(">II", handle.read(8))
            for _ in range(count):
                offset, size = struct.unpack(">II", handle.read(8))
                name = bytearray()
                while True:
                    byte = handle.read(1)
                    if byte in (b"\0", b""):
                        break
                    name += byte
                entry = name.decode("latin-1")
                key = normalise(entry)
                self.entries[key] = (offset, size)
                self.order.append(entry)

    def size_of(self, name):
        return self.entries[normalise(name)][1]

    def read(self, name, limit=None):
        offset, size = self.entries[normalise(name)]
        want = size if limit is None else min(size, limit)
        with self.path.open("rb") as handle:
            handle.seek(offset)
            return handle.read(want)

    def audio_entries(self):
        """Entry names whose extension is one the audio path handles."""
        return [name for name in self.order if name.lower().endswith(AUDIO_EXTENSIONS)]


def normalise(name):
    """Archive lookups are case-insensitive and slash-insensitive, as the engine's are."""
    return name.replace("/", "\\").lower()


class GameData:
    """The archives of one installation, searched in the order the engine searches them."""

    def __init__(self, roots):
        self.archives = []
        for root in roots:
            for path in sorted(pathlib.Path(root).glob("*.big")):
                try:
                    self.archives.append(BigArchive(path))
                except ValueError:
                    continue

    def audio_archives(self):
        return [a for a in self.archives if a.audio_entries()]

    def find(self, path):
        """(archive, entry name) for a Windows-style archive path, or None."""
        key = normalise(path)
        for archive in self.archives:
            if key in archive.entries:
                return archive, path
        return None

    def read(self, path, limit=None):
        found = self.find(path)
        return None if found is None else found[0].read(found[1], limit)


# ------------------------------------------------------------------------------ codec parsing


def parse_wave(data):
    """Container facts of a WAV image: codec, layout and the extent of the payload.

    Deliberately independent of the C++ parser in OpenALWaveFile.cpp: if the two agree about
    3523 retail files, that is evidence; if the C++ one were used to check itself it would not be.
    """
    if len(data) < 12 or data[0:4] != b"RIFF" or data[8:12] != b"WAVE":
        return None
    at = 12
    out = {}
    while at + 8 <= len(data):
        tag = data[at:at + 4]
        size = struct.unpack("<I", data[at + 4:at + 8])[0]
        body = data[at + 8:at + 8 + size]
        if tag == b"fmt " and len(body) >= 16:
            fmt, channels, rate, _bytes_per_second, block_align, bits = struct.unpack(
                "<HHIIHH", body[0:16])
            out.update(format=fmt, format_name=WAVE_FORMAT_NAMES.get(fmt, f"0x{fmt:04x}"),
                       channels=channels, rate=rate, block_align=block_align, bits=bits)
            if fmt == WAVE_FORMAT_IMA_ADPCM and len(body) >= 20:
                out["samples_per_block"] = struct.unpack("<H", body[18:20])[0]
        elif tag == b"fact" and len(body) >= 4:
            out["fact_samples"] = struct.unpack("<I", body[0:4])[0]
        elif tag == b"data":
            out["data_offset"] = at + 8
            out["data_len"] = size
            break
        at += 8 + size + (size & 1)
    if "format" not in out or "data_offset" not in out:
        return None
    return out


def wave_frames(info):
    """Decoded frame count of a WAV, from the header alone."""
    if info["format"] == WAVE_FORMAT_PCM:
        frame = info["channels"] * (info["bits"] // 8)
        return info["data_len"] // frame if frame else 0
    if info["format"] == WAVE_FORMAT_IMA_ADPCM:
        block = info["block_align"]
        channels = info["channels"]
        if not block or block <= 4 * channels:
            return 0
        per_block = info.get("samples_per_block") \
            or ((block - 4 * channels) * 2) // channels + 1
        return (info["data_len"] // block) * per_block
    return 0


def wave_class(info):
    """A short label for one (codec, channels, rate, bits, block) combination."""
    return (f"{info['format_name']}_{info['channels']}ch_{info['rate']}hz"
            f"_{info['bits']}bit_block{info['block_align']}")


def parse_mpeg(data):
    """First MPEG audio frame header of an elementary stream, skipping any ID3v2 tag."""
    at = 0
    if data[0:3] == b"ID3" and len(data) > 10:
        size = 0
        for byte in data[6:10]:
            size = (size << 7) | (byte & 0x7F)
        at = 10 + size
    limit = min(len(data) - 4, at + 0x20000)
    while at <= limit:
        if data[at] == 0xFF and (data[at + 1] & 0xE0) == 0xE0:
            version = (data[at + 1] >> 3) & 0x03
            layer = (data[at + 1] >> 1) & 0x03
            rate_index = (data[at + 2] >> 2) & 0x03
            bitrate_index = (data[at + 2] >> 4) & 0x0F
            mode = (data[at + 3] >> 6) & 0x03
            if version != 1 and layer != 0 and rate_index != 3 and bitrate_index not in (0, 15):
                rates = MPEG_RATES.get(version)
                bitrates = MPEG1_BITRATES.get(layer) if version == 3 else None
                return {
                    "codec": {3: "mp1", 2: "mp2", 1: "mp3"}[layer],
                    "mpeg_version": {3: 1, 2: 2, 0: 2.5}[version],
                    "layer": {3: 1, 2: 2, 1: 3}[layer],
                    "rate": rates[rate_index] if rates else 0,
                    "channels": 1 if mode == 3 else 2,
                    "bitrate_kbps": bitrates[bitrate_index] if bitrates else 0,
                    "frame_offset": at,
                }
        at += 1
    return None


def describe(name, data):
    """Codec facts for one audio entry, from its own bytes."""
    if name.lower().endswith(".wav"):
        info = parse_wave(data)
        if info is None:
            return {"container": "wav", "codec": "unparsed"}
        return {
            "container": "wav",
            "codec": info["format_name"],
            "channels": info["channels"],
            "rate": info["rate"],
            "bits": info["bits"],
            "block_align": info["block_align"],
            "data_len": info["data_len"],
            # Where the payload starts, i.e. how many bytes of the file have to be in hand before
            # the layout is known. The stream defect was a fixed 1024-byte window, so this is the
            # field that says whether 1024 was ever the wrong size for *metadata* as opposed to
            # for payload.
            "header_bytes": info["data_offset"],
            "frames": wave_frames(info),
            "fact_samples": info.get("fact_samples"),
            "samples_per_block": info.get("samples_per_block"),
            "class": wave_class(info),
        }
    info = parse_mpeg(data)
    if info is None:
        return {"container": "mpeg", "codec": "unparsed"}
    return {
        "container": "mpeg",
        "codec": info["codec"],
        "channels": info["channels"],
        "rate": info["rate"],
        "bitrate_kbps": info["bitrate_kbps"],
        "class": f"{info['codec']}_{info['channels']}ch_{info['rate']}hz",
    }


# ------------------------------------------------------------------------ independent decoding

IMA_INDEX_TABLE = (-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8)

IMA_STEP_TABLE = (
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253,
    279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166,
    1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
    4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289,
    16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767)


def decode_ima_adpcm(data, info):
    """The IMA ADPCM reference decoder, in Python, over a WAV's payload.

    A third implementation, written from the ADPCM reference algorithm rather than from the C++
    under test or from ffmpeg: the C++ decoder is asserted to agree with this one bit for bit, so
    "the decode is right" does not rest on a decoder agreeing with itself. ffmpeg is kept as a
    second opinion, but its step update is ((2*nibble+1)*step)>>3 where the reference is
    shift-and-add, so the two differ by a few LSBs and only bound the drift.

    Returns interleaved 16-bit samples.
    """
    channels = info["channels"]
    block = info["block_align"]
    payload = data[info["data_offset"]:info["data_offset"] + info["data_len"]]
    per_block = info.get("samples_per_block") \
        or ((block - 4 * channels) * 2) // channels + 1
    out = []
    for start in range(0, len(payload) - block + 1, block):
        chunk = payload[start:start + block]
        predictors = []
        indices = []
        for channel in range(channels):
            predictor, index = struct.unpack("<hB", chunk[channel * 4:channel * 4 + 3])
            predictors.append(predictor)
            indices.append(min(88, max(0, index)))
        samples = [[predictors[channel]] for channel in range(channels)]

        # After the per-channel preamble the nibbles come in groups of four bytes per channel.
        at = 4 * channels
        while at + 4 * channels <= len(chunk) and len(samples[0]) < per_block:
            for channel in range(channels):
                group = chunk[at:at + 4]
                at += 4
                for byte in group:
                    for nibble in (byte & 0x0F, byte >> 4):
                        step = IMA_STEP_TABLE[indices[channel]]
                        difference = step >> 3
                        if nibble & 1:
                            difference += step >> 2
                        if nibble & 2:
                            difference += step >> 1
                        if nibble & 4:
                            difference += step
                        if nibble & 8:
                            difference = -difference
                        value = predictors[channel] + difference
                        value = max(-32768, min(32767, value))
                        predictors[channel] = value
                        indices[channel] = min(
                            88, max(0, indices[channel] + IMA_INDEX_TABLE[nibble]))
                        samples[channel].append(value)
        for frame in range(per_block):
            for channel in range(channels):
                out.append(samples[channel][frame] if frame < len(samples[channel]) else 0)
    return out


# --------------------------------------------------------------------------------- INI parsing


def strip_comment(line):
    return line.split(";", 1)[0].rstrip()


def parse_audio_settings(text):
    """The AudioSettings block's path fields, which decide where each audio type is looked up."""
    settings = {}
    inside = False
    for raw in text.splitlines():
        line = strip_comment(raw).strip()
        if not line:
            continue
        if line.split()[0] == "AudioSettings":
            inside = True
            continue
        if inside:
            if line.lower() == "end":
                break
            if "=" in line:
                key, value = line.split("=", 1)
                settings[key.strip()] = value.strip()
    return settings


def parse_audio_events(text):
    """AudioEvent / DialogEvent / MusicTrack blocks, with the fields that name files.

    `Sounds`/`Attack`/`Decay` name sound-effect assets without an extension; `Filename` names a
    streamed or music asset with one. That difference is the engine's, in
    AudioEventRTS::generateFilename().
    """
    events = []
    current = None
    for raw in text.splitlines():
        line = strip_comment(raw).strip()
        if not line:
            continue
        head = line.split()[0]
        if head in ("AudioEvent", "DialogEvent", "MusicTrack"):
            parts = line.split()
            current = {"kind": head, "name": parts[1] if len(parts) > 1 else "",
                       "filename": None, "sounds": []}
            continue
        if current is None:
            continue
        if line.lower() == "end":
            events.append(current)
            current = None
            continue
        if "=" not in line:
            continue
        key, value = (part.strip() for part in line.split("=", 1))
        if key.lower() == "filename":
            current["filename"] = value
        elif key.lower() in ("sounds", "attack", "decay"):
            current["sounds"].extend(value.split())
    return events


def is_streamed(entry, settings):
    """True when an archive entry lives where the engine looks for streamed audio.

    A retail asset's *codec* does not say which code path plays it; its folder does. Dialogue under
    StreamingFolder goes through AIL_open_stream, sound effects under SoundsFolder through
    AIL_set_sample_file, and the two paths have different defects.
    """
    folder = f"\\{settings.get('StreamingFolder', 'Speech')}\\".lower()
    return folder in normalise(entry)


def is_music(entry, settings):
    folder = f"\\{settings.get('MusicFolder', 'Tracks')}\\".lower()
    return folder in normalise(entry)


def select_probe_assets(data, settings, min_bytes=4096, max_bytes=600000,
                        max_music_bytes=8000000):
    """One real retail asset per codec/layout class, for the probe to decode.

    Chosen by sorted name so that two runs pick the same files, and bounded in size so that a
    stage stays a few seconds. Every class the survey finds in the retail set is represented,
    tagged with the code path the engine would use for it.

    Music has its own, larger bound: the seven retail tracks are 2.5-6 MB each, and leaving them
    out would mean the probe never put a real retail MP3 through the stream path at all.
    """
    wanted = {}
    for archive in data.audio_archives():
        for entry in sorted(archive.audio_entries()):
            size = archive.size_of(entry)
            limit = max_music_bytes if is_music(entry, settings) else max_bytes
            if size < min_bytes or size > limit:
                continue
            facts = describe(entry, archive.read(entry, 8192))
            if facts.get("codec") == "unparsed":
                continue
            path = "stream" if is_streamed(entry, settings) else (
                "music" if is_music(entry, settings) else "sample")
            label = f"{path}-{facts['class']}"
            if label not in wanted:
                wanted[label] = dict(facts, archive=str(archive.path), entry=entry,
                                     entry_bytes=size, engine_path=path)
    return dict(sorted(wanted.items()))


def extract(data, selection, directory):
    """Writes selected retail entries to a scratch directory. Never inside the repository."""
    directory = pathlib.Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    out = {}
    for label, asset in selection.items():
        archive = BigArchive(asset["archive"])
        target = directory / f"{label}{pathlib.PurePosixPath(asset['entry'].lower()).suffix}"
        target.write_bytes(archive.read(asset["entry"]))
        out[label] = target
    return out


def event_paths(event, settings, language="English"):
    """The archive paths one event can play, in the order the engine tries them.

    AudioEventRTS::generateFilenamePrefix() puts sound effects under SoundsFolder, streamed
    dialogue under StreamingFolder and music under MusicFolder, and adjustForLocalization() then
    prefers a `<folder>\\<language>\\` copy when the file system has one.
    """
    root = settings.get("AudioRoot", "Data\\Audio")
    extension = settings.get("SoundsExtension", "wav")
    if event["kind"] == "MusicTrack":
        folder = settings.get("MusicFolder", "Tracks")
        names = [event["filename"]] if event["filename"] else []
    elif event["kind"] == "DialogEvent":
        folder = settings.get("StreamingFolder", "Speech")
        names = [event["filename"]] if event["filename"] else []
    else:
        folder = settings.get("SoundsFolder", "Sounds")
        names = [f"{name}.{extension}" for name in event["sounds"]]
    paths = []
    for name in names:
        paths.append((name, [f"{root}\\{folder}\\{language}\\{name}",
                             f"{root}\\{folder}\\{name}"]))
    return paths
