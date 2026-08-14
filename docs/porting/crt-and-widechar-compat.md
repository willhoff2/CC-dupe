# CRT, wide-character and misc-header compatibility

The native 64-bit build had a long tail of translation units that failed for reasons unrelated to
any one seam: an MSVC CRT function with no POSIX spelling, a wide-character entry point, a Win32
header that no longer arrives through `PreRTS.h`, a name that collides with POSIX. This slice
cleared that tail. It is not a seam in the sense
[`timing-and-threading.md`](timing-and-threading.md) is — there is no single subsystem behind it —
so what follows is organised by root cause, which is how the failures actually group.

Everything here was measured on Linux x86-64 with clang 14 and the SDKs `scripts/ci/fetch-probe-deps.sh`
fetches. **Nothing in this document was compiled or run on macOS or arm64.** The parts written for
Apple platforms and never executed are listed under [Blind](#blind-written-for-macos-arm64-never-compiled-there).

## Measured

Re-derived from a fresh run, not from the committed report — `scripts/native-build.py --level 1
--level 2 --with-shims` and `scripts/native-port-probe.py`, both with `CLANGXX=clang++-14`.

Both columns were measured on this machine after the rebase onto `main` at `b7187cd7b` (the merge of
#36, which took the `LANMessage` static assert with it), the "before" column in a clean worktree of
that commit:

| | Before (`b7187cd7b`) | After |
|---|---:|---:|
| Objects produced (levels 1–2, shimmed) | 679 / 717 | **704 / 716** |
| Translation units with no object | 38 | **12** |
| Unresolved symbols at link | 376 | **273** |
| Probe-clean, shimmed | 664 / 743 | **683 / 742** |
| Probe-clean, native (no shims at all) | 634 / 743 | **639 / 742** |
| Probe-clean but uncompilable | 0 | **0** |

The denominator moved by one in each tool for a reason worth stating: see
[Two measurement bugs](#two-measurement-bugs).

The native column moving at all is the useful signal. Declaring a Win32 entry point in the shim
directory buys a compile in the *shimmed* column only; the five units the native column gained came
from the changes that are real portable implementations rather than declarations —
`Utility/fpu_compat.h`, the wide CRT helpers, `strtok_r`, `wwdebug.cpp` and the `pause` rename.

## By root cause

### 1. MSVC CRT functions with no POSIX spelling

`Utility/fpu_compat.h` (new, reached from `Utility/compat.h`) implements `_fpreset`, `_statusfp`
and `_controlfp` over `<fenv.h>`, and defines the `_MCW_*`/`_RC_*`/`_PC_*` bits so that call sites
keep their MSVC spelling.

One of the three is not portable, and the header says so rather than pretending:

- **Rounding mode** maps onto `fesetround`/`fegetround` exactly.
- **Precision control** does not map at all. `GameLogic.cpp::setFPMode()` asks for `_PC_24` — round
  every intermediate result to 24-bit significand — which is an x87 control-word feature. On x86-64
  SSE2 and on arm64 NEON the width of an arithmetic operation is a property of the instruction, not
  of a mode register. The request is accepted and dropped, and `_statusfp()` reads the precision
  field back as `_PC_64`.
- **Exception mask** is not mapped, and `_MCW_EM` is deliberately *not* defined, so a future call
  site that wants it fails to compile instead of being silently ignored.

This is the first identified reason a native build cannot be assumed to reproduce Windows
simulation results bit-for-bit. It is a lockstep/replay-determinism concern, not a compile concern,
and it belongs on the determinism work rather than here; see
[`review-and-decisions.md`](review-and-decisions.md).

`Utility/wchar_compat.h` gained `_wtoi`, `_wtol`, `_wtof` and `iswascii`, and
`Utility/mbstring_compat.h` (new) gained `_mbsnccnt`. These are CRT functions rather than Win32
entry points, so they are real definitions — nothing has to be implemented behind them later.

`Utility/endian_compat.h` gained `<stddef.h>`. It used `size_t` without including anything that
defines it and compiled only because every consumer happened to include something else first; the
new gate compiles each compat header on its own, which is how this surfaced.

### 2. Language linkage

The repo has been bitten here before: `_strlwr` was defined with C++ linkage while the GameSpy SDK
declares it inside `extern "C"`, and every translation unit reaching both failed with "different
language linkage" — 33 of them. The new wide CRT helpers are therefore defined inside `extern "C"`
too, matching the linkage a C library gives the names they impersonate.

`scripts/ci/check-crt-compat.py` now gates this. For each name a vendor SDK also declares, it
compiles the compat layer together with the vendor header **in both include orders**, because a
linkage clash is only diagnosed at the second of the two declarations — one order alone can pass
while a consumer that includes them the other way round fails. No vendor header in the tree
declares the wide helpers today; the gate is there for the next one that does.

### 3. POSIX name collisions

`InGamePopupMessage.cpp` had a file-scope `static Bool pause`, which collides with POSIX
`pause(2)`. `<unistd.h>` is not includable-out-of: libstdc++'s `<atomic>` includes it, which
`mempool.h` includes, so the declaration arrives in every GameEngine translation unit. Renamed the
variable to `s_pause`; file-scope static either way, no effect on the Windows build.

`strtok_r.h`/`strtok_r.cpp` declared and defined `strtok_r` themselves under `#ifndef _UNIX`, which
is not the same question as "does this platform have one". Against glibc's declaration, the
redeclaration fails on the exception specification. Narrowed the guard to
`!defined(_UNIX) && defined(_WIN32)`: Windows keeps the vendored implementation byte-for-byte,
everything else uses libc's.

`wwdebug.cpp`'s `Convert_System_Error_To_String()` was guarded `#ifndef _UNIX` while the
`<windows.h>` include above it is `#ifdef _WIN32`, so off Windows it called `FormatMessage` with no
declaration in scope. Gave it an errno branch, which is the counterpart of the Win32 one —
`Get_Last_System_Error()` right below it already returns `errno` off Windows.

### 4. Win32 declarations that no longer arrive through `PreRTS.h`

[`prerts-win32-surgery.md`](prerts-win32-surgery.md) removed the shared Win32 header wall, which is
what makes the remaining boundary attributable per file. Six files still use Win32 APIs and had
lost the declarations: `GameMemory.cpp` (`GlobalAlloc`), `PopupPlayerInfo.cpp` (`OSVERSIONINFO`),
`PopupReplay.cpp` and `Recorder.cpp` (`DeleteFile`/`CopyFile`), `PeerDefs.cpp` (`CreateDirectory`)
and `rcfile.h`
(`HMODULE`/`HRSRC`/`HGLOBAL`). Each got the `#include <windows.h>` it actually needs, with a
comment naming the seam that owns replacing the call — filesystem for the file APIs, memory for
`GlobalAlloc`, nothing for `rcfile.h` (a PE resource is a Windows-only concept; the class exists to
read one).

The declaration shim gained the entry points and types those files reference: the `GMEM_*`
constants, `GlobalReAlloc`/`GlobalSize`, `GetSystemDirectoryA`, the version-info trio, and the four
W entry points the engine calls by name (`GetModuleFileNameW`, `GetModuleHandleW`,
`GetDateFormatW`, `GetTimeFormatW`, `FormatMessageW`).

Two fixes in the shim are worth calling out because they were wrong rather than missing:

- `HGLOBAL` and `HLOCAL` were `DECLARE_HANDLE`d into distinct struct pointers. Real `winnt.h`
  typedefs both to `HANDLE`, i.e. to `void*`, and code that hands a `T*` straight to `GlobalFree()`
  — `SystemAllocator.h`, `profile.cpp` — depends on the implicit conversion a distinct handle type
  refuses.
- `GUID` had no `operator==`, which `<guiddef.h>` provides inline. `WebBrowser.cpp`'s
  `QueryInterface` compares IIDs.

**The W entry points are declarations only, and they hide a real problem.** MSVC's `wchar_t` is 2
bytes; `wchar_t` on Linux and macOS is 4. `WideChar` is `wchar_t`
([`widechar-fallout.md`](widechar-fallout.md) concluded that changing it is a separate, larger
piece of work, and this slice does not touch it). So a native *implementation* of
`GetDateFormatW`-into-a-`WideChar`-buffer cannot simply be the platform's wide API: the buffer
element width differs from the one the rest of the engine's serialisation assumes. Declaring the
entry point lets `GameState.cpp` and `ReplaySimulation.cpp` compile and moves the blocker into the
unresolved-symbol count, where it is visible and counted, rather than leaving it as a failed
compile. Implementing them is the wide-character slice's job, after the `char16_t` decision.

### 5. Missing headers — one real, three cut scope

| Header | Consumer | What was done |
|---|---|---|
| `mbstring.h` | `IMEManager.cpp` | Forwards to the new `Utility/mbstring_compat.h`. `_mbsnccnt` is implemented over `mblen()`. |
| `gnu_regex.h` | `regexpr.cpp` | Forwards to glibc's `<regex.h>`, which has the GNU `re_*` extensions this file uses. `regexpr.cpp` is commented out of `WWLib/CMakeLists.txt`, so this is compile-only support. |
| `winnt.h` | `verchk.cpp` | Declares `IMAGE_DOS_HEADER`/`IMAGE_FILE_HEADER`. These are the on-disk PE layout, which is fixed by the file format, so they are exact — but there is no PE image to read off Windows. |
| `oaidl.h`, `atlcom.h`, `comutil.h` | `WWCOMUtil.cpp`, `FEBDispatch.h` | OLE Automation and ATL's COM object plumbing, declaration-only. |
| `EABrowserDispatch/BrowserDispatch.h` | six files | **Loud stub.** See below. |

`_mbsnccnt` deserves a caveat: MSVC's multibyte CRT works in the process code page, and there is no
code page off Windows, so the shim walks the C locale's multibyte encoding (UTF-8 in any locale a
native build will run under). For ASCII the two agree exactly; for the CJK composition strings this
function exists to measure they do not. The IME path is Windows-only regardless — it is driven by
`ImmGetCompositionStringW` — so this exists to let the translation unit compile, not to make Asian
input work off Windows. Note also that `mbstring.h` lives in the probe's shim directory, not in
`Dependencies/Utility`, so a real non-Windows CMake build still would not find it; `IMEManager.cpp`
fails on `HWND` anyway, which the window/input slice owns.

### 6. Deliberate loud stubs

`EABrowserDispatch/BrowserDispatch.h` is the interface header of EA's embedded Internet Explorer
control — a Windows-only in-process COM server that is **not in this repository on any platform**.
Six translation units cannot be parsed without it: `INIWebpageURL.cpp`, `WebBrowser.cpp`,
`GameEngine.cpp`, and the WOL ladder/login/welcome menus.

The embedded browser renders the WOL/GameSpy online screens. Single-player Zero Hour never creates
one; the retail source has `GameEngine.cpp`'s `initSubsystem` call for `TheWebBrowser` commented
out. It is cut scope, and the stub is written to fail loudly rather than plausibly:

- `IID_IBrowserDispatch` is declared, never defined, and is deliberately **not** EA's IID. A native
  link that reaches it fails at link time instead of talking to a browser that is not there.
- The vtable layout is inferred from `WebBrowser.h`'s own override list, not from EA's IDL. It must
  never be used to talk to a real BrowserDispatch DLL.
- `atlcom.h`'s `BEGIN_COM_MAP` expands to a `QueryInterface` that always returns `E_NOINTERFACE`,
  and `CComObject<T>::CreateInstance` is declared without a definition. There is no COM runtime off
  Windows; an implementation that appeared to work would hide that.

Crash reporting is stubbed on the same terms, and was already: `imagehlp.h` gained the four
`StackWalk` callback typedefs `DbgHelpLoader.h` names in its own signatures. `DbgHelpGuard.cpp` and
`DbgHelpLoader.cpp` now compile; nothing behind them does anything.

### 7. Two measurement bugs

Both were found by re-deriving the failure list instead of trusting the committed report, and both
made the tooling count translation units the configured build does not compile:

- `native-build.py` reported 717 units where the probe reported 742 minus its own exclusions,
  because `native_probe_targets.target_sources()` walked the source directories unfiltered while
  the probe excluded the opt-in SDL2 window backend. `platform_window_sdl2.cpp` therefore appeared
  as a permanent "`SDL.h` not found" failure in one tool and not in the other. Both now filter
  through the probe's `is_measured_source()`.
- `GameMemoryNull.cpp` is the *alternative* memory implementation, mutually exclusive with the
  default `RTS_GAMEMEMORY_ENABLE=ON`. Compiling it alongside `GameMemory.cpp` produces
  redefinitions of `DynamicMemoryAllocator` and friends — correctly, because that configuration
  does not exist. It is now excluded from the measured set the same way, as
  `EXCLUSIVE_ALTERNATIVES`.

Neither is a fix to the port; both are fixes to what the numbers mean. The denominator is 1 lower
in each tool as a result, and the `redefinition of 'DynamicMemoryAllocator'` line that was in the
committed report is gone because the report should never have contained it.

## The gate

`scripts/ci/check-crt-compat.py`, wired into the native probe job. The baselines already notice
when a compat header stops working; this notices the two failure modes they cannot:

- **linkage** — each name a vendor SDK also declares, compiled with the compat layer in both
  include orders (4 checks).
- **presence** — each helper reached through a call expression rather than an address, because
  several are macros on one platform and functions on another (10 checks).
- **standalone** — every `Dependencies/Utility/Utility/*_compat.h` and every shim header compiled
  on its own, twice, so a header that only works second fails here rather than in a consumer
  months later (24 + 39 checks).

## Still open

- The W entry points were declared, not implemented, when this slice landed: four of them
  (`GetModuleFileNameW`, `GetDateFormatW`, `GetTimeFormatW`, `FormatMessageW`) were unresolved
  symbols out of 43 in the `Win32 API` category and 273 in total. They are implemented over
  `wchar_t` as it stands by [`win32-file-api-seam.md`](win32-file-api-seam.md); the
  `WideChar`/`char16_t` decision in [`widechar-fallout.md`](widechar-fallout.md) is still open and
  will change what those bytes mean, not whether the functions exist.
- `GlobalAlloc`/`GlobalLock`/`GlobalFree` under `GameMemory.cpp` were still Win32; they are now
  defined by [`win32-file-api-seam.md`](win32-file-api-seam.md) over `malloc`. `GlobalSize` has no
  call site in the linked set.
- `DeleteFile`/`CopyFile`/`CreateDirectory` were still Win32; also
  [`win32-file-api-seam.md`](win32-file-api-seam.md).
- `_PC_24` is dropped, so native arithmetic is not bit-identical to the Windows build.
- Not in this slice, and left failing — the whole of what remains, 12 units by first diagnostic:
  3 on window/input types (`HWND`, `HKL`), 4 on GameSpy sockets/SNMP (`HOSTENT`, `recvfrom`,
  `AsnObjectIdentifier`), 4 on `HRESULT` inside `WWDownload/ftp.h`, and `GameEngine.cpp` on
  `SetWindowText`/`SetWindowTextW`. The `HRESULT` group is the same root cause as §4 — a file that
  needs `<windows.h>` and no longer gets it — but `WWDownload` is not this slice's to claim, and
  `SetWindowText` is a window entry point owned by the window/input slice, so its declaration is
  left to them rather than added to the shim from here.

## Blind: written for macOS/arm64, never compiled there

Written on Linux, reasoned from documentation, and **not executed anywhere**:

- The `iswascii` guard. The BSD C library, and so macOS, already supplies it — as a macro on some
  releases and as an inline function on others, which is why the guard tests `!defined(iswascii)`
  *and* `!defined(__APPLE__)`. Whether that is the right shape on a current macOS SDK is unverified.
- `gnu_regex.h` forwarding to `<regex.h>`. The `re_*` functions and `RE_*` syntax constants
  `regexpr.cpp` uses are **GNU extensions that macOS and the BSDs do not have**. This header cannot
  work there as written. It is not currently reached (`regexpr.cpp` is commented out of the CMake
  target), so this is a deferred problem, not a hidden one.
- The claim that `_PC_24` has no arm64 NEON equivalent. True for x86-64 SSE2 by inspection; for
  arm64 it is read from the architecture reference, not tested.
- `_mbsnccnt` over `mblen()` assumes a UTF-8 `MB_CUR_MAX` locale. Not exercised on macOS, and not
  exercised with a real composition string anywhere.
