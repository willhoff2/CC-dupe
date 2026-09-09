# Static destruction order, the allocator's critical sections, and the exit-time SIGSEGV

`SIGSEGV` after a clean shutdown, exit status only: OpenAL Soft's static destructors free memory, the
engine's `operator delete` reaches `DynamicMemoryAllocator`, and the allocator takes
`TheDmaCriticalSection` — which, on the non-Windows path, was a plain file-scope static in
`PlatformMain.cpp` that static destruction had already destroyed. Entering a destroyed
`std::recursive_mutex` wrapper recursed until the stack ran out.

The cause is destruction *order*, so the fix is a lifetime, not a `try`/`catch` and not a suppressed
signal: the five critical sections the allocator can take must outlive every static destructor that
might allocate. `ImmortalCriticalSection` (in both `CriticalSection.h` trees) holds a
`CriticalSection` in aligned storage, constructs it with placement `new`, and **has no destructor**,
so the mutex is still live during and after static destruction; the process simply exits and the OS
reclaims the storage. `PlatformMain.cpp` — the non-Windows entry point — uses it for the five
allocator-related sections. **`WinMain.cpp` is untouched**, so Windows keeps the exact lifetimes it
had.

The evidence is a native harness, `Core/GameEngine/Source/Common/System/tests/memory_shutdown_test.cpp`,
run by `scripts/native-memory-shutdown-test.py`. It links the real engine archives, so the real
allocator and the real `operator new`/`delete` are under test, and it holds both arrangements in one
process: a plain static `CriticalSection` subclass that records its own destruction, the five
immortal sections, and a static object that allocates and frees *from its destructor*, declared so
that it is destroyed last. It runs after `shutdownMemoryManager()`:

```
a plain static section is already destroyed by now             yes
the section the allocator holds is still alive                 yes
late free completed
exit status: 0
```

The first line is the control: it proves the destructor really does run after plain statics are gone,
which is the window the crash lived in — if it printed `NO`, the run would prove nothing and the
runner fails it. The second is the fix. An earlier version of this harness *did* pass without the fix,
because `SmudgeSet::m_freeSmudgeList` in `Smudge.cpp` allocates before `main` and sent
`shutdownMemoryManager()` down its pre-main path; gdb showed
`preMainInitMemoryManager()` under `__global_var_init()` in `Smudge.cpp`. The harness therefore
allocates nothing before `main`.

Classification: **port defect**, fixed here. Verified on Linux x86-64 against the native archives;
the OpenAL Soft destructor that triggered it on the M1 Pro is not in this harness, so the crash
itself is Linux-unreproducible and the next Mac session should confirm exit status 0 on hardware.

## Running it

```sh
python3 scripts/native-memory-shutdown-test.py
```

It needs the levels 1-4 archives, so run `scripts/native-build.py --level 1 --level 2 --level 3
--level 4 --with-shims --strict-link` first. It reuses the render harness's own compile flags and
link recipe, renames `main` in `PlatformMain.cpp.o` so the harness is the entry point, and requires
all four results above. It runs in the Native Port CI levels 1-4 job, next to the audio-backend
gate — the job that links the OpenAL surface the crash came from.

# The second mechanism: `ObjectPoolClass` puts its first object on top of its block header

The crash #117 did not cover, ranked #4 in `docs/porting/playability-probe.md` §9 and measured there
in §7 on the real M1 Pro: every quit wrote a crash report faulting in
`ObjectPoolClass<MultiListNodeClass, 256>::~ObjectPoolClass()` under `__cxa_finalize_ranges` ← `exit`,
`KERN_INVALID_ADDRESS at 0x7ab5804c00000000`.

**It is not a destruction-order problem, and it is not about the memory manager at all.** It is
64-bit pointer arithmetic in `Core/Libraries/Source/WWVegas/WWLib/mempool.h`,
`ObjectPoolClass::Allocate_Object_Memory()`. A pool block is one allocation holding a next-block
pointer followed by `BLOCK_SIZE` objects:

```cpp
BlockListHead = (uint32*)::operator new( sizeof(T) * BLOCK_SIZE + sizeof(uint32 *));
*(void **)BlockListHead = tmp_block_head;   // the header: pointer-sized, 8 bytes on LP64
FreeListHead = (T*)(BlockListHead + 1);     // the objects: uint32-sized step, 4 bytes  <-- defect
```

