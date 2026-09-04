# The death and veterancy flags at 64 bits: nothing ever dies

`DeathTypeFlags` and `VeterancyLevelFlags` are two 32-bit masks that decide whether a die module runs
at all. Both put enumerator `e` on bit `e - 1`:

```cpp
// Core/GameEngine/Include/GameLogic/Damage.h, unchanged since the initial commit (3d0ee53a0)
inline Bool getDeathTypeFlag(DeathTypeFlags flags, DeathType dt)
{
	return (flags & (1UL << (dt - 1))) != 0;
}
```

and both enumerations start at zero — `DEATH_NORMAL = 0` (`Damage.h:171`), `LEVEL_REGULAR = 0`
(`GameCommon.h:200`). For those two the shift count is **-1**, which is undefined behaviour, and the
two platforms disagree about it in a way that no compile, no link and no static analysis sees:

| | `sizeof(unsigned long)` | shift count truncates to | `1UL << (0 - 1)` | `0xffffffff & that` |
|---|---:|---|---|---|
| 32-bit Windows (every preset here: `vc6`, `win32`, `mingw-w64-i686`) | 4 | 5 bits → 31 | `0x80000000` | non-zero — **passes** |
| LP64 (macOS arm64, Linux x86-64) | 8 | 6 bits → 63 | `1UL << 63` | zero — **always false** |

Being undefined, a compiler owes nothing here — the table is what the *hardware* does when the shift
survives to a real `shl`/`lsl` with a runtime count, which is what both builds emit because `dt`
comes from INI and from `DamageInfo` at run time. The LP64 row is measured on this machine (end of
§1); the Windows row is not measured here, and §6 says what stands behind it instead.

So retail works **by accident**: the flag for a normal death is bit 31, which
`DEATH_TYPE_FLAGS_ALL = 0xffffffff` happens to have set. On LP64 the same expression asks for bit 63
of a 32-bit mask and gets nothing, and `DieMuxData::isDieApplicable`
(`GeneralsMD/Code/GameEngine/Source/GameLogic/Object/Die/DieModule.cpp:69`) rejects **every** normal
death before any die module body runs.

## 1. Measured: what that costs, in a real game

**MAC-REPLAY** (the label from [`combat-probe.md`](combat-probe.md) §0): `main` at `4eb0ec3fa`, Apple
M1 Pro, `./zh -headless -replay` of a `.rep` a human recorded from a real skirmish, launched under
LLDB — `scripts/macos-combat-probe.py` is attach-only and attach is refused with developer mode off.
The replay ran to completion, exit 0, 14,344 logic frames, all 36 breakpoints resolved.

| entry | hits, `main` `4eb0ec3fa` |
|---|---:|
| `DieMuxData::isDieApplicable` entered (`DieModule.cpp:69`) | 66 |
| — rejected at the **death-type** gate (`:70`) | **65** |
| — rejected at the **veterancy** gate (`:74`) | **1** |
| — rejected at the exempt / required status gates (`:78`, `:82`) | 0 |
| — **accepted** (`:84`) | **0** |
| `SlowDeathBehavior::onDie` | 36 |
| `SlowDeathBehavior::beginSlowDeath` / `update` | 0 / 0 |
| `JetSlowDeathBehavior::onDie` → `beginSlowDeath` | 2 → 0 |
| `DestroyDie::onDie` | 8 |
| `KeepObjectDie::onDie` | 0 |
| `Object::onDie` | 15 |
| `GameLogic::destroyObject` / `processDestroyList` | 484 / 14,344 |

Read the first block downward: the gate is entered 66 times and passes 0. Everything below follows
from that. `SlowDeathBehavior::onDie` returns at `SlowDeathBehavior.cpp:478`, so it never reaches
`ai->markAsDead()` at `:487`; `DestroyDie::onDie` returns at `DestroyDie.cpp:56`, before
`TheGameLogic->destroyObject(obj)`. `KeepObjectDie::onDie` was never called, which rules out "hulks
are by design", and the 484 `destroyObject` calls prove the destroy machinery itself is healthy —
they come from crates, parachutes and `Player::killPlayer`, not from a die module.

What a player sees is the dead-object curve. Counting `EFFECTIVELY_DEAD` in `m_privateStatus` over
the replay, excluding the 105 map props that start dead:

