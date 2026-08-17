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

/***********************************************************************************************
 *                                                                                             *
 *  What rts::ClientInstance::initialize() says when it refuses to start.                       *
 *                                                                                             *
 *  A leftover process holding the instance lock made a dozen launches fail with nothing on      *
 *  stdout or stderr: initialize() returned false, main returned, and the only way to find out    *
 *  why was lsof (docs/porting/real-input-menu-drive.md 4.4). The refusal is correct and stays   *
 *  a refusal; this file is about whether it is now legible.                                     *
 *                                                                                             *
 *  Three cases, one process each, chosen by argv[1] and run by                                  *
 *  scripts/native-instance-lock-test.py:                                                        *
 *    free        nothing holds the lock: initialize() succeeds and says nothing. The control --  *
 *                a diagnostic that also appears on the success path is noise, not a diagnostic.  *
 *    held        this process holds the lock on another descriptor, which flock() refuses the    *
 *                same way it refuses another process (the lock belongs to the open file          *
 *                description, not to the pid). initialize() must fail and name the file and the  *
 *                holder's pid.                                                                  *
 *    unusable    the runtime directory cannot be written, so the lock cannot be taken at all --  *
 *                a different cause with the same return value, which is exactly why it has to    *
 *                be said out loud rather than reported as "already running".                     *
 *                                                                                             *
 *  The lock, the diagnostic and the engine's real ClientInstance are linked out of the archives  *
 *  scripts/native-build.py produced; nothing here is a stand-in for them.                        *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#include "Common/CriticalSection.h"
#include "Common/GameMemory.h"
#include "GameClient/ClientInstance.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

#include <cstdio>

namespace
{

int Failures = 0;

void Check(const char * what, bool ok)
{
	if (!ok) Failures++;
	std::printf("%-62s %s\n", what, ok ? "yes" : "NO");
	std::fflush(stdout);
}

// main()'s prologue in GeneralsMD/Code/Main/PlatformMain.cpp: ClientInstance builds a std::string,
// so the allocator and its sections have to be up.
ImmortalCriticalSection AsciiStringSection;
ImmortalCriticalSection UnicodeStringSection;
ImmortalCriticalSection DmaSection;
ImmortalCriticalSection MemoryPoolSection;
ImmortalCriticalSection DebugLogSection;

void Engine_Prologue()
{
	TheAsciiStringCriticalSection = AsciiStringSection.get();
	TheUnicodeStringCriticalSection = UnicodeStringSection.get();
	TheDmaCriticalSection = DmaSection.get();
	TheMemoryPoolCriticalSection = MemoryPoolSection.get();
	TheDebugLogCriticalSection = DebugLogSection.get();
	initMemoryManager();
}

/**
	The lock file the engine will try, built the way WWPlatform::Instance_Lock_Acquire() builds it.
	The runner points XDG_RUNTIME_DIR at a directory of its own, so this is inside it.
*/
void Lock_Path(char * buffer, size_t capacity)
{
	const char * directory = getenv("XDG_RUNTIME_DIR");
	if (directory == NULL || *directory == '\0')
		directory = getenv("TMPDIR");
	if (directory == NULL || *directory == '\0')
		directory = "/tmp";
	std::snprintf(buffer, capacity, "%s/%s.lock", directory,
		rts::ClientInstance::getFirstInstanceName());
}

}	// namespace

int main(int argc, char ** argv)
{
	Engine_Prologue();

	const char * which = argc > 1 ? argv[1] : "";
	std::printf("case: %s\n", which);

	char path[1024];
	Lock_Path(path, sizeof(path));
	std::printf("lock file: %s\n", path);
	std::fflush(stdout);

	if (strcmp(which, "held") == 0)
	{
		// A second open file description on the same file: flock() treats it as a competing holder
		// whether or not it belongs to another process, so the case is deterministic without a
		// second process to leave lying around.
		const int descriptor = open(path, O_RDWR | O_CREAT, 0600);
		Check("the lock file could be opened", descriptor >= 0);
		Check("this process took the lock first", flock(descriptor, LOCK_EX | LOCK_NB) == 0);

		// What Instance_Lock_Acquire() writes for a human to read; the diagnostic quotes it back.
		char text[32];
		const int length = std::snprintf(text, sizeof(text), "%ld\n", (long)getpid());
		Check("the holder's pid is recorded in the file", write(descriptor, text, length) == length);
		std::printf("holder pid: %ld\n", (long)getpid());
		std::fflush(stdout);

		Check("initialize() refuses to start", !rts::ClientInstance::initialize());
		Check("and it is not initialized", !rts::ClientInstance::isInitialized());
	}
	else if (strcmp(which, "free") == 0)
	{
		Check("initialize() succeeds when nothing holds the lock",
			rts::ClientInstance::initialize());
		Check("and it is initialized", rts::ClientInstance::isInitialized());
	}
	else if (strcmp(which, "unusable") == 0)
	{
		// The runner has made the directory unwritable, so open() fails. Reported as its own cause
		// rather than as another instance.
		Check("initialize() refuses to start", !rts::ClientInstance::initialize());
		Check("and it is not initialized", !rts::ClientInstance::isInitialized());
	}
	else
	{
		std::printf("no case selected: pass free, held or unusable\n");
		return 2;
	}

	std::printf("main returning with %d failed checks\n", Failures);
	std::fflush(stdout);
	return Failures;
}
