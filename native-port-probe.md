# Native 64-bit clang probe — shimmed

Compiled 744 translation units with `clang++ -fsyntax-only -std=c++20 -m64 -ferror-limit=0 -fms-extensions -include Utility/CppMacros.h -DWIN32_LEAN_AND_MEAN -D_REENTRANT` (no Windows SDK, no Wine, no MSVC).

Mode: **shimmed**. `scripts/native-port-shims/` supplies declaration-only stand-ins for the Win32 headers `PreRTS.h` pulls into every GameEngine translation unit, so the numbers below measure the engine's *own* C++ rather than the absence of `windows.h`.

Fetched SDK headers (dx8, gamespy, miles, lzhl) taken from `/home/ubuntu/repos/CC-dupe/build/docker/_deps`: dx8-src, gamespy-src/include, lzhl-src, miles-src, miles-src/mss.

- Translation units that compile clean: **654 / 744** (87%)
- Translation units with errors: **90**
- Total errors: **379**

## Errors by category

| Category | Errors | Files | Example |
|---|---:|---:|---|
| Win32 / MSVC identifiers undeclared | 86 | 15 | `Core/Libraries/Source/WWVegas/WWLib/DbgHelpGuard.cpp: use of undeclared identifier 'GMEM_FIXED'` |
| Other | 81 | 25 | `Core/Libraries/Source/WWVegas/WWLib/DbgHelpGuard.cpp: unknown type name 'PREAD_PROCESS_MEMORY_ROUTINE'` |
| Win32 types undeclared | 79 | 3 | `Core/Libraries/Source/WWVegas/WWLib/rcfile.cpp: unknown type name 'HMODULE'` |
| Non-conforming template/name lookup | 73 | 18 | `Core/Libraries/Source/WWVegas/WWLib/DbgHelpLoader.cpp: no member named 'GetSystemDirectoryA' in the global nam` |
| Missing project/vendor headers | 41 | 41 | `Core/Libraries/Source/WWVegas/WWLib/WWCOMUtil.cpp: 'oaidl.h' file not found` |
| 64-bit size/layout assumptions | 14 | 14 | `Core/GameEngine/Source/GameNetwork/ConnectionManager.cpp: static_assert failed due to requirement 'sizeof(LANM` |
| Missing generated headers (IDL / build-time) | 4 | 4 | `Core/GameEngine/Source/Common/INI/INIWebpageURL.cpp: 'EABrowserDispatch/BrowserDispatch.h' file not found` |
| Inline x86 assembly | 1 | 1 | `Core/Libraries/Source/debug/debug_debug.cpp: "Unsupported compiler or architecture for inline assembly"` |

## Per-target breakdown

| Target | Clean | Total | Clean % |
|---|---:|---:|---:|
| Core/Libraries/Source/Compression | 11 | 11 | 100% |
| Core/Libraries/Source/WWVegas/WWMath | 35 | 35 | 100% |
| Core/Libraries/Source/WWVegas/WWLib | 60 | 67 | 89% |
| Core/Libraries/Source/WWVegas/WWDebug | 2 | 3 | 66% |
| Core/Libraries/Source/WWVegas/WWSaveLoad | 12 | 12 | 100% |
| Core/Libraries/Source/debug | 4 | 20 | 20% |
| Core/Libraries/Source/profile | 4 | 6 | 66% |
| Core/GameEngine | 179 | 210 | 85% |
| GeneralsMD/Code/GameEngine | 347 | 380 | 91% |

## Translation units with errors

