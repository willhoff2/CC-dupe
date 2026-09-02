#!/usr/bin/env python3
"""Sample a running native Zero Hour process while a skirmish plays.

`macos-input-drive.py` drives the shell and reads one engine snapshot per invocation, which is
enough to prove a menu path but not to answer "is it a game": survival, frame pacing, whether the
AI builds and fights, and whether audio is actually being pulled all need the same process watched
over minutes. This attaches once and keeps the attachment for the whole run, resuming the process
between samples, so the game keeps playing while it is measured.

Every number here comes from engine state or from the OS, never from the screen -- the mission
viewport is known to be wrong (docs/porting/draws-per-frame.md 5.1) and pixels cannot be trusted.

  run       attach, sample every --interval seconds for --minutes, write JSON
  snapshot  one sample, printed as JSON
  audio     count OpenAL/CoreAudio HAL callbacks over a window, with source states
"""

import argparse
import json
import os
import struct
import subprocess
import sys
import threading
import time

XCODE_PYTHON = "/Applications/Xcode.app/Contents/Developer/usr/bin/python3"

# The list walk is JIT-compiled into the target, so it costs one expression per sample rather than
# one per object. `getControllingPlayer()` is out of line, so there is something to call.
# `alGetSourcei` has no debug info (the OpenAL headers are not in the DWARF), so it is called
# through an explicit signature. 0x1010 is AL_SOURCE_STATE and 0x1012 is AL_PLAYING.
# `std::vector::operator[]` was never instantiated for these element types, so there is no function
# in the target to call: the element pointer is read straight out of the vector's first word.


def vector_data(name, element_type):
    return "(*(%s***)&OpenALAudio::lib().%s)" % (element_type, name)


SOURCE_STATE_COUNT = """
int probePlaying = 0;
for (unsigned i = 0; i < OpenALAudio::lib().{name}.size(); i++) {{
    int probeState = 0;
    ((void(*)(unsigned int, int, int*))alGetSourcei)({data}[i]->{source}, 0x1010, &probeState);
    if (probeState == 0x1012) probePlaying++;
}}
probePlaying;
"""

# `framesPlayed` only advances when OpenAL reports a buffer processed, i.e. when the CoreAudio HAL
# has consumed it. It is the difference between "the engine started music" and "music is audible".
STREAM_FRAMES = """
int probeFrames = 0;
for (unsigned i = 0; i < OpenALAudio::lib().streams.size(); i++)
    probeFrames += (int)%s[i]->%s;
probeFrames;
"""

OBJECTS_BY_PLAYER = """
int probeCounts[16]; for (int i = 0; i < 16; i++) probeCounts[i] = 0;
int probeTotal = 0;
for (Object* o = ((W3DGameLogic*)TheGameLogic)->m_objList; o != 0; o = o->getNextObject()) {
    probeTotal++;
    Player* p = o->getControllingPlayer();
    if (p != 0) {
        int index = (int)p->m_playerIndex;
        if (index >= 0 && index < 16) probeCounts[index]++;
    }
}
probeCounts[%d] * 65536 + probeTotal;
"""


def require_lldb():
    """Re-exec under Xcode's interpreter, the only one carrying the `lldb` module."""
    try:
        import lldb  # noqa: F401
        return
    except ImportError:
        pass
    if os.environ.get("PLAYABILITY_PROBE_REEXEC"):
        sys.exit("no lldb python module, even under %s" % sys.executable)
    lldb_python_path = subprocess.run(
        ["lldb", "-P"], capture_output=True, text=True, check=True).stdout.strip()
    environment = dict(os.environ,
                       PYTHONPATH=lldb_python_path, PLAYABILITY_PROBE_REEXEC="1")
    interpreter = XCODE_PYTHON if os.path.exists(XCODE_PYTHON) else sys.executable
    os.execve(interpreter, [interpreter, os.path.abspath(__file__)] + sys.argv[1:], environment)


def rss_bytes(pid):
    """Resident set size from the OS, not from the engine's own accounting."""
    output = subprocess.run(["ps", "-o", "rss=", "-p", str(pid)],
                            capture_output=True, text=True).stdout.strip()
    return int(output) * 1024 if output else None


