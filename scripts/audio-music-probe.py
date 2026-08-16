#!/usr/bin/env python3
"""Decode every retail music track through the engine's own stream path and measure the PCM.

The audio slice (#102) measured that all the retail music is MP3 and that nothing could decode it.
This is the other side of that finding: it opens each track with AIL_open_stream through the same
file callbacks the engine installs, drains the stream's own decoder, and asserts on the samples.

What is asserted, per track, and why each one is here:

  * the stream OPENS, and reports MPEG -- a track that fell back to "not a WAV" would otherwise look
    like a missing file;
  * the PCM is NOT SILENT -- the defect class this project exists to catch is a success that plays
    nothing. `Silence60.mp3` is retail silence by design, so it is asserted to be silent, and is
    named as the exception rather than quietly excluded;
  * the sample rate and channel count are the file's own, from an independent header walk
    (audio_retail_assets.mpeg_facts), not from the decoder;
  * the DURATION AIL_stream_ms_position reports matches that walk, which is what
    MilesAudioManager::getFileLengthMS returns and what music fades and event timing are computed
    from;
  * the decode reaches the END of the payload and produces exactly as many PCM frames as the indexed
    frames account for -- a truncation check that does not depend on amplitude, since retail tracks
    legitimately fade to silence;
  * SEEKING lands where it was asked to, still decodes, and decodes the audio that belongs at that
    time, since a seek that silently misses is invisible in playback;
  * the samples ARE THE FILE'S AUDIO, not merely plausible PCM: they are compared with the system
    ffmpeg's own decode of the same file, which is an independent decoder rather than this one
    checking itself. (The ffmpeg this project pins and links cannot do it: it is configured
    `--disable-everything` and has no MP3 decoder, which is why minimp3 is linked instead.)

Two tracks are also played in real time through OpenAL with the mix captured, so the assertion is
not only about the decoder in isolation: the whole queueing path is exercised on real bytes. The
other 54 are drained at full speed, because the set is 2h 13m of audio.

Both roots must be given: `MusicZH.big` holds 7 tracks and the base game's `Music.big` holds the
other 49, which are exactly the MusicTrack references that go unresolved with only the Zero Hour
root mounted. The report says what that number is with each root and with both.

Not a CI gate: it needs the full retail game-data object, which is not redistributable. No retail
bytes and no decoded audio are written into the repository.

Usage:
    python3 scripts/audio-music-probe.py --data ~/gamedata/full/GeneralsMD \
        --data ~/gamedata/full/Generals [--json out.json]

Exits non-zero if any assertion fails.
"""

import argparse
import importlib.util
import json
import pathlib
import shutil
import subprocess
import sys
import tempfile

import numpy

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import audio_retail_assets as ara  # noqa: E402  (path shim must run first)


def load_module(name, path):
    """Imports a sibling script whose file name is not an identifier."""
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


np = load_module("native_audio_probe", HERE / "native-audio-probe.py")
sv = load_module("audio_retail_survey", HERE / "audio-retail-survey.py")

# Duration tolerance. The C++ index and the Python walk both sum whole frames, so they should agree
# exactly; the millisecond conversions are integer in C++ and float here, hence one millisecond.
DURATION_TOLERANCE_MS = 1

# RMS below this is silence for the purposes of "did anything decode". A 16-bit fade-out floor sits
# well above it, and dither alone sits below it.
SILENCE_RMS = 0.0005

# Tracks that are silence in the retail data itself. Asserted to BE silent, so that neither a broken
# decoder nor a special case can hide behind them.
SILENT_BY_DESIGN = ("silence60.mp3",)

# How many tracks are additionally played in real time through OpenAL with the mix captured.
REALTIME_TRACKS = 2

# Agreement with the oracle decoder. Two conformant MP3 decoders differ by a few least-significant
# bits (the standard specifies limited accuracy, not exact output), so this is a signal comparison:
# a correlation this high with a mean difference this small cannot be silence, noise, a channel
# swap, a wrong sample rate or a drifting frame index, and a correct decode meets it with room to
# spare (the measured retail figures are in the report, not asserted from memory here).
ORACLE_CORRELATION = 0.999
ORACLE_MEAN_DIFFERENCE = 0.002


def music_entries(data):
    """Every MPEG music entry in the mounted archives, with the archive it came from."""
    found = {}
    for archive in data.archives:
        for entry in archive.order:
            if not entry.lower().endswith((".mp3", ".mp2")):
                continue
            key = ara.normalise(entry)
            if key in found:
                continue                    # first archive wins, as the engine's search does
            found[key] = {"entry": entry, "archive": archive.path.name,
                          "entry_bytes": archive.size_of(entry)}
    return dict(sorted(found.items()))