| Translation unit | Errors | First diagnostic |
|---|---:|---|
| `Core/Libraries/Source/debug/netserv/netserv.cpp` | 46 | `use of undeclared identifier 'AllocConsole'` |
| `Core/GameEngine/Source/GameNetwork/DownloadManager.cpp` | 42 | `unknown type name 'HRESULT'; did you mean 'MMRESULT'?` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/DownloadMenu.cpp` | 42 | `unknown type name 'HRESULT'; did you mean 'MMRESULT'?` |
| `Core/Libraries/Source/debug/debug_io_con.cpp` | 35 | `use of undeclared identifier 'AllocConsole'` |
| `Core/Libraries/Source/debug/test2/test2.cpp` | 27 | `unknown type name 'HACCEL'` |
| `Core/Libraries/Source/WWVegas/WWLib/DbgHelpLoader.cpp` | 17 | `use of undeclared identifier 'GMEM_FIXED'` |
| `Core/Libraries/Source/debug/debug_debug.cpp` | 16 | `"Unsupported compiler or platform. This code requires MSVC or GCC/MinGW-w64 targeting Windows."` |
| `Core/Libraries/Source/debug/debug_stack.cpp` | 16 | `unknown type name 'PREAD_PROCESS_MEMORY_ROUTINE'` |
| `Core/Libraries/Source/WWVegas/WWLib/DbgHelpGuard.cpp` | 9 | `use of undeclared identifier 'GMEM_FIXED'` |
| `Core/Libraries/Source/debug/debug_io_net.cpp` | 9 | `use of undeclared identifier 'PIPE_READMODE_MESSAGE'` |
| `Core/GameEngine/Source/Common/System/GameMemoryNull.cpp` | 6 | `redefinition of 'DynamicMemoryAllocator'` |
| `Core/Libraries/Source/debug/debug_internal.cpp` | 6 | `use of undeclared identifier 'MB_ICONSTOP'` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/PopupReplay.cpp` | 6 | `use of undeclared identifier 'DeleteFile'` |
| `Core/Libraries/Source/WWVegas/WWLib/rcfile.cpp` | 5 | `unknown type name 'HMODULE'` |
| `Core/Libraries/Source/debug/debug_io_flat.cpp` | 5 | `use of undeclared identifier 'FILE_FLAG_WRITE_THROUGH'` |
| `Core/Libraries/Source/profile/profile.cpp` | 5 | `use of undeclared identifier 'GMEM_FIXED'` |
| `Core/GameEngine/Source/GameNetwork/GameSpy/Thread/GameResultsThread.cpp` | 4 | `unknown type name 'HOSTENT'` |
| `GeneralsMD/Code/GameEngine/Source/Common/System/SaveGame/GameState.cpp` | 4 | `use of undeclared identifier 'GetDateFormatW'; did you mean 'GetDateFormatA'?` |
| `Core/GameEngine/Source/Common/System/GameMemory.cpp` | 3 | `no member named 'GlobalAlloc' in the global namespace` |
| `Core/GameEngine/Source/Common/ReplaySimulation.cpp` | 2 | `use of undeclared identifier 'GetModuleFileNameW'; did you mean 'GetModuleFileNameA'?` |
| `Core/GameEngine/Source/GameNetwork/GameSpy/Thread/PingThread.cpp` | 2 | `unknown type name 'HOSTENT'` |
| `Core/Libraries/Source/WWVegas/WWLib/verchk.cpp` | 2 | `unknown type name 'VS_FIXEDFILEINFO'` |
| `GeneralsMD/Code/GameEngine/Source/Common/Recorder.cpp` | 2 | `static_assert failed due to requirement 'sizeof(LANMessage) <= MAX_LANAPI_PACKET_SIZE' "LANMessage s` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/InGamePopupMessage.cpp` | 2 | `redefinition of 'pause' as different kind of symbol` |
| `Core/GameEngine/Source/Common/Diagnostic/SimulationMathCrc.cpp` | 1 | `use of undeclared identifier '_fpreset'` |
| `Core/GameEngine/Source/Common/INI/INIWebpageURL.cpp` | 1 | `'EABrowserDispatch/BrowserDispatch.h' file not found` |
| `Core/GameEngine/Source/Common/UserPreferences.cpp` | 1 | `'gscommon.h' file not found` |
| `Core/GameEngine/Source/GameClient/GUI/GameWindowGlobal.cpp` | 1 | `use of undeclared identifier 'iswascii'; did you mean 'isascii'?` |
| `Core/GameEngine/Source/GameClient/GUI/IMEManager.cpp` | 1 | `'mbstring.h' file not found` |
| `Core/GameEngine/Source/GameClient/GUI/LoadScreen.cpp` | 1 | `'gscommon.h' file not found` |
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
