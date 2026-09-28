# `_strdup(nullptr)`: cloning a particle emitter into the fog of war

WW3D2's particle emitter and its definition class own their `Name` / `UserString` buffers as raw
`char *`, duplicated with `::_strdup` and released with `::free`. Six of those call sites — across
three classes in the Zero Hour and shared trees, plus four duplicates in the base game's copy —
passed a pointer that is routinely, and in one case *always*, null:

```cpp
// GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/part_emt.cpp, copy constructor, before the fix
	NameString(::_strdup (src.NameString)),
	UserString(::_strdup (src.UserString)),
```

`_strdup` has to measure its argument before it can copy it, so it calls `strlen`, which reads
address zero. The destructor forty lines below already assumed the pointers could be null and
guarded both `::free` calls (`part_emt.cpp:186-194`); the copy constructor did not.

This is an **upstream defect, not a port regression**. `git blame` puts both lines on `3d0ee53a05`,
"Initial commit of Command & Conquer Generals and Command & Conquer Generals Zero Hour source code"
(LFeenanEA, 2025-02-27) — the EA source release, unchanged since. It survived on 32-bit Windows
because the MSVC CRT's `_strdup` historically returned `NULL` for a `NULL` argument instead of
dereferencing it, so the expression quietly produced the same `NULL` the field already held.

## 1. MEASURED: the crash

A human playing a real game on `main` at `097101125`, 2026-09-27 21:50:41 -0500. Crash report
`~/Library/Logs/DiagnosticReports/zh-2026-09-27-215041.ips`: MacBookPro18,1, macOS 26.6.1 (25G76),
ARM-64, not translated. `EXC_BAD_ACCESS` / `SIGSEGV`, `KERN_INVALID_ADDRESS at 0x0000000000000000`,
faulting thread 0 (the main thread). Top of the stack, down to `GameMain`:

```
_platform_strlen                    libsystem_platform.dylib
strdup                              libsystem_c.dylib
ParticleEmitterClass::ParticleEmitterClass(ParticleEmitterClass const&)
ParticleEmitterClass::Clone() const
W3DRenderObjectSnapshot::update(RenderObjClass*, DrawableInfo*, bool)
W3DRenderObjectSnapshot::W3DRenderObjectSnapshot(RenderObjClass*, DrawableInfo*, bool)
W3DGhostObject::snapShot(int)
PartitionData::getShroudedStatus(int)
Object::getShroudedStatus(int) const
GameClient::update()  ->  W3DGameClient::update()  ->  GameEngine::update()  ->  GameMain()
```

That is the fog-of-war ghost path: when a unit the player has seen leaves or dies inside shroud, the
engine snapshots its render objects so the remembered shape persists, and snapshotting a render
object clones it. Cloning a `ParticleEmitterClass` runs the copy constructor above.

## 2. MEASURED: why that clone could not have worked

The emitter's `UserString` is **always** null, so the copy constructor faulted **every** time, not
occasionally:

* `part_emt.cpp:102` initialises it to `nullptr` in the only real constructor;
* `ParticleEmitterClass` has no setter for it — the string is written nowhere in either game tree;
* `part_emt.h:221` defines its accessor as `virtual const char *Get_User_String () const { return
  nullptr; }`, i.e. the class answers "no user string" unconditionally.

The member exists only so the destructor can free it. `NameString` is the opposite case: it is set
to `"ParticleEmitter"` at `part_emt.cpp:101` and replaced by `Set_Name`, so it is non-null in
practice — but `Set_Name` itself did an unguarded `::_strdup (pname)` (`part_emt.cpp:843` before the
fix) while guarding the `::free` immediately above it, so `Set_Name(nullptr)` was a crash too.

## 3. INFERRED: why it surfaced now

The stack is reachable only when something with a particle emitter dies or leaves visibility inside
shroud. PR #161 (`df3dde0ad`) fixed `getDeathTypeFlag`'s `1UL << (dt - 1)`, which at 64 bits had been
rejecting **every** normal death before any die module body ran — see
[`death-flag-shift.md`](death-flag-shift.md). Until that landed, nothing on this platform was
destroyed, so this clone path was rarely or never reached with a particle-emitting unit. Fixing the
death path exposed the bug behind it.

This is **INFERRED**, not measured: no before/after instrumentation of `W3DGhostObject::snapShot`
call counts was taken, and the ghost path is also reachable without a death (a unit merely walking
out of vision). What is measured is only that the crash report post-dates #161.

## 4. The fix

Guard every `::_strdup` of a possibly-null pointer, mirroring what the destructors already assume:

| file | site | before | after |
|---|---|---|---|
| `GeneralsMD/…/WW3D2/part_emt.cpp` | copy ctor, `:149` | `::_strdup (src.NameString)` | `src.NameString != nullptr ? … : nullptr` |
| `GeneralsMD/…/WW3D2/part_emt.cpp` | copy ctor, `:150` | `::_strdup (src.UserString)` | `src.UserString != nullptr ? … : nullptr` |
| `GeneralsMD/…/WW3D2/part_emt.cpp` | `Set_Name`, `:845` | `::_strdup (pname)` | `(pname != nullptr) ? … : nullptr` |
| `GeneralsMD/…/WW3D2/part_ldr.cpp` | `Set_User_String`, `:273` | `::_strdup (pstring)` | `(pstring != nullptr) ? … : nullptr` |
| `GeneralsMD/…/WW3D2/part_ldr.cpp` | `Set_Name`, `:285` | `::_strdup (pname)` | `(pname != nullptr) ? … : nullptr` |

