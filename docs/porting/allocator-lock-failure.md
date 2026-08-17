# The allocator's lock-failure path, and why a mutex error became a stack overflow

Nine Apple Silicon crash reports and the exit of every native run on that machine share one shape:
an allocation takes a `CriticalSection`, the lock fails, **reporting the failure allocates**, the
allocation takes the same section, and the process dies on the stack guard page
(`Thread stack size exceeded due to excessive recursion`). `std::recursive_mutex::lock()` reports a
failure by calling `__throw_system_error`, which builds a `std::system_error` whose message is a
`std::string`, whose memory comes from the engine's global `operator new`, which *is*
`DynamicMemoryAllocator`, which enters the section that just failed:

```text
preMainInitMemoryManager()
operator new(unsigned long)
std::__1::basic_string<...>::basic_string(char const*)
std::__1::system_error::system_error(std::__1::error_code, char const*)
std::__1::__throw_system_error(int, char const*)
std::__1::recursive_mutex::lock()
CriticalSection::enter()
ScopedCriticalSection::ScopedCriticalSection(CriticalSection*)
DynamicMemoryAllocator::allocateBytesDoNotZeroImplementation(int)
operator new(unsigned long)                     ← and around again
```

So there are two separate things wrong, and they need separate answers:

* **the recursion**, which turns any recoverable mutex error into a crash with no diagnosis. That is
  true whatever made the lock fail, and it is fixed here.
* **the lock failing at all**, which is not normal. Four things could do it — a mutex used before its
  constructor ran, one used after its destructor ran, memory the process cannot use, or genuine
  resource exhaustion — and they have four different fixes. This document measures which, rather
  than picking the plausible one.

Companion documents: [memory-shutdown-order.md](memory-shutdown-order.md) (#113, the lifetime that
stops the exit-end failure from happening), [real-input-menu-drive.md](real-input-menu-drive.md) §4.3
and §4.4 (where the crashes were observed), and
[apple-silicon-verification.md](apple-silicon-verification.md) §6 and §8.5.

## 1. What actually makes the lock fail — measured

`Core/GameEngine/Source/Common/System/tests/lock_failure_test.cpp`, run by
`scripts/native-lock-failure-test.py`, links the real engine archives and asks the platform directly.
Its `platform` case puts a **recursive `pthread_mutex_t` into each of the four states a crash report
cannot tell apart** and prints what the runtime says. On Ubuntu 22.04, glibc 2.35, clang 14:

| The mutex is | `pthread_mutex_lock()` says | So a crash report showing a failed lock… |
|---|---|---|
| live | `0`, locked | is not this |
| destroyed (`pthread_mutex_destroy` has run) | **`EINVAL` (22)** | **can be this** |
| never initialised (all-zero, i.e. bss before the constructor) | `0`, **locked** | cannot be this on glibc |
| overwritten (`0xff` filled) | `EINVAL` (22) | can be this |

Two of the four candidates are eliminated by that table plus the rest of the harness:

* **static-initialisation order** — a mutex used before its constructor ran. Excluded twice over.
  A zeroed mutex *locks* on glibc rather than failing, and the harness's `premain` case measures the
  stronger fact: a genuine allocation before `main` cannot enter a critical section at all, because
  the five section pointers are set by `main`'s prologue and `ScopedCriticalSection` does nothing
  with a null one:

  ```text
  an allocation before main ran                                  yes
  no allocator existed before it                                 yes
  it brought the memory manager up by itself                     yes
  TheDmaCriticalSection was null before it                       yes
  TheDmaCriticalSection was still null after it                  yes
  so a pre-main allocation can enter a critical section          no
  ```

  This **corrects the reading in §4.3** of [real-input-menu-drive.md](real-input-menu-drive.md),
  which took the `preMainInitMemoryManager()` frame as dating the crash to before `main`. That frame
  dates nothing: `operator new` in `GameMemory.cpp` calls `preMainInitMemoryManager()` before every
  allocation — all thirteen global allocation entry points do — so it appears in the innermost frame
  of *any* allocation, at any age of the process. A trace that contains `CriticalSection::enter()`
  is necessarily after `main`'s prologue, because before it there is no section to enter.

* **memory the allocator has not mapped** would not produce a lock error at all: touching it is a
  `SIGSEGV`/`SIGBUS` inside `pthread_mutex_lock`, not a return value. None of the nine reports is
  that.

* **resource exhaustion** (`EAGAIN` from a recursion count that cannot go deeper, `ENOMEM`) is not
  excluded by measurement — it is not reproducible on demand without a mutex held four billion times
  deep — but it is now *named if it happens*: the diagnostic in §3 prints the errno, so the next
  occurrence on hardware distinguishes it from `EINVAL` in one line instead of a debugger session.

* **a section whose destructor has run** is the one candidate that is both reachable and consistent
  with the reports, and it is reachable by construction: the exit-time trace
  (`__cxa_finalize_ranges` → `SimpleVecClass<Vector4>::~SimpleVecClass` →
  `DynamicMemoryAllocator::freeBytes` → `CriticalSection::enter`) is a static destructor allocating
  after static destruction has passed the sections. Its `EINVAL` is measured above, and #113's
  lifetime fix is what stops it — see §5.