def is_translated(pid):
    """1 means Rosetta, which would make every timing number meaningless."""
    output = subprocess.run(["sysctl", "-n", "sysctl.proc_translated"],
                            capture_output=True, text=True).stdout.strip()
    return output


class Probe:
    """A long-lived LLDB attachment. The process runs between samples."""

    def __init__(self, pid, binary):
        import lldb
        self.lldb = lldb
        self.pid = pid
        self.debugger = lldb.SBDebugger.Create()
        self.debugger.SetAsync(False)
        self.target = self.debugger.CreateTarget(str(binary))
        error = lldb.SBError()
        self.process = self.target.AttachToProcessWithID(
            self.debugger.GetListener(), pid, error)
        if not self.process.IsValid():
            raise RuntimeError("attach to %d failed: %s" % (pid, error))
        self.refresh_frame()

    def refresh_frame(self):
        self.frame = self.process.GetSelectedThread().GetFrameAtIndex(0)

    def value(self, expression):
        result = self.frame.EvaluateExpression(expression)
        if result.GetError().Fail():
            return None
        return result

    def integer(self, expression):
        result = self.value(expression)
        return None if result is None else result.GetValueAsSigned()

    def real(self, expression):
        result = self.value(expression)
        if result is None:
            return None
        try:
            return float(result.GetValue())
        except (TypeError, ValueError):
            return None

    def text(self, expression):
        result = self.value(expression)
        if result is None:
            return None
        summary = result.GetSummary() or result.GetValue() or ""
        return summary.strip('"')

    def fps_history(self):
        """The engine's own ring of the last 30 render frames' instantaneous FPS
        (`W3DDisplay::updateAverageFPS`). Sampling this is the only way to get a frame-time
        *distribution* rather than one averaged number, because nothing else records per-frame
        cost. The ring is a function-local static, so it is found by symbol, not by name lookup."""
        symbols = self.target.FindSymbols(
            "_ZZN10W3DDisplay16updateAverageFPSEvE10fpsHistory")
        if not symbols.GetSize():
            return None
        address = symbols.GetContextAtIndex(0).GetSymbol().GetStartAddress()
        error = self.lldb.SBError()
        data = self.process.ReadMemory(address.GetLoadAddress(self.target), 4 * 30, error)
        if error.Fail():
            return None
        return [round(value, 3) for value in struct.unpack("<30f", data)]

    def resume_for(self, seconds):
        """Let the game run for `seconds` of wall clock, then stop it again."""
        timer = threading.Timer(seconds, self.process.SendAsyncInterrupt)
        timer.start()
        started = time.monotonic()
        self.process.Continue()
        timer.cancel()
        self.refresh_frame()
        return time.monotonic() - started

    def alive(self):
        return self.process.IsValid() and self.process.GetState() not in (
            self.lldb.eStateExited, self.lldb.eStateDetached, self.lldb.eStateCrashed)

    def exit_report(self):
        state = self.process.GetState()
        report = {"state": self.lldb.SBDebugger.StateAsCString(state)}
        if state == self.lldb.eStateExited:
            report["exit_status"] = self.process.GetExitStatus()
            report["exit_description"] = self.process.GetExitDescription()
        elif state in (self.lldb.eStateStopped, self.lldb.eStateCrashed):
            thread = self.process.GetSelectedThread()
            report["stop_reason"] = thread.GetStopDescription(256)
            report["backtrace"] = [str(f) for f in thread][:20]
        return report

    def players(self):
        """Per-player money, score and live object count -- the only honest way to ask whether the
        AI is playing, because the viewport cannot be read."""
        count = self.integer("(int)ThePlayerList->m_playerCount") or 0
        rows = []
        for index in range(count):
            base = "ThePlayerList->m_players[%d]" % index
            if not self.integer("(long)%s" % base):
                continue
            packed = self.integer(OBJECTS_BY_PLAYER % index)
            score = "%s->m_scoreKeeper" % base
            rows.append({
                "index": index,
                "side": self.text("(const char*)%s->m_side.str()" % base),
                "type": self.integer("(int)%s->m_playerType" % base),
                "has_ai": bool(self.integer("(long)%s->m_ai" % base)),
                "money": self.integer("(int)%s->m_money.m_money" % base),
                "objects": None if packed is None else packed >> 16,
                "units_built": self.integer("(int)%s.m_totalUnitsBuilt" % score),
                "units_lost": self.integer("(int)%s.m_totalUnitsLost" % score),
                "buildings_built": self.integer("(int)%s.m_totalBuildingsBuilt" % score),
                "buildings_lost": self.integer("(int)%s.m_totalBuildingsLost" % score),
                "money_earned": self.integer("(int)%s.m_totalMoneyEarned" % score),
                "money_spent": self.integer("(int)%s.m_totalMoneySpent" % score),
            })
            if rows and rows[-1]["objects"] is not None and packed is not None:
                rows[-1]["objects_total_all_players"] = packed & 0xFFFF
        return rows

    def audio_state(self):
        """Voice/stream counts come from the OpenAL shim's own library singleton, so a playing
        source is evidence the engine asked for a sound, not that a decoder returned success."""
        report = {
            "mute_reason_bits": self.integer("(int)TheAudio->m_muteReasonBits"),
            "music_volume": self.real("TheAudio->m_musicVolume"),
            "sound_volume": self.real("TheAudio->m_soundVolume"),
            "samples": self.integer("(int)OpenALAudio::lib().samples.size()"),
            "streams": self.integer("(int)OpenALAudio::lib().streams.size()"),
            "objects_3d": self.integer("(int)OpenALAudio::lib().objects.size()"),
            "quick_audio": self.integer("(int)OpenALAudio::lib().quickAudio.size()"),
            "al_context": self.integer("(long)OpenALAudio::lib().context"),
        }
        streams = vector_data("streams", "OpenALAudio::StreamVoice")
        report["playing_samples"] = self.integer(SOURCE_STATE_COUNT.format(
            name="samples", data=vector_data("samples", "OpenALAudio::SampleVoice"),
            source="source"))
        report["playing_streams"] = self.integer(SOURCE_STATE_COUNT.format(
            name="streams", data=streams, source="source"))
        report["playing_3d_voices"] = self.integer(SOURCE_STATE_COUNT.format(
            name="objects", data=vector_data("objects", "OpenALAudio::Object3D"),
            source="voice.source"))
        report["stream_frames_played"] = self.integer(STREAM_FRAMES % (streams, "framesPlayed"))
        report["stream_frames_queued"] = self.integer(STREAM_FRAMES % (streams, "framesQueued"))
        report["stream_names"] = [
            self.text("(const char*)%s[%d]->fileName.c_str()" % (streams, index))
            for index in range(min(report["streams"] or 0, 4))]
        return report

    def sample(self):
        """One engine+OS snapshot. Nothing here reads a pixel."""
        display = "((W3DDisplay*)TheDisplay)"
        logic = "((W3DGameLogic*)TheGameLogic)"
        ui = "((W3DInGameUI*)TheInGameUI)"
        report = {
            "wall": time.time(),
            "monotonic": time.monotonic(),
            "rss_bytes": rss_bytes(self.pid),
            "game_mode": self.integer("(int)%s->m_gameMode" % logic),
            "frame": self.integer("(int)%s->m_frame" % logic),
            "paused": self.integer("(int)%s->m_gamePaused" % logic),
            "current_fps": self.real("%s->m_currentFPS" % display),
            "average_fps": self.real("%s->m_averageFPS" % display),
            "fps_limit": self.integer(
                "(int)TheWritableGlobalData->m_framesPerSecondLimit"),
            "fps_limit_enabled": self.integer(
                "(int)TheWritableGlobalData->m_useFpsLimit"),
            "selected_count": self.integer("(int)%s->m_selectCount" % ui),
            "mouse_x": self.integer("TheMouse->m_currMouse.pos.x"),
            "mouse_y": self.integer("TheMouse->m_currMouse.pos.y"),
            "frame_selection_changed": self.integer("(int)%s->m_frameSelectionChanged" % ui),
            "quit_menu_visible": self.integer("(int)%s->m_isQuitMenuVisible" % ui),
            "window_active": self.text(
                "(bool)WWPlatform::Window_Is_Active(WWPlatform::Window_Current())"),
            "shell_screen_count": self.integer("TheShell->m_screenCount"),
            "video_stream": self.integer("(long)((VideoPlayer*)TheVideoPlayer)->m_firstStream"),
        }
        report["fps_history"] = self.fps_history()
        report["audio"] = self.audio_state()
        report["players"] = self.players()
        return report

    def count_calls(self, symbol, seconds):
        """Break on `symbol`, let the game run, and report how often it was reached. Used to prove
        the audio HAL is pulling buffers: a callback that never fires is silence no matter what the
        engine believes it is playing."""
        breakpoint = self.target.BreakpointCreateByName(symbol)
        if breakpoint.GetNumLocations() == 0:
            self.target.BreakpointDelete(breakpoint.GetID())
            return {"symbol": symbol, "error": "no location"}
        # Auto-continue: the point is the rate, not any single hit.
        breakpoint.SetAutoContinue(True)
        elapsed = self.resume_for(seconds)
        hits = breakpoint.GetHitCount()
        self.target.BreakpointDelete(breakpoint.GetID())
        return {"symbol": symbol, "seconds": round(elapsed, 3), "hits": hits,
                "per_second": round(hits / elapsed, 2) if elapsed else None}

    def detach(self):
        self.process.Detach()
        self.lldb.SBDebugger.Destroy(self.debugger)


