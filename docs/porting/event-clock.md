# Window event stamps and the engine clock

`Keyboard::checkKeyRepeat()` decides that a key is held by subtracting the key's down time from
`timeGetTime()`. The down time is the window event's `Time_Ms`. Both window backends stamped
`Time_Ms` on their toolkit's own clock, not on `timeGetTime()`'s, so on macOS every key repeated in
the frame it went down. The SDL2 backend (Linux) had the same defect, from a different clock.

The fix converts each event's stamp into `timeGetTime()`'s clock in the backend, keeping the
event's age. Windows is untouched: both backends are compiled only off Windows.

Status words: MEASURED (run and observed), INFERRED (read from code or documentation, not run),
UNMEASURED (open).

## 1. The two clocks on macOS: MEASURED

`time_compat.h` implements `timeGetTime()` with `clock_gettime(CLOCK_BOOTTIME)`. `CLOCK_BOOTTIME`
is not defined on Darwin, so the header's fallback makes it `CLOCK_MONOTONIC`. On Darwin that clock
keeps counting while the Mac sleeps. AppKit stamps `NSEvent.timestamp` with the system uptime,
which stops during sleep.

Read on the M1 Pro (macOS 26.6.1, 2026-10-05) with a throwaway program, in milliseconds:

| Clock | Value |
|---|---:|
| `CLOCK_MONOTONIC` (what `timeGetTime()` reads) | 4,016,740,159 |
| `mach_continuous_time()` | 4,016,778,796 |
| `CLOCK_UPTIME_RAW` | 1,081,220,569 |
| `[NSProcessInfo systemUptime]` | 1,081,220,569 |
| `mach_absolute_time()` | 1,081,220,569 |

`CLOCK_MONOTONIC` follows `mach_continuous_time` (counts sleep). `systemUptime` is
`mach_absolute_time` (does not). The gap was 2,935,519,590 ms, about 34 days of sleep since the
boot on 2026-08-20. That matches the 2,935,519,624 ms the probe agent measured earlier.

The test checks that AppKit's own stamps are on `systemUptime`'s clock. It records the timestamps
of events the window server delivers (not the ones the test posts) and compares them with
`systemUptime`. In every mode the newest such stamp was 19–196 ms old, not 34 days.

## 2. The chain, link by link

| Link | Where | Status |
|---|---|---|
| Cocoa stamps every key, text, flags, mouse and wheel event with `[event timestamp] * 1000` | `platform_window_cocoa.mm` `Translate()` (pre-fix line 879); focus, resize and close events used `Now_Ms()`, i.e. `systemUptime` | MEASURED (red run, §4) |
| `serviceOS()` copies `Time_Ms` into `theMessageTime` and queues the event | `PlatformWindowHost.cpp:309` | INFERRED (read) |
| The keyboard device copies a KEY_DOWN's `Time_Ms` into `KeyboardIO::keyDownTimeMsec` | `Win32DIKeyboard.cpp:481` (`#ifndef _WIN32` half) | INFERRED (read) |
| `Keyboard::updateKeys()` copies it into `m_keyStatus[key].keyDownTimeMsec` | `Keyboard.cpp:187` | INFERRED (read) |
| `checkKeyRepeat()` computes `timeGetTime() - keyDownTimeMsec` in unsigned arithmetic and repeats any key whose hold exceeds `KEY_REPEAT_DELAY_MSEC` (333) | `Keyboard.cpp:269-287` | INFERRED (read); the test asserts the same subtraction on the real stamp |
| So, before the fix, a repeat (`KEY_STATE_DOWN \| KEY_STATE_AUTOREPEAT`) is added in the same frame as the real key-down | — | INFERRED. The test measured a hold of 2,935,519,590 ms for a key pressed 0 ms earlier. No game was run. |

