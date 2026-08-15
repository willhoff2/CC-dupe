# Native 64-bit build — objects and link

Produced by `scripts/native-build.py`. Unlike every other number in `docs/porting/`,
these come from real object files and a real linker invocation, not from
`clang++ -fsyntax-only`.

Toolchain: `Ubuntu clang version 14.0.0-1ubuntu1.1`, target `x86_64-pc-linux-gnu`, levels built: 1, 2, 3, 4.

Mode: **shimmed** — `scripts/native-port-shims/` supplies declaration-only stand-ins for the Win32 headers, so a missing platform layer shows up as an undefined symbol rather than as a failed compile. That is the point: it moves the blockers from §1 to §3, where they can be counted individually.

## 1. Compilation

| Library | Objects produced | Translation units | Probe-clean |
|---|---:|---:|---:|
| `Core/Libraries/Source/Compression` | 11 | 11 | 11 |
| `Core/Libraries/Source/WWVegas/WWMath` | 36 | 36 | 36 |
| `Core/Libraries/Source/WWVegas/WWLib` | 74 | 74 | 74 |
| `Core/Libraries/Source/WWVegas/WWDebug` | 3 | 3 | 3 |
| `Core/Libraries/Source/WWVegas/WWSaveLoad` | 12 | 12 | 12 |
| `Core/GameEngine` | 204 | 210 | 204 |
| `GeneralsMD/Code/GameEngine` | 378 | 380 | 378 |
| `Core/Libraries/Source/WWVegas/WW3D2` | 66 | 74 | 66 |
| `Core/Libraries/Source/WWVegas/WWAudio` | 19 | 19 | 19 |
| `Core/Libraries/Source/WWVegas/WWDownload` | 0 | 4 | 0 |
| `GeneralsMD/Code/Libraries/Source/WWVegas` | 33 | 35 | 33 |
| `Core/GameEngineDevice` | 64 | 70 | 64 |
| `GeneralsMD/Code/GameEngineDevice` | 35 | 39 | 35 |
| `GeneralsMD/Code/Main` | 0 | 1 | 0 |
| **Total** | **935** | **968** | **935** |

33 translation units produced no object file:

| Translation unit | First diagnostic |
|---|---|
| `Core/GameEngine/Source/GameNetwork/DownloadManager.cpp` | `unknown type name 'HRESULT'; did you mean 'MMRESULT'?` |
| `Core/GameEngine/Source/GameNetwork/GameSpy/MainMenuUtils.cpp` | `unknown type name 'HRESULT'; did you mean 'MMRESULT'?` |
| `Core/GameEngine/Source/GameNetwork/GameSpy/StagingRoomGameInfo.cpp` | `unknown type name 'AsnObjectIdentifier'` |
| `Core/GameEngine/Source/GameNetwork/GameSpy/Thread/GameResultsThread.cpp` | `unknown type name 'HOSTENT'` |
| `Core/GameEngine/Source/GameNetwork/GameSpy/Thread/PeerThread.cpp` | `no matching function for call to 'recvfrom'` |
| `Core/GameEngine/Source/GameNetwork/GameSpy/Thread/PingThread.cpp` | `unknown type name 'HOSTENT'` |
| `Core/GameEngineDevice/Source/MilesAudioDevice/MilesAudioManager.cpp` | `unknown type name 'HANDLE'` |
| `Core/GameEngineDevice/Source/StdDevice/Common/StdLocalFileSystem.cpp` | `use of undeclared identifier 'unNormalized'; did you mean 'nonNormalized'?` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DScreenshot.cpp` | `use of undeclared identifier 'InterlockedExchangePointer'; did you mean '_InterlockedExchangePointer'?` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DShaderManager.cpp` | `no member named 'DriverVersion' in '_D3DADAPTER_IDENTIFIER8'` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/Water/W3DWater.cpp` | `use of undeclared identifier 'D3DXMatrixInverse'` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/Water/W3DWaterTracks.cpp` | `use of undeclared identifier 'VK_F5'` |
| `Core/Libraries/Source/WWVegas/WW3D2/FramGrab.cpp` | `'windowsx.h' file not found` |
| `Core/Libraries/Source/WWVegas/WW3D2/dx8caps.cpp` | `no member named 'DriverVersion' in '_D3DADAPTER_IDENTIFIER8'` |
| `Core/Libraries/Source/WWVegas/WW3D2/dx8webbrowser.cpp` | `unknown type name 'LPDISPATCH'` |
| `Core/Libraries/Source/WWVegas/WW3D2/dx8wrapper.cpp` | `unknown type name 'LPDISPATCH'` |
| `Core/Libraries/Source/WWVegas/WW3D2/pointgr.cpp` | `use of undeclared identifier 'D3DX_PI'` |
| `Core/Libraries/Source/WWVegas/WW3D2/sortingrenderer.cpp` | `invalid operands to binary expression ('D3DXMATRIX' and 'D3DXMATRIX')` |
| `Core/Libraries/Source/WWVegas/WW3D2/textdraw.cpp` | `too many arguments to function call, expected 0, have 1` |
| `Core/Libraries/Source/WWVegas/WW3D2/w3d_dep.cpp` | `unknown type name 'size_t'; did you mean 'std::size_t'?` |
| `Core/Libraries/Source/WWVegas/WWDownload/Download.cpp` | `'Common/Debug.h' file not found` |
| `Core/Libraries/Source/WWVegas/WWDownload/FTP.cpp` | `unknown type name 'HRESULT'` |
| `Core/Libraries/Source/WWVegas/WWDownload/registry.cpp` | `use of undeclared identifier 'KEY_READ'` |
| `Core/Libraries/Source/WWVegas/WWDownload/urlBuilder.cpp` | `use of undeclared identifier 'sku'` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/DownloadMenu.cpp` | `unknown type name 'HRESULT'; did you mean 'MMRESULT'?` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/MainMenu.cpp` | `unknown type name 'HRESULT'; did you mean 'MMRESULT'?` |
| `GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DDisplay.cpp` | `unknown type name 'LPDISPATCH'` |
| `GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DWebBrowser.cpp` | `'IDispatch' does not refer to a value` |
| `GeneralsMD/Code/GameEngineDevice/Source/Win32Device/Common/Win32GameEngine.cpp` | `allocating an object of abstract class type 'CComObject<W3DWebBrowser>'` |
| `GeneralsMD/Code/GameEngineDevice/Source/Win32Device/Common/Win32OSDisplay.cpp` | `use of undeclared identifier 'MB_APPLMODAL'` |
| `GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/ddsfile.cpp` | `'ddraw.h' file not found` |
| `GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/ww3d.cpp` | `'windowsx.h' file not found` |
| `GeneralsMD/Code/Main/PlatformMain.cpp` | `allocating an object of abstract class type 'CComObject<W3DWebBrowser>'` |

## 2. How much the probe over-reports

**0 translation units that the probe calls clean fail to compile**, out of 935 probe-clean units (0%). These are the codegen-class failures `-fsyntax-only` cannot see.

## 3. Undefined symbols

The 12 archives were linked into one binary with `--whole-archive`, plus the third-party libraries the engine calls into: `libthirdparty_lzhl`, `z (system)` (binary produced: no; clean link: no). **386 distinct symbols are unresolved** once libc, libstdc++, libm, libpthread and the CRT/unwinder symbols are discounted. The full categorised list is in the JSON output; examples follow each count.

| Cause | Symbols |
|---|---:|
| Defined in a translation unit that failed to compile | 214 |
| Miles Sound System | 74 |
| GameSpy SDK (cut scope, not linked) | 33 |
| Other / unclassified | 30 |
| Defined only in a backend this configuration excludes (SDL2 / Cocoa) | 12 |
| Generated gitinfo (build-time, not a blocker) | 6 |
| Defined in a built translation unit behind a disabled #if (build option / platform) | 5 |
| Direct3D 8 / DirectX | 5 |
| Win32 API | 3 |
| COM / OLE (browser embedding, cut scope) | 3 |
| Engine C++ not built at this level | 1 |

### Defined in a translation unit that failed to compile