the same four in the base game's copy (`Generals/…/part_emt.cpp:161`, `:832`;
`Generals/…/part_ldr.cpp:270`, `:282` — the base game's emitter has no `UserString` member at all,
so it has four sites, not five), and one more in the shared tree:

| file | site | before | after |
|---|---|---|---|
| `Core/…/WW3D2/agg_def.h` | `AggregateDefClass::Set_Name`, `:108` | `::_strdup (pname)` | `(pname != nullptr) ? … : nullptr` |

Nothing changes for a non-null argument: the string is still duplicated into fresh storage with the
same call. That matters because this is render-path code and **Windows is the oracle** — the replay
gate (`Replay Check GeneralsMD`) compares against it — so the fix is null-safety and nothing else.

### 4.1 The sibling class, which had the same defect

`ParticleEmitterDefClass` is not on the measured stack, but it carries the identical bug on a path
that needs no shroud and no death:

```cpp
// part_ldr.cpp:161-162, inside operator=, which the copy constructor calls as `(*this) = src`
	Set_Name (src.Get_Name ());
	Set_User_String (src.Get_User_String ());
```

Both accessors return the source's raw pointer, both start life `nullptr` (`part_ldr.cpp:71`, `:73`),
and both setters did an unguarded `::_strdup`. So **copying a default-constructed definition was an
unconditional segfault**. That is what §5 reproduces, because unlike the emitter it can be built
standalone.

### 4.2 A third class with the same defect: `AggregateDefClass`

Sweeping `strdup` in *all three* WW3D2 trees — `Core/…/WW3D2`, `Generals/…/WW3D2`,
`GeneralsMD/…/WW3D2` — turned up one more instance of the exact pattern, in the shared tree that
both games compile:

```cpp
// Core/Libraries/Source/WWVegas/WW3D2/agg_def.h:108, before the fix
void Set_Name (const char *pname) { SAFE_FREE (m_pName); m_pName = ::_strdup (pname); }
```

The reachability argument is the same one, line for line: `m_pName` starts `nullptr`
(`agg_def.cpp:68`), the copy constructor is `(*this) = src` (`agg_def.cpp:90`), and `operator=`
hands the source's own name straight back in — `Set_Name (src.Get_Name ())`, `agg_def.cpp:147`.
`AggregateDefClass::Clone()` (`agg_def.h:110`) is that copy constructor, so **cloning a nameless
aggregate definition was an unconditional segfault** too. Fixed here, and covered by §5's scan.

### 4.3 What the sweep found and deliberately left

Everything else the sweep reached was inspected and **left alone**. The reasons differ, so they are
listed rather than summarised:

| site | argument | why it is left |
|---|---|---|
| `hlod.cpp:372` (both games) | `phtree->Get_Name ()` | already inside `if (phtree != nullptr)`; `HTreeClass::Name` is a char array |
| `hlod.cpp:648`, `:649`, `:820`; `part_ldr.cpp:525`/`:518`; `agg_def.cpp:587` | `header.Name`, `header.HierarchyName`, `subobjdef.Name` | char arrays inside POD chunk structs read off disk — not pointers, cannot be null |
| `distlod.cpp`, `hanim.cpp`, `hcanim.cpp`, `texture.cpp` | various, via `nstrdup` | `WWLib/nstrdup.cpp:57` already opens with `if (str == nullptr) return nullptr;` — these were never exposed |
| `font3d.cpp:62` | `filename` | a constructor parameter supplied by callers, not a class's own possibly-null member |
| `hlod.cpp:369`, `:398` (both games) | `src_lod.Get_Name ()`, `prender_obj->Get_Name ()` | a virtual accessor whose base returns the literal `"UNNAMED"` (`rendobj.h:229`); reaching null needs an `HLodDefClass` whose `Name` was never loaded, which nothing here shows happening |

One find outside WW3D2 deserves naming rather than burying, because it **is** the same shape:

```cpp
// Core/Libraries/Source/WWVegas/WWAudio/WWAudio.h:522 -- NOT changed by this PR
_CACHE_ENTRY_STRUCT ()
    : string_id (0), buffer (nullptr) {}
_CACHE_ENTRY_STRUCT &operator= (const _CACHE_ENTRY_STRUCT &src)
    { string_id = ::strdup (src.string_id); REF_PTR_SET (buffer, src.buffer); return *this; }
```

Default-constructed `string_id` is null and `operator=` duplicates it unguarded, so assigning from
an empty cache entry faults exactly the way the emitter did. It is left out of this PR on purpose:
it is in WWAudio rather than WW3D2, it also leaks the string it overwrites (so a null guard alone is
not the whole fix), and the surrounding cache code has not been traced for whether a null
`string_id` would simply fault later at the comparison instead. Turning a crash at assignment into a
crash one frame further on is not an improvement, and guessing is worse than reporting.

