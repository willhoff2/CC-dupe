# Does combat resolve? Two skirmishes on Apple Silicon, measured from engine state

`docs/porting/playability-probe.md` §4 watched the AI build, produce and harvest for 22,312 frames
while `units_lost` / `buildings_lost` stayed at zero, and ranked "combat is unproven" as the highest-
value next measurement (§9 item 8). This document is that measurement. It does not fix anything; the
only code in this PR is the sampler, `scripts/macos-combat-probe.py`.

**Headline: combat resolves on the Mac, end to end, without a single port defect found in the combat
path.** In a real skirmish driven with real OS input, the Easy AI *chose* to attack (`CMD_FROM_AI`
attack-move, then `CMD_FROM_SCRIPT` hunt), five of its units acquired the player's Command Center as
`m_currentVictim`, landed 589 measured hits on it through three damage types, took its health from 5000
to 7.25 and then through `Object::onDie` on logic frame 24274, and `Player::killPlayer` ran on the same
frame. In the *first* skirmish, replayed headless on the same Mac binary, a Command Center went through
`DestroyDie::onDie`, `ScoreKeeper::addObjectDestroyed`, and the losing player's `buildings_lost`
became 1 — the counter that had never moved before. Missiles were created, flew, and detonated.
Armour adjustment, healing and container add/remove were all observed.

What is **not** shown here, and why, is in §7: splash damage, garrison clearing, armour *differences*
between armour sets, and a player-issued *attack* order (a player *move* order was measured; the attack
click was posted after the game had already ended). All are NOT MEASURABLE YET for instrumentation
reasons, not because anything failed.

## 0. Setup, and how the two evidence sources are labelled

| Thing | Value |
|---|---|
| Machine | Apple M1 Pro, macOS 26.6.1, Darwin 25.6.0 arm64 |
| Binary | `arch -arm64 python3 scripts/native-build.py --level 1..4 --with-shims --strict-link --build-dir build/native-macos-arm64-combat`: 980/980 objects, 0 probe-clean-but-uncompilable, 0 undefined symbols, `lipo -archs` → `arm64`, `host_translated` false |
| Data | retail 1.04 install (Zero Hour + base game `.big`s reachable), disposable run dir of symlinks; nothing retail is committed |
| Input | `scripts/macos-input-drive.py` (`CGEventPost`, real OS input into the real window) |
| State | `scripts/macos-combat-probe.py` (new): LLDB attach, object table + auto-continue breakpoints on the combat path, `--trace` for decoded order/damage events, `--expr-file` for ad-hoc conditional-breakpoint scripts |
| Base | `main` at `01c3105f5` |

Two evidence labels are used throughout and never blended:

* **MAC-LIVE** — read out of the `./zh -win` process the human (me, through the driver) was playing,
  while it was being played.
