# Next slice — scope

## Live state and handoff (written 2026-09-04 at `main` = #156)

This section is the orchestration handoff for whoever picks the port up next — a fresh session
should be able to read only this, `review-and-decisions.md`, the skills under `.agents/skills/`
and the org knowledge notes, and continue. Everything below the rule is the older phase scoping
and is kept for history. Evidence categories are kept apart on purpose: **MEASURED** (with the
method), **INFERRED** (from source, not run), **UNMEASURED**.

### Where the port stands (waves 10–14)

Target unchanged: single-player Zero Hour, skirmish + campaign, native arm64 macOS, no Wine at
runtime. Explicit cuts unchanged (tools, GameSpy, retail save/replay compatibility, `Generals/Code`).

MEASURED on the M1 Pro against `main`, real game data (see the per-topic docs and the JSON under
`ci-baselines/` for the figures — do not quote numbers from prose):

- Native arm64 build links, missions and skirmish render, structures textured
  (`playability-probe.md`, `renderer-first-frame.md`).
- Skirmish soak with pause and combat, callbacks on the engine thread, no crash, quit exit 0 on
  the pre-#155 shim (`audio-callback-soak-macos-arm64.json`, `memory-shutdown-order.md`).
- Radar panel renders; texture/surface counts are in elements and flat apart from AI build-out.
- Cursor visible with retail `.ANI` hotspots; with #157 the pointer also moves under capture
  (`mouse-cursor-seam.md` §6.2, `cursor-capture-macos-arm64.json`).
- Human audibility: music and SFX reach the speakers; the constant crackle reported before #155
  was not heard again after #155 (one human listen, not instrumented).

MEASURED on Linux only: USA-01 campaign scripts/objectives/triggers execute headless
(`campaign-flow-probe.md`); movie audio decodes and renders through the Miles/AIL sample
(`movie-audio-linux.json`, `sound-effects-chain.md` §9).

### Open PRs and in-flight work at the time of writing

| Item | State | What is still owed |
|---|---|---|
| #157 cursor confinement (`platform_window_cocoa.mm` only) | open, CI pending | **human trackpad confirmation** on the Mac in both `-win` and fullscreen — the exit criterion. Nobody has done it yet. |
| Wave 14.2 quit abort (exception out of `OpenALAudio::Library::~Library()` at static destruction → `std::terminate`) | Linux child in flight, no PR | reproduce, bisect #153 vs #155, shim-only non-throwing destruction, red/green test that exits without `AIL_shutdown()`. If no PR exists when you read this, the slice restarts from `docs/porting/memory-shutdown-order.md` and the prompt in this table. |
| #156 movie audio | merged | Mac human audibility UNMEASURED. |

### Ranked residuals (next slices, in order)

1. **Quit abort** (above) — every clean quit on `main` #155/#156 reportedly aborts; user-observed
   on the Mac, mechanism INFERRED from source (joinable service thread or diagnostics/static
   destruction order), not yet reproduced.
2. **Cursor human confirmation** for #157, then the UNMEASURED cursor rows: drag across the edge
   with a button held, multi-display, hide-on-cinematic, seven scroll directions.
3. **Fullscreen is not fullscreen**: on the Mac "fullscreen" is an 800×600 borderless window at
   the top-left of the display, not screen-covering. Window-mode seam
   (`WWLib/platform/platform_window_cocoa.mm`), separate slice.
4. **Mac audibility of movie audio** (#156) and of the stream/SFX path on the current shim, with
   `OPENAL_AUDIO_DIAG=<file>` counters captured during the listen.
5. **Campaign beyond mission entry** on the Mac: natural victory / next-mission transition,
   in-mission movies, EVA (Linux probe exists, Mac UNMEASURED).
6. Logic FPS without probe overhead; ≥20-min soak on the post-#155 shim.

### Process rules that were learned the hard way

- One Mac session at a time on the `will-mac` outpost; the M1 Pro is the only Apple Silicon
  machine. Retail data lives in `~/devin-work/zh-data`; the loose files (cursors, movies) are in
  `s3://cc-mac-game-data/zerohour104_loose_data.7z`. Never commit retail data or screenshots of it.
- Branch from **current** `origin/main` and check `git rev-parse origin/main` after any VM
  restart — a stale checkout produced a PR based one merge behind (#156, fixed by rebase).
- One seam per PR; Windows bytes unchanged; the CI Windows VC6 build + replay gate is the oracle.
- Never call `AIL_*` or OpenAL from a debugger attached to the game — it deadlocks the shim.
- Synthetic absolute-coordinate clicks do not test pointer motion under capture; use relative
  HID deltas (`IOHIDPostEvent` with `kIOHIDSetRelativeCursorPosition`) or a human.
- Re-measure before quoting any figure from `docs/porting/*.md`; the JSON baselines are truth.
- When orchestrating children: judge by their messages and ACUs, not `status=working`; a child
  that reports a VM/outpost failure should stop and be resumed, not retried in a loop.

---

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

> Measured 2026-08-17 at commit `3098ef1`: 4 direct / 4 allowlisted, 66 seam-owned; re-measured
> 2026-09-02 at commit `632ba201f`, unchanged. The earlier
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
   the direct count is **4**, all allowlisted in `WW3D2/d3dx8texcreate.cpp` as the paragraph above
   records, and `scripts/ci/check-d3d8-surface.py` holds it there exactly. The next renderer
   slice is Vulkan backend coverage behind `RenderBackendClass`, measured method by method.