```
   frame  objects   dead-new
    7423      268          3
   11923      277          5
   12591      283         10
   13150      283         12
   14163      279         15     <- one per Object::onDie, and it only ever climbs
```

At the last sample 15 units and buildings are dead and still in `m_objList`, at 0 hp, and one of them
is still moving:

```
  id=243   Tank_ChinaInfantryRedguard   samples=9   moved_after_death=True   ai=['AI_GUARD']
        f12512   pos=[1089.5, 630.5, 18.8] ai=AI_GUARD
        f12591   pos=[1084.0, 657.1, 18.8] ai=AI_GUARD
```

That is the defect in one line: `SlowDeathBehavior::onDie` returns before `ai->markAsDead()`, so the
corpse keeps a live AI and walks its guard patrol.

Verified in the live target at the gate, on this machine: `(0xffffffffu & (1UL << (0 - 1))) != 0`
evaluates to **0**, and `sizeof(unsigned long)` is **8**.

## 2. The fix: reproduce the 32-bit truncation, do not renumber

The constraint that decides the shape is that **Windows behaviour must not change**. This code is in
the determinism path — `m_deathTypes` comes from INI and gates which module runs, which changes the
random draw in `SlowDeathBehavior::onDie` and everything after it — and CI's `Replay Check
GeneralsMD` replays committed `.rep` files against a `vc6` build and compares. Windows is the oracle;
the port agrees with it or it is wrong.

So the truncation the 32-bit hardware performs is done explicitly, in one place per flag set, and
get/set/clear all go through it:

```cpp
inline UnsignedInt deathTypeFlagBit(DeathType dt)
{
	return 1U << ((static_cast<UnsignedInt>(dt) - 1U) & 31U);
}
```

`1U` rather than `1UL` because `unsigned long` is the whole problem; `& 31U` because that is what
`shl` does with a 5-bit count field; `static_cast<UnsignedInt>` first so the subtraction is unsigned
and there is no signed overflow or implementation-defined `&` on a negative value left in the
expression. The result is identical to today's on 32-bit Windows for every enumerator, including
`DEATH_NORMAL`, and identical to Windows on LP64.

**Renumbering was considered and rejected.** `1U << dt` is the cleaner C++ and would need no mask at
all. It also moves every flag by one bit, so `DeathTypes = ALL -CRUSHED` in INI produces a different
`m_deathTypes`, a different set of applicable modules, a different `GameLogicRandomValue` draw and a
different simulation. That is a replay-breaking change on Windows, and it cannot be proved harmless —
the masks are built from INI text at load time and read by `isDieApplicable` in the same process, so
nothing outside the process pins them, but the *behaviour* they select is exactly what the replay
gate compares. Not doing it.

Bit assignment, before and after, on both platforms:

| enumerator | 32-bit Windows, before | 32-bit Windows, after | LP64, before | LP64, after |
|---|---|---|---|---|
| `DEATH_NORMAL` (0) | bit 31 | bit 31 | *nothing* | bit 31 |
| `DEATH_NONE` (1) … `DEATH_POISONED_GAMMA` (20) | bits 0…19 | bits 0…19 | bits 0…19 | bits 0…19 |
| `LEVEL_REGULAR` (0) | bit 31 | bit 31 | *nothing* | bit 31 |
| `LEVEL_VETERAN`…`LEVEL_HEROIC` (1…3) | bits 0…2 | bits 0…2 | bits 0…2 | bits 0…2 |

`DEATH_NORMAL` owns bit 31 alone only while no `DeathType` reaches 32 — the enum reserves
`DEATH_EXTRA_2`…`DEATH_EXTRA_8` "for modders", so that is a live risk. Both headers now carry a
`STATIC_ASSERT_ALWAYS` for it, not a plain `static_assert`, because VC6 defines `static_assert` away
to nothing (`Dependencies/Utility/Utility/CppMacros.h:34`) and Windows is the one toolchain where a
silently skipped alias would go unnoticed.

### Consistency with the mask constants and the INI parsers

The only things that build or read these masks are:

| site | what it does |
|---|---|
| `Core/GameEngine/Source/Common/INI/INI.cpp:1911` `parseDeathTypeFlags` | starts at `DEATH_TYPE_FLAGS_ALL`, `ALL`/`NONE` set the whole word, `+NAME`/`-NAME` call `setDeathTypeFlag`/`clearDeathTypeFlag` |
| `Core/GameEngine/Source/Common/INI/INI.cpp:1825` `parseVeterancyLevelFlags` | the same, with `setVeterancyLevelFlag`/`clearVeterancyLevelFlag` |
| `DieModule.cpp:46` `DieMuxData::DieMuxData` | defaults both members to `*_FLAGS_ALL` |
| `DieModule.cpp:69`, `:73` `isDieApplicable` | the only reader, through `get*Flag` |

There is no third way in and no serialisation out: the masks are `DieMuxData` members built from INI
text at load and never `Xfer`ed, so a save game and a replay both re-derive them from the same INI.
Because set/get/clear now share one `*FlagBit` helper they cannot disagree with each other, and both
hardcoded constants stay correct by inspection: `_FLAGS_ALL` is `0xffffffff`, which contains bit 31
and bits 0…19, so it accepts every enumerator; `_FLAGS_NONE` is `0`, which accepts none. Those two
are asserted in the unit test rather than left to inspection.

## 3. The unit test

`Core/GameEngine/Source/Common/System/tests/death_veterancy_flags_test.cpp`, run by
`scripts/native-death-veterancy-flags-test.py`, wired into `Native Port CI` as "Run the
death/veterancy flag bit-assignment test". It includes the two real headers and links nothing, so it
runs anywhere they parse — Windows included, where it passes both before and after this change,
which is the point of it.

159 checks: the retail bit for every `DeathType` and every `VeterancyLevel` written out
independently of the implementation (bit 31 for the zero-valued enumerator, `1 << (e - 1)` for the
rest), `_FLAGS_ALL` accepting all and `_FLAGS_NONE` accepting none, set/get and clear/get round
trips, no two enumerators sharing a bit, `DeathTypes = ALL -CRUSHED` still letting a normal death
through, and `sizeof` of both flag types being 4.

Against the pre-fix headers on LP64 (extracted from `origin/main` into a shadow include directory,
same test binary otherwise):

```
159 checks, 11 failure(s)
FAIL: DEATH_TYPE_FLAGS_ALL accepts DEATH_NORMAL
FAIL: DEATH_NORMAL is bit 31, as it is on 32-bit Windows (got 0x00000000, expected 0x80000000)
FAIL: VETERANCY_LEVEL_FLAGS_ALL accepts LEVEL_REGULAR
FAIL: LEVEL_REGULAR is bit 31, as it is on 32-bit Windows (got 0x00000000, expected 0x80000000)
...
```

Against this branch: `159 checks, 0 failure(s)`.

## 4. Green: the same replay, the same breakpoints

Same machine, same `.rep`, same breakpoints, this branch built by
`scripts/native-build.py --level 1 --level 2 --level 3 --level 4 --with-shims --strict-link`
(981/981 objects, 0 compile failures, 0 unresolved symbols).

### 4.1 The same two events, red and green

The cleanest comparison is a decoded gate event from each run at the same frame, on the same object,
with the same INI-derived masks. Two of them, one per gate:

| | frame | object | `m_deathTypes` | `m_deathType` | red, `4eb0ec3fa` | green |
|---|---:|---|---|---|---|---|
| the **veterancy** gate | 7423 | `Tank_ChinaInfantryRedguard` | `0x00000004` (`+BURNED`) | 3 = `DEATH_BURNED` | `:74` — rejected | **`:84` — accepted** |
| the **death-type** gate | 7443 | `Tank_ChinaJetMIG` | `0xffffffff` (`ALL`) | 0 = `DEATH_NORMAL` | `:70` — rejected | **`:84` — accepted** |

The first is the veterancy half on its own: `0x4` has bit 2, so `DEATH_BURNED` (3) passed the
death-type gate in *both* runs, and the module was thrown away on `LEVEL_REGULAR` alone. The second is
the death-type half. Both frames are before the divergence point below, so the two runs are still
simulating the same game there.

### 4.2 The replay diverges, and the engine says so

This `.rep` was recorded **by the defective build**, so its recorded CRCs encode the defective
simulation. A build that fixes the defect must diverge from it, and does:

```
Simulating Replay "probe-green.rep"
CRC Mismatch in Frame 7500
```

The first die module ran at frame 7423. `REPLAY_CRC_INTERVAL` is 100 (`Recorder.cpp:60`), so 7500 is
the **first checkpoint after the first death**. That is a positive result, not a regression: it is the
tightest available confirmation that the change is real and that nothing before the first death moved.
Red logged no mismatch and exited 0.

`ReplaySimulation` stops at the first mismatch (`ReplaySimulation.cpp:114`), which is why the first
green run ended at frame 7502. The numbers below come from a re-run with `CNC_CRC_DIAG=/dev/null
CNC_CRC_DIAG_CONTINUE=1`, the opt-in mode that keeps the playback going, so the whole game could be
watched: `Game Time: 07:57/07:57`, 14,335 `processDestroyList` frames against red's 14,344, exit 1
(the mismatch is counted as an error). **Everything after frame 7500 is a different game** — same
inputs, different simulation — so those counters show that the death path executes end to end, not
that the two runs are comparable.

### 4.3 The death path, over the whole replay

| entry | red, `4eb0ec3fa` | green | |
|---|---:|---:|---|
| `isDieApplicable` entered (`:69`) | 66 | 84 | |
| — rejected, death type (`:70`) | 65 | 16 | modules that genuinely do not list that death type |
| — rejected, veterancy (`:74`) | 1 | **0** | |
| — **accepted** (`:84`) | **0** | **68** | |
| `SlowDeathBehavior.cpp:481` — past `isDieApplicable` | 0 | **22** | |
| `SlowDeathBehavior.cpp:491` — past `ai->markAsDead()` | 0 | **22** | the corpse's AI is now dead |
| `SlowDeathBehavior.cpp:517` — `sdu->beginSlowDeath()` | 0 | **22** | |
| `SlowDeathBehavior::beginSlowDeath` | 0 | **22** | |
| `JetSlowDeathBehavior::beginSlowDeath` | 0 | **2** | |
| `Object::onDie` | 15 | 22 | |
| `GameLogic::destroyObject` | 484 | 322 | different game after 7500 |

Every one of the 22 slow deaths reached `markAsDead()` and `beginSlowDeath()`; none did in red.

The dead-object curve is the other half. Excluding the 105 map props that start dead, and dropping
the final teardown sample where the engine marks everything:

| | red, `4eb0ec3fa` | green |
|---|---|---|
| dead objects above baseline, by frame | 3 → 5 → 10 → 12 → **15**, never falling | 3 → 6 → **0** → 5 → **0** → 0 → 7 |
| returns to baseline | never | at frames 7884, 9595, 12913 |

Corpses now appear, run their slow death, and leave `m_objList`. In red they only ever accumulated.

## 5. The `combat-probe.md` §5 tension, resolved

[`combat-probe.md`](combat-probe.md) §5 records an older build (`01c3105f5`) where `DestroyDie::onDie`
fired at frame 60832 on a Command Center and `buildings_lost` moved to 1, and concludes "objects die
and get removed: works". Taken at face value that says the gate passed on a build predating this
branch, which would contradict a defect the code has carried since `3d0ee53a0`. It does not, for two
reasons, and neither of them needs a behaviour change between the two builds:

* **The breakpoint is at function entry, not past the gate.** `macos-combat-probe.py` installs it
  with `BreakpointCreateByName("DestroyDie::onDie")`, and LLDB resolved it — in this branch's own
  run, printed into `gate.driver.log` — to `DestroyDie.cpp:56`, which *is* the
  `if (!isDieApplicable(damageInfo))` line. A hit there records that `Object::onDie` dispatched to
  the module. It says nothing about whether the module's body ran. The red run above hit
  `DestroyDie::onDie` 8 times with 0 gate acceptances, which is the same shape.
* **`buildings_lost` is not credited by a die module.** It comes from
  `Object::scoreTheKill` (`Object.cpp:2998`), which calls `addObjectLost` at `:3020` and
  `addObjectDestroyed` at `:3035`, and which is called from the *killer's* damage path
  (`ActiveBody.cpp:403`, `:418`, `:470`, `:672`, `AIStates.cpp:452`, the projectile behaviours). §5's
  sentence "`addObjectDestroyed` is credited from `DestroyDie::onDie` at the end of the slow death"
  is wrong about the call site; the counters move whether or not any die module runs.

§5 also records, without drawing the conclusion, the corroborating half: `SlowDeathBehavior::
beginSlowDeath` and `update` had **0 hits for the whole replay**, and no module was shown to own the
1,248-frame delay it noticed. That is what the defect predicts.

Checked against history so the "did something change?" question is closed rather than assumed:
`git log -L 239,252:Core/GameEngine/Include/GameLogic/Damage.h` returns exactly one commit,
`3d0ee53a0` — the initial import. `DEATH_NORMAL = 0` and `LEVEL_REGULAR = 0` have never been
renumbered, `DeathTypeFlags`/`VeterancyLevelFlags` have been `UnsignedInt` throughout, and the only
later commits touching those regions are cosmetic (`8ee3d219a` added the `TheDeathNames` array-size
assertion and constness, `5b804ca74` gave the enums an underlying type for GCC). So §5 was measuring
a build with the same defect, and its §5 heading overstates what its evidence supports.

§5 is annotated in place rather than rewritten: its measurements stand, its inference does not.

## 6. Verified, and owed

**Verified (MEASURED, this branch, Apple M1 Pro / macOS 26.6.1 / Darwin 25.6.0 arm64):**

* the red and green replay runs above, same binary flags, same `.rep`, same breakpoint set;
* `native-build.py --level 1..4 --with-shims --strict-link`: 981/981 objects, 0 failures,
  0 unresolved symbols;
* the unit test, red against `origin/main`'s headers and green against this branch;
* `python3 -m flake8 scripts/`.

**Boundaries of the green run, stated so the table above is not over-read:**

* everything after frame 7500 is a **different game**. The `.rep` was recorded by the defective
  build, so the fixed build necessarily diverges from it; §4.1's two events and everything before
  7500 are the directly comparable part;
* `SlowDeathBehavior::update` and `JetSlowDeathBehavior::update` were **not** installed in the
  full-length green run. With the fix they fire once per frame per dying object — the code red never
  reached — and each LLDB stop costs milliseconds, which turned a 7-minute replay into hours. Their
  green counts are therefore unmeasured; the shorter first green run had them at 60 and 59 against
  red's 0 / 0, and `beginSlowDeath` (22 / 2 against 0 / 0) shows the same path was taken;
* `DestroyDie::onDie` is 8 in red and 0 in green. That is the divergence, not a regression: after
  7500 the two games kill different objects, and green's happened not to have a `DestroyDie` module.

**Owed:**

* **Windows is unchanged — INFERRED, not measured here.** The argument is the table in §1 plus the
  fact that the bit assignment is identical for every enumerator on a 32-bit `unsigned long`. It is
  not measured on this branch by this session; `Replay Check GeneralsMD` (`vc6+t+e` and
  `vc6-releaselog+t+e`, over `GeneralsReplays/GeneralsZH/1.04`) is the oracle, it is live in this
  fork, and `Core/**` is in `ci.yml`'s `shared` path filter, so this PR is gated by it.
* **Linux is unmeasured.** Nothing here ran on Linux. The defect is a property of LP64 rather than of
  macOS, so Linux is expected to behave the same way, but that is inference.
* **No cross-platform replay or CRC claim is made.** The green run shows the death path executing;
  it does not show that the macOS simulation matches the Windows one frame for frame. That needs the
  CRC work in [`crc-divergence.md`](crc-divergence.md).
* **The rest of §5's "works" classification.** This branch shows removal now happens through the die
  modules; it does not re-run the two skirmishes §5 was written from.

## 7. Reproducing

```sh
# build (the shell must not be under Rosetta)
arch -arm64 python3 scripts/native-build.py --level 1 --level 2 --level 3 --level 4 \
    --with-shims --strict-link --build-dir build/native-arm64-fix

# the focused test, no game data needed
python3 scripts/native-death-veterancy-flags-test.py

# the replay probe: launch under LLDB (attach is refused with developer mode off) with an
# interpreter carrying the lldb module, and pass os.environ through SBLaunchInfo or the game
# cannot find its user-data directory. See combat-probe.md §10 for the run-directory recipe.
#
# to watch a whole replay that the build no longer matches, past the first CRC checkpoint:
CNC_CRC_DIAG=/dev/null CNC_CRC_DIAG_CONTINUE=1 ./zh -headless -replay <file>.rep
```

A replay recorded on **Windows** would be the direct test — pre-fix it should mismatch as soon as
anything dies, post-fix it should not. `GeneralsReplays/` is empty in this checkout, so that was not
run; `Replay Check GeneralsMD` does it on Windows, which is the gate this branch is held to.