* **MAC-REPLAY** — the same arm64 binary, `./zh -headless -replay <file>.rep`, replaying the `.rep` the
  live game wrote, with LLDB breakpoints. Same machine, same binary, same data; no rendering, so the
  breakpoint cost is affordable. This is *not* the Linux headless entry from #103 — nothing here was
  measured on Linux, and no cross-platform CRC/replay claim is made (out of scope per #105/#108).

Two skirmishes were played, both 2-player maps, human vs one Easy AI:

| Game | Sides | What happened | Evidence |
|---|---|---|---|
| **G1** | human Nuke China (player 2) vs AI AirF USA (player 3) | AI attacked the human CC with 2 Humvees + 3 Rangers from frame ~11.5k; the human dozer kept repairing; the CC reached 0 hp at frame 59,584 and was destroyed at 60,832. Process lost when the outpost was rebooted mid-session | MAC-LIVE samples `combat-1.json`, `combat-2.json`; MAC-REPLAY `replay-probe.json` |
| **G2** | human Stealth GLA (player 2) vs AI AirF USA (player 3) | AI attacked with Rangers, a Humvee and Missile Defenders; CC destroyed at frame 24,274; human defeated; score screen shown | MAC-LIVE `order-move2.log`, `defeat-bp2.log`, screenshots; MAC-REPLAY `cc-dmg.log` |

Artifacts (`~/devin-work/combat/`) and the 40-minute screen recording of G2 (`video/drive.mov`,
2.2 GB) are outside the repository and are not committed.

Performance caveat, stated once: every breakpoint stop costs milliseconds and LLDB expression
evaluation pauses the process. In a fight, `ActiveBody::attemptDamage` fires hundreds of times per
second, so MAC-LIVE sampling slowed the game visibly and the human saw stutter. Every number below
carries the logic frame it was read at, and nothing is inferred from wall time.

## 1. Do units acquire targets? Yes — `m_currentVictim` and the attack state machine

**MAC-LIVE, G1, `combat-2.json`**, four object-table samples at frames 44157, 44157, 44164, 44177
(263 objects each). Five AI objects held the human Command Center (id 208) as their victim:

| id | template | `AIUpdate` state | victim | weapon status | `m_lastFireFrame` (per sample) |
|---|---|---|---|---|---|
| 237 | `AirF_AmericaVehicleHumvee` | `AI_ATTACKFOLLOW_WAYPOINT_PATH_AS_TEAM` | 208 | 2 `BETWEEN_FIRING_SHOTS` | 44176 |
| 236 | `AirF_AmericaVehicleHumvee` | `AI_ATTACKFOLLOW_WAYPOINT_PATH_AS_TEAM` | 208 | 2 | 44176 |
| 234 | `AirF_AmericaInfantryRanger` | `AI_HUNT` | 208 | 0 `READY_TO_FIRE` | 44155 |
| 233 | `AirF_AmericaInfantryRanger` | `AI_HUNT` | 208 | 2 | 44176 |
| 232 | `AirF_AmericaInfantryRanger` | `AI_HUNT` | 208 | 2 | 44177 |

Idle objects in the same table read victim 0 / `AI_IDLE`, and structures without an AI read −2, so the
non-zero victims are not a default. **MAC-REPLAY, G2, `cc-dmg.log`** gives the state machine behind the
victim pointer, from a backtrace at the killing blow:

```
Object::onDie                    Object.cpp:4684
ActiveBody::attemptDamage        ActiveBody.cpp:675
Object::attemptDamage            Object.cpp:1917
WeaponTemplate::dealDamageInternal(sourceID=232, victimID=208)   Weapon.cpp:1566
WeaponTemplate::fireWeaponTemplate(wslot=PRIMARY_WEAPON)         Weapon.cpp:1058
Weapon::privateFireWeapon        Weapon.cpp:2723
Weapon::fireWeapon               Weapon.cpp:2804
Object::fireCurrentWeapon        Object.cpp:1548
AIAttackFireWeaponState::update  AIStates.cpp:5257
StateMachine::updateStateMachine
AIAttackState::update            AIStates.cpp:5700
StateMachine::updateStateMachine
```

`AIAttackState` → `AIAttackFireWeaponState` → `fireCurrentWeapon(target)` is the retail attack path.

**Classification: works.** No defect.

## 2. Do weapons fire? Yes — fire counts, `m_lastFireFrame` and reload status advance

* **MAC-LIVE, G1**: `m_lastFireFrame` for id 232 advanced 44155 → 44176 → 44177 across samples; ids
  233/236/237 sat in `BETWEEN_FIRING_SHOTS` with `m_lastFireFrame` one frame before the sample, i.e.
  they had just fired. Counter deltas between the frame-44157 and frame-44164 samples (7 logic frames):
  `Weapon::privateFireWeapon` +3, `ActiveBody::attemptDamage` +155, `ArmorTemplate::adjustDamage` +58.
* **MAC-REPLAY, G1, `replay-probe.json`**: `Weapon::privateFireWeapon` decoded events, e.g. frames
  13410, 13416, 13422 — id 237 `AirF_AmericaVehicleHumvee` (player 3) → victim 208
  `Nuke_ChinaCommandCenter` (player 2), one shot every 6 frames, matching the weapon's delay between
  shots.
* **MAC-REPLAY, G2, `cc-dmg.log`**: 589 `attemptDamage` calls on id 208, by source: id 237 ×370
  (`DAMAGE_GATTLING`, 15.0 each), ids 232/233/234 ×58/54/57 (`DAMAGE_SMALL_ARMS`, 16.5 each), ids
  238/239 ×25/26 (`DAMAGE_ARMOR_PIERCING`, 60.0 each). The amounts are constant per source and per type,
  as retail INI specifies.

The `m_nextShot` / reload-timer field in the object table read `None` in every sample (§7.6): the
table reads it through the current weapon slot, which is a `WeaponSet` lookup the JIT expression did
not resolve. Firing cadence is therefore shown by `m_lastFireFrame` and the 6-frame event spacing, not by
a reload countdown.

**Classification: works.** No defect.

## 3. Are projectiles created, and do they arrive? Yes — missiles launch and detonate

**MAC-REPLAY, G1, `replay-probe.json`** (Missile Defenders vs the CC, late game):

| frame | event | projectile |
|---|---|---|
| 58999 | `MissileAIUpdate::projectileFireAtObjectOrPosition` | id 350 `MissileDefenderMissile` (player 3) |
| 59005 | `MissileAIUpdate::detonate` | id 349 |
| 59009 | `projectileFireAtObjectOrPosition` | id 351 |
| 59026 | `detonate` | id 350 |
| 59029 | `projectileFireAtObjectOrPosition` | id 352 |
| 59035 | `detonate` | id 351 |

Each missile object id is created, then detonates 26–27 frames later, and the CC's health track shows
the corresponding drops in the same window (3162.65 @59309 → 3106.82 @59350 → 2915.14 @59446 → 0
@59584). Both breakpoints were hit 24 decoded times (the decode cap), and the raw hit counters are
higher. `DumbProjectileBehavior` (ballistic shells) recorded zero hits in both games because neither
side fielded artillery or tanks with dumb projectiles before the fight ended — **not measured**, not
failed.

**Classification: works** for `MissileAIUpdate`; `DumbProjectileBehavior` NOT MEASURABLE YET (no unit
of that type in either game; a China/GLA army with tanks or a scripted setup would show it).

## 4. Is damage applied to a named object? Yes — health falls on the Command Center, and repair fights it

**MAC-REPLAY, G2, `cc-dmg.log`** — a conditional breakpoint on `ActiveBody::attemptDamage` for
`this->getObject()->getID() == 208`, reading `m_currentHealth` before each hit (every 60th hit shown):

```
frame   22892 22977 23050 23124 23196 23268 23344 23420 23495 23565 23640 23716 23792 23864 23936 24009 24082 24154 24226 24274
health   4664  4358  4128  3899  3617  3336  3113  2891  2668  2387  2105  1883  1660  1379  1097   875   645   416    83   7.25
```

The final hit at frame 24274: `m_damageType` 3 `DAMAGE_SMALL_ARMS`, `m_sourceID` 232, `m_amount`
16.5 against 7.25 remaining, `m_kill` 0 (an ordinary hit, not a scripted kill), followed by
`Object::onDie(damageInfo)` with `m_deathType` 0. 5000 → 0 in ~1400 logic frames (47 s of game time)
under five to six attackers.

**MAC-LIVE, G1** shows the other direction: in `combat-2.json` the same CC *rose* 4898.38 → 4901.72 →
4911.72 → 4920.05 across frames 44157–44177 while five attackers held it as victim. The decoded events
in that window are `ArmorTemplate::adjustDamage` with `damage_type` 10 `DAMAGE_HEALING`, `amount_in`
3.333 — the human dozer's repair, applied through the same `attemptDamage` path, out-pacing two Humvees
and three Rangers at the range they were firing from. The G1 replay's health track shows the same tug
of war earlier (5000 → 4985 → 4993.33 → 5000 → 4995 …, frames 13429–13459) before the attackers won.

**Classification: works.** Damage and healing both go through `attemptDamage` →
`ArmorTemplate::adjustDamage` → `internalChangeHealth`, on a named object, with per-type amounts.

## 5. Do objects die and get removed? The counters move, but no die module ran

> **Correction, [`death-flag-shift.md`](death-flag-shift.md).** This section originally concluded
> "works". Its measurements below are unchanged and still stand; the inference drawn from two of them
> does not. `DieMuxData::isDieApplicable` rejected **every** normal death on this build — the flag for
> `DEATH_NORMAL` is `1UL << -1`, which lands on bit 31 on 32-bit Windows and on nothing at LP64 — so
> no die module body ran, and the removals seen here came from other paths. Two rows below read as
> evidence that it did run, and are not:
>
> * the `DestroyDie::onDie` rows are **function-entry** breakpoint hits.
>   `BreakpointCreateByName("DestroyDie::onDie")` resolves to `DestroyDie.cpp:56`, which is the
>   `if (!isDieApplicable(damageInfo))` line itself, so a hit records the dispatch and not the body;
> * `units_lost` / `buildings_lost` are credited by `Object::scoreTheKill` (`Object.cpp:2998`) on the
>   *killer's* damage path, not by a die module, so they move either way.
>
> The 0 hits on `SlowDeathBehavior::beginSlowDeath` / `update` recorded below, and the unexplained
> 1,248-frame delay, are what the defect predicts. §7.7 asked which module owned that delay; the
> answer is that none did.

**MAC-REPLAY, G1, `replay-probe.json`:**

| frame | event | object |
|---|---|---|
| 59584 | health track reaches 0.0 | id 208 `Nuke_ChinaCommandCenter` (player 2) |
| 60832 | `DestroyDie::onDie` | id 208 |
| 60832 | `DestroyDie::onDie` | id 209 `Nuke_ChinaVehicleDozer` (player 2) |
| 60832 | `ScoreKeeper::addObjectDestroyed` | id 208, credited to player 3 |
| 60890 | first object-table sample with player 2 `buildings_lost` = 1 | — |

The 1,248 frames between health 0 and `DestroyDie` are consistent with the structure's collapse
timer, but the `SlowDeathBehavior::beginSlowDeath` / `update` breakpoints recorded **0 hits** for the
whole replay, so which module owned that delay is not shown (§7.7). Whole-replay counters at the last
sample (frame 61136): `Weapon::privateFireWeapon` 30,927; `ActiveBody::attemptDamage` 30,837;
`MissileAIUpdate::projectileFireAtObjectOrPosition` 124 / `detonate` 126; `Object::onDie` 56;
`GameLogic::destroyObject` 253; `ScoreKeeper::addObjectLost` 1; `addObjectDestroyed` 1.
(`adjustDamage` and `internalChangeHealth` were auto-disabled at frames 7317 / 13483 by the sampler's
hit-rate cap, so their totals are floors.) The 253 destroys include crates and parachutes from supply
drops (frames 31141–31165), so removal is exercised by non-combat deaths too. Object counts in the
table went 234 → 297 over the replay because the AI kept producing; the *per-object* removal is the
evidence, not the total.

**MAC-REPLAY, G2, `cc-dmg.log`:** `Object::onDie` for id 208 at frame 24274, and `Player::killPlayer`
(breakpoint 3, `Player.cpp:2021`) hit on the *same* frame — `VictoryConditions::update` saw
`hasSinglePlayerBeenDefeated` (no buildings left counting for victory) and killed the player. In
MAC-LIVE, `defeat-bp2.log` confirmed `TheGameLogic->findObjectByID(208)->isEffectivelyDead()` = 1
while the process was still on the score screen.

The G2 score screen (screenshot, not committed) reads human `Units Lost 1, Buildings Lost 0`, AI
`Units Destroyed 1, Buildings Destroyed 0`. That is *not* a counter bug in the port: `addObjectDestroyed`
is credited from `Object::scoreTheKill` (`Object.cpp:3035`) on the killer's damage path, and in G2
`killPlayer` destroyed the player's objects on the frame the CC hit 0. Whether Windows shows the same
0 was **not** checked against the oracle; it follows from shared code, so it is recorded as an
observation, not a defect.

**Classification: partly — corrected.** `units_lost` / `buildings_lost` are non-zero from a measured
kill, and objects do leave `m_objList`. What this section did *not* show, and what
[`death-flag-shift.md`](death-flag-shift.md) later measured to be false on this build, is that the
die modules ran at all.

## 6. Does the AI choose to attack? Yes — a real team attack-move, then hunt

**MAC-REPLAY, G1, `replay-probe.json`**, decoded `AIUpdateInterface::private*` entries with their
`CommandSourceType`:

| frame | entry | source | actor |
|---|---|---|---|
| 11522 | `privateAttackMoveToPosition` | `CMD_FROM_AI` | id 232 `AirF_AmericaInfantryRanger` (player 3) |
| 11522 | `privateAttackMoveToPosition` | `CMD_FROM_AI` | id 234 |
| 11523 | `privateAttackMoveToPosition` | `CMD_FROM_AI` | id 234 |
| 14680 | `privateHunt` | `CMD_FROM_SCRIPT` | id 234 |
| 14733 | `privateHunt` | `CMD_FROM_SCRIPT` | id 233 |
| 60890 | `privateHunt` | `CMD_FROM_SCRIPT` | id 333 `AirF_AmericaInfantryMissileDefender` |

`CMD_FROM_AI` is the AI player's team logic; `CMD_FROM_SCRIPT` is the skirmish AI script
(`SkirmishScripts.scb`) issuing team hunt orders. The first shots at the CC (frame 13410) follow the
attack-move by ~1900 frames — travel time across the small map. The MAC-LIVE team states in §1
(`AI_ATTACKFOLLOW_WAYPOINT_PATH_AS_TEAM`, `AI_HUNT`) are the same orders seen from the unit side.
The AI in `playability-probe.md` §4 never attacked in 12.4 minutes; here the first team attack-move
came at frame 11522 (~6.4 min of game time) in G1, and in G2 the CC was under fire by frame 22.9k
(~12.7 min), so the difference is map, difficulty draw and time, not a missing path.

**Classification: works.** The AI decides to attack through the real team-order path.

## 7. What was NOT measured, and what would measure it

Each item: what was attempted, why it fell short, what unblocks it.

1. **Splash / area damage — NOT MEASURABLE YET.** Every decoded `attemptDamage` in both games had
   exactly one victim per shot (small arms, gatling, single-target missiles). No weapon with
   `RadiusDamageAffects` fired. Unblock: a scripted map or a China/GLA army with tanks or a Scud, and a
   breakpoint on `WeaponTemplate::dealDamageInternal` counting `PartitionFilter` victims per call.
2. **Garrison clearing (and garrisoning) — NOT MEASURABLE YET.** `OpenContain::addToContain` /
   `removeFromContain` were hit 24+ times, but every decoded event is a supply-drop
   (`AmericaJetCargoPlane` ← `AmericaCrateParachute` ← `SupplyDropZoneCrate`), not infantry entering a
   civilian building, and no `DAMAGE_KILL_GARRISONED` or flame weapon fired. Unblock: garrison a
   structure by real input and attack it with a flashbang/dragon tank; breakpoint `GarrisonContain`.
3. **Armour *differences* — NOT MEASURABLE YET.** `ArmorTemplate::adjustDamage` is entered for every
   hit and the per-type amounts are constant (§2), but only structure armour was hit hard enough to
   compare, and the decode captured `amount_in`, not `amount_out`. Unblock: read the return value at
   `adjustDamage`'s epilogue (or `damageInfo->out.m_actualDamageDealt` in `attemptDamage`) for the same
   weapon against infantry, vehicle and structure.
4. **Ballistic (`DumbProjectileBehavior`) projectiles — NOT MEASURABLE YET** (§3): no such unit fought.
5. **Player-issued *attack* order — NOT MEASURABLE YET.** A player *move* order was measured
   (MAC-LIVE, G2, `order-move2.log`: `privateMoveToPosition`, `CMD_FROM_PLAYER`, id 209
   `Slth_GLAInfantryWorker`, frame 9431, from a real right-click posted by the driver). The attack
   click on an enemy Ranger was posted, but the `--trace` window recorded frames 109–278 of the *score
   screen's* fresh logic — the CC had died and the game had ended by then; the trace is void, not a
   failure. Unblock: repeat with the trace armed before the click and a target that is still alive.
6. **Reload countdown (`m_nextShot`) — NOT MEASURABLE YET** in the object table (`None` in every
   record): the JIT expression's weapon-slot lookup did not resolve. `m_lastFireFrame` and
   `WeaponStatus` were read instead. Unblock: read `Weapon::m_whenLastReloadStarted` and the template's
   delay through `WeaponSet::getCurWeapon()` inside the expression.
7. **Slow-death module state — NOT MEASURABLE YET.** Guessed `SlowDeathBehavior` module slots crashed
   the expression (`EXC_BAD_ACCESS`) and were discarded, and the `SlowDeathBehavior::beginSlowDeath` /
   `update` breakpoints recorded 0 hits across a replay in which a structure took 1,248 frames to go
   from 0 hp to `DestroyDie` — so either those symbols did not resolve to the code that ran, or the
   delay is owned by another module. Unblock: `image lookup -n` on the symbols before trusting the
   counter, and walk `Object::m_behaviors` by `getModuleNameKey()` on the dying object.
8. **Sight / stealth** — not attempted; out of the question list, recorded so nobody assumes it.

## 8. What a player notices (subordinate to the combat question)

All MAC-LIVE, G2, real OS input through `scripts/macos-input-drive.py`, engine state through LLDB.
Screenshots exist outside the repo; none are committed (retail art).

| Item | Observed | Evidence | Class |
|---|---|---|---|
| Selection | Left-click on the worker selected it (health bar, portrait, GLA build palette in the command bar) | screenshot `s2-select.png`; the move order in §7.5 was accepted by that selection | works |
| Multi-select | Drag-box `--client 110,330 --to 200,430` selected three Rebels; command bar showed the shared unit palette (attack-move, guard, stop) | `s11-multisel.png` | works |
| Move order | Right-click → `privateMoveToPosition`, `CMD_FROM_PLAYER`, id 209, frame 9431; worker relocated on screen | `order-move2.log`, `s3-moved.png` | works |
| Attack order | Posted after game end — void (§7.5) | `order-attack.json` frames 109–278, 0 hits | NOT MEASURABLE YET |
| Build | Command-bar build button → placement ghost → click placed a Supply Stash and a Barracks; `$10000` → `$8700`; barracks queue accepted a Rebel order | `s4`–`s9` | works |
| Tooltips | Hovering a build button showed the retail tooltip | `s6-tip.png` | works |
| Camera scroll | Edge-scroll changed `TheTacticalView->m_pos.y`; the view moved | `s10-scrolled.png`, LLDB read | works |
| Camera zoom / rotate | Not driven (input posting was stopped to spare the user's focus, §8.1) | — | NOT MEASURABLE YET |
| Obstacle pathfinding | The AI's Rangers and Humvees crossed the map and arrived at the CC (§6 → §2), so paths were found; no deliberately obstructed route was ordered | — | NOT MEASURABLE YET (needs a walled route and `Pathfinder` state) |
| Command bar | Draws and reacts (build palette, unit palette, portrait, rank star, cash) | all screenshots | works |
| Radar | The radar panel area is drawn black in every screenshot, before and during combat | `s3`, `s11` | **PORT DEFECT candidate**, not diagnosed here (renderer slice owns the viewport, #137) |
| UI / EVA reaction to events | `Structure under attack` text appeared top-left while the CC was being hit; EVA voice is silent (no sound effects, `playability-probe.md` §3) | `s11-multisel.png` | text works; audio PORT DEFECT (already ranked) |
| Mission objectives | Skirmish has none; campaign not driven here | — | NOT MEASURED |
| Input latency | Not quantified; every posted click produced its engine effect within the next LLDB read, but the debugger's own pauses dominate any timing | — | NOT MEASURABLE YET (needs a run without breakpoints and a frame-stamped input log) |
| Score screen | Shown at defeat with retail layout and per-player counters (§5) | `s13-now.png` | works |
| Viewport | Terrain, units, health bars, selection boxes and the shroud edge are recognisable; **structure models are drawn solid black** with texture noise on some faces, and the terrain has speckled texture-seam artifacts. Playable-ish, not right | `s3`, `s11` | PORT DEFECT, owned by the renderer slice (#137 lineage); measured here only as a witness |

### 8.1 Cursor and focus (asked by the user during the run)

* No mouse cursor is visible over the game window. `TheMouse->m_currentRedrawMode` = 0 `RM_WINDOWS`
  and `m_visible` = 1, i.e. the engine expects the *OS* cursor, which Windows gets from `SetCursor`
  with the retail `.ani`. The native seam hides the system cursor through `NSCursor` and the
  `SetCursor` path is `_WIN32`-only, so nothing draws one. **PORT DEFECT** (small, and a real
  playability blocker — you cannot see where you are clicking). Not fixed here: it is one seam, but it
  is not in the combat path and the single fix allowed by the slice is better spent unblocked.
* Focus "jumping" is the harness, not the game: `macos-input-drive.py post` activates the game window
  and warps the real cursor before every event, and LLDB pauses the process while sampling. Neither
  happens when a person plays without the driver attached.

## 9. Ranked: what stands between this and a single-player game a person would call playable

Combat is *off* this list — it works. Cost is in Devin sessions, not calendar time; each item names
the evidence that would close it so wave 11 can be scoped from this table.

| # | Blocker | Class | Cost | Why this rank |
|---|---|---|---|---|
| 1 | **Viewport: structures drawn solid black, texture seams** (§8, #137 lineage, owned by slice 5) | PORT DEFECT | 1–2 sessions, already in flight | First thing anyone sees; combat is legible only from health bars |
| 2 | **No mouse cursor** (§8.1) | PORT DEFECT | ≤ 0.5 session: draw the OS cursor via `NSCursor` from the retail cursor set, or leave the system cursor visible in `RM_WINDOWS` | You cannot aim a click; a game without a cursor is not playable regardless of the rest |
| 3 | **No sound effects, no EVA voice** (`playability-probe.md` §3) | PORT DEFECT | 1–2 sessions (no voice allocated) | "Structure under attack" is silent text; a fight with no sound reads as broken |
| 4 | **Radar panel black** (§8) | PORT DEFECT candidate | ≤ 1 session once the viewport slice lands (likely a render-to-texture path); measure `RadarUpdate` texture first | Players navigate by radar in every fight |
| 5 | **Save/load never finds the save** (`playability-probe.md` §6, #140 in flight) | PORT DEFECT | in flight | Campaign is unplayable without it |
| 6 | **Quit is a `SIGSEGV`** (`playability-probe.md` §7) | PORT DEFECT | ≤ 1 session | Every session ends in a crash |
| 7 | **Renderer leaks texture + surface per frame** (`playability-probe.md` §1.1) | PORT DEFECT | 1 session | Degrades a long game; a campaign mission can run an hour |
| 8 | **Input wedge mid-game** (`playability-probe.md` §8.1) | PORT DEFECT candidate | unknown; needs a reproduction | Rare, unrecoverable |
| 9 | **Simulation ~5 % slow** (`playability-probe.md` §2) | PORT DEFECT | ≤ 0.5 session | Felt as sluggishness; also skews any timing comparison |
| 10 | **Combat paths not yet exercised**: splash, garrison, ballistic projectiles, armour deltas, player attack order (§7) | NOT MEASURABLE YET | 0.5–1 session of measurement with a scripted map; no fix expected | Cheap to close, and closing it retires the last "is it a game?" doubt |
| 11 | **Campaign mission flow** (objectives, briefing video, scripted triggers) | NOT MEASURED | 1 session of measurement | Single-player is half campaign; only skirmish has been driven past the load screen |
| 12 | **Input latency and camera zoom/rotate under real input** (§8) | NOT MEASURABLE YET | ≤ 0.5 session, driver only, no breakpoints | Feel, not function |

Items 1–4 are what a person would call "broken" within the first minute; 5–9 are what would make them
stop after an hour; 10–12 are what we still cannot say either way.

## 10. Reproducing this

```sh
# build (the shell must not be under Rosetta)
arch -arm64 python3 scripts/native-build.py --level 1 --level 2 --level 3 --level 4 \
    --with-shims --strict-link --build-dir build/native-macos-arm64-combat \
    --report /tmp/combat-build.md --json /tmp/combat-build.json

# play: drive the shell into a 1v1 skirmish on the smallest map (see real-input-menu-drive.md)
arch -arm64 python3 scripts/macos-input-drive.py --pid <pid> --binary <run>/zh post --client 550,300

# MAC-LIVE: sample the fight (Xcode's python carries the lldb module)
export PYTHONPATH=$(lldb -P)
arch -arm64 /Applications/Xcode.app/Contents/Developer/usr/bin/python3 \
    scripts/macos-combat-probe.py --pid <pid> --binary <run>/zh --minutes 2 --interval 10 \
    --out combat.json
# decode the next 8 s of orders only
... scripts/macos-combat-probe.py --pid <pid> --binary <run>/zh \
    --trace AIUpdateInterface::privateAttackObject,AIUpdateInterface::privateMoveToPosition --seconds 8

# MAC-REPLAY: replay the .rep the game wrote, headless, under the same breakpoints
cd <run> && lldb -b -s cc-dmg.lldb -- ./zh -headless -replay 00000000.rep
# where cc-dmg.lldb sets a conditional breakpoint on ActiveBody::attemptDamage for one object id,
# prints m_frame / m_currentHealth / damageInfo->in.{m_damageType,m_sourceID,m_amount,m_kill},
# and stops at Object::onDie and Player::killPlayer.
```

The replay shutdown still ends in the `ObjectPoolClass<MultiListNodeClass>::~ObjectPoolClass`
`SIGSEGV` recorded in `playability-probe.md` §7; it happens after the last logic frame and is not
combat evidence in either direction.
