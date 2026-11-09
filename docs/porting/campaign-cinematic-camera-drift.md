# Camera keeps scrolling after a campaign cinematic

User report (M1 Pro, fullscreen): in USA-01, once control came back after the train cinematic, the
view kept dragging left. Moving the mouse or pressing arrow keys moved it for a moment, then it
drifted again.

The cause is in `Core`, shared by both games, and is not specific to the port. It is an interaction
between two TheSuperHackers changes. Retail did not have it (INFERRED from the pre-#2621 source,
not run on Windows).

Status words: MEASURED (run and observed), INFERRED (read from code, not run), UNMEASURED (open).

## Mechanism

1. `LookAtTranslator` keeps a file-static `scrollDir[4]`, one flag per arrow key, set on every
   `MSG_RAW_KEY_DOWN` and cleared on `MSG_RAW_KEY_UP`. On every key event it counts the flags that
   are set and starts a keyboard scroll (`SCROLL_KEY`) if any are, or stops it if none are.
2. A cinematic's `DISABLE_INPUT` runs `ScriptActions::doDisableInput` → `resetModes()`. Since #168
   this stops the scroll in progress, but it left `scrollDir[]` as it was.
3. While input is disabled, `WindowTranslator` (priority 10) drops every key except ESC (#2621,
   `cec296caa`: "Block all button presses except ESC during scripted camera scenes"). Before #2621 it
   dropped only ESC's key-up, so arrow key-ups still reached `LookAtTranslator` (priority 60).
4. So an arrow held when the cinematic starts and released during it keeps its flag set. After
   `ENABLE_INPUT`, the next key event of any kind finds that flag, starts `SCROLL_KEY`, and scrolls
   toward the released arrow every frame. Tapping another arrow adds and removes its own direction
   but leaves the stale one. Mouse moves can't start an edge or right-drag scroll while a key
   scroll is running. Only pressing and releasing the stale arrow again clears it.

USA-01's `CINE_TRSetup` (the train leaving the depot) runs `DISABLE_INPUT` at a moment when a player
is likely to be scrolling with the arrows. The intro cinematic cannot cause this: it disables input
at frame 0, before any key event can reach the translator.

## MEASURED: reproduction on the Mac

M1 Pro, fullscreen `-nologo -xres 1728 -yres 1117`, branch base `0047fd94f` built with
`native-build.py --level 1..4 --with-shims --strict-link` (981/981, 0 unresolved, arm64). Harness:
`scripts/macos-campaign-cinematic-probe.py`. It runs the game under LLDB with a private
`CNC_USER_DATA` copy of the user's `Options.ini`, and clicks Single Player → USA → Easy with real
`CGEventPost` events. The intro runs on its own: input disabled at frame 37, enabled at 2494.

**How the cinematic was reached (forced).** In the game, the train cinematic starts when the player
fills both train cars (`Check the Train` → flag `Go Train Go`). The probe sets that flag in
`TheScriptEngine->m_flags` and changes nothing else. The map's own scripts did the rest:
`Leave The Depot`, then `CINE_TRSetup`'s `DISABLE_INPUT` (`resetModes` hit from
`ScriptEngine::executeActions` at frame 2861), then `CINE_TRReturnToPlayer`'s `ENABLE_INPUT` at
frame 3734. The probe looked for the `Time_To_Wait_At_Station` timer by name and did not find it,
so it wrote nothing there. The cinematic started anyway, and why the timer was not found is
UNEXPLAINED. The script tree used here was decoded from `MapsZH.big`'s `MD_USA01.map`, offline.

**Input (all real OS events).** Left arrow down at frame ~2472 while input was enabled; it stayed
down across `DISABLE_INPUT` and came up during the cinematic. After `ENABLE_INPUT` the probe waited
4 s, then tapped Right, then Up, moved the pointer to the centre, and finally tapped Left.

| Phase (unfixed build) | `scrollDir[LEFT]` | LookAt scrolling | camera Δx (world) |
|---|---|---|---|
| `DISABLE_INPUT` (frame 2861) | 1 | stopped by `resetModes` | |
| Left released during the cinematic (2897); keyboard reports it up | **1** | 0 | |
| `ENABLE_INPUT` (3734), 4 s idle | **1** | 0 | 0.0 |
| Right tapped | 1 | **1** (`SCROLL_KEY`, from 3780) | **−569.97** (moved left) |
| Up tapped | 1 | 1 | −21.59, then at the map's left edge (x 130.68) |
| pointer centred | 1 | 1 | 0.0 (pinned at the edge) |
| Left tapped | 0 | 0 (`stopScrolling` at 3929) | — |

