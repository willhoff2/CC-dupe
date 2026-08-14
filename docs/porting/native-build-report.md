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
| `Core/Libraries/Source/WWVegas/WWMath` | 35 | 35 | 35 |
| `Core/Libraries/Source/WWVegas/WWLib` | 72 | 72 | 72 |
| `Core/Libraries/Source/WWVegas/WWDebug` | 3 | 3 | 3 |
| `Core/Libraries/Source/WWVegas/WWSaveLoad` | 12 | 12 | 12 |
| `Core/GameEngine` | 204 | 210 | 204 |
| `GeneralsMD/Code/GameEngine` | 378 | 380 | 378 |
| `Core/GameEngineDevice` | 26 | 70 | 26 |
| `GeneralsMD/Code/GameEngineDevice` | 14 | 39 | 14 |
| `GeneralsMD/Code/Main` | 0 | 2 | 0 |
| **Total** | **755** | **834** | **755** |

79 translation units produced no object file:

| Translation unit | First diagnostic |
|---|---|
| `Core/GameEngine/Source/GameNetwork/DownloadManager.cpp` | `unknown type name 'HRESULT'; did you mean 'MMRESULT'?` |
| `Core/GameEngine/Source/GameNetwork/GameSpy/MainMenuUtils.cpp` | `unknown type name 'HRESULT'; did you mean 'MMRESULT'?` |
| `Core/GameEngine/Source/GameNetwork/GameSpy/StagingRoomGameInfo.cpp` | `unknown type name 'AsnObjectIdentifier'` |
| `Core/GameEngine/Source/GameNetwork/GameSpy/Thread/GameResultsThread.cpp` | `unknown type name 'HOSTENT'` |
| `Core/GameEngine/Source/GameNetwork/GameSpy/Thread/PeerThread.cpp` | `no matching function for call to 'recvfrom'` |
| `Core/GameEngine/Source/GameNetwork/GameSpy/Thread/PingThread.cpp` | `unknown type name 'HOSTENT'` |
| `Core/GameEngineDevice/Source/MilesAudioDevice/MilesAudioManager.cpp` | `static_assert failed due to requirement 'sizeof (m_status) == sizeof(long)' "Must be size of long, because it is used wi` |
| `Core/GameEngineDevice/Source/StdDevice/Common/StdLocalFileSystem.cpp` | `use of undeclared identifier 'unNormalized'; did you mean 'nonNormalized'?` |
| `Core/GameEngineDevice/Source/VideoDevice/Bink/BinkVideoPlayer.cpp` | `'bink.h' file not found` |
| `Core/GameEngineDevice/Source/VideoDevice/FFmpeg/FFmpegFile.cpp` | `'Common/File.h' file not found` |
| `Core/GameEngineDevice/Source/VideoDevice/FFmpeg/FFmpegVideoPlayer.cpp` | `'libavcodec/avcodec.h' file not found` |
| `Core/GameEngineDevice/Source/W3DDevice/Common/System/W3DRadar.cpp` | `use of undeclared identifier 'TheD3D8RenderBackend'` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/BaseHeightMap.cpp` | `use of undeclared identifier 'TheD3D8RenderBackend'` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/CameraShakeSystem.cpp` | `use of undeclared identifier 'TheD3D8RenderBackend'` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DPropDraw.cpp` | `use of undeclared identifier 'TheD3D8RenderBackend'` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DTankDraw.cpp` | `'bink.h' file not found` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DTankTruckDraw.cpp` | `'bink.h' file not found` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DTreeDraw.cpp` | `use of undeclared identifier 'TheD3D8RenderBackend'` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/Drawable/Draw/W3DTruckDraw.cpp` | `'bink.h' file not found` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/FlatHeightMap.cpp` | `use of undeclared identifier 'TheD3D8RenderBackend'` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/GUI/GUICallbacks/W3DControlBar.cpp` | `unknown type name 'HFONT'` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/GUI/Gadget/W3DCheckBox.cpp` | `unknown type name 'HFONT'` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/GUI/Gadget/W3DComboBox.cpp` | `unknown type name 'HFONT'` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/GUI/Gadget/W3DHorizontalSlider.cpp` | `unknown type name 'HFONT'` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/GUI/Gadget/W3DListBox.cpp` | `unknown type name 'HFONT'` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/GUI/Gadget/W3DProgressBar.cpp` | `unknown type name 'HFONT'` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/GUI/Gadget/W3DPushButton.cpp` | `unknown type name 'HFONT'` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/GUI/Gadget/W3DRadioButton.cpp` | `unknown type name 'HFONT'` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/GUI/Gadget/W3DStaticText.cpp` | `unknown type name 'HFONT'` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/GUI/Gadget/W3DTabControl.cpp` | `unknown type name 'HFONT'` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/GUI/Gadget/W3DTextEntry.cpp` | `unknown type name 'HFONT'` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/GUI/Gadget/W3DVerticalSlider.cpp` | `unknown type name 'HFONT'` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/HeightMap.cpp` | `use of undeclared identifier 'TheD3D8RenderBackend'` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/TerrainTex.cpp` | `use of undeclared identifier 'TheD3D8RenderBackend'` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DMouse.cpp` | `use of undeclared identifier 'TheD3D8RenderBackend'` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DParticleSys.cpp` | `use of undeclared identifier 'TheD3D8RenderBackend'` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DPropBuffer.cpp` | `use of undeclared identifier 'TheD3D8RenderBackend'` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DScreenshot.cpp` | `use of undeclared identifier 'TheD3D8RenderBackend'` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DShaderManager.cpp` | `use of undeclared identifier 'TheD3D8RenderBackend'` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DSmudge.cpp` | `unknown type name 'HRESULT'; did you mean 'MMRESULT'?` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DSnow.cpp` | `use of undeclared identifier 'TheD3D8RenderBackend'` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DTerrainBackground.cpp` | `use of undeclared identifier 'TheD3D8RenderBackend'` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DTerrainTracks.cpp` | `use of undeclared identifier 'TheD3D8RenderBackend'` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DTerrainVisual.cpp` | `use of undeclared identifier 'TheD3D8RenderBackend'` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DTreeBuffer.cpp` | `use of undeclared identifier 'TheD3D8RenderBackend'` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DView.cpp` | `use of undeclared identifier 'TheD3D8RenderBackend'` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/Water/W3DWater.cpp` | `use of undeclared identifier 'TheD3D8RenderBackend'` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/Water/W3DWaterTracks.cpp` | `use of undeclared identifier 'TheD3D8RenderBackend'` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/WorldHeightMap.cpp` | `use of undeclared identifier 'TheD3D8RenderBackend'` |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/stb_image_write_impl.cpp` | `'stb_image_write.h' file not found` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/DownloadMenu.cpp` | `unknown type name 'HRESULT'; did you mean 'MMRESULT'?` |
| `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/MainMenu.cpp` | `unknown type name 'HRESULT'; did you mean 'MMRESULT'?` |
| `GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/Common/System/W3DFunctionLexicon.cpp` | `unknown type name 'HFONT'` |
| `GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/GUI/W3DGameFont.cpp` | `unknown type name 'HFONT'` |
| `GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/GUI/W3DGameWindow.cpp` | `unknown type name 'HFONT'` |
| `GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/GUI/W3DGameWindowManager.cpp` | `unknown type name 'HFONT'` |
| `GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/Shadow/W3DProjectedShadow.cpp` | `use of undeclared identifier 'TheD3D8RenderBackend'` |
| `GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/Shadow/W3DShadow.cpp` | `use of undeclared identifier 'TheD3D8RenderBackend'` |
| `GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/Shadow/W3DVolumetricShadow.cpp` | `use of undeclared identifier 'TheD3D8RenderBackend'` |
| `GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DAssetManager.cpp` | `use of undeclared identifier 'TheD3D8RenderBackend'` |
| `GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DBibBuffer.cpp` | `use of undeclared identifier 'TheD3D8RenderBackend'` |
| `GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DBridgeBuffer.cpp` | `use of undeclared identifier 'TheD3D8RenderBackend'` |
| `GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DCustomEdging.cpp` | `use of undeclared identifier 'TheD3D8RenderBackend'` |
| `GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DDebugIcons.cpp` | `use of undeclared identifier 'TheD3D8RenderBackend'` |
| `GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DDisplay.cpp` | `'bink.h' file not found` |
| `GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DDisplayString.cpp` | `unknown type name 'HFONT'` |
| `GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DDisplayStringManager.cpp` | `unknown type name 'HFONT'` |
| `GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DGameClient.cpp` | `'bink.h' file not found` |
| `GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DRoadBuffer.cpp` | `use of undeclared identifier 'TheD3D8RenderBackend'` |
| `GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DScene.cpp` | `use of undeclared identifier 'TheD3D8RenderBackend'` |
| `GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DShroud.cpp` | `use of undeclared identifier 'TheD3D8RenderBackend'` |
| `GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DStatusCircle.cpp` | `use of undeclared identifier 'TheD3D8RenderBackend'` |
| `GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DWebBrowser.cpp` | `use of undeclared identifier 'TheD3D8RenderBackend'` |
| `GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3dWaypointBuffer.cpp` | `use of undeclared identifier 'TheD3D8RenderBackend'` |
| `GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameLogic/W3DTerrainLogic.cpp` | `use of undeclared identifier 'TheD3D8RenderBackend'` |
| `GeneralsMD/Code/GameEngineDevice/Source/Win32Device/Common/Win32GameEngine.cpp` | `static_assert failed due to requirement 'sizeof (m_status) == sizeof(long)' "Must be size of long, because it is used wi` |
| `GeneralsMD/Code/GameEngineDevice/Source/Win32Device/Common/Win32OSDisplay.cpp` | `use of undeclared identifier 'MB_APPLMODAL'` |
| `GeneralsMD/Code/Main/PlatformMain.cpp` | `static_assert failed due to requirement 'sizeof (m_status) == sizeof(long)' "Must be size of long, because it is used wi` |
| `GeneralsMD/Code/Main/WinMain.cpp` | `'eh.h' file not found` |

