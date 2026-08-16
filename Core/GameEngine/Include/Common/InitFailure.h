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

// FILE: InitFailure.h //////////////////////////////////////////////////////////////////////////
// TheSuperHackers @port How a subsystem reports that its init() failed.
//
// Every init() entry point GameEngine::init() drives returns void, and
// SubsystemInterfaceList::initSubsystem() discards nothing because there is nothing to discard,
// so the only way an initialisation failure can reach the caller is by throwing. On Windows with
// retail data these paths never failed; a port makes them reachable, and a subsystem that returns
// normally after failing turns a startup error into arbitrary later misbehaviour. See
// docs/porting/init-failure-reporting.md and docs/porting/process-and-crash-seam.md.
//
// This adds no new mechanism: it throws the exception GameEngine::init() already catches and
// already turns into a message-carrying RELEASE_CRASH, which writes the message to
// ReleaseCrashInfo.txt with a stack trace and exits. It is spelled as a function rather than as a
// macro so it can be exercised by a test on its own -- see scripts/native-init-failure-test.py.
///////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <stdarg.h>
#include <stdio.h>

#include <Utility/stdio_adapter.h>  // snprintf/vsnprintf on VC6

#include "Common/INIException.h"

// VC6 has no [[noreturn]]. Without it the compiler cannot see that the callers below never fall
// through, and warns about values it thinks are used uninitialised afterwards.
#if defined(_MSC_VER) && _MSC_VER < 1900
#define RTS_INIT_FAILURE_NORETURN __declspec(noreturn)
#else
#define RTS_INIT_FAILURE_NORETURN [[noreturn]]
#endif

namespace rts
{

	// Reports that a subsystem could not initialise, and does not return. `subsystem` is the name
	// the subsystem is registered under (`"TheGameText"`), so the message names the failing
	// subsystem; `format` and its arguments say what specifically failed, e.g. which file.
	RTS_INIT_FAILURE_NORETURN inline void throwInitFailure(const char* subsystem,
		const char* format, ...)
	{
		char detail[512];
		va_list args;
		va_start(args, format);
		vsnprintf(detail, sizeof(detail), format, args);
		va_end(args);
		detail[sizeof(detail) - 1] = 0;

		char message[640];
		snprintf(message, sizeof(message), "%s failed to initialize: %s",
			subsystem ? subsystem : "An unnamed subsystem", detail);
		message[sizeof(message) - 1] = 0;

		throw INIException(message);
	}

}  // namespace rts
