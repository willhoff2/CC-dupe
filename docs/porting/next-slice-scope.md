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

The renderer surface is contained, but **not as contained as first stated here.** The original
claim — "all Direct3D 8 traffic goes through `DX8CALL(...)`: 13,517 LOC of `dx8*` files, 45 call
sites, 34 distinct D3D8 device methods" — was measured properly during the Phase 4 renderer spike
and two thirds of it is wrong:

| | Claimed | Measured |
|---|---:|---:|
| LOC of `dx8*` files | 13,517 | 13,517 |
| `DX8CALL` call sites | 45 | **82** |
| distinct D3D8 methods | 34 | **62** (52 `IDirect3DDevice8` + 10 `IDirect3D8`) |
| total D3D8 call sites | — | **458**, of which only 82 (18%) go through the macro |

At the time of measurement the remaining **376 call sites bypassed `DX8Wrapper` and talked to
`IDirect3DDevice8` directly**, concentrated in `W3DWater.cpp` (129), `W3DShaderManager.cpp` (103),
`W3DVolumetricShadow.cpp` (38) and `W3DProjectedShadow.cpp` (25) — effectively a second renderer
beside the wrapper.

**Since superseded: that routing is done.** The direct (non-wrapper, non-backend) count is now **4**,
all in `WW3D2/d3dx8texcreate.cpp` — d3dx8.lib's own creation entry points reimplemented off Windows,
calling back out on the device their caller passes in — and `scripts/ci/check-d3d8-surface.py`
enforces that allowlist exactly in both directions. A further **66** D3D8 call sites live inside the
seam implementation, `WW3D2/d3d8renderbackend.cpp`. What is left of the renderer job is backend
coverage of the 62 methods, not re-routing call sites. See [`STATUS.md`](STATUS.md) for the current
figure.

> Measured 2026-08-17 at commit `3098ef1`: 4 direct / 4 allowlisted, 66 seam-owned. The earlier
> "0 direct, 64 seam-owned" in this paragraph is superseded — the allowlist went 3 → 4 in the D3DX
> routing slice (`renderer-first-frame.md`).

Full enumeration, Vulkan mapping and working proof-of-concept: `docs/porting/renderer-surface.md`
and `spikes/renderer/`.

## Revised estimates

| Phase | Was | Now | Why |
|---|---:|---:|---|
| 3 Platform abstraction | 400–800 h | **250–450 h** | ~20 `HWND` files in scope, not 287 |
| 4 Renderer | 600–1,200 h | **700–1,300 h** | 62 D3D8 entry points; the 376 direct call sites have since been routed through the wrapper — see `renderer-surface.md` |
| Total (cut scope, skirmish + campaign) | 3,000–6,000 h | **2,800–5,600 h** | |

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
2. ~~**Phase 4 renderer spike**~~ — **done**, see `docs/porting/renderer-surface.md` and
   `spikes/renderer/`. A textured triangle renders through a `DX8Wrapper`-shaped abstraction on
   Vulkan; the two architectural risks (immutable pipelines, the `SetTextureStageState` cascade)
   both have working solutions. Verified on Linux only — running it through MoltenVK on macOS is
   the one open item.
3. ~~**Route the 376 direct D3D8 call sites through `DX8Wrapper`, while still on D3D8.**~~ — **done**:
   the direct count is 4 — the allowlisted `WW3D2/d3dx8texcreate.cpp` entry points, re-measured on
   `main` at commit `632ba20` — and `scripts/ci/check-d3d8-surface.py` holds it there. The next renderer
   slice is Vulkan backend coverage behind `RenderBackendClass`, measured method by method.
