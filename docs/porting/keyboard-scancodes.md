# Keyboard scan codes — taking `<dinput.h>` off the device-independent side

`GameClient/KeyDefs.h` defines `KeyDefType`, the engine's key enumeration. It is included by the
input system, the GUI, the hotkey/meta-event tables and most menu code. Until now its values were
written as DirectInput macros:

```cpp
enum KeyDefType CPP_11(: UnsignedByte)
{
    KEY_KP0 = DIK_NUMPAD0,
    ...
};
```

so the header did `#include <dinput.h>`, and every translation unit that wanted a key code pulled
the DirectInput SDK — and through it the Windows SDK — into itself. The file's own comment has
carried a `@todo` about this since 2001.

## Measurement

| | Count |
|---|---:|
| `DIK_` references in the two `KeyDefs.h` copies | 121 each (107 distinct codes) |
| `DIK_` references anywhere else in `Core/`, `Generals/`, `GeneralsMD/` (tools excluded) | **0** |
| Probe TUs whose first native diagnostic was inside the fetched `dinput.h` | **82** |

The whole dependency was those two headers. Nothing in the engine reads a `DIK_` constant
directly, so the include could be removed without touching call sites.

## What changed

`Core/GameEngine/Include/GameClient/KeyScanCodes.h` holds the 107 codes as a plain enum
(`KEYSCAN_A`, `KEYSCAN_NUMPAD0`, …) with the same numeric values, and both `KeyDefs.h` copies now
include that instead of `<dinput.h>`. The values are PC/AT set-1 scan codes; they are stored in
saved key bindings and in the shipped INI files, so they are part of the data format and were
copied out of the SDK header rather than renumbered — including the aliases (`DIK_CAPSLOCK` →
`DIK_CAPITAL`) and the seven codes that older SDKs omit, which `KeyDefs.h` used to `#define`
itself.

On Windows nothing about the include graph changes: `KeyScanCodes.h` still includes `<dinput.h>`
under `_WIN32`, and each constant is checked against its `DIK_` counterpart with
`STATIC_ASSERT_ALWAYS`, which is enforced on VC6 too. If a future SDK disagrees with the table the
Windows build fails instead of silently remapping a key. The seven optional codes are asserted only
where the SDK defines them.

Three translation units — `GadgetListBox.cpp`, `GadgetTextEntry.cpp`, `KeyboardOptionsMenu.cpp` —
were getting `windows.h` by accident through this chain and use `GetDoubleClickTime` / `VK_RETURN`.
They now include `<windows.h>` themselves, with the same marker comment the `PreRTS.h` surgery
used. That is a pre-existing Win32 dependency made visible, not a new one.

## Probe effect

| Mode | Before | After |
|---|---:|---:|
| Native (no Windows SDK) | 489 / 737 | **541 / 737** |
| — `Core/GameEngine` | 107 / 207 | 137 / 207 |
| — `GeneralsMD/Code/GameEngine` | 267 / 379 | 289 / 379 |
| Shimmed (Win32 headers stubbed) | 638 / 737 | 638 / 737 |

+52 natively. The shimmed control is unchanged, which is the expected result: removing an include
cannot make the engine's own C++ more portable, and if that number had moved something else would
have broken.

52 of the 82 `dinput.h` casualties now compile; the other 30 were failing for a second reason as
well and still fail on it: 13 on GameSpy's un-vendored `gscommon.h`, 8 on the pre-existing
`sizeof(LANMessage) <= MAX_LANAPI_PACKET_SIZE` static-assert (a genuine 64-bit layout bug, and the
kind of thing this probe is for), 3 on a `windows.h` they ask for on purpose, and 6 on assorted
single items (`itoa`, `HKL`, a `pause` redefinition).

## What this does *not* do

- It does not port input. `Win32DIKeyboard`/`Win32DIMouse` still talk to DirectInput, still live in
  `Core/GameEngineDevice/Source/Win32Device`, and a native build still has no keyboard or mouse
  device behind `Keyboard`/`Mouse`. What it removes is the reason the rest of the engine had to
  know about DirectInput at all.
- It does not change any key code, binding or INI value. Same numbers, asserted equal to the SDK's
  on Windows.
- Set-1 scan codes are a PC/AT concept. A macOS/Linux input backend will have to translate from
  whatever it receives (HID usage codes, X11 keycodes, SDL scan codes) into this table; the table
  is now the thing to translate *to*, instead of DirectInput being the thing to reimplement.