After the first synthesised repeat, `checkKeyRepeat()` sets the key's down time to
`now - (333 + 67)`. On the next frame the hold is already more than 333 ms, so a held key repeats
on every frame until it is released (INFERRED). The same happens on Windows after the real 333 ms.
This is upstream behaviour and is not changed here. What the port got wrong was only the first
delay: zero instead of 333 ms.

Who sees the repeats (INFERRED, by reading):

- `MetaEventTranslator` eats repeats of keys that have a meta mapping (`MetaEvent.cpp:618`). That
  is why Escape and the scroll arrows still behave.
- `WindowTranslator` runs earlier and passes every `MSG_RAW_KEY_DOWN`, repeats included, to
  `TheWindowManager->winProcessKey()`. Any focused GUI control sees them: a text entry's
  Backspace and arrows, and list boxes. Before the fix, those got a second press on every frame
  they were held, from the first frame on.
- Unmapped keys in game behave the same way.

None of this was observed in a running game (§6).

## 3. Every consumer of the event stamp

Every read of `WindowEvent::Time_Ms` in the tree (`grep -rn Time_Ms`):

| Consumer | Compares against | Affected by the clock gap? |
|---|---|---|
| `Win32DIKeyboard.cpp:481` → `KeyboardIO::keyDownTimeMsec` → `Keyboard::checkKeyRepeat()` | `timeGetTime()` | **yes**: the defect |
| `Win32Mouse.cpp:121` → `MouseIO::time` → the third argument of `MSG_RAW_MOUSE_{LEFT,MIDDLE,RIGHT}_{BUTTON_DOWN,BUTTON_UP,DOUBLE_CLICK}` (`Mouse.cpp:750-834`) | only another mouse stamp: `CommandXlat.cpp:3944/3954` and `SelectionXlat.cpp:1077/1086` pass a down/up pair to `Mouse::isClick()`, which compares the difference with `m_dragToleranceMS` | no: both stamps were on the same clock. This is the right-click-versus-drag decision (cancelling a GUI command or building placement, deselecting). It worked before the fix and keeps working after. |
| `PlatformWindowHost.cpp:309` → `theMessageTime` → `PlatformWindowHost::getMessageTime()` | nothing: `getMessageTime()` has no caller | no |

Mouse timing that does compare against the engine clock reads `timeGetTime()` directly and never
sees an event stamp: `LookAtXlat.cpp` (`m_lastMouseMoveTimeMsec`, the scroll and hold timers) and
`SelectionXlat.cpp:1158/1230` (group double-tap). Double clicks come from the backend's
`Click_Count`, not from timestamps. Building-placement rotation (`PlaceEventTranslator`) reads no
time at all.

## 4. The fix, and why it is at the seam

Each backend converts its toolkit stamp into `timeGetTime()`'s clock by keeping the event's age:

```cpp
// platform_window_cocoa.mm
unsigned int Engine_Time_Ms(NSEvent * event)
{
	const double age_seconds = [NSProcessInfo processInfo].systemUptime - [event timestamp];
	return timeGetTime() - static_cast<unsigned int>(std::llround(age_seconds * 1000.0));
}

// platform_window_sdl2.cpp
unsigned int Engine_Time_Ms(Uint32 sdl_timestamp)
{
	return timeGetTime() - (SDL_GetTicks() - sdl_timestamp);
}
```

The events the Cocoa backend makes itself (focus, minimise, resize, close) are stamped with
`timeGetTime()`, and `Now_Ms()` is gone. `platform_window.h` now states that `Time_Ms` must be on
`timeGetTime()`'s clock.

Why this design:

- **`timeGetTime()` is left alone.** Moving it to the uptime clock on macOS would also move the
  network timeouts, the GUI timers and every other one of its 43 files (`timing-and-threading.md`
  §2). It would also not fix SDL2, whose clock starts at `SDL_Init`.
