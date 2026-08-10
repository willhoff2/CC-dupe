# Native build target — slice notes (stopped early, nothing measured yet)

**Status: not started beyond a survey.** This slice was halted by the requester before any
compiler or linker was invoked. Nothing in this document is a measurement. It exists so the
next attempt starts from the survey rather than repeating it.

## Goal of the slice (unchanged, still open)

Everything published about this port so far comes from `scripts/native-port-probe.py`, which
runs `clang++ -fsyntax-only`. The current headline (**621 / 742 native**, **650 / 742 shimmed**)
is a *syntax* result. No object file has been produced for a 64-bit non-Windows target and no
linker has been run, so codegen errors and undefined symbols are entirely unmeasured.

The slice is to add a real `native` CMake configuration (Linux x86-64 and macOS arm64) that
compiles objects, links static libraries bottom-up, and reports the undefined-symbol list
grouped by cause. A playable binary is explicitly *not* the goal; an honest, reproducible build
target plus an accurate blocker list is.

## Intended build order

1. `Core/Libraries/Source/Compression`, `WWVegas/WWMath`, `WWVegas/WWLib`, `WWVegas/WWSaveLoad`,
   `WWVegas/WWDebug` — expected closest to buildable.
2. `Core/GameEngine`, then `GeneralsMD/Code/GameEngine`.
3. `GameEngineDevice`, `WW3D2`, `Main` only if cheap — known to be far off.

For each level: compiled objects first, then a linked static library, then the undefined symbols
blocking the next level up.

## Constraints that apply to the next attempt

- **No fake Win32 compatibility layer.** `scripts/native-port-shims/` is declaration-only, exists
  only so the probe measures something, and must not be linked against. Real seams already exist
  and should be extended instead: `Core/Libraries/Source/WWVegas/WWLib/platform/`
  (`platform_{mutex,path,process,settings,thread,time}`), plus the sockets/text-encoding,
  filesystem/registry and audio-device seams.
- **Stub loudly.** Where no implementation exists, abort with a clear message and list the stub in
  the report. Never a silent no-op returning success.
- **Windows stays green** — `./scripts/docker-build.sh --clean --game zh` and all 13 `GenCI`
  configurations. This outranks native progress.
- Whatever builds gets wired into `.github/workflows/native-port.yml` on `ubuntu-latest` and
  `macos-15` (`macos-14` cannot run MoltenVK, relevant only to renderer jobs). The job must fail
  if a previously building target stops building — no `continue-on-error`.

## Survey findings before the stop

These are observations from reading the tree, not results.

- `CMakePresets.json` has `vc6*`, `win32*`, `unix` and `mingw-w64-i686*` presets. The `unix`
  preset inherits `default-vcpkg` and is a **32-bit vcpkg** configuration — it is not a usable
  base for a 64-bit native preset, so a new preset is needed rather than a tweak to that one.
- The top-level `CMakeLists.txt` already branches on
  `(WIN32 OR CMAKE_SYSTEM MATCHES Windows) AND CMAKE_SIZEOF_VOID_P EQUAL 4`: off that path it
  skips `miles.cmake`/`bink.cmake`/`dx8.cmake` and includes `cmake/openal.cmake`, which supplies
  the `milesstub` target from an OpenAL backend. So a non-Windows 64-bit configure already has a
  defined dependency path and does not immediately demand the retail Miles/Bink/DX8 SDKs.
- `Core/CMakeLists.txt` builds the Core renderer/device pieces as `INTERFACE` libraries whose
  sources are compiled inside the game-specific targets. Any native target set has to account for
  that: `Core/GameEngineDevice` and `WW3D2` are not standalone compiled libraries today.
- Per `native-port-plan.md`, `Core/Libraries/Source` must **not** be a blanket include path under
  libstdc++ — its `debug/` and `profile/` subdirectories shadow libstdc++'s internal `<debug/...>`
  and `<profile/...>` header directories and produce ~6,000 spurious errors. The native build
  needs per-library include paths.
- The probe relies on force-including `Utility/CppMacros.h` (mirroring MSVC's
  `/FIUtility/CppMacros.h`) and on `-fms-extensions`; dropping the latter costs ~65 errors from
  `__int64`/`__forceinline` alone. A real build target has to make the same choices deliberately.

## What remains unknown

Everything the slice was meant to answer:

- whether any of the bottom-level libraries actually produce object files;
- the undefined-symbol list, categorised (Win32 API still reached, D3D8, Miles/Bink, GameSpy,
  engine code not yet built, x86 assembly) with counts;
- the codegen-class errors `-fsyntax-only` structurally cannot see — templates only instantiated
  on emission, inline assembly, alignment/ABI issues, declared-but-undefined symbols;
- **specifically: how many files the probe reports "clean" then fail to compile or link.** That
  number determines how much to trust every figure published so far, including 621 / 742.

Until that is measured, the probe numbers should be quoted as syntax-only results and nothing
more.
