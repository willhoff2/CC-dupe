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
| `Core/Libraries/Source/WWVegas/WWMath` | 37 | 37 | 37 |
| `Core/Libraries/Source/WWVegas/WWLib` | 78 | 78 | 78 |
| `Core/Libraries/Source/WWVegas/WWDebug` | 3 | 3 | 3 |
| `Core/Libraries/Source/WWVegas/WWSaveLoad` | 12 | 12 | 12 |
| `Core/GameEngine` | 207 | 210 | 207 |
| `GeneralsMD/Code/GameEngine` | 380 | 380 | 380 |
| `Core/Libraries/Source/WWVegas/WW3D2` | 73 | 73 | 73 |
| `Core/Libraries/Source/WWVegas/WWAudio` | 19 | 19 | 19 |
| `Core/Libraries/Source/WWVegas/WWDownload` | 4 | 4 | 4 |
| `GeneralsMD/Code/Libraries/Source/WWVegas` | 35 | 35 | 35 |
| `Core/GameEngineDevice` | 70 | 70 | 70 |
| `GeneralsMD/Code/GameEngineDevice` | 39 | 39 | 39 |
| `GeneralsMD/Code/Main` | 1 | 1 | 1 |
| **Total** | **969** | **972** | **969** |

3 translation units produced no object file:

| Translation unit | First diagnostic |
|---|---|
| `Core/GameEngine/Source/GameNetwork/GameSpy/MainMenuUtils.cpp` | `unknown type name 'HANDLE'` |
| `Core/GameEngine/Source/GameNetwork/GameSpy/StagingRoomGameInfo.cpp` | `unknown type name 'AsnObjectIdentifier'` |
| `Core/GameEngine/Source/GameNetwork/GameSpy/Thread/PingThread.cpp` | `unknown type name 'HOSTENT'` |

## 2. How much the probe over-reports

**0 translation units that the probe calls clean fail to compile**, out of 969 probe-clean units (0%). These are the codegen-class failures `-fsyntax-only` cannot see.

## 3. Undefined symbols

The 14 archives were linked into one binary with `--whole-archive`, plus the third-party libraries the engine calls into: `libsupport_openalaudiodevice`, `libthirdparty_lzhl`, `openal (system)`, `z (system)` (binary produced: yes; linker exited 0 -- unresolved symbols are warnings here, so a file being produced does not mean it can run; entry point: `libgeneralsmd_code_main`; 4 standalone test-tool `main()` object(s) removed from the archives first: `libcore_libraries_source_wwvegas_wwlib(gdi_font_metrics_dump.cpp.o)`, `libcore_libraries_source_wwvegas_wwlib(win32_file_api_test.cpp.o)`, `libcore_libraries_source_wwvegas_wwlib(win32_runtime_test.cpp.o)`, `libcore_libraries_source_wwvegas_wwmath(d3dx8math_test.cpp.o)`). **173 distinct symbols are unresolved** once libc, libstdc++, libm, libpthread and the CRT/unwinder symbols are discounted. The full categorised list is in the JSON output; examples follow each count.

| Cause | Symbols |
|---|---:|
| GameSpy SDK (cut scope, not linked) | 81 |
| FFmpeg (not linked here) | 29 |
| Defined in a translation unit that failed to compile | 18 |
| Win32 API | 12 |
| Defined only in a backend this configuration excludes (SDL2 / Cocoa) | 12 |
| Direct3D 8 / DirectX | 9 |
| Generated gitinfo (build-time, not a blocker) | 6 |
| Defined in a built translation unit behind a disabled #if (build option / platform) | 4 |
| Other / unclassified | 1 |
| Engine C++ not built at this level | 1 |

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

### Defined in a translation unit that failed to compile

- `ThePinger`
- `StartPatchCheck()`
- `HTTPThinkWrapper()`
- `StopAsyncDNSCheck()`
- `StartDownloadingPatches()`
- `CancelPatchCheckCallback()`
- `GetLocalChatConnectionAddress(AsciiString, unsigned short, unsigned int&)`
- `GameSpyGameSlot::setPingString(AsciiString)`
- `PingerInterface::createNewPingerInterface()`
- `GameSpyStagingRoom::launchGame()`
- `GameSpyStagingRoom::setPingString(AsciiString)`
- `GameSpyStagingRoom::getGameSpySlot(int)`
- `GameSpyStagingRoom::cleanUpSlotPointers()`
- `GameSpyStagingRoom::generateLadderGameResultsPacket()`
- `GameSpyStagingRoom::generateGameSpyGameResultsPacket()`
- …and 3 more

### Win32 API

- `AdjustWindowRect`
- `GetClientRect`
- `GetCursorPos`
- `GetDesktopWindow`
- `GetMonitorInfoA`
- `GetWindowLongA`
- `IsIconic`
- `MonitorFromWindow`
- `ScreenToClient`
- `SetCursor`
- `SetDeviceGammaRamp`
- `SetWindowPos`

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

