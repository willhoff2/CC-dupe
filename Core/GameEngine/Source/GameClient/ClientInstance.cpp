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
#include "PreRTS.h"

#ifdef _WIN32
// TheSuperHackers @port Win32 header pushed down from PreRTS.h; see docs/porting/prerts-win32-surgery.md
#include <windows.h>
#else
#include "WWLib/platform/platform_process.h"
#endif

#include "GameClient/ClientInstance.h"

#define GENERALS_GUID "685EAFF2-3216-4265-B047-251C5F4B82F3"

namespace rts
{
namespace
{
// TheSuperHackers @port The single instance test, and only that test, differs by platform. A
// Win32 named mutex vanishes with the process that created it; the lock file used elsewhere does
// not, so the lock rather than the file is what marks an instance as running. The crash
// semantics are not identical - see docs/porting/process-and-crash-seam.md.
// Returns null when the lock could not be taken, with alreadyExists telling the two reasons
// apart the way GetLastError() == ERROR_ALREADY_EXISTS does on Windows.
void* acquireInstanceLock(const char* name, bool& alreadyExists)
{
#ifdef _WIN32
	HANDLE mutexHandle = CreateMutex(nullptr, FALSE, name);
	alreadyExists = GetLastError() == ERROR_ALREADY_EXISTS;
	if (alreadyExists && mutexHandle != nullptr)
	{
		CloseHandle(mutexHandle);
		mutexHandle = nullptr;
	}
	return mutexHandle;
#else
	// flock() cannot say whether the lock is held by another instance or could not be taken at
	// all, and treating an unusable lock as an instance already running is the safe reading.
	void* lock = WWPlatform::Instance_Lock_Acquire(name);
	alreadyExists = lock == nullptr;
	return lock;
#endif
}

#ifndef _WIN32
// TheSuperHackers @bugfix Devin 17/08/2026 Say why the process is not starting.
//
// Off Windows the single instance test is an flock() on a file in the per user runtime directory,
// and a process that is still holding it - a leftover client, a hung one, a debugger's inferior -
// makes every launch return from main having printed nothing at all on stdout or stderr. That cost
// a session a dozen launches that looked like the game silently refusing to run, until lsof found
// PID 23569 still on the descriptor (docs/porting/real-input-menu-drive.md 4.4). The lock is right;
// what was missing is a sentence naming the file and the holder, so the next person can look.
//
// stderr rather than DEBUG_LOG because the log file is what a release build does not write and what
// nobody reads before knowing there is something to read. Windows is untouched: it raises the
// running instance's window instead, which is a report of its own, and WinMain has no console.
void reportInstanceLockRefusal(const char* name)
{
	WWPlatform::InstanceLockFailure failure;
	if (!WWPlatform::Instance_Lock_Last_Failure(failure))
	{
		// The lock was refused without the platform recording why, which should not happen.
		fprintf(stderr, "Generals is already running: the single instance lock for %s is held.\n",
			name);
		return;
	}

	if (strcmp(failure.Operation, "flock") == 0)
	{
		if (failure.Holder_Pid > 0)
		{
			// The actionable case, and the one that was mistaken for a silent failure.
			fprintf(stderr, "Generals is already running: %s is locked by pid %ld%s. "
				"Close that process (or `kill %ld`) and start again.\n",
				failure.Path, failure.Holder_Pid,
				failure.Holder_Is_Running ? "" : ", which no longer exists",
				failure.Holder_Pid);
		}
		else
		{
			fprintf(stderr, "Generals is already running: %s is locked by another process, which "
				"did not record its pid. `lsof %s` will name it.\n", failure.Path, failure.Path);
		}
	}
	else
	{
		// Not another instance at all: the lock itself is unusable, and the refusal is the
		// deliberately safe reading of that. Naming it keeps a permissions problem from looking
		// like a running game.
		fprintf(stderr, "Cannot take the single instance lock %s: %s failed with %s (%d). "
			"Refusing to start rather than risk a second instance.\n",
			failure.Path, failure.Operation, strerror(failure.Error), failure.Error);
	}

	fflush(stderr);
}
#endif // !_WIN32
} // anonymous namespace

void* ClientInstance::s_instanceLock = nullptr;
UnsignedInt ClientInstance::s_instanceIndex = 0;

#if defined(RTS_MULTI_INSTANCE)
Bool ClientInstance::s_isMultiInstance = true;
#else
Bool ClientInstance::s_isMultiInstance = false;
#endif

bool ClientInstance::initialize()
{
	if (isInitialized())
	{
		return true;
	}

	// Take a lock with a unique name to Generals in order to determine if our app is already running.
	// WARNING: DO NOT use this number for any other application except Generals.
	bool alreadyExists = false;
	while (true)
	{
		if (isMultiInstance())
		{
			std::string guidStr = getFirstInstanceName();
			if (s_instanceIndex > 0u)
			{
				// TheSuperHackers @port itoa() is an MSVC CRT spelling with no portable equivalent.
				char idStr[33];
				sprintf(idStr, "%u", s_instanceIndex);
				guidStr.push_back('-');
				guidStr.append(idStr);
			}
			s_instanceLock = acquireInstanceLock(guidStr.c_str(), alreadyExists);
			if (alreadyExists)
			{
				// Try again with a new instance.
				++s_instanceIndex;
				continue;
			}
		}
		else
		{
			s_instanceLock = acquireInstanceLock(getFirstInstanceName(), alreadyExists);
			if (alreadyExists)
			{
#ifndef _WIN32
				reportInstanceLockRefusal(getFirstInstanceName());
#endif
				return false;
			}
		}
		break;
	}

	return true;
}

bool ClientInstance::isInitialized()
{
	return s_instanceLock != nullptr;
}

bool ClientInstance::isMultiInstance()
{
	return s_isMultiInstance;
}

void ClientInstance::setMultiInstance(bool v)
{
	if (isInitialized())
	{
		DEBUG_CRASH(("ClientInstance::setMultiInstance(%d) - cannot set multi instance after initialization", (int)v));
		return;
	}
	s_isMultiInstance = v;
}

void ClientInstance::skipPrimaryInstance()
{
	if (isInitialized())
	{
		DEBUG_CRASH(("ClientInstance::skipPrimaryInstance() - cannot skip primary instance after initialization"));
		return;
	}
	s_instanceIndex = 1;
}

UnsignedInt ClientInstance::getInstanceIndex()
{
	DEBUG_ASSERTLOG(isInitialized(), ("ClientInstance::isInitialized() failed"));
	return s_instanceIndex;
}

UnsignedInt ClientInstance::getInstanceId()
{
	return getInstanceIndex() + 1;
}

const char* ClientInstance::getFirstInstanceName()
{
	return GENERALS_GUID;
}

} // namespace rts
