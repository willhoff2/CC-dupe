# Concurrent slices — ownership register

Several sessions work port slices in parallel. This file is how they avoid colliding: claim the paths
your slice owns *before* you start, and check that nobody else holds them. It is deliberately a plain
table rather than tooling — the cost of a stale row is a rebase, the cost of two slices silently
editing `dx8wrapper.cpp` is a day.

Hand-maintained. `STATUS.md`, by contrast, is generated; do not put measurements here.

## In flight

| Slice | Owns (paths) | Exit criterion | PR |
|---|---|---|---|
| _example_ `platform/audio-device` | `Core/Libraries/Source/OpenALAudioDevice/**` | every declared `AIL_*` symbol defined; `check-openal-symbols.py` green | #28 |
| `wire/lanmessage-64bit` | `Core/GameEngine/Include/GameNetwork/LANAPI.h`, `LANMessageWire.h`, `Core/GameEngine/Source/GameNetwork/LANAPI*.cpp`, `Core/GameEngine/Source/GameNetwork/GameInfo.cpp`, `{Generals,GeneralsMD}/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/{LanGameOptionsMenu,NetworkDirectConnect,GameInfoWindow}.cpp`, `scripts/native-layout-test.py`, `scripts/ci/check-lanmessage-layout.py`, `docs/porting/lanmessage-64bit.md`, `docs/porting/ci-baselines/*.json` | `sizeof(LANMessage)` identical under `-m32` and `-m64` and `<= MAX_LANAPI_PACKET_SIZE`, asserted in a committed test; the 17 translation units that failed on that `static_assert` compile | tbd |
| `renderer/backend-coverage` | `spikes/renderer/src/{vulkan_backend.cpp,render_backend.h,state_translate.*,fixedfunc_tests.cpp}`, `spikes/renderer/shaders/**`, `spikes/renderer/tools/{backend-coverage-scan.py,backend-coverage-map.json,backend-coverage-baseline.json}`, `scripts/ci/check-backend-coverage.py`, `docs/porting/renderer-surface.md` | a committed coverage gate over the 62 (now 64) measured D3D8 entry points and every render/texture-stage state the engine sets; `check-backend-coverage.py` green with a measured before/after count, pixel-verified on lavapipe | this PR |
| `platform/macos-window-compile` | `Core/Libraries/Source/WWVegas/WWLib/platform/platform_window_cocoa.mm`, `spikes/renderer/src/metal_surface_probe.mm`, `spikes/renderer/tools/macos-window-check.sh`, `spikes/renderer/CMakeLists.txt`, the Apple branch of `Core/Libraries/Source/WWVegas/WWLib/CMakeLists.txt`, `scripts/window-input-scan.py`, `docs/porting/window-event-loop.md`, the `window-seam-macos` job in `.github/workflows/native-port-ci.yml` | a green (not `continue-on-error`) `macos-15` job that compiles the Cocoa backend and asserts the Vulkan/Metal surface facts the runner can prove | this PR |
| `compat/crt-and-widechar` | `scripts/native-port-shims/**` (except `dinput.h`, `windows.h` window/input types), `Dependencies/Utility/**` compat headers, `docs/porting/crt-and-widechar-compat.md`, `scripts/ci/check-crt-compat.py`, plus the serialised `ci-baselines/*.json`, `scripts/native-port-probe.py` and `scripts/native_probe_targets.py` for the duration | native-build compile failures for the CRT/wide-char/misc-header cluster resolved; objects up, unresolved symbols down; baselines regenerated | #38 |
| `build/native-level3-and-thirdparty` | `scripts/native-build.py`, `scripts/native_probe_targets.py`, `scripts/ci/check-native-build-baseline.py`, `cmake/native/CMakeLists.txt`, `docs/porting/ci-baselines/native-build-shimmed-*.json`, `docs/porting/native-build.md`, `docs/porting/native-build-report.md`, the `native-build` job in `.github/workflows/native-port-ci.yml` | a level 3 (`GameEngineDevice` + entry point) build measured and ratcheted in CI, lzhl and zlib linked so the 9 third-party symbols resolve, and every undefined-symbol category attributable to a named file or a named cut | (this slice) |
| `renderer/staging-pool` | `spikes/renderer/src/{staging_workload.cpp,resource_lock_tests.cpp}`, the staging/lock path of `spikes/renderer/src/vulkan_backend.cpp` and the `ResourceStats` block of `render_backend.h`, `spikes/renderer/tools/surface-lock-audit.{py,json}`, `scripts/ci/check-staging-cost.py`, `docs/porting/ci-baselines/staging-cost-ceiling.json` (new file, no shared baseline edited), `docs/porting/renderer-resource-seam.md`, the `spike-resources` steps of `.github/workflows/native-port-ci.yml` | staging buffers pooled behind the unchanged D3D8-shaped lock/unlock contract, with a committed per-mode staging-cost ceiling that CI enforces and that the pre-pool behaviour provably violates; measured peak/steady-state residency in the resource-seam doc instead of "unknown" | (this slice) |
| `platform/window-seam-wiring` | `Core/GameEngine/Include/GameClient/PlatformWindowHost.h`, `Core/GameEngine/Source/GameClient/PlatformWindowHost.cpp`, `Core/GameEngine/Source/Common/System/Debug.cpp`, `Core/GameEngine/Source/GameClient/Input/Keyboard.cpp`, `Core/GameEngineDevice/**/Win32Device/GameClient/**`, `GeneralsMD/Code/GameEngineDevice/Source/Win32Device/Common/Win32GameEngine.cpp`, `GeneralsMD/Code/Main/**`, `Core/Libraries/Source/WWVegas/WWLib/platform/platform_dialog.*`, `scripts/ci/check-window-seam-wiring.py`, `docs/porting/window-event-loop.md` | the engine's non-Windows window/message/input path goes through `platform_window.h`; `check-window-seam-wiring.py` green; native-build probe-clean rises | (this slice) |

Remove your row when the PR merges.

## Serialised paths

At most **one** in-flight slice may touch each of these, because a second concurrent edit produces a
conflict that is not mechanical to resolve, or a measurement that cannot be attributed:

- `docs/porting/ci-baselines/*.json` — two slices refreshing baselines produce numbers neither can
  reproduce.
- `scripts/native-port-probe.py` and `scripts/porting-status.py` — changing the measurement while
  someone else is measuring invalidates both results.
- `Core/Libraries/Source/WWVegas/WW3D2/dx8wrapper.{h,cpp}` and `d3d8renderbackend.cpp` — the renderer
  chokepoint; every renderer slice wants it.
- the root `CMakeLists.txt` and `cmake/*.cmake` — build wiring conflicts are silent until link time.

If your slice needs a serialised path that someone else holds, resequence rather than sharing it.

## Rules that keep concurrent work compatible

1. Branch from current `main`, never from another slice's branch.
2. One seam per PR; no drive-by edits outside your claimed paths.
3. Merge one port PR at a time; after each merge, every in-flight slice rebases onto `main` and
   re-measures before quoting a figure.
4. Additive over invasive: add a seam, a shim or a new implementation file rather than reshaping a
   header that other slices are reading.
5. Prefer extending `scripts/native-port-shims/` and `scripts/ci/` over editing shared engine headers.
6. If two slices genuinely need the same file, that is an escalation, not a merge conflict to be
   resolved later.
