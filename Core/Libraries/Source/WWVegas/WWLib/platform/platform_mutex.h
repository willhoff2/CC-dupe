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

/***********************************************************************************************
 *                                                                                             *
 *                 Project Name : Westwood Library                                             *
 *                                                                                             *
 *  Synchronisation for the platforms that have no CreateMutex or CRITICAL_SECTION. The         *
 *  handles are opaque so that no standard library headers leak into mutex.h.                   *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#pragma once

#ifndef _WIN32

namespace WWPlatform
{

/*
**	Equivalent of CreateMutex()/CloseHandle() and the wait/release pair. Unlike the Win32
**	mutex this one is process local, so the globally unique name is not honoured.
*/
void * Mutex_Create();
void Mutex_Destroy(void * handle);

/*
**	Time is in milliseconds. A negative time waits forever, matching INFINITE. Returns false
**	if the wait timed out.
*/
bool Mutex_Lock(void * handle, int time);
void Mutex_Unlock(void * handle);

/*
**	Equivalent of InitializeCriticalSection() and friends. Recursive, like the Win32 one.
*/
void * Critical_Section_Create();
void Critical_Section_Destroy(void * handle);
void Critical_Section_Enter(void * handle);
void Critical_Section_Leave(void * handle);

}	// namespace WWPlatform

#endif // !_WIN32
