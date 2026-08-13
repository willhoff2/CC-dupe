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
| `Core/Libraries/Source/WWVegas/WWLib` | 66 | 66 | 66 |
| `Core/Libraries/Source/WWVegas/WWDebug` | 3 | 3 | 3 |
| `Core/Libraries/Source/WWVegas/WWSaveLoad` | 12 | 12 | 12 |
| `Core/GameEngine` | 193 | 208 | 193 |
| `GeneralsMD/Code/GameEngine` | 366 | 380 | 366 |
| **Total** | **686** | **715** | **686** |

29 translation units produced no object file:

| Translation unit | First diagnostic |
|---|---|
| `Core/GameEngine/Source/Common/System/Debug.cpp` | `unknown type name 'HWND'` |
| `Core/GameEngine/Source/GameClient/GUI/IMEManager.cpp` | `unknown type name 'HWND'` |
| `Core/GameEngine/Source/GameClient/Input/Keyboard.cpp` | `unknown type name 'HKL'` |
| `Core/GameEngine/Source/GameNetwork/ConnectionManager.cpp` | `static_assert failed due to requirement 'sizeof(LANMessage) <= MAX_LANAPI_PACKET_SIZE' "LANMessage struct cannot be larg` |
| `Core/GameEngine/Source/GameNetwork/DownloadManager.cpp` | `unknown type name 'HRESULT'; did you mean 'MMRESULT'?` |
| `Core/GameEngine/Source/GameNetwork/GameInfo.cpp` | `static_assert failed due to requirement 'sizeof(LANMessage) <= MAX_LANAPI_PACKET_SIZE' "LANMessage struct cannot be larg` |
| `Core/GameEngine/Source/GameNetwork/GameSpy/MainMenuUtils.cpp` | `unknown type name 'HRESULT'; did you mean 'MMRESULT'?` |
| `Core/GameEngine/Source/GameNetwork/GameSpy/StagingRoomGameInfo.cpp` | `unknown type name 'AsnObjectIdentifier'` |
| `Core/GameEngine/Source/GameNetwork/GameSpy/Thread/GameResultsThread.cpp` | `unknown type name 'HOSTENT'` |
| `Core/GameEngine/Source/GameNetwork/GameSpy/Thread/PeerThread.cpp` | `no matching function for call to 'recvfrom'` |
| `Core/GameEngine/Source/GameNetwork/GameSpy/Thread/PingThread.cpp` | `unknown type name 'HOSTENT'` |
| `Core/GameEngine/Source/GameNetwork/LANAPI.cpp` | `static_assert failed due to requirement 'sizeof(LANMessage) <= MAX_LANAPI_PACKET_SIZE' "LANMessage struct cannot be larg` |
| `Core/GameEngine/Source/GameNetwork/LANAPICallbacks.cpp` | `static_assert failed due to requirement 'sizeof(LANMessage) <= MAX_LANAPI_PACKET_SIZE' "LANMessage struct cannot be larg` |
| `Core/GameEngine/Source/GameNetwork/LANAPIhandlers.cpp` | `static_assert failed due to requirement 'sizeof(LANMessage) <= MAX_LANAPI_PACKET_SIZE' "LANMessage struct cannot be larg` |
| `Core/GameEngine/Source/GameNetwork/LANGameInfo.cpp` | `static_assert failed due to requirement 'sizeof(LANMessage) <= MAX_LANAPI_PACKET_SIZE' "LANMessage struct cannot be larg` |
| `GeneralsMD/Code/GameEngine/Source/Common/GameEngine.cpp` | `static_assert failed due to requirement 'sizeof(LANMessage) <= MAX_LANAPI_PACKET_SIZE' "LANMessage struct cannot be larg` |
| `GeneralsMD/Code/GameEngine/Source/Common/Recorder.cpp` | `static_assert failed due to requirement 'sizeof(LANMessage) <= MAX_LANAPI_PACKET_SIZE' "LANMessage struct cannot be larg` |
| `GeneralsMD/Code/GameEngine/Source/Common/StatsCollector.cpp` | `static_assert failed due to requirement 'sizeof(LANMessage) <= MAX_LANAPI_PACKET_SIZE' "LANMessage struct cannot be larg` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/DownloadMenu.cpp` | `unknown type name 'HRESULT'; did you mean 'MMRESULT'?` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/GameInfoWindow.cpp` | `static_assert failed due to requirement 'sizeof(LANMessage) <= MAX_LANAPI_PACKET_SIZE' "LANMessage struct cannot be larg` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/LanGameOptionsMenu.cpp` | `static_assert failed due to requirement 'sizeof(LANMessage) <= MAX_LANAPI_PACKET_SIZE' "LANMessage struct cannot be larg` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/LanLobbyMenu.cpp` | `static_assert failed due to requirement 'sizeof(LANMessage) <= MAX_LANAPI_PACKET_SIZE' "LANMessage struct cannot be larg` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/LanMapSelectMenu.cpp` | `static_assert failed due to requirement 'sizeof(LANMessage) <= MAX_LANAPI_PACKET_SIZE' "LANMessage struct cannot be larg` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/MainMenu.cpp` | `unknown type name 'HRESULT'; did you mean 'MMRESULT'?` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/NetworkDirectConnect.cpp` | `static_assert failed due to requirement 'sizeof(LANMessage) <= MAX_LANAPI_PACKET_SIZE' "LANMessage struct cannot be larg` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/ScoreScreen.cpp` | `static_assert failed due to requirement 'sizeof(LANMessage) <= MAX_LANAPI_PACKET_SIZE' "LANMessage struct cannot be larg` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/SkirmishMapSelectMenu.cpp` | `static_assert failed due to requirement 'sizeof(LANMessage) <= MAX_LANAPI_PACKET_SIZE' "LANMessage struct cannot be larg` |
| `GeneralsMD/Code/GameEngine/Source/GameLogic/System/GameLogic.cpp` | `static_assert failed due to requirement 'sizeof(LANMessage) <= MAX_LANAPI_PACKET_SIZE' "LANMessage struct cannot be larg` |
| `GeneralsMD/Code/GameEngine/Source/GameNetwork/GUIUtil.cpp` | `static_assert failed due to requirement 'sizeof(LANMessage) <= MAX_LANAPI_PACKET_SIZE' "LANMessage struct cannot be larg` |

