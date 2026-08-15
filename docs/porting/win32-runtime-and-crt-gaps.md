# The kernel32 runtime and CRT gaps

This slice is the small-change cluster the native build was left with after the wave-3 stack: seven
translation units whose first diagnostic was a missing Win32 spelling, a missing CRT spelling, an
upstream typo, or a vendored-header layout difference. The interesting part is not the count but
where each fix belongs, so this document is organised by that decision.

Everything here follows the port's rule: the portable implementation goes *under* the existing Win32
or MSVC spelling, so no consumer grows an `#ifdef` and Windows keeps compiling the same code it
compiled before.

## Where each fix went, and why

| Symptom | Fix | Why there |
|---|---|---|
| `unknown type name 'HANDLE'` (`MilesAudioManager.cpp`) | one `#include <windows.h>` in the consumer, plus `CreateMutex`/`WaitForSingleObject`/`ReleaseMutex`/`CloseHandle` in `WWLib/platform/platform_win32_kernel.cpp` | the file reached those spellings through `<dsound.h>`, which is Windows-only and is already `#ifdef`-ed out; `<windows.h>` is where they are declared on both platforms. Wave 3 deliberately made `MilesAudioManager.h`'s member the `void*` a `HANDLE` is, so nothing had to become a Win32 type: what was missing was the *handle behaviour*, and `platform_mutex.cpp` (a `std::recursive_timed_mutex`) already had the body |
| `use of undeclared identifier 'InterlockedExchangePointer'` (`W3DScreenshot.cpp`, through `mpsc_intrusive_queue.h`) | the Interlocked family in `platform_win32_kernel.cpp`, declared by the shim's `<windows.h>` | `Utility/interlocked_adapter.h` exists for the opposite problem — VC6 has no pointer-width intrinsics — and is a *fallback for old MSVC*, not a portability layer; the port's `<windows.h>` declares the real names, so the definitions belong beside the other kernel32 ones. `interlocked_adapter.h` and `mpsc_intrusive_queue.h` are untouched |
| `unknown type name 'HANDLE'`, again, for `CreateThread` (same file) | `CreateThread` over `WWPlatform::Thread_Create()` | not in the original list: it only surfaced once `W3DScreenshot.cpp` compiled, as a *new unresolved Win32 symbol*, which `check-win32-undefined.py` fails on. The screenshot writer is a fire-and-forget worker, which is exactly what the existing thread seam provides |
| `no member named 'DriverVersion' in '_D3DADAPTER_IDENTIFIER8'` (`W3DShaderManager.cpp`) | `Dependencies/Utility/Utility/d3d8_compat.h`, a `D3D8AdapterDriverVersion()` accessor | this was a vendored-header question, and the answer is that nothing is missing from the vendored header. The retail DX8 SDK spells the field twice, `LARGE_INTEGER DriverVersion` under `#ifdef _WIN32` and a `DriverVersionLowPart`/`HighPart` pair without it — the SDK's own 2001-era Win32-versus-Win16 split, not a portability guard. A 64-bit clang build does not define `_WIN32` (in this codebase that macro means Windows), lands in the Win16 branch, and sees two `DWORD`s describing the same eight bytes. So no field is added anywhere; the accessor reads whichever spelling the header gave, and the consumer loses a `LARGE_INTEGER` type pun it did not need |
| `use of undeclared identifier 'VK_F5'` (`W3DWaterTracks.cpp`) | `VK_F1`–`VK_F12` and `VK_INSERT` in `scripts/native-port-shims/windows.h`, and `GetAsyncKeyState`/`GetKeyState` in `platform_win32_user.cpp` | virtual key codes are `<windows.h>` constants, and the shim already carried the rest of that block. The two poll functions are stubs (see below), not a new input path: `docs/porting/window-event-loop.md` owns replacing the calls with `Window_Key_Is_Down()` |
| `use of undeclared identifier 'lstrcpyn'` (`W3DAssetManager.cpp`, and `lstrcat` behind it) | `lstrlenA`/`lstrcpyA`/`lstrcpynA`/`lstrcatA`/`lstrcmpA`/`lstrcmpiA` in `platform_win32_kernel.cpp`, with the ANSI aliases in the shim | these look like CRT functions but are kernel32 entry points with C linkage, and they do not behave like their CRT lookalikes: `lstrcpyn`'s length *counts the terminator*, which is precisely what the call site relies on to cut `"container.mesh"` at the dot. Putting them in `Dependencies/Utility` would have made them CRT spellings, and made the `n`-off-by-one easy to get wrong |
| `no member named '_strdup' in the global namespace` (`W3DDisplay.cpp`, through `WW3D2/agg_def.h`) | `#define _strdup strdup` in `Dependencies/Utility/Utility/string_compat.h` | this *is* a CRT spelling — MSVC's conforming name for POSIX `strdup`, which the header already includes `<string.h>` for — so it belongs in the family that already holds `_stricmp` and `_strnicmp`. A macro rather than an inline wrapper because the engine writes `::_strdup`, which only resolves for a global name |
| `use of undeclared identifier 'MB_APPLMODAL'` (`Win32OSDisplay.cpp`, and `MB_ICONSTOP`, `MessageBoxW`, `SetThreadExecutionState` behind it) | the `MB_*`/`ES_*` constants in the shim, `MessageBoxA`/`MessageBoxW` over `WWPlatform::Dialog_Message_Box()` and `SetThreadExecutionState` in `platform_win32_user.cpp` | `MB_APPLMODAL` is `0` on Windows; defining it as anything else would change behaviour, so it is the canonical value, in the block that already had `MB_OK` and friends |
| `use of undeclared identifier 'unNormalized'` (`StdLocalFileSystem.cpp`) | renamed to `nonNormalized` | an actual upstream typo, not a portability gap. The local is declared as `nonNormalized`; the `std::replace` line that misspells it is inside `#ifndef _WIN32`, so MSVC never compiled it and nothing caught it. This is the one consumer edit that is a genuine bug fix |

