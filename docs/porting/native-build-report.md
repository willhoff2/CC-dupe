# Native 64-bit build — objects and link

Produced by `scripts/native-build.py`. Unlike every other number in `docs/porting/`,
these come from real object files and a real linker invocation, not from
`clang++ -fsyntax-only`.

Toolchain: `Ubuntu clang version 14.0.0-1ubuntu1.1`, target `x86_64-pc-linux-gnu`, levels built: 1, 2.

Mode: **shimmed** — `scripts/native-port-shims/` supplies declaration-only stand-ins for the Win32 headers, so a missing platform layer shows up as an undefined symbol rather than as a failed compile. That is the point: it moves the blockers from §1 to §3, where they can be counted individually.

## 1. Compilation

| Library | Objects produced | Translation units | Probe-clean |
|---|---:|---:|---:|
| `Core/Libraries/Source/Compression` | 11 | 11 | 11 |
| `Core/Libraries/Source/WWVegas/WWMath` | 35 | 35 | 35 |
| `Core/Libraries/Source/WWVegas/WWLib` | 59 | 66 | 59 |
| `Core/Libraries/Source/WWVegas/WWDebug` | 2 | 3 | 2 |
| `Core/Libraries/Source/WWVegas/WWSaveLoad` | 12 | 12 | 12 |
| `Core/GameEngine` | 186 | 209 | 186 |
| `GeneralsMD/Code/GameEngine` | 358 | 380 | 358 |
| **Total** | **663** | **716** | **663** |

53 translation units produced no object file:

