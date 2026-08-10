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
