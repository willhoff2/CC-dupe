# Native 64-bit build — objects and link

Produced by `scripts/native-build.py`. Unlike every other number in `docs/porting/`,
these come from real object files and a real linker invocation, not from
`clang++ -fsyntax-only`.

Toolchain: `Ubuntu clang version 14.0.0-1ubuntu1.1`, target `x86_64-pc-linux-gnu`, levels built: 1, 2, 3.

Mode: **shimmed** — `scripts/native-port-shims/` supplies declaration-only stand-ins for the Win32 headers, so a missing platform layer shows up as an undefined symbol rather than as a failed compile. That is the point: it moves the blockers from §1 to §3, where they can be counted individually.

## 1. Compilation

| Library | Objects produced | Translation units | Probe-clean |
|---|---:|---:|---:|
| `Core/Libraries/Source/Compression` | 11 | 11 | 11 |
| `Core/Libraries/Source/WWVegas/WWMath` | 36 | 36 | 36 |
| `Core/Libraries/Source/WWVegas/WWLib` | 74 | 74 | 74 |
| `Core/Libraries/Source/WWVegas/WWDebug` | 3 | 3 | 3 |
| `Core/Libraries/Source/WWVegas/WWSaveLoad` | 12 | 12 | 12 |
| `Core/GameEngine` | 207 | 210 | 207 |
| `GeneralsMD/Code/GameEngine` | 380 | 380 | 380 |
| `Core/GameEngineDevice` | 64 | 70 | 64 |
| `GeneralsMD/Code/GameEngineDevice` | 34 | 39 | 34 |
| `GeneralsMD/Code/Main` | 0 | 1 | 0 |
| **Total** | **821** | **836** | **821** |

15 translation units produced no object file:

| Translation unit | First diagnostic |
|---|---|
| `Core/GameEngine/Source/GameNetwork/GameSpy/MainMenuUtils.cpp` | `unknown type name 'HANDLE'` |
| `Core/GameEngine/Source/GameNetwork/GameSpy/StagingRoomGameInfo.cpp` | `unknown type name 'AsnObjectIdentifier'` |
| `Core/GameEngine/Source/GameNetwork/GameSpy/Thread/PingThread.cpp` | `unknown type name 'HOSTENT'` |
| `Core/GameEngineDevice/Source/MilesAudioDevice/MilesAudioManager.cpp` | `unknown type name 'HANDLE'` |
| `Core/GameEngineDevice/Source/StdDevice/Common/StdLocalFileSystem.cpp` | `use of undeclared identifier 'unNormalized'; did you mean 'nonNormalized'?` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DScreenshot.cpp` | `use of undeclared identifier 'InterlockedExchangePointer'; did you mean '_InterlockedExchangePointer'?` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DShaderManager.cpp` | `no member named 'DriverVersion' in '_D3DADAPTER_IDENTIFIER8'` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/Water/W3DWater.cpp` | `use of undeclared identifier 'D3DXMatrixInverse'` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/Water/W3DWaterTracks.cpp` | `use of undeclared identifier 'VK_F5'` |
| `GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DAssetManager.cpp` | `use of undeclared identifier 'lstrcpyn'` |
| `GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DDisplay.cpp` | `no member named '_strdup' in the global namespace; did you mean 'strdup'?` |
| `GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DWebBrowser.cpp` | `'IDispatch' does not refer to a value` |
| `GeneralsMD/Code/GameEngineDevice/Source/Win32Device/Common/Win32GameEngine.cpp` | `allocating an object of abstract class type 'CComObject<W3DWebBrowser>'` |
| `GeneralsMD/Code/GameEngineDevice/Source/Win32Device/Common/Win32OSDisplay.cpp` | `use of undeclared identifier 'MB_APPLMODAL'` |
| `GeneralsMD/Code/Main/PlatformMain.cpp` | `allocating an object of abstract class type 'CComObject<W3DWebBrowser>'` |

## 2. How much the probe over-reports

**0 translation units that the probe calls clean fail to compile**, out of 821 probe-clean units (0%). These are the codegen-class failures `-fsyntax-only` cannot see.

## 3. Undefined symbols

