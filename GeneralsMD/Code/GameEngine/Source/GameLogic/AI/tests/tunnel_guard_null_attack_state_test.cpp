/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 TheSuperHackers
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// Leave a tunnel-network guard state current without its attack state, the way the real state
// machine does, and update it.
//
// AITNGuardOuterState::onEnter() returns STATE_SUCCESS before it allocates m_attackState when there
// is no nemesis or the guard mode is GUARDMODE_GUARD_WITHOUT_PURSUIT. The success transition
// (OUTER -> GET_CRATE) normally moves on at once -- but State::friend_checkForTransitions() refuses
// to transition once 20 calls of it are nested (a function-static counter shared by every machine)
// and returns STATE_FAILURE with the state it was asked about still current. The tunnel guard's
// RETURN -> INNER -> OUTER -> GET_CRATE -> RETURN loop closes when the owner has no tunnel to return
// to, so it nests until that cutoff. The next update() of a state left current that way read
// m_attackState->m_machine at null + 0x38: the measured crash.
//
// Real code throughout: StateMachine, AITNGuardMachine and its states, and GameLogic, linked from
// the archives scripts/native-build.py builds. The harness supplies only the nesting depth that the
// loop's earlier laps would (a machine whose one state succeeds into itself), because the loop
// itself needs a real Object in RETURN and GET_CRATE. Every case runs in a forked child, so a fault
// is reported with its address instead of ending the run. See
// docs/porting/tunnel-guard-null-attack-state.md; run through
// scripts/native-tunnel-guard-null-attack-state-test.py.

#include "PreRTS.h"

#include "Common/CriticalSection.h"
#include "Common/GameMemory.h"
#include "Common/StateMachine.h"
#include "GameLogic/AI.h"
#include "GameLogic/AITNGuard.h"
#include "GameLogic/GameLogic.h"

#include <csignal>
#include <cstdio>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>

