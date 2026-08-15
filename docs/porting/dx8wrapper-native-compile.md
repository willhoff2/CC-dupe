# Compiling `dx8wrapper.cpp` off Windows at 64-bit

`Core/Libraries/Source/WWVegas/WW3D2/dx8wrapper.cpp` was the last non-cut compile failure in the
level 1-4 native build. It holds the definitions of the whole `DX8Wrapper::*` surface, so as long as
it failed to compile, ~86 symbols the engine already defines were reported unresolved.

Measured at `6df9b180a` (before) and on this branch (after), `clang++-14` on Ubuntu 22.04:

```sh
./scripts/ci/fetch-probe-deps.sh
CLANGXX=clang++-14 python3 scripts/native-build.py \
  --level 1 --level 2 --level 3 --level 4 --with-shims --strict-link \
  --report docs/porting/native-build-report.md \
  --json docs/porting/ci-baselines/native-build-shimmed-level1-2-3-4.json
```

| levels 1-4, shimmed | before | after |
|---|---:|---:|
| objects | 968/972 | **969/972** |
| compile failures | 4 | **3** (all three the cut-scope GameSpy units) |
| strict-link unresolved symbols | 250 | **173** |
| `compile-blocked` pile | 108 | **18** (the GameSpy remainder) |
| `no-definition-anywhere` pile | 9 | **22** (see below) |
| strict link produces an executable | no | no |

## What actually stopped it compiling

The first diagnostic the baseline records is not the whole story — it records one per file. Compiling
the file alone with `-ferror-limit=0` and the harness' own flags gives the complete list, and the
`DriverVersion` member DirectX itself gates on `_WIN32` was already routed through the D3D8 accessor
by #73. What remained was two classes:

1. **user32 window metrics and the GDI gamma fallback.** The windowed-device sizing path calls
   `GetWindowLong`/`AdjustWindowRect`/`SetWindowPos` and centres the window with
   `MonitorFromWindow`/`GetMonitorInfo`/`MONITORINFO`; the gamma path falls back to
   `GetDesktopWindow`/`GetDC`/`SetDeviceGammaRamp` when the device cannot set a ramp. None of those
   spellings existed in `scripts/native-port-shims/windows.h`, so they are declared there —
   declarations only, under the Win32 spelling, with the call sites untouched.
2. **One real 64-bit defect.** `Validate_Device()` passed `unsigned long *` to `ValidateDevice`,
   whose parameter is `DWORD *`. On 32-bit MSVC the two are the same type; at LP64 `unsigned long` is
   64 bits wide and the call does not compile. Spelling the local `DWORD` fixes the width on both
   platforms and cannot change what Windows does.

Neither change touches behaviour on Windows: one is a non-Windows-only header, the other is a local's
type spelled as the API spells it.

## `no-definition-anywhere` grew, 9 → 22, and that is the finding

Making a translation unit compile stops its calls hiding behind the compile failure, exactly as
happened when #65 made `W3DDisplay.cpp` compile and exposed `IsIconic`. Thirteen symbols became
visible. **None is stubbed here**, and none belongs to this slice:

| newly visible | owner |
|---|---|
| `AdjustWindowRect`, `GetClientRect`, `GetDesktopWindow`, `GetMonitorInfoA`, `GetWindowLongA`, `MonitorFromWindow`, `SetWindowPos`, `SetDeviceGammaRamp` | the window/input seam (`Core/Libraries/Source/WWVegas/WWLib/platform/platform_win32_user.cpp` and the GDI seam), where `GetCursorPos`/`IsIconic`/`ScreenToClient`/`SetCursor` already sit in the same budget |
| `D3DXCreateTexture`, `D3DXCreateCubeTexture`, `D3DXCreateVolumeTexture`, `D3DXCreateTextureFromFileExA`, `D3DXGetErrorStringA` | the D3DX slices (`renderer/methods-and-d3dx`, `renderer/d3dx8-math-entrypoints`), which already own the four D3DX symbols in the pile |

`docs/porting/ci-baselines/win32-undefined-budget.json` is widened by the eight Win32 names for that
reason: they are now *referenced* by a translation unit that compiles, which is a more honest state
than a file that does not build, but they are not yet *defined*. The strict link still produces no
executable, and the count to watch is that these fall to 0 as the two owning seams land.

## Numbers elsewhere that this supersedes

`docs/porting/startability.md` and `docs/porting/ww3d2-and-download-headers.md` quote the 968/972,
250, 108 and 9 figures from before this slice. The table above and
`docs/porting/ci-baselines/native-build-shimmed-level1-2-3-4.json` are the current measurement.
