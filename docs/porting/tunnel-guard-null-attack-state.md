# The tunnel guard's missing null check: `AITNGuardOuterState::update()` at 0x38

The GLA tunnel-network guard (`AITNGuard.cpp`) is a copy of the ordinary guard (`AIGuard.cpp`). Its
inner, outer and attack-aggressor states each own an `AIAttackState *m_attackState` that `onEnter()`
creates, and each `onEnter()` returns `STATE_SUCCESS` *before* creating it when there is nothing to
attack:

```cpp
// GeneralsMD/Code/GameEngine/Source/GameLogic/AI/AITNGuard.cpp, AITNGuardOuterState::onEnter()
	if (getGuardMachine()->getGuardMode() == GUARDMODE_GUARD_WITHOUT_PURSUIT)
		return STATE_SUCCESS;                 // "patrol" mode: m_attackState stays null
	...
	if (nemesis == nullptr)
		return STATE_SUCCESS;                 // no nemesis: m_attackState stays null
```

`AIGuard`'s three twin states open `update()` with a null check. The tunnel copies did not:

```cpp
// AIGuard.cpp:575, AIGuardOuterState::update() -- the EA source release, unchanged
	if (m_attackState==nullptr) return STATE_SUCCESS;

// AITNGuard.cpp:505 before this fix, AITNGuardOuterState::update()
	Object* goalObj = m_attackState->getMachineGoalObject();
```

This is an **upstream defect, not a port regression**: `git blame` puts every line involved on
`3d0ee53a0`, the EA source release. Upstream's own refactors since (`nullptr`, `override`, `void`
arguments) have not touched the logic.

## 1. MEASURED: the crash

Reported by the user from their M1 Pro: an 8-player skirmish, fullscreen, on a build of `origin/main`
plus PR #170. `EXC_BAD_ACCESS (SIGSEGV)`, `KERN_INVALID_ADDRESS at 0x0000000000000038`:

```
0 State::getMachineGoalObject() + 20
1 AITNGuardOuterState::update() + 44
2 StateMachine::updateStateMachine() + 204
3 AITunnelNetworkGuardState::update() + 148
4 StateMachine::updateStateMachine() + 204
5 AIStateMachine::updateStateMachine() + 200
6 AIUpdateInterface::update() + 60
7 GameLogic::update() + 1708
```

The `.ips` report itself was not available to this change; the stack above is as relayed.

## 2. MEASURED: what 0x38 is

Disassembled from the arm64 `native_strict_link` this worktree's levels 1-4 build produced (Apple
clang 21, unoptimised, as `scripts/native-build.py` builds it):

```
State::getMachineGoalObject():
  +0x10  ldr  x8, [sp, #0x8]        ; this
  +0x14  ldr  x0, [x8, #0x38]       ; this->m_machine   <- frame 0, "+ 20"
  +0x18  bl   StateMachine::getGoalObject

AITNGuardOuterState::update():
  +0x24  ldr  x0, [x8, #0x50]       ; this->m_attackState
  +0x28  bl   State::getMachineGoalObject
  +0x2c                             ; return address  <- frame 1, "+ 44"
```

`State`'s layout on LP64 is two vtable pointers (`MemoryPoolObject`, `Snapshot`), three 4-byte
`StateID`s padded to 0x20, a 24-byte `std::vector<TransitionInfo>`, then `m_machine` at **0x38**. So
the faulting read is `m_attackState->m_machine` with `m_attackState == nullptr`: line 505 above.

## 3. How a state with no attack state stays current

A state whose `onEnter()` returned `STATE_SUCCESS` should never be updated: `internalSetState()`
hands the result to `State::friend_checkForTransitions()`, which takes the success transition at
once (INNER -> OUTER -> GET_CRATE, `AITNGuard.cpp:163-165`). The exception is in that function's
first lines:

```cpp
// GeneralsMD/Code/GameEngine/Source/Common/StateMachine.cpp:114
	static Int checkfortransitionsnum = 0;
	StIncrementer inc(checkfortransitionsnum);
	if (checkfortransitionsnum >= 20)
	{
		DEBUG_CRASH(("checkfortransitionsnum is > 20"));
		return STATE_FAILURE;          // no transition: the state just entered stays current
	}
```

