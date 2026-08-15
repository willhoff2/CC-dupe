# The legacy `debug` and `profile` libraries

`Core/Libraries/Source/debug` (`core_debug`) and `Core/Libraries/Source/profile`
(`core_profile_legacy`) are EA's 2001-era assert/logging/crash-reporting library and its companion
profiler. They are the layer that turns a retail crash into a stack, and they were the
worst-scoring targets in [`STATUS.md`](STATUS.md): **1 of 20** debug translation units and **0 of
6** profile ones compiled natively.

This slice is the same shape as the other seam slices ([`renderer-seam.md`](renderer-seam.md) is
the house style): measure, put the portable implementation *under the existing spelling* so no
call site changes, leave Windows byte-for-byte, and write down what is deliberately not
abstracted.
[`process-and-crash-seam.md`](process-and-crash-seam.md) covers the engine's *other* crash path
(`StackDump.cpp`, `MiniDumper.cpp`) and explicitly left this library alone; that is the part being
picked up here. The minidump remains a Windows-only feature, and this document does not change
that.

## What is in scope, and the honest denominator

The probe walks directories, so its 20 and 6 include files no CMake target builds. What the build
actually compiles is 10 debug + 5 profile translation units:

| | translation units the build compiles | probe-only files in the same tree |
| --- | --- | --- |
| `core_debug` | `debug_cmd`, `debug_debug`, `debug_except`, `debug_getdefaultcommands`, `debug_internal`, `debug_io_con`, `debug_io_flat`, `debug_io_net`, `debug_io_ods`, `debug_stack` | `debug_purecall.cpp` (includes a `_pch.h` that does not exist in the tree), `debug_dlg/**`, `netserv/**`, `test1`–`test6`, `test2/**` |
| `core_profile_legacy` | `profile`, `profile_cmd`, `profile_funclevel`, `profile_highlevel`, `profile_result` | `test1/test1.cpp` |