One more measurement, because it explains why the allocator is still *live* while this happens
(`frames` case): an engine static allocates before `main` (`SmudgeSet::m_freeSmudgeList`), which puts
the memory manager on its pre-main path, and `shutdownMemoryManager()` then deliberately does
nothing. The retail process therefore reaches its static destructors with a working allocator and a
destroyed lock — allocation succeeds right up to the point where it takes the section.

```text
an engine static already allocated before main                 yes
shutdownMemoryManager() tore the manager down                  no (it was inited before main)
an allocation after shutdownMemoryManager() is still served    yes
```

**Classification.** The recursion is a **port defect** (Windows' `CRITICAL_SECTION` cannot report a
failure, so the path never existed there) and is fixed here. The destroyed-section cause is a **port
defect**, diagnosed by #113 and fixed by its lifetime; this slice adds the failure-path fix that
would have made it a one-line diagnosis instead of nine crash reports. The `platform` table's answers
are **measurements of this platform**, not of Apple's — see §6.

## 2. The deterministic reproduction

The crash is intermittent on hardware and does not occur on Linux by itself. What is deterministic is
the *failure*, and the harness produces it twice, in the two ways the process can arrive at it:

```sh
python3 scripts/native-build.py --level 1 --level 2 --level 3 --level 4 --with-shims --strict-link
python3 scripts/native-lock-failure-test.py
```

* `destroyed-dma` — mid-run, with the real allocator: a `CriticalSection` is constructed and
  destroyed in storage that outlives it, `TheDmaCriticalSection` points at it, and the next `new`
  fails the lock. **Synthetic-only** in the sense that the retail process does not point the pointer
  at a dead section mid-run; the *state* is the retail one.
* `exit-destroyed` — the retail route, with no pointer surgery at all: the section is a plain
  file-scope static, exactly as `PlatformMain.cpp`'s five were before #113, and a static that
  allocates from its destructor is declared so that it is destroyed after it. This is the arrangement
  that produced the Apple exit crash, reproduced in-process.

Both, with the fix in place, produce one line and stop:

```text
CriticalSection::lock failed: EINVAL (22) on TheDmaCriticalSection at 0x55db83c929d0,
memory manager live, pid 18379, mutex 0x55db83c929d8 = 00000000000000000000000000000000ffffffff00000000
the allocator cannot report a lock failure by allocating, so this aborts instead of recursing;
see docs/porting/allocator-lock-failure.md
```

The mutex bytes are in the message on purpose: they are how the *next* occurrence is classified
without this harness. All-zero is a mutex that was never constructed; the pattern above is glibc's
destroyed one (`__kind` set to `0xffffffff`); anything else is neither.

## 3. The fix: a failure path that cannot allocate

`CriticalSection` (both trees, `#ifndef _WIN32` only) now holds a `pthread_mutex_t` created with
`PTHREAD_MUTEX_RECURSIVE` instead of a `std::recursive_mutex`, and every one of `init`, `lock`,
`unlock` and `destroy` checks its return value. `Common/System/CriticalSectionFailure.cpp` reports:

