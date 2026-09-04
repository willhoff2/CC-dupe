#!/usr/bin/env python3
"""Count click-shaped discontinuities in a rendered PCM file.

The OpenAL Soft `wave` backend writes exactly what the mixer produced, so a WAV rendered from the
game (or from a shim unit test) carries every data-path artefact -- a truncated block, a decoder
frame glued in the wrong place, a source rewound mid-waveform, a refill that arrived late enough for
the mixer to run dry -- as a sample-to-sample jump the music itself would never contain. Real-time
underruns cannot appear in a file render (the mixer waits for the file), so a clean render says the
data path is clean and nothing more; a dirty render is a deterministic defect.

Two detectors, both cheap and both explainable:

  * `jumps`: frames where |x[n] - x[n-1]| exceeds `--jump` full-scale on any channel. Music at
    44.1 kHz stays far below 0.3 FS between adjacent samples; a hard edit or a dropout boundary
    does not.
  * `gaps`: runs of digital silence at least `--gap-ms` long that begin and end inside a
    non-silent region. A starved ring of stream buffers, or a source that stopped and was
    restarted, leaves exactly this hole.

Usage:
    python3 scripts/audio-pcm-discontinuity.py out.wav [--jump 0.3] [--gap-ms 2] [--json r.json]
    python3 scripts/audio-pcm-discontinuity.py out.wav --start 30 --end 90   # seconds
"""

import argparse
import json
import pathlib
import struct
import sys

import numpy as np

WAVE_FORMAT_PCM = 1
WAVE_FORMAT_IEEE_FLOAT = 3
WAVE_FORMAT_EXTENSIBLE = 0xFFFE


def read_wave(path):
    """(rate, channels, float32 array of shape (frames, channels)). RIFF only; no dependencies."""
    data = path.read_bytes()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError(f"{path}: not a RIFF/WAVE file")
    at = 12
    fmt = None
    payload = None
    while at + 8 <= len(data):
        chunk_id = data[at:at + 4]
        size = struct.unpack_from("<I", data, at + 4)[0]
        body = data[at + 8:at + 8 + size]
        if chunk_id == b"fmt ":
            fmt = body
        elif chunk_id == b"data":
            # The wave backend writes the size only on a clean close; a killed game leaves 0 or a
            # stale value, so the payload is whatever follows the header.
            payload = data[at + 8:] if size == 0 or at + 8 + size > len(data) else body
            break
        at += 8 + size + (size & 1)
    if fmt is None or payload is None:
        raise ValueError(f"{path}: fmt or data chunk missing")
    tag, channels, rate, _, _, bits = struct.unpack_from("<HHIIHH", fmt, 0)
    if tag == WAVE_FORMAT_EXTENSIBLE and len(fmt) >= 26:
        tag = struct.unpack_from("<H", fmt, 24)[0]
    if tag == WAVE_FORMAT_PCM and bits == 16:
        samples = np.frombuffer(payload[:len(payload) - len(payload) % (2 * channels)],
                                dtype="<i2").astype(np.float32) / 32768.0
    elif tag == WAVE_FORMAT_PCM and bits == 8:
        samples = (np.frombuffer(payload[:len(payload) - len(payload) % channels],
                                 dtype=np.uint8).astype(np.float32) - 128.0) / 128.0
    elif tag == WAVE_FORMAT_IEEE_FLOAT and bits == 32:
        samples = np.frombuffer(payload[:len(payload) - len(payload) % (4 * channels)],
                                dtype="<f4").astype(np.float32)
    else:
        raise ValueError(f"{path}: unsupported format tag={tag} bits={bits}")
    return rate, channels, samples.reshape(-1, channels)


def find_jumps(frames, threshold):
    """Frame indices where any channel moves more than `threshold` between adjacent samples."""
    if frames.shape[0] < 2:
        return np.zeros(0, dtype=np.int64)
    delta = np.abs(np.diff(frames, axis=0)).max(axis=1)
    return np.nonzero(delta > threshold)[0] + 1


