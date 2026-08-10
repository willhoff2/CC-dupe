# Process, single-instance and crash-reporting seam

Covers the two rows **Process / single-instance** and **Crash reporting** of the table
"Platform behaviour left for a later slice" in
[`prerts-win32-surgery.md`](prerts-win32-surgery.md).

House style is [`renderer-seam.md`](renderer-seam.md): measure first, put the seam where the
policy already lives, leave the Windows path behaviourally identical, and write down what was
deliberately *not* abstracted.

## What was measured first

`rg` over the in-scope tree — `Core/GameEngine`, `Core/GameEngineDevice`, `Core/Libraries`,
`GeneralsMD/Code/{GameEngine,GameEngineDevice,Libraries,Main}`. `Core/Tools`,
`GeneralsMD/Code/Tools` and `Generals/Code` are out of scope and are counted separately.

| API | In-scope call sites | Files | Where | Out-of-scope sites |
| --- | --- | --- | --- | --- |
| `__try` | **2** | 2 | `MiniDumper.cpp`, `WWLib/thread.cpp` | 0 |
| `__except` | **2** | 2 | same two files | 0 |
| `CreateProcess` / `CreateProcessW` | 2 | 2 | `WorkerProcess.cpp`, `Audio/urllaunch.cpp` | 11 (Tools, Generals) |
| `ShellExecute` | **0** | 0 | — | 1 (`Core/Tools/Launcher/patch.cpp`) |
| `_spawnl` (any `_spawn*`) | 2 | 1 | `GeneralsMD .../Menus/MainMenu.cpp` | 11 (Tools, Generals) |
| `CreateMutex` | 6 | 5 | `ClientInstance.cpp` (2), `WWLib/mutex.cpp`, `WWAudio/SoundSceneObj.cpp`, `WWDebug/wwmemlog.cpp`, `MilesAudioDevice/MilesAudioManager.cpp` | 3 |
| `OpenMutex` | **0** | 0 | — | 2 (`Core/Tools/Autorun/autorun.cpp`) |
| `CreatePipe` | 1 | 1 | `WorkerProcess.cpp` | 1 |
| `TerminateProcess` | 1 | 1 | `WorkerProcess.cpp` | 1 in-tree legacy (`Libraries/Source/debug`) |
| `SetUnhandledExceptionFilter` | 1 | 1 | `GeneralsMD/Code/Main/WinMain.cpp` | 1 legacy `Libraries/Source/debug` |

### The SEH number, and why it is the good news

**There are exactly two `__try`/`__except` blocks in the in-scope tree**, and both are already
inside `#if defined(_MSC_VER)` with a non-SEH fallback next to them:

- `Core/GameEngine/Source/Common/System/MiniDumper.cpp` — wraps the minidump write itself.
- `Core/Libraries/Source/WWVegas/WWLib/thread.cpp` — wraps the thread body so a crash in a
  worker thread is reported.

So SEH, which clang cannot express off Windows at all, is **not** a structural problem for this
engine: the error handling that matters is `DEBUG_CRASH`/`assert` and C++ exceptions, not
`__except`. This contradicts the reasonable prior expectation that a 2001 Win32 codebase would be
riddled with `__try`; it is worth recording precisely because it removes a feared blocker. The
remaining Win32-specific error handling is the *unhandled exception filter* in `WinMain.cpp`, one
call site, which is entry-point code and stays Windows-only.

## What changed

### Process / single-instance

New non-Windows-only file `Core/Libraries/Source/WWVegas/WWLib/platform/platform_process.{h,cpp}`,
alongside the existing `platform_mutex`, `platform_thread`, `platform_time` and
`platform_settings`. It follows the same conventions as those files: opaque `void*` handles, no
POSIX types in the header, and each function documents where it *differs* from the Win32 call it
stands in for. Three capabilities, nothing more:

1. `Instance_Lock_Acquire` / `Instance_Lock_Release` — single-instance detection.
2. `Process_Spawn_Detached` — `_spawnl(_P_NOWAIT, ...)`.
3. `Child_Process_Start/Read/Wait/Kill/Close` — a child process whose stdout/stderr is captured.