Reachability of that struct, and of the remaining `strdup` sites in `WWAudio`, `WWLib`,
`WWSaveLoad` and `Core/GameEngine/Source/GameNetwork`, is **UNMEASURED**. None is in WW3D2, none is
on the measured stack, and none was audited here.

## 5. MEASURED: the gate

`scripts/native-particle-emitter-strdup-test.py`, run in CI by the `native-build*` jobs and listed in
[`.agents/skills/native-port-measure/SKILL.md`](../../.agents/skills/native-port-measure/SKILL.md).
It has two halves, because the three classes are not equally reachable.

**A compiled test** (`Core/Libraries/Source/WWVegas/WW3D2/tests/particle_emitter_strdup_test.cpp`)
links the **real** `part_ldr.cpp` — plus `WWMath/v3_rnd.cpp` and `WWLib/random.cpp`, because
`Create_Randomizer` builds a real `Vector3SolidBoxRandomizer` for a zeroed definition
(`CLASSID_SOLIDBOX` is 0) — against ~18 stub definitions written into the test file itself, and
actually copies a `ParticleEmitterDefClass`. It measures three things:

* `::_strdup(nullptr)` faults here. Measured, not assumed: the call runs in a forked child and the
  child's termination signal is the result. On this machine it reports
  `::_strdup(nullptr) died on signal 11 (Segmentation fault: 11)`.
* copying a default-constructed definition does not fault, and leaves both strings null.
* the guarded form yields null for null and a byte-exact, independently allocated copy otherwise —
  including for `""`, which must stay non-null.

Before/after, on this machine (macOS 26.6.1, arm64, Apple clang 21.0.0):

| tree | result |
|---|---|
| `HEAD` (`097101125`), the five files unmodified | **FAIL** — `died on signal 11`, harness exit 245 |
| with this PR's fix | **PASS** — 18 checks, 0 failures |

**A source scan**, in the harness, asserting that every `::_strdup` argument in the six scanned
files is a string literal, null-guarded on the same line, or one of the few expressions that cannot
be null by construction (`header.Name`, named in the script with its reason). This is what pins the
two call sites in `ParticleEmitterClass`'s copy constructor, and `AggregateDefClass::Set_Name`:
`ParticleEmitterClass` **cannot** be driven standalone — its constructor needs a
`ParticleBufferClass`, and a standalone link would have to stub 89 symbols including two complete
render-object vtables (measured with `nm -u` on the compiled object). The scan reports 15 call sites
across the 6 files, all accounted for.

One further check, run once rather than in CI: every symbol the test's four objects reference is
defined either inside that same object set or by libc / libc++ / libc++abi (`nm -u` minus `nm
--defined-only`, residual inspected by hand — 71 undefined, 305 defined, residual entirely runtime).
That closure is why the test cannot fail on the Linux clang-14 runner with the undefined references
that ld64-versus-GNU-ld differences have caused here before.

## 6. Verified vs owed

| claim | status |
|---|---|
| the crash, its stack and its address | **MEASURED** — the `.ips` report in §1 |
| `UserString` is always null, so the emitter copy always faulted | **MEASURED** — source, §2 |
| `_strdup(nullptr)` faults on this libc | **MEASURED** — §5, forked child, signal 11 |
| copying a default definition faulted before the fix and does not after | **MEASURED** — §5 |
| the fix does not change the non-null case | **MEASURED** — §5, byte-exact copy check |
| the fix is in the game binary and the binary still links | **MEASURED** — `scripts/native-build.py --level 1..4 --with-shims --strict-link`: 981 objects, 0 failures, strict link 0 unresolved, 27.1 MiB arm64 executable |
| the original crash no longer happens in a real game | **OWED** — the game was not launched for this change. §5 reproduces the fault and the fix, but only on the definition class; the emitter's own copy constructor is covered by the source scan and by expression identity, not by a run |
| Windows behaviour is unchanged | **INFERRED** — from the diff (non-null inputs take the same call) and from §5's byte-exact check, not from a Windows run. The replay gate is the oracle that would decide it |
| the base game's copy is fixed too | **MEASURED** by the source scan; **UNMEASURED** by any build — `scripts/native-build.py` compiles only `Core/` and `GeneralsMD/` (`LEVELS` in the script names no `Generals/` directory), so the native build never sees those four lines. `RTS_BUILD_GENERALS` defaults `ON` in `cmake/config-build.cmake`, so the CMake build does |
| `AggregateDefClass::Clone()` faulted before the fix | **INFERRED** — from source, §4.2: null `m_pName`, `operator=` feeding `Set_Name`. Not reproduced at runtime; it is covered by the scan, not by a run |
| the `hlod.cpp:369` / `:398` path can or cannot produce null | **UNMEASURED** — §4.3 |
| `WWAudio.h:522`'s identical shape is reachable | **UNMEASURED**, and deliberately not fixed — §4.3 |
