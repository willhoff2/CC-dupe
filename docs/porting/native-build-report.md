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
| `Core/Libraries/Source/WWVegas/WWLib` | 77 | 77 | 77 |
| `Core/Libraries/Source/WWVegas/WWDebug` | 3 | 3 | 3 |
| `Core/Libraries/Source/WWVegas/WWSaveLoad` | 12 | 12 | 12 |
| `Core/GameEngine` | 207 | 210 | 207 |
| `GeneralsMD/Code/GameEngine` | 380 | 380 | 380 |
| `Core/Libraries/Source/WWVegas/WW3D2` | 67 | 74 | 67 |
| `Core/Libraries/Source/WWVegas/WWAudio` | 19 | 19 | 19 |
| `Core/Libraries/Source/WWVegas/WWDownload` | 0 | 4 | 0 |
| `GeneralsMD/Code/Libraries/Source/WWVegas` | 33 | 35 | 33 |
| `Core/GameEngineDevice` | 69 | 70 | 69 |
| `GeneralsMD/Code/GameEngineDevice` | 39 | 39 | 39 |
| `GeneralsMD/Code/Main` | 1 | 1 | 1 |
| **Total** | **954** | **971** | **954** |

17 translation units produced no object file:

| Translation unit | First diagnostic |
|---|---|
| `Core/GameEngine/Source/GameNetwork/GameSpy/MainMenuUtils.cpp` | `unknown type name 'HANDLE'` |
| `Core/GameEngine/Source/GameNetwork/GameSpy/StagingRoomGameInfo.cpp` | `unknown type name 'AsnObjectIdentifier'` |
| `Core/GameEngine/Source/GameNetwork/GameSpy/Thread/PingThread.cpp` | `unknown type name 'HOSTENT'` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/Water/W3DWater.cpp` | `use of undeclared identifier 'D3DXMatrixInverse'` |
| `Core/Libraries/Source/WWVegas/WW3D2/FramGrab.cpp` | `'windowsx.h' file not found` |
| `Core/Libraries/Source/WWVegas/WW3D2/dx8caps.cpp` | `no member named 'DriverVersion' in '_D3DADAPTER_IDENTIFIER8'` |
| `Core/Libraries/Source/WWVegas/WW3D2/dx8wrapper.cpp` | `no member named 'DriverVersion' in '_D3DADAPTER_IDENTIFIER8'` |
| `Core/Libraries/Source/WWVegas/WW3D2/pointgr.cpp` | `use of undeclared identifier 'D3DX_PI'` |
| `Core/Libraries/Source/WWVegas/WW3D2/sortingrenderer.cpp` | `invalid operands to binary expression ('D3DXMATRIX' and 'D3DXMATRIX')` |
| `Core/Libraries/Source/WWVegas/WW3D2/textdraw.cpp` | `too many arguments to function call, expected 0, have 1` |
| `Core/Libraries/Source/WWVegas/WW3D2/w3d_dep.cpp` | `unknown type name 'size_t'; did you mean 'std::size_t'?` |
| `Core/Libraries/Source/WWVegas/WWDownload/Download.cpp` | `'Common/Debug.h' file not found` |
| `Core/Libraries/Source/WWVegas/WWDownload/FTP.cpp` | `unknown type name 'LPCSTR'` |
| `Core/Libraries/Source/WWVegas/WWDownload/registry.cpp` | `use of undeclared identifier 'KEY_READ'` |
| `Core/Libraries/Source/WWVegas/WWDownload/urlBuilder.cpp` | `use of undeclared identifier 'sku'` |
| `GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/ddsfile.cpp` | `'ddraw.h' file not found` |
| `GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/ww3d.cpp` | `'windowsx.h' file not found` |

## 2. How much the probe over-reports

**0 translation units that the probe calls clean fail to compile**, out of 954 probe-clean units (0%). These are the codegen-class failures `-fsyntax-only` cannot see.

## 3. Undefined symbols

The 13 archives were linked into one binary with `--whole-archive`, plus the third-party libraries the engine calls into: `libthirdparty_lzhl`, `z (system)` (binary produced: yes; linker exited 0 -- unresolved symbols are warnings here, so a file being produced does not mean it can run; entry point: `libgeneralsmd_code_main`; 3 standalone test-tool `main()` object(s) removed from the archives first: `libcore_libraries_source_wwvegas_wwlib(gdi_font_metrics_dump.cpp.o)`, `libcore_libraries_source_wwvegas_wwlib(win32_file_api_test.cpp.o)`, `libcore_libraries_source_wwvegas_wwlib(win32_runtime_test.cpp.o)`). **427 distinct symbols are unresolved** once libc, libstdc++, libm, libpthread and the CRT/unwinder symbols are discounted. The full categorised list is in the JSON output; examples follow each count.

| Cause | Symbols |
|---|---:|
| Defined in a translation unit that failed to compile | 192 |
| Miles Sound System | 89 |
| GameSpy SDK (cut scope, not linked) | 81 |
| FFmpeg (not linked here) | 29 |
| Defined only in a backend this configuration excludes (SDL2 / Cocoa) | 12 |
| Direct3D 8 / DirectX | 8 |
| Generated gitinfo (build-time, not a blocker) | 6 |
| Win32 API | 4 |
| Defined in a built translation unit behind a disabled #if (build option / platform) | 4 |
| Other / unclassified | 1 |
| Engine C++ not built at this level | 1 |

### Defined in a translation unit that failed to compile

- `DX8Wrapper_IsWindowed`
- `DX8Wrapper_PreserveFPU`
- `ThePinger`
- `TheWaterRenderObj`
- `DX8_Assert()`
- `doSkyBoxSet(bool)`
- `StartPatchCheck()`
- `HTTPThinkWrapper()`
- `Log_DX8_ErrorCode(unsigned int)`
- `StopAsyncDNSCheck()`
- `StartDownloadingPatches()`
- `CancelPatchCheckCallback()`
- `SetUnsignedIntInRegistry(std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >, unsigned int)`
- `GetUnsignedIntFromRegistry(std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >, unsigned int&)`
- `GetLocalChatConnectionAddress(AsciiString, unsigned short, unsigned int&)`
- …and 177 more

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
- `AIL_close_3D_listener`
- `AIL_close_3D_provider`
- `AIL_close_stream`
- `AIL_decompress_ADPCM`
- `AIL_end_3D_sample`
- `AIL_end_sample`
- …and 74 more

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

### FFmpeg (not linked here)

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

### Direct3D 8 / DirectX

- `D3DXFilterTexture`
- `D3DXGetFVFVertexSize`
- `D3DXLoadSurfaceFromSurface`
- `D3DXMatrixInverse`
- `D3DXMatrixMultiply`
- `D3DXMatrixScaling`
- `D3DXMatrixTranslation`
- `D3DXMatrixTranspose`

### Generated gitinfo (build-time, not a blocker)

- `GitCommitAuthorName`
- `GitCommitTimeStamp`
- `GitRevision`
- `GitShortSHA1`
- `GitTag`
- `GitUncommittedChanges`

### Win32 API

- `GetCursorPos`
- `IsIconic`
- `ScreenToClient`
- `SetCursor`

### Defined in a built translation unit behind a disabled #if (build option / platform)

- `FillStackAddresses(void**, unsigned int, unsigned int)`
- `StackDumpFromAddresses(void**, unsigned int, void (*)(char const*))`
- `g_LastErrorDump`
- `getQR2HostingStatus`

### Other / unclassified

- `MSS_auto_cleanup`

### Engine C++ not built at this level

- `ListenerHandleClass::Initialize(SoundBufferClass*)`

## Reproducing

```sh
bash scripts/ci/fetch-probe-deps.sh
python3 scripts/native-build.py --level 1 --level 2 --level 3 --level 4 --with-shims --report docs/porting/native-build-report.md --json native-build.json
```
