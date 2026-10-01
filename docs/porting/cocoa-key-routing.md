# Cocoa key routing: the alert sound on Escape and the arrows

User report, on the M1 Pro (2026-09-30): *"In full screen I can't press Esc. It gives me the 'pop'
[the macOS alert sound] instead of bringing up the pause menu. Same when I navigate with the arrow
keys (at any size)."*

That is two symptoms: (1) the alert sound on plain keys, windowed and fullscreen; (2) Escape not
opening the pause (quit) menu in fullscreen. The same change fixes both. (1) is MEASURED in §1. (2)
is MEASURED once per case live in §3; its mechanism is INFERRED.

## 1. The alert sound: MEASURED

`Window_Pump()` in `platform_window_cocoa.mm` dequeues each `NSEvent`, calls `Translate()` (which
queues `WINDOW_EVENT_KEY_DOWN`/`_UP` and `WINDOW_EVENT_TEXT` for the engine), and then forwards the
event with `-[NSApplication sendEvent:]` so that the title bar, the menu and Cmd-Q keep working.
For a plain key, `sendEvent:` hands the event to the key window's first responder. That is the game
view, which had no `keyDown:`, so the key went up to `NSWindow`'s own `keyDown:`. That method plays
the system alert sound: Escape through `cancelOperation:`, every other unconsumed key through
`-[NSResponder noResponderFor:]`. The engine had already received the key, so the sound was the only visible effect.

The measurement is `scripts/macos-cocoa-key-routing-test.py`. It compiles the real backend with
`Core/Libraries/Source/WWVegas/WWLib/platform/tests/cocoa_key_routing_test.mm`, which:
- opens the window through `Window_Create()`;
- posts key events with `-[NSApplication postEvent:atStart:]`;
- drains them through `Window_Poll_Event()`, which is the real pump;
- replaces both AppKit methods that end in `NSBeep()` with counters that do not beep.

There are two such paths, and an LLDB breakpoint on `NSBeep` was needed to find the second:

- **`-[NSResponder noResponderFor:]` with `keyDown:`.** This is where the arrows, letters, Return
  and the F-keys end. It is implemented only on `NSResponder`; `NSView`, `NSWindow` and
  `NSApplication` inherit it (checked with the ObjC runtime).
- **`-[NSWindow cancelOperation:]`.** `NSWindow`'s own `keyDown:` turns Escape into
  `cancelOperation:`, which beeps when the window has nothing to cancel. Pre-fix, one Escape gave
  1 hit on `cancelOperation:`, 1 hit on `NSBeep`, and 0 on `noResponderFor:`. Post-fix, all three
  were 0.

