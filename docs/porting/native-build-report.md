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
| `Core/Libraries/Source/WWVegas/WWLib` | 72 | 72 | 72 |
| `Core/Libraries/Source/WWVegas/WWDebug` | 3 | 3 | 3 |
| `Core/Libraries/Source/WWVegas/WWSaveLoad` | 12 | 12 | 12 |
| `Core/GameEngine` | 204 | 210 | 204 |
| `GeneralsMD/Code/GameEngine` | 378 | 380 | 378 |
| `Core/GameEngineDevice` | 43 | 70 | 43 |
| `GeneralsMD/Code/GameEngineDevice` | 24 | 39 | 24 |
| `GeneralsMD/Code/Main` | 0 | 1 | 0 |
| **Total** | **783** | **834** | **783** |

51 translation units produced no object file:

| Translation unit | First diagnostic |
|---|---|
| `Core/GameEngine/Source/GameNetwork/DownloadManager.cpp` | `` |
| `Core/GameEngine/Source/GameNetwork/GameSpy/MainMenuUtils.cpp` | `` |
| `Core/GameEngine/Source/GameNetwork/GameSpy/StagingRoomGameInfo.cpp` | `unknown type name 'AsnObjectIdentifier'` |
| `Core/GameEngine/Source/GameNetwork/GameSpy/Thread/GameResultsThread.cpp` | `unknown type name 'HOSTENT'` |
| `Core/GameEngine/Source/GameNetwork/GameSpy/Thread/PeerThread.cpp` | `no matching function for call to 'recvfrom'` |
| `Core/GameEngine/Source/GameNetwork/GameSpy/Thread/PingThread.cpp` | `unknown type name 'HOSTENT'` |
| `Core/GameEngineDevice/Source/MilesAudioDevice/MilesAudioManager.cpp` | `unknown type name 'LONG'; did you mean 'ULONG'?` |
| `Core/GameEngineDevice/Source/StdDevice/Common/StdLocalFileSystem.cpp` | `use of undeclared identifier 'unNormalized'; did you mean 'nonNormalized'?` |
| `Core/GameEngineDevice/Source/VideoDevice/Bink/BinkVideoPlayer.cpp` | `` |
| `Core/GameEngineDevice/Source/VideoDevice/FFmpeg/FFmpegFile.cpp` | `'Common/File.h' file not found` |
| `Core/GameEngineDevice/Source/VideoDevice/FFmpeg/FFmpegVideoPlayer.cpp` | `'libavcodec/avcodec.h' file not found` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DTankDraw.cpp` | `` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DTankTruckDraw.cpp` | `` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DTruckDraw.cpp` | `` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/GUI/GUICallbacks/W3DControlBar.cpp` | `` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/GUI/Gadget/W3DCheckBox.cpp` | `` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/GUI/Gadget/W3DComboBox.cpp` | `` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/GUI/Gadget/W3DHorizontalSlider.cpp` | `` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/GUI/Gadget/W3DListBox.cpp` | `` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/GUI/Gadget/W3DProgressBar.cpp` | `` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/GUI/Gadget/W3DPushButton.cpp` | `` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/GUI/Gadget/W3DRadioButton.cpp` | `` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/GUI/Gadget/W3DStaticText.cpp` | `` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/GUI/Gadget/W3DTabControl.cpp` | `` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/GUI/Gadget/W3DTextEntry.cpp` | `` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/GUI/Gadget/W3DVerticalSlider.cpp` | `` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DScreenshot.cpp` | `'stb_image_write.h' file not found` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DShaderManager.cpp` | `no member named 'DriverVersion' in '_D3DADAPTER_IDENTIFIER8'` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DSmudge.cpp` | `` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DView.cpp` | `` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/Water/W3DWater.cpp` | `use of undeclared identifier 'D3DXMatrixInverse'` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/Water/W3DWaterTracks.cpp` | `use of undeclared identifier 'VK_F5'` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/stb_image_write_impl.cpp` | `'stb_image_write.h' file not found` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/DownloadMenu.cpp` | `` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/MainMenu.cpp` | `` |
| `GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/Common/System/W3DFunctionLexicon.cpp` | `` |
| `GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/GUI/W3DGameFont.cpp` | `cannot initialize object parameter of type 'const RefCountClass' with an expression of type 'FontCharsClass'` |
| `GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/GUI/W3DGameWindow.cpp` | `` |
| `GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/GUI/W3DGameWindowManager.cpp` | `` |
| `GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/Shadow/W3DProjectedShadow.cpp` | `` |
| `GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/Shadow/W3DShadow.cpp` | `` |
| `GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/Shadow/W3DVolumetricShadow.cpp` | `` |
| `GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DAssetManager.cpp` | `use of undeclared identifier 'lstrcpyn'` |
| `GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DDisplay.cpp` | `` |
| `GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DDisplayString.cpp` | `` |
| `GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DDisplayStringManager.cpp` | `` |
| `GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DGameClient.cpp` | `` |
| `GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DWebBrowser.cpp` | `'IDispatch' does not refer to a value` |
| `GeneralsMD/Code/GameEngineDevice/Source/Win32Device/Common/Win32GameEngine.cpp` | `` |
| `GeneralsMD/Code/GameEngineDevice/Source/Win32Device/Common/Win32OSDisplay.cpp` | `use of undeclared identifier 'MB_APPLMODAL'` |
| `GeneralsMD/Code/Main/PlatformMain.cpp` | `` |

