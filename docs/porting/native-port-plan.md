# Native macOS/Linux port — phased plan

Target: **single-player Zero Hour (skirmish + campaign) running natively on Apple Silicon
macOS**, 64-bit, no Wine.

Explicitly out of scope: the Win32/MFC tools (WorldBuilder, W3DView, GUIEdit, ImagePacker,
ParticleEditor), GameSpy online matchmaking, and binary/replay/save compatibility with retail
1.04. Those cuts are what make the project finishable; see the effort table at the end.

## Phase 0 — Measurement (done)

`scripts/native-port-probe.py` compiles the platform-independent libraries with
`clang++ -fsyntax-only -std=c++20 -m64` (no Windows SDK, no Wine) and categorises every
diagnostic. Baseline is in [`native-port-probe-report.md`](native-port-probe-report.md).

Headline result: **106 of 147 translation units (72%) compile clean under native 64-bit
clang**, with **88 errors** remaining in the other 41. The `Core/Libraries` slice is much
closer to portable than the raw `windows.h` counts suggest.

Findings worth recording:

- Force-including `Utility/CppMacros.h` (as the MSVC build does via `/FIUtility/CppMacros.h`)
  is required; without it the VC6 compatibility macros are undefined everywhere.
- Under `-fms-extensions` clang predeclares `_lrotl` and `_ReturnAddress` as builtins, which
  collide with the shims in `Utility/intrin_compat.h`; the shims now defer to the builtins.
  Keeping `-fms-extensions` is worth it — dropping it costs 65 new errors from `__int64` and
  `__forceinline` alone, so replacing those keywords is its own mechanical pass.
- `Core/Libraries/Source` must **not** be on the include path for a libstdc++ build: its
  `debug/` and `profile/` subdirectories shadow libstdc++'s internal `<debug/...>` and
  `<profile/...>` header directories and generate ~6,000 spurious errors inside the standard
  library. Any future native build system needs per-library include paths, not a blanket one.

## Phase 1 — Portable core libraries (~200–400 h)

Get `Compression`, `WWMath`, `WWLib`, `WWDebug`, `WWSaveLoad`, `debug`, `profile` building and
unit-testable natively. Remaining work by category, from the probe:

| Work item | Errors | Notes |
|---|---:|---|
| Win32 API calls in `mpu.cpp`, `mutex.cpp`, `thread.cpp`, `registry.cpp`, `wwdebug.cpp` | 44 | Needs the Phase 3 thread/timing/registry shims |
| `windows.h` / vendored headers absent off-Windows (`d3d8types.h`, `imagehlp.h`, `oaidl.h`, `gnu_regex.h`, `CompLibHeader/lzhl.h`) | 29 | Vendor, stub, or exclude the file |
| Win32 types (`LARGE_INTEGER`, `HANDLE`, `HKEY`, `HMODULE`, `HRSRC`) | 10 | Same shim layer |
| Misc (`main` signature in the bundled `debug` test programs, `strtok_r` exception spec) | 5 | Per-site fixes |

Already fixed in this pass, taking the slice from 11% to 72% clean:

- `#include <wctype.h>` in `WWLib/stringex.h` for `towlower` (100 errors, on its own worth 54
  percentage points).
- `sint64` in `cpudetect.h` was typedef'd only under `WIN32` or `_UNIX`; falls back to
  `long long` now.
- Removed the 10 `register` storage-class specifiers in `lzo1x_c.cpp` / `lzo1x_d.cpp`, which
  C++17 no longer allows.
- Deferred to clang's builtin `_lrotl` / `_ReturnAddress` in `Utility/intrin_compat.h`.

What is left is no longer incidental: essentially every remaining error is a real Win32 API
dependency, so Phase 1 now converges into Phase 3 rather than standing alone.

## Phase 2 — 64-bit correctness (~600–1,200 h)

Apple Silicon has no 32-bit mode, so this is mandatory and blocks most later work. Rewrite the
save/load serialisation to explicit fixed-width fields instead of raw struct dumps, audit
pointer↔int casts, and fix the custom allocators. Dropping retail save/replay compatibility is
what keeps this from doubling.

## Phase 3 — Platform abstraction (~400–800 h)

One SDL3-backed layer providing window, event pump, input, timing, threads and filesystem.
Shim the WndProc message model so the ~287 `HWND`-touching files compile largely unchanged
rather than being rewritten. Delete the 19 inline `__asm` blocks in favour of plain C++.

## Phase 4 — Renderer (~600–1,200 h, critical path)

Retarget `DX8Wrapper` — the engine's existing D3D8 abstraction — rather than writing a Metal
backend by hand. Preferred route: D3D8→Vulkan translation running on MoltenVK. The engine is
fixed-function-era and uses a small slice of D3D8, which is why this is a fraction of a
from-scratch renderer.

## Phase 5 — Audio and video (~250–450 h)

Rewrite the ~10 Miles Sound System call sites the engine actually needs against OpenAL; drop
DirectSound. Video is largely solved: enable the existing `RTS_BUILD_OPTION_FFMPEG` path in
place of Bink.

## Phase 6 — Campaign enablement (~400–800 h)

Mission scripting, triggers and objectives are platform-independent C++ and come for free. The
real cost is save/load (Phase 2), cutscene playback, speech/EVA dialogue with subtitles,
scripted cinematic camera paths that exercise renderer paths skirmish never touches, and
playing through ~20 missions to verify parity.

## Phase 7 — Integration and QA (~600–1,200 h)

Does not parallelise. Gameplay-parity testing, performance work, packaging as a `.app`.

## Effort and parallelism

| Phase | Hours | Parallelisable |
|---|---:|---|
| 1 Portable core libraries | 200–400 | yes, split per library |
| 2 64-bit correctness | 600–1,200 | partly; blocks others, land early |
| 3 Platform abstraction | 400–800 | yes, once the layer exists |
| 4 Renderer | 600–1,200 | 2–3 people max, critical path |
| 5 Audio/video | 250–450 | yes, isolated |
| 6 Campaign | 400–800 | yes |
| 7 Integration/QA | 600–1,200 | no |
| **Total** | **~3,000–6,000** | 4–6 engineers, ~12–18 months |

Independently startable right now, with no dependency on the renderer: the `register`/`__asm`
cleanups, the fixed-width typedef consolidation, the OpenAL audio layer, and filesystem/path
portability.

## Reproducing the measurement

```sh
python3 scripts/native-port-probe.py --report docs/porting/native-port-probe-report.md
```
