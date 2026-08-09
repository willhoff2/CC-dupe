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

#include "WWLib/platform/platform_thread.h"

#ifndef _WIN32

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <new>
#include <thread>

namespace WWPlatform
{

unsigned long Thread_Create(ThreadEntryType entry, void * parameter)
{
	static std::atomic<unsigned long> _next_token(1);

	try {
		std::thread thread(entry, parameter);
		thread.detach();
	} catch (const std::system_error &) {
		return 0;
	}

	return _next_token++;
}

bool Thread_Set_Priority(unsigned long, int)
{
	/*
	**	There is no portable per thread priority outside of the realtime scheduling policies,
	**	which need privileges the game does not have. The threads run at the default priority.
	*/
	return false;
}

struct EventImplementation
{
	std::mutex Mutex;
	std::condition_variable Condition;
	bool Signalled;

	EventImplementation() : Signalled(false) {}
};

EventClass::EventClass() : Implementation(new EventImplementation)
{
}

EventClass::~EventClass()
{
	delete Implementation;
}

bool EventClass::Wait(int time)
{
	std::unique_lock<std::mutex> lock(Implementation->Mutex);

	if (time < 0) {
		Implementation->Condition.wait(lock, [this]() { return Implementation->Signalled; });
	} else {
		if (!Implementation->Condition.wait_for(lock, std::chrono::milliseconds(time),
				[this]() { return Implementation->Signalled; })) {
			return false;
		}
	}

	// Auto reset, as CreateEvent() was called with manual reset turned off.
	Implementation->Signalled = false;
	return true;
}

void EventClass::Signal()
{
	{
		std::lock_guard<std::mutex> lock(Implementation->Mutex);
		Implementation->Signalled = true;
	}
	Implementation->Condition.notify_one();
}

}	// namespace WWPlatform

#endif // !_WIN32