The test runs three modes:
- windowed 320x240;
- fullscreen, as `Window_Create()` makes it (borderless);
- fullscreen as the engine then places it: `DX8Wrapper`'s `SetWindowPos(HWND_TOPMOST, 0, 0, w,
  h, 0)`, replayed through the shim's three calls.

It checks nine keys: Escape, the four arrows, A, Return, Tab and F1. It also checks that
`WINDOW_EVENT_TEXT 'a'` is still queued, and that Cmd-Q still raises `WINDOW_EVENT_CLOSE`. Results
on the M1 Pro (macOS 26.6.1, AppKit, unlocked login session, 2026-10-01):

| Build | Keys reaching the seam's queue (KEY_DOWN + KEY_UP, set-1 code) | Keys reaching a beep path | Cmd-Q / text |
|---|---|---|---|
| pre-fix (`490f12bca`) | 9 of 9, in all 3 modes | **8 of 9 in every mode**: Escape (`cancelOperation:`), the 4 arrows, A, Return, F1 (`noResponderFor:keyDown:`). **24 failures** | both still work |
| fixed | 9 of 9, in all 3 modes | **0**. 73 PASS, **0 failures** | both still work |

Tab does not beep before the fix: `NSWindow` consumes it for key-view navigation. `keyUp:` and
`mouseMoved:` also fall off the chain, but AppKit does not beep for those, so the test only logs
them.

Two source-reading inferences turned out wrong under measurement. Both are recorded so that nobody
repeats them:
- The first guess was that `-[NSWindow makeFirstResponder:view]` was refused because `NSView`
  answers `acceptsFirstResponder` with `NO`. The game view **is** the first responder, measured in
  both modes.
- The engine **does** receive Escape and the arrows in both modes. The beep is not the engine
  missing the key.

### The fix

`WWGameView` answers `keyDown:`, `keyUp:` and `flagsChanged:` itself, and does nothing in them. The
pump keeps forwarding every event to `sendEvent:`. This was chosen over filtering key events out of
`sendEvent:`, because:
- `-[NSApplication sendEvent:]` matches key equivalents against the main menu *before* any view sees
  the key, so Cmd-Q (and any future menu shortcut) keeps working with no special case in the pump.
- AppKit still sees every key for its own bookkeeping (key-window state, the menu bar's tracking).
- Text input is unaffected, because `Translate_Text()` reads `-[NSEvent characters]` in the pump
  and never uses the responder chain.

Nothing under `_WIN32` changed; the file is not compiled on Windows.

Gate: `scripts/macos-cocoa-key-routing-test.py`. It runs in the `window-seam-macos` CI job, and
locally with `--require-display`. It needs the window to become key, so on a locked or headless
session it reports SKIP rather than PASS.

## 2. Status of each claim

| Claim | Status |
|---|---|
| Plain keys reached the beep path before the fix, windowed and fullscreen | MEASURED (NSEvents posted through the real pump, not a human keyboard) |
| They no longer do after the fix; Cmd-Q still raises `WINDOW_EVENT_CLOSE`; text is still queued | MEASURED, same harness: 0 failures in all three modes; an LLDB breakpoint on `NSBeep` counted 0 hits post-fix against 1 per Escape pre-fix |
| The sound the user hears is this path | INFERRED. The sound itself has not been heard by anyone since the fix; a human has to confirm it is gone |
| A `CGEventPost` HID key in the running game hits `NSBeep` before the fix and not after | MEASURED, n=1 per case (§3): 1 beep per Escape and per arrow before the fix, 0 after, windowed and fullscreen |
| Escape opens and closes the pause menu in fullscreen after the fix | MEASURED, n=1 (§3); before the fix one press toggled it twice |

## 3. Escape in fullscreen: MEASURED once per case, fixed by the same change

`scripts/macos-key-routing-probe.py` was run in a live skirmish on 2026-10-01. Setup:
- the M1 Pro, `~/devin-work/cocoa-keys/run`;
- real `CGEventPost` HID-tap events (synthetic, not a human keyboard);
- both binaries, `zh-before` (`490f12bca`) and `zh-after` (`5dea77f56`), each run fullscreen
  (`-xres 1728 -yres 1117`) and windowed (`-win -xres 1024 -yres 768`);
- one run per case.

Each step pressed one key once, or held an arrow for one second. Hit counts are per step. `NSBeep`
is the sound; `noResponderFor:` also counts the silent `keyUp:` calls. "Quit menu" is
`m_isQuitMenuVisible` after the step.

| Case | Step | `NSBeep` | `cancelOperation:` | `ToggleQuitMenu` | Quit menu | Camera pivot moved |
|---|---|---:|---:|---:|---|---|
| before, windowed | Escape (open) | 1 | 1 | 1 | open | — |
| | Escape (close) | 1 | 1 | 1 | closed | — |
| | each arrow | 1 | 0 | 0 | closed | yes, all four |
| before, **fullscreen** | Escape (open) | 1 | 1 | 1 | open | — |
| | **Escape (close)** | **2** | **2** | **2** | **still open** | — |
| | left arrow (next step) | 2 | **1** | **1** | closed | yes |
| | other arrows | 1 | 0 | 0 | closed | yes |
| after, windowed | Escape (open) / (close) | 0 / 0 | 0 / 0 | 1 / 1 | open / closed | — |
| | each arrow | 0 | 0 | 0 | closed | yes, all four |
| after, fullscreen | Escape (open) / (close) | 0 / 0 | 0 / 0 | 1 / 1 | open / closed | — |
| | each arrow | 0 | 0 | 0 | closed | yes, all four |

How much the arrows moved the camera did not change with the fix. Windowed: left 1362.7 →
269.8, down 1966 → 600. Fullscreen after the fix: left → 714.8, down → 1156.7. Before the fix:
left → 735.9, down → 1180.4.

What this settles:

- **The user's fullscreen symptom reproduces before the fix and is gone after it.** Before the
  fix, one Escape press in fullscreen produced two `cancelOperation:` calls, two beeps and two
  toggles. The menu closed and reopened, so to a player Escape "did nothing". A third
  `cancelOperation:` and toggle then landed during the next step. After the fix, every Escape
  produced exactly one toggle and no beep, in both modes.
- **The window was key and the engine active throughout**, in every case:
  - `[NSApp isActive]` 1 and `TheGameEngine->m_isActive` 1;
  - the key window was the game window and its first responder `WWGameView`.
  
  So the "not key in fullscreen" candidate is ruled out. The extra toggles are not a gating
  problem in `canOpenQuitMenu()`.
- The arrows scrolled in every case, before and after the fix. Only the beep differed.

The mechanism of the extra Escape is **INFERRED, n=1 per case**. `NSBeep()` is synchronous and
runs inside `Window_Pump()`'s `sendEvent:`. Before the fix it ran on the pump for every Escape,
and in fullscreen it stalled the pump long enough for the window server to deliver Escape
again. The fix removes the beep, and with it the stall. One thing does not fit a plain
auto-repeat: `PlatformWindowHost::handleEvent()` drops key events whose `Repeat` flag is set, so
an auto-repeat alone could not toggle the menu. The second and third toggles therefore came from
key-downs that arrived *without* the repeat flag, or from a second caller of `ToggleQuitMenu()`.
Which of these it was is UNMEASURED. Settling it needs the `WINDOW_EVENT_KEY_DOWN` stream for
Escape (with `Repeat` and `Time_Ms`) and the callers of each `ToggleQuitMenu()` hit, recorded in
a pre-fix fullscreen run. It also needs more than one run per case.

## 4. The run that could not be measured, and why

The first live attempt was made while the login session was locked
(`CGSSessionCopyCurrentDictionary`: `CGSSessionScreenIsLocked=1`, `CGDisplayIsAsleep=1`). No window
became key, and the game sat at the main menu with `window_is_active false` and `mouse_x 0`. A
locked session is a measurement limitation, not a port defect, and nothing taken under it is
quoted here. The unit results in §1 and the live table in §3 were taken after it unlocked. Even
then, the first window a test process opens occasionally fails to become key within the unit
test's 3 s wait; the test reports that mode as SKIP rather than PASS.

Still owed: the user's confirmation, by ear and by hand, that the sound is gone and that Escape
opens and closes the pause menu in fullscreen.
