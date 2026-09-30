# The immortal Angry Mob: a spawn that leaves without dying

A GLA Angry Mob is an invisible nexus (`GLAInfantryAngryMobNexus`, `ImmortalBody`,
`InvulnerableAllArmor`) plus up to 10 members. Its `SpawnBehavior` has `AggregateHealth = Yes`,
so the nexus can't take damage. It leaves the world only when `m_spawnCount` reaches 0
(`SpawnBehavior::onSpawnDeath`, `SpawnBehavior.cpp:765`), and it respawns a member 30 s after each
one is lost (`SpawnReplaceDelay = 30000`).

The count goes down only in `onSpawnDeath`. Before this fix, the only caller was `Object::onDie`,
so a member that is **destroyed without dying** stays counted forever. The nexus then can't reach
0, keeps respawning members, and survives even `Player::killPlayer`: killing everything on the
team kills the members, but the stale member keeps the count at 1.

This is original game logic, not a port defect, and retail Zero Hour 1.04 has the same code path.

## The path that triggered it

`PhysicsBehavior::onCollide` (`PhysicsUpdate.cpp:1208`): infantry that touches a
`DISABLED_UNMANNED` vehicle (a vehicle whose driver was sniped) becomes its crew. The vehicle
defects to the infantry's team and the infantry is removed with `TheGameLogic->destroyObject`,
not killed.

## Measured

A human-recorded skirmish (Defcon 6, 1 human vs 5 Hard AI), replayed headless under LLDB with
`scripts/macos-mob-nexus-probe.py`. The probe logs every `onSpawnDeath` on a mob nexus, with the
count and the number of listed member ids that still resolve to an object.

| | nexuses | nexuses whose count ran ahead of live members | nexus 18629 |
|---|---:|---:|---|
| `main` `2ce4fd7e7` | 11 | **1** | 275 member deaths, 272 with count > live; removed only at teardown (frame 85819) |
| this branch | 9 | **0** | 10 member deaths, counted 10→1; removed at frame 34450 when its last member died |

On `main`, member 18635 crewed an unmanned vehicle at frame 34423 (the stack is
`PartitionManager::update → Object::onCollide → PhysicsBehavior::onCollide → GameLogic::destroyObject`)
and never reached `onSpawnDeath`. Every later "last member" death left the count at 1.

The unfixed build replays this recording with no CRC mismatch. The fixed build first mismatches at
frame 34500, the first checkpoint after the vehicle-crewing event, which is expected: from there the
nexus is gone and the recording assumed it wasn't.

## The fix

`Object::onDestroy` (both games), before the modules' `onDelete`: if the object is not effectively
dead and its producer has a `SpawnBehaviorInterface`, call `onSpawnDeath` with an empty
`DamageInfo` (no killer to score). The fix is not gated by `RETAIL_COMPATIBLE_CRC`, by decision. Gated,
it would be inert in every build, because that macro is 1 everywhere.

Why it doesn't double count or misfire:

- A member that died went through `Object::onDie` → `onSpawnDeath`, and is effectively dead by the
  time it is destroyed, so it is skipped. Even if it weren't, `onSpawnDeath` returns early for an id
  no longer in `m_spawnIDs`.
- `GameLogic::destroyAllObjectsImmediate` marks every object effectively dead before destroying
  any, so teardown never calls in.
- A nexus being destroyed destroys its live members from `SpawnBehavior::onDelete`. Their
  `onSpawnDeath` erases an element the loop has already passed (safe for `std::list`), and a count
  of 0 re-enters `destroyObject` on the nexus, which returns at its `isDestroyed()` check.
- Objects leave the lookup table only when freed (`processDestroyList`), so the spawner found by id
  is never a freed pointer.

## Compatibility

Any replay where a live spawn is destroyed without dying (a mob member crewing a vehicle, and any
other such path) diverges from retail at that point. `Replay Check GeneralsMD` measures whether a
checked-in replay contains one.

## Reproducing

```sh
# -g build; the binary has to run from the run dir (launched from the build dir it exits 1)
CFLAGS=-g CXXFLAGS=-g arch -arm64 python3 scripts/native-build.py --level 1 --level 2 --level 3 \
    --level 4 --with-shims --strict-link --jobs 8 --build-dir build/native-macos-arm64-g
cp build/native-macos-arm64-g/native_strict_link ~/devin-work/run0911/run/zh-g
python3 scripts/macos-mob-nexus-probe.py --binary ~/devin-work/run0911/run/zh-g \
    --run-dir ~/devin-work/run0911/run --replay <name>.rep --out mob-nexus.json
```

The replay itself (7.2 MB) is not checked in.