- **The event's age is kept, rather than stamping "now" at translate time.** If the pump stamped
  "now", an event that waited in the queue during a long frame would be stamped late. The
  right-click-versus-drag decision compares the times of two events, and stamping "now" would make
  that difference depend on frame timing. Keeping the age leaves the gap between events exactly
  what the toolkit measured. The test checks a key stamped 500 ms ago (SDL2: queued 300 ms before
  the pump) arrives 500 (304) ms before `timeGetTime()`.
- **The 32-bit wrap still works.** The result is `timeGetTime()` minus an unsigned age, which is
  modulo 2^32 just like `timeGetTime()` itself (`time_compat.h`). SDL's `Uint32` tick difference
  is also wrap-safe. A negative age (impossible for a delivered event) converts modulo 2^32 too;
  it is not undefined behaviour.
- The two clock reads in `Engine_Time_Ms` are not atomic, so the result can be off by under a
  millisecond (INFERRED). The test allows 2 ms against the engine time at which it stamped the
  event (§5.1).

What it costs: both backends now include `<Utility/time_compat.h>`. `scripts/native-build.py`
already put `Dependencies/Utility` on the backend's include path, and so did the engine's CMake
(through `corei_always` → `core_utility`). The spike's five backend targets get it from
`UTILITY_INCLUDE_DIR` in `spikes/renderer/CMakeLists.txt`.

## 5. Red and green

**Cocoa**, `python3 scripts/macos-cocoa-key-routing-test.py --require-display`, M1 Pro, unlocked
session, all three modes (windowed, borderless fullscreen, fullscreen as the engine places it).
The test prints `timeGetTime() - systemUptime = 2935519590 ms on this machine` in each mode.
These are the first version of the test, which compared stamps with `timeGetTime()` after the
pump; §5.1 replaces that comparison.

| Check (per mode) | Pre-fix (`6a593e409`'s backend) | Fixed |
|---|---|---|
| AppKit's own stamps are on `systemUptime`'s clock | PASS (68–196 ms old) | PASS |
| fresh KEY_DOWN: hold as `checkKeyRepeat()` computes it | **FAIL 2,935,519,590 ms** | PASS 0–1 ms |
| fresh TEXT | **FAIL 2,935,519,590 ms** | PASS 0–1 ms |
| KEY_DOWN stamped 500 ms ago | **FAIL 2,935,520,090 ms** | PASS 500 ms |
| fresh MOUSE_DOWN | **FAIL 2,935,519,590 ms** | PASS 0 ms |
| the key-routing checks from #170 (24 per mode, 72 in all) | PASS | PASS |
| total | **12 failures** | **0 failures** |

On a Mac that has not slept since boot, the two clocks agree. There the clock checks cannot fail,
and the test prints a `NOTE` saying so.

**SDL2**, `CLANGXX=clang++-18 python3 scripts/native-sdl2-event-clock-test.py --require-display`.
Run in an Ubuntu 24.04 container (Docker on the M1 Pro, aarch64) with `libsdl2-dev`,
`libvulkan-dev`, `mesa-vulkan-drivers` and Xvfb, the packages the `window-seam-linux` CI job
installs:

| Check | Pre-fix | Fixed |
|---|---|---|
| clocks on the run | `timeGetTime()` = 920,773,532 ms, `SDL_GetTicks()` = 36 ms | — |
| fresh KEY_DOWN hold | **FAIL 920,773,496 ms** | PASS 0 ms |
| KEY_DOWN queued 300 ms before the pump | **FAIL 920,773,802 ms** | PASS 304 ms |
| fresh MOUSE_DOWN | **FAIL 920,773,496 ms** | PASS 0 ms |

SDL2's gap is not about sleep. `SDL_GetTicks()` counts from `SDL_Init` and `timeGetTime()` counts
from boot, so the SDL2 test fails on any machine. That makes it the robust red, and it is in CI:
the `window-seam-linux` job runs it. The Cocoa test runs in `window-seam-macos`. A runner that
never slept would not be able to tell the clocks apart, but the `macos-15` runner of PR #172's
first run measured `timeGetTime() - systemUptime = -446 ms`, which is outside the 333 ms the
`NOTE` uses as its threshold.

Gates run on the fixed tree: `window-input-scan.py --check` (759/759, 30/30),
`check-window-scancodes.py`, `check-window-seam-wiring.py`, `check-skill-coverage.py`,
`classify-changes.py --self-check` (15/15), `check-doc-figures.py`, `flake8 scripts/` and
`actionlint`. The spike's Cocoa targets (`zh-window-spike-cocoa`, `zh-macos-window-metrics`,
`zh-hidpi-tests-cocoa`) build on the Mac with AppleClang. Its SDL2 targets (`zh-window-spike`,
`zh-hidpi-tests-sdl2`) build in the container with clang 18.