## 2. How much the probe over-reports

**0 translation units that the probe calls clean fail to compile**, out of 686 probe-clean units (0%). These are the codegen-class failures `-fsyntax-only` cannot see.

## 3. Undefined symbols

The 7 archives were linked into one binary with `--whole-archive` (binary produced: yes; clean link: yes). **469 distinct symbols are unresolved** once libc, libstdc++, libm, libpthread and the CRT/unwinder symbols are discounted. The full categorised list is in the JSON output; examples follow each count.

| Cause | Symbols |
|---|---:|
| Defined in a translation unit that failed to compile | 231 |
| Well-known Dict keys (instantiated in GameEngineDevice) | 103 |
| Win32 API | 43 |
| Other / unclassified | 18 |
| Defined in a layer not built here (renderer / audio / device / entry point) | 18 |
| Engine C++ not built at this level | 18 |
| GameSpy | 18 |
| Third-party library not linked (lzhl, zlib) | 9 |
| Generated gitinfo (build-time, not a blocker) | 6 |
| Direct3D 8 / DirectX | 5 |

### Defined in a translation unit that failed to compile

- `DX8Wrapper_IsWindowed`
- `ReplayWasPressed`
- `TheGameEngine`
- `TheGameInfo`
- `TheGameLogic`
- `TheGameSpyPeerMessageQueue`
- `TheIMEManager`
- `TheKeyboard`
- `ThePinger`
- `TheRecorder`
- `TheStatsCollector`
- `TheSubsystemList`
- `MainMenuInit(WindowLayout*, void*)`
- `MainMenuInput(GameWindow*, unsigned int, unsigned int, unsigned int)`
- `MainMenuSystem(GameWindow*, unsigned int, unsigned int, unsigned int)`
- …and 216 more

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

### Win32 API

- `CopyFileA`
- `CreateDirectoryA`
- `CreateStdDispatch`
- `DeleteFileA`
- `FindClose`
- `FindFirstFileA`
- `FindNextFileA`
- `FindResourceA`
- `FormatMessageW`
- `FreeLibrary`
- `GetCommandLineA`
- `GetCurrentDirectoryA`
- `GetDateFormatA`
- `GetDateFormatW`
- `GetDoubleClickTime`
- …and 28 more

### Other / unclassified

- `CloseStatsConnection`
- `FreeGame`
- `GenerateAuthA`
- `GetChallenge`
- `GetPersistDataValuesA`
- `IID_IBrowserDispatch`
- `IID_IUnknown`
- `InitStatsConnection`
- `IsStatsConnected`
- `NewGame`
- `PersistThink`
- `PreAuthenticatePlayerCDA`
- `PreAuthenticatePlayerPM`
- `SendGameSnapShotA`
- `SetPersistDataValuesA`
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
- `SetUnsignedIntInRegistry(std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >, unsigned int)`
- `GetUnsignedIntFromRegistry(std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >, unsigned int&)`
- `TextureFilterClass::getTextureFilterMode(char const*)`
- `WW3D::Get_Texture_Reduction()`
- `MapObject::getWaypointID()`
- `MapObject::getWaypointName()`
- …and 3 more

### Engine C++ not built at this level

- `ConnectionManager::ConnectionManager()`
- `ConnectionManager::~ConnectionManager()`
- `GameSpyStagingRoom::GameSpyStagingRoom()`
- `TextureFilterClass::TextureFilterModeString`
- `WW3D::PreviousSyncTime`
- `WW3D::SyncTime`
- `GameInfo::GameInfo()`
- `GameSlot::GameSlot()`
- `MapObject::TheWorldDict`
- `MapObject::TheMapObjectListPtr`
- `MapObject::MapObject(Coord3D, AsciiString, float, int, Dict const*, ThingTemplate const*)`
- `_com_util::ConvertStringToBSTR(char const*)`
- `typeinfo for GameInfo`
- `typeinfo for GameSlot`
- `vtable for SkirmishGameInfo`
- …and 3 more

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
