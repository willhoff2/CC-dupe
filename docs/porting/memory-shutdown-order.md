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