### 5.1 What PR #172's first CI run caught, and the two corrections

**The native build counted the SDL2 test as an engine translation unit.** The `WWLib` probe
target walks `WWLib/**/*.cpp`, so `platform/tests/sdl2_event_clock_test.cpp` joined the build. The
denominator went from 845 to 846 TUs, and that one TU failed:
`'platform_window.h' file not found`. It also needs `<SDL.h>`, which the probe deliberately does
not have.

MEASURED by reproducing the job's exact command in an Ubuntu 22.04 container with clang 14:
`native-build.py --level 1 --level 2 --level 3 --with-shims --strict-link` gave "845 objects,
1 failures". The test now sits in `probe.OPTIONAL_BACKENDS` next to the backend it tests, for the
same reason. With that change the same command compiles 845/845 TUs with 0 failures.

The container ran on the M1 Pro, so it was aarch64. On that host the gate also reports 12
`__aarch64_*` outline-atomics helpers as undefined, in both the red and the green run. They are an
artefact of the host architecture, not of this change.

Rerun as `linux/amd64` (emulated), which is what the CI job runs: 845/845 objects, 0 failures,
393 unresolved (baseline 393), and `check-native-build-baseline.py` reports "OK: no regression
against the baseline".

**The macOS runner failed `fresh MOUSE_DOWN: Time_Ms is 53 ms before timeGetTime()`, windowed
mode only.** The conversion was right; the assertion was not. The test compared the stamp with
`timeGetTime()` read *after* the pump. That measures the event's age at the moment of the check,
which includes however long the pump took to reach the event and dispatch it.

Reproduced on the M1 Pro by sleeping 60 ms between posting the mouse-down and pumping it. The old
check failed with "68 ms before timeGetTime()". That the runner's 53 ms is the same kind of latency
is INFERRED. It hit only the first mouse event of the process (windowed mode), and both other
modes read 0 ms.

The checks now compare `Time_Ms` with the engine time at which the test stamped the event. That
time is read back to back with `systemUptime` (Cocoa), or just before `SDL_PushEvent` (SDL2). The
tolerance is 2 ms, for millisecond truncation. Each check prints the pump's latency separately.

With the 60 ms injected delay, the new check passes: 0 ms off, pump 68 ms late. Run against the
pre-fix backend, it still fails all 4 Cocoa checks (by about 34 days of sleep) and all 3 SDL2
checks (by 980,244,456 ms).

## 6. Still open

- **UNMEASURED in a running game:**
  - that a key press no longer produces a same-frame `KEY_STATE_AUTOREPEAT` before the fix and
    does after;
  - what the player saw before the fix (a text entry's Backspace, list-box arrows).

  The check: log `Keyboard::checkKeyRepeat()`'s return, or the `MSG_RAW_KEY_DOWN` states, for
  one tap and one 1 s hold of Backspace in a text field, before and after.
- **Not explained:** the doubled Escape in fullscreen (`cocoa-key-routing.md` §3). The synthetic
  repeats carry `KEY_STATE_AUTOREPEAT` and `MetaEventTranslator` eats them for the mapped
  Escape, so they should not toggle the quit menu (INFERRED). That question stays open.
- `getMessageTime()` has no caller. It is left in place because it belongs to the seam's
  wiring, not to this fix.
