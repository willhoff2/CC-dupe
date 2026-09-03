#!/usr/bin/env python3
"""Count sound-effect events at every link of the audio chain in a running native Zero Hour.

Run under gdb, attached to a live `zh` process on Linux:

    AUDIO_PROBE_SECONDS=120 AUDIO_PROBE_INTERVAL=10 AUDIO_PROBE_JSON=/tmp/audio-chain.json \\
        gdb -q -batch -p <pid> -x scripts/linux-audio-chain-probe.py

The chain from a gameplay event to an OpenAL source is

    AudioManager::addAudioEvent           the event is raised
    SoundManager::canPlayNow              it survived isOn / locality / muting
    AudioManager::appendAudioRequest      it was queued (canPlayNow said yes)
    MilesAudioManager::playAudioEvent     the request was processed
    MilesAudioManager::playSample/3D      a voice handle was taken from the pool
    AIL_set_sample_file / AIL_set_3D_sample_file   the .wav image was handed to the backend
    AIL_start_sample / AIL_start_3D_sample / AIL_start_stream   alSourcePlay

Each link gets a breakpoint that only increments a counter and resumes. Every hit still stops all
threads, and busy maps raise thousands of events a minute, so the logic rate drops to a few frames a
second while attached; the audio mixer runs in real time regardless, so short sounds can finish
between two samples. Once per --interval the probe stops in `MilesAudioManager::update`, reads the
engine's own pool and provider state, asks OpenAL for `AL_SOURCE_STATE` / `AL_SAMPLE_OFFSET` on
every voice (alGetSourcei called in the inferior -- device state, not engine bookkeeping), and
prints one line.
Nothing here alters engine state.

`docs/porting/sound-effects-chain.md` reads the output. Linux gdb only; the Mac equivalent is
`scripts/macos-playability-probe.py audio`.
"""

import json
import os
import time

import gdb  # type: ignore  # only exists inside gdb's embedded interpreter

AL_SOURCE_STATE = 0x1010
AL_PLAYING = 0x1012
AL_SAMPLE_OFFSET = 0x1025

PROVIDER_ERROR = 0xFFFFFFFF

# Link name -> function the counter is attached to. Names are what `info functions` prints.
LINKS = [
    ("raised", "AudioManager::addAudioEvent"),
    ("sound_manager", "SoundManager::canPlayNow"),
    ("queued", "AudioManager::appendAudioRequest"),
    ("processed", "MilesAudioManager::playAudioEvent"),
    ("handle_2d", "MilesAudioManager::playSample"),
    ("handle_3d", "MilesAudioManager::playSample3D"),
    ("handle_stream", "MilesAudioManager::playStream"),
    ("image_2d", "AIL_set_sample_file"),
    ("image_3d", "AIL_set_3D_sample_file"),
    ("started_2d", "AIL_start_sample"),
    ("started_3d", "AIL_start_3D_sample"),
    ("started_stream", "AIL_start_stream"),
]

# Links whose first argument (after `this`) is an AudioEventRTS*, so the event name can be kept.
EVENT_ARGUMENT = {
    "raised": "eventToAdd",
    "sound_manager": "event",
    "handle_2d": "event",
    "handle_3d": "event",
    "handle_stream": "event",
}

# Links that receive the backend handle whose OpenAL source we can read; paired with the most
# recent event name from the matching playSample link so a live AL source can be named.
SOURCE_ARGUMENT = {
    "started_2d": ("sample", "OpenALAudio::SampleVoice", ["source"], "handle_2d"),
    "started_3d": ("sample", "OpenALAudio::Object3D", ["voice", "source"], "handle_3d"),
}

RECENT_NAMES = 12


def ascii_string(value):
    """Read an AsciiString without calling into the inferior (stop handlers cannot)."""
    try:
        data = value["m_data"]
        if int(data) == 0:
            return ""
        chars = (data + 1).cast(gdb.lookup_type("char").pointer())
        return chars.string(errors="replace")
    except gdb.error as error:
        return "<unreadable: %s>" % error