The probe-only files are Win32 GUI sample programs and standalone tools (a dialog-based debug
viewer, a named-pipe log server, six sample `main()`s). They are not on the path to running the
game, nothing links them, and porting a Win32 dialog sample would be work with no consumer, so
they are left failing on purpose. The ceiling this slice aims at is therefore every translation
unit either library actually builds — 10 and 5 — not the probe's 20 and 6 (22 and 6 after this
slice adds two files of its own; see [Measured effect](#measured-effect)).

## The 19 + 6 failures, grouped by root cause

Measured with `CLANGXX=clang++-14 python3 scripts/native-port-probe.py`, then again
`--with-shims`, and by reading the error text per file.

| # | Root cause | Where | Decision |
| --- | --- | --- | --- |
| 1 | `#include <windows.h>` — the first fatal error in **all 15** production units | every file, plus `internal.h`, `internal_io.h`, `internal_funclevel.h` | Guard the include, and put the Win32 calls behind a per-library `platform/` implementation |
| 2 | DbgHelp/imagehlp stack walk, 32-bit `_CONTEXT` (`Eip`/`Esp`/`Ebp`), x86 register-capture `__asm` | `debug_stack.cpp` | **Real implementation**: `backtrace()` + `dladdr()` + `__cxa_demangle()` under the existing `DebugStackwalk` spelling |
| 3 | SEH: `_set_se_translator`, `__try`/`__except`, `_EXCEPTION_POINTERS`, plus the ComCtl32 crash dialog and the minidump | `debug_debug.cpp`, `debug_except.cpp` | **Loud stub** + POSIX signal handlers that log a real backtrace. No `.dmp`, no register dump, no dialog |
| 4 | x86 inline asm outside the stack walker, and the MSVC `.CRT$XCB`/`.CRT$XCY` static-init trick | `debug_debug.cpp` | `__builtin_return_address(0)` and the `__attribute__((constructor))` path that already existed for MinGW |
| 5 | `RDTSC` and the `timeGetTime`/`QueryPerformanceCounter` calibration loop | `profile/internal.h`, `profile.cpp` | The existing monotonic clock seam, `WWPlatform::Get_Performance_Counter()`. Precision loss documented below |
| 6 | Win32 console, file, named-pipe, `GlobalAlloc`, `IsBadReadPtr`, `MessageBox`, `GetComputerName`/`GetUserName`/`GetLocalTime` | `debug_io_*.cpp`, `debug_internal.cpp`, `debug_debug.cpp`, `profile.cpp` | `platform/debug_platform.{h,cpp}`; the named pipe is a loud stub, its peer being the Win32-only `netserv` tool |

Two expectations from the brief did **not** show up: there is no `__try`/`__except` in either
library (the SEH use is `_set_se_translator` plus a `__try` in *consumers* of
`Debug::InstallExceptionHandler`), and there is no Win32 module or thread enumeration —
`EnumProcessModules`/`Toolhelp32` appear nowhere in these two directories. The module lookup this
library needs is `SymGetModuleBase`, which is group 2.

## What changed

### `debug/platform/debug_platform.{h,cpp}` (new, compiled only when `NOT WIN32`)

One file, the same conventions as `WWLib/platform/*`: no POSIX types in the header, and every
function documents where it differs from the Win32 call it stands in for. It covers memory
(`malloc` for `GlobalAlloc`), files (`open`/`read`/`write`), the executable path
(`_NSGetExecutablePath` on Apple, `/proc/self/exe` elsewhere), host and user name, local time,
a tick count, terminal detection, fatal reporting, an `IsBadReadPtr` equivalent, the stack
capture, the symboliser, and the fatal-signal handlers.

It also carries `wsprintf`/`wvsprintf` and the `_itoa`/`_ultoa`/`_i64toa`/`_ui64toa` radix
conversions, because this library formats every number with them. Those are CRT debt, not this
seam's business: they are declaration-only in `scripts/native-port-shims/stdlib.h`, so they are
counted as open work by [`crt-and-widechar-compat.md`](crt-and-widechar-compat.md), and if that
slice ever defines them for real these copies should go.

### The stack walk (group 2) — a real implementation, not a stub

`DebugStackwalk::StackWalk()` and both `Signature::GetSymbol()` overloads keep their signatures,
including `StackWalk(Signature&, struct _CONTEXT*)`: off Windows `struct _CONTEXT` stays an
incomplete type, so the declaration and every call site are unchanged. Behind them:

- `StackWalk` → `backtrace()` into a local buffer, dropping its own frames.
- `GetSymbol` → `dladdr()` for the module and nearest symbol, `abi::__cxa_demangle()` for the
  name, and the same output field order the Win32 path produced: address, `module+0x…`,
  `symbol+0x…`.
- `GetDbghelpHandle()` returns `nullptr` and `IsOldDbghelp()` returns `false`, which is what the
  `.dbgcmd` command that prints the DbgHelp version wants to hear.

**One representation change, and it does not touch Windows.** A code address no longer fits in
`unsigned`, so `debug_stack.h` introduces `DebugStackAddr`: `unsigned` on Windows (32-bit, so
`Signature`'s layout, ordering and `%08x` formatting are what they always were) and
`unsigned long long` off it. `Debug::curStackFrame` and the frame hash in `debug_debug.h` follow.
Truncating instead would have made every symbol lookup fail, since a Linux PIE loads above 4 GiB.

What it cannot do: **no file names and no line numbers**. That needs the DWARF tables, i.e.
`llvm-symbolizer`/`atos`/libbacktrace, which is a build-system decision, not a `#ifdef`. The
symbol buffer says `(no dwarf)` rather than reporting a bogus zero, and the module-relative offset
is what you feed `addr2line`/`atos` offline. A frame in a function the linker did not put in a
symbol table (a `static` function, or a stripped binary) resolves to `module+offset` with
`(unknown)` for the symbol; the Windows path with PDBs does better. Linking `-rdynamic` (or
`-Wl,--export-dynamic`) is what makes an executable's own functions visible to `dladdr` on ELF;
the test below links that way, and a future native game binary must too.

### Crash handling (group 3) — a loud stub, said out loud

There is no portable equivalent of any of it, so nothing pretends otherwise:

- `Debug::InstallExceptionHandler()` compiles to nothing off Windows. It installs a *per-thread*
  `_set_se_translator` so a Win32 structured exception can be caught as a C++ exception; POSIX
  signals are not exceptions, cannot be resumed, and there is no standard way to turn one into a
  throw. Callers keep calling it.
- No minidump. **No fake `.dmp` file is written**, because a file a Windows debugger cannot open is
  worse than no file: it looks like a crash dump in a bug report.
- No register or FPU dump: the Windows path reads `EXCEPTION_RECORD` and the `CONTEXT`'s x86
  register set, and both the struct and the registers are gone on arm64. `ucontext_t` could give
  the registers back per architecture; it is not done here.
- No crash dialog. The Windows path builds a ComCtl32 dialog with "ignore/exit/dump"; off Windows
  the message goes to stderr and the fatal path exits.

What replaces it: `Debug::PreStaticInit()` installs `sigaction` handlers on an alternate signal
stack for `SIGSEGV`, `SIGBUS`, `SIGFPE`, `SIGILL` and `SIGABRT`, and the handler logs the signal
name, the build info and a **real symbolised backtrace** through group 2, flushes the log, then
re-raises the signal with the handler reset so the OS still writes a core dump and a debugger
still stops in the right place. That is strictly more than the Windows-only library did off
Windows before (which was nothing), and strictly less than a minidump.

`IsWindowed()` returns `false` off Windows: it exists to decide between a modal dialog and console
output, and window enumeration lives under a different seam
([`window-event-loop.md`](window-event-loop.md)).

### The I/O back ends (group 6)

| back end | Windows | off Windows |
| --- | --- | --- |
| `debug_io_flat` | `CreateFile`/`WriteFile`/`CopyFile`, `\` paths, filename magic from `GetModuleFileName`/`GetComputerName`/`GetUserName`/`GetLocalTime` | `open`/`write`, `/` paths, same magic from `/proc/self/exe`-or-`_NSGetExecutablePath`, `gethostname`, `getpwuid`, `localtime_r` |
| `debug_io_con` | `AllocConsole`, `WriteConsole`, console resize, console *input* for interactive `.dbgcmd` commands | `write()` to stdout when stdout is a terminal. **Input is not implemented**: there is no `AllocConsole` equivalent, so the process never owns a console it may read, and resizing a terminal it did not create would be rude. The interactive command prompt is therefore Windows-only |
| `debug_io_ods` | `OutputDebugString` | `write()` to stderr — there is no system-wide debugger channel |
| `debug_io_net` | a named pipe to the `netserv` log server | **loud unsupported stub**: the peer is a Win32-only tool in this same directory that nothing builds off Windows |

### The profiler's clock (group 5), and what precision that costs

`ProfileGetTime()` was `RDTSC` (VC6 `_asm`, `_rdtsc()` elsewhere) and `GetClockCyclesFast()`
calibrated the cycle rate by spinning against `timeGetTime()` and `QueryPerformanceCounter()`.
Off Windows both go through the clock seam that already exists,
`WWLib/platform/platform_time.{h,cpp}`: `WWPlatform::Get_Performance_Counter()` and
`Get_Performance_Frequency()`, i.e. `std::chrono::steady_clock` in **microseconds**.

Why not a cycle counter: on arm64 `PMCCNTR_EL0` is not readable from user space by default, so
`__builtin_readcyclecounter()` traps. There is `CNTVCT_EL0`, but that is a fixed-frequency timer,
which is what `steady_clock` already gives us with less architecture-specific code.

What is lost, precisely:

- Resolution goes from one CPU cycle (~0.3 ns at 3 GHz) to **1 µs**. Any profiled region shorter
  than a microsecond now measures 0 or 1 ticks. Function-level profiling of small functions is the
  casualty; the high-level named zones are unaffected in practice.
- The unit changes meaning. The profiler prints "clock cycles" and derives its percentages from
  the ratio of a measurement to `GetClockCyclesFast()`; off Windows both numerator and denominator
  are microsecond-based, so **percentages and relative timings stay correct** while the absolute
  "cycles" column is microseconds. It is not renamed, because renaming it would change the
  Windows output too.
- The calibration loop is gone off Windows, and with it its ~1 ms of startup spin and its
  dependence on the CPU running at a fixed frequency — which modern CPUs do not, making the
  Windows number the less trustworthy of the two on any machine that boosts.

`_penter`/`_pleave` function-level instrumentation (`profile_funclevel.cpp`) is a block of MSVC
`__declspec(naked)` x86 assembly and stays Windows-only behind the `HAS_PROFILE`/`_MSC_VER`
guards it already had. It is enabled by an MSVC compiler switch (`/Gh`), so it has no clang
counterpart to be reached from.

## What is deliberately *not* done

- The Win32 sample/tool programs in these directories (`debug_dlg`, `netserv`, `test1`–`test6`,
  `debug_purecall.cpp`, `profile/test1`). No consumer, no CMake target.
- The interactive console command prompt off Windows (see `debug_io_con` above).
- The named-pipe log transport off Windows.
- Register/FPU state in the native crash log, and any form of minidump.
- File names and line numbers in native symbols.
- Anything to do with wide characters. This library is `char`-only; the deferred
  `WideChar`/`wchar_t` question ([`crt-and-widechar-compat.md`](crt-and-widechar-compat.md)) is
  untouched.
- Deleting the duplication between this library's stack walker and `GameEngine`'s
  `StackDump.cpp`. Both now have portable implementations; unifying them is a decision about which
  API survives, which outlives this PR.

## Windows is unchanged

Every Win32 body is inside `#ifdef _WIN32` and was moved, not edited: DbgHelp loading and the
`_StackWalk`/`_SymGetSymFromAddr`/`_SymGetLineFromAddr` calls, the `_CONTEXT` fields, the SEH
translator and unhandled-exception filter, the register/FPU dump, the ComCtl32 dialog, the
minidump, `GlobalAlloc`, the console/named-pipe/`OutputDebugString` back ends, `RDTSC` and the QPC
calibration. `DebugStackAddr` is `unsigned` there, so `Signature`'s layout and every `%08x` are
identical. The new `platform/debug_platform.cpp` is added to `core_debug` only when `NOT WIN32`.
The Wine/VC6 build (`scripts/docker-build.sh`) and the 13 Windows CI configurations are the gate.

## Measured effect

`CLANGXX=clang++-14 python3 scripts/native-port-probe.py` (`-fsyntax-only -std=c++20 -m64`, no
Windows SDK), before from a worktree of `main`, after from this branch, same machine.

| target | native before | native after | shimmed before | shimmed after |
| --- | ---: | ---: | ---: | ---: |
| `Core/Libraries/Source/debug` | 1 / 20 | **12 / 22** | 4 / 20 | **13 / 22** |
| `Core/Libraries/Source/profile` | 0 / 6 | **5 / 6** | 5 / 6 | **5 / 6** |

The denominators grew by two because this slice adds two files the probe walks:
`platform/debug_platform.cpp` and `tests/native_stackwalk_test.cpp`, both of which are clean.

All 15 translation units the build actually compiles — the 10 in `core_debug` and the 5 in
`core_profile_legacy` — are clean natively, up from 1 and 0. Repository totals move 647 / 749 →
663 / 751 native and 694 / 749 → 703 / 751 shimmed (re-measured after rebasing onto `main` with the
Win32 file-API seam, #45, merged). Superseded on `main` at 4306101: 663 / 752 native and
704 / 752 shimmed — the denominator grew by one and the shimmed count with it, because #46 added
`WWMath/d3dx8math.cpp`, not because anything in this slice changed. `docs/porting/STATUS.md` carries
the current totals.

The 10 remaining native failures in `debug` are all sample and tool programs that no CMake target
builds and that this slice deliberately leaves on Wine: `test1/` … `test6/` (five of them fail only
on `'main' must return 'int'`, i.e. a `void main()` from 1998), the `test2/` Win32 GUI sample,
`netserv/` and `debug_dlg/` (both `windows.h`-only), and `debug_purecall.cpp`, which includes a
`_pch.h` that does not exist anywhere in the tree. The one remaining `profile` failure is
`profile/test1/test1.cpp`, the same `void main()` case.

Native `debug` is 12 and shimmed 13 only because `test2/StdAfx.cpp` is satisfied by the
declaration-only shim headers; the production library is at 10 / 10 in both columns.

## The gates

Two, both wired into `native-port-ci.yml` as `debug-profile-seam` (Linux, clang 14) and
`debug-profile-seam-macos` (macOS arm64):

1. `scripts/ci/check-stackwalk-symbols.py` — compiles every translation unit CMake lists for
   `core_debug` and `core_profile_legacy`, then checks with `nm` that the stack walk entry points
   are *defined* and that no Win32 symbol this seam replaced (`Sym*`, `StackWalk64`,
   `MiniDumpWriteDump`, `GlobalAlloc`, `IsBadReadPtr`, `OutputDebugString`, `QueryPerformance*`,
   `timeGetTime`, …) is still referenced. A header-only stub satisfies the compiler and then fails
   the link, which is exactly the state this library was in; this catches that. It also fails if
   the CMake source list drifts from the runner below.
2. `scripts/native-stackwalk-test.py` — links the whole debug library plus
   `debug/tests/native_stackwalk_test.cpp` and **runs** it. The test calls
   `DebugStackwalk::StackWalk` through three deliberately non-inlined frames and asserts the
   symbolised output names all three plus `main`, that the split `GetSymbol` overload resolves the
   innermost frame's module and symbol with a small in-symbol offset, and that a `Signature`
   survives being copied. "It links" and "it produces frames that name my functions" are
   different claims, and only the second one is useful when a native build crashes.

Both run on `macos-15` as well, so the arm64 claim is CI-proven rather than asserted from Linux.
