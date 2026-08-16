#!/usr/bin/env python3
"""Decode and play REAL retail audio through the OpenAL Miles replacement, and measure the samples.

scripts/native-audio-probe.py drives the same AIL_* sequences against ffmpeg-synthesised assets and
says so: it decoded zero retail bytes, because no archive it could reach held audio. This script is
the retail half. It reads the retail `.big` archives (scripts/audio_retail_assets.py), extracts one
asset per codec/layout class the survey found to a scratch directory outside the repository, and
runs each through the code path the *engine* would use for it:

  * `sample`        -- AIL_set_sample_file, the one-shot path (sound effects, short dialogue);
  * `engine-adpcm`  -- AudioFileCache::openFile()'s AIL_decompress_ADPCM handoff, verbatim;
  * `stream`        -- AIL_open_stream, the path streamed dialogue and music use;
  * `sample3d`      -- a 3D voice left and right of the listener, for the handedness assertion.

The assertion is the samples, not the exit status. Every stage's mix is captured with OpenAL Soft's
wave writer and measured, so a "successful" silent decode fails; decoded lengths are checked against
the frame count computed from the file's own header by an independent parser; and where ffmpeg is
present the ADPCM decode is compared with ffmpeg's decode of the same retail bytes.

Not a CI gate: it needs the full retail game-data object (s3://cc-mac-game-data, 2.23 GB unpacked),
which is not redistributable. No retail bytes are written into the repository, and the report holds
measurements only -- counts, rates, RMS, lengths -- never audio.

Usage:
    python3 scripts/audio-retail-probe.py --data ~/gamedata/full/GeneralsMD [--json out.json]

Exits non-zero if any retail assertion fails.
"""

import argparse
import importlib.util
import json
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import audio_retail_assets as ara  # noqa: E402  (path shim must run first)


def load_module(name, path):
    """Imports a sibling script whose file name is not an identifier."""
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


# The build, capture and measurement machinery is the synthetic probe's; only the assets differ, so
# that a retail number and a synthetic number are produced by the same instrument.
np = load_module("native_audio_probe", HERE / "native-audio-probe.py")
sv = load_module("audio_retail_survey", HERE / "audio-retail-survey.py")


def ffmpeg_reference(asset, work, verbose=False):
    """ffmpeg's own decode of a retail file, as raw s16le, or None when ffmpeg cannot be used."""
    if shutil.which(np.FFMPEG) is None:
        return None
    out = work / f"{asset.stem}.reference.s16le"
    command = [np.FFMPEG, "-y", "-loglevel", "error", "-i", str(asset),
               "-f", "s16le", "-acodec", "pcm_s16le", str(out)]
    if verbose:
        print(" ".join(command))
    result = subprocess.run(command, capture_output=True, text=True)
    return out if result.returncode == 0 and out.exists() and out.stat().st_size > 0 else None


