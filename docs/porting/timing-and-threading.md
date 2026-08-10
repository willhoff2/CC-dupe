# Timing and threading primitives

Two rows of the "Platform behaviour left for a later slice" table in
[`prerts-win32-surgery.md`](prerts-win32-surgery.md): **High-resolution timing**
(`FrameRateLimit.cpp`, `ProcessAnimateWindow.cpp`, `LANAPI.cpp`) and **Threading primitives**
(`CriticalSection.h`).

The short version: the timing row needed one new file's worth of code and no call-site changes
at all, because every in-scope use of `timeGetTime` / `QueryPerformanceCounter` / `GetTickCount` /
`Sleep` already has a portable equivalent with the same contract. The threading row needed a
recursive mutex, not a mutex, and it could not reuse `WWLib/platform/platform_mutex` — §4 explains
why. Nothing in either row is on a determinism-relevant path except two RNG-seed sites, which are
§3.4.

## 1. What is out there — the sweep

Symbol occurrences with comments and string literals stripped, `.cpp`/`.h`/`.inl`, measured over
`Core/GameEngine`, `Core/GameEngineDevice`, `GeneralsMD/Code/GameEngine`,
`GeneralsMD/Code/GameEngineDevice` and `Core/Libraries`:

| Symbol | Sites | Files |
|---|---:|---:|
| `timeGetTime` | 201 | 43 |
| `LARGE_INTEGER` | 67 | 14 |
| `QueryPerformanceCounter` | 44 | 14 |
| `GetTickCount` | 43 | 14 |
| `QueryPerformanceFrequency` | 22 | 12 |
| `Sleep` | 16 | 9 |
| `Initialize`/`Delete`/`Enter`/`Leave`/`TryEnterCriticalSection` | 16 | 5 |
| `CRITICAL_SECTION` | 13 | 6 |
| `GetLocalTime`/`GetSystemTime` | 8 | 7 |
| `CreateMutex*` | 5 | 4 |
| `timeBeginPeriod`/`timeEndPeriod` | 4 | 2 |
| **Total** | **439** | **86** |

The out-of-scope areas (`Core/Tools`, `GeneralsMD/Code/Tools`, `Generals/Code`, which stay on Wine)
contain another **158** occurrences in **50** files, measured the same way. `GeneralsMD/Code/Main`
contains none.

**439 is not the size of this change; it is the size of the surface that had to be checked.** The
number that mattered was the answer to a different question: how many of those 86 files need a
*source* change to compile natively, as opposed to needing the symbols they already spell to mean
something off Windows? The answer is **6**, and 3 of those 6 are the same file duplicated between
the two games or edited only for the debug configuration. The rest is spread over
43 `timeGetTime` files that use it exactly as documented — a millisecond stamp, subtracted from a
later one — and 14 `QueryPerformanceCounter` files of which 9 are diagnostics.

Concentration is high and the distribution is the reason this slice is small:

| Where the timing calls actually are | Files | Character |
|---|---:|---|
| GUI/menu animation, fades, scroll and click timing (`timeGetTime`) | 17 | wall clock, client-only |
| Network timeouts, resends, last-heard, latency (`timeGetTime`, `GetTickCount`) | 14 | wall clock, scheduling only (§3.3) |
| Diagnostics: perf timers, `DUMP_PERF_STATS`, `DEBUG_LOGGING`, pathfinder timing, script profiling | 9 | compiled out of release builds |
| Network frame scheduling (`Network.cpp`, `QueryPerformanceCounter`) | 1 | multiplayer pacing (§3.3) |
| Renderer/device/audio (`W3DDisplay`, `W3DView`, `W3DShroud`, `textureloader`, WWAudio) | 11 | later slice, renderer's row |
| Frame pacing (`FrameRateLimit.cpp`, `FramePacer.cpp`) | 2 | the one real policy site (§3.2) |
| RNG seeding | 2 | determinism-relevant input (§3.4) |
| Loading screens, replay harness, file transfer (`Sleep`) | 5 | wall clock |

## 2. What already existed

The repo has more of this than the table suggested, and finding out changed the shape of the slice:

- `Dependencies/Utility/Utility/time_compat.h` already implemented `timeGetTime`, `GetTickCount`,
  `timeBeginPeriod` and `timeEndPeriod`; `thread_compat.h` already implemented `Sleep`. Both are
  reachable from every engine translation unit, because `Utility/compat.h` includes them and is
  itself included from `WWLib/always.h` and `Lib/BaseTypeCore.h`. The whole file is inside
  `#ifndef _WIN32`, so Windows never sees any of it.