The counter is a function-static shared by every state machine and counts *nesting*, and each
transition's `onEnter()` runs inside the previous transition's call. When the twentieth nested
transition is refused, the state whose `onEnter()` just ran is left current — with whatever
`onEnter()` left behind, here a null `m_attackState`. Release builds compile the `DEBUG_CRASH` out.

### 3.1 MEASURED: the real machine leaves OUTER current with no attack state

`scripts/native-tunnel-guard-null-attack-state-test.py` links the real `StateMachine` and
`AITNGuard.cpp` from the native build's archives. A harness machine supplies the nesting depth (a
state that succeeds into itself *n* times and then calls the guard machine's `setState(INNER)`), and
the guard machine, with no nemesis, does the rest. Pre-fix tree, `6a593e409`, this machine:

```
-- INNER entered with no nemesis, transitions nested to the cutoff
      depth 18: AI_TN_GUARD_OUTER is left current
      child faulted: signal 11 at address 0x38
-- the same in GUARDMODE_GUARD_WITHOUT_PURSUIT
      depth 18: AI_TN_GUARD_OUTER is left current
      child faulted: signal 11 at address 0x38
-- one level deeper
      depth 19: AI_TN_GUARD_INNER is left current
-- attack-aggressor state with no attack state
      child faulted: signal 11 at address 0x38
```

At depth 18 INNER's transition is the 19th and allowed, OUTER's is the 20th and refused; at depth
19 the refusal moves to INNER's own. The state left current is decided by the depth alone. Updating
OUTER then faults at **0x38**, the measured crash's address.

### 3.2 INFERRED: the route in a game

The depth is reached without any help from the tunnel guard's own loop. With the owner outside a
tunnel and no tunnel left to return to, every state on the success/failure path returns at once from
`onEnter()`:

| state | `onEnter()` returns | because | next |
|---|---|---|---|
| GET_CRATE | `STATE_SUCCESS` | `checkForCrateToPickup()` is null (below) | RETURN |
| RETURN | `STATE_FAILURE` | `findBestTunnel()` is null (`AITNGuard.cpp:598`) | INNER |
| INNER | `STATE_SUCCESS` | no nemesis | OUTER |
| OUTER | `STATE_SUCCESS` | no nemesis | GET_CRATE |

so the four states chain into each other until the counter refuses. Where the loop stops depends
on where it started. One entry point lands it on OUTER exactly: `AITNGuardIdleState::update()` sees
a crate the unit created by a kill (`AIUpdateInterface::getCrateID()`, set by `CreateCrateDie`) and
calls `getMachine()->setState(AI_TN_GUARD_GET_CRATE)` (`AITNGuard.cpp:709`). Counting from there,
GET_CRATE's transition is level 1, and level 4k is OUTER's: level 20 is refused with OUTER current.
That call **ignores** the `STATE_FAILURE` it gets back, and `StateMachine::updateStateMachine()`
then replaces IDLE's sleep result with `STATE_CONTINUE` because the current state changed under it
(`StateMachine.cpp:454`). OUTER has no condition transitions, so it is still current at the next
frame's `AITunnelNetworkGuardState::update()` -> `updateStateMachine()` -> `AITNGuardOuterState::update()`
— frames 1-3 of the measured stack.

The other entry points this reading checked do not survive the refusal: a transition chain started
by `AITunnelNetworkGuardState::onEnter()` or by the guard machine's own update returns the
`STATE_FAILURE` up to `AITunnelNetworkGuardState`, whose machine then fails over to `AI_IDLE`
(`AIStates.cpp:719`) and deletes the guard machine. So IDLE's ignored `setState` is the one route
this reading finds to the measured stack; the search was by hand, not exhaustive.

Two further EA defects keep that route open, both in the source release:

* `AIUpdateInterface::checkForCrateToPickup()` clears `m_crateCreated` and *then* looks it up
  (`AIUpdate.cpp:897-898`), so it always asks `findObjectByID(INVALID_ID)` and always returns null.
  GET_CRATE can therefore never stop the loop.