class Counter(gdb.Breakpoint):
    """Increments and resumes; never stops the inferior."""

    def __init__(self, link, function, names, source_names, peers):
        super().__init__(function, internal=True)
        self.link = link
        self.count = 0
        self.names = names
        self.source_names = source_names
        self.peers = peers
        self.recent = []

    def stop(self):
        self.count += 1
        argument = EVENT_ARGUMENT.get(self.link)
        if argument is not None:
            try:
                event = gdb.parse_and_eval(argument)
                if int(event) != 0:
                    name = ascii_string(event["m_eventName"])
                    self.recent.append(name)
                    del self.recent[:-RECENT_NAMES]
                    self.names[name] = self.names.get(name, 0) + 1
            except gdb.error:
                pass
        source_argument = SOURCE_ARGUMENT.get(self.link)
        if source_argument is not None:
            argument, type_name, path, peer = source_argument
            try:
                handle = gdb.parse_and_eval(argument).cast(gdb.lookup_type(type_name).pointer())
                value = handle
                for field in path:
                    value = value[field]
                recent = self.peers[peer].recent
                self.source_names[int(value)] = recent[-1] if recent else "?"
            except gdb.error:
                pass
        return False


class Sampler(gdb.Breakpoint):
    """Stops in MilesAudioManager::update when the sampling interval has elapsed."""

    def __init__(self, interval):
        super().__init__("MilesAudioManager::update", internal=True)
        self.interval = interval
        self.last = 0.0
        self.updates = 0

    def stop(self):
        self.updates += 1
        now = time.monotonic()
        if now - self.last >= self.interval:
            self.last = now
            return True
        return False


def eval_int(expression):
    try:
        return int(gdb.parse_and_eval(expression))
    except gdb.error as error:
        return "<%s>" % error


def vector_size(expression):
    """std::vector size without instantiating anything: (finish - start) / sizeof(element)."""
    try:
        vector = gdb.parse_and_eval(expression)
        start = vector["_M_impl"]["_M_start"]
        finish = vector["_M_impl"]["_M_finish"]
        return int(finish - start)
    except gdb.error as error:
        return "<%s>" % error


def list_size(expression):
    try:
        node = gdb.parse_and_eval(expression)["_M_impl"]["_M_node"]
        count = 0
        cursor = node["_M_next"]
        head = node.address
        while int(cursor) != int(head) and count < 100000:
            count += 1
            cursor = cursor["_M_next"]
        return count
    except gdb.error as error:
        return "<%s>" % error


_SCRATCH = []


def al_get_source_i(source, parameter):
    """alGetSourcei has no DWARF: call it through an explicit signature.

    gdb's expression parser has no statement expressions, so the out-parameter lives in a
    4-byte inferior allocation made once per session.
    """
    if not _SCRATCH:
        _SCRATCH.append(int(gdb.parse_and_eval("(long)((void*(*)(unsigned long))malloc)(4)")))
    scratch = _SCRATCH[0]
    gdb.parse_and_eval(
        "((void(*)(unsigned int, int, int*))alGetSourcei)(%d, %d, (int*)%d)"
        % (source, parameter, scratch))
    return int(gdb.parse_and_eval("*(int*)%d" % scratch))


def list_entries(expression):
    """Yield the PlayingAudio* entries of a std::list<PlayingAudio*>."""
    node = gdb.parse_and_eval(expression)["_M_impl"]["_M_node"]
    head = node.address
    cursor = node["_M_next"]
    element = gdb.lookup_type("PlayingAudio").pointer()
    count = 0
    while int(cursor) != int(head) and count < 100000:
        count += 1
        # _List_node<T>: the value sits right after the two _List_node_base pointers.
        value = (cursor.cast(gdb.lookup_type("char").pointer()) + 16).cast(element.pointer())
        yield value.dereference()
        cursor = cursor["_M_next"]