Call sites:

- `ClientInstance.{h,cpp}` — the `HANDLE s_mutexHandle` member became an opaque `void*
  s_instanceLock` and the two `CreateMutex` calls became one `acquireInstanceLock()` helper with a
  Windows and a non-Windows arm. `ClientInstance.h` no longer includes `<windows.h>`. The public
  API is unchanged.
- `WorkerProcess.{h,cpp}` — the header no longer includes `<windows.h>`; its three `HANDLE`
  members are `void*` on Windows and one opaque child-process handle elsewhere, and `getExitCode()`
  returns `UnsignedInt` instead of `DWORD` (same type, no call site changes). The Windows body is
  unchanged apart from casts; the non-Windows body is a second implementation of the same class.
- `MainMenu.cpp` (Zero Hour) — the two `_spawnl` calls became
  `rts::launchProcessDetached("WorldBuilder[D].exe")`. Because this is engine policy rather than
  a WWLib primitive, the wrapper lives in `Core/GameEngine`
  (`Common/ProcessLaunch.{h,cpp}`) and calls `_spawnl` directly on Windows, so the Windows build
  keeps the exact same CRT call. `<process.h>` is gone from `MainMenu.cpp`.

### Crash reporting

- `MiniDumper.cpp` — the whole file is now inside `#ifdef RTS_ENABLE_CRASHDUMP`, and
  `cmake/config-memory.cmake` only defines `RTS_ENABLE_CRASHDUMP` for `WIN32`. Off Windows the
  translation unit is empty. The Windows code, including its two SEH blocks, is untouched.
- `StackDump.h` — `<windows.h>` is included only on Windows, and the two entry points whose
  signatures contain Win32 types (`StackDumpFromContext(DWORD,DWORD,DWORD,...)` and
  `DumpExceptionInfo(unsigned, EXCEPTION_POINTERS*)`) are declared only on Windows. The portable
  four — `StackDump`, `FillStackAddresses`, `StackDumpFromAddresses`, `GetFunctionDetails` — are
  declared everywhere.
- `StackDump.cpp` — the existing body is wrapped in `#ifdef _WIN32`; the `#else` arm implements
  the portable four with `backtrace()`/`backtrace_symbols()`.

## What the non-Windows behaviour actually is, honestly

**Single instance is a lock file, not a named mutex, and the difference is real.**
`Instance_Lock_Acquire` opens `$XDG_RUNTIME_DIR`/`$TMPDIR`/`/tmp` + `<name>.lock` and takes
`flock(LOCK_EX|LOCK_NB)`.

- Same as Win32: while a process holds it, a second process is refused; when the holder exits for
  *any* reason, including `SIGKILL`, the kernel drops the lock and the next launch succeeds. The
  "crashed game leaves the game unlaunchable" failure mode does not happen.