| Translation unit | First diagnostic |
|---|---|
| `Core/GameEngine/Source/Common/Diagnostic/SimulationMathCrc.cpp` | `use of undeclared identifier '_fpreset'` |
| `Core/GameEngine/Source/Common/INI/INIWebpageURL.cpp` | `'EABrowserDispatch/BrowserDispatch.h' file not found` |
| `Core/GameEngine/Source/Common/ReplaySimulation.cpp` | `use of undeclared identifier 'GetModuleFileNameW'; did you mean 'GetModuleFileNameA'?` |
| `Core/GameEngine/Source/Common/System/Debug.cpp` | `unknown type name 'HWND'` |
| `Core/GameEngine/Source/Common/System/GameMemory.cpp` | `no member named 'GlobalAlloc' in the global namespace` |
| `Core/GameEngine/Source/Common/System/GameMemoryNull.cpp` | `redefinition of 'DynamicMemoryAllocator'` |
| `Core/GameEngine/Source/GameClient/GUI/GameWindowGlobal.cpp` | `use of undeclared identifier 'iswascii'; did you mean 'isascii'?` |
| `Core/GameEngine/Source/GameClient/GUI/IMEManager.cpp` | `'mbstring.h' file not found` |
| `Core/GameEngine/Source/GameClient/Input/Keyboard.cpp` | `unknown type name 'HKL'` |
| `Core/GameEngine/Source/GameNetwork/ConnectionManager.cpp` | `static_assert failed due to requirement 'sizeof(LANMessage) <= MAX_LANAPI_PACKET_SIZE' "LANMessage struct cannot be larg` |
| `Core/GameEngine/Source/GameNetwork/DownloadManager.cpp` | `unknown type name 'HRESULT'; did you mean 'MMRESULT'?` |
| `Core/GameEngine/Source/GameNetwork/GameInfo.cpp` | `static_assert failed due to requirement 'sizeof(LANMessage) <= MAX_LANAPI_PACKET_SIZE' "LANMessage struct cannot be larg` |
| `Core/GameEngine/Source/GameNetwork/GameSpy/MainMenuUtils.cpp` | `unknown type name 'HRESULT'; did you mean 'MMRESULT'?` |
| `Core/GameEngine/Source/GameNetwork/GameSpy/PeerDefs.cpp` | `use of undeclared identifier 'CreateDirectory'` |
| `Core/GameEngine/Source/GameNetwork/GameSpy/StagingRoomGameInfo.cpp` | `unknown type name 'AsnObjectIdentifier'` |
| `Core/GameEngine/Source/GameNetwork/GameSpy/Thread/GameResultsThread.cpp` | `unknown type name 'HOSTENT'` |
| `Core/GameEngine/Source/GameNetwork/GameSpy/Thread/PeerThread.cpp` | `no matching function for call to 'recvfrom'` |
| `Core/GameEngine/Source/GameNetwork/GameSpy/Thread/PingThread.cpp` | `unknown type name 'HOSTENT'` |
| `Core/GameEngine/Source/GameNetwork/LANAPI.cpp` | `static_assert failed due to requirement 'sizeof(LANMessage) <= MAX_LANAPI_PACKET_SIZE' "LANMessage struct cannot be larg` |
| `Core/GameEngine/Source/GameNetwork/LANAPICallbacks.cpp` | `static_assert failed due to requirement 'sizeof(LANMessage) <= MAX_LANAPI_PACKET_SIZE' "LANMessage struct cannot be larg` |
| `Core/GameEngine/Source/GameNetwork/LANAPIhandlers.cpp` | `static_assert failed due to requirement 'sizeof(LANMessage) <= MAX_LANAPI_PACKET_SIZE' "LANMessage struct cannot be larg` |
| `Core/GameEngine/Source/GameNetwork/LANGameInfo.cpp` | `static_assert failed due to requirement 'sizeof(LANMessage) <= MAX_LANAPI_PACKET_SIZE' "LANMessage struct cannot be larg` |
| `Core/GameEngine/Source/GameNetwork/WOLBrowser/WebBrowser.cpp` | `'EABrowserDispatch/BrowserDispatch.h' file not found` |
| `Core/Libraries/Source/WWVegas/WWDebug/wwdebug.cpp` | `use of undeclared identifier 'FORMAT_MESSAGE_FROM_SYSTEM'` |
| `Core/Libraries/Source/WWVegas/WWLib/DbgHelpGuard.cpp` | `use of undeclared identifier 'GMEM_FIXED'` |
| `Core/Libraries/Source/WWVegas/WWLib/DbgHelpLoader.cpp` | `use of undeclared identifier 'GMEM_FIXED'` |
| `Core/Libraries/Source/WWVegas/WWLib/WWCOMUtil.cpp` | `'oaidl.h' file not found` |
| `Core/Libraries/Source/WWVegas/WWLib/rcfile.cpp` | `unknown type name 'HMODULE'` |
| `Core/Libraries/Source/WWVegas/WWLib/regexpr.cpp` | `'gnu_regex.h' file not found` |
| `Core/Libraries/Source/WWVegas/WWLib/strtok_r.cpp` | `exception specification in declaration does not match previous declaration` |
| `Core/Libraries/Source/WWVegas/WWLib/verchk.cpp` | `unknown type name 'VS_FIXEDFILEINFO'` |
| `GeneralsMD/Code/GameEngine/Source/Common/GameEngine.cpp` | `'EABrowserDispatch/BrowserDispatch.h' file not found` |
| `GeneralsMD/Code/GameEngine/Source/Common/Recorder.cpp` | `static_assert failed due to requirement 'sizeof(LANMessage) <= MAX_LANAPI_PACKET_SIZE' "LANMessage struct cannot be larg` |
| `GeneralsMD/Code/GameEngine/Source/Common/StatsCollector.cpp` | `static_assert failed due to requirement 'sizeof(LANMessage) <= MAX_LANAPI_PACKET_SIZE' "LANMessage struct cannot be larg` |
| `GeneralsMD/Code/GameEngine/Source/Common/System/SaveGame/GameState.cpp` | `use of undeclared identifier 'GetDateFormatW'; did you mean 'GetDateFormatA'?` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/InGamePopupMessage.cpp` | `redefinition of 'pause' as different kind of symbol` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/DownloadMenu.cpp` | `unknown type name 'HRESULT'; did you mean 'MMRESULT'?` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/GameInfoWindow.cpp` | `static_assert failed due to requirement 'sizeof(LANMessage) <= MAX_LANAPI_PACKET_SIZE' "LANMessage struct cannot be larg` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/LanGameOptionsMenu.cpp` | `static_assert failed due to requirement 'sizeof(LANMessage) <= MAX_LANAPI_PACKET_SIZE' "LANMessage struct cannot be larg` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/LanLobbyMenu.cpp` | `static_assert failed due to requirement 'sizeof(LANMessage) <= MAX_LANAPI_PACKET_SIZE' "LANMessage struct cannot be larg` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/LanMapSelectMenu.cpp` | `static_assert failed due to requirement 'sizeof(LANMessage) <= MAX_LANAPI_PACKET_SIZE' "LANMessage struct cannot be larg` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/MainMenu.cpp` | `unknown type name 'HRESULT'; did you mean 'MMRESULT'?` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/NetworkDirectConnect.cpp` | `static_assert failed due to requirement 'sizeof(LANMessage) <= MAX_LANAPI_PACKET_SIZE' "LANMessage struct cannot be larg` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/PopupPlayerInfo.cpp` | `unknown type name 'OSVERSIONINFO'` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/PopupReplay.cpp` | `use of undeclared identifier 'DeleteFile'` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/ScoreScreen.cpp` | `static_assert failed due to requirement 'sizeof(LANMessage) <= MAX_LANAPI_PACKET_SIZE' "LANMessage struct cannot be larg` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/SkirmishMapSelectMenu.cpp` | `static_assert failed due to requirement 'sizeof(LANMessage) <= MAX_LANAPI_PACKET_SIZE' "LANMessage struct cannot be larg` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/WOLBuddyOverlay.cpp` | `use of undeclared identifier '_wtoi'` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/WOLLadderScreen.cpp` | `'EABrowserDispatch/BrowserDispatch.h' file not found` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/WOLLoginMenu.cpp` | `'EABrowserDispatch/BrowserDispatch.h' file not found` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/WOLWelcomeMenu.cpp` | `'EABrowserDispatch/BrowserDispatch.h' file not found` |
| `GeneralsMD/Code/GameEngine/Source/GameLogic/System/GameLogic.cpp` | `static_assert failed due to requirement 'sizeof(LANMessage) <= MAX_LANAPI_PACKET_SIZE' "LANMessage struct cannot be larg` |
| `GeneralsMD/Code/GameEngine/Source/GameNetwork/GUIUtil.cpp` | `static_assert failed due to requirement 'sizeof(LANMessage) <= MAX_LANAPI_PACKET_SIZE' "LANMessage struct cannot be larg` |