def unresolved_music_names(events, settings, entries):
    """The MusicTrack definitions whose every candidate path is absent from the mounted archives."""
    missing = []
    for event in events:
        if event["kind"] != "MusicTrack":
            continue
        for name, candidates in ara.event_paths(event, settings):
            if not any(ara.normalise(candidate) in entries for candidate in candidates):
                missing.append(name)
    return sorted(set(missing))


def music_track_resolution(roots, verbose=False):
    """Unresolved MusicTrack references with each root alone and with all of them mounted.

    This is the number that says the music is *reachable*, not merely decodable: 49 of the 56 tracks
    live in the base game's archives, so a Zero Hour-only mount leaves their MusicTrack definitions
    pointing at nothing.
    """
    out = {}
    for label, mounted in [*[(root, [root]) for root in roots], ("all", list(roots))]:
        data = ara.GameData(mounted)
        if not data.archives:
            out[label] = {"error": "no .big archives"}
            continue
        settings, events = sv.load_settings_and_events(data, verbose)
        if not settings:
            out[label] = {"error": "no AudioSettings block"}
            continue
        _archives, entries = sv.survey_archives(data, settings)
        resolved = sv.resolve_events(events, settings, entries)
        out[label] = {
            "music_track_definitions": resolved["definitions"].get("MusicTrack", 0),
            "resolved": resolved["resolved_references"].get("MusicTrack", 0),
            "unresolved": resolved["unresolved_references"].get("MusicTrack", 0),
            "distinct_files": resolved["distinct_files"].get("MusicTrack", 0),
            # Named, not just counted: "3 unresolved" is only honest if it says which three, since
            # otherwise a decoder bug and a genuinely absent retail file look identical.
            "unresolved_names": unresolved_music_names(events, settings, entries),
        }
    return out


