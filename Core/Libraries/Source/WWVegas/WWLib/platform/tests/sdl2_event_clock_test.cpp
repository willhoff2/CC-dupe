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
#include <vector>

using namespace WWPlatform;

namespace
{

// Keyboard::KEY_REPEAT_DELAY_MSEC: a key held longer than this on timeGetTime()'s clock repeats.
const unsigned int KEY_REPEAT_DELAY_MSEC = 333;
// Slack for the pump and the two clock reads.
const unsigned int CLOCK_TOLERANCE_MSEC = 50;

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

// The event's age exactly as Keyboard::checkKeyRepeat() computes a hold: timeGetTime() minus the
// stamp, unsigned.
unsigned int Engine_Age_Ms(const WindowEvent * event)
{
	return event != nullptr ? timeGetTime() - event->Time_Ms : ~0u;
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

	char what[200];
	Push_Key(SDL_KEYDOWN);
	Push_Key(SDL_KEYUP);
	std::vector<WindowEvent> events = Drain(window);
	const unsigned int key_age = Engine_Age_Ms(Find_Event(events, WINDOW_EVENT_KEY_DOWN));
	std::snprintf(what, sizeof(what),
	              "fresh KEY_DOWN: checkKeyRepeat() would see it held %u ms (want <= %u; it "
	              "repeats past %u)", key_age, CLOCK_TOLERANCE_MSEC, KEY_REPEAT_DELAY_MSEC);
	Check(key_age <= CLOCK_TOLERANCE_MSEC, what);

	// An event that waited in SDL's queue keeps its age: the stamp is when SDL received it.
	const unsigned int queued_age_ms = 300;
	Push_Key(SDL_KEYDOWN);
	SDL_Delay(queued_age_ms);
	Push_Key(SDL_KEYUP);
	events = Drain(window);
	const unsigned int aged_key_age = Engine_Age_Ms(Find_Event(events, WINDOW_EVENT_KEY_DOWN));
	std::snprintf(what, sizeof(what),
	              "KEY_DOWN queued %u ms before the pump: Time_Ms is %u ms before timeGetTime()",
	              queued_age_ms, aged_key_age);
	Check(aged_key_age + CLOCK_TOLERANCE_MSEC >= queued_age_ms &&
	          aged_key_age <= queued_age_ms + CLOCK_TOLERANCE_MSEC,
	      what);

	Push_Mouse_Down();
	events = Drain(window);
	const unsigned int mouse_age = Engine_Age_Ms(Find_Event(events, WINDOW_EVENT_MOUSE_DOWN));
	std::snprintf(what, sizeof(what), "fresh MOUSE_DOWN: Time_Ms is %u ms before timeGetTime()",
	              mouse_age);
	Check(mouse_age <= CLOCK_TOLERANCE_MSEC, what);

	Window_Destroy(window);
	std::printf("%d failure(s)\n", TheFailures);
	return TheFailures == 0 ? 0 : 1;
}