namespace
{

int Failures = 0;

// main()'s prologue in GeneralsMD/Code/Main/PlatformMain.cpp, in storage that is never destroyed.
ImmortalCriticalSection AsciiStringSection;
ImmortalCriticalSection UnicodeStringSection;
ImmortalCriticalSection DmaSection;
ImmortalCriticalSection MemoryPoolSection;
ImmortalCriticalSection DebugLogSection;

// friend_checkForTransitions() levels this harness nests before the guard machine's first one.
// INNER's success transition then runs at level DepthBeforeGuard + 1 and OUTER's at + 2, which is
// refused when it reaches 20 (StateMachine.cpp, `checkfortransitionsnum >= 20`).
const Int OuterLeftCurrentDepth = 18;
const Int InnerLeftCurrentDepth = 19;

// The real guard machine, with the state lookup the engine keeps protected made reachable.
class HarnessGuardMachine : public AITNGuardMachine
{
	MEMORY_POOL_GLUE_WITH_EXPLICIT_CREATE(HarnessGuardMachine, "HarnessGuardMachinePool", 4, 4)
public:
	HarnessGuardMachine() : AITNGuardMachine(nullptr) {}
	State *stateFor(StateID id) { return internalGetState(id); }
};
HarnessGuardMachine::~HarnessGuardMachine() {}

HarnessGuardMachine *TheGuard = nullptr;
Int RemainingDepth = 0;

enum { DEPTH_STATE = 1 };

// Succeeds into itself RemainingDepth times, then sets the guard machine's state from inside the
// deepest of those transitions -- what the guard loop's own earlier laps amount to.
class HarnessDepthState : public State
{
	MEMORY_POOL_GLUE_WITH_EXPLICIT_CREATE(HarnessDepthState, "HarnessDepthStatePool", 4, 4)
public:
	HarnessDepthState(StateMachine *machine) : State(machine, "HarnessDepthState") {}
	virtual StateReturnType onEnter() override
	{
		if (RemainingDepth > 0)
		{
			--RemainingDepth;
			return STATE_SUCCESS;
		}
		TheGuard->setState(AI_TN_GUARD_INNER);
		return STATE_CONTINUE;
	}
	virtual StateReturnType update() override { return STATE_CONTINUE; }
protected:
	virtual void crc(Xfer *) override {}
	virtual void xfer(Xfer *) override {}
	virtual void loadPostProcess() override {}
};
HarnessDepthState::~HarnessDepthState() {}

class HarnessDepthMachine : public StateMachine
{
	MEMORY_POOL_GLUE_WITH_EXPLICIT_CREATE(HarnessDepthMachine, "HarnessDepthMachinePool", 4, 4)
public:
	HarnessDepthMachine() : StateMachine(nullptr, "HarnessDepthMachine")
	{
		defineState(DEPTH_STATE, newInstance(HarnessDepthState)(this), DEPTH_STATE, DEPTH_STATE);
	}
};
HarnessDepthMachine::~HarnessDepthMachine() {}

// Async-signal-safe: report the faulting address on stdout and end the child with the signal number.
void reportFault(int signalNumber, siginfo_t *info, void *)
{
	char line[96];
	int length = std::snprintf(line, sizeof(line), "      child faulted: signal %d at address %p\n",
		signalNumber, info->si_addr);
	write(STDOUT_FILENO, line, length);
	_exit(128 + signalNumber);
}

// Runs `scenario` in a forked child: true only if it returned STATE_SUCCESS without faulting.
bool childReturnsSuccess(StateReturnType (*scenario)())
{
	std::fflush(stdout);
	pid_t child = fork();
	if (child == 0)
	{
		struct sigaction action;
		std::memset(&action, 0, sizeof(action));
		action.sa_sigaction = reportFault;
		action.sa_flags = SA_SIGINFO;
		sigaction(SIGSEGV, &action, nullptr);
		sigaction(SIGBUS, &action, nullptr);
		StateReturnType result = scenario();
		std::fflush(stdout);
		_exit(result == STATE_SUCCESS ? 0 : 1);
	}
	int status = 0;
	waitpid(child, &status, 0);
	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

void check(const char *what, bool ok)
{
	if (!ok)
		Failures++;
	std::printf("%-84s %s\n", what, ok ? "ok" : "FAILED");
	std::fflush(stdout);
}

const char *guardStateName(StateID id)
{
	switch (id)
	{
		case AI_TN_GUARD_INNER: return "AI_TN_GUARD_INNER";
		case AI_TN_GUARD_IDLE: return "AI_TN_GUARD_IDLE";
		case AI_TN_GUARD_OUTER: return "AI_TN_GUARD_OUTER";
		case AI_TN_GUARD_RETURN: return "AI_TN_GUARD_RETURN";
		case AI_TN_GUARD_GET_CRATE: return "AI_TN_GUARD_GET_CRATE";
		case AI_TN_GUARD_ATTACK_AGGRESSOR: return "AI_TN_GUARD_ATTACK_AGGRESSOR";
		default: return "none";
	}
}

// Enters INNER with no nemesis at the given nesting depth and reports which state is left current.
StateID enterInnerAtDepth(Int depth, GuardMode mode)
{
	TheGuard = newInstance(HarnessGuardMachine)();
	TheGuard->setGuardMode(mode);
	RemainingDepth = depth;
	HarnessDepthMachine *prefix = newInstance(HarnessDepthMachine)();
	prefix->setState(DEPTH_STATE);
	StateID current = TheGuard->getCurrentStateID();
	std::printf("      depth %d: %s is left current\n", depth, guardStateName(current));
	std::fflush(stdout);
	return current;
}

GuardMode ScenarioMode = GUARDMODE_NORMAL;

// The measured crash: OUTER left current by the refused transition, then updated.
StateReturnType outerLeftCurrentThenUpdated()
{
	if (enterInnerAtDepth(OuterLeftCurrentDepth, ScenarioMode) != AI_TN_GUARD_OUTER)
		return STATE_CONTINUE;	// the mechanism did not reproduce; reported as a failure
	return TheGuard->stateFor(AI_TN_GUARD_OUTER)->update();
}

// INNER left current the same way. Its update() reads the owner's team before the attack state, and
// the harness has no Object, so only the state left current is checked here.
StateReturnType innerLeftCurrent()
{
	return enterInnerAtDepth(InnerLeftCurrentDepth, GUARDMODE_NORMAL) == AI_TN_GUARD_INNER
		? STATE_SUCCESS : STATE_CONTINUE;
}

// Its onEnter() reads the owner's body module first, so it is updated as constructed: m_attackState
// null, as its null-nemesis early return leaves it.
StateReturnType attackAggressorWithoutAttackState()
{
	TheGuard = newInstance(HarnessGuardMachine)();
	return TheGuard->stateFor(AI_TN_GUARD_ATTACK_AGGRESSOR)->update();
}

}	// namespace

int main()
{
	TheAsciiStringCriticalSection = AsciiStringSection.get();
	TheUnicodeStringCriticalSection = UnicodeStringSection.get();
	TheDmaCriticalSection = DmaSection.get();
	TheMemoryPoolCriticalSection = MemoryPoolSection.get();
	TheDebugLogCriticalSection = DebugLogSection.get();
	initMemoryManager();

	// Asked only findObjectByID(INVALID_ID), which answers null without looking at any object.
	TheGameLogic = NEW GameLogic;

	std::printf("-- INNER entered with no nemesis, transitions nested to the cutoff\n");
	ScenarioMode = GUARDMODE_NORMAL;
	check("  OUTER is left current with no attack state, and its update() returns STATE_SUCCESS",
		childReturnsSuccess(outerLeftCurrentThenUpdated));

	std::printf("-- the same in GUARDMODE_GUARD_WITHOUT_PURSUIT\n");
	ScenarioMode = GUARDMODE_GUARD_WITHOUT_PURSUIT;
	check("  OUTER is left current with no attack state, and its update() returns STATE_SUCCESS",
		childReturnsSuccess(outerLeftCurrentThenUpdated));

	// One level deeper the refusal moves to INNER's own transition: the depth, not the state, decides.
	std::printf("-- one level deeper\n");
	check("  INNER is left current with no attack state (its update() is covered by the source scan)",
		childReturnsSuccess(innerLeftCurrent));

	std::printf("-- attack-aggressor state with no attack state\n");
	check("  update() returns STATE_SUCCESS instead of dereferencing a null attack state",
		childReturnsSuccess(attackAggressorWithoutAttackState));

	std::printf("%d failed checks\n", Failures);
	std::fflush(stdout);
	return Failures == 0 ? 0 : 1;
}
