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

#include "mutex.h"
#include "WWDebug/wwdebug.h"
#ifdef _WIN32
#include <windows.h>
#else
#include "platform/platform_mutex.h"
#endif

// ----------------------------------------------------------------------------

MutexClass::MutexClass(const char* name) : handle(nullptr), locked(false)
{
	#ifdef _WIN32
		handle=CreateMutex(nullptr,false,name);
	#else
		(void)name;	// A globally unique mutex needs an inter-process primitive we don't have.
		handle=WWPlatform::Mutex_Create();
	#endif
	WWASSERT(handle);
}

MutexClass::~MutexClass()
{
	WWASSERT(!locked); // Can't delete locked mutex!
	#ifdef _WIN32
		CloseHandle(handle);
	#else
		WWPlatform::Mutex_Destroy(handle);
	#endif
}

bool MutexClass::Lock(int time)
{
	#ifdef _WIN32
		int res = WaitForSingleObject(handle,time==WAIT_INFINITE ? INFINITE : time);
		if (res!=WAIT_OBJECT_0) return false;
	#else
		if (!WWPlatform::Mutex_Lock(handle,time==WAIT_INFINITE ? -1 : time)) return false;
	#endif
	locked++;
	return true;
}

void MutexClass::Unlock()
{
	WWASSERT(locked);
	locked--;
	#ifdef _WIN32
		int res=ReleaseMutex(handle);
		res;	// silence compiler warnings
		WWASSERT(res);
	#else
		WWPlatform::Mutex_Unlock(handle);
	#endif
}

// ----------------------------------------------------------------------------

MutexClass::LockClass::LockClass(MutexClass& mutex_,int time) : mutex(mutex_)
{
	failed=!mutex.Lock(time);
}

MutexClass::LockClass::~LockClass()
{
	if (!failed) mutex.Unlock();
}







// ----------------------------------------------------------------------------

CriticalSectionClass::CriticalSectionClass() : handle(nullptr), locked(false)
{
	#ifdef _WIN32
		handle=W3DNEWARRAY char[sizeof(CRITICAL_SECTION)];
		InitializeCriticalSection((CRITICAL_SECTION*)handle);
	#else
		handle=WWPlatform::Critical_Section_Create();
	#endif
}

CriticalSectionClass::~CriticalSectionClass()
{
	WWASSERT(!locked); // Can't delete locked mutex!
	#ifdef _WIN32
		DeleteCriticalSection((CRITICAL_SECTION*)handle);
		delete[] handle;
	#else
		WWPlatform::Critical_Section_Destroy(handle);
	#endif
}

void CriticalSectionClass::Lock()
{
	#ifdef _WIN32
		EnterCriticalSection((CRITICAL_SECTION*)handle);
	#else
		WWPlatform::Critical_Section_Enter(handle);
	#endif
	locked++;
}

void CriticalSectionClass::Unlock()
{
	WWASSERT(locked);
	locked--;
	#ifdef _WIN32
		LeaveCriticalSection((CRITICAL_SECTION*)handle);
	#else
		WWPlatform::Critical_Section_Leave(handle);
	#endif
}

// ----------------------------------------------------------------------------

CriticalSectionClass::LockClass::LockClass(CriticalSectionClass& critical_section) : CriticalSection(critical_section)
{
	CriticalSection.Lock();
}

CriticalSectionClass::LockClass::~LockClass()
{
	CriticalSection.Unlock();
}