The size reserved for the header is `sizeof(uint32 *)`; the distance skipped is `sizeof(uint32)`. On
Win32 those are both 4 and the code is correct — this is why the Windows oracle has never crashed
here and why no 32-bit build can reproduce it. On LP64 the objects start 4 bytes inside an 8-byte
header, so **the first object of every block overlaps the top half of that block's next-block
pointer**, and the very first write to it — the free-list link the loop below installs, then the
object's own constructor — destroys half of the pointer. Little-endian, the surviving value is
`(low 32 bits of some heap pointer) << 32`: a pointer whose low half is zero, which is exactly the
shape of the retail fault address `0x7ab5804c00000000`. The destructor is where it finally gets
dereferenced:

```cpp
while (BlockListHead != nullptr) {
    uint32 * next_block = *(uint32 **)BlockListHead;   // the faulting read
```

Classification: **PORT DEFECT**, a use-after-corruption. The corruption happens *during play*, at
the first write to the first object of every pool block; only its consumption is at exit, which is
why a player sees it once per session. Secondary effect of the same arithmetic: every pooled object
was 4-byte-misaligned against its own 8-byte pointer members (`alignof` 8, address ≡ 4 mod 8) — UB
on any platform and only tolerated by x86-64 and, for scalars, by arm64.

The fix is the actual lifetime/layout defect, one line, and it is byte-for-byte identical on Win32
(`sizeof(uint32 *)` is 4 there):

```cpp
FreeListHead = (T*)((char *)BlockListHead + sizeof(uint32 *));
```

No `_exit`, no deliberate leak, no immortal pool: none of those would have been honest here, because
the pool is self-contained and the memory it walks is its own. All 11 `DEFINE_AUTO_POOL` pools in the
tree (`MultiListNodeClass`, `GenericSLNode`, `PolyRenderTaskClass`, `MatPassTaskClass`,
`HAnimComboDataClass`, the AAB-tree and grid links, `SoundSceneClass::AudibleInfoClass`,
`CameraShakerClass`) share the template and were corrupting their own block header the same way.

## Evidence

`Core/GameEngine/Source/Common/System/tests/exit_teardown_test.cpp`, run by
`scripts/native-exit-teardown-test.py`, links the real archives and takes 300 nodes (two blocks of
256) out of the real `AutoPoolClass<MultiListNodeClass, 256>::Allocator`, writes to every one, calls
`shutdownMemoryManager()` as `PlatformMain.cpp` does, and lets `exit()` destroy the pool.

Before the fix, on Linux x86-64, the harness reproduced the retail crash frame exactly (gdb):

```text
Program received signal SIGSEGV, Segmentation fault.
0x0000555555a90792 in ObjectPoolClass<MultiListNodeClass, 256>::~ObjectPoolClass (
    this=0x555556428b28 <AutoPoolClass<MultiListNodeClass, 256>::Allocator>)
    at Core/Libraries/Source/WWVegas/WWLib/mempool.h:208
208			uint32 * next_block = *(uint32 **)BlockListHead;
#1  __run_exit_handlers (…) at ./stdlib/exit.c:113
#2  __GI_exit (…) at ./stdlib/exit.c:143
```

After the fix, both cases pass:

```text
--- shipping: exit() destroys the real MultiListNodeClass pool (exit 0)
the retail MultiListNodeClass pool served the run               ok
the objects start a pointer into the block, clear of the header ok
the objects are pointer-aligned                                ok
the block list is walkable and terminated after the objects were written ok
the block list is still intact once the objects are back        ok
shutdownMemoryManager() left the allocator                     alive (it was inited before main)
an allocation from a static destructor is usable               yes
exit teardown completed
--- control: the pre-fix arithmetic … (exit 0)
control: the first block's next pointer was 0x8cbdef1400000000, and is (nil) after one object
the pre-fix arithmetic overwrites the block's next-block pointer ok
the first object overlaps the block header                      ok
```

The control is a copy of the pre-fix template, arithmetic verbatim, and it reads the corrupted
pointer without dereferencing it: `0x8cbdef1400000000` is the same half-zero shape as the Mac's
`0x7ab5804c00000000`, produced by the free-list link alone, and then zeroed outright by the object's
first field. Without that control the shipping run would prove nothing, since a 32-bit build cannot
carry the defect. `shutdownMemoryManager()` reporting the allocator still alive is the pre-main-init
path documented above (`SmudgeSet::m_freeSmudgeList`), recorded rather than asserted: it is why the
manager is *not* part of this mechanism.

Valgrind was tried and is not usable here: `valgrind 3.18.1` cannot read this binary's debug info
(`unhandled dwarf2 abbrev form code 0x25`, then `I can't recover`), so the harness asserts on the
block chain it can read instead of on a memcheck summary.

## What is measured where

