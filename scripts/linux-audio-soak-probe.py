#!/usr/bin/env python3
"""Count sound-effect completions and loop restarts in a live `zh`, and the thread they arrive on.

Run under gdb, attached to a running Linux native build:

    gdb -q -batch -p <pid> -x scripts/linux-audio-soak-probe.py

    SOAK_SECONDS=1200 SOAK_INTERVAL=60 SOAK_LOG=/tmp/soak.log gdb -q -batch -p <pid> \\
        -x scripts/linux-audio-soak-probe.py

Companion to `scripts/linux-audio-chain-probe.py`, which counts the whole chain (and stops the
inferior thousands of times a minute doing it). This one puts a count-and-resume breakpoint on
the three end-of-sample callbacks the engine registers with the shim, the engine handler behind
them, and the loop-restart path the M1 Pro crash went through
(`docs/porting/playability-probe.md` §1.3):

    setSampleCompleted / set3DSampleCompleted / setStreamCompleted   (MilesAudioManager.cpp)
    MilesAudioManager::notifyOfAudioCompletion
    MilesAudioManager::startNextLoop

and records, for every hit, whether it happened on the thread that runs
`MilesAudioManager::update` (the engine's main thread) or on another one, and for the callbacks
whether the caller is `OpenALAudio::deliverCompletions` (the shim's queued drain). Before the
queued delivery fix every callback hit was on the shim's service thread; after it the off-main
count must be zero, and that is what the SOAK summary line asserts. Nothing here changes engine
state, and a hit costs one stop of the inferior, so the game's rate is barely affected:
completions arrive a few times a second, not thousands of times a minute.

Output goes to stderr and to $SOAK_LOG when set. Progress lines are emitted from the breakpoint
hits themselves (gdb has no timer while the inferior runs), and the summary is the line starting
`SOAK`. `docs/porting/sound-effects-chain.md` §6/§7 read it.
"""

import os
import sys
import time

import gdb  # type: ignore  # only exists inside gdb's embedded interpreter

SECONDS = int(os.environ.get("SOAK_SECONDS", "1200"))
INTERVAL = int(os.environ.get("SOAK_INTERVAL", "60"))
LOG = os.environ.get("SOAK_LOG")

UPDATE = "MilesAudioManager::update"
DRAIN = "OpenALAudio::deliverCompletions"
CALLBACKS = [
    ("cb_2d", "setSampleCompleted"),
    ("cb_3d", "set3DSampleCompleted"),
    ("cb_stream", "setStreamCompleted"),
]
HANDLERS = [
    ("completed", "MilesAudioManager::notifyOfAudioCompletion"),
    ("loop_restarted", "MilesAudioManager::startNextLoop"),
]


def emit(line):
    sys.stderr.write(line + "\n")
    sys.stderr.flush()
    if LOG:
        with open(LOG, "a") as handle:
            handle.write(line + "\n")


class State:
    main_thread = None
    update_hits = 0
    started = time.monotonic()
    next_report = started + INTERVAL
    counters = []

    @classmethod
    def elapsed(cls):
        return time.monotonic() - cls.started

    @classmethod
    def report(cls, force=False):
        now = time.monotonic()
        if not force and now < cls.next_report:
            return
        while cls.next_report <= now:
            cls.next_report += INTERVAL
        emit("t=%4.0fs " % (now - cls.started) + " ".join(
            "%s=%d(off_main=%d)" % (c.link, c.count, c.off_main) for c in cls.counters))

    @classmethod
    def deadline_passed(cls):
        return cls.elapsed() >= SECONDS


def thread_id():
    thread = gdb.selected_thread()
    return None if thread is None else thread.ptid[1]


def caller_name():
    frame = gdb.newest_frame()
    older = frame.older() if frame is not None else None
    return older.name() if older is not None else None


class UpdateMarker(gdb.Breakpoint):
    """Learns which thread the engine pumps audio from, then gets out of the way."""

    def __init__(self):
        super().__init__(UPDATE, internal=True)

    def stop(self):
        State.update_hits += 1
        if State.main_thread is None:
            State.main_thread = thread_id()
            emit("main thread (first %s): LWP %s" % (UPDATE, State.main_thread))
        if State.update_hits >= 3:
            self.enabled = False
        return False


class Counter(gdb.Breakpoint):
    """Increments and resumes; stops the inferior only once the soak deadline has passed."""

    def __init__(self, link, function, check_caller=False):
        super().__init__(function, internal=True)
        self.link = link
        self.check_caller = check_caller
        self.on_main = 0
        self.off_main = 0
        self.before_main_known = 0
        self.from_drain = 0
        self.from_elsewhere = 0

    def stop(self):
        tid = thread_id()
        if State.main_thread is None:
            self.before_main_known += 1
        elif tid == State.main_thread:
            self.on_main += 1
        else:
            self.off_main += 1
            emit("!!! %s hit on LWP %s, not the main thread LWP %s"
                 % (self.link, tid, State.main_thread))
        if self.check_caller:
            caller = caller_name()
            if caller is not None and caller.startswith(DRAIN):
                self.from_drain += 1
            else:
                self.from_elsewhere += 1
                emit("!!! %s called from %s, not %s" % (self.link, caller, DRAIN))
        State.report()
        return State.deadline_passed()

    @property
    def count(self):
        return self.on_main + self.off_main + self.before_main_known


def main():
    gdb.execute("set pagination off")
    gdb.execute("set confirm off")
    gdb.execute("set breakpoint pending on")

    marker = UpdateMarker()
    counters = [Counter(link, function, check_caller=True) for link, function in CALLBACKS]
    counters += [Counter(link, function) for link, function in HANDLERS]
    State.counters = counters
    for counter in counters:
        if counter.pending:
            emit("!!! %s: no such function in the inferior" % counter.location)

    emit("soak: %d s, reporting every %d s" % (SECONDS, INTERVAL))

    exited = False
    while not State.deadline_passed():
        try:
            gdb.execute("continue", to_string=True)
        except gdb.error as error:
            emit("!!! inferior stopped: %s" % error)
            exited = True
            break
        if gdb.selected_inferior().pid == 0:
            emit("!!! inferior exited")
            exited = True
            break
        stopped = gdb.selected_thread()
        if stopped is None or not stopped.is_valid():
            exited = True
            break

    State.report(force=True)
    counts = {c.link: c for c in counters}
    off_main = sum(c.off_main for c in counters)
    callbacks = sum(c.count for c in counters if c.check_caller)
    from_drain = sum(c.from_drain for c in counters if c.check_caller)
    from_elsewhere = sum(c.from_elsewhere for c in counters if c.check_caller)
    completed = counts["completed"].count
    restarted = counts["loop_restarted"].count
    ok = (not exited and completed > 0 and restarted > 0 and off_main == 0
          and from_elsewhere == 0)
    emit("SOAK %s elapsed=%.0fs callbacks=%d from_drain=%d from_elsewhere=%d completed=%d "
         "loop_restarted=%d off_main_thread=%d update_hits_seen=%d main_thread=%s exited=%s"
         % ("pass" if ok else "FAIL", State.elapsed(), callbacks, from_drain, from_elsewhere,
            completed, restarted, off_main, State.update_hits, State.main_thread, exited))
    marker.delete()
    for counter in counters:
        counter.delete()
    if not exited:
        gdb.execute("detach")


main()