The 9 archives were linked into one binary with `--whole-archive`, plus the third-party libraries the engine calls into: `libthirdparty_lzhl`, `z (system)` (binary produced: no; clean link: no). **496 distinct symbols are unresolved** once libc, libstdc++, libm, libpthread and the CRT/unwinder symbols are discounted. The full categorised list is in the JSON output; examples follow each count.

| Cause | Symbols |
|---|---:|
| Defined in a layer not built here (renderer / audio) | 274 |
| GameSpy SDK (cut scope, not linked) | 81 |
| Defined in a translation unit that failed to compile | 75 |
| Other / unclassified | 29 |
| Defined only in a backend this configuration excludes (SDL2 / Cocoa) | 12 |
| Defined in a built translation unit behind a disabled #if (build option / platform) | 10 |
| Generated gitinfo (build-time, not a blocker) | 6 |
| Direct3D 8 / DirectX | 3 |
| Win32 API | 3 |
| COM / OLE (browser embedding, cut scope) | 3 |

### Defined in a layer not built here (renderer / audio)

- `TheDX8MeshRenderer`
- `Log_DX8_ErrorCode(unsigned int)`
- `Get_Bytes_Per_Pixel(WW3DFormat)`
- `ARGB_Color_To_WW3D_Color(WW3DFormat, unsigned int)`
- `SetUnsignedIntInRegistry(std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >, unsigned int)`
- `DX8Wrapper::CurrentCaps`
- `DX8Wrapper::Has_Stencil()`
- `DX8Wrapper::Pixel_Shader`
- `DX8Wrapper::RenderStates`
- `DX8Wrapper::render_state`
- `DX8Wrapper::RenderBackend`
- `DX8Wrapper::Vertex_Shader`
- `DX8Wrapper::Draw_Triangles(unsigned short, unsigned short, unsigned short, unsigned short)`
- `DX8Wrapper::m_pCleanupHook`
- `DX8Wrapper::FrameStatistics`
- …and 259 more

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
- `SBServerGetIntValueA`
- `SBServerGetPlayerIntValueA`
- `SBServerGetPlayerStringValueA`
- `SBServerGetPrivateInetAddress`
- …and 66 more

### Defined in a translation unit that failed to compile

- `ThePinger`
- `TheWaterRenderObj`
- `TheWin32Mouse`
- `doSkyBoxSet(bool)`
- `StartPatchCheck()`
- `CreateGameEngine()`
- `HTTPThinkWrapper()`
- `StopAsyncDNSCheck()`
- `OSDisplaySetBusyState(bool, bool)`
- `StartDownloadingPatches()`
- `testMinimumRequirements(ChipsetType*, CpuType*, int*, unsigned long long*, float*, float*, float*)`
- `CancelPatchCheckCallback()`
- `GetLocalChatConnectionAddress(AsciiString, unsigned short, unsigned int&)`
- `W3DDisplay::m_assetManager`
- `W3DDisplay::m_3DInterfaceScene`
- …and 60 more

### Other / unclassified

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
- `avcodec_find_decoder`
- …and 14 more

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

### Defined in a built translation unit behind a disabled #if (build option / platform)

- `ApplicationHInstance`
- `DX8Wrapper_IsWindowed`
- `DX8Wrapper_PreserveFPU`
- `FillStackAddresses(void**, unsigned int, unsigned int)`
- `StackDumpFromAddresses(void**, unsigned int, void (*)(char const*))`
- `GetUnsignedIntFromRegistry(std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >, unsigned int&)`
- `DX8Wrapper::Set_Vertex_Buffer(VertexBufferClass const*, unsigned int)`
- `DX8Wrapper::Set_Vertex_Buffer(DynamicVBAccessClass const&)`
- `g_LastErrorDump`
- `getQR2HostingStatus`

### Generated gitinfo (build-time, not a blocker)

- `GitCommitAuthorName`
- `GitCommitTimeStamp`
- `GitRevision`
- `GitShortSHA1`
- `GitTag`
- `GitUncommittedChanges`

### Direct3D 8 / DirectX

- `D3DXFilterTexture`
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

## Reproducing

```sh
bash scripts/ci/fetch-probe-deps.sh
python3 scripts/native-build.py --level 1 --level 2 --level 3 --with-shims --report docs/porting/native-build-report.md --json native-build.json
```
