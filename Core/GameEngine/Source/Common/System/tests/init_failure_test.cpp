/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
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

// Behaviour test for the init-failure reporting seam, driven by
// scripts/native-init-failure-test.py.
//
// Two things are asserted, and the second is the point of the test:
//
//   * rts::throwInitFailure() reports: it does not return, and the exception it throws carries a
//     message that names the failing subsystem and what specifically failed.
//   * a subsystem whose init() is *made* to fail is diagnosable through the path
//     GameEngine::init() actually uses -- initSubsystem() calling init(), with the engine's
//     catch(INIException) clause converting the message into the text a RELEASE_CRASH would
//     write. The same driver is run over the pre-fix shape of GameTextManager::init(), which
//     returns silently on failure, and the test asserts that that shape produces no diagnostic at
//     all. That is the negative control: it is what made the .csf desync in #87 cost a debugger
//     session, and if the loud path ever regresses to it, the first assertion below fails.
//
// The subsystem here is a stand-in, not GameTextManager: pulling the real one in would drag in the
// file system, the registry and the memory pools, and what is under test is the seam. See
// docs/porting/init-failure-reporting.md.

#include <stdio.h>
#include <string.h>

#include "Common/InitFailure.h"

static int g_failures = 0;

static void check(bool condition, const char* what)
{
	printf("%s: %s\n", condition ? "ok" : "FAIL", what);
	if (!condition)
		++g_failures;
}

//-----------------------------------------------------------------------------------------------
// A subsystem that cannot load its data, in both shapes.
//-----------------------------------------------------------------------------------------------

class FakeStringManager
{
public:
	FakeStringManager() : m_stringCount(0), m_initialized(false) {}

	// The shape GameTextManager::init() had: the failure is detected and then discarded.
	void initSilently()
	{
		m_initialized = true;
		if (!loadStringTable())
			return;
		m_stringCount = 1;
	}

	// The shape it has now.
	void initLoudly()
	{
		m_initialized = true;
		if (!loadStringTable())
		{
			rts::throwInitFailure("TheGameText", "cannot parse the compiled string file '%s'",
				"Data\\English\\generals.csf");
		}
		m_stringCount = 1;
	}

	bool loadStringTable() const { return false; }  // the forced failure

	int m_stringCount;
	bool m_initialized;
};

// The engine's shape: initSubsystem() cannot inspect a void init(), so an exception is the only
// route out, and GameEngine::init()'s catch clause is what turns it into a reported failure.
// `reason` receives what RELEASE_CRASH would be handed; it stays empty if nothing was reported.
template <class SUBSYSTEM>
static bool initSubsystem(SUBSYSTEM& subsystem, void (SUBSYSTEM::*init)(), char* reason,
	size_t reasonSize)
{
	reason[0] = 0;
	try
	{
		(subsystem.*init)();
		return true;
	}
	// By reference, where GameEngine::init() catches by value: INIException owns its message
	// through a raw pointer and has no copy constructor, so a copy double-frees it. The engine
	// gets away with it because RELEASE_CRASH() has already reported and exited by then; a test
	// that catches more than once does not. See docs/porting/init-failure-reporting.md.
	catch (const INIException& e)
	{
		snprintf(reason, reasonSize, "%s",
			e.mFailureMessage ? e.mFailureMessage : "Uncaught Exception during initialization.");
		return false;
	}
}

//-----------------------------------------------------------------------------------------------

// Never returns, so it needs no return statement. If RTS_INIT_FAILURE_NORETURN stops being
// applied, -Werror=return-type fails this translation unit.
static int alwaysFails()
{
	rts::throwInitFailure("TheSubsystem", "%s", "detail");
}

int main()
{
	char reason[512];

	// 1. The loud shape: the failure reaches the caller, naming the subsystem and the file.
	FakeStringManager loud;
	bool loudOk = initSubsystem(loud, &FakeStringManager::initLoudly, reason, sizeof(reason));
	check(!loudOk, "a failing init() reports failure to its caller");
	check(strstr(reason, "TheGameText") != nullptr, "the report names the failing subsystem");
	check(strstr(reason, "generals.csf") != nullptr, "the report names what failed to load");
	check(strstr(reason, "failed to initialize") != nullptr, "the report says it is an init failure");
	check(loud.m_stringCount == 0, "the subsystem did not finish initializing");

	// 2. The negative control: the shape this seam replaces reports nothing, and the caller cannot
	//    tell the difference between that and success.
	FakeStringManager silent;
	bool silentOk = initSubsystem(silent, &FakeStringManager::initSilently, reason, sizeof(reason));
	check(silentOk, "the pre-fix shape looks like success to its caller");
	check(reason[0] == 0, "the pre-fix shape produces no diagnostic at all");
	check(silent.m_initialized && silent.m_stringCount == 0,
		"the pre-fix shape leaves the subsystem half-initialized");

	// 3. throwInitFailure() does not return, whatever the caller does with the result.
	bool returned = false;
	try
	{
		alwaysFails();
		returned = true;
	}
	catch (const INIException& e)
	{
		check(e.mFailureMessage != nullptr, "the exception carries a message");
	}
	check(!returned, "throwInitFailure() does not return");

	// 4. A detail longer than the buffer is truncated, not written past the end: an init failure
	//    must not become a stack overwrite while it is being reported.
	char huge[4096];
	memset(huge, 'x', sizeof(huge) - 1);
	huge[sizeof(huge) - 1] = 0;
	try
	{
		rts::throwInitFailure("TheSubsystem", "%s", huge);
		check(false, "an over-long detail still throws");
	}
	catch (const INIException& e)
	{
		check(e.mFailureMessage != nullptr && strlen(e.mFailureMessage) < sizeof(huge),
			"an over-long detail is truncated rather than overrunning the buffer");
		check(e.mFailureMessage != nullptr && strstr(e.mFailureMessage, "TheSubsystem") != nullptr,
			"an over-long detail still names the subsystem");
	}

	if (g_failures)
		printf("\nFAILED: %d assertion(s)\n", g_failures);
	else
		printf("\nOK: all assertions passed\n");
	return g_failures ? 1 : 0;
}