def playing_voices(manager):
    """Engine bookkeeping joined to device state: for every entry the engine believes is playing,
    (event name, AL source, AL_PLAYING?, AL_SAMPLE_OFFSET). Stale entries -- the engine still
    lists a voice whose source has stopped -- show up here as AL_PLAYING false."""
    rows = []
    try:
        for playing in list_entries(manager + "->m_playingSounds"):
            name = ascii_string(playing["m_audioEventRTS"]["m_eventName"])
            sample = playing["m_sample"]
            source = int(sample.cast(gdb.lookup_type("OpenALAudio::SampleVoice").pointer())
                         ["source"]) if int(sample) else 0
            rows.append(_voice_row("2d", name, source))
        for playing in list_entries(manager + "->m_playing3DSounds"):
            name = ascii_string(playing["m_audioEventRTS"]["m_eventName"])
            sample = playing["m_3DSample"]
            source = int(sample.cast(gdb.lookup_type("OpenALAudio::Object3D").pointer())
                         ["voice"]["source"]) if int(sample) else 0
            rows.append(_voice_row("3d", name, source))
    except gdb.error as error:
        rows.append({"kind": "error", "name": str(error), "source": 0,
                     "al_playing": False, "offset": 0})
    return rows


def _voice_row(kind, name, source):
    if source == 0:
        return {"kind": kind, "name": name, "source": 0, "al_playing": False, "offset": 0}
    return {"kind": kind, "name": name, "source": source,
            "al_playing": al_get_source_i(source, AL_SOURCE_STATE) == AL_PLAYING,
            "offset": al_get_source_i(source, AL_SAMPLE_OFFSET)}


def voice_states(vector, source_path):
    """Per-voice (AL_SOURCE_STATE == AL_PLAYING, AL_SAMPLE_OFFSET, source) for every entry in a
    lib() vector."""
    states = []
    try:
        vec = gdb.parse_and_eval(vector)
        start = vec["_M_impl"]["_M_start"]
        count = int(vec["_M_impl"]["_M_finish"] - start)
        for i in range(count):
            entry = (start + i).dereference()
            for field in source_path:
                entry = entry[field]
            source = int(entry)
            if source == 0:
                continue
            states.append((al_get_source_i(source, AL_SOURCE_STATE) == AL_PLAYING,
                           al_get_source_i(source, AL_SAMPLE_OFFSET), source))
    except gdb.error as error:
        return [("<%s>" % error, 0, 0)]
    return states


def engine_state():
    manager = "((MilesAudioManager*)TheAudio)"
    return {
        "logic_frame": eval_int("TheGameLogic->m_frame"),
        "in_game": eval_int("TheGameLogic->m_gameMode != 0"),
        "sound_on": eval_int("TheAudio->m_soundOn"),
        "sound3d_on": eval_int("TheAudio->m_sound3DOn"),
        "speech_on": eval_int("TheAudio->m_speechOn"),
        "provider_count": eval_int(manager + "->m_providerCount"),
        "selected_provider": eval_int(manager + "->m_selectedProvider"),
        "pref_3d_provider": ascii_string(gdb.parse_and_eval(manager + "->m_pref3DProvider")),
        "num_2d_samples": eval_int(manager + "->m_num2DSamples"),
        "num_3d_samples": eval_int(manager + "->m_num3DSamples"),
        "available_2d": vector_size(manager + "->m_availableSamples"),
        "available_3d": vector_size(manager + "->m_available3DSamples"),
        "playing_sounds": list_size(manager + "->m_playingSounds"),
        "playing_3d_sounds": list_size(manager + "->m_playing3DSounds"),
        "playing_streams": list_size(manager + "->m_playingStreams"),
        "listener": eval_int(manager + "->m_listener"),
        "openal_started": eval_int("OpenALAudio::lib().started"),
        "openal_samples": vector_size("OpenALAudio::lib().samples"),
        "openal_objects": vector_size("OpenALAudio::lib().objects"),
        "openal_streams": vector_size("OpenALAudio::lib().streams"),
    }


