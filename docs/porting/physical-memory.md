# Physical memory off Windows

## 1. Defect

`CPUDetectClass::Init_Memory()` (`Core/Libraries/Source/WWVegas/WWLib/cpudetect.cpp`) was
`#warning FIX Init_Memory()` off Windows, so `TotalPhysicalMemory` stayed 0. These values were read
with lldb from a running native Generals build on a 16 GiB M1 Pro:

```
CPUDetectClass::TotalPhysicalMemory    = 0
TheGameLODManager->m_numRAM            = 0
TheGameLODManager->m_memPassed         = false
TheGameLODManager->m_idealDetailLevel  = STATIC_GAME_LOD_LOW
```

`GameLODManager::init()` sets `m_memPassed` only when `m_numRAM / 256 MB >= 0.94`. With 0 it was
false on every machine. Both games read it, so both games were affected:

| Consumer | With `m_memPassed` false |
|---|---|
| `Intro::doEALogoMovie()`, `doSizzleMovie()` | the `...640` movies instead of the full-size ones |
| `GameLODManager::applyStaticLODLevel()` | `requestedTrees = m_memPassed`: no trees at the preset detail levels |
| `SinglePlayerLoadScreen::init()` (Generals) | the "min spec" branch: no briefing video, so no map or unit pictures, only the text |
| `ShellGameLoadScreen::init()` | no title-screen image on first load |
| `GameLODManager::init()` | the ideal detail level was auto-detected with 0 RAM: Low |

The last row persists. `IdealStaticGameLOD` is written to `Options.ini` once and not re-detected, so
an existing install keeps Low until the player changes it in Options.

## 2. Fix

Off Windows, `Init_Memory()` calls the platform layer's `GlobalMemoryStatus()`
(`platform/platform_win32_module.cpp`), which already reads `sysctl hw.memsize` on macOS and
`sysinfo()` on Linux. It is reached through `platform/platform_win32_compat.h`, as the rest of the
layer is. The unshimmed probe mode has no `<windows.h>`, and there the `#warning` stays. Windows is
unchanged.

## 3. Test

`scripts/native-physical-memory-test.py` compiles
`Core/GameEngine/Source/Common/System/tests/physical_memory_test.cpp` with `cpudetect.cpp`'s own
flags and links it against the native build's WWLib archive, so `CPUDetectClass`'s static
initialiser runs as it does in the game. The total must equal the OS's own figure and pass
`GameLODManager`'s test. It runs in CI's `native-build-renderer` job.

| | Total |
|---|---:|
| this branch, macOS arm64 | 17179869184 (16.0 GiB) = `sysctl hw.memsize` |
| the same harness linked against a pre-fix WWLib archive | **0** |