- `WWLib/platform/platform_time.h` provides `WWPlatform::Get_Performance_Counter()` and
  `Get_Performance_Frequency()`, and `WWLib/mpu.cpp` and `mutex.cpp` already route through
  `WWLib/platform/` off Windows. **WWLib's half of both rows is already done**; what was left was
  the engine's half.
- `scripts/native-port-shims/windows.h` declared `LARGE_INTEGER`, `QueryPerformanceCounter` and
  `QueryPerformanceFrequency` for the measuring probe, and its `mmsystem.h` already deferred
  `timeGetTime` to `time_compat.h` with a comment saying so. That comment described the layering
  this slice completes.

So the missing pieces were exactly two: a high-resolution counter in the compat layer, and a
non-Windows `CriticalSection`.

## 3. Timer semantics

### 3.1 `timeGetTime` and `GetTickCount` — 32 bits, and the wrap is load-bearing

Both are `DWORD` millisecond counters since boot and both wrap every 2^32 ms ≈ 49.7 days. Callers
rely on that: `SysTimeClass::Get()` and the network layer compute `now - then` in unsigned
arithmetic, which is correct across the wrap only if the type is exactly 32 bits wide. The compat
implementations compute a 64-bit millisecond count and convert to `unsigned int`, which is a
modulo-2^32 truncation — the same value the Win32 counter would report at the same instant modulo
the (different) epoch, and the same wrap behaviour. The return type must not be widened to fix the
"overflow"; widening it is what would break the arithmetic. There is now a comment in
`time_compat.h` saying so, because it looks like a bug and is not one.