| Path | Platform | Result |
| --- | --- | --- |
| The real pool destroyed by `exit()` after `shutdownMemoryManager()` | Linux x86-64, native archives | exit 0, block list intact; **exit -11 in `~ObjectPoolClass` before the fix** |
| Pre-fix arithmetic overwrites the block header | Linux x86-64 | reproduced, half-zero pointer |
| Main Menu → Exit | Apple Silicon (M1 Pro, live, real input, no debugger) | **exit 0**, 2 s after the click, no hang, no SIGSEGV; DiagnosticReports 41 → 41 |
| Quit from a running skirmish | Apple Silicon (M1 Pro, live, real input, no debugger) | **exit 0**, no hang, no SIGSEGV; DiagnosticReports 41 → 41. Quit at ~1 min of game time (Escape → Exit → Yes), through the score screen and skirmish setup back to the shell, then Exit Game |
| Quit from a running campaign mission | Apple Silicon (M1 Pro, live, real input, no debugger) | **exit 0**, no hang, no SIGSEGV; DiagnosticReports 41 → 41. USA-01 on Easy, quit at 00:02:11 mission time after the intro cinematic (Escape → Exit Mission → Yes), score screen → shell → Exit Game |
| No crash report written in `~/Library/Logs/DiagnosticReports`, no hang | Apple Silicon | **Measured for all three clean exits: none written** (41 before and after each fresh launch, `ls ~/Library/Logs/DiagnosticReports \| wc -l`). Measured for the soak: count 40 → 41, `zh-2026-09-03-020314.ips`, `EXC_BAD_ACCESS`/`SIGSEGV` at `0xd8` in `MilesAudioManager::initFilters3D` on the OpenAL service thread, 16.2 resumed minutes into a running skirmish — a **runtime** crash, not a shutdown one |

Wave 11 reached a running real-input skirmish on the M1 Pro twice under the probe. The first run
ended in a probe-induced deadlock (tooling, `playability-probe.md` §1.3 run 1); the second ran
without any OpenAL function evaluation and crashed in the audio completion callback
(`sound-effects-chain.md` §6 item 1). That crash says nothing about the #145 `~ObjectPoolClass`
fix or about `exit()`: the process never reached shutdown.

The three clean-exit rows were then measured separately, on the same `main` build (HEAD
`66f7183e5`), as fresh launches with **no debugger attached at any point** (LLDB attached only to
disposable scout launches used to read the GameWindow button positions; those were killed and are
not the measured runs). Each run: `ls ~/Library/Logs/DiagnosticReports | wc -l` before launch;
`cd <run-dir> && arch -arm64 ./zh -win &`; posted real CGEvent input via
`scripts/macos-input-drive.py post --pid <pid> --key 53` (skip intro) and
`post --client <x,y>` for each button (Exit Game at client 644,334; in-game quit menu Exit at
400,347; confirm Yes at 299,412); `wait <pid>` for the status; count again. All three returned
`0` with the process gone within 2 s of the final click; no `.ips` appeared and the count stayed
at 41 throughout. Screenshots of the running skirmish (HUD, 00:01:13 game clock), the running
mission (HUD, 00:02:11) and the shell were taken through the same script and kept in the session,
not the repo (retail art).

Two caveats on how the quit paths behave, both matching the Windows shell flow rather than a Mac
regression: quitting a skirmish or mission does not exit the process — it goes to the score screen,
then back to the skirmish setup (or the shell), so the row's exit status is the one from the final
Exit Game click. Two earlier harness attempts that stopped at the score/setup screen were killed by
the harness after its 120/180 s wait (SIGKILL, DiagnosticReports unchanged); those are harness
timeouts, not game hangs, and they are not the measured runs. `/usr/bin/sample` could not read
thread state on this machine without elevation, so a hang, had one occurred, would have been
recorded from `ps` state and the timeout only.

## The third mechanism: `OpenALAudio::Library::~Library()` with a joinable service thread

Reported from the M1 Pro on `main` #155 (`fbfc0f574`): a clean quit aborts in `std::terminate()`
with `OpenALAudio::Library::~Library()` on the stack during static destruction, "every clean quit".
The authoritative record is `ci-baselines/quit-path-static-destruction.json`; this section reads it.

**Mechanism.** `lib()` returns a function-local static `Library`. Its `std::thread service` is
started by `AIL_startup` and joined only by `AIL_shutdown` → `stopServiceThread()`. Until this fix
`Library` had no user-declared destructor, so `exit()` ran the implicit one, which destroys the
members in reverse order and reaches `std::thread::~thread` with the thread still joinable
whenever `AIL_shutdown` was not called. The standard makes that a direct `std::terminate()`; there
is **no exception**. The exact frame, from gdb on the standalone reproduction:

```
#0  std::terminate ()
#1  std::thread::~thread (this=<OpenALAudio::lib()::instance+464>) at bits/std_thread.h:152
#2  OpenALAudio::Library::~Library (this=<OpenALAudio::lib()::instance>)
#3  __run_exit_handlers / #4 exit ()
```

libstdc++ prints `terminate called without an active exception`. The report's "exception escaping
`~Library`" is this same frame read from a `noexcept` destructor; `catch throw` never fires. The
other candidates were checked and are not it: the recursive mutex is not held at exit, and the
#155 `Diagnostics` static, though destroyed *before* `Library`, is only reached from the service
thread — which is the same precondition (thread alive at exit) as the terminate itself.

**Bisect.** `scripts/native-audio-static-destruction-test.py --shim-rev REV --expect-defect`
compiles the shim from `c6fd1bd7c` (#153, pre-#155) and from `fbfc0f574` (#155) against the same
harness. Both abort with `-6` (SIGABRT) in all three diagnostics variants. **#155 did not introduce
it**: the defect is as old as the service thread. #155 changed nothing about the thread's lifetime;
it added `Diagnostics` and `LibraryGuard`, which the fix also makes order-independent. Wave 12's
"exit 0 on the #153 binary" row above is consistent with this: that quit path reached
`AIL_shutdown`, so nothing was joinable. What the fix does not tell us is *which* macOS quit route
reaches `exit()` without `AIL_shutdown` — that is UNMEASURED and is the Mac follow-up (after the fix,
`OPENAL_AUDIO_DIAG=stderr` prints `static destruction: AIL_shutdown was not called; service thread
joined` on exactly that route).

**Fix (shim only, `OpenALDriver.cpp` / `OpenALAudioInternal.h`).** An explicit
`Library::~Library() noexcept` sets `serviceQuit`, joins the service thread if it is joinable
(detaches instead if the destructor is somehow running *on* that thread), reports the skipped
shutdown to the diagnostics log, releases the OpenAL context and device, and closes the diagnostics
file. `Diagnostics` is now heap-allocated once and never freed, so `serviceLoop`, `LibraryGuard` and
the destructor read it regardless of static destruction order; `diagnosticsClose()` flushes and
`fclose`s a file log (never `stderr`) and cannot throw. Nothing is wrapped in `catch (...)`: the one
operation that could terminate is removed, not hidden. `AIL_shutdown`'s own timing and the engine
call sites are unchanged; the shim is not compiled on Windows.

**Red/green.** `Core/Libraries/Source/OpenALAudioDevice/tests/openal_static_destruction_test.cpp`
starts the library, plays a 2D sample, a 3D sample and a memory stream (5 s voices) for 300 ms of
service-thread work, and returns from `main` without `AIL_shutdown`, in three variants (diagnostics
off, `OPENAL_AUDIO_DIAG=stderr`, `OPENAL_AUDIO_DIAG=<file>`). Before the fix: exit `-6`,
`terminate called without an active exception`, the diagnostics file cut off mid-run. After: exit
`0`, and the file's last line is the `static-destruction` counters report. It runs in the same
Native Port CI job as the #153 callback-thread and #155 render tests.

**Measured quit paths.** Linux x86-64, real game and retail data under Xvfb + lavapipe + OpenAL Soft
`null`, each run under `gdb -batch` with `break std::terminate`, `catch throw` and SIGTERM
stop-and-pass, real X11 input, window close as a `WM_DELETE_WINDOW` ClientMessage. Rows are the
fixed binary; the pre-fix `fbfc0f574` game rows are in the JSON.

| Path | Diag | Linux x86-64 (fixed binary) | Apple Silicon |
| --- | --- | --- | --- |
| Main menu → Exit Game | off / file | **exit 0**, 0 gdb stops, no terminate; diag file ends `counters shutdown` | UNMEASURED |
| In mission → Escape → Exit Game → Yes → score screen → Main Menu → Exit Game | off / file | **exit 0**, 0 stops, no terminate; `AIL_shutdown` reached | UNMEASURED |
| In mission → window close (`WM_DELETE_WINDOW` → `MSG_META_DEMO_INSTANT_QUIT` → `GameLogic::quit(TRUE)`) | off / file | **exit 0**, 0 stops, no terminate; `AIL_shutdown` reached | UNMEASURED |
| Main menu → window close | off / file | **exit 0**, 0 stops, no terminate | UNMEASURED |
| SIGTERM at the main menu | off / file | killed by SIGTERM, 1 gdb stop (the signal itself), no terminate; no handler is installed, so no C++ teardown runs and static destruction never happens | n/a (no SIGTERM path on Windows) |

Zero Hour's in-game menu has no direct quit-to-desktop; the window close is the only in-mission
route that calls `GameLogic::quit(toDesktop=TRUE)`. Every clean route on Linux tears down through
`GameEngine::~GameEngine` → `SubsystemInterfaceList::shutdownAll` → `MilesAudioManager::closeDevice`
→ `AIL_shutdown`, so **the Linux game does not reproduce the abort on any reachable path, before or
after the fix**; the defect only reproduces once `AIL_shutdown` is skipped, which the standalone
test does deterministically. The Mac rows are UNMEASURED in this slice.

## The fourth mechanism: `exit()` from inside the frame loop, and why the Mac had no Quit

Two findings from the same M1 Pro session, both about *how the process is asked to stop* rather
than about what happens once it is stopping.

**There was no way to quit.** MEASURED on the M1 Pro: the running game's menu bar carried exactly
two menus, Apple and `zh`, and the `zh` menu was **empty** — no Quit item, therefore no Cmd-Q key
equivalent for `-[NSApplication sendEvent:]` to match. Cmd-Q was posted both through the session
event tap and through `CGEventPostToPid` and the process ignored both. The only routes out were the
shell's own Exit Game button, the window's red close button, and Force Quit. The cause is in
`platform_window_cocoa.mm`: it created the `NSApplication` and never called `setMainMenu:`, so what
the menu bar showed was the process name with nothing under it.

**Wiring a Quit item to `-[NSApplication terminate:]` would have made this worse, not better.**
`terminate:` calls `exit()` from inside whatever frame the game was drawing, so
`GameEngine::execute()`'s `while (!m_quitting)` never returns, `GameMain()` never deletes
`TheGameEngine`, and the process falls straight into static destruction with the engine's threads
and subsystems still live. Two aborts on exactly that route are on record:

| Abort | Where | Status |
| --- | --- | --- |
| `OpenALAudio::Library::~Library()` with a joinable service thread | the third mechanism above | fixed in #159 |
| `ThreadClass::Switch_Thread()` → `WWPlatform::EventClass::Wait()` → `platform_thread.cpp:76`, `std::unique_lock<std::mutex>` → `std::mutex::lock()` throwing `std::system_error` → `std::terminate` → SIGABRT, on thread 8 | reported from the M1 Pro on the #159-fixed binary; crash report `~/Library/Logs/DiagnosticReports/zh-2026-09-04-113305.ips` | **not fixed, not re-measured here** |

The second row is a *relayed* report, not a measurement taken in the slice that wrote this section:
what was checked here is only that `platform_thread.cpp:76` is indeed the `std::unique_lock`
construction inside `EventClass::Wait()`, which is consistent with a mutex whose storage has already
been torn down. Nobody has attached a debugger to it.

**What the window seam does instead.** The Quit menu item, its Cmd-Q key equivalent, the window's
red close button (`-windowShouldClose:`, which answers `NO`) and the Dock's Quit
(`-applicationShouldTerminate:`, which answers `NSTerminateCancel`) all raise the seam's
`WINDOW_EVENT_CLOSE`. `PlatformWindowHost::handleEvent()` turns that into `requestQuit()` — the
`WM_CLOSE` body from `WinMain.cpp` — which posts `MSG_META_DEMO_INSTANT_QUIT` or sets
`GameEngine::setQuitting(TRUE)`, the same flag the shell's Exit Game button sets. Nothing in the
seam calls `terminate:` or `exit()`. The cost, stated once: a macOS logout now waits for the
engine's own shutdown and is reported as blocked if that shutdown hangs. That is the deliberate
trade against the aborts above.

Whether this actually exits 0 on the Mac is **UNMEASURED** — see the table in the third mechanism,
whose Apple Silicon column is still UNMEASURED on every row, and
`docs/porting/window-event-loop.md` §4.

## The input wedge is a different finding

`docs/porting/playability-probe.md` §8.1 (rendering and mouse motion continued while clicks and
Escape did nothing for 20+ minutes, cleared by a real posted click) is **not** this mechanism and was
not absorbed here. Everything in this document is exit-time memory in a self-contained pool; §8.1 is
live event-loop/focus state, it happened while the process was healthy, and the pool corruption fixed
here is only ever read during static destruction. No evidence connects them. It stays ranked #4 in
§9 for wave 11.

## Running it

```sh
python3 scripts/native-exit-teardown-test.py
```

Same prerequisites as the harness above: the levels 1-4 archives, `CLANGXX=clang++-14`. It runs in
the Native Port CI levels 1-4 job next to the shutdown-order gate.