def read_pcm(path, channels, byte_limit=None, byte_offset=0):
    """Interleaved 16-bit PCM as an (frames, channels) array of floats in [-1, 1)."""
    raw = numpy.fromfile(path, dtype="<i2")
    if byte_offset:
        raw = raw[byte_offset // 2:]
    if byte_limit is not None:
        raw = raw[:byte_limit // 2]
    frames = len(raw) // max(1, channels)
    return raw[:frames * channels].astype(numpy.float32).reshape(frames, channels) / 32768.0


def measure_pcm(samples):
    """RMS, peak and frame count of a PCM array, as fractions of full scale."""
    if samples.shape[0] == 0:
        return {"frames": 0, "rms": [0.0] * samples.shape[1], "peak": [0.0] * samples.shape[1]}
    return {"frames": int(samples.shape[0]),
            "rms": [float(value) for value in numpy.sqrt((samples ** 2).mean(axis=0))],
            "peak": [float(value) for value in numpy.abs(samples).max(axis=0)]}


def ffmpeg_decode(asset, destination):
    """The independent decode oracle: the system ffmpeg's own MP3 decoder, as raw 16-bit PCM.

    Not the ffmpeg this project pins and links -- that one is configured `--disable-everything` and
    has no MP3 decoder at all, which is why minimp3 is linked in the first place. This is whatever
    ffmpeg the machine has, used only as a second opinion, so a missing one degrades the run to a
    reported gap rather than failing it.
    """
    if shutil.which("ffmpeg") is None:
        return None
    result = subprocess.run(["ffmpeg", "-v", "error", "-i", str(asset),
                             "-f", "s16le", "-acodec", "pcm_s16le", "-y", str(destination)],
                            capture_output=True, text=True)
    if result.returncode != 0 or not destination.exists():
        return None
    return destination


def compare_to_oracle(mine, theirs):
    """How far this decoder's samples are from ffmpeg's for the same file.

    Two conformant MP3 decoders do not agree bit for bit -- the standard specifies limited accuracy,
    and minimp3 and ffmpeg differ in their IMDCT and rounding -- so the assertion is that the
    waveforms are the same signal: the difference is small against full scale and the correlation is
    near one. A decoder that produced silence, noise, the wrong rate or a channel swap fails both.
    """
    frames = min(mine.shape[0], theirs.shape[0])
    channels = min(mine.shape[1], theirs.shape[1])
    if frames == 0 or channels == 0:
        return {"compared": False, "my_frames": int(mine.shape[0]),
                "oracle_frames": int(theirs.shape[0])}
    left = mine[:frames, :channels].ravel()
    right = theirs[:frames, :channels].ravel()
    difference = left - right
    denominator = float(numpy.sqrt((left ** 2).sum()) * numpy.sqrt((right ** 2).sum()))
    if denominator == 0.0:
        # Both decodes are digital silence, which the retail Silence60.mp3 is: a correlation is
        # undefined there, and the samples being identical is the whole of the agreement.
        correlation = 1.0 if float(numpy.abs(difference).max()) == 0.0 else 0.0
    else:
        correlation = float((left * right).sum() / denominator)
    return {
        "compared": True,
        "my_frames": int(mine.shape[0]),
        "oracle_frames": int(theirs.shape[0]),
        "frame_difference": int(mine.shape[0]) - int(theirs.shape[0]),
        "mean_abs_difference": float(numpy.abs(difference).mean()),
        "max_abs_difference": float(numpy.abs(difference).max()),
        "correlation": correlation,
    }


def reference_facts(raw):
    """The independent frame walk of one track's bytes."""
    facts = ara.mpeg_facts(raw)
    if facts is None:
        return None
    # The frame list itself is large and is not evidence once summarised; the summary is.
    return {key: value for key, value in facts.items() if key != "frames"}


def probe_track(binary, work, label, asset, reference, verbose):
    """Drains one track, measures its PCM, and compares it with the oracle's decode of the same."""
    raw_pcm = work / f"{label}.s16le"
    seek_ms = int(reference["duration_ms"] / 2)
    facts, _ = np.probe(binary, work, ["stream-drain", str(asset), str(raw_pcm), str(seek_ms)],
                        verbose=verbose)
    channels = reference["channels"]
    drained_bytes = facts.get("decoded_pcm_bytes") or 0
    if not raw_pcm.exists():
        return facts

    mine = read_pcm(raw_pcm, channels, byte_limit=drained_bytes)
    facts["pcm"] = measure_pcm(mine)

    seek_offset = facts.get("seek_pcm_offset_bytes")
    after_seek = read_pcm(raw_pcm, channels, byte_offset=seek_offset) \
        if seek_offset is not None else None
    if after_seek is not None:
        facts["pcm_after_seek"] = measure_pcm(after_seek)

    oracle_raw = ffmpeg_decode(asset, work / f"{label}.oracle.s16le")
    if oracle_raw is None:
        facts["oracle"] = {"compared": False, "reason": "no ffmpeg with an MP3 decoder on this box"}
    else:
        theirs = read_pcm(oracle_raw, channels)
        facts["oracle"] = compare_to_oracle(mine, theirs)
        if after_seek is not None and facts.get("seek_frames_played") is not None:
            # Where the seek says it landed, in the oracle's own samples: this is what turns "the
            # seek reported a plausible number" into "the audio after the seek is the audio that
            # belongs at that time". The offset is the exact PCM frame the stream reports, not the
            # rounded millisecond, because a millisecond is 44 samples of misalignment.
            at = int(facts["seek_frames_played"])
            facts["seek_oracle"] = compare_to_oracle(after_seek,
                                                     theirs[at:at + after_seek.shape[0]])
        oracle_raw.unlink()
    raw_pcm.unlink()
    return facts


def track_findings(label, asset_facts, reference, facts):
    """The per-track assertions. Returns a list of findings."""
    entry = asset_facts["entry"]
    out = []

    def add(name, ok, detail):
        out.append({"finding": f"{name}: {label}", "kind": "gate", "ok": bool(ok),
                    "detail": f"{entry} {detail}"})

    pcm = facts.get("pcm") or {}
    rms = pcm.get("rms") or [0.0]
    silent_by_design = pathlib.PurePosixPath(entry.replace("\\", "/")).name.lower() \
        in SILENT_BY_DESIGN
    reported_ms = facts.get("stream_length_ms") or 0
    expected_ms = reference["duration_ms"]

    add("retail MP3 opens as an MPEG stream",
        facts.get("open_stream_handle") and facts.get("stream_codec_is_mpeg")
        and not facts.get("open_stream_last_error"),
        f"handle={facts.get('open_stream_handle')} mpeg={facts.get('stream_codec_is_mpeg')} "
        f"indexed_frames={facts.get('stream_mpeg_frames_indexed')}/{reference['frame_count']} "
        f"last_error={facts.get('open_stream_last_error')!r}")

    add("the decoded format is the file's own",
        facts.get("stream_playback_rate") == reference["rate"]
        and facts.get("stream_channels") == reference["channels"]
        and facts.get("stream_bits") == 16,
        f"rate={facts.get('stream_playback_rate')}/{reference['rate']} "
        f"channels={facts.get('stream_channels')}/{reference['channels']} "
        f"bits={facts.get('stream_bits')} layer={reference['layer']} "
        f"mpeg={reference['mpeg_version']} bitrates={reference['bitrates_kbps']}")

    add("the reported duration matches an independent frame walk",
        abs(reported_ms - expected_ms) <= DURATION_TOLERANCE_MS,
        f"length_ms={reported_ms} expected_ms={expected_ms:.1f} "
        f"samples={facts.get('stream_total_frames')}/{reference['samples']} vbr={reference['vbr']}")

    add("the whole payload decodes, with no truncation",
        facts.get("drained_to_end")
        and pcm.get("frames") == reference["samples"]
        and not facts.get("drain_last_error"),
        f"decoded_frames={pcm.get('frames')}/{reference['samples']} "
        f"drained_to_end={facts.get('drained_to_end')} "
        f"read_cursor={facts.get('read_cursor')}/{facts.get('stream_payload_bytes')} "
        f"last_error={facts.get('drain_last_error')!r}")

    if silent_by_design:
        add("retail silence decodes as silence, not as noise",
            pcm.get("frames", 0) > 0 and max(rms) < SILENCE_RMS,
            f"rms={rms} peak={pcm.get('peak')} (this track IS silence in the retail data)")
    else:
        add("the PCM is audible, not a silent success",
            max(rms) > SILENCE_RMS,
            f"rms={rms} peak={pcm.get('peak')}")

    seek_requested = facts.get("seek_requested_ms")
    if seek_requested:
        # The index lands on a frame boundary, so the reported position is the start of the frame
        # containing the requested time: within one frame, not exact.
        frame_ms = 1000.0 * 1152 / max(1, reference["rate"])
        add("a seek lands within a frame of where it was asked to, and still decodes",
            facts.get("seek_decoded_bytes", 0) > 0
            and abs((facts.get("seek_reported_ms") or 0) - seek_requested) <= frame_ms + 1,
            f"requested_ms={seek_requested} reported_ms={facts.get('seek_reported_ms')} "
            f"decoded_bytes={facts.get('seek_decoded_bytes')} "
            f"last_error={facts.get('seek_last_error')!r}")

    oracle = facts.get("oracle") or {}
    if oracle.get("compared"):
        add("the samples are ffmpeg's samples, within decoder tolerance",
            oracle["correlation"] >= ORACLE_CORRELATION
            and oracle["mean_abs_difference"] <= ORACLE_MEAN_DIFFERENCE
            and abs(oracle["frame_difference"]) <= reference["rate"] // 20,
            f"correlation={oracle['correlation']:.6f} "
            f"mean_abs_difference={oracle['mean_abs_difference']:.6f} "
            f"max_abs_difference={oracle['max_abs_difference']:.4f} "
            f"frames={oracle['my_frames']} oracle_frames={oracle['oracle_frames']}")
    else:
        out.append({"finding": f"the ffmpeg oracle was not available: {label}", "kind": "observed",
                    "ok": True, "detail": f"{entry} {oracle.get('reason', 'no comparison')}"})

    seek_oracle = facts.get("seek_oracle") or {}
    if seek_oracle.get("compared"):
        add("the audio after a seek is the audio that belongs at that time",
            seek_oracle["correlation"] >= ORACLE_CORRELATION
            and seek_oracle["mean_abs_difference"] <= ORACLE_MEAN_DIFFERENCE,
            f"correlation={seek_oracle['correlation']:.6f} "
            f"mean_abs_difference={seek_oracle['mean_abs_difference']:.6f} "
            f"frames={seek_oracle['my_frames']} at_frame={facts.get('seek_frames_played')} "
            f"at_ms={facts.get('seek_reported_ms')}")
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--data", action="append", required=True,
                        help="a directory of retail .big archives; repeatable, and both the Zero "
                             "Hour and base-game roots are needed for the whole music set")
    parser.add_argument("--json", help="write the collected facts to this path")
    parser.add_argument("--limit", type=int, help="probe only the first N tracks (for a smoke run)")
    parser.add_argument("--keep", action="store_true")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    for root in args.data:
        if not pathlib.Path(root).is_dir():
            print(f"error: {root} is not a directory", file=sys.stderr)
            return 1

    data = ara.GameData(args.data)
    if not data.archives:
        print("error: no .big archives found", file=sys.stderr)
        return 1
    tracks = music_entries(data)
    if not tracks:
        print("error: no .mp3/.mp2 entries in the mounted archives; is the base game root mounted?",
              file=sys.stderr)
        return 1

    work = pathlib.Path(tempfile.mkdtemp(prefix="audio-music-probe-"))
    report = {"roots": args.data, "work_dir": str(work),
              "archives_read": [archive.path.name for archive in data.archives],
              "tracks_found": len(tracks), "tracks": {}, "findings": []}
    try:
        report["music_track_references"] = music_track_resolution(args.data, args.verbose)
        binary = np.build(work, args.verbose)
        if binary is None:
            return 1

        selected = list(tracks.items())[:args.limit] if args.limit else list(tracks.items())
        decoded = 0
        failed = []
        for index, (key, asset_facts) in enumerate(selected):
            label = pathlib.PurePosixPath(asset_facts["entry"].replace("\\", "/")).stem
            raw = data.read(asset_facts["entry"])
            reference = reference_facts(raw)
            if reference is None:
                report["findings"].append({
                    "finding": f"the track is MPEG audio at all: {label}", "kind": "gate",
                    "ok": False, "detail": f"{asset_facts['entry']} no frame header found"})
                failed.append(asset_facts["entry"])
                continue
            asset = work / pathlib.PurePosixPath(asset_facts["entry"].replace("\\", "/")).name
            asset.write_bytes(raw)

            facts = probe_track(binary, work, label, asset, reference, args.verbose)
            findings = track_findings(label, asset_facts, reference, facts)
            if index < REALTIME_TRACKS:
                # The realtime path, through OpenAL, on real bytes: a few seconds is enough to say
                # the queueing works, and the capture is measured rather than trusted.
                played, capture = np.probe(binary, work, ["stream", str(asset)],
                                           capture_name=f"music-{label}", verbose=args.verbose)
                played["capture"] = np.measure_capture(capture)
                facts["realtime"] = played
                mix = played["capture"].get("rms") or [0.0]
                findings.append({
                    "finding": f"the track plays through OpenAL in real time, audibly: {label}",
                    "kind": "gate",
                    "ok": bool(played.get("open_stream_handle")
                               and played.get("stream_position_advanced")
                               and max(mix) > SILENCE_RMS),
                    "detail": f"{asset_facts['entry']} "
                              f"high_water_ms={played.get('stream_high_water_position_ms')} "
                              f"mix_rms={mix}"})
            asset.unlink()

            report["tracks"][key] = {"asset": asset_facts, "reference": reference,
                                     "probe": {name: value for name, value in facts.items()
                                               if name != "stderr"}}
            report["findings"].extend(findings)
            if all(finding["ok"] for finding in findings):
                decoded += 1
            else:
                failed.append(asset_facts["entry"])

        report["decoded"] = decoded
        report["attempted"] = len(selected)
        report["failed_tracks"] = failed
        print_report(report)
        if args.json:
            pathlib.Path(args.json).write_text(json.dumps(report, indent=2) + "\n")
            print(f"\nwrote {args.json}")
        return 0 if decoded == len(selected) else 1
    finally:
        if args.keep:
            print(f"\nkept {work}")
        else:
            shutil.rmtree(work, ignore_errors=True)


def print_report(report):
    print("Retail music decode probe")
    print("=" * 78)
    print(f"roots          : {', '.join(report['roots'])}")
    print(f"tracks found   : {report['tracks_found']}")
    print(f"tracks decoded : {report.get('decoded')} of {report.get('attempted')}")
    if report.get("failed_tracks"):
        print("failed tracks  :")
        for entry in report["failed_tracks"]:
            print(f"  {entry}")

    print("\nMusicTrack references per mount")
    for label, facts in report.get("music_track_references", {}).items():
        if "error" in facts:
            print(f"  {label}: {facts['error']}")
            continue
        print(f"  {label}: {facts['music_track_definitions']} definitions, "
              f"{facts['resolved']} resolved, {facts['unresolved']} unresolved, "
              f"{facts['distinct_files']} distinct files")
        for name in facts.get("unresolved_names", []):
            print(f"      unresolved: {name}")

    failures = [f for f in report["findings"] if not f["ok"]]
    print(f"\nAssertions: {len(report['findings']) - len(failures)} ok, {len(failures)} failed")
    for finding in failures:
        print(f"  [FAIL] {finding['finding']}")
        print(f"         {finding['detail']}")


if __name__ == "__main__":
    sys.exit(main())