The heap functions (`GetProcessHeap`/`HeapAlloc`/`HeapFree`/`HeapSize`) come along with the same
kernel32 file: the shim declared them, nothing defined them, and they are two lines each over
`malloc`.

## What the portable side actually does

`platform_win32_kernel.cpp` and `platform_win32_user.cpp` join the existing
`platform_win32_file.cpp`/`_locale.cpp`/`_module.cpp`/`_gdi_font.cpp` family: whole-file
`#ifndef _WIN32`, compiled to nothing unless a `<windows.h>` is on the include path, and taking
their declarations (and their C linkage) from that header rather than inventing a second set.

- **Mutexes** are `WWPlatform::Mutex_*` — a `std::recursive_timed_mutex`, which already matches
  Win32's recursive, timed semantics — behind a signature-checked handle. Deliberate differences:
  a name is process-local rather than kernel-wide, and an abandoned mutex is not detected, so
  `WAIT_ABANDONED` never comes back. Nothing in the engine depends on either.
- **Threads** are `WWPlatform::Thread_Create()`, detached. The handle therefore accepts only
  `CloseHandle()`; waiting on it, suspending it or terminating it reports a loud stub and fails,
  and `CREATE_SUSPENDED` fails rather than quietly starting the thread running. The caller this
  slice unblocks — `W3DScreenshot.cpp`'s PNG writer — is fire-and-forget and does none of those
  things; the other three callers (`MiniDumper.cpp`, `WWDownload/FTP.cpp` and a GameSpy unit) are in
  translation units this configuration still does not compile.
- **Interlocked** is clang's `__atomic` builtins with `seq_cst` ordering, returning the values Win32
  returns — the *initial* value for the compare-exchanges, which is how the lock-free queue decides
  whether it won.
- **The heap** is `malloc`, honouring `HEAP_ZERO_MEMORY`. `HeapSize()` reports `(SIZE_T)-1`
  ("unknown") rather than a wrong number, because `malloc` has no portable usable-size query.
- **`MessageBoxA`/`MessageBoxW`** go through `WWPlatform::Dialog_Message_Box()`, which is itself a
  loud stderr stub whose `DialogResult` is already numbered as the `ID*` constants are, so the return
  value needs no translation (`platform_dialog.h`). Only the button bits of the flags are read — the
  icon and modality bits mean nothing without a window — and `MessageBoxW` narrows through
  `WideCharToMultiByte(CP_UTF8, …)`.

### Deliberate stubs

