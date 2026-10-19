# Generals renders through DX8Wrapper

The third step of the base-game target (`native-port-plan.md`). Once Generals linked natively
(`generals-native-build.md`), its first launch opened the window and created the Vulkan device, then
exited 139 (SIGSEGV):

```
EXC_BAD_ACCESS (address=0x0)
W3DVolumetricShadowManager::ReAcquireResources() at W3DVolumetricShadow.cpp:3600
    m_pDev->CreateIndexBuffer(...)
```

`m_pDev` is the D3D8 device, and there is none off Windows. Zero Hour had already removed every
direct device call from its game code. This slice replays those commits' `GeneralsMD/` hunks onto
the Generals twins, using the method in `generals-compile.md` §1.

## 1. What was replicated

| Commit | Zero Hour change | Generals files |
|---|---|---|
| `75c62d52e` | shadow renderers through DX8Wrapper | `W3DProjectedShadow.cpp`, `W3DVolumetricShadow.cpp` |
| `dca1b5231` | the last direct renderer calls through DX8Wrapper | `W3DDisplay.cpp`, `W3DScene.cpp`, `W3DShroud.cpp`, `ww3d.cpp` |
| `e471aac3e` | the three mission-frame corruption mechanisms | the six files above, plus `ddsfile.h` |

`dca1b5231`'s `Core/` half already covers both games.

Every hunk applied with a 3-way merge and no conflict. Each Generals file moved toward its Zero Hour
twin, counted as lines differing between the twins:

| File | Before | After |
|---|---:|---:|
| `W3DProjectedShadow.cpp` | 167 | 24 |
| `W3DVolumetricShadow.cpp` | 551 | 358 |
| `W3DDisplay.cpp` | 168 | 166 |
| `W3DScene.cpp` | 196 | 188 |
| `W3DShroud.cpp` | 4 | 2 |
| `ddsfile.h` | 7 | 2 |
| `ww3d.cpp` | 13 | 11 |

After this, neither tree's game code calls the device directly: searching for
`_Get_D3D_Device8()`, `m_pDev` or `LPDIRECT3DDEVICE8` finds only WorldBuilder in each, and the same
five lines in both.

## 2. Result

Measured on macOS arm64 (Apple clang 21), together with `generals-native-build.md`'s entry point,
from a directory of symlinks into the retail base game:

- The build: 946 / 946 objects, strict link with 0 unresolved symbols.
- `./generals -win` reaches the main menu at 30 fps, with the animated shell map behind it.
- Real OS mouse input (`macos-input-drive.py click`) drives SOLO PLAY → USA → NORMAL.
- The USA campaign's first mission loads. Its in-engine intro ("Iraqi War Zone, Operation: Final
  Justice") plays and hands over to the mission, which was still running six minutes after launch.

## 3. Still open

- **The briefing movie is black.** Between NORMAL and the mission, the screen stays black, and the
  log shows `[bink] Packet corrupt` and a `yuv420p` to `rgb565le` conversion. The video path is all
  `Core/` (`LoadScreen.cpp`, `W3DVideoBuffer.cpp`, the FFmpeg player), and `createVideoBuffer()`
  is the same in both games. Zero Hour's mission-briefing movie was never measured either
  (`playability-probe.md` §5), so this may be shared rather than a Generals difference. Not measured.
- **The other Zero Hour runtime fixes** listed in `generals-native-build.md` §4: frame CRC,
  save path, 64-bit save layout.