- Different from Win32: the *name* is a path in a filesystem, so the file itself survives the
  crash (a zero-byte turd, not a functional problem), the namespace is not per-session or
  per-desktop the way `Local\`/`Global\` are, and a name containing `/` would be a path traversal —
  the caller's names are GUID strings, but that is a caller property, not a guarantee of the API.
  `flock` is also advisory and does not work reliably on NFS.
- Different in a way that matters for diagnostics: `flock` cannot tell "another instance holds it"
  apart from "the lock could not be taken at all" (read-only `/tmp`, out of descriptors), whereas
  Win32 distinguishes `ERROR_ALREADY_EXISTS` from a failed `CreateMutex`. The seam treats an
  unusable lock as "already running", which is the safe reading but *is* a behaviour difference.

**`_spawnl(_P_NOWAIT)` becomes double-`fork` + `execv`.** The intermediate child `setsid()`s and
exits, so the grandchild is reparented to init and never becomes a zombie. The cost: the parent
learns only that the *fork* succeeded. `_spawnl` returns `-1` for a missing executable; the
double-fork returns success and the grandchild's failed `execv` is invisible. `MainMenu.cpp` uses
the return value to decide whether to show "could not launch WorldBuilder", so off Windows that
message would not appear. Fixing it needs a close-on-exec status pipe; it is not worth it until
something off Windows actually ships a WorldBuilder binary.

**The worker process loses its kill-on-parent-death guarantee on macOS.** On Windows,
`WorkerProcess` puts the child in a job object with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, so the
child dies even if the game is killed with no chance to clean up. Linux has
`prctl(PR_SET_PDEATHSIG, SIGKILL)`, which is close enough. **macOS has neither**, and there is no
equivalent: the honest options are a `kqueue`/`EVFILT_PROC` babysitter process or accepting the
leak. This implementation accepts the leak on macOS and says so here. It is the single largest
unresolved semantic difference in this slice.

Also different: the Win32 path passes a command *line* to `CreateProcessW`, which parses it with
Windows' own rules; off Windows the same string goes to `/bin/sh -c`, whose quoting and globbing
rules differ, and an exit due to a signal is reported as `128 + signum` because POSIX has no
`GetExitCodeProcess`.

**Crash reporting off Windows is a stub, deliberately.** No minidump is written, and `StackDump`
is `backtrace_symbols`: return addresses and mangled names, no line numbers, no source files, and
nothing useful for a static function or a stripped binary. Producing a `.dmp` a Windows debugger
can open is not possible off Windows, and a real symboliser (`atos`, `llvm-symbolizer`,
libbacktrace, or linking `-rdynamic`) is a separate decision with a build-system cost. The
callers that matter — `DEBUG_CRASH` and the memory-pool leak report — get frames with addresses,
which is enough to symbolise offline.

## What was deliberately left alone

- **The two SEH blocks.** Both are already `_MSC_VER`-guarded with a fallback. Nothing to do.
- **`WinMain.cpp`'s `SetUnhandledExceptionFilter` and `DumpExceptionInfo` call.** Entry-point
  code, Windows-only by construction; the non-Windows entry point is a later slice and will need
  its own `sigaction` handler. Wrapping it now would be inventing a caller that does not exist.
- **`Core/Libraries/Source/WWVegas/WWLib/Except.cpp`.** In scope only if it blocked the probe. It
  did not — it is already `#if defined(_WIN32)`-guarded end to end and compiles clean. Untouched.
- **`Core/Libraries/Source/debug/debug_stack.cpp`.** Same rule: it is in the legacy `debug`
  library, which is not in the probe's default target set and did not block this slice. Its
  `SetUnhandledExceptionFilter` and `TerminateProcess` are a separate, self-contained Win32 crash
  reporter that duplicates `StackDump`. Untouched; deleting or porting it is a later decision.
- **The other four `CreateMutex` sites** (`WWLib/mutex.cpp`, `WWAudio/SoundSceneObj.cpp`,
  `WWDebug/wwmemlog.cpp`, `MilesAudioDevice/MilesAudioManager.cpp`). These are *intra-process*
  locks that happen to use a kernel mutex, not single-instance detection. They belong to the
  existing `platform_mutex` seam, not this one; three of the four are in libraries whose porting
  slice has not happened yet, and `MilesAudioManager.cpp` is inside the audio device that is being
  replaced wholesale.
- **`Audio/urllaunch.cpp`'s `CreateProcess`.** It builds a Windows registry-derived browser
  command line; the portable answer is `open`/`xdg-open`, i.e. a *different* function, not this
  abstraction. One call site, no shared policy — abstracting it here would be speculative.
- **`Generals/Code`'s copies** of `MainMenu.cpp`, `StackDump.{h,cpp}` and `ClientInstance` usage.
  Out of scope by instruction; they still compile because the seam is additive.

## Measured effect

`python3 scripts/native-port-probe.py` (clang, `-fsyntax-only -std=c++20 -m64`, no Windows SDK).

