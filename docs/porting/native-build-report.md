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
| `Core/Libraries/Source/WWVegas/WWLib` | 67 | 67 | 67 |
| `Core/Libraries/Source/WWVegas/WWDebug` | 3 | 3 | 3 |
| `Core/Libraries/Source/WWVegas/WWSaveLoad` | 12 | 12 | 12 |
| `Core/GameEngine` | 203 | 210 | 203 |
| `GeneralsMD/Code/GameEngine` | 377 | 380 | 377 |
| **Total** | **708** | **718** | **708** |

10 translation units produced no object file:

| Translation unit | First diagnostic |
|---|---|
| `Core/GameEngine/Source/GameClient/GUI/IMEManager.cpp` | `unknown type name 'HWND'` |
| `Core/GameEngine/Source/GameNetwork/DownloadManager.cpp` | `unknown type name 'HRESULT'; did you mean 'MMRESULT'?` |
| `Core/GameEngine/Source/GameNetwork/GameSpy/MainMenuUtils.cpp` | `unknown type name 'HRESULT'; did you mean 'MMRESULT'?` |
| `Core/GameEngine/Source/GameNetwork/GameSpy/StagingRoomGameInfo.cpp` | `unknown type name 'AsnObjectIdentifier'` |
| `Core/GameEngine/Source/GameNetwork/GameSpy/Thread/GameResultsThread.cpp` | `unknown type name 'HOSTENT'` |
| `Core/GameEngine/Source/GameNetwork/GameSpy/Thread/PeerThread.cpp` | `no matching function for call to 'recvfrom'` |
| `Core/GameEngine/Source/GameNetwork/GameSpy/Thread/PingThread.cpp` | `unknown type name 'HOSTENT'` |
| `GeneralsMD/Code/GameEngine/Source/Common/GameEngine.cpp` | `no member named 'SetWindowText' in the global namespace` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/DownloadMenu.cpp` | `unknown type name 'HRESULT'; did you mean 'MMRESULT'?` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/MainMenu.cpp` | `unknown type name 'HRESULT'; did you mean 'MMRESULT'?` |

## 2. How much the probe over-reports

**0 translation units that the probe calls clean fail to compile**, out of 708 probe-clean units (0%). These are the codegen-class failures `-fsyntax-only` cannot see.

## 3. Undefined symbols

The 7 archives were linked into one binary with `--whole-archive` (binary produced: yes; clean link: yes). **280 distinct symbols are unresolved** once libc, libstdc++, libm, libpthread and the CRT/unwinder symbols are discounted. The full categorised list is in the JSON output; examples follow each count.

| Cause | Symbols |
|---|---:|
| Well-known Dict keys (instantiated in GameEngineDevice) | 104 |
| Win32 API | 43 |
| Defined in a translation unit that failed to compile | 33 |
| Engine C++ not built at this level | 22 |
| Defined in a layer not built here (renderer / audio / device / entry point) | 21 |
| Other / unclassified | 19 |
| GameSpy | 18 |
| Third-party library not linked (lzhl, zlib) | 9 |
| Generated gitinfo (build-time, not a blocker) | 6 |
| Direct3D 8 / DirectX | 5 |

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
- …and 89 more

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

### Defined in a translation unit that failed to compile

- `DX8Wrapper_IsWindowed`
- `TheGameEngine`
- `TheGameResultsQueue`
- `TheGameSpyPeerMessageQueue`
- `TheIMEManager`
- `ThePinger`
- `TheSubsystemList`
- `MainMenuInit(WindowLayout*, void*)`
- `MainMenuInput(GameWindow*, unsigned int, unsigned int, unsigned int)`
- `MainMenuSystem(GameWindow*, unsigned int, unsigned int, unsigned int)`
- `MainMenuUpdate(WindowLayout*, void*)`
- `setupGameStart(AsciiString, GameDifficulty)`
- `DownloadMenuInit(WindowLayout*, void*)`
- `MainMenuShutdown(WindowLayout*, void*)`
- `DownloadMenuInput(GameWindow*, unsigned int, unsigned int, unsigned int)`
- …and 18 more

### Engine C++ not built at this level

- `FillStackAddresses(void**, unsigned int, unsigned int)`
- `StackDumpFromAddresses(void**, unsigned int, void (*)(char const*))`
- `WWPlatform::Window_Show(void*, bool)`
- `WWPlatform::Window_Create(WWPlatform::WindowConfig const&)`
- `WWPlatform::Window_Destroy(void*)`
- `WWPlatform::Window_Set_Mode(void*, int, int, bool)`
- `WWPlatform::Window_Is_Active(void*)`
- `WWPlatform::Window_Poll_Event(void*, WWPlatform::WindowEvent&)`
- `WWPlatform::Window_Client_Size(void*, int&, int&)`
- `WWPlatform::Window_Warp_Cursor(void*, int, int)`
- `WWPlatform::Window_Is_Minimised(void*)`
- `WWPlatform::Window_Modifier_State(void*)`
- `WWPlatform::Window_Set_Cursor_Clip(void*, bool)`
- `GameSpyStagingRoom::GameSpyStagingRoom()`
- `TextureFilterClass::TextureFilterModeString`
- …and 7 more

### Defined in a layer not built here (renderer / audio / device / entry point)

- `DX8Wrapper_PreserveFPU`
- `TheProjectedShadowManager`
- `TheSmudgeManager`
- `MOTDSystem(GameWindow*, unsigned int, unsigned int, unsigned int)`
- `doSkyBoxSet(bool)`
- `CreateGameEngine()`
- `ReloadAllTextures()`
- `oversizeTheTerrain(int)`
- `OSDisplaySetBusyState(bool, bool)`
- `testMinimumRequirements(ChipsetType*, CpuType*, int*, unsigned long long*, float*, float*, float*)`
- `SetUnsignedIntInRegistry(std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >, unsigned int)`
- `GetUnsignedIntFromRegistry(std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >, unsigned int&)`
- `TextureFilterClass::getTextureFilterMode(char const*)`
- `WW3D::Get_Texture_Reduction()`
- `MapObject::getWaypointID()`
- …and 6 more

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
- …and 4 more

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
