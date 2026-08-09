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
 *  Threads and events for the platforms that have no _beginthread or CreateEvent. Threads are  *
 *  identified by an opaque non zero token so that ThreadClass can keep storing a single value  *
 *  the way it stored the _beginthread handle.                                                  *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#pragma once

#ifndef _WIN32

namespace WWPlatform
{

typedef void (*ThreadEntryType)(void * parameter);

/*
**	Equivalent of _beginthread(). Returns zero if the thread could not be started. The thread
**	runs detached, so the token only has to stay valid until the entry point returns.
*/
unsigned long Thread_Create(ThreadEntryType entry, void * parameter);

/*
**	Equivalent of SetThreadPriority(). Priority zero is normal, as in ThreadClass. Returns false
**	if the priority could not be changed, which is the usual case away from Windows.
*/
bool Thread_Set_Priority(unsigned long thread, int priority);

/*
**	Equivalent of the CreateEvent()/WaitForSingleObject()/SetEvent() trio, for the auto reset
**	events the library uses. Time is in milliseconds; a negative time waits forever.
*/
class EventClass
{
public:
	EventClass();
	~EventClass();

	bool Wait(int time);
	void Signal();

private:
	EventClass(const EventClass &);
	EventClass & operator=(const EventClass &);

	struct EventImplementation * Implementation;
};

}	// namespace WWPlatform

#endif // !_WIN32