- `D3DXAssembleShader`
- `D3DXCreateCubeTexture`
- `D3DXCreateTexture`
- `D3DXCreateTextureFromFileExA`
- `D3DXCreateVolumeTexture`
- `D3DXFilterTexture`
- `D3DXGetErrorStringA`
- `D3DXGetFVFVertexSize`
- `D3DXLoadSurfaceFromSurface`

### Generated gitinfo (build-time, not a blocker)

- `GitCommitAuthorName`
- `GitCommitTimeStamp`
- `GitRevision`
- `GitShortSHA1`
- `GitTag`
- `GitUncommittedChanges`

### Defined in a built translation unit behind a disabled #if (build option / platform)

- `FillStackAddresses(void**, unsigned int, unsigned int)`
- `StackDumpFromAddresses(void**, unsigned int, void (*)(char const*))`
- `g_LastErrorDump`
- `getQR2HostingStatus`

### Other / unclassified

- `MSS_auto_cleanup`

### Engine C++ not built at this level

- `ListenerHandleClass::Initialize(SoundBufferClass*)`

## 4. What would resolve them

The causes above say what each symbol *is*. They do not say what makes it go away, and the two get confused: before level 4 the renderer's 272 symbols read as port work when they were only "the build does not include that layer". Each unresolved symbol is therefore also assigned to exactly one pile, and only one of the five is remaining port work.

| Pile | Symbols | Meaning |
|---|---:|---|
| `library-not-linked` | 42 | A library defines it and this configuration links no such library. A link line, not port work. |
| `cut-scope-not-linked` | 82 | A library defines it and this project will never link that library, because the feature is cut scope. Goes away by excising the call sites, not by defining it. |
| `compile-blocked` | 18 | An in-tree translation unit defines it in its source text but that unit does not compile natively yet. The definition exists; the file is the blocker. |
| `harness-artefact` | 9 | An artefact of how this harness is configured: a build-time generated definition it does not generate, one a disabled `#if` removed, or one in a layer this level selection does not build. |
| `no-definition-anywhere` | 22 | Nothing in the repository, the provisioned dependencies or a linkable library defines it. This is the remaining port work. |

The libraries in the `library-not-linked` and `cut-scope-not-linked` piles, the evidence each attribution rests on, and the slice that owns it:

| Library | Pile | Symbols | Evidence files | Why it is not linked | Owner |
|---|---|---:|---:|---|---|
| Miles AIL_* API — the `milesstub`/OpenAL backend | `library-not-linked` | 1 | 7 | `cmake/openal.cmake` builds an OpenAL-backed implementation of the same AIL_* API, and the 32-bit Windows build links the fetched miles-sdk-stub. This harness now builds `Core/Libraries/Source/OpenALAudioDevice` as a support archive and links libopenal, so what is left here is the part of the Miles surface that backend does not implement rather than the whole API. | platform/audio-device (the Miles/OpenAL link) |
| FFmpeg (libavcodec / libavformat / libavutil / libswscale) | `library-not-linked` | 29 | 823 | The video path is the engine's own `RTS_BUILD_OPTION_FFMPEG` route. `fetch-probe-deps.sh` provisions the pinned headers so the code compiles, and nothing installs an FFmpeg runtime for the link. | video/bink-excision-and-harness-headers |
| The window/input backend this configuration does not choose (SDL2, Cocoa) | `library-not-linked` | 12 | 2 | `probe.OPTIONAL_BACKENDS` keeps the SDL2 backend opt-in and the Cocoa backend is Objective-C++, so no target lists either. The definitions are in the tree; a configuration that picks one resolves all of them. | platform/macos-window-compile and platform/window-seam-wiring |
| GameSpy SDK | `cut-scope-not-linked` | 82 | 279 | Online matchmaking is permanently cut scope (docs/porting/native-port-plan.md). The SDK's own sources are provisioned and define these symbols, so this is a link refused rather than one that is missing: they disappear when the call sites go, which is `online/absent-menu-seam`'s work, and must not be stubbed to make a link pass. | online/absent-menu-seam |

Evidence is the provisioned sources or headers that define the symbols, not a library found on the measuring machine: the CI container has no FFmpeg or SDL2 runtime, and a pile split that changed with the box would not be a measurement.

## 5. Strict link: is there an executable?

`--strict-link` linked the same archives with no tolerance for unresolved symbols: **failed**, 173 unresolved symbol(s), executable produced: no. Nothing is stubbed to make this pass, and nothing may be — a green strict link bought with stubs would hide exactly the work this number exists to count.

The linker's list and §3's `nm` scan agree, so the categorised list above is the list standing between this build and an executable.

Symbol resolution is necessary and not sufficient; `docs/porting/startability.md` defines what else a first launch needs.

## Reproducing

```sh
bash scripts/ci/fetch-probe-deps.sh
python3 scripts/native-build.py --level 1 --level 2 --level 3 --level 4 --with-shims --strict-link --report docs/porting/native-build-report.md --json native-build.json
```