## 2. How much the probe over-reports

**0 translation units that the probe calls clean fail to compile**, out of 663 probe-clean units (0%). These are the codegen-class failures `-fsyntax-only` cannot see.

## 3. Undefined symbols

The 7 archives were linked into one binary with `--whole-archive` (binary produced: yes; clean link: yes). **522 distinct symbols are unresolved** once libc, libstdc++, libm, libpthread and the CRT/unwinder symbols are discounted. The full categorised list is in the JSON output; examples follow each count.

| Cause | Symbols |
|---|---:|
| Defined in a translation unit that failed to compile | 321 |
| Well-known Dict keys (instantiated in GameEngineDevice) | 103 |
| GameSpy | 18 |
| Defined in a layer not built here (renderer / audio / device / entry point) | 17 |
| Engine C++ not built at this level | 17 |
| Other / unclassified | 16 |
| Win32 API | 10 |
| Third-party library not linked (lzhl, zlib) | 9 |
| Generated gitinfo (build-time, not a blocker) | 6 |
| Direct3D 8 / DirectX | 5 |

### Defined in a translation unit that failed to compile

- `DX8Wrapper_IsWindowed`
- `ReplayWasPressed`
- `TheDynamicMemoryAllocator`
- `TheGameEngine`
- `TheGameInfo`
- `TheGameLogic`
- `TheGameSpyGame`
- `TheGameSpyInfo`
- `TheGameSpyPeerMessageQueue`
- `TheGameState`
- `TheIMEManager`
- `TheKeyboard`
- `TheMemoryPoolFactory`
- `ThePinger`
- `TheRankPointValues`
- …and 306 more

### Well-known Dict keys (instantiated in GameEngineDevice)

- `TheKey_InitialCameraPosition`
- `TheKey_mapName`
- `TheKey_multiplayerIsLocal`
- `TheKey_multiplayerStartIndex`
- `TheKey_objectAggressiveness`
- `TheKey_objectEnabled`
- `TheKey_objectGrantUpgrade`
- `TheKey_objectIndestructible`
- `TheKey_objectInitialHealth`
- `TheKey_objectMaxHPs`
- `TheKey_objectName`
- `TheKey_objectPowered`
- `TheKey_objectRecruitableAI`
- `TheKey_objectScriptAttachment`
- `TheKey_objectSelectable`
- …and 88 more

### GameSpy

