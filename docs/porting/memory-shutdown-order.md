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
| Main Menu → Exit | Apple Silicon | **UNMEASURED** |
| Quit from a running skirmish | Apple Silicon | **UNMEASURED** |
| Quit from a running campaign mission | Apple Silicon | **UNMEASURED** |
| No crash report written in `~/Library/Logs/DiagnosticReports`, no hang | Apple Silicon | **UNMEASURED** |

The three retail quit paths and the crash-report directory are owed by a Mac session: this slice was
run on Linux deliberately (the single M1 Pro outpost is reserved for renderer measurement), so the
Apple Silicon rows are not claimed. The mechanism is LP64 arithmetic, identical on both targets, and
the fix is verified against the same source on x86-64 — but "no crash report on the Mac" is a
measurement nobody has taken yet.

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