| Probe | Before | After |
| --- | --- | --- |
| native, default targets | 489 / 737 | **496 / 739** |
| shimmed, default targets | 640 / 737 | **643 / 739** |
| native, `--include-renderer` | 542 / 979 | **549 / 981** |

The totals rise by two because this slice adds two translation units
(`platform_process.cpp`, `ProcessLaunch.cpp`), both clean. Before and after were measured in the
same environment, with the fetched dx8/gamespy/miles/lzhl headers present, from a worktree of
`main` and from this branch. The checked-in baselines in `docs/porting/ci-baselines/` are
regenerated to 496/739 and 643/739. (The shimmed "before" is 640 rather than the 638 this slice
first measured because `main` moved underneath it; the +3 delta is unchanged.)

Five translation units went from failing to clean, and two of the new files are themselves clean
translation units:

- `Core/GameEngine/Source/GameClient/ClientInstance.cpp` (`'windows.h' file not found`)
- `Core/GameEngine/Source/Common/WorkerProcess.cpp` (`'windows.h' file not found`)
- `Core/GameEngine/Source/Common/System/MiniDumper.cpp` (`'io.h' file not found`)
- `Core/GameEngine/Source/Common/OptionPreferences.cpp` (via `ClientInstance.h`)
- `GeneralsMD/Code/GameEngine/Source/Common/SkirmishBattleHonors.cpp` (via `ClientInstance.h`)

Two headers stopped pulling `<windows.h>` into unrelated translation units, which is where the
last two of those come from — that is the same leverage the PreRTS surgery found, one header at a
time.

`MainMenu.cpp` moved from `'process.h' file not found` to `'gamespy/…/gscommon.h' file not found`:
still failing, now for a reason that is somebody else's slice (the fetched GameSpy SDK), which is
the honest outcome for that file.

Four translation units in the GameSpy-dependent set (`LanLobbyMenu.cpp`,
`SkirmishGameOptionsMenu.cpp`, `WOLLoginMenu.cpp`, and `MainMenuUtils.cpp` transitively) went from
reporting *one* fatal `'windows.h' file not found` to reporting ~1350 errors. They were failing
before and are failing now — removing the early fatal include simply lets clang get far enough to
report the pre-existing GameSpy-header cascade that other translation units in that set already
report. No translation unit regressed from clean to failing.

### A number in an existing doc that this slice could not confirm

`StackDump.cpp` never appears in the probe's failure list, before or after. That is not because it
is portable: the file's whole body is inside
`#if defined(RTS_DEBUG) || defined(IG_DEBUG_STACKTRACE)`, and `IG_DEBUG_STACKTRACE` is defined by
`StackDump.h`, which is included *after* that `#if`. Under the probe's flags the file therefore
compiles to nothing at all and has always counted as "clean". The new non-Windows body was
verified by compiling the file by hand with `-DRTS_DEBUG=1` (clean). Any future claim that "the
stack dumper compiles natively" needs that flag, not the default probe run.

## What the next slice in this area has to solve

1. **A non-Windows entry point.** `WinMain.cpp` owns `SetUnhandledExceptionFilter`, the
   single-instance check's caller and the crash hook. Until there is a `main()`, the crash seam
   has no non-Windows caller and cannot be exercised.
2. **`sigaction`-based crash capture** — `SIGSEGV`/`SIGBUS`/`SIGILL`/`SIGABRT` on an altstack,
   writing the `StackDump` output to the same log the Windows filter uses. Async-signal-safety
   makes this genuinely harder than it looks: `backtrace_symbols` allocates.
3. **Symbolisation.** Decide between `-rdynamic` + `backtrace_symbols` (cheap, exported symbols
   only), an out-of-process `atos`/`llvm-symbolizer` call (good output, needs a child process —
   which this slice now provides), or libbacktrace (best, new dependency).
4. **macOS child-process reaping**, if a worker process is ever launched off Windows. See above:
   `kqueue`/`EVFILT_PROC`, or accept the leak.
5. **`urllaunch.cpp`** — `open`/`xdg-open`, and the registry lookup that feeds it, which belongs
   with the `platform_settings` slice.