## 2. How much the probe over-reports

**0 translation units that the probe calls clean fail to compile**, out of 755 probe-clean units (0%). These are the codegen-class failures `-fsyntax-only` cannot see.

## 3. Undefined symbols

The 9 archives were linked into one binary with `--whole-archive`, plus the third-party libraries the engine calls into: `libthirdparty_lzhl`, `z (system)` (binary produced: no; clean link: no). **346 distinct symbols are unresolved** once libc, libstdc++, libm, libpthread and the CRT/unwinder symbols are discounted. The full categorised list is in the JSON output; examples follow each count.

| Cause | Symbols |
|---|---:|
| Defined in a translation unit that failed to compile | 104 |
| Well-known Dict keys: `Core/GameEngineDevice/Source/W3DDevice/GameClient/WorldHeightMap.cpp` failed to compile | 104 |
| Defined in a layer not built here (renderer / audio) | 72 |
| GameSpy SDK (cut scope, not linked) | 33 |
| Defined only in a backend this configuration excludes (SDL2 / Cocoa) | 12 |
| Defined in a built translation unit behind a disabled #if (build option / platform) | 7 |
| Generated gitinfo (build-time, not a blocker) | 6 |
| Direct3D 8 / DirectX | 5 |
| COM / OLE (browser embedding, cut scope) | 3 |