def command_snapshot(probe, args):
    return probe.sample()


def command_audio(probe, args):
    report = {"before": probe.audio_state()}
    for symbol in args.symbol:
        report.setdefault("callbacks", []).append(probe.count_calls(symbol, args.seconds))
    report["after"] = probe.audio_state()
    return report


def command_run(probe, args):
    deadline = time.monotonic() + args.minutes * 60.0
    samples = []
    report = {"pid": probe.pid, "started": time.time(),
              "proc_translated_host": is_translated(probe.pid),
              "interval": args.interval, "samples": samples}
    resumed_for = None
    while time.monotonic() < deadline:
        stopped = time.monotonic()
        sample = probe.sample()
        sample["stop_cost"] = round(time.monotonic() - stopped, 3)
        # The game is frozen while it is being read, so logic rate is only meaningful against the
        # time it was actually running: that is this window, not the wall clock between samples.
        sample["ran_for"] = resumed_for
        samples.append(sample)
        if args.out:
            with open(args.out, "w") as handle:
                json.dump(report, handle, indent=2)
        if not probe.alive():
            break
        try:
            resumed_for = round(probe.resume_for(
                min(args.interval, max(deadline - time.monotonic(), 1.0))), 3)
        except Exception as error:  # the process died while running
            report["resume_error"] = str(error)
            break
        if not probe.alive():
            report["died"] = probe.exit_report()
            break
    report["finished"] = time.time()
    report["final_state"] = probe.exit_report()
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--binary", required=True)
    parser.add_argument("--out", help="write the JSON report here as it is collected")
    subparsers = parser.add_subparsers(dest="command", required=True)

    subparsers.add_parser("snapshot").set_defaults(handler=command_snapshot)

    run = subparsers.add_parser("run")
    run.add_argument("--minutes", type=float, default=20.0)
    run.add_argument("--interval", type=float, default=15.0)
    run.set_defaults(handler=command_run)

    audio = subparsers.add_parser("audio")
    audio.add_argument("--seconds", type=float, default=5.0)
    audio.add_argument("--symbol", action="append", default=[])
    audio.set_defaults(handler=command_audio)

    args = parser.parse_args()
    if args.command == "audio" and not args.symbol:
        args.symbol = ["alcRenderSamplesSOFT"]

    probe = Probe(args.pid, args.binary)
    try:
        report = args.handler(probe, args)
    finally:
        try:
            probe.detach()
        except Exception:
            pass
    text = json.dumps(report, indent=2)
    if args.out:
        with open(args.out, "w") as handle:
            handle.write(text)
    print(text)


if __name__ == "__main__":
    require_lldb()
    main()