* Tunnel guard is entered only by script (`ScriptActions::doTeamGuardInTunnelNetwork` is the one
  caller of `aiGuardTunnelNetwork`) — in a skirmish, the AI players' scripts. Consistent with an
  8-player skirmish.

What is **not** measured: no LLDB run of the user's game recorded the transition count, the entry
point, the crate, or that every tunnel of that player was gone. The game binary was deliberately not
launched for this change. The route above is the one the source admits, not the one observed.

Why this appears now is **INFERRED** the same way `particle-emitter-strdup.md` §3 inferred it: the
route needs units to die (a kill to create the crate, a nemesis that is gone, tunnels destroyed), and
until PR #161 ([`death-flag-shift.md`](death-flag-shift.md)) no normal death ran at 64 bits.

### 3.3 A second route: loading a save

`AITNGuardOuterState::loadPostProcess()` is meant to call `onEnter()` and rebuild the attack state,
as the inner and aggressor states' do. A stray token turned the call into a declaration:

```cpp
// AITNGuard.cpp:461-464, unchanged since the source release
void AITNGuardOuterState::loadPostProcess()
{						 AITNGuardOuterState
	onEnter();          // parses as "AITNGuardOuterState onEnter();": declares a local function
}
```

So a save taken with OUTER current loads with `m_attackState` null, and the first update faulted the
same way. Read from source, not run; save/load is not what the reported crash was doing.

## 4. Was the missing check intentional? No

* The **same classes** already treat the pointer as nullable: all three states' `onExit()` check
  `if (m_attackState)` before using it, and the destructors call `deleteInstance`, which accepts null.
* The **twin classes** check it, in the source release: `AIGuardInnerState::update()` (`if
  (m_attackState) ... return STATE_SUCCESS;`), `AIGuardOuterState::update()` and
  `AIGuardAttackAggressorState::update()` (`if (m_attackState==nullptr) return STATE_SUCCESS;`), and
  all three `AIGuardRetaliate*` states likewise. `AITNGuard` was derived from that code (its
  `jba [8/24/2003]` additions sit on top of the shared structure); the line was lost in the copy.
* The unchecked path cannot have been behaviour anything relied on: it reads through a null pointer.
  On 32-bit Windows `m_machine` sits at a smaller offset but still inside the first 64 KiB, which
  Windows never maps, so retail takes an access violation on the same path (INFERRED — not run on
  Windows). There is no retail outcome to preserve.
* Reaching it at all requires the transition cutoff, which EA wrote as a `DEBUG_CRASH`: their debug
  builds stopped there. They regarded the route itself as a bug.

## 5. The fix

`if (m_attackState == nullptr) return STATE_SUCCESS;` — `AIGuard`'s line and `AIGuard`'s return
value — in both game trees, placed so that **only executions that previously dereferenced null
change**:

| state | where | why there |
|---|---|---|
| `AITNGuardOuterState::update()` | first statement | the first thing it did was the dereference |
| `AITNGuardAttackAggressorState::update()` | first statement | likewise (`m_attackState->getMachine()`) |
| `AITNGuardInnerState::update()` | in the no-nemesis branch, after the team-victim and tunnel-nemesis early returns, before `m_scanForEnemy` | both early returns are `STATE_CONTINUE` without touching the attack state, and a condition transition (attack-aggressor) can take the unit out of INNER before the next update — a path retail survives, so it is left exactly as it was. Everything after this point dereferenced it |
| `AITNGuardInnerState::update()` | before the final `return m_attackState->update()` | the nemesis-present branch reaches it |

`STATE_SUCCESS` takes the state's ordinary success transition (OUTER -> GET_CRATE, INNER -> OUTER,
AGGRESSOR -> RETURN), the same as `onEnter()`'s own "nothing to attack" answer.

## 6. Replays and Windows