- `DX8Wrapper_IsWindowed`
- `DX8Wrapper_PreserveFPU`
- `TheGameResultsQueue`
- `TheGameSpyPeerMessageQueue`
- `ThePinger`
- `TheWaterRenderObj`
- `TheWin32Mouse`
- `DX8_Assert()`
- `doSkyBoxSet(bool)`
- `MainMenuInit(WindowLayout*, void*)`
- `MainMenuInput(GameWindow*, unsigned int, unsigned int, unsigned int)`
- `MainMenuSystem(GameWindow*, unsigned int, unsigned int, unsigned int)`
- `MainMenuUpdate(WindowLayout*, void*)`
- `setupGameStart(AsciiString, GameDifficulty)`
- `CreateGameEngine()`
- …and 199 more

### Miles Sound System

- `AIL_3D_sample_length`
- `AIL_3D_sample_loop_count`
- `AIL_3D_sample_offset`
- `AIL_3D_sample_playback_rate`
- `AIL_3D_sample_volume`
- `AIL_3D_user_data`
- `AIL_WAV_info`
- `AIL_allocate_3D_sample_handle`
- `AIL_allocate_sample_handle`
- `AIL_close_3D_provider`
- `AIL_close_stream`
- `AIL_end_3D_sample`
- `AIL_end_sample`
- `AIL_enumerate_3D_providers`
- `AIL_enumerate_filters`
- …and 59 more

### GameSpy SDK (cut scope, not linked)

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
- …and 18 more

### Other / unclassified

- `MSS_auto_cleanup`
- `av_frame_alloc`
- `av_frame_clone`
- `av_frame_free`
- `av_freep`
- `av_get_bytes_per_sample`
- `av_malloc`
- `av_packet_alloc`
- `av_packet_free`
- `av_packet_unref`
- `av_read_frame`
- `av_samples_get_buffer_size`
- `av_seek_frame`
- `av_strerror`
- `avcodec_alloc_context3`
- …and 15 more

### Defined only in a backend this configuration excludes (SDL2 / Cocoa)

- `WWPlatform::Window_Show(void*, bool)`
- `WWPlatform::Window_Create(WWPlatform::WindowConfig const&)`
- `WWPlatform::Window_Destroy(void*)`
- `WWPlatform::Window_Set_Mode(void*, int, int, bool)`
- `WWPlatform::Window_Is_Active(void*)`
- `WWPlatform::Window_Set_Title(void*, char const*)`
- `WWPlatform::Window_Poll_Event(void*, WWPlatform::WindowEvent&)`
- `WWPlatform::Window_Client_Size(void*, int&, int&)`
- `WWPlatform::Window_Warp_Cursor(void*, int, int)`
- `WWPlatform::Window_Is_Minimised(void*)`
- `WWPlatform::Window_Modifier_State(void*)`
- `WWPlatform::Window_Set_Cursor_Clip(void*, bool)`

### Generated gitinfo (build-time, not a blocker)

- `GitCommitAuthorName`
- `GitCommitTimeStamp`
- `GitRevision`
- `GitShortSHA1`
- `GitTag`
- `GitUncommittedChanges`

### Defined in a built translation unit behind a disabled #if (build option / platform)

- `ApplicationHInstance`
- `FillStackAddresses(void**, unsigned int, unsigned int)`
- `StackDumpFromAddresses(void**, unsigned int, void (*)(char const*))`
- `g_LastErrorDump`
- `getQR2HostingStatus`

### Direct3D 8 / DirectX

- `D3DXFilterTexture`
- `D3DXGetFVFVertexSize`
- `D3DXLoadSurfaceFromSurface`
- `D3DXMatrixMultiply`
- `D3DXMatrixTranspose`

### Win32 API

- `GetCursorPos`
- `ScreenToClient`
- `SetCursor`

### COM / OLE (browser embedding, cut scope)

- `IID_IBrowserDispatch`
- `IID_IUnknown`
- `_com_util::ConvertStringToBSTR(char const*)`

### Engine C++ not built at this level

- `ListenerHandleClass::Initialize(SoundBufferClass*)`

## Reproducing

```sh
bash scripts/ci/fetch-probe-deps.sh
python3 scripts/native-build.py --level 1 --level 2 --level 3 --level 4 --with-shims --report docs/porting/native-build-report.md --json native-build.json
```