def find_gaps(frames, rate, min_ms, silence=1.0 / 32768.0):
    """(start_frame, length_frames) of interior silences at least `min_ms` long.

    Leading and trailing silence are not gaps: the render starts before the first voice and ends
    after the last one, and both are expected to be silent.
    """
    quiet = np.abs(frames).max(axis=1) <= silence
    if not quiet.any():
        return []
    edges = np.diff(quiet.astype(np.int8))
    starts = np.nonzero(edges == 1)[0] + 1
    ends = np.nonzero(edges == -1)[0] + 1
    if quiet[0]:
        starts = np.concatenate(([0], starts))
    if quiet[-1]:
        ends = np.concatenate((ends, [len(quiet)]))
    min_frames = int(rate * min_ms / 1000.0)
    gaps = []
    for start, end in zip(starts, ends):
        if start == 0 or end == len(quiet):
            continue
        if end - start >= min_frames:
            gaps.append((int(start), int(end - start)))
    return gaps


def analyse(path, jump, gap_ms, start_s, end_s):
    rate, channels, frames = read_wave(path)
    total_frames = frames.shape[0]
    lo = int(start_s * rate) if start_s else 0
    hi = int(end_s * rate) if end_s else total_frames
    frames = frames[lo:hi]
    jumps = find_jumps(frames, jump)
    gaps = find_gaps(frames, rate, gap_ms)
    seconds = frames.shape[0] / float(rate) if rate else 0.0
    nonsilent = int((np.abs(frames).max(axis=1) > 1.0 / 32768.0).sum())
    return {
        "file": str(path),
        "rate": rate,
        "channels": channels,
        "frames": int(frames.shape[0]),
        "seconds": round(seconds, 3),
        "nonsilent_frames": nonsilent,
        "nonsilent_seconds": round(nonsilent / float(rate), 3) if rate else 0.0,
        "peak": round(float(np.abs(frames).max()) if frames.size else 0.0, 4),
        "jump_threshold": jump,
        "jumps": int(jumps.size),
        "jumps_per_minute": round(jumps.size / (seconds / 60.0), 3) if seconds else 0.0,
        "first_jumps_s": [round((lo + int(i)) / rate, 3) for i in jumps[:20]],
        "gap_min_ms": gap_ms,
        "gaps": len(gaps),
        "gap_frames_total": int(sum(length for _, length in gaps)),
        "first_gaps_s": [[round((lo + s) / rate, 3), round(n * 1000.0 / rate, 1)]
                         for s, n in gaps[:20]],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("wav", type=pathlib.Path)
    parser.add_argument("--jump", type=float, default=0.3,
                        help="sample-to-sample jump, full scale, that counts as a click")
    parser.add_argument("--gap-ms", type=float, default=2.0,
                        help="shortest interior silence that counts as a dropout")
    parser.add_argument("--start", type=float, default=None, help="analyse from this second")
    parser.add_argument("--end", type=float, default=None, help="analyse up to this second")
    parser.add_argument("--json", type=pathlib.Path, help="write the facts here")
    parser.add_argument("--max-jumps", type=int, default=None,
                        help="exit 1 when more jumps than this are found")
    parser.add_argument("--max-gaps", type=int, default=None,
                        help="exit 1 when more gaps than this are found")
    args = parser.parse_args()

    facts = analyse(args.wav, args.jump, args.gap_ms, args.start, args.end)
    text = json.dumps(facts, indent=2)
    print(text)
    if args.json:
        args.json.write_text(text + "\n")
    failed = False
    if args.max_jumps is not None and facts["jumps"] > args.max_jumps:
        print(f"FAIL: {facts['jumps']} jumps > {args.max_jumps}", file=sys.stderr)
        failed = True
    if args.max_gaps is not None and facts["gaps"] > args.max_gaps:
        print(f"FAIL: {facts['gaps']} gaps > {args.max_gaps}", file=sys.stderr)
        failed = True
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