## 2. How much the probe over-reports

**0 translation units that the probe calls clean fail to compile**, out of 783 probe-clean units (0%). These are the codegen-class failures `-fsyntax-only` cannot see.

## 3. Undefined symbols

The 9 archives were linked into one binary with `--whole-archive`, plus the third-party libraries the engine calls into: `libthirdparty_lzhl`, `z (system)` (binary produced: no; clean link: no). **435 distinct symbols are unresolved** once libc, libstdc++, libm, libpthread and the CRT/unwinder symbols are discounted. The full categorised list is in the JSON output; examples follow each count.

| Cause | Symbols |
|---|---:|
| Defined in a layer not built here (renderer / audio) | 221 |
| Defined in a translation unit that failed to compile | 146 |
| GameSpy SDK (cut scope, not linked) | 33 |
| Defined only in a backend this configuration excludes (SDL2 / Cocoa) | 12 |
| Defined in a built translation unit behind a disabled #if (build option / platform) | 8 |
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
- `DX8Wrapper::Has_Stencil()`
- `DX8Wrapper::Draw_Triangles(unsigned short, unsigned short, unsigned short, unsigned short)`
- `DX8Wrapper::Set_Index_Buffer(IndexBufferClass const*, unsigned short)`
- `DX8Wrapper::Set_Index_Buffer(DynamicIBAccessClass const&, unsigned short)`
- `DX8Wrapper::_Create_DX8_Surface(unsigned int, unsigned int, WW3DFormat)`
- `DX8Wrapper::getBackBufferFormat()`
- `DX8Wrapper::Set_Light_Environment(LightEnvironmentClass*)`
- `DX8Wrapper::Apply_Render_State_Changes()`
- `DX8Wrapper::Get_DX8_Render_State_Value_Name(StringClass&, _D3DRENDERSTATETYPE, unsigned int)`
- `DX8Wrapper::Invalidate_Cached_Render_States()`
- …and 206 more

### Defined in a translation unit that failed to compile

- `TheGameResultsQueue`
- `TheGameSpyPeerMessageQueue`
- `ThePinger`
- `TheProjectedShadowManager`
- `TheSmudgeManager`
- `TheW3DProjectedShadowManager`
- `TheW3DShadowManager`
- `TheWaterRenderObj`
- `TheWin32Mouse`
- `doSkyBoxSet(bool)`
- `MainMenuInit(WindowLayout*, void*)`
- `MainMenuInput(GameWindow*, unsigned int, unsigned int, unsigned int)`
- `MainMenuSystem(GameWindow*, unsigned int, unsigned int, unsigned int)`
- `MainMenuUpdate(WindowLayout*, void*)`
- `PrepareShadows()`
- …and 131 more

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
