# Next slice — scope

Measured after Phase 0/1. Numbers are from the repo as it stands, not estimates.

## What the measurement changed

The earlier "287 files touch `HWND`" figure was repo-wide, and repo-wide includes everything the
plan already cuts. Split by area:

| Area | `HWND` files | `windows.h` files | In scope? |
|---|---:|---:|---|
| `Core/Tools` + `GeneralsMD/Code/Tools` (WorldBuilder, W3DView, GUIEdit, ImagePacker…) | 140 | 90 | **no** — cut, stays on Wine |
| `Generals/Code` (base game, not Zero Hour) | 36 | 23 | **no** — ZH only |
| `GeneralsMD` engine + device layers | 5 | 5 | yes |
| `Core/GameEngine`, `Core/GameEngineDevice`, `Core/Libraries` | 15 | 44 | yes |

So the platform layer faces roughly **20 `HWND` files and 49 `windows.h` files**, not 287 and 167.

Better still, the 338k-LOC Zero Hour game engine is already almost platform-clean:

| `GeneralsMD/Code/GameEngine` (757 files, 338,846 LOC) | count |
|---|---:|
| files including `windows.h` | 1 |
| files referencing `HWND` | 1 |
| files referencing `d3d8`/`d3dx8` | 2 |

Gameplay, AI, scripting, INI parsing and the WND GUI system are portable C++ already. The
Win32 and D3D dependencies are concentrated in the device layers exactly where a port wants
them.

The renderer surface is similarly contained. All Direct3D 8 traffic goes through
`DX8CALL(...)` in `dx8wrapper`: **13,517 LOC of `dx8*` files, 45 call sites, 34 distinct D3D8
device methods**. Retargeting that one wrapper is the whole renderer job — no engine-wide
rewrite.

## Revised estimates

| Phase | Was | Now | Why |
|---|---:|---:|---|
| 3 Platform abstraction | 400–800 h | **250–450 h** | ~20 `HWND` files in scope, not 287 |
| 4 Renderer | 600–1,200 h | **500–900 h** | 34 distinct D3D8 entry points behind one wrapper |
| Total (cut scope, skirmish + campaign) | 3,000–6,000 h | **2,600–5,200 h** | |

## Proposed next slice: Phase 3a — threading, timing, registry

Smallest unit that clears the 88 remaining `Core/Libraries` errors and unblocks everything
downstream. Does not touch the renderer, so it can run in parallel with renderer spikes.

Scope:

- `platform/` abstraction with the same shape as the existing Win32 calls, over:
  - **threads** — `thread.cpp` (`_beginthread`, `CreateEvent`, `WaitForSingleObject`) → `std::thread`, `std::condition_variable`
  - **mutex** — `mutex.cpp` (`CRITICAL_SECTION`, `CreateMutex`) → `std::mutex`, `std::recursive_mutex`
  - **timing** — `mpu.cpp` (`LARGE_INTEGER`, `QueryPerformanceCounter`) → `std::chrono::steady_clock`
  - **registry** — `registry.cpp` (`HKEY`) → a small INI/plist-backed settings store
- Leave `wwdebug.cpp` crash reporting and `DbgHelp*` stubbed on non-Windows; they need
  `imagehlp.h` and are not on the path to running the game.
- Extend `native-port-probe.py` to cover `Core/GameEngine` so the next slice starts measured.

Exit criterion: `Core/Libraries` at **147/147** translation units clean under native 64-bit
clang, Windows Docker build still green.

Estimated 60–120 h. Roughly a third of it is the registry replacement, which needs a decision
on where settings live on macOS.

## After that, in dependency order

1. **Phase 2, 64-bit correctness** — must land before the renderer work is worth debugging.
   Start with the save/load serialisation rewrite, since campaign depends on it.
2. **Phase 4 renderer spike** — prove one triangle through `DX8CALL` → Vulkan → MoltenVK before
   committing to the full backend. A spike is ~40 h and de-risks the largest line item.