def compare_independent(asset, decoded_path):
    """Compares the backend's ADPCM decode with the Python reference decoder, bit for bit.

    ffmpeg is a second opinion but not an arbiter: its step update is ((2*nibble+1)*step)>>3 where
    the IMA reference is shift-and-add, so on real retail bytes the two legitimately differ by a few
    LSBs. audio_retail_assets.decode_ima_adpcm implements the reference algorithm from the spec, not
    from the code under test, so exact agreement with it is the assertion and the ffmpeg delta only
    bounds the drift.
    """
    raw = pathlib.Path(asset).read_bytes()
    info = ara.parse_wave(raw)
    if info is None or info["format"] != ara.WAVE_FORMAT_IMA_ADPCM:
        return {"comparable": False, "reason": "not an IMA ADPCM WAV"}
    expected = ara.decode_ima_adpcm(raw, info)
    got = np.payload_of(decoded_path)
    have = list(struct.unpack(f"<{len(got) // 2}h", got[:len(got) // 2 * 2]))
    count = min(len(expected), len(have))
    mismatches = 0
    worst = 0
    for index in range(count):
        difference = abs(have[index] - expected[index])
        if difference:
            mismatches += 1
            worst = max(worst, difference)
    return {"comparable": True, "reference_samples": len(expected), "decoded_samples": len(have),
            "compared_samples": count, "mismatched_samples": mismatches,
            "max_abs_difference": worst}


def expected_frames(asset_facts):
    """Decoded frames the file's own header implies, from the independent parser."""
    return asset_facts.get("frames", 0)


def measure(report, label, facts, asset_facts, capture, kind):
    """Records one stage and the retail expectations it has to meet."""
    report["stages"][f"{kind}:{label}"] = dict(facts, retail=asset_facts, capture=capture)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--data", action="append", required=True,
                        help="a directory of retail .big archives; repeatable")
    parser.add_argument("--json", help="write the collected facts to this path")
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
    settings, _events = sv.load_settings_and_events(data, args.verbose)
    if not settings:
        print("error: no AudioSettings block found; is this the full game-data object?",
              file=sys.stderr)
        return 1

    selection = ara.select_probe_assets(data, settings)
    if not selection:
        print("error: no retail audio entries selected", file=sys.stderr)
        return 1

    work = pathlib.Path(tempfile.mkdtemp(prefix="audio-retail-probe-"))
    report = {"work_dir": str(work), "roots": args.data, "stages": {},
              "selection": {label: {key: value for key, value in asset.items()
                                    if key != "archive"}
                            for label, asset in selection.items()}}
    try:
        binary = np.build(work, args.verbose)
        if binary is None:
            return 1

        assets = ara.extract(data, selection, work / "retail")
        for label, path in sorted(assets.items()):
            facts_of_file = selection[label]
            engine_path = facts_of_file["engine_path"]
            codec = facts_of_file["codec"]

            if engine_path == "sample":
                facts, capture = np.probe(binary, work, ["sample", str(path), "0.5"],
                                          capture_name=f"retail-sample-{label}",
                                          verbose=args.verbose)
                measure(report, label, facts, facts_of_file,
                        np.measure_capture(capture), "sample")
                if codec == "ima_adpcm":
                    # The engine's own handoff: AIL_decompress_ADPCM's buffer straight into
                    # AIL_set_sample_file, which is where retail ADPCM sound was being dropped.
                    facts, capture = np.probe(binary, work, ["engine-adpcm", str(path)],
                                              capture_name=f"retail-engine-{label}",
                                              verbose=args.verbose)
                    measure(report, label, facts, facts_of_file,
                            np.measure_capture(capture), "engine-adpcm")
                    decoded = work / f"{label}.decoded.wav"
                    facts, _ = np.probe(binary, work, ["adpcm", str(path), str(decoded)],
                                        verbose=args.verbose)
                    reference = ffmpeg_reference(path, work, args.verbose)
                    if decoded.exists() and reference is not None:
                        facts["comparison"] = np.compare_pcm(decoded, reference)
                    if decoded.exists():
                        facts["independent"] = compare_independent(path, decoded)
                    report["stages"][f"adpcm:{label}"] = dict(facts, retail=facts_of_file)
            else:
                facts, capture = np.probe(binary, work, ["stream", str(path)],
                                          capture_name=f"retail-stream-{label}",
                                          verbose=args.verbose)
                measure(report, label, facts, facts_of_file,
                        np.measure_capture(capture), "stream")

            if engine_path == "sample" and facts_of_file["channels"] == 1:
                for side, x in (("left", -20.0), ("right", 20.0)):
                    facts, capture = np.probe(
                        binary, work, ["sample3d", str(path), str(x), "0.0"],
                        capture_name=f"retail-3d-{label}-{side}", verbose=args.verbose)
                    measure(report, f"{label}-{side}", facts, facts_of_file,
                            np.measure_capture(capture), "sample3d")

        report["findings"] = summarise(report)
        print_report(report)
        if args.json:
            pathlib.Path(args.json).write_text(json.dumps(report, indent=2) + "\n")
            print(f"\nwrote {args.json}")
        gates = [f for f in report["findings"] if f["kind"] == "gate"]
        return 0 if all(finding["ok"] for finding in gates) else 1
    finally:
        if args.keep:
            print(f"\nkept {work}")
        else:
            shutil.rmtree(work, ignore_errors=True)


def summarise(report):
    """Retail assertions: the samples, the length, the rate, the channels, the direction."""
    findings = []
    stages = report["stages"]

    def add(name, ok, detail):
        findings.append({"finding": name, "kind": "gate", "ok": bool(ok), "detail": detail})

    def observe(name, detail):
        findings.append({"finding": name, "kind": "observation", "ok": True, "detail": detail})

    for key, stage in sorted(stages.items()):
        kind, label = key.split(":", 1)
        retail = stage.get("retail", {})
        capture = stage.get("capture", {})
        rms = capture.get("rms") or [0.0]
        audible = max(rms) > 0.0005

        if kind == "sample":
            expected_ms = round(1000.0 * expected_frames(retail) / max(1, retail.get("rate", 1)))
            reported_ms = stage.get("sample_length_ms") or 0
            add(f"retail one-shot decodes, is the right length and is audible: {label}",
                stage.get("set_sample_file_result") == 1
                and abs(reported_ms - expected_ms) <= max(30, expected_ms // 50)
                and stage.get("wav_rate") == retail.get("rate")
                and stage.get("wav_channels") == retail.get("channels")
                and audible,
                f"{retail.get('entry')} codec={retail.get('codec')} "
                f"rate={stage.get('wav_rate')}/{retail.get('rate')} "
                f"channels={stage.get('wav_channels')}/{retail.get('channels')} "
                f"length_ms={reported_ms} expected_ms={expected_ms} rms={rms}")

        elif kind == "engine-adpcm":
            add(f"the engine's ADPCM handoff plays real retail ADPCM: {label}",
                stage.get("decompressed_is_riff_wave")
                and stage.get("decompressed_wav_info_ok")
                and stage.get("set_sample_file_result") == 1
                and audible,
                f"{retail.get('entry')} decompressed_bytes={stage.get('decompressed_bytes')} "
                f"riff={stage.get('decompressed_is_riff_wave')} "
                f"set_sample_file={stage.get('set_sample_file_result')} rms={rms}")

        elif kind == "adpcm":
            expected_bytes = expected_frames(retail) * retail.get("channels", 1) * 2
            independent = stage.get("independent") or {}
            add(f"retail ADPCM decode matches the reference decoder exactly: {label}",
                independent.get("comparable")
                and independent.get("compared_samples", 0) > 0
                and independent.get("mismatched_samples") == 0
                and independent.get("decoded_samples") == independent.get("reference_samples")
                and abs(expected_bytes - 2 * independent.get("decoded_samples", 0))
                <= retail.get("channels", 1) * 2,
                f"{retail.get('entry')} samples={independent.get('compared_samples')} "
                f"mismatched={independent.get('mismatched_samples')} "
                f"decoded_samples={independent.get('decoded_samples')} "
                f"reference_samples={independent.get('reference_samples')} "
                f"header_implies_samples={expected_bytes // 2}")

            comparison = stage.get("comparison")
            if comparison is None:
                observe(f"MISSING TOOL: no ffmpeg cross-check for {label}",
                        "install ffmpeg to bound the decode against a third implementation")
                continue
            # A bound, not an equality: compare_independent explains why ffmpeg differs by LSBs.
            add(f"retail ADPCM decode stays within rounding distance of ffmpeg's: {label}",
                comparison.get("comparable")
                and comparison.get("max_abs_difference", 1 << 20) <= 256
                and comparison.get("rms_difference", 1 << 20) <= 64
                and abs(comparison.get("decoded_bytes", 0) - expected_bytes)
                <= retail.get("channels", 1) * 2,
                f"{retail.get('entry')} samples={comparison.get('compared_samples')} "
                f"max_abs_difference={comparison.get('max_abs_difference')} "
                f"rms_difference={comparison.get('rms_difference')} "
                f"decoded_bytes={comparison.get('decoded_bytes')} "
                f"header_implies={expected_bytes}")

        elif kind == "stream":
            expected_ms = round(1000.0 * expected_frames(retail) / max(1, retail.get("rate", 1)))
            reported_ms = stage.get("stream_length_ms") or 0
            if retail.get("container") == "mpeg":
                # UNIMPLEMENTED and REQUIRED: retail music is MP3 and there is no MPEG decoder.
                # What is gated is that it says so instead of playing silence.
                add(f"retail MP3 music fails to open, loudly: {label}",
                    not stage.get("open_stream_handle") and stage.get("open_stream_last_error"),
                    f"{retail.get('entry')} handle={stage.get('open_stream_handle')} "
                    f"last_error={stage.get('open_stream_last_error')!r}")
                observe("UNIMPLEMENTED (required, not cuttable): retail music is MPEG and no "
                        f"decoder is linked: {label}",
                        f"{retail.get('entry')} codec={retail.get('codec')} "
                        f"rate={retail.get('rate')} channels={retail.get('channels')}")
                continue
            add(f"retail stream decodes, is the right length and is audible: {label}",
                stage.get("open_stream_handle")
                and abs(reported_ms - expected_ms) <= max(60, expected_ms // 20)
                and stage.get("stream_playback_rate") == retail.get("rate")
                and audible,
                f"{retail.get('entry')} codec={retail.get('codec')} "
                f"block={retail.get('block_align')} "
                f"rate={stage.get('stream_playback_rate')}/{retail.get('rate')} "
                f"length_ms={reported_ms} expected_ms={expected_ms} "
                f"high_water_ms={stage.get('stream_high_water_position_ms')} rms={rms}")

    for key, stage in sorted(stages.items()):
        if not key.startswith("sample3d:") or not key.endswith("-right"):
            continue
        label = key[len("sample3d:"):-len("-right")]
        left = stages.get(f"sample3d:{label}-left", {}).get("capture", {})
        right = stage.get("capture", {})
        if len(left.get("rms") or []) != 2 or len(right.get("rms") or []) != 2:
            continue
        add(f"retail 3D pan is not mirrored: +x is heard on the right: {label}",
            right["rms"][1] > right["rms"][0] * 1.5 and left["rms"][0] > left["rms"][1] * 1.5,
            f"x=-20 rms={left['rms']} x=+20 rms={right['rms']}")

    return findings


def print_report(report):
    print("Retail audio decode probe")
    print("=" * 72)
    print(f"assets selected : {len(report['selection'])} (one per codec/layout class)")
    for label, asset in report["selection"].items():
        print(f"  {label}: {asset['entry']} ({asset['entry_bytes']} bytes)")
    print()
    gates = [f for f in report["findings"] if f["kind"] == "gate"]
    failed = 0
    print("Retail assertions")
    for finding in gates:
        failed += 0 if finding["ok"] else 1
        print(f"  [{'ok  ' if finding['ok'] else 'FAIL'}] {finding['finding']}")
        print(f"          {finding['detail']}")
    print(f"\n  {len(gates) - failed} ok, {failed} failed\n")
    print("Observations")
    for finding in report["findings"]:
        if finding["kind"] == "observation":
            print(f"  * {finding['finding']}")
            print(f"    {finding['detail']}")


if __name__ == "__main__":
    sys.exit(main())
