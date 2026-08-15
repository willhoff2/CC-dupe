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
| `Core/Libraries/Source/WWVegas/WWLib` | 77 | 77 | 77 |
| `Core/Libraries/Source/WWVegas/WWDebug` | 3 | 3 | 3 |
| `Core/Libraries/Source/WWVegas/WWSaveLoad` | 12 | 12 | 12 |
| `Core/GameEngine` | 207 | 210 | 207 |
| `GeneralsMD/Code/GameEngine` | 380 | 380 | 380 |
| `Core/GameEngineDevice` | 69 | 70 | 69 |
| `GeneralsMD/Code/GameEngineDevice` | 38 | 39 | 38 |
| `GeneralsMD/Code/Main` | 1 | 1 | 1 |
| **Total** | **834** | **839** | **834** |

5 translation units produced no object file:

| Translation unit | First diagnostic |
|---|---|
| `Core/GameEngine/Source/GameNetwork/GameSpy/MainMenuUtils.cpp` | `unknown type name 'HANDLE'` |
| `Core/GameEngine/Source/GameNetwork/GameSpy/StagingRoomGameInfo.cpp` | `unknown type name 'AsnObjectIdentifier'` |
| `Core/GameEngine/Source/GameNetwork/GameSpy/Thread/PingThread.cpp` | `unknown type name 'HOSTENT'` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/Water/W3DWater.cpp` | `use of undeclared identifier 'D3DXMatrixInverse'` |
| `GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DDisplay.cpp` | `unknown type name 'LPDISPATCH'` |

## 2. How much the probe over-reports

**0 translation units that the probe calls clean fail to compile**, out of 834 probe-clean units (0%). These are the codegen-class failures `-fsyntax-only` cannot see.

## 3. Undefined symbols

The 10 archives were linked into one binary with `--whole-archive`, plus the third-party libraries the engine calls into: `libthirdparty_lzhl`, `z (system)` (binary produced: yes; linker exited 0 -- unresolved symbols are warnings here, so a file being produced does not mean it can run; entry point: `libgeneralsmd_code_main`; 3 standalone test-tool `main()` object(s) removed from the archives first: `libcore_libraries_source_wwvegas_wwlib(gdi_font_metrics_dump.cpp.o)`, `libcore_libraries_source_wwvegas_wwlib(win32_file_api_test.cpp.o)`, `libcore_libraries_source_wwvegas_wwlib(win32_runtime_test.cpp.o)`). **577 distinct symbols are unresolved** once libc, libstdc++, libm, libpthread and the CRT/unwinder symbols are discounted. The full categorised list is in the JSON output; examples follow each count.

| Cause | Symbols |
|---|---:|
| Defined in a layer not built here (renderer / audio) | 323 |
| GameSpy SDK (cut scope, not linked) | 81 |
| Miles Sound System | 60 |
| Defined in a translation unit that failed to compile | 46 |
| FFmpeg (not linked here) | 29 |
| Defined only in a backend this configuration excludes (SDL2 / Cocoa) | 12 |
| Defined in a built translation unit behind a disabled #if (build option / platform) | 10 |
| Direct3D 8 / DirectX | 6 |
| Generated gitinfo (build-time, not a blocker) | 6 |
| Win32 API | 3 |
| Other / unclassified | 1 |

### Defined in a layer not built here (renderer / audio)

- `TheDX8MeshRenderer`
- `Log_DX8_ErrorCode(unsigned int)`
- `Get_Bytes_Per_Pixel(WW3DFormat)`
- `ARGB_Color_To_WW3D_Color(WW3DFormat, unsigned int)`
- `SetUnsignedIntInRegistry(std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >, unsigned int)`
- `DX8Wrapper::Draw_Strip(unsigned short, unsigned short, unsigned short, unsigned short)`
- `DX8Wrapper::CurrentCaps`
- `DX8Wrapper::Has_Stencil()`
- `DX8Wrapper::Pixel_Shader`
- `DX8Wrapper::RenderStates`
- `DX8Wrapper::render_state`
- `DX8Wrapper::DX8Transforms`
- `DX8Wrapper::RenderBackend`
- `DX8Wrapper::Vertex_Shader`
- `DX8Wrapper::Draw_Triangles(unsigned short, unsigned short, unsigned short, unsigned short)`
- …and 308 more

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

### Miles Sound System

- `AIL_3D_sample_playback_rate`
- `AIL_WAV_info`
- `AIL_allocate_3D_sample_handle`
- `AIL_allocate_sample_handle`
- `AIL_close_3D_listener`
- `AIL_close_3D_provider`
- `AIL_close_stream`
- `AIL_decompress_ADPCM`
- `AIL_enumerate_3D_providers`
- `AIL_enumerate_filters`
- `AIL_get_DirectSound_info`
- `AIL_init_sample`
- `AIL_mem_free_lock`
- `AIL_open_3D_listener`
- `AIL_open_3D_provider`
- …and 45 more

### Defined in a translation unit that failed to compile

- `ThePinger`
- `TheWaterRenderObj`
- `doSkyBoxSet(bool)`
- `StartPatchCheck()`
- `HTTPThinkWrapper()`
- `StopAsyncDNSCheck()`
- `StartDownloadingPatches()`
- `CancelPatchCheckCallback()`
- `GetLocalChatConnectionAddress(AsciiString, unsigned short, unsigned int&)`
- `W3DDisplay::m_assetManager`
- `W3DDisplay::m_3DInterfaceScene`
- `W3DDisplay::m_2DScene`
- `W3DDisplay::m_3DScene`
- `W3DDisplay::W3DDisplay()`
- `GameSpyGameSlot::setPingString(AsciiString)`
- …and 31 more

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

### Defined in a built translation unit behind a disabled #if (build option / platform)

- `DX8Wrapper_IsWindowed`
- `DX8Wrapper_PreserveFPU`
- `Convert_Pixel(unsigned char*, SurfaceClass::SurfaceDescription const&, Vector3 const&)`
- `FillStackAddresses(void**, unsigned int, unsigned int)`
- `StackDumpFromAddresses(void**, unsigned int, void (*)(char const*))`
- `GetUnsignedIntFromRegistry(std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >, unsigned int&)`
- `DX8Wrapper::Set_Vertex_Buffer(VertexBufferClass const*, unsigned int)`
- `DX8Wrapper::Set_Vertex_Buffer(DynamicVBAccessClass const&)`
- `g_LastErrorDump`
- `getQR2HostingStatus`

### Direct3D 8 / DirectX

- `D3DXFilterTexture`
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
- `ScreenToClient`
- `SetCursor`

### Other / unclassified

- `MSS_auto_cleanup`

## Reproducing

```sh
bash scripts/ci/fetch-probe-deps.sh
python3 scripts/native-build.py --level 1 --level 2 --level 3 --with-shims --report docs/porting/native-build-report.md --json native-build.json
```