### Defined in a translation unit that failed to compile

- `ApplicationHInstance`
- `TheGameResultsQueue`
- `TheGameSpyPeerMessageQueue`
- `ThePinger`
- `TheProjectedShadowManager`
- `TheSmudgeManager`
- `TheTerrainTracksRenderObjClassSystem`
- `TheW3DShadowManager`
- `TheWin32Mouse`
- `doSkyBoxSet(bool)`
- `MainMenuInit(WindowLayout*, void*)`
- `MainMenuInput(GameWindow*, unsigned int, unsigned int, unsigned int)`
- `MainMenuSystem(GameWindow*, unsigned int, unsigned int, unsigned int)`
- `MainMenuUpdate(WindowLayout*, void*)`
- `setupGameStart(AsciiString, GameDifficulty)`
- …and 89 more

### Well-known Dict keys: `Core/GameEngineDevice/Source/W3DDevice/GameClient/WorldHeightMap.cpp` failed to compile

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

### Defined in a layer not built here (renderer / audio)

- `SetUnsignedIntInRegistry(std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >, unsigned int)`
- `LightClass::Notify_Added(SceneClass*)`
- `LightClass::Notify_Removed(SceneClass*)`
- `LightClass::Load(ChunkLoadClass&)`
- `LightClass::Save(ChunkSaveClass&)`
- `LightClass::LightClass(LightClass::LightType)`
- `LightClass::~LightClass()`
- `Line3DClass::Set_Opacity(float)`
- `Line3DClass::Reset(Vector3 const&, Vector3 const&)`
- `Line3DClass::Reset(Vector3 const&, Vector3 const&, float)`
- `Line3DClass::Re_Color(float, float, float)`
- `Line3DClass::Line3DClass(Vector3 const&, Vector3 const&, float, float, float, float, float)`
- `ShaderClass::_PresetAdditiveShader`
- `ShaderClass::_PresetAdditiveSpriteShader`
- `SurfaceClass::Lock(int*)`
- …and 57 more

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

- `D3DXVec4Dot(D3DXVECTOR4 const*, D3DXVECTOR4 const*)`
- `D3DXVec4Transform(D3DXVECTOR4*, D3DXVECTOR4 const*, D3DXMATRIX const*)`
- `D3DXMATRIX::D3DXMATRIX(float, float, float, float, float, float, float, float, float, float, float, float, float, float, float, float)`
- `D3DXVECTOR4::D3DXVECTOR4(float, float, float, float)`
- `D3DXVECTOR4::D3DXVECTOR4()`

### COM / OLE (browser embedding, cut scope)

- `IID_IBrowserDispatch`
- `IID_IUnknown`
- `_com_util::ConvertStringToBSTR(char const*)`

## Reproducing

```sh
bash scripts/ci/fetch-probe-deps.sh
python3 scripts/native-build.py --level 1 --level 2 --level 3 --with-shims --report docs/porting/native-build-report.md --json native-build.json
```