- `gpAuthBuddyRequest`
- `gpConnectA`
- `gpConnectNewUserA`
- `gpDeleteBuddy`
- `gpDeleteProfile`
- `gpDenyBuddyRequest`
- `gpDestroy`
- `gpDisconnect`
- `gpGetBuddyStatus`
- `gpGetInfo`
- `gpInitialize`
- `gpIsConnected`
- `gpProcess`
- `gpSendBuddyMessageA`
- `gpSendBuddyRequestA`
- …and 3 more

### Defined in a layer not built here (renderer / audio / device / entry point)

- `DX8Wrapper_PreserveFPU`
- `TheProjectedShadowManager`
- `TheSmudgeManager`
- `MOTDSystem(GameWindow*, unsigned int, unsigned int, unsigned int)`
- `doSkyBoxSet(bool)`
- `CreateGameEngine()`
- `ReloadAllTextures()`
- `oversizeTheTerrain(int)`
- `testMinimumRequirements(ChipsetType*, CpuType*, int*, unsigned long long*, float*, float*, float*)`
- `GetUnsignedIntFromRegistry(std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >, unsigned int&)`
- `TextureFilterClass::getTextureFilterMode(char const*)`
- `WW3D::Get_Texture_Reduction()`
- `MapObject::getWaypointID()`
- `MapObject::getWaypointName()`
- `MapObject::getThingTemplate() const`
- …and 2 more

### Engine C++ not built at this level

- `SaveGameInfo::~SaveGameInfo()`
- `ConnectionManager::ConnectionManager()`
- `ConnectionManager::~ConnectionManager()`
- `GameSpyStagingRoom::GameSpyStagingRoom()`
- `TextureFilterClass::TextureFilterModeString`
- `WW3D::PreviousSyncTime`
- `WW3D::SyncTime`
- `GameInfo::GameInfo()`
- `GameSlot::GameSlot()`
- `MapObject::TheMapObjectListPtr`
- `MapObject::MapObject(Coord3D, AsciiString, float, int, Dict const*, ThingTemplate const*)`
- `typeinfo for GameInfo`
- `typeinfo for GameSlot`
- `vtable for SkirmishGameInfo`
- `vtable for GameSpyStagingRoom`
- …and 2 more

### Other / unclassified

- `CloseStatsConnection`
- `FreeGame`
- `GenerateAuthA`
- `GetChallenge`
- `GetPersistDataValuesA`
- `InitStatsConnection`
- `IsStatsConnected`
- `NewGame`
- `PersistThink`
- `PreAuthenticatePlayerCDA`
- `PreAuthenticatePlayerPM`
- `SendGameSnapShotA`
- `SetPersistDataValuesA`
- `gcd_gamename`
- `gcd_secret_key`
- …and 1 more

### Win32 API

- `DeleteFileA`
- `FreeLibrary`
- `GetCommandLineA`
- `GetDoubleClickTime`
- `GetLocalTime`
- `GetModuleFileNameA`
- `GetProcAddress`
- `GlobalMemoryStatus`
- `LoadLibraryA`
- `itoa`

### Third-party library not linked (lzhl, zlib)

- `LZHLCompress`
- `LZHLCompressorCalcMaxBuf`
- `LZHLCreateCompressor`
- `LZHLCreateDecompressor`
- `LZHLDecompress`
- `LZHLDestroyCompressor`
- `LZHLDestroyDecompressor`
- `compress2`
- `uncompress`

### Generated gitinfo (build-time, not a blocker)

- `GitCommitAuthorName`
- `GitCommitTimeStamp`
- `GitRevision`
- `GitShortSHA1`
- `GitTag`
- `GitUncommittedChanges`

### Direct3D 8 / DirectX

- `D3DXVec4Dot(D3DXVECTOR4 const*, D3DXVECTOR4 const*)`
- `D3DXVec4Transform(D3DXVECTOR4*, D3DXVECTOR4 const*, D3DXMATRIX const*)`
- `D3DXMATRIX::D3DXMATRIX(float, float, float, float, float, float, float, float, float, float, float, float, float, float, float, float)`
- `D3DXVECTOR4::D3DXVECTOR4(float, float, float, float)`
- `D3DXVECTOR4::D3DXVECTOR4()`

## Reproducing

```sh
bash scripts/ci/fetch-probe-deps.sh
python3 scripts/native-build.py --level 1 --level 2 --with-shims --report docs/porting/native-build-report.md --json native-build.json
```
