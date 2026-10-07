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

// Key routing through the real Cocoa pump (platform_window_cocoa.mm), windowed and fullscreen:
// every key the engine consumes must reach its queue as KEY_DOWN/KEY_UP and must NOT fall off the
// end of AppKit's responder chain, which is where -[NSResponder noResponderFor:] plays the system
// alert sound. Cmd-Q must still reach the main menu. Every event's Time_Ms must be on
// timeGetTime()'s clock, which Keyboard::checkKeyRepeat() measures a key's hold against. Built and
// run by scripts/macos-cocoa-key-routing-test.py; see docs/porting/cocoa-key-routing.md and
// docs/porting/event-clock.md.
//
// Exit status: 0 pass, 1 fail, 77 skip (the window never became key: no windowing session).

#include "platform_window.h"

#include <Utility/time_compat.h>

#import <AppKit/AppKit.h>
#import <objc/runtime.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace WWPlatform;

namespace
{

int TheFailures = 0;
int TheUnhandledKeyCount = 0;			// calls into either AppKit path that ends in NSBeep()

void Check(bool condition, const char * what)
{
	std::printf("%s   %s\n", condition ? "PASS" : "FAIL", what);
	if (!condition) ++TheFailures;
}

// Not forwarded: forwarding would play the alert sound on the test machine. AppKit beeps only
// for keyDown:; the other selectors that fall off the chain are silent and only logged.
void Counting_No_Responder_For(id self, SEL selector, SEL event_selector)
{
	(void)self;
	(void)selector;
	const bool beeps = event_selector == @selector(keyDown:);
	std::printf("       noResponderFor:%s reached%s\n", sel_getName(event_selector),
	            beeps ? " (AppKit would beep)" : "");
	if (beeps) ++TheUnhandledKeyCount;
}

// Escape takes the other path: NSWindow's keyDown: turns it into cancelOperation:, which beeps
// when the window has nothing to cancel (MEASURED: one NSBeep() per Escape before the fix).
void Counting_Cancel_Operation(id self, SEL selector, id sender)
{
	(void)self;
	(void)selector;
	(void)sender;
	std::printf("       -[NSWindow cancelOperation:] reached (AppKit would beep)\n");
	++TheUnhandledKeyCount;
}

void Install_Unhandled_Key_Counter()
{
	Method no_responder = class_getInstanceMethod([NSResponder class], @selector(noResponderFor:));
	method_setImplementation(no_responder, reinterpret_cast<IMP>(Counting_No_Responder_For));
	Method cancel = class_getInstanceMethod([NSWindow class], @selector(cancelOperation:));
	method_setImplementation(cancel, reinterpret_cast<IMP>(Counting_Cancel_Operation));
}

NSWindow * Game_Window()
{
	for (NSWindow * window in [NSApp windows]) {
		if ([window isKindOfClass:NSClassFromString(@"WWGameWindow")]) return window;
	}
	return nil;
}

std::vector<WindowEvent> Drain(void * window)
{
	std::vector<WindowEvent> events;
	WindowEvent event;
	while (Window_Poll_Event(window, event)) events.push_back(event);
	return events;
}

bool Wait_Until_Active(void * window)
{
	for (int attempt = 0; attempt < 300; ++attempt) {
		Drain(window);
		if (Window_Is_Active(window)) return true;
		[NSThread sleepForTimeInterval:0.01];
	}
	return false;
}

void Post_Key_At(NSWindow * window, NSEventType type, unsigned short key_code, unichar character,
                 NSEventModifierFlags modifiers, double timestamp)
{
	NSString * characters = [NSString stringWithCharacters:&character length:1];
	NSEvent * event = [NSEvent keyEventWithType:type
	                                   location:NSZeroPoint
	                              modifierFlags:modifiers
	                                  timestamp:timestamp
	                               windowNumber:[window windowNumber]
	                                    context:nil
	                                 characters:characters
	                charactersIgnoringModifiers:characters
	                                  isARepeat:NO
	                                    keyCode:key_code];
	[NSApp postEvent:event atStart:NO];
}

void Post_Key(NSWindow * window, NSEventType type, unsigned short key_code, unichar character,
              NSEventModifierFlags modifiers)
{
	Post_Key_At(window, type, key_code, character, modifiers,
	            [NSProcessInfo processInfo].systemUptime);
}

void Post_Mouse_At(NSWindow * window, NSEventType type, double timestamp)
{
	NSEvent * event = [NSEvent mouseEventWithType:type
	                                     location:NSMakePoint(10.0, 10.0)
	                                modifierFlags:0
	                                    timestamp:timestamp
	                                 windowNumber:[window windowNumber]
	                                      context:nil
	                                  eventNumber:0
	                                   clickCount:1
	                                     pressure:type == NSEventTypeLeftMouseDown ? 1.0f : 0.0f];
	[NSApp postEvent:event atStart:NO];
}

// Keyboard::KEY_REPEAT_DELAY_MSEC: a key held longer than this on timeGetTime()'s clock repeats.
const unsigned int KEY_REPEAT_DELAY_MSEC = 333;
// Both clocks are truncated or rounded to whole milliseconds and read a few microseconds apart.
const int CLOCK_TOLERANCE_MSEC = 2;

// Latest timestamp of an event the window server delivered (anything the test did not post), so
// the test can show which clock AppKit's own stamps are on rather than assume it.
double TheLatestServerTimestamp = 0.0;

void Install_Server_Timestamp_Monitor()
{
	[NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskAny handler:^NSEvent *(NSEvent * event) {
		const NSEventType type = [event type];
		const bool posted_by_test = type == NSEventTypeKeyDown || type == NSEventTypeKeyUp ||
		                            type == NSEventTypeLeftMouseDown || type == NSEventTypeLeftMouseUp;
		if (!posted_by_test && [event timestamp] > TheLatestServerTimestamp)
			TheLatestServerTimestamp = [event timestamp];
		return event;
	}];
}

// One instant read on both clocks back to back: the engine time a stamp of Uptime_Seconds means.
struct PostTime
{
	double Uptime_Seconds;
	unsigned int Engine_Ms;
};

PostTime Post_Time_Now()
{
	PostTime now;
	now.Uptime_Seconds = [NSProcessInfo processInfo].systemUptime;
	now.Engine_Ms = timeGetTime();
	return now;
}

const WindowEvent * Find_Event(const std::vector<WindowEvent> & events, WindowEventType type)
{
	for (const WindowEvent & event : events) {
		if (event.Type == type) return &event;
	}
	return nullptr;
}

// The event must be stamped with the engine time at which it happened, i.e. when the test stamped
// it, however late the pump reaches it: that makes checkKeyRepeat()'s hold the real time since the
// press. Comparing with timeGetTime() after the pump instead measures the pump's latency too.
void Check_Stamp(const char * name, const WindowEvent * event, const PostTime & posted,
                 unsigned int age_ms)
{
	const unsigned int expected_ms = posted.Engine_Ms - age_ms;
	const int error_ms = event != nullptr ? static_cast<int>(event->Time_Ms - expected_ms) : 0;
	const unsigned int pump_latency_ms = timeGetTime() - posted.Engine_Ms;
	char what[240];
	std::snprintf(what, sizeof(what),
	              "%s stamped %u ms before it was posted: Time_Ms is %d ms off that engine time "
	              "(want within %d; the pump reached it %u ms after posting)",
	              name, age_ms, error_ms, CLOCK_TOLERANCE_MSEC, pump_latency_ms);
	Check(event != nullptr && std::abs(error_ms) <= CLOCK_TOLERANCE_MSEC, what);
}

// Time_Ms has to be on timeGetTime()'s clock: Keyboard::checkKeyRepeat() repeats any key whose
// down time is more than KEY_REPEAT_DELAY_MSEC before timeGetTime(). See docs/porting/event-clock.md.
void Check_Event_Clock(void * window, NSWindow * ns_window)
{
	const PostTime start = Post_Time_Now();
	const long long engine_minus_uptime_ms =
		static_cast<long long>(start.Engine_Ms) - std::llround(start.Uptime_Seconds * 1000.0);
	std::printf("       timeGetTime() - systemUptime = %lld ms on this machine\n",
	            engine_minus_uptime_ms);
	if (std::llabs(engine_minus_uptime_ms) <= KEY_REPEAT_DELAY_MSEC) {
		std::printf("NOTE   the two clocks agree here (the Mac has not slept since boot), so the\n"
		            "       Time_Ms checks below cannot tell the clocks apart on this run\n");
	}

	const double server_age_ms = (start.Uptime_Seconds - TheLatestServerTimestamp) * 1000.0;
	char what[200];
	std::snprintf(what, sizeof(what),
	              "AppKit's own event stamps are on systemUptime's clock (latest is %.0f ms old)",
	              server_age_ms);
	Check(TheLatestServerTimestamp > 0.0 && server_age_ms >= 0.0 && server_age_ms < 10000.0, what);

	PostTime posted = Post_Time_Now();
	Post_Key_At(ns_window, NSEventTypeKeyDown, 0x00, 'a', 0, posted.Uptime_Seconds);
	Post_Key(ns_window, NSEventTypeKeyUp, 0x00, 'a', 0);
	std::vector<WindowEvent> events = Drain(window);
	Check_Stamp("KEY_DOWN", Find_Event(events, WINDOW_EVENT_KEY_DOWN), posted, 0);
	Check_Stamp("TEXT", Find_Event(events, WINDOW_EVENT_TEXT), posted, 0);

	// An event that waited in the queue keeps its age: the stamp is when the key went down, not
	// when the pump got to it, so a held key starts repeating 333 ms after the real press.
	const unsigned int queued_age_ms = 500;
	posted = Post_Time_Now();
	Post_Key_At(ns_window, NSEventTypeKeyDown, 0x00, 'a', 0,
	            posted.Uptime_Seconds - queued_age_ms / 1000.0);
	Post_Key(ns_window, NSEventTypeKeyUp, 0x00, 'a', 0);
	events = Drain(window);
	Check_Stamp("KEY_DOWN", Find_Event(events, WINDOW_EVENT_KEY_DOWN), posted, queued_age_ms);

	posted = Post_Time_Now();
	Post_Mouse_At(ns_window, NSEventTypeLeftMouseDown, posted.Uptime_Seconds);
	events = Drain(window);
	Check_Stamp("MOUSE_DOWN", Find_Event(events, WINDOW_EVENT_MOUSE_DOWN), posted, 0);
	// Release the button the test pressed, so the next mode starts with nothing held.
	Post_Mouse_At(ns_window, NSEventTypeLeftMouseUp, [NSProcessInfo processInfo].systemUptime);
	Drain(window);
}

struct KeyCase
{
	const char * Name;
	unsigned short Virtual_Key;
	unichar Character;
	int Set1;
};

const KeyCase KEY_CASES[] = {
	{"Escape", 0x35, 0x1B, 0x01},
	{"Left arrow", 0x7B, NSLeftArrowFunctionKey, 0xCB},
	{"Right arrow", 0x7C, NSRightArrowFunctionKey, 0xCD},
	{"Up arrow", 0x7E, NSUpArrowFunctionKey, 0xC8},
	{"Down arrow", 0x7D, NSDownArrowFunctionKey, 0xD0},
	{"A", 0x00, 'a', 0x1E},
	{"Return", 0x24, '\r', 0x1C},
	{"Tab", 0x30, '\t', 0x0F},
	{"F1", 0x7A, NSF1FunctionKey, 0x3B},
};

bool Has_Key_Event(const std::vector<WindowEvent> & events, WindowEventType type, int set1)
{
	for (const WindowEvent & event : events) {
		if (event.Type == type && event.Scan_Code == set1) return true;
	}
	return false;
}

bool Has_Event(const std::vector<WindowEvent> & events, WindowEventType type)
{
	for (const WindowEvent & event : events) {
		if (event.Type == type) return true;
	}
	return false;
}

// What DX8Wrapper::Resize_And_Position_Window() does to a fullscreen window after creation:
// SetWindowPos(HWND_TOPMOST, 0, 0, width, height, 0), which the Win32 shim turns into these.
void Apply_Engine_Fullscreen_Placement(void * window)
{
	NSRect screen = [[NSScreen mainScreen] frame];
	Window_Set_Always_On_Top(window, true);
	Window_Set_Position(window, 0, 0);
	Window_Set_Client_Size(window, static_cast<int>(screen.size.width),
	                       static_cast<int>(screen.size.height));
}

// Returns false when the window never became key, which is a skip rather than a failure.
bool Run_Mode(const char * mode_name, bool fullscreen, bool engine_placement)
{
	std::printf("== %s\n", mode_name);
	WindowConfig config;
	config.Title = "cocoa key routing test";
	config.Width = 320;
	config.Height = 240;
	config.Fullscreen = fullscreen;
	void * window = Window_Create(config);
	Check(window != nullptr, "Window_Create()");
	if (window == nullptr) return true;

	if (engine_placement) Apply_Engine_Fullscreen_Placement(window);
	const bool active = Wait_Until_Active(window);
	if (!active) {
		std::printf("SKIP   the window never became key; no windowing session to route keys to\n");
		Window_Destroy(window);
		return false;
	}
	NSWindow * ns_window = Game_Window();
	Check([ns_window isKeyWindow], "the game window is the key window");
	Check([ns_window firstResponder] == [ns_window contentView],
	      "the game view is the first responder, so keys start at the view and not the window");

	for (const KeyCase & key : KEY_CASES) {
		const int unhandled_before = TheUnhandledKeyCount;
		Post_Key(ns_window, NSEventTypeKeyDown, key.Virtual_Key, key.Character, 0);
		Post_Key(ns_window, NSEventTypeKeyUp, key.Virtual_Key, key.Character, 0);
		const std::vector<WindowEvent> events = Drain(window);
		char what[160];
		std::snprintf(what, sizeof(what), "%s: KEY_DOWN and KEY_UP with set-1 0x%02X queued",
		              key.Name, key.Set1);
		Check(Has_Key_Event(events, WINDOW_EVENT_KEY_DOWN, key.Set1) &&
		          Has_Key_Event(events, WINDOW_EVENT_KEY_UP, key.Set1),
		      what);
		std::snprintf(what, sizeof(what), "%s: no unhandled-key (beep) path in AppKit", key.Name);
		Check(TheUnhandledKeyCount == unhandled_before, what);
	}

	{
		Post_Key(ns_window, NSEventTypeKeyDown, 0x00, 'a', 0);
		Post_Key(ns_window, NSEventTypeKeyUp, 0x00, 'a', 0);
		const std::vector<WindowEvent> events = Drain(window);
		bool text_a = false;
		for (const WindowEvent & event : events) {
			if (event.Type == WINDOW_EVENT_TEXT && event.Character == 'a') text_a = true;
		}
		Check(text_a, "A: WINDOW_EVENT_TEXT 'a' still queued");
	}

	{
		const int unhandled_before = TheUnhandledKeyCount;
		Post_Key(ns_window, NSEventTypeKeyDown, 0x0C, 'q', NSEventModifierFlagCommand);
		Post_Key(ns_window, NSEventTypeKeyUp, 0x0C, 'q', NSEventModifierFlagCommand);
		const std::vector<WindowEvent> events = Drain(window);
		Check(Has_Event(events, WINDOW_EVENT_CLOSE),
		      "Cmd-Q: the main menu's Quit item raised WINDOW_EVENT_CLOSE");
		Check(TheUnhandledKeyCount == unhandled_before, "Cmd-Q: no unhandled-key (beep) path");
	}

	Check_Event_Clock(window, ns_window);

	Window_Destroy(window);
	Drain(nullptr);
	return true;
}

} // namespace

int main(int argc, char ** argv)
{
	bool windowed_only = false;
	for (int i = 1; i < argc; ++i) {
		if (std::strcmp(argv[i], "--windowed-only") == 0) windowed_only = true;
	}
	@autoreleasepool {
		Install_Unhandled_Key_Counter();
		Install_Server_Timestamp_Monitor();
		const bool windowed_ran = Run_Mode("windowed", false, false);
		const bool fullscreen_ran = windowed_only ||
			(Run_Mode("fullscreen (borderless)", true, false) &&
			 Run_Mode("fullscreen as the engine places it (SetWindowPos HWND_TOPMOST)", true, true));
		if (!windowed_ran || !fullscreen_ran) {
			std::printf("SKIP: %d failure(s) before the skip\n", TheFailures);
			return TheFailures == 0 ? 77 : 1;
		}
	}
	std::printf("%d failure(s)\n", TheFailures);
	return TheFailures == 0 ? 0 : 1;
}