The epoch differs (`CLOCK_BOOTTIME`/`CLOCK_MONOTONIC` since boot, vs. Windows' own boot count) and
nothing in the sweep depends on the absolute value: every site either subtracts two readings or
compares two readings. The two sites that *store* a reading — the RNG seeds in §3.4 — only need
entropy.

`GetTickCount` on Windows advances in 10–16 ms steps; `timeGetTime` advances in 1 ms steps after
`timeBeginPeriod(1)`. The POSIX implementations are both ~1 ms or better, i.e. the stand-ins are
*more* precise than the originals, never less. Nothing in the sweep depends on a coarse tick.

### 3.2 `QueryPerformanceCounter` — different epoch, different frequency, same usage pattern

`QueryPerformanceCounter` has an unspecified epoch and a frequency reported separately by
`QueryPerformanceFrequency`, so portable code cannot assume either. Every in-scope call site
observes the contract: read the frequency once, then divide a *difference* of two counters by it.
The new implementation returns `CLOCK_MONOTONIC` in nanoseconds and reports a frequency of
1 000 000 000. Consequences, stated plainly:

- Absolute counter values are not comparable to Windows ones. No call site compares them.
- Resolution goes from the platform's (typically 100 ns – 1 µs) to 1 ns nominal, in practice the
  clock's own resolution. Higher, never lower.
- 64-bit nanoseconds since boot overflows after 292 years. Every in-scope caller keeps counters in
  `__int64`/`Int64`, so a nanosecond tick cannot overflow an intermediate: the only 32-bit use of a
  counter anywhere in scope is `pc.LowPart` in the desync test of §3.4, which wants noise.
- WWLib's own stand-in (`WWLib/platform/platform_time.cpp`) deliberately ticks in *micro*seconds
  because `mpu.cpp` samples fixed tick counts and needed a magnitude close to the 1.19 MHz counter
  it was written against. The two rates coexist because no code mixes a counter from one with a
  frequency from the other; the engine's callers all divide by the frequency they were handed.
- `QueryPerformanceCounter` cannot fail on any Windows version the game supports, and the
  stand-in cannot either; it returns non-zero always. `PerfTimer.cpp` has an `else` branch for the
  failure case that is dead on both platforms, and it stays dead.

The one call site where the frequency is *policy* rather than instrumentation is the frame-rate
limiter, `FrameRateLimit.cpp`:

```cpp
QueryPerformanceFrequency(&freq);  m_freq  = freq.QuadPart;
QueryPerformanceCounter(&start);   m_start = start.QuadPart;
...
double elapsedSeconds = static_cast<double>(tick.QuadPart - m_start) / m_freq;
```

`m_freq` comes from the same source as `m_start`, so a different tick rate cancels out. Had the
file hard-coded a frequency, or mixed a counter from one clock with a frequency from another, this
slice would have had to rewrite it. It does not, so it was left as it is.

### 3.3 What is on a determinism-relevant path

The game is lock-step deterministic: peers agree because they execute the *same commands on the
same logic frame numbers*, not because their clocks agree. Wall-clock time enters the loop in
exactly one place, `GameEngine::isTimeToUpdateLogic()`:

```cpp
m_logicTimeAccumulator += min(TheFramePacer->getUpdateTime(), targetFrameTime);
if (m_logicTimeAccumulator >= targetFrameTime) { m_logicTimeAccumulator -= targetFrameTime; return true; }
```

That decides **when** the next logic frame runs, never **what** it computes. `GameLogic::update()`
advances `m_frame` by one and consumes the frame's command list; no simulation value is derived
from a clock reading. So clock resolution and epoch cannot desync a game. What a wrong *frequency*
would do is change how fast the game runs in real time — a very visible bug, not a silent one, and
it is ruled out by the frequency and the counter coming from the same clock (§3.2).

Network timing (`Connection.cpp`, `ConnectionManager.cpp`, `DisconnectManager.cpp`,
`FrameMetrics.cpp`, `Transport.cpp`, `LANAPI*.cpp`, 14 files, plus `Network::timeForNewFrame()`
which is the one non-diagnostic `QueryPerformanceCounter` user outside the frame limiter) is
scheduling and failure detection:
when to resend, when to declare a peer gone, how far to run ahead. Commands carry their frame
number, so these decisions change *timing and connectivity*, not the result of a frame. They are
not determinism-relevant in the desync sense. They are still user-visible, which is why none of
them were touched.

### 3.4 The two sites where a clock reading really does reach the simulation

```cpp
Core/GameEngine/Source/GameNetwork/LANAPI.cpp:894                  myGame->setSeed(GetTickCount());
GeneralsMD/.../Menus/SkirmishGameOptionsMenu.cpp:1310  TheSkirmishGameInfo->setSeed(GetTickCount());
```

The seed is passed to `InitRandom()` on every peer (`LANAPICallbacks.cpp:260`,
`SkirmishGameOptionsMenu.cpp:444`) and written into replays (`Recorder.cpp:377`), so the clock is an
*input to* the deterministic stream rather than something read during it: whatever value the host
picks, everyone uses that one. The requirements are that it be integral milliseconds, fit in `Int`,
and vary between runs. All three hold. The skirmish site is in the single-player path this port
targets, which is why it is called out even though nothing about it changed.

One site is deliberately non-deterministic and must stay that way:

```cpp
#ifdef RAILROAD_DESYNC_TEST
	_LARGE_INTEGER pc;
	QueryPerformanceCounter( &pc ); // absolutely, positively random every call!
	Real random = 100000.0f / (Real)pc.LowPart;
```

`RailroadGuideAIUpdate.cpp` uses the counter as an entropy source to *provoke* a desync while
testing the desync detector. `RAILROAD_DESYNC_TEST` is not defined in any configuration. It was
left alone: replacing it with a simulation-safe clock would silently turn a determinism test into a
no-op. It reads `pc.LowPart`, which is why the new `LARGE_INTEGER` keeps the Win32 anonymous-struct
layout rather than exposing only `QuadPart`.

### 3.5 `timeBeginPeriod` and `Sleep`

`timeBeginPeriod(1)` in `FramePacer`'s constructor raises the Windows timer resolution so
`Sleep(n)` is accurate to about 1 ms; the compat stand-in is a no-op returning `TIMERR_NOERROR`,
which is honest — there is no global timer-resolution knob to set, and `nanosleep` is already
~1 ms-accurate or better. `FrameRateLimit::wait` sleeps for the target minus 2 ms and spins the
rest, so the sleep is never the thing that decides the frame boundary. This is an approximation
with no code change required; it is recorded here because "it is a no-op" is a decision, not an
omission.

## 4. `CRITICAL_SECTION` is recursive; `std::mutex` is not

`CriticalSection` (in both games' `GameEngine/Include/Common/CriticalSection.h`) wraps a Win32
`CRITICAL_SECTION`, which a thread may re-enter while it already owns it. **The engine relies on
this**, provably, in the two hottest users:

```cpp
// AsciiString.cpp:256
void AsciiString::set(const AsciiString& stringSrc)
{
	ScopedCriticalSection scopedCriticalSection(TheAsciiStringCriticalSection);
	...
	releaseBuffer();          // AsciiString.cpp:217, whose first line is:
	                          //   ScopedCriticalSection scopedCriticalSection(TheAsciiStringCriticalSection);
```

`AsciiString::ensureUniqueBufferOfSize` (`:123`) does the same thing, and
`UnicodeString::ensureUniqueBufferOfSize` (`:68` → `releaseBuffer` at `:133`) is the identical
pattern on the other string class. With `std::mutex` the first string assignment in the process
would deadlock. Hence `std::recursive_mutex`.

There are five of these objects, all with static storage duration, in
`Common/System/CriticalSection.cpp`: `TheAsciiStringCriticalSection`,
`TheUnicodeStringCriticalSection`, `TheDmaCriticalSection`, `TheMemoryPoolCriticalSection`,
`TheDebugLogCriticalSection`. Nesting across *different* ones also happens
(`DynamicMemoryAllocator` holds `TheDmaCriticalSection` and calls pool code that takes
`TheMemoryPoolCriticalSection`); the order is consistent and was not changed.

### Why not `WWLib/platform/platform_mutex`

The task allowed either reusing it or keeping something local. Local, for three reasons, in order
of weight:

1. **Storage.** `WWPlatform::Critical_Section_Create()` returns a `void*` from
   `new std::recursive_mutex`. Two of the five critical sections exist *for* the memory manager
   (`TheDmaCriticalSection`, `TheMemoryPoolCriticalSection`) and all five are constructed during
   static initialisation, before `initMemoryManager()` — and the engine replaces global
   `operator new` with its own allocator (`GameMemory.cpp`), which is not usable until then. A
   member `std::recursive_mutex` allocates nothing and needs no ordering guarantee. A handle from
   `new` would need the allocator that needs the handle.
2. **Dependency direction.** `core_gameengine` does not link WWLib today: in
   `Core/GameEngine/CMakeLists.txt` the `core_wwvegas` entry of `corei_gameengine_public` is
   commented out, and the games reach WWLib only through their own `z_wwvegas`/`g_wwvegas`
   targets. Its users are the memory manager and both string classes, i.e. the bottom of the
   engine, so making this the file that introduces an engine → WWLib dependency is the wrong place
   to spend that decision.
3. **Cost.** The WWLib API is four out-of-line calls through a `void*` handle, deliberately, to
   keep `<mutex>` out of its header. `CriticalSection::enter`/`exit` are inlined today and are taken
   on every single string assignment in the game; going out-of-line through a pointer indirection
   for that is a measurable regression with no portability benefit.

### A correction: the fan-out is small

The slice brief and the table in `prerts-win32-surgery.md` describe `CriticalSection.h` as "a
header with very large fan-out". Measured, it is not: **8 files include it and none of them is a
header**, so it reaches 8 translation units, not the ~500 that `PreRTS.h` reaches.

```
Core/GameEngine/Source/Common/System/{AsciiString,UnicodeString,GameMemory,Debug}.cpp
{Generals,GeneralsMD}/Code/GameEngine/Source/Common/System/CriticalSection.cpp
{Generals,GeneralsMD}/Code/Main/WinMain.cpp
```

That is what made the fix cheap, and it also means putting `<mutex>` in this header costs almost
nothing in build time. The *runtime* fan-out is the opposite of small — those four Core files are
the allocator, the debug log and both string classes, which is why the recursion question in this
row was the dangerous part rather than the include question.

The Windows path is untouched: the `CRITICAL_SECTION` member and the four `*CriticalSection()`
calls are inside `#ifdef _WIN32` exactly as they were, and the `PERF_TIMERS` instrumentation is
still evaluated at the same four points. VC6 never sees `<mutex>`.

## 5. What changed

| File | Change |
|---|---|
| `Dependencies/Utility/Utility/time_compat.h` | new `LARGE_INTEGER`, `QueryPerformanceCounter`, `QueryPerformanceFrequency`; comments recording the 32-bit wrap and epoch/frequency contracts |
| `GeneralsMD/Code/GameEngine/Include/Common/CriticalSection.h` | `std::recursive_mutex` member off Windows; Windows path unchanged |
| `Generals/Code/GameEngine/Include/Common/CriticalSection.h` | same, to keep the shared header copy compiling |
| `Core/GameEngine/Source/Common/FrameRateLimit.cpp` | `<windows.h>` behind `#ifdef _WIN32`; `DWORD` → `UnsignedInt` for the `Sleep` argument |
| `Core/GameEngine/Source/GameClient/GUI/ProcessAnimateWindow.cpp` | `<windows.h>`/`<mmsystem.h>` behind `#ifdef _WIN32` |
| `Core/GameEngine/Source/GameLogic/AI/AIPathfind.cpp` | its debug-only `<windows.h>` also requires `_WIN32` |
| `GeneralsMD/…/Source/Common/PerfTimer.cpp`, `Generals/…/Source/Common/PerfTimer.cpp` | same, for the `PERF_TIMERS`/`DUMP_PERF_STATS` block |
| `scripts/native-port-shims/windows.h` | drops its `LARGE_INTEGER` and `QueryPerformance*` declarations; the shim defers to the compat layer, as it already did for `timeGetTime` |
| `docs/porting/ci-baselines/native-port-probe-native.json` | 489 → 494 |

No public signature changed. No call site changed except one local variable's type. 439 occurrences
were checked; 1 was edited.

`LANAPI.cpp`, the third file in the timing row, needed **no** change: its `timeGetTime` and
`GetTickCount` calls now resolve off Windows, and it keeps its `<windows.h>` unconditionally
because it also calls `GetUserNameA`/`GetComputerNameA` and uses `DWORD`. That is the
process/identity row, not this one. This is the honest state of that row's entry: the timing part
is done, the file is still Win32-bound for a different reason.

## 6. Measured effect

`python3 scripts/native-port-probe.py` (clang 14, `-fsyntax-only -std=c++20 -m64`), same fetched
dependencies for both runs:

| | Before | After |
|---|---:|---:|
| Native, all probe targets | 489 / 737 | **494 / 737** |
| — `Core/GameEngine` | 107 / 207 | **111 / 207** |
| — `GeneralsMD/Code/GameEngine` | 267 / 379 | **268 / 379** |
| Shimmed, all probe targets | 638 / 737 | 638 / 737 |

The six translation units that stopped failing are `FrameRateLimit.cpp`,
`ProcessAnimateWindow.cpp`, `AsciiString.cpp`, `UnicodeString.cpp`,
`GeneralsMD/…/CriticalSection.cpp` and `GameMemory.cpp`; net +5 because `GameMemory.cpp` now gets
past `CriticalSection.h` and stops at `::GlobalAlloc`/`GMEM_FIXED` instead, which belongs to the
memory row. The shimmed number is the control and did not move, as expected: the shim already
declared these symbols, so nothing about the engine's own C++ got more portable — the Win32
stand-ins simply became unnecessary.

A sharper measurement than the TU count, since a TU stops at its first fatal include error: recompile
every one of the 737 translation units and count those emitting **any** diagnostic naming a timing or
threading symbol (`QueryPerformance*`, `LARGE_INTEGER`, `timeGetTime`, `GetTickCount`, `Sleep`,
`CRITICAL_SECTION`, `*CriticalSection`, `CreateMutex`, `timeBeginPeriod`/`timeEndPeriod`, `MMRESULT`,
`mmsystem.h`):

| | Before | After |
|---|---:|---:|
| TUs with a timing/threading diagnostic | 5 | **0** |

All five were `CriticalSection.h`. **No translation unit in the probe now fails for a timing or
threading reason.** Every remaining `windows.h` in a file that reads a clock arrives from
`Recorder.h`, `GameState.h` or `ClientInstance.h` — the `SYSTEMTIME`-in-file-formats and
process/single-instance rows.

The probe compiles the release configuration only, which hides part of this row: with
`-DRTS_DEBUG -DDEBUG_LOGGING` added, `PerfTimer.cpp`, `AIPathfind.cpp` and `FrameRateLimit.cpp` each
went from 1 error (`'windows.h' file not found`) to 0. That is why the two debug-guarded includes are
in this slice even though the default probe cannot see them.

Windows: `./scripts/docker-build.sh --clean --game zh` and `--clean --game generals` both complete
(1351 and 1301 targets respectively, exit 0). That is the win32 preset; VC6 is covered by GenCI.

## 7. Deliberately not abstracted

- **The Win32 spelling stays at the call sites.** No `Platform::Now()` was introduced for the
  engine. `timeGetTime()` still says `timeGetTime()` in 43 files. Introducing an engine-side clock
  API would mean either touching ~250 call sites, or adding a header that VC6 must also compile —
  VC6 is 4 of the 13 CI configurations and has no `<chrono>` and no `unsigned long long`, so the
  entry point would have to be typed in `__int64`-flavoured terms and implemented twice anyway.
  The compat layer already gives the identical contract for zero call-site churn. If a future
  slice wants a real engine clock (e.g. to inject a fake clock for tests), the seam to add is a
  `FramePacer`-level one, and the 43 files are then a mechanical rename with the semantics already
  documented here.
- **`Core/Libraries/Source/debug` and `Core/Libraries/Source/profile` (25 TUs).** They include
  `<windows.h>` for DbgHelp, consoles, threads and structured exceptions, of which timing is a
  small part. Nothing there compiles natively for reasons this row cannot fix; that is the
  crash-reporting row.
- **`WWAudio/Utils.{h,cpp}`, `WWAudio.cpp`, `MilesAudioManager` (raw `CRITICAL_SECTION` and
  `CreateMutex`).** Renderer/audio targets, excluded from the default probe, owned by the audio
  slice. They also already have a portable path available in `WWLib` if they want it — and unlike
  `CriticalSection.h`, WWLib's `CriticalSectionClass` already allocates its handle on *both*
  platforms (`W3DNEWARRAY char[sizeof(CRITICAL_SECTION)]` on Windows), so §4's storage objection
  does not apply to them.
- **`Core/GameEngine/Include/Common/simpleplayer.h` + `Source/Common/Audio/simpleplayer.cpp`**
  (1 `CRITICAL_SECTION` member, 6 `*CriticalSection` calls). Both are commented out of both games'
  `CMakeLists.txt` with `# unused`, and the header includes the Windows Media SDK's `wmsdk.h`.
  Porting dead code would be all cost.
- **`WWDebug/wwmemlog.cpp`** already resolves this row off Windows by disabling the memory log
  entirely (`DISABLE_MEMLOG`), which is a defensible approximation and not this slice's to revisit.
- **`GetLocalTime`/`GetSystemTime` (8 sites, 7 files).** `SYSTEMTIME` is a wall-clock *struct in
  file formats* (saved games, replay headers) and its own row in the table. It is not a monotonic
  clock problem and must not be solved with one.
- **`_rdtsc`-based `ProfileGetTime`** in `PerfTimer.h/.cpp`. x86-only by construction; on Apple
  Silicon it needs `CNTVCT_EL0` or `mach_absolute_time`. Only compiled under
  `PERF_TIMERS`/`DUMP_PERF_STATS`, so it blocks nothing yet, and the honest fix is an
  architecture-specific counter, not a timing abstraction.

## 8. What the next slice in this area has to solve

1. **`GameMemory.cpp` is now the front-most blocker in `Core/GameEngine/Source/Common/System`** and
   needs `::GlobalAlloc`/`GlobalFree`/`GMEM_FIXED` (3 diagnostics) — a raw OS-allocation shim,
   trivially `malloc`/`free` off Windows, but it is the memory row's call whether the engine's
   `operator new` replacement should route there at all.
2. **`Recorder.h`, `GameState.h`, `ClientInstance.h`.** Between them they are why ~14 clock-reading
   translation units still fail natively. Two are the `SYSTEMTIME` file-format row; the third is
   the single-instance named-mutex row. Either would flip more TUs than this slice did.
3. **Threads.** `WWLib/platform/platform_thread` exists and `thread.cpp` uses it, but the engine's
   GameSpy worker threads (`PeerThread.cpp`, `BuddyThread.cpp`, `PingThread.cpp`,
   `GameResultsThread.cpp`, `PersistentStorageThread.cpp`) go through WWLib's `ThreadClass` and
   `MutexClass`. `platform_mutex` already backs `MutexClass` with `std::recursive_timed_mutex`,
   which is right — a Win32 mutex object *is* re-entrant for its owner — but the timed wait's
   `WAIT_ABANDONED` case and *named* cross-process mutexes (`CreateMutex` with a name, which is how
   `ClientInstance.cpp` does single-instance detection) have no `std::` equivalent and are the real
   content of that row.
4. **`thread_compat.h`'s `GetCurrentThreadId()`** casts `pthread_self()` to `int`. On macOS
   `pthread_t` is a pointer; the cast is lossy and the result is only used for equality
   comparisons, where collisions are unlikely but possible. `pthread_threadid_np` /
   `gettid` is the honest answer. Noted here because it is the same file this slice edited, and
   `time_compat.h`'s Linux-only `CLOCK_BOOTTIME` (already aliased to `CLOCK_MONOTONIC` elsewhere)
   is its neighbour.
5. **A determinism harness.** §3 argues from reading the code that no clock reading reaches
   simulation state. That argument is only as good as the reader. The cheap mechanical check is a
   replay-CRC comparison between two runs at different frame rates, which the `Replay Check` CI job
   already has the shape of.
