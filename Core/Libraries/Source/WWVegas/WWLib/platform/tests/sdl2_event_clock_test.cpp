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

// Every event the SDL2 backend (platform_window_sdl2.cpp) queues must carry a Time_Ms on
// timeGetTime()'s clock, which Keyboard::checkKeyRepeat() measures a key's hold against. SDL
// stamps events with SDL_GetTicks(), milliseconds since SDL was initialised, so a raw stamp makes
// every key look held since boot. Built and run by scripts/native-sdl2-event-clock-test.py; see
// docs/porting/event-clock.md.
//
// Exit status: 0 pass, 1 fail, 77 skip (no video device to open the window on).

#include "platform_window.h"

#include <Utility/time_compat.h>

#include <SDL.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace WWPlatform;

namespace
{

// Both clocks are truncated to whole milliseconds and read a few microseconds apart.
const int CLOCK_TOLERANCE_MSEC = 2;

int TheFailures = 0;

void Check(bool condition, const char * what)
{
	std::printf("%s   %s\n", condition ? "PASS" : "FAIL", what);
	if (!condition) ++TheFailures;
}

std::vector<WindowEvent> Drain(void * window)
{
	std::vector<WindowEvent> events;
	WindowEvent event;
	while (Window_Poll_Event(window, event)) events.push_back(event);
	return events;
}

const WindowEvent * Find_Event(const std::vector<WindowEvent> & events, WindowEventType type)
{
	for (const WindowEvent & event : events) {
		if (event.Type == type) return &event;
	}
	return nullptr;
}

// The event must be stamped with the engine time at which SDL queued it, however late the pump
// reaches it: that makes checkKeyRepeat()'s hold the real time since the press. Comparing with
// timeGetTime() after the pump instead would measure the pump's latency too.
void Check_Stamp(const char * name, const WindowEvent * event, unsigned int pushed_engine_ms)
{
	const int error_ms = event != nullptr ? static_cast<int>(event->Time_Ms - pushed_engine_ms) : 0;
	char what[200];
	std::snprintf(what, sizeof(what),
	              "%s: Time_Ms is %d ms off the engine time it was pushed at (want within %d; "
	              "the pump reached it %u ms later)",
	              name, error_ms, CLOCK_TOLERANCE_MSEC, timeGetTime() - pushed_engine_ms);
	Check(event != nullptr && std::abs(error_ms) <= CLOCK_TOLERANCE_MSEC, what);
}

// SDL_PushEvent stamps the event with SDL_GetTicks() itself.
void Push_Key(SDL_EventType type)
{
	SDL_Event event = {};
	event.type = type;
	event.key.state = type == SDL_KEYDOWN ? SDL_PRESSED : SDL_RELEASED;
	event.key.keysym.scancode = SDL_SCANCODE_A;
	SDL_PushEvent(&event);
}

void Push_Mouse_Down()
{
	SDL_Event event = {};
	event.type = SDL_MOUSEBUTTONDOWN;
	event.button.button = SDL_BUTTON_LEFT;
	event.button.state = SDL_PRESSED;
	event.button.clicks = 1;
	SDL_PushEvent(&event);
}

} // namespace

int main()
{
	WindowConfig config;
	config.Title = "sdl2 event clock test";
	config.Width = 320;
	config.Height = 240;
	void * window = Window_Create(config);
	if (window == nullptr) {
		std::printf("SKIP   Window_Create() failed: %s\n", Window_Last_Error());
		return 77;
	}
	Drain(window);
	std::printf("       timeGetTime() = %u ms, SDL_GetTicks() = %u ms on this run\n", timeGetTime(),
	            SDL_GetTicks());

	unsigned int pushed_engine_ms = timeGetTime();
	Push_Key(SDL_KEYDOWN);
	Push_Key(SDL_KEYUP);
	std::vector<WindowEvent> events = Drain(window);
	Check_Stamp("KEY_DOWN", Find_Event(events, WINDOW_EVENT_KEY_DOWN), pushed_engine_ms);

	// An event that waited in SDL's queue keeps its age: the stamp is when SDL received it, so a
	// held key starts repeating 333 ms after the real press, not after the pump.
	pushed_engine_ms = timeGetTime();
	Push_Key(SDL_KEYDOWN);
	SDL_Delay(300);
	Push_Key(SDL_KEYUP);
	events = Drain(window);
	Check_Stamp("KEY_DOWN queued 300 ms before the pump", Find_Event(events, WINDOW_EVENT_KEY_DOWN),
	            pushed_engine_ms);

	pushed_engine_ms = timeGetTime();
	Push_Mouse_Down();
	events = Drain(window);
	Check_Stamp("MOUSE_DOWN", Find_Event(events, WINDOW_EVENT_MOUSE_DOWN), pushed_engine_ms);

	Window_Destroy(window);
	std::printf("%d failure(s)\n", TheFailures);
	return TheFailures == 0 ? 0 : 1;
}