This is simulation code, so the Windows VC6 build and the `Replay Check GeneralsMD` gate are the
oracle. The argument that replays are unchanged: every execution this diff alters is one that, before
it, read through a null pointer (§5's placement). On Windows that is an access violation that ends
the process (§4), so no retail replay that completes can contain one, and every replay that
completes takes exactly the code it took before. **INFERRED**; neither the Wine/VC6 build nor the
replay check was run for this change — see the table below.

## 7. MEASURED: the gate

`scripts/native-tunnel-guard-null-attack-state-test.py`, in CI next to the other archive-linked
harnesses and listed in [`.agents/skills/native-port-measure/SKILL.md`](../../.agents/skills/native-port-measure/SKILL.md).

* **Compiled half** (`GeneralsMD/Code/GameEngine/Source/GameLogic/AI/tests/tunnel_guard_null_attack_state_test.cpp`):
  §3.1's cases, each in a forked child that reports a fault's address. Before the fix: 3 failed
  checks, each fault at 0x38. After: 0 failed.
* **Source scan** over both trees' `AITNGuard.cpp`: every `m_attackState->` in the three `update()`
  bodies must be dominated by the guard in an enclosing block. Before the fix it reports 10 sites per
  tree (20); after, none. It is what covers `AITNGuardInnerState::update()`, which reads the owner's
  team and controlling player before its attack state and so cannot run without a constructed
  `Object` — and constructing one needs a `ThingTemplate`, the module factory and the partition
  manager, i.e. most of `GameEngine::init()`. The scan was also checked to fail when only the
  inner state's final guard is removed.

| claim | status |
|---|---|
| the crash, its stack and its address | **MEASURED** by the user, §1 |
| 0x38 is `State::m_machine` read through a null `m_attackState` | **MEASURED**, disassembly, §2 |
| the real state machine leaves OUTER current with a null attack state at the cutoff, and updating it faulted at 0x38 | **MEASURED**, §3.1 |
| the in-game route is IDLE's crate `setState` into the RETURN/INNER/OUTER/GET_CRATE loop | **INFERRED** from source, §3.2 |
| PR #161 is why it surfaced now | **INFERRED**, §3.2 |
| the save/load route | **INFERRED** from source, §3.3 |
| the fix is in the game binary and it links | **MEASURED** — `scripts/native-build.py --level 1..4 --with-shims --strict-link`, Apple clang 21: 981/981 objects, 0 unresolved, 27.1 MiB arm64 |
| the original crash no longer happens in a real game | **OWED** — the game was not launched for this change |
| the base game's copy is fixed too | **MEASURED** by the source scan; not compiled by `native-build.py`, which builds only `Core/` and `GeneralsMD/` |
| Windows behaviour and replays are unchanged | **INFERRED**, §6. Not built: `scripts/docker-build.sh` on this M1 Pro aborts in `wineboot` before compiling (`anon_mmap_fixed: Assertion ... host_page_mask` under qemu — Wine on a 16 KiB-page host), so the Wine/VC6 build needs an x86-64 box. `Replay Check GeneralsMD`: not run |

## 8. The same shape elsewhere, left alone

Swept: every `m_*State->` dereference in `GeneralsMD/Code/GameEngine/Source`.

| site | state |
|---|---|
| `AIGuard.cpp` inner/outer/aggressor `update()` | already guarded (source release) |
| `AIGuardRetaliate.cpp` inner/outer/aggressor `update()` | already guarded (source release) |
| `AIStates.cpp:863`, `m_temporaryState->update()` | inside `if (m_temporaryState)` |
| `AITNGuardInnerState::update()`, nemesis-present branch: `tunnels->updateNemesis(nemesis)` | **not fixed.** `tunnels` is null-checked everywhere else in the function, not here; reachable only for a player with no tunnel system. Same file, different pointer, not on the measured stack |
| `AITNGuardInnerState::m_scanForEnemy` | **not fixed.** Never initialised and never set true anywhere, so the one-shot scan's behaviour depends on whatever the pool slot held. Changing it would change which units scan, i.e. replays |
| `AIUpdateInterface::checkForCrateToPickup()` clears before it looks up (§3.2) | **not fixed.** Affects `AIGuard` too; fixing it makes guards pick up crates they never did, which changes replays |
| `AITNGuardOuterState::loadPostProcess()` stray token (§3.3) | **not fixed.** With this fix a loaded OUTER state no longer faults; restoring the call would rebuild the attack state on load, a save/load behaviour change of its own |