Throughout: `m_cameraLock` 0, `m_mouseLocked` released by `resetModes`, pointer captured, and
`TheMouse->m_visible` 0 during the cinematic and 1 after it. No camera lock or tether was involved,
and none exists in USA-01's scripts.

## The fix

`LookAtTranslator::resetModes()` also clears `scrollDir[]`. Disabling input already means "forget
the modes we may miss the exit message for". `InGameUI::setInputEnabled(false)` says exactly this
for Ctrl/Alt/Shift. The arrows are the same case, and #2621 made them reachable. An arrow still
held when input comes back resumes scrolling with its next auto-repeat key-down: the unit test
asserts the translator side, and that `Keyboard::checkKeyRepeat` sends those key-downs is INFERRED
from source, not run live.

Why not let key-ups through `WindowTranslator`? It would reopen the hole #2621 closed for other
translators' key handling, and `resetModes` already exists as the single "input was disabled" reset.

**Windows and determinism (INFERRED; the CI replay gate is the oracle).** Everything changed is
GameClient-side: the look-at translator's arrow flags. None of it is simulated, transferred by
`xfer`, or included in the CRC. The same code runs on Windows, where current upstream builds
should have the same bug (UNMEASURED there).

## MEASURED: test, red then green

`lookat_reset_modes_test.cpp` (run by `scripts/native-lookat-reset-modes-test.py`) gained two cases
against the real translator. In the first, an arrow is held into a disable, its key-up is not
delivered, and after the re-enable another arrow is tapped and released: there must be no scroll
and no lock. In the second, an arrow is held through the whole cinematic: its auto-repeat must
scroll again, and releasing it must stop. On the Mac against the `native-build.py` archives:
**2 failed checks** before the fix, **0** after it. All earlier checks still pass.

## MEASURED: re-test on the fixed build

Same machine and flow, binary rebuilt from this branch:

- After the release during the cinematic (frame 2868) `scrollDir[LEFT]` is **0**.
- After `ENABLE_INPUT` (3706): idle Δx 0. Right tap Δx **+129.53**, then stopped. Up tap Δy
  +164.58, then stopped. Left tap Δx −129.53, then stopped. Each tap is one
  `setScrolling`/`stopScrolling` pair, one frame apart.

## INFERRED / UNMEASURED

- **UNMEASURED:** the natural trigger, with the cars filled by a player. Only the flag was forced.
  A save made just before the depot capture would allow a natural run.
- **INFERRED:** that this is the user's mechanism. It matches every detail of the report (left
  drift after the train cinematic, other arrows pushing the camera only briefly). The user's own
  confirmation on the fixed build is UNMEASURED.
- **UNMEASURED:** Windows, retail and current upstream.
- **UNMEASURED:** the `NSCursor` hidden during a cinematic. Only the engine's `m_visible` was read.
- `scrollDir[]` outlives a game (it is static). A key released on the score screen goes through
  the same drop when input is disabled. The end-of-game route runs `resetModes` too, so it is
  covered by the same fix (INFERRED).

## Reproducing

```sh
python3 scripts/native-build.py --level 1 --level 2 --level 3 --level 4 --with-shims \
    --strict-link --build-dir build/native-macos-arm64-drift
cp build/native-macos-arm64-drift/native_strict_link <run dir>/zh
codesign -s - -f --entitlements <get-task-allow.plist> <run dir>/zh
python3 scripts/macos-campaign-cinematic-probe.py --run-dir <run dir> \
    --user-data-src <dir holding "Command and Conquer Generals Zero Hour Data"> \
    --hold-key left --out held.json          # --hold-key none: the control, not run yet
python3 scripts/native-lookat-reset-modes-test.py   # reads build/native: symlink it to the build dir
```

Each probe run took about 12 minutes on the M1 Pro, most of it the intro under LLDB. The reports
and logs belong outside the repo.