* no exception, no `std::string`, no `printf` (glibc's stdio takes locks and buffers), no `malloc`.
  The message is formatted into a stack buffer by hand and handed to one `write(2)` on `stderr`.
* it names the errno, which of the five engine sections it was (by identity), whether the memory
  manager was up, the pid, and the mutex's own bytes.
* then it `abort()`s.

**It is deliberately not survivable, and there is no silent-success path.** Returning from `enter()`
without the lock would let the allocator walk its free lists unsynchronised — quietly wrong memory
is worse than a crash. The `destroy` path is the one exception to aborting: a destructor that aborted
would turn a leftover holder into a crash on the way out, which is the failure mode this file exists
to remove, so it reports and returns.

Why `pthread_mutex_t` rather than keeping `std::recursive_mutex` and catching the exception: the
exception *is* the allocation. `try`/`catch` around `lock()` does not help, because the recursion
happens while the exception object is being constructed, before any handler exists. The recursive
semantics are identical — `AsciiString::set()` re-enters the string section, which is why the mutex
must be recursive at all.

Windows keeps `CRITICAL_SECTION`, `InitializeCriticalSection`/`EnterCriticalSection` and its exact
lifetimes; none of the above compiles there. VC6 never sees `pthread.h`, the `[[noreturn]]`
declaration or `alignas`.

## 4. The negative control

The fix is measured against the path it replaced. `scripts/native-lock-failure-test.py` builds the
same harness a second time with `-DLOCK_FAILURE_NEGATIVE_CONTROL`, links it against archives with
`CriticalSectionFailure.cpp.o` **removed** (`ar d`), and supplies the standard library's behaviour in
its place — `throw std::system_error(...)`, whose message allocates. Nothing else differs between the
two binaries.

| | as it ships | negative control (the fix reverted) |
|---|---|---|
| `destroyed-dma` | one diagnostic, `SIGABRT` (-6) | recursion depth ≥ 9000, `SIGSEGV` (-11) |
| `exit-destroyed` | one diagnostic, `SIGABRT` (-6) | recursion depth ≥ 9000, `SIGSEGV` (-11) |

The runner fails if the shipping binary prints anything other than exactly one diagnostic line (a
path that can report twice is a path that allocated to report once), if it dies of `SIGSEGV` instead
of `SIGABRT`, if the control *survives*, or if the control dies before reaching depth 1000 — that
last one is what distinguishes "died of the recursion this slice is about" from "died of something
else". The `platform` case is the control for the control: if a runtime granted the lock on a
destroyed mutex, none of these cases could fail and the run says so rather than reporting a pass.

## 5. The exit end, on top of #113

#113 established that the five sections the allocator can take must outlive static destruction and
gave `PlatformMain.cpp` `ImmortalCriticalSection`. That is the fix for the exit crash and it stands;
what it did not cover is everything *else* that sets those pointers. Three native harnesses and the
simulation probe still used plain file-scope `CriticalSection` statics, which is why "the exit crash
was still observed" in #115's session: **every harness run on that Mac was reproducing the pre-#113
arrangement**, because the harness, not `PlatformMain.cpp`, was the entry point.

Converted here to `ImmortalCriticalSection`: `native_render_run.cpp`, `native_video_frame_run.cpp`,
`spikes/sim/src/sim_probe.cpp`. The new harness holds both arrangements so the difference is
measured rather than asserted — `exit-destroyed` aborts, and `exit-immortal`, identical but for the
lifetime, completes the late free and exits 0:

```text
main returning; the late free happens after this
the late destructor is about to free through the engine's allocator
late free completed
```

The lifetime is the fix; the failure path is the seatbelt. Neither replaces the other: with immortal
sections the lock does not fail, and if it ever does, it now says so in one line.

## 6. What remains Mac-only

Stated plainly, because this was measured on Linux and both crashes were observed on Apple Silicon:

* **The Apple runtime's own answers to §1's table are unknown here.** libc++ and Apple's pthread
  implementation need not agree with glibc — in particular whether a zeroed mutex locks (glibc: yes)
  and what a destroyed one reports. `scripts/native-lock-failure-test.py` is the check to run: it
  needs no retail data and prints the table.
* **The startup intermittency's trigger is not reproduced.** What is reproduced is the family — a
  failed lock in the allocator and the recursion it caused. Whether the nine startup reports were a
  destroyed section, an overwritten one or something else is answered by the *next* occurrence on
  that hardware, which now prints its errno, the section and the mutex's bytes instead of a stack
  overflow. If the diagnostic never appears again, the lifetime work removed the cause.
* **`std::recursive_mutex` is gone from this path, so libc++'s throwing `lock()` is no longer
  reachable at all** — that part of the fix is platform-independent by construction.

A Mac session should run, from a levels 1-4 build:

```sh
python3 scripts/native-lock-failure-test.py     # the table, the reproduction, the control
python3 scripts/native-memory-shutdown-test.py  # #113's ordering, on Apple's runtime
python3 scripts/native-instance-lock-test.py    # §4.4's diagnostic
```

and record the `platform` table's four lines in
[apple-silicon-verification.md](apple-silicon-verification.md).

## 7. Determinism

The simulation is lock-step and the replay gate is the oracle, so anything that could change
allocation order has to be argued and then checked. The lock path is a `pthread_mutex_lock()` call
where it was a `std::recursive_mutex::lock()` call — the same recursive mutex acquisition, in the
same place, with the same success path; libstdc++'s `recursive_mutex::lock()` *is*
`pthread_mutex_lock()` plus the throw. No allocation was added or removed on any success path, no
critical section was added or removed, and the new code executes only when a lock operation returns
non-zero, which on a working process is never. `CriticalSectionFailure.cpp` allocates nothing at all.

## 8. The instance lock (§4.4)

Also in this slice, and separate: a leftover process holding the single-instance `flock` made
`ClientInstance::initialize()` return `false` with nothing on stdout or stderr, which looked like a
dozen mysterious launch failures. It now names the lock file and the holder's pid, and tells a lock
that cannot be *used* apart from a lock that is *held*. Measured by
`scripts/native-instance-lock-test.py`, run as `ubuntu` rather than root so the unusable case runs
rather than skipping:

```text
Generals is already running: /tmp/.../685EAFF2-....lock is locked by pid 20438.
Close that process (or `kill 20438`) and start again.

Cannot take the single instance lock /tmp/.../685EAFF2-....lock: open failed with
Permission denied (13). Refusing to start rather than risk a second instance.
```

The free-lock case succeeds and prints nothing, and the refusal itself is unchanged:
`initialize()` still returns `false` in every case. The table of messages and the Windows note belong
with the other startup reporting: see
[init-failure-reporting.md](init-failure-reporting.md) §"The single instance lock".