def main():
    seconds = float(os.environ.get("AUDIO_PROBE_SECONDS", "60"))
    interval = float(os.environ.get("AUDIO_PROBE_INTERVAL", "10"))
    json_path = os.environ.get("AUDIO_PROBE_JSON")

    gdb.execute("set pagination off")
    gdb.execute("set print thread-events off")
    # The service thread polls OpenAL at 10 ms; stopping all threads for every counter hit is what
    # keeps the counters consistent, so leave all-stop mode alone.

    names = {}
    source_names = {}
    peers = {}
    counters = [Counter(link, function, names, source_names, peers) for link, function in LINKS]
    peers.update({c.link: c for c in counters})
    sampler = Sampler(interval)

    samples = []
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        try:
            gdb.execute("continue", to_string=True)
        except gdb.error as error:
            print("probe: inferior stopped: %s" % error)
            break
        if not any(t.is_valid() for t in gdb.selected_inferior().threads()):
            print("probe: inferior exited")
            break
        if gdb.selected_thread() is None:
            break
        state = engine_state()
        state["wall"] = round(time.monotonic(), 1)
        state["updates"] = sampler.updates
        state["counts"] = {c.link: c.count for c in counters}
        sample_states = voice_states("OpenALAudio::lib().samples", ["source"])
        object_states = voice_states("OpenALAudio::lib().objects", ["voice", "source"])
        stream_states = voice_states("OpenALAudio::lib().streams", ["source"])
        state["al_playing"] = {
            "samples": sum(1 for playing, _, _ in sample_states if playing is True),
            "objects": sum(1 for playing, _, _ in object_states if playing is True),
            "streams": sum(1 for playing, _, _ in stream_states if playing is True),
        }
        state["sample_offsets"] = {
            "samples": [offset for _, offset, _ in sample_states],
            "objects": [offset for _, offset, _ in object_states],
        }
        # Device-side view: which AL sources are AL_PLAYING right now, named through the
        # event that last started them (recorded at AIL_start_sample / AIL_start_3D_sample).
        state["al_live"] = [
            {"source": source, "offset": offset, "name": source_names.get(source, "?")}
            for playing, offset, source in sample_states + object_states if playing is True]
        state["stream_frames_played"] = eval_int(
            "(OpenALAudio::lib().streams._M_impl._M_finish != "
            "OpenALAudio::lib().streams._M_impl._M_start) ? "
            "(*OpenALAudio::lib().streams._M_impl._M_start)->framesPlayed : 0")
        state["voices"] = playing_voices("((MilesAudioManager*)TheAudio)")
        state["recent"] = {c.link: list(c.recent) for c in counters if c.recent}
        samples.append(state)
        live = ["%s:src%d@%d" % (v["name"], v["source"], v["offset"]) for v in state["al_live"]]
        print("probe frame=%s inGame=%s provider=%s/%s pools 2D=%s 3D=%s | counts %s | "
              "AL_PLAYING %s | engine-playing 2D=%s 3D=%s | live %s" % (
                  state["logic_frame"], state["in_game"], state["selected_provider"],
                  state["provider_count"], state["num_2d_samples"], state["num_3d_samples"],
                  json.dumps(state["counts"]), json.dumps(state["al_playing"]),
                  state["playing_sounds"], state["playing_3d_sounds"], live))

    top = sorted(names.items(), key=lambda item: -item[1])[:40]
    report = {"samples": samples, "event_names": top,
              "counts": {c.link: c.count for c in counters}}
    print(json.dumps({"counts": report["counts"], "event_names": top}, indent=1))
    if json_path:
        with open(json_path, "w") as handle:
            json.dump(report, handle, indent=1)
    gdb.execute("detach")


main()
