# Native 64-bit clang probe report

Produced by `scripts/native-port-probe.py`. Every number in this document is a count of real
`clang++` diagnostics, not an estimate.

> The counts below predate the `PreRTS.h` surgery and are kept as the historical baseline. For
> current numbers, including the renderer/device/`Main` targets this report excludes, see
> [`prerts-win32-surgery.md`](prerts-win32-surgery.md).

```
clang++ -fsyntax-only -std=c++20 -m64 -ferror-limit=0 -fms-extensions \
        -include Utility/CppMacros.h -DWIN32_LEAN_AND_MEAN -D_REENTRANT
```

Ubuntu clang 14.0.0, no Wine, no MSVC, no Windows SDK. Vendor headers that the CMake build fetches
(dx8, gamespy, miles, lzhl) are taken from `build/docker/_deps`; nothing else stands in for the
platform.

## Scope

733 translation units across nine targets. The `.cpp` list for each GameEngine target is parsed out
of its `CMakeLists.txt`, so the probe measures the translation units the build actually enables
rather than everything on disk (this is why ZH GameEngine is 379 TUs here and not the 757 files the
scoping doc counted).

`Core/Libraries/Source` is deliberately **not** an include directory. Its `debug/` and `profile/`
subdirectories shadow libstdc++'s internal `<debug/...>` and `<profile/...>` headers; each library
gets its own include path instead.

## The two modes, and why there are two

**Native** — nothing replaces the Windows SDK. This measures what compiles today on a machine with
no Win32 headers.

**Shimmed** (`--with-shims`) — `scripts/native-port-shims/` supplies declaration-only stand-ins for
the ~30 Win32 headers `PreRTS.h` pulls into every GameEngine translation unit. Nothing is defined,
only declared, so this measures the engine's *own* C++ rather than the absence of `windows.h`.

The shims are a **measurement instrument, not a port.** They do not link and are not intended to.
They exist so that "this file needs `windows.h`" and "this file contains code that will not compile
as 64-bit C++" can be counted separately, because the two have very different costs.

## Verdict on the platform-clean hypothesis: refuted

The hypothesis was that the game engine is almost entirely platform-clean, on the grounds that only
one file includes `windows.h`, one references `HWND`, and two reference d3d.

Measured natively, **0 of 586 GameEngine translation units compile** — 0/207 in `Core/GameEngine`
and 0/379 in `GeneralsMD/Code/GameEngine`. Not "almost all clean": none.

The grep was accurate but the inference from it was not. Every GameEngine `.cpp` is compiled with a
forced-include of `PreRTS.h`, which includes `windows.h`, `atlbase.h`, `imagehlp.h`, `dinput.h` and
around thirty more Win32 headers. One file including `windows.h` is enough when every file includes
that file. Grepping for direct `#include <windows.h>` lines cannot see this; only compiling can.

The corrected picture from the shimmed run is more encouraging than the native one and still worth
stating plainly: with the Win32 header layer replaced by declarations, **514 of 586 GameEngine
translation units compile clean** as 64-bit C++ (169/207 Core, 345/379 ZH). So the engine's own code
is largely portable — but reaching that state requires building a real Win32 compatibility layer
first, which is work the hypothesis assumed away. The estimate should carry that layer as an
explicit work item, not as a rounding error.

## Headline numbers

| | Native | Shimmed |
|---|---:|---:|
| Clean translation units | **107 / 733** (14%) | **629 / 733** (85%) |
| Translation units with errors | 626 | 104 |
| Total errors | 697 | 470 |

Native's 697 errors are mostly one-per-file `'atlbase.h' file not found`: the compiler stops at the
first missing include, so a native run cannot see past the header wall. The shimmed run has *fewer*
clean-blocking files but *more* interesting errors, because it gets far enough to type-check.

## Per-target breakdown

| Target | Native clean | Shimmed clean | Total |
|---|---:|---:|---:|
| Core/Libraries/Source/Compression | 11 | 11 | 11 |
| Core/Libraries/Source/WWVegas/WWMath | 33 | 33 | 35 |
| Core/Libraries/Source/WWVegas/WWLib | 49 | 49 | 60 |
| Core/Libraries/Source/WWVegas/WWDebug | 2 | 2 | 3 |
| Core/Libraries/Source/WWVegas/WWSaveLoad | 11 | 12 | 12 |
| Core/Libraries/Source/debug | 1 | 4 | 20 |
| Core/Libraries/Source/profile | 0 | 4 | 6 |
| Core/GameEngine | 0 | 169 | 207 |
| GeneralsMD/Code/GameEngine | 0 | 345 | 379 |
| **Total** | **107** | **629** | **733** |

