# Cocoa key routing: the alert sound on Escape and the arrows

User report, on the M1 Pro (2026-09-30): *"In full screen I can't press Esc. It gives me the 'pop'
[the macOS alert sound] instead of bringing up the pause menu. Same when I navigate with the arrow
keys (at any size)."*

That is two symptoms: (1) the alert sound on plain keys, windowed and fullscreen; (2) Escape not
opening the pause (quit) menu in fullscreen. This document fixes (1). It does **not** explain (2),
because nothing measured so far reproduces it. See §3.

## 1. The alert sound: MEASURED

`Window_Pump()` in `platform_window_cocoa.mm` dequeues each `NSEvent`, calls `Translate()` (which
queues `WINDOW_EVENT_KEY_DOWN`/`_UP` and `WINDOW_EVENT_TEXT` for the engine), and then forwards the
event with `-[NSApplication sendEvent:]` so that the title bar, the menu and Cmd-Q keep working.
For a plain key, `sendEvent:` hands the event to the key window's first responder. That is the game
view, which had no `keyDown:`, so the key walked the responder chain (view, window, application)
unhandled and ended in `-[NSResponder noResponderFor:]`. For `keyDown:`, that method plays the system
alert sound. The engine had already received the key, so the sound was the only visible effect.

The measurement is `scripts/macos-cocoa-key-routing-test.py`. It compiles the real backend with
`Core/Libraries/Source/WWVegas/WWLib/platform/tests/cocoa_key_routing_test.mm`, which:
- opens the window through `Window_Create()`;
- posts key events with `-[NSApplication postEvent:atStart:]`;
- drains them through `Window_Poll_Event()`, which is the real pump;
- replaces `-[NSResponder noResponderFor:]` (implemented only on `NSResponder`; `NSView`, `NSWindow`
  and `NSApplication` inherit it, checked with the ObjC runtime) with a counter that does not beep.

Before the fix, on the M1 Pro (macOS 26.6.1, AppleClang, unlocked login session):

| Mode | Keys | Reached the seam's queue | `noResponderFor:keyDown:` (beeps) |
|---|---|---|---|
| windowed 320x240 | Escape, 4 arrows, A, Tab, F1 | KEY_DOWN + KEY_UP, correct set-1 codes, all 8 | **all 8** |
| windowed | Return | KEY_DOWN + KEY_UP | no; only `noResponderFor:keyUp:`, which is silent |
| fullscreen (borderless) | same 9 keys | all 9 | same 8 of 9 |
| both | Cmd-Q | `WINDOW_EVENT_CLOSE` through the main menu | no |

`keyUp:` and `mouseMoved:` also fall off the chain, but AppKit beeps only for `keyDown:`. The test
logs those two and counts only `keyDown:`.

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
| They no longer do after the fix; Cmd-Q still raises `WINDOW_EVENT_CLOSE`; text is still queued | UNMEASURED: the session locked before the green run. See §4 |
| The sound the user hears is this path | INFERRED. The sound itself has not been heard by anyone since the fix; a human has to confirm it is gone |
| A `CGEventPost` HID key in the running game hits `NSBeep` before the fix and not after | UNMEASURED: `scripts/macos-key-routing-probe.py` exists for this, but the session was locked |

## 3. Escape in fullscreen: OPEN

`ToggleQuitMenu()` opens the menu only when `canOpenQuitMenu()` allows it, and that requires
`TheGameEngine->isActive()`, which is the seam's `Window_Is_Active()`: whether the game window is
the key window. Since the pump queues Escape whatever AppKit does with it afterwards, the beep fix
does not change whether Escape *reaches* the engine. The candidates, none measured:

- **The window is not key in the user's fullscreen session**, so the engine is inactive and the
  quit menu is refused. `docs/porting/mouse-cursor-seam.md` §6.2 measured `active=true` in
  fullscreen after #157, but that was a fullscreen pause menu reached through the control bar's
  Options button, not Escape.
- **The user's fullscreen session differed from the probes'** (for example, launched without
  `-xres/-yres`, which leaves an 800x600 borderless window at the top-left; see
  `next-slice-scope.md` residual 3), and the key went to another app.
- **Escape was pressed in a state where the quit menu is gated** (a cinematic, map loading, the
  game ending).

What has been ruled out, MEASURED in the unit test: AppKit swallowing Escape before the pump in a
borderless window created fullscreen. Escape reached the queue there, and the window was key.
The test's third case replays the engine's own fullscreen placement after creation
(`SetWindowPos(HWND_TOPMOST, ...)` through the shim's three calls: always-on-top, position, client
size). It was added after the session locked and is UNMEASURED.

The next measurement is `scripts/macos-key-routing-probe.py`, on an unlocked session, in fullscreen
(`-xres 1728 -yres 1117`) and windowed. It clicks into a skirmish, posts Escape and each arrow
through the HID tap, and records:
- `NSBeep` and `ToggleQuitMenu` hits;
- `quit_menu_visible`, `TheGameEngine->m_isActive` and `Window_Is_Active`;
- the key window and its first responder;
- the tactical camera's pivot before and after each arrow.

If the fullscreen run opens the menu, the user's report needs their exact launch line and the
moment they pressed Escape.

## 4. Why the live half is missing

The red run above was taken while the login session was unlocked. Minutes later the session
locked (`CGSessionCopyCurrentDictionary`: `CGSSessionScreenIsLocked=1`, `CGDisplayIsAsleep=1`).
From then on, no window from this machine's processes became key:
- the unit test reported SKIP;
- the game, launched from the shell in both modes, sat at the main menu with
  `window_is_active false` and `mouse_x 0`;
- under LLDB, Accessibility activation left `[NSApp isActive]` at 0.

A locked session is a measurement limitation, not a port defect. It also invalidates any key or
click result taken while it lasts, so none is quoted here.