| Entry point | Behaviour off Windows |
|---|---|
| `GetAsyncKeyState`, `GetKeyState` | report once on stderr and return 0 (no key down). The real answer is the window seam's `Window_Key_Is_Down()`; rewriting the two call sites to it is that slice's work, not this one's |
| `SetThreadExecutionState` | reports once and returns `ES_CONTINUOUS`. Keeping the display awake is a Windows power API; the macOS equivalent is a product decision that has not been made |
| `WaitForSingleObject`/`SuspendThread`/`TerminateThread` on a thread handle | report and fail, because threads are detached here |
| `HeapSize` | returns `(SIZE_T)-1` |

All of these are off the path to running the game, which is the precedent
`docs/porting/process-and-crash-seam.md` set for stubs; all of them announce themselves.

## The gate

Linking proves nothing about behaviour, so `scripts/native-win32-runtime-test.py` builds
`WWLib/platform/tests/win32_runtime_test.cpp` against the seam and runs 43 assertions: that the mutex
is recursive for its owner, that a zero-timeout wait still takes a free mutex, that `CreateThread`
runs the routine with its parameter, that each Interlocked call returns the value Win32 returns, that
`HEAP_ZERO_MEMORY` zeroes and a zero-byte request still succeeds, and that `lstrcpyn` counts the
terminator. Windows is the oracle for every assertion — the test is written so it builds and passes
against the real API too — and it runs in `native-port-ci.yml` next to the file-API seam's test.

`scripts/ci/check-win32-undefined.py` is the other half: it is what caught `CreateThread` becoming
newly unresolved, and its budget is unchanged by this slice (`GetCursorPos`, `ScreenToClient`,
`SetCursor`, all owned by the window seam).

## Measurements

`clang++-14`, `-std=c++20 -m64`, pinned vendor headers from `scripts/ci/fetch-probe-deps.sh`.

| Measurement | Before (`ac520adf8`) | After |
|---|---|---|
| Native build, levels 1–3, shimmed | 816/836 objects, 20 compile failures | 826/839 objects, 13 compile failures |
| Probe, native (no shims) | 664/754 clean | 666/757 clean |
| Probe, shimmed | 706/754 clean | 709/757 clean |
| Undefined Win32 API symbols | 3 (budget 3) | 3 (budget 3) |
| Unresolved symbols, total | 457 | 546 |

The denominator moves by three: `platform_win32_kernel.cpp`, `platform_win32_user.cpp` and the
behaviour test are new translation units. Two of the three are clean in both probe modes; the test
includes `<windows.h>` directly, so it is clean shimmed and not clean unshimmed, exactly like the
file-API seam's test.

The total unresolved count *rises*, which is progress rather than regression: seven translation units
that used to fail to compile now link, so their references stop being hidden behind their own
failure. The report's categories show where the 89 go: `Defined in a translation unit that failed to
compile` falls 86 → 61 as those units start compiling, `Defined in a layer not built here
(renderer / audio)` rises 272 → 321, and a `Miles Sound System` bucket of 60 appears, which is
`MilesAudioManager.cpp` reaching a sound library this build does not link — the whole point of
compiling it. `Direct3D 8 / DirectX` goes 3 → 6 for the same reason (`W3DShaderManager.cpp`). The two
categories that would signal a new *dependency* rather than a newly visible one — `Win32 API` (3, at
budget) and `COM / OLE` — are unchanged, which is what `check-win32-undefined.py` asserts.

## Still open

- `W3DDisplay.cpp` is the one unit in the cluster that still fails, and no longer for a reason in
  this slice: its `_strdup` diagnostic is gone, and what is left is `LPDISPATCH` from
  `WW3D2/dx8webbrowser.h` (the COM/browser slice) and a missing `WinMain.h` from
  `GeneralsMD/Code/Main` (the entry-point slice).
- `GetAsyncKeyState`/`GetKeyState` should stop being stubs and start being
  `Window_Key_Is_Down()` — the two call sites in `W3DWaterTracks.cpp` and `Win32DIKeyboard.cpp` are
  in the window seam's scope.
- `SetThreadExecutionState` needs the product decision about macOS power assertions before it can be
  anything but a stub.
- Named mutexes being process-local would matter if the port ever wanted the Win32
  single-instance-by-mutex trick; the port's single-instance check is
  `WWPlatform::Process_*` instead (`docs/porting/process-and-crash-seam.md`), so it does not.