`Core/Libraries` is unchanged between modes because it does not force-include `PreRTS.h`; its 41
failures are genuine platform code (`mpu.cpp`, `mutex.cpp`, `thread.cpp`, `registry.cpp`), which
sibling work owns.

## What the shimmed failures actually are

| Category | Errors | Files |
|---|---:|---:|
| Non-conforming template/name lookup | 139 | 27 |
| Win32 / MSVC identifiers undeclared | 136 | 21 |
| Other | 121 | 35 |
| Missing project/vendor headers | 41 | 41 |
| Win32 types undeclared | 14 | 5 |
| 64-bit size/layout assumptions | 14 | 14 |
| Missing generated headers | 4 | 4 |
| Inline x86 assembly | 1 | 1 |

Reading these by cause rather than by category:

- **GameSpy (41 files)** — `'gscommon.h' file not found`. Online matchmaking is out of scope per the
  plan doc; these translation units need stubbing out or excluding, not porting.
- **64-bit layout (14 files)** — all the same diagnostic:
  `static_assert failed ... sizeof(LANMessage) <= MAX_LANAPI_PACKET_SIZE`. A single struct whose
  embedded pointers grew from 4 to 8 bytes pushed it past its wire-packet budget. This is exactly
  the class of bug the probe was built to find, and it is one fix, not fourteen.
- **Win32 API calls (~48 files)** — Winsock (`WSACleanup`, `closesocket`, `WSADATA`, `HOSTENT`),
  `GMEM_FIXED`/`GlobalAlloc`, `FormatMessageW`, `GetDateFormatW`, `RemoveFontResource`,
  `GetKeyboardLayout`, `_stricmp`, `_fpreset`. Real work, but mechanical and mostly shared.
- **Inline x86 assembly (1 file)** — the only one left in the engine proper.
- **Structured exception handling (3 TUs)** — `WWLib/Except.cpp`, `WWLib/thread.cpp`,
  `Common/System/MiniDumper.cpp`. Not shimmed on purpose: libstdc++ defines `__try`/`__catch` as
  macros over real `try`/`catch`, so defining SEH macros would rewrite every `try` block in the
  standard library and bury the measurement under tens of thousands of spurious errors. SEH is a
  genuine port problem and is reported as one.

## Reproducing

```sh
# native, no Windows SDK
python3 scripts/native-port-probe.py --report /tmp/native.md

# engine-only C++, Win32 headers declaration-shimmed
python3 scripts/native-port-probe.py --with-shims --report /tmp/shimmed.md
```

The tables below are the generated output of those two runs; the analysis above is written around
them.

`--deps-dir` overrides where the fetched vendor headers are found (default `build/docker/_deps`);
`--jobs` sets parallelism.

## Shimmed run: errors by category

| Category | Errors | Files | Example |
|---|---:|---:|---|
| Non-conforming template/name lookup | 139 | 27 | `Core/Libraries/Source/WWVegas/WWLib/DbgHelpLoader.cpp: no member named 'GetSystemDirectoryA' in the global nam` |
| Win32 / MSVC identifiers undeclared | 136 | 21 | `Core/Libraries/Source/WWVegas/WWLib/DbgHelpGuard.cpp: use of undeclared identifier 'GMEM_FIXED'` |
| Other | 121 | 35 | `Core/Libraries/Source/WWVegas/WWMath/matrix3d.cpp: unknown type name 'LONG'; did you mean 'ULONG'?` |
| Missing project/vendor headers | 41 | 41 | `Core/Libraries/Source/WWVegas/WWLib/WWCOMUtil.cpp: 'oaidl.h' file not found` |
| Win32 types undeclared | 14 | 5 | `Core/Libraries/Source/WWVegas/WWMath/matrix3d.cpp: unknown type name 'HWND'` |
| 64-bit size/layout assumptions | 14 | 14 | `Core/GameEngine/Source/GameNetwork/ConnectionManager.cpp: static_assert failed due to requirement 'sizeof(LANM` |
| Missing generated headers (IDL / build-time) | 4 | 4 | `Core/GameEngine/Source/Common/INI/INIWebpageURL.cpp: 'EABrowserDispatch/BrowserDispatch.h' file not found` |
| Inline x86 assembly | 1 | 1 | `Core/Libraries/Source/debug/debug_debug.cpp: "Unsupported compiler or architecture for inline assembly"` |

