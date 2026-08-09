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

#include "WWLib/platform/platform_mutex.h"

#ifndef _WIN32

#include <chrono>
#include <mutex>

namespace WWPlatform
{

/*
**	The Win32 mutex is recursive, so the timed stand-in has to be too. std::recursive_timed_mutex
**	provides exactly that.
*/
void * Mutex_Create()
{
	return new std::recursive_timed_mutex;
}

void Mutex_Destroy(void * handle)
{
	delete static_cast<std::recursive_timed_mutex *>(handle);
}

bool Mutex_Lock(void * handle, int time)
{
	std::recursive_timed_mutex * mutex = static_cast<std::recursive_timed_mutex *>(handle);
	if (time < 0) {
		mutex->lock();
		return true;
	}
	return mutex->try_lock_for(std::chrono::milliseconds(time));
}

void Mutex_Unlock(void * handle)
{
	static_cast<std::recursive_timed_mutex *>(handle)->unlock();
}

void * Critical_Section_Create()
{
	return new std::recursive_mutex;
}

void Critical_Section_Destroy(void * handle)
{
	delete static_cast<std::recursive_mutex *>(handle);
}

void Critical_Section_Enter(void * handle)
{
	static_cast<std::recursive_mutex *>(handle)->lock();
}

void Critical_Section_Leave(void * handle)
{
	static_cast<std::recursive_mutex *>(handle)->unlock();
}

}	// namespace WWPlatform

#endif // !_WIN32
