#!/usr/bin/env python3
"""Survey the retail audio the game ships, and which code path each asset would take.

The audio probe (docs/porting/audio-path-probe.md) could only *infer* how much retail audio its
three defects broke, because the archives available to it held no audio at all. This script
measures it instead: it reads the retail `.big` archives, parses the codec of every audio entry,
resolves the engine's own AudioEvent / DialogEvent / MusicTrack definitions onto those entries, and
reports how many assets reach the sample path, the stream path and the music path in each codec.

No retail bytes leave the archives: the output is counts and header fields. The archives are not
redistributable and must never be committed, so this script is not a CI gate -- it is how a number
quoted in docs/porting/ about retail audio gets measured, and it needs the full game-data object
(s3://cc-mac-game-data/zerohour104_gamedata_full.7z) unpacked somewhere local.

Usage:
    python3 scripts/audio-retail-survey.py --data ~/gamedata/full/GeneralsMD [--json out.json]

`--data` may be given more than once; the first archive that has a path wins, as in the engine.
"""

import argparse
import collections
import json
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import audio_retail_assets as ara  # noqa: E402  (path shim must run first)

INI_ARCHIVE_GLOB = "Data\\INI"


def load_settings_and_events(data, verbose=False):
    """AudioSettings plus every audio event definition, from the INI archives."""
    settings = {}
    events = []
    ini_files = 0
    for archive in data.archives:
        for entry in archive.order:
            if not entry.lower().endswith(".ini"):
                continue
            if INI_ARCHIVE_GLOB.lower() not in ara.normalise(entry):
                continue
            text = archive.read(entry).decode("latin-1")
            ini_files += 1
            if entry.lower().endswith("audiosettings.ini") and not settings:
                settings = ara.parse_audio_settings(text)
            events.extend(ara.parse_audio_events(text))
    if verbose:
        print(f"parsed {ini_files} INI files, {len(events)} audio definitions", file=sys.stderr)
    return settings, events


def survey_archives(data, settings):
    """Codec facts for every audio entry in every archive."""
    per_archive = collections.Counter()
    classes = collections.Counter()
    by_path = collections.defaultdict(collections.Counter)
    unparsed = []
    entries = {}
    for archive in data.audio_archives():
        for entry in archive.audio_entries():
            facts = ara.describe(entry, archive.read(entry, 8192), complete=False)
            per_archive[(archive.path.name, pathlib.PurePosixPath(entry.lower()).suffix)] += 1
            if facts.get("codec") == "unparsed":
                unparsed.append(f"{archive.path.name}:{entry}")
                continue
            path = "stream" if ara.is_streamed(entry, settings) else (
                "music" if ara.is_music(entry, settings) else "sample")
            classes[facts["class"]] += 1
            by_path[path][facts["codec"]] += 1
            entries[ara.normalise(entry)] = dict(facts, archive=archive.path.name,
                                                 engine_path=path,
                                                 entry_bytes=archive.size_of(entry))
    return {
        "per_archive": {f"{name}{extension}": count
                        for (name, extension), count in sorted(per_archive.items())},
        "classes": dict(classes.most_common()),
        "by_engine_path": {path: dict(counter.most_common())
                           for path, counter in sorted(by_path.items())},
        "unparsed": unparsed,
        "total_audio_entries": sum(per_archive.values()),
    }, entries


def defect_impact(entries):
    """How much of the retail set each of the probe's three defects actually broke.

    The probe could only infer this ("hits all 2568 DialogEvents"). These are counts over the real
    archives instead, one bucket per defect:

      * `adpcm_handoff`   -- one-shot assets that went through AudioFileCache::openFile()'s
        AIL_decompress_ADPCM handoff, i.e. every IMA ADPCM asset on the sample path;
      * `stream_window`   -- streamed assets whose payload does not fit in the old fixed 1024-byte
        read, which is what turned them into zero-length silent streams, split out from the
        separate question of whether 1024 bytes was ever too small for the *metadata*;
      * `mirrored_pan`    -- assets reachable on a 3D voice; the sign error was in the backend's
        listener/source conversion, so it was codec-independent and hit every one of them.
    """
    streams = [facts for facts in entries.values() if facts["engine_path"] == "stream"]
    samples = [facts for facts in entries.values() if facts["engine_path"] == "sample"]
    payload_end = [facts["header_bytes"] + facts["data_len"] for facts in streams]
    return {
        "adpcm_handoff_broken": sum(1 for f in samples if f["codec"] == "ima_adpcm"),
        "sample_path_total": len(samples),
        "stream_path_total": len(streams),
        "stream_payload_beyond_1024": sum(1 for end in payload_end if end > 1024),
        "stream_payload_within_1024": sum(1 for end in payload_end if end <= 1024),
        "stream_metadata_beyond_1024": sum(1 for f in streams if f["header_bytes"] > 1024),
        "stream_metadata_bytes_max": max((f["header_bytes"] for f in streams), default=0),
        "mirrored_pan_reachable": len(samples),
    }