## Shimmed run: every failing translation unit

| Translation unit | Errors | First diagnostic |
|---|---:|---|
| `Core/Libraries/Source/WWVegas/WWLib/registry.cpp` | 67 | `unknown type name 'HKEY'` |
| `Core/Libraries/Source/debug/netserv/netserv.cpp` | 46 | `use of undeclared identifier 'AllocConsole'` |
| `Core/Libraries/Source/debug/debug_io_con.cpp` | 35 | `use of undeclared identifier 'AllocConsole'` |
| `Core/Libraries/Source/debug/test2/test2.cpp` | 27 | `unknown type name 'HACCEL'` |
| `Core/Libraries/Source/WWVegas/WWLib/mpu.cpp` | 19 | `unknown type name 'LARGE_INTEGER'` |
| `Core/Libraries/Source/WWVegas/WWLib/DbgHelpLoader.cpp` | 17 | `use of undeclared identifier 'GMEM_FIXED'` |
| `Core/Libraries/Source/debug/debug_debug.cpp` | 17 | `"Unsupported compiler or platform. This code requires MSVC or GCC/MinGW-w64 targeting Windows."` |
| `Core/Libraries/Source/debug/debug_stack.cpp` | 16 | `unknown type name 'PREAD_PROCESS_MEMORY_ROUTINE'` |
| `Core/GameEngine/Source/Common/WorkerProcess.cpp` | 14 | `use of undeclared identifier 'CreatePipe'` |
| `Core/Libraries/Source/WWVegas/WWLib/mutex.cpp` | 14 | `use of undeclared identifier 'CreateMutex'` |
| `Core/Libraries/Source/WWVegas/WWLib/thread.cpp` | 11 | `use of undeclared identifier '_beginthread'` |
| `Core/Libraries/Source/WWVegas/WWMath/matrix3d.cpp` | 11 | `unknown type name 'LONG'; did you mean 'ULONG'?` |
| `Core/Libraries/Source/WWVegas/WWMath/matrix4.cpp` | 11 | `unknown type name 'LONG'; did you mean 'ULONG'?` |
| `Core/Libraries/Source/WWVegas/WWLib/DbgHelpGuard.cpp` | 9 | `use of undeclared identifier 'GMEM_FIXED'` |
| `Core/Libraries/Source/debug/debug_io_net.cpp` | 9 | `use of undeclared identifier 'PIPE_READMODE_MESSAGE'` |
| `Core/GameEngine/Source/Common/System/Debug.cpp` | 6 | `use of undeclared identifier 'g_LastErrorDump'` |
| `Core/GameEngine/Source/Common/System/GameMemoryNull.cpp` | 6 | `redefinition of 'DynamicMemoryAllocator'` |
| `Core/GameEngine/Source/GameNetwork/IPEnumeration.cpp` | 6 | `use of undeclared identifier 'WSACleanup'` |
| `Core/GameEngine/Source/GameNetwork/udp.cpp` | 6 | `use of undeclared identifier 'closesocket'` |
| `Core/Libraries/Source/debug/debug_internal.cpp` | 6 | `use of undeclared identifier 'MB_ICONSTOP'` |
| `Core/Libraries/Source/WWVegas/WWLib/rcfile.cpp` | 5 | `unknown type name 'HMODULE'` |
| `Core/Libraries/Source/debug/debug_io_flat.cpp` | 5 | `use of undeclared identifier 'FILE_FLAG_WRITE_THROUGH'` |
| `Core/Libraries/Source/profile/profile.cpp` | 5 | `use of undeclared identifier 'GMEM_FIXED'` |
| `Core/GameEngine/Source/GameNetwork/GameSpy/Thread/GameResultsThread.cpp` | 4 | `unknown type name 'HOSTENT'` |
| `Core/GameEngine/Source/GameNetwork/Transport.cpp` | 4 | `unknown type name 'WSADATA'` |
| `GeneralsMD/Code/GameEngine/Source/Common/System/SaveGame/GameState.cpp` | 4 | `use of undeclared identifier 'GetDateFormatW'; did you mean 'GetDateFormatA'?` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/PopupReplay.cpp` | 4 | `use of undeclared identifier 'FormatMessageW'; did you mean 'FormatMessageA'?` |
| `Core/GameEngine/Source/Common/ReplaySimulation.cpp` | 2 | `use of undeclared identifier 'GetModuleFileNameW'; did you mean 'GetModuleFileNameA'?` |
| `Core/GameEngine/Source/Common/System/GameMemory.cpp` | 2 | `use of undeclared identifier 'GMEM_FIXED'` |
| `Core/GameEngine/Source/GameClient/GlobalLanguage.cpp` | 2 | `use of undeclared identifier 'RemoveFontResource'` |
| `Core/GameEngine/Source/GameClient/VideoStream.cpp` | 2 | `use of undeclared identifier '_stricmp'; did you mean 'strcmp'?` |
| `Core/Libraries/Source/WWVegas/WWLib/verchk.cpp` | 2 | `unknown type name 'VS_FIXEDFILEINFO'` |
| `GeneralsMD/Code/GameEngine/Source/Common/GlobalData.cpp` | 2 | `use of undeclared identifier 'FOLDERID_Documents'` |
| `GeneralsMD/Code/GameEngine/Source/Common/Recorder.cpp` | 2 | `static_assert failed due to requirement 'sizeof(LANMessage) <= MAX_LANAPI_PACKET_SIZE' "LANMessage s` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/InGamePopupMessage.cpp` | 2 | `redefinition of 'pause' as different kind of symbol` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/ReplayMenu.cpp` | 2 | `use of undeclared identifier 'FormatMessageW'; did you mean 'FormatMessageA'?` |
| `Core/GameEngine/Source/Common/Diagnostic/SimulationMathCrc.cpp` | 1 | `use of undeclared identifier '_fpreset'` |
| `Core/GameEngine/Source/Common/INI/INIWebpageURL.cpp` | 1 | `'EABrowserDispatch/BrowserDispatch.h' file not found` |
| `Core/GameEngine/Source/Common/UserPreferences.cpp` | 1 | `'gscommon.h' file not found` |
| `Core/GameEngine/Source/GameClient/GUI/GameWindowGlobal.cpp` | 1 | `use of undeclared identifier 'iswascii'; did you mean 'isascii'?` |
| `Core/GameEngine/Source/GameClient/GUI/IMEManager.cpp` | 1 | `'mbstring.h' file not found` |
| `Core/GameEngine/Source/GameClient/GUI/LoadScreen.cpp` | 1 | `'gscommon.h' file not found` |
| `Core/GameEngine/Source/GameClient/Input/Keyboard.cpp` | 1 | `use of undeclared identifier 'GetKeyboardLayout'` |
| `Core/GameEngine/Source/GameClient/MessageStream/CommandXlat.cpp` | 1 | `'gscommon.h' file not found` |
| `Core/GameEngine/Source/GameNetwork/ConnectionManager.cpp` | 1 | `static_assert failed due to requirement 'sizeof(LANMessage) <= MAX_LANAPI_PACKET_SIZE' "LANMessage s` |
| `Core/GameEngine/Source/GameNetwork/GameInfo.cpp` | 1 | `static_assert failed due to requirement 'sizeof(LANMessage) <= MAX_LANAPI_PACKET_SIZE' "LANMessage s` |
| `Core/GameEngine/Source/GameNetwork/GameSpy/Chat.cpp` | 1 | `'gscommon.h' file not found` |
| `Core/GameEngine/Source/GameNetwork/GameSpy/LadderDefs.cpp` | 1 | `'gscommon.h' file not found` |
| `Core/GameEngine/Source/GameNetwork/GameSpy/LobbyUtils.cpp` | 1 | `'gscommon.h' file not found` |
| `Core/GameEngine/Source/GameNetwork/GameSpy/MainMenuUtils.cpp` | 1 | `'gscommon.h' file not found` |
| `Core/GameEngine/Source/GameNetwork/GameSpy/PeerDefs.cpp` | 1 | `'gscommon.h' file not found` |
| `Core/GameEngine/Source/GameNetwork/GameSpy/StagingRoomGameInfo.cpp` | 1 | `'gscommon.h' file not found` |
| `Core/GameEngine/Source/GameNetwork/GameSpy/Thread/BuddyThread.cpp` | 1 | `'gscommon.h' file not found` |
| `Core/GameEngine/Source/GameNetwork/GameSpy/Thread/PeerThread.cpp` | 1 | `'gscommon.h' file not found` |
| `Core/GameEngine/Source/GameNetwork/GameSpy/Thread/PersistentStorageThread.cpp` | 1 | `'gscommon.h' file not found` |
| `Core/GameEngine/Source/GameNetwork/GameSpy/Thread/PingThread.cpp` | 1 | `unknown type name 'HOSTENT'` |
| `Core/GameEngine/Source/GameNetwork/GameSpyOverlay.cpp` | 1 | `'gscommon.h' file not found` |
| `Core/GameEngine/Source/GameNetwork/LANAPI.cpp` | 1 | `static_assert failed due to requirement 'sizeof(LANMessage) <= MAX_LANAPI_PACKET_SIZE' "LANMessage s` |
| `Core/GameEngine/Source/GameNetwork/LANAPICallbacks.cpp` | 1 | `static_assert failed due to requirement 'sizeof(LANMessage) <= MAX_LANAPI_PACKET_SIZE' "LANMessage s` |
| `Core/GameEngine/Source/GameNetwork/LANAPIhandlers.cpp` | 1 | `static_assert failed due to requirement 'sizeof(LANMessage) <= MAX_LANAPI_PACKET_SIZE' "LANMessage s` |
| `Core/GameEngine/Source/GameNetwork/LANGameInfo.cpp` | 1 | `static_assert failed due to requirement 'sizeof(LANMessage) <= MAX_LANAPI_PACKET_SIZE' "LANMessage s` |
| `Core/GameEngine/Source/GameNetwork/NAT.cpp` | 1 | `'gscommon.h' file not found` |
| `Core/GameEngine/Source/GameNetwork/WOLBrowser/WebBrowser.cpp` | 1 | `'EABrowserDispatch/BrowserDispatch.h' file not found` |
| `Core/Libraries/Source/WWVegas/WWDebug/wwdebug.cpp` | 1 | `use of undeclared identifier 'FORMAT_MESSAGE_FROM_SYSTEM'` |
| `Core/Libraries/Source/WWVegas/WWLib/WWCOMUtil.cpp` | 1 | `'oaidl.h' file not found` |
| `Core/Libraries/Source/WWVegas/WWLib/regexpr.cpp` | 1 | `'gnu_regex.h' file not found` |
| `Core/Libraries/Source/WWVegas/WWLib/strtok_r.cpp` | 1 | `exception specification in declaration does not match previous declaration` |
| `Core/Libraries/Source/debug/debug_dlg/debug_dlg.cpp` | 1 | `'commctrl.h' file not found` |
| `Core/Libraries/Source/debug/debug_except.cpp` | 1 | `'commctrl.h' file not found` |
| `Core/Libraries/Source/debug/debug_purecall.cpp` | 1 | `'_pch.h' file not found` |
| `Core/Libraries/Source/debug/test1/test1.cpp` | 1 | `'main' must return 'int'` |
| `Core/Libraries/Source/debug/test3/test3.cpp` | 1 | `'main' must return 'int'` |
| `Core/Libraries/Source/debug/test4/test4.cpp` | 1 | `'main' must return 'int'` |
| `Core/Libraries/Source/debug/test5/test5.cpp` | 1 | `'main' must return 'int'` |
| `Core/Libraries/Source/debug/test6/test6.cpp` | 1 | `'main' must return 'int'` |
| `Core/Libraries/Source/profile/test1/test1.cpp` | 1 | `'main' must return 'int'` |
| `GeneralsMD/Code/GameEngine/Source/Common/GameEngine.cpp` | 1 | `'EABrowserDispatch/BrowserDispatch.h' file not found` |
| `GeneralsMD/Code/GameEngine/Source/Common/StatsCollector.cpp` | 1 | `static_assert failed due to requirement 'sizeof(LANMessage) <= MAX_LANAPI_PACKET_SIZE' "LANMessage s` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Diplomacy.cpp` | 1 | `'gscommon.h' file not found` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/GameInfoWindow.cpp` | 1 | `static_assert failed due to requirement 'sizeof(LANMessage) <= MAX_LANAPI_PACKET_SIZE' "LANMessage s` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/LanGameOptionsMenu.cpp` | 1 | `static_assert failed due to requirement 'sizeof(LANMessage) <= MAX_LANAPI_PACKET_SIZE' "LANMessage s` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/LanLobbyMenu.cpp` | 1 | `static_assert failed due to requirement 'sizeof(LANMessage) <= MAX_LANAPI_PACKET_SIZE' "LANMessage s` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/LanMapSelectMenu.cpp` | 1 | `static_assert failed due to requirement 'sizeof(LANMessage) <= MAX_LANAPI_PACKET_SIZE' "LANMessage s` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/MainMenu.cpp` | 1 | `'gscommon.h' file not found` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/NetworkDirectConnect.cpp` | 1 | `'gscommon.h' file not found` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/OptionsMenu.cpp` | 1 | `'gscommon.h' file not found` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/PopupHostGame.cpp` | 1 | `'gscommon.h' file not found` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/PopupJoinGame.cpp` | 1 | `'gscommon.h' file not found` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/PopupLadderSelect.cpp` | 1 | `'gscommon.h' file not found` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/PopupPlayerInfo.cpp` | 1 | `'gscommon.h' file not found` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/ScoreScreen.cpp` | 1 | `'gscommon.h' file not found` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/SkirmishMapSelectMenu.cpp` | 1 | `static_assert failed due to requirement 'sizeof(LANMessage) <= MAX_LANAPI_PACKET_SIZE' "LANMessage s` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/WOLBuddyOverlay.cpp` | 1 | `'gscommon.h' file not found` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/WOLGameSetupMenu.cpp` | 1 | `'gscommon.h' file not found` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/WOLLadderScreen.cpp` | 1 | `'EABrowserDispatch/BrowserDispatch.h' file not found` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/WOLLobbyMenu.cpp` | 1 | `'gscommon.h' file not found` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/WOLLocaleSelectPopup.cpp` | 1 | `'gscommon.h' file not found` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/WOLLoginMenu.cpp` | 1 | `'gscommon.h' file not found` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/WOLMapSelectMenu.cpp` | 1 | `'gscommon.h' file not found` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/WOLQuickMatchMenu.cpp` | 1 | `'gscommon.h' file not found` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/WOLWelcomeMenu.cpp` | 1 | `'gscommon.h' file not found` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/Shell/Shell.cpp` | 1 | `'gscommon.h' file not found` |
| `GeneralsMD/Code/GameEngine/Source/GameLogic/System/GameLogic.cpp` | 1 | `'gscommon.h' file not found` |
| `GeneralsMD/Code/GameEngine/Source/GameNetwork/GUIUtil.cpp` | 1 | `static_assert failed due to requirement 'sizeof(LANMessage) <= MAX_LANAPI_PACKET_SIZE' "LANMessage s` |

## Native run: errors by category

| Category | Errors | Files | Example |
|---|---:|---:|---|
| Missing Win32 headers | 611 | 611 | `Core/Libraries/Source/WWVegas/WWMath/matrix3d.cpp: 'objbase.h' file not found` |
| Other | 33 | 12 | `Core/Libraries/Source/WWVegas/WWMath/matrix3d.cpp: unknown type name 'LONG'; did you mean 'ULONG'?` |
| Non-conforming template/name lookup | 22 | 3 | `Core/Libraries/Source/WWVegas/WWLib/mpu.cpp: use of undeclared identifier 'GetCurrentProcess'` |
| Win32 types undeclared | 14 | 5 | `Core/Libraries/Source/WWVegas/WWMath/matrix3d.cpp: unknown type name 'HWND'` |
| Win32 / MSVC identifiers undeclared | 14 | 5 | `Core/Libraries/Source/WWVegas/WWLib/mpu.cpp: use of undeclared identifier 'REALTIME_PRIORITY_CLASS'` |
| Missing project/vendor headers | 3 | 3 | `Core/Libraries/Source/WWVegas/WWLib/WWCOMUtil.cpp: 'oaidl.h' file not found` |
