# Dead score screen after a skirmish that ended mid-scroll

User report: after a skirmish that ended naturally (win or lose), the score screen showed its stats
and the pointer, and none of its buttons answered a click. Earlier Mac measurements reached the
same screen through Escape → Exit → Yes, and its buttons worked there.

The cause is original engine code in `Core`, shared by both games, not a port defect. Retail has
the same path; the Mac made it easier to reach once #157 let the fullscreen pointer rest on the
screen edge, where it edge-scrolls (INFERRED: not measured on Windows or on a pre-#157 build).

## Mechanism

1. Starting a camera scroll (screen edge, arrow key or right-drag) runs
   `LookAtTranslator::setScrolling`, which sets `TheTacticalView->setMouseLock(TRUE)`. Only
   `LookAtTranslator::stopScrolling` clears it.
2. A skirmish ends through the skirmish scripts: `ScriptActions::doVictory` / `doDefeat` →
   `doDisableInput` → `LookAtTranslator::resetModes`. Before the fix `resetModes` set
   `m_isScrolling = FALSE` without calling `stopScrolling`, so the lock stayed set. Nothing else
   stopped the scroll: with input disabled, `MSG_RAW_MOUSE_POSITION` calls `stopScrolling` only
   `if (m_isScrolling)`, and that flag was already false.
3. On the way to the shell `InGameUI::reset` clears the in-game UI's scrolling flag. `View::reset`
   does not touch `m_mouseLocked`.
4. `WindowTranslator::translateGameMessage` (`WindowXlat.cpp`) returns `KEEP_MESSAGE` for every
   message while the view is mouse-locked, unless the in-game UI is scrolling. Step 3 removed that
   exception, so no mouse message reached the window manager.

The quit route never hits this: `MSG_META_OPTIONS` (Escape) calls `stopScrolling()`
unconditionally (`LookAtXlat.cpp`, `case GameMessage::MSG_META_OPTIONS`).

## MEASURED: reproduction on the Mac

M1 Pro, macOS 26.6.1, fullscreen `-nologo -xres 1728 -yres 1117`, `main` `ec61b5c07` built by
`native-build.py --level 1..4 --with-shims --strict-link` (981/981 objects, 0 unresolved, arm64).
Harness: `scripts/macos-game-end-mouse-lock-probe.py`, which launches the game under LLDB, clicks
Single Player → Skirmish → Play Game with real `CGEventPost` events (default skirmish: one Easy AI),
and reads engine state at each phase.

**How the game was ended.** At the top of one `GameLogic::update` the probe calls
`Player::killPlayer()` on every computer player except the neutral one. That kill is the only
engine call the probe makes. The skirmish scripts then detected the win on their own:
`doVictory` was reached from `ScriptEngine::update → executeScripts → executeAction`, and the
end-game timer took the game to `Menus/ScoreScreen.wnd`. This is stronger evidence than calling
`doVictory` directly, but the enemy did not lose by fighting.

**Edge case.** Before the end, one mouse move to the right edge (client x = 1727). Once `doVictory`
had run, the pointer was moved to the centre, as a player does when the victory banner appears.
Then `ButtonOk` (the score screen's `EXIT`, centre 1477,1060) was clicked. Two runs, the same
result; the second run below.

| Phase | `m_mouseLocked` | LookAt `m_isScrolling` / `m_scrollType` | InGameUI `m_isScrolling` / `m_inputEnabled` | shell top |
|---|---|---|---|---|
| in game, pointer on the edge | 1 | 1 / 3 (`SCROLL_SCREENEDGE`) | 1 / 1 | (in game) |
| entry to `doVictory` (frame 420) | 1 | 1 / 3 | 1 / 1 | |
| entry to `resetModes` | 1 | 1 / 3 | 1 / 0 | |
| after the end, pointer centred | **1** | 0 / 3 | 1 / 0 | |
| score screen | **1** | 0 / 3 | 0 / 1 | `ScoreScreen.wnd` |
| after the `ButtonOk` click | **1** | 0 / 3 | 0 / 1 | `ScoreScreen.wnd` (did not move) |

- `TheWindowManager` `m_modalHead`, `m_grabWindow`, `m_mouseCaptor` were 0 in every phase, which
  rules out the two lower-ranked alternatives (a stale grab window, a leftover modal).
- A hardware watchpoint on `m_mouseLocked`, armed before the pointer reached the edge, recorded
  exactly one write: 0 → 1 from `View::setMouseLock ← LookAtTranslator::setScrolling ←
  translateGameMessage ← MessageStream::propagateMessages`. `stopScrolling` was never entered.
- During the click (and 3 s after it) `GameWindowManager::winProcessMouseEvent` ran **0** times.
  `WindowTranslator` calls it for every mouse message it lets through, so the click was dropped
  in `WindowTranslator` before any window saw it.

**Control, no scroll at the end** (pointer at the screen centre): `m_mouseLocked` 0 throughout,
`winProcessMouseEvent` ran **51** times during the click, and the shell moved to
`Menus/SkirmishGameOptionsMenu.wnd`: EXIT worked.

## The fix

`LookAtTranslator::resetModes()` now calls `stopScrolling()` when a scroll is in progress, instead
of clearing `m_isScrolling`. The other four mode flags are cleared as before.

Why here and not in the game-reset path:

- `resetModes` has one caller, `ScriptActions::doDisableInput`, which runs for victory, defeat,
  quick victory and the `DISABLE_INPUT` script action that campaign cinematics use. All of them
  want the scroll stopped: with input disabled the translator's own mouse-position handler already
  calls `stopScrolling()` whenever `m_isScrolling` is set. `resetModes` clearing the flag first is
  what pre-empted that.
- Clearing the lock in `View::reset` would fix only the end of a game. A cinematic started
  mid-scroll would still leave the view locked and the in-game UI "scrolling" after
  `ENABLE_INPUT`, until the player scrolled again (INFERRED from source, not run).
- `stopScrolling` also restores the cursor saved when the scroll started and closes the
  `StatsCollector` scroll timer that `setScrolling` opened, so both stay balanced.
  `InGameUI::setScrolling(FALSE)` sets the arrow cursor and touches no camera or logic state.
  Every pointer it uses (`TheInGameUI`, `TheTacticalView`, `TheMouse`) is also used by
  `doDisableInput`, so none can be null there.

**Windows and determinism (INFERRED, the CI `Replay Check GeneralsMD` gate is the oracle).**
`doDisableInput` runs inside the script engine, but everything the change touches is client-side:
the look-at translator, the in-game UI's scroll flag and cursor, the view's mouse lock and the
debug stats collector. None of it is simulated, transferred by `xfer`, or included in the CRC, so
logic and replays cannot change. On Windows the only behavioural difference is the bug going
away. No `#ifdef` is involved.

## MEASURED: test, red then green

`scripts/native-lookat-reset-modes-test.py` compiles
`Core/GameEngine/Source/GameClient/MessageStream/tests/lookat_reset_modes_test.cpp` with
`LookAtXlat.cpp`'s own flags and links it against the `native-build.py` archives, using the
`native-exit-teardown-test.py` recipe. It runs the real `LookAtTranslator` and `InGameUI` (a
subclass supplying its two pure virtuals) with the engine's `ViewDummy` and `MouseDummy`. It starts
a key scroll and a right-drag scroll with raw `GameMessage`s, runs `doDisableInput`'s input
reset, and asserts that the in-game UI stopped scrolling and that the view is unlocked. A third
case checks that scrolling still locks and unlocks after input is re-enabled. Edge scrolling is
not in the test, because it needs `TheDisplay`; the live run above covers it.

On the Mac, against the same archives: **6 failed checks** before the fix (both scroll types:
still scrolling, still locked, clicks dropped), **0 failed checks** after it. Wired into
`native-port-ci.yml` next to the exit-teardown test. On Linux it runs in CI only.

## MEASURED: re-test on the fixed build

Same machine, same flow, binary rebuilt from this branch:

- **Edge case:** the watchpoint recorded the 0 → 1 write from `setScrolling`, then 1 → 0 at
  frame 423 from `View::setMouseLock ← stopScrolling ← resetModes ← doDisableInput ← doVictory`.
  `m_mouseLocked` was 0 at the score screen, `winProcessMouseEvent` ran 50 times during the
  `ButtonOk` click, and the shell moved to `Menus/SkirmishGameOptionsMenu.wnd`: EXIT works.
  An earlier fixed-build run gave the same state and the same 50 calls. Its post-click snapshot
  then failed, and the probe of that time did not record why (it now records the exception), so
  that run does not count as evidence that the shell moved on.
- **Control, no scroll:** unchanged. 50 calls, and the shell moved to `SkirmishGameOptionsMenu.wnd`.

## INFERRED / UNMEASURED

- **UNMEASURED:** a skirmish lost or won by fighting, as opposed to ended with `killPlayer`.
  `doDefeat` follows the same `doDisableInput` path as `doVictory`.
- **UNMEASURED:** the arrow-key and right-drag triggers live on the Mac. The test covers both
  against the real translator.
- **UNMEASURED:** Windows. The same code runs there, and edge-scrolling with a clipped pointer at
  the moment of victory should reproduce it. It is not known whether it was ever reported.
- **INFERRED:** before the fix, a player on the score screen in fullscreen could probably unstick it
  by touching a screen edge and leaving it. With input enabled again in the shell, that starts and
  stops an edge scroll, and the stop clears the lock. Not tried.
- **INFERRED:** the campaign cinematic variant (`DISABLE_INPUT` mid-scroll) is fixed by the same
  change.
- **UNMEASURED:** the user's own confirmation by hand.

## Reproducing

```sh
python3 scripts/native-build.py --level 1 --level 2 --level 3 --level 4 --with-shims \
    --strict-link --build-dir build/native-macos-arm64-lock
cp build/native-macos-arm64-lock/native_strict_link <run dir>/zh
codesign -s - -f --entitlements <get-task-allow.plist> <run dir>/zh
python3 scripts/macos-game-end-mouse-lock-probe.py --run-dir <run dir> --scroll edge --out edge.json
python3 scripts/macos-game-end-mouse-lock-probe.py --run-dir <run dir> --scroll none --out none.json
# the test reads build/native: symlink it to the build dir above, or build there
python3 scripts/native-lookat-reset-modes-test.py
```

Each probe run took about eight minutes on the M1 Pro. The JSON reports and stderr logs belong
outside the repo.