def resolve_events(events, settings, entries):
    """Every event definition mapped onto the archive entries it can actually play."""
    kinds = collections.Counter()
    resolved = collections.Counter()
    missing = collections.Counter()
    codec_per_kind = collections.defaultdict(collections.Counter)
    files_per_kind = collections.defaultdict(set)
    for event in events:
        kinds[event["kind"]] += 1
        for _name, candidates in ara.event_paths(event, settings):
            found = None
            for candidate in candidates:
                found = entries.get(ara.normalise(candidate))
                if found is not None:
                    files_per_kind[event["kind"]].add(ara.normalise(candidate))
                    break
            if found is None:
                missing[event["kind"]] += 1
                continue
            resolved[event["kind"]] += 1
            codec_per_kind[event["kind"]][found["codec"]] += 1
    return {
        "definitions": dict(kinds.most_common()),
        "resolved_references": dict(resolved.most_common()),
        "unresolved_references": dict(missing.most_common()),
        "distinct_files": {kind: len(files) for kind, files in sorted(files_per_kind.items())},
        "codec_per_definition_kind": {kind: dict(counter.most_common())
                                      for kind, counter in sorted(codec_per_kind.items())},
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--data", action="append", required=True,
                        help="a directory of .big archives; repeatable")
    parser.add_argument("--json", help="write the survey to this path")
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

    settings, events = load_settings_and_events(data, args.verbose)
    if not settings:
        print("error: no AudioSettings block found; the INI archive is missing", file=sys.stderr)
        return 1

    archives, entries = survey_archives(data, settings)
    report = {
        "roots": args.data,
        "archives_read": [archive.path.name for archive in data.archives],
        "audio_settings": {key: settings.get(key) for key in
                           ("AudioRoot", "SoundsFolder", "MusicFolder", "StreamingFolder",
                            "SoundsExtension", "OutputRate", "OutputBits", "OutputChannels")},
        "audio": archives,
        "events": resolve_events(events, settings, entries),
        "defect_impact": defect_impact(entries),
        "probe_selection": ara.select_probe_assets(data, settings),
    }

    print("Retail audio survey")
    print("=" * 72)
    print(f"archives read      : {len(data.archives)}")
    print(f"audio entries      : {archives['total_audio_entries']}")
    print(f"unparsed entries   : {len(archives['unparsed'])}")
    print(f"folders            : sounds={settings.get('SoundsFolder')} "
          f"streaming={settings.get('StreamingFolder')} music={settings.get('MusicFolder')}")
    print("\nAudio entries per archive")
    for name, count in report["audio"]["per_archive"].items():
        print(f"  {count:6d}  {name}")
    print("\nCodec and layout classes (codec_channels_rate_bits_block)")
    for name, count in report["audio"]["classes"].items():
        print(f"  {count:6d}  {name}")
    print("\nWhich engine path plays them, by codec")
    for path, counter in report["audio"]["by_engine_path"].items():
        print(f"  {path}:")
        for codec, count in counter.items():
            print(f"    {count:6d}  {codec}")
    print("\nWhat each probe defect actually broke, measured over the archives")
    impact = report["defect_impact"]
    print(f"  ADPCM handoff  : {impact['adpcm_handoff_broken']} of "
          f"{impact['sample_path_total']} one-shot assets were IMA ADPCM")
    print(f"  stream window  : {impact['stream_payload_beyond_1024']} of "
          f"{impact['stream_path_total']} streams have payload past byte 1024; "
          f"{impact['stream_payload_within_1024']} would have fitted")
    print(f"  stream metadata: {impact['stream_metadata_beyond_1024']} streams keep metadata past "
          f"byte 1024 (largest header {impact['stream_metadata_bytes_max']} bytes)")
    print(f"  mirrored pan   : {impact['mirrored_pan_reachable']} assets can play on a 3D voice "
          "(codec-independent, the sign error was at the backend seam)")
    print("\nINI definitions and what they resolve to")
    events_report = report["events"]
    for kind, count in events_report["definitions"].items():
        files = events_report["distinct_files"].get(kind, 0)
        unresolved = events_report["unresolved_references"].get(kind, 0)
        print(f"  {count:6d}  {kind}: {files} distinct files, "
              f"{unresolved} unresolved references")
        per_codec = events_report["codec_per_definition_kind"].get(kind, {})
        for codec, codec_count in per_codec.items():
            print(f"          {codec_count:6d}  {codec}")
    print("\nAssets the probe would decode (one per class)")
    for label, asset in report["probe_selection"].items():
        print(f"  {label}: {asset['entry']} ({asset['entry_bytes']} bytes, "
              f"{asset['archive'].rsplit('/', 1)[-1]})")

    if args.json:
        pathlib.Path(args.json).write_text(json.dumps(report, indent=2) + "\n")
        print(f"\nwrote {args.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
