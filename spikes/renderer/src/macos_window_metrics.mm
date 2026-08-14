// macOS window metrics probe: the numbers the paravirtualised CI runner cannot produce.
//
// The window spike (src/window_spike.cpp) asserts that a window can be created and presented to.
// It does not print what the window and its CAMetalLayer actually measure, and on a scale-1 CI
// runner the Retina arithmetic in platform_window_cocoa.mm is multiplied by 1.0, so a wrong
// factor cannot show up. This probe reports, for each phase of the window's life:
//
//   * NSScreen backingScaleFactor, frame and visibleFrame (so the Dock/menu-bar inset is visible);
//   * the NSWindow frame and the content view bounds, in points;
//   * convertRectToBacking() of the content view, i.e. the backing store in pixels;
//   * the CAMetalLayer contentsScale and drawableSize, which is what the seam sets;
//   * Window_Client_Size(), which is what the engine sees and passes to the renderer;
//   * the VkSurfaceCapabilitiesKHR currentExtent MoltenVK reports for that layer;
//   * the window's safeAreaInsets and the screen's auxiliaryTopLeftArea, which is how the notch
//     shows up, and whether the menu bar and Dock are still on screen in fullscreen.
//
// It also drains the event queue and prints every translated event, so real key presses can be
// checked against the set-1 scan codes in KeyScanCodes.h and mouse coordinates can be checked in
// a resized and a Retina window. With --inject it posts its own CGEvents through the window
// server (kCGHIDEventTap) instead of waiting for a human; those are real NSEvents with real
// virtual key codes, but they are synthesised, and the log says so.
//
//   ./zh-macos-window-metrics                       # phases with no input, ~1s each
//   ./zh-macos-window-metrics --inject              # also posts keys, mouse moves, clicks, wheel
//   ./zh-macos-window-metrics --wait-ms 20000       # holds each phase open for a human
//
// See docs/porting/macos-hardware-verification.md.

#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <ApplicationServices/ApplicationServices.h>

#define VK_USE_PLATFORM_METAL_EXT 1
#include <vulkan/vulkan.h>

#include "platform/platform_window.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

VkInstance Instance = VK_NULL_HANDLE;
VkPhysicalDevice Physical = VK_NULL_HANDLE;
VkSurfaceKHR Surface = VK_NULL_HANDLE;

bool Create_Vulkan(void * window)
{
	const char * extension_names[8] = {nullptr};
	const int extension_count =
	    WWPlatform::Window_Vulkan_Instance_Extensions(window, extension_names, 8);
	std::vector<const char *> enabled(extension_names, extension_names + extension_count);
	enabled.push_back("VK_KHR_portability_enumeration");

	VkApplicationInfo app{};
	app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	app.pApplicationName = "zh-macos-window-metrics";
	app.apiVersion = VK_API_VERSION_1_1;

	VkInstanceCreateInfo ici{};
	ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	ici.pApplicationInfo = &app;
	ici.enabledExtensionCount = static_cast<uint32_t>(enabled.size());
	ici.ppEnabledExtensionNames = enabled.data();
	ici.flags |= 0x00000001;	// VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR
	if (vkCreateInstance(&ici, nullptr, &Instance) != VK_SUCCESS) return false;

	if (!WWPlatform::Window_Create_Vulkan_Surface(window, Instance, &Surface)) return false;

	uint32_t device_count = 0;
	vkEnumeratePhysicalDevices(Instance, &device_count, nullptr);
	std::vector<VkPhysicalDevice> devices(device_count);
	if (device_count > 0) vkEnumeratePhysicalDevices(Instance, &device_count, devices.data());
	if (device_count == 0) return false;
	Physical = devices[0];

	VkPhysicalDeviceProperties properties{};
	vkGetPhysicalDeviceProperties(Physical, &properties);
	VkPhysicalDeviceDriverProperties driver{};
	driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
	VkPhysicalDeviceProperties2 properties2{};
	properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
	properties2.pNext = &driver;
	vkGetPhysicalDeviceProperties2(Physical, &properties2);
	std::printf("gpu: %s\n", properties.deviceName);
	std::printf("driver: %s / %s\n", driver.driverName, driver.driverInfo);
	std::printf("driverVersion: %u.%u.%u, apiVersion: %u.%u.%u\n",
	            VK_VERSION_MAJOR(properties.driverVersion),
	            VK_VERSION_MINOR(properties.driverVersion),
	            VK_VERSION_PATCH(properties.driverVersion),
	            VK_VERSION_MAJOR(properties.apiVersion), VK_VERSION_MINOR(properties.apiVersion),
	            VK_VERSION_PATCH(properties.apiVersion));
	return true;
}

std::string Surface_Extent()
{
	if (Physical == VK_NULL_HANDLE || Surface == VK_NULL_HANDLE) return "unavailable";
	VkSurfaceCapabilitiesKHR caps{};
	vkGetPhysicalDeviceSurfaceCapabilitiesKHR(Physical, Surface, &caps);
	return std::to_string(caps.currentExtent.width) + "x" +
	       std::to_string(caps.currentExtent.height) + " (min " +
	       std::to_string(caps.minImageExtent.width) + "x" +
	       std::to_string(caps.minImageExtent.height) + ", max " +
	       std::to_string(caps.maxImageExtent.width) + "x" +
	       std::to_string(caps.maxImageExtent.height) + ")";
}

void Print_Window_Server_Order(void * window);

void Print_Metrics(const char * phase, void * window)
{
	const WWPlatform::NativeSurface native = WWPlatform::Window_Native_Surface(window);
	CAMetalLayer * layer = static_cast<CAMetalLayer *>(native.Handle_A);
	NSWindow * ns_window = static_cast<NSWindow *>(native.Handle_B);
	NSView * view = [ns_window contentView];
	NSScreen * screen = [ns_window screen] != nil ? [ns_window screen] : [NSScreen mainScreen];

	int client_width = 0;
	int client_height = 0;
	WWPlatform::Window_Client_Size(window, client_width, client_height);

	const NSRect window_frame = [ns_window frame];
	const NSRect content_rect = [ns_window contentRectForFrameRect:window_frame];
	const NSRect bounds = [view bounds];
	const NSRect backing = [view convertRectToBacking:bounds];
	const NSRect screen_frame = [screen frame];
	const NSRect visible = [screen visibleFrame];
	const NSEdgeInsets insets = [ns_window contentView].safeAreaInsets;

	std::printf("\n=== phase: %s\n", phase);
	std::printf("  screen                  frame %.0fx%.0f at (%.0f,%.0f), visibleFrame "
	            "%.0fx%.0f at (%.0f,%.0f), backingScaleFactor %.2f\n",
	            screen_frame.size.width, screen_frame.size.height, screen_frame.origin.x,
	            screen_frame.origin.y, visible.size.width, visible.size.height, visible.origin.x,
	            visible.origin.y, [screen backingScaleFactor]);
	if (@available(macOS 12.0, *)) {
		const NSRect aux = [screen auxiliaryTopLeftArea];
		std::printf("  screen notch            auxiliaryTopLeftArea %.0fx%.0f at (%.0f,%.0f), "
		            "safeAreaInsets top %.0f left %.0f bottom %.0f right %.0f\n",
		            aux.size.width, aux.size.height, aux.origin.x, aux.origin.y,
		            [screen safeAreaInsets].top, [screen safeAreaInsets].left,
		            [screen safeAreaInsets].bottom, [screen safeAreaInsets].right);
	}
	std::printf("  window                  frame %.0fx%.0f at (%.0f,%.0f), contentRect "
	            "%.0fx%.0f, title \"%s\"\n",
	            window_frame.size.width, window_frame.size.height, window_frame.origin.x,
	            window_frame.origin.y, content_rect.size.width, content_rect.size.height,
	            [[ns_window title] UTF8String]);
	std::printf("  window state            visible %s, key %s, main %s, level %ld, styleMask "
	            "0x%lX, occlusion %s\n",
	            [ns_window isVisible] ? "yes" : "no", [ns_window isKeyWindow] ? "yes" : "no",
	            [ns_window isMainWindow] ? "yes" : "no", static_cast<long>([ns_window level]),
	            static_cast<unsigned long>([ns_window styleMask]),
	            ([ns_window occlusionState] & NSWindowOcclusionStateVisible) ? "visible"
	                                                                        : "occluded");
	std::printf("  window ordering         orderedIndex %ld of %lu on-screen window(s), app "
	            "active %s, menu bar visible %s\n",
	            static_cast<long>([[NSApp orderedWindows] indexOfObject:ns_window]),
	            static_cast<unsigned long>([[NSApp orderedWindows] count]),
	            [NSApp isActive] ? "yes" : "no", [NSMenu menuBarVisible] ? "yes" : "no");
	std::printf("  view bounds (points)    %.1fx%.1f\n", bounds.size.width, bounds.size.height);
	std::printf("  view backing (pixels)   %.1fx%.1f  <- convertRectToBacking\n",
	            backing.size.width, backing.size.height);
	std::printf("  window safeAreaInsets   top %.0f left %.0f bottom %.0f right %.0f\n",
	            insets.top, insets.left, insets.bottom, insets.right);
	std::printf("  Window_Client_Size()    %dx%d  <- what the engine and the renderer see\n",
	            client_width, client_height);
	std::printf("  layer contentsScale     %.2f\n", layer.contentsScale);
	std::printf("  layer drawableSize      %.0fx%.0f  <- what platform_window_cocoa.mm sets\n",
	            layer.drawableSize.width, layer.drawableSize.height);
	std::printf("  layer bounds (points)   %.1fx%.1f\n", layer.bounds.size.width,
	            layer.bounds.size.height);
	std::printf("  vk currentExtent        %s\n", Surface_Extent().c_str());
	std::printf("  backing/point ratio     %.2f x %.2f\n",
	            bounds.size.width > 0.0 ? backing.size.width / bounds.size.width : 0.0,
	            bounds.size.height > 0.0 ? backing.size.height / bounds.size.height : 0.0);
	std::fflush(stdout);
	Print_Window_Server_Order(window);
}

// Which windows the window server has in front of ours, and whether they overlap it. This is
// how "does the menu bar cover a borderless fullscreen window" is answered without a screenshot:
// the menu bar and the Dock are windows too, with their own kCGWindowLayer.
void Print_Window_Server_Order(void * window)
{
	const WWPlatform::NativeSurface native = WWPlatform::Window_Native_Surface(window);
	NSWindow * ns_window = static_cast<NSWindow *>(native.Handle_B);
	const CGWindowID own_id = static_cast<CGWindowID>([ns_window windowNumber]);
	const NSRect own_frame = [ns_window frame];
	const CGFloat primary_height = [[[NSScreen screens] objectAtIndex:0] frame].size.height;
	// The window server's rects are top-left origin, so ours is flipped to compare.
	const CGRect own_cg = CGRectMake(own_frame.origin.x,
	                                 primary_height - own_frame.origin.y - own_frame.size.height,
	                                 own_frame.size.width, own_frame.size.height);

	CFArrayRef list = CGWindowListCopyWindowInfo(
	    kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID);
	if (list == nullptr) {
		std::printf("  window server        CGWindowListCopyWindowInfo returned null\n");
		return;
	}
	std::printf("  window server        our window id %u, layer:\n", own_id);
	const CFIndex count = CFArrayGetCount(list);
	for (CFIndex i = 0; i < count; ++i) {
		NSDictionary * info =
		    static_cast<NSDictionary *>(const_cast<void *>(CFArrayGetValueAtIndex(list, i)));
		const CGWindowID window_id = [info[(id)kCGWindowNumber] unsignedIntValue];
		const int layer = [info[(id)kCGWindowLayer] intValue];
		NSString * owner = info[(id)kCGWindowOwnerName];
		CGRect bounds = CGRectZero;
		CGRectMakeWithDictionaryRepresentation(
		    static_cast<CFDictionaryRef>(info[(id)kCGWindowBounds]), &bounds);
		const bool overlaps = CGRectIntersectsRect(bounds, own_cg);
		const bool ours = window_id == own_id;
		// Only what matters is printed: ourselves, and anything in front of us that overlaps.
		if (!ours && !(overlaps && layer != 0)) continue;
		std::printf("                       %-24s id %-6u layer %-4d %.0fx%.0f at (%.0f,%.0f)%s\n",
		            owner != nil ? [owner UTF8String] : "?", window_id, layer, bounds.size.width,
		            bounds.size.height, bounds.origin.x, bounds.origin.y,
		            ours ? "  <- ours" : (overlaps ? "  overlaps ours, in front" : ""));
	}
	CFRelease(list);
	std::printf("  app presentation     NSApp presentationOptions 0x%lX\n",
	            static_cast<unsigned long>([NSApp presentationOptions]));
	std::fflush(stdout);
}

const char * Event_Name(WWPlatform::WindowEventType type)
{
	switch (type) {
		case WWPlatform::WINDOW_EVENT_CLOSE: return "close";
		case WWPlatform::WINDOW_EVENT_RESIZE: return "resize";
		case WWPlatform::WINDOW_EVENT_MOVE: return "move";
		case WWPlatform::WINDOW_EVENT_FOCUS_GAINED: return "focus-gained";
		case WWPlatform::WINDOW_EVENT_FOCUS_LOST: return "focus-lost";
		case WWPlatform::WINDOW_EVENT_MINIMISED: return "minimised";
		case WWPlatform::WINDOW_EVENT_RESTORED: return "restored";
		case WWPlatform::WINDOW_EVENT_KEY_DOWN: return "key-down";
		case WWPlatform::WINDOW_EVENT_KEY_UP: return "key-up";
		case WWPlatform::WINDOW_EVENT_TEXT: return "text";
		case WWPlatform::WINDOW_EVENT_MOUSE_MOVE: return "mouse-move";
		case WWPlatform::WINDOW_EVENT_MOUSE_DOWN: return "mouse-down";
		case WWPlatform::WINDOW_EVENT_MOUSE_UP: return "mouse-up";
		case WWPlatform::WINDOW_EVENT_MOUSE_WHEEL: return "mouse-wheel";
		case WWPlatform::WINDOW_EVENT_MOUSE_ENTER: return "mouse-enter";
		case WWPlatform::WINDOW_EVENT_MOUSE_LEAVE: return "mouse-leave";
		default: return "none";
	}
}

int Events_Seen = 0;
int Failures = 0;

// What the next injected event is expected to produce, so the log carries a verdict per event
// rather than a number the reader has to check by hand.
int Expected_Scan_Code = -1;
const char * Expected_Key_Name = "";
int Expected_Mouse_X = -1;
int Expected_Mouse_Y = -1;

void Check(bool ok, const std::string & detail)
{
	std::printf("  %-6s %s\n", ok ? "PASS" : "FAIL", detail.c_str());
	if (!ok) ++Failures;
}

void Drain(void * window)
{
	WWPlatform::WindowEvent event;
	while (WWPlatform::Window_Poll_Event(window, event)) {
		++Events_Seen;
		switch (event.Type) {
			case WWPlatform::WINDOW_EVENT_KEY_DOWN:
			case WWPlatform::WINDOW_EVENT_KEY_UP:
				std::printf("  event %-12s set-1 scan code 0x%02X%s modifiers 0x%X\n",
				            Event_Name(event.Type), event.Scan_Code,
				            event.Repeat ? " (repeat)" : "", event.Modifiers);
				if (event.Type == WWPlatform::WINDOW_EVENT_KEY_DOWN &&
				    Expected_Scan_Code >= 0) {
					char detail[128];
					std::snprintf(detail, sizeof(detail),
					              "%s: expected KEYSCAN 0x%02X, got 0x%02X",
					              Expected_Key_Name, Expected_Scan_Code, event.Scan_Code);
					Check(event.Scan_Code == Expected_Scan_Code, detail);
					Expected_Scan_Code = -1;
				}
				break;
			case WWPlatform::WINDOW_EVENT_TEXT:
				std::printf("  event %-12s U+%04X\n", Event_Name(event.Type), event.Character);
				break;
			case WWPlatform::WINDOW_EVENT_MOUSE_MOVE:
			case WWPlatform::WINDOW_EVENT_MOUSE_DOWN:
			case WWPlatform::WINDOW_EVENT_MOUSE_UP:
				std::printf("  event %-12s client (%d,%d) button %d clicks %d modifiers 0x%X\n",
				            Event_Name(event.Type), event.Mouse_X, event.Mouse_Y,
				            event.Mouse_Button, event.Click_Count, event.Modifiers);
				if (Expected_Mouse_X >= 0 && event.Type ==
				        WWPlatform::WINDOW_EVENT_MOUSE_MOVE) {
					char detail[160];
					std::snprintf(detail, sizeof(detail),
					              "mouse expected at client (%d,%d), got (%d,%d)",
					              Expected_Mouse_X, Expected_Mouse_Y, event.Mouse_X,
					              event.Mouse_Y);
					// One point of slack: the pointer lands on a pixel, not on a real.
					Check(std::abs(event.Mouse_X - Expected_Mouse_X) <= 1 &&
					          std::abs(event.Mouse_Y - Expected_Mouse_Y) <= 1,
					      detail);
					// Consumed: any further move is the machine's own cursor, not ours.
					Expected_Mouse_X = -1;
				}
				break;
			case WWPlatform::WINDOW_EVENT_MOUSE_WHEEL:
				std::printf("  event %-12s wheel delta %d at client (%d,%d)\n",
				            Event_Name(event.Type), event.Wheel_Delta, event.Mouse_X,
				            event.Mouse_Y);
				break;
			case WWPlatform::WINDOW_EVENT_RESIZE:
				std::printf("  event %-12s %dx%d (client points)\n", Event_Name(event.Type),
				            event.Width, event.Height);
				break;
			default:
				std::printf("  event %-12s\n", Event_Name(event.Type));
				break;
		}
	}
	std::fflush(stdout);
}

void Pump(void * window, int milliseconds)
{
	for (int elapsed = 0; elapsed < milliseconds; elapsed += 16) {
		WWPlatform::Window_Pump(window);
		Drain(window);
		usleep(16000);
	}
}

// A real NSEvent with a real virtual key code, delivered by the window server - but synthesised
// by this process, which is not the same thing as a human pressing the key. Posting needs the
// Accessibility TCC grant; without it the events silently never arrive, which the caller sees as
// "no events".
void Post_Key(const char * name, CGKeyCode key_code, CGEventFlags flags)
{
	std::printf("  inject key %s (kVK 0x%02X, flags 0x%llX)\n", name,
	            static_cast<unsigned>(key_code), static_cast<unsigned long long>(flags));
	CGEventRef down = CGEventCreateKeyboardEvent(nullptr, key_code, true);
	CGEventRef up = CGEventCreateKeyboardEvent(nullptr, key_code, false);
	if (flags != 0) {
		CGEventSetFlags(down, flags);
		CGEventSetFlags(up, flags);
	}
	CGEventPost(kCGHIDEventTap, down);
	usleep(40000);
	CGEventPost(kCGHIDEventTap, up);
	CFRelease(down);
	CFRelease(up);
	usleep(40000);
}

// Injection goes to whatever the window server thinks is focused, so it is only ever done while
// our own window is key: otherwise a Mac someone is using would receive the keystrokes.
bool Injection_Safe(void * window)
{
	const WWPlatform::NativeSurface native = WWPlatform::Window_Native_Surface(window);
	NSWindow * ns_window = static_cast<NSWindow *>(native.Handle_B);
	const bool safe = [NSApp isActive] && [ns_window isKeyWindow];
	if (!safe) {
		std::printf("  SKIP   injection: our window is not key (someone else is using this "
		            "Mac); no synthetic events posted\n");
		std::fflush(stdout);
	}
	return safe;
}

// A cursor position outside the client area, which is where the two possible readings of
// NSEvent.locationInWindow diverge: window coordinates (correct when the event has a window) and
// screen coordinates (what Cocoa supplies when [event window] is nil). Both readings are printed,
// so the log says which one the seam produced rather than only that a number looked odd.
void Post_Mouse_Global(void * window, double global_x, double global_y)
{
	const WWPlatform::NativeSurface native = WWPlatform::Window_Native_Surface(window);
	NSWindow * ns_window = static_cast<NSWindow *>(native.Handle_B);
	NSView * view = [ns_window contentView];
	const NSRect bounds = [view bounds];
	const CGFloat primary_height = [[[NSScreen screens] objectAtIndex:0] frame].size.height;
	const NSPoint on_screen = NSMakePoint(global_x, primary_height - global_y);
	const NSRect in_window =
	    [ns_window convertRectFromScreen:NSMakeRect(on_screen.x, on_screen.y, 1.0, 1.0)];
	const NSPoint in_view = [view convertPoint:in_window.origin fromView:nil];
	std::printf("  inject mouse to global (%.0f,%.0f): correct client would be (%.0f,%.0f), "
	            "screen-coordinates-as-window-coordinates would be (%.0f,%.0f)\n",
	            global_x, global_y, in_view.x, bounds.size.height - in_view.y, on_screen.x,
	            bounds.size.height - on_screen.y);
	std::fflush(stdout);
	Expected_Mouse_X = -1;
	CGEventRef move = CGEventCreateMouseEvent(nullptr, kCGEventMouseMoved,
	                                          CGPointMake(global_x, global_y), kCGMouseButtonLeft);
	CGEventPost(kCGHIDEventTap, move);
	CFRelease(move);
	usleep(80000);
}

// A mouse-moved event whose windowNumber is 0, i.e. the "event not associated with a window"
// case AppKit documents, where locationInWindow is in screen coordinates instead. This is posted
// into our own queue rather than through the HID tap, so it is deterministic, needs no TCC grant
// and does not touch the machine's real cursor or whatever window happens to be focused.
void Post_Nil_Window_Mouse(void * window, double client_x, double client_y)
{
	const WWPlatform::NativeSurface native = WWPlatform::Window_Native_Surface(window);
	NSWindow * ns_window = static_cast<NSWindow *>(native.Handle_B);
	NSView * view = [ns_window contentView];
	const NSRect bounds = [view bounds];
	const NSPoint in_view = NSMakePoint(client_x, bounds.size.height - client_y);
	const NSPoint in_window = [view convertPoint:in_view toView:nil];
	const NSRect on_screen =
	    [ns_window convertRectToScreen:NSMakeRect(in_window.x, in_window.y, 1.0, 1.0)];

	NSEvent * event = [NSEvent mouseEventWithType:NSEventTypeMouseMoved
	                                     location:on_screen.origin
	                                modifierFlags:0
	                                    timestamp:[[NSProcessInfo processInfo] systemUptime]
	                                 windowNumber:0
	                                      context:nil
	                                  eventNumber:0
	                                   clickCount:0
	                                     pressure:0.0f];
	std::printf("  post windowNumber 0 mouse-moved at screen (%.0f,%.0f): the client point it "
	            "stands for is (%.0f,%.0f)\n",
	            on_screen.origin.x, on_screen.origin.y, client_x, client_y);
	Expected_Mouse_X = static_cast<int>(client_x);
	Expected_Mouse_Y = static_cast<int>(client_y);
	[NSApp postEvent:event atStart:YES];
}

void Post_Mouse(void * window, double point_x, double point_y, bool click)
{
	const WWPlatform::NativeSurface native = WWPlatform::Window_Native_Surface(window);
	NSWindow * ns_window = static_cast<NSWindow *>(native.Handle_B);
	NSView * view = [ns_window contentView];
	// Cocoa view point (bottom-left origin) -> screen point -> CoreGraphics global point
	// (top-left origin, from the primary display's top edge).
	const NSRect bounds = [view bounds];
	const NSPoint in_view = NSMakePoint(point_x, bounds.size.height - point_y);
	const NSRect in_window = [view convertRect:NSMakeRect(in_view.x, in_view.y, 1.0, 1.0)
	                                   toView:nil];
	const NSRect on_screen = [ns_window convertRectToScreen:in_window];
	const CGFloat primary_height = [[[NSScreen screens] objectAtIndex:0] frame].size.height;
	const CGPoint global = CGPointMake(on_screen.origin.x, primary_height - on_screen.origin.y);
	std::printf("  inject mouse to client (%.0f,%.0f) = global (%.0f,%.0f)%s\n", point_x, point_y,
	            global.x, global.y, click ? " with a left click" : "");

	Expected_Mouse_X = static_cast<int>(point_x);
	Expected_Mouse_Y = static_cast<int>(point_y);
	CGEventRef move = CGEventCreateMouseEvent(nullptr, kCGEventMouseMoved, global,
	                                          kCGMouseButtonLeft);
	CGEventPost(kCGHIDEventTap, move);
	CFRelease(move);
	usleep(60000);
	if (click) {
		CGEventRef down = CGEventCreateMouseEvent(nullptr, kCGEventLeftMouseDown, global,
		                                          kCGMouseButtonLeft);
		CGEventRef up = CGEventCreateMouseEvent(nullptr, kCGEventLeftMouseUp, global,
		                                        kCGMouseButtonLeft);
		CGEventPost(kCGHIDEventTap, down);
		usleep(60000);
		CGEventPost(kCGHIDEventTap, up);
		CFRelease(down);
		CFRelease(up);
		usleep(60000);
	}
	CGEventRef wheel = CGEventCreateScrollWheelEvent(nullptr, kCGScrollEventUnitLine, 1, 1);
	CGEventPost(kCGHIDEventTap, wheel);
	CFRelease(wheel);
	usleep(60000);
}

// The named keys the report quotes, with the KEYSCAN_* value KeyScanCodes.h gives each one.
struct Injected_Key
{
	const char * Name;
	CGKeyCode Virtual_Key;
	int Expected_Set1;
};

// The expected values are the KEYSCAN_* constants in
// Core/GameEngine/Include/GameClient/KeyScanCodes.h, which are DirectInput DIK_* values: the
// arrow keys are the extended 0xCB/0xC8 codes, not the numeric-keypad 0x4B/0x48 ones.
const Injected_Key Injected_Keys[] = {
    {"A", 0, 0x1E},				// kVK_ANSI_A, KEYSCAN_A
    {"Z", 6, 0x2C},				// kVK_ANSI_Z, KEYSCAN_Z
    {"1", 18, 0x02},			// kVK_ANSI_1, KEYSCAN_1
    {"Space", 49, 0x39},		// kVK_Space, KEYSCAN_SPACE
    {"Return", 36, 0x1C},		// kVK_Return, KEYSCAN_RETURN
    {"Tab", 48, 0x0F},			// kVK_Tab, KEYSCAN_TAB
    {"LeftArrow", 123, 0xCB},	// kVK_LeftArrow, KEYSCAN_LEFTARROW
    {"UpArrow", 126, 0xC8},		// kVK_UpArrow, KEYSCAN_UPARROW
    {"F1", 122, 0x3B},			// kVK_F1, KEYSCAN_F1
    {"Escape", 53, 0x01},		// kVK_Escape, KEYSCAN_ESCAPE
};

} // namespace

int main(int argc, char ** argv)
{
	bool inject = false;
	int wait_ms = 1200;
	bool fullscreen_phase = true;
	for (int i = 1; i < argc; ++i) {
		if (std::strcmp(argv[i], "--inject") == 0) inject = true;
		else if (std::strcmp(argv[i], "--no-fullscreen") == 0) fullscreen_phase = false;
		else if (std::strcmp(argv[i], "--wait-ms") == 0 && i + 1 < argc) {
			wait_ms = std::atoi(argv[++i]);
		} else if (std::strcmp(argv[i], "--help") == 0) {
			std::printf("usage: %s [--inject] [--wait-ms N] [--no-fullscreen]\n", argv[0]);
			return 0;
		}
	}

	std::printf("window backend: %s\n", WWPlatform::Window_Backend_Name());
	std::printf("expected set-1 codes: ");
	for (const Injected_Key & key : Injected_Keys) {
		std::printf("%s=0x%02X ", key.Name, key.Expected_Set1);
	}
	std::printf("\n");

	WWPlatform::WindowConfig config;
	config.Title = "Zero Hour macOS metrics";
	config.Width = 800;
	config.Height = 600;
	config.Resizable = true;
	void * window = WWPlatform::Window_Create(config);
	if (window == nullptr) {
		std::fprintf(stderr, "Window_Create failed: %s\n", WWPlatform::Window_Last_Error());
		return 1;
	}
	if (!Create_Vulkan(window)) {
		std::fprintf(stderr, "Vulkan surface failed: %s\n", WWPlatform::Window_Last_Error());
		return 1;
	}

	Pump(window, 400);
	Print_Metrics("windowed 800x600 as created", window);
	if (inject && Injection_Safe(window)) {
		for (const Injected_Key & key : Injected_Keys) {
			Expected_Scan_Code = key.Expected_Set1;
			Expected_Key_Name = key.Name;
			Post_Key(key.Name, key.Virtual_Key, 0);
			Pump(window, 100);
		}
		Expected_Scan_Code = 0x1E;
		Expected_Key_Name = "Shift+A";
		Post_Key("Shift+A", 0, kCGEventFlagMaskShift);
		Pump(window, 100);
		Post_Mouse(window, 10.0, 10.0, false);
		Pump(window, 200);
		Post_Mouse(window, 400.0, 300.0, true);
		Pump(window, 200);
		Post_Mouse(window, 799.0, 599.0, false);
		Pump(window, 200);
		// Just left of the window, i.e. a negative client x under the Win32 convention.
		Post_Mouse_Global(window, 100.0, 400.0);
		Pump(window, 300);
	}
	// Needs no injection grant and no focus, so it runs whether or not the Mac is in use.
	if (inject) {
		Post_Nil_Window_Mouse(window, 200.0, 150.0);
		Pump(window, 200);
	}
	Pump(window, wait_ms);

	WWPlatform::Window_Set_Mode(window, 1024, 768, false);
	Pump(window, 400);
	Print_Metrics("windowed 1024x768 after Window_Set_Mode", window);
	if (inject && Injection_Safe(window)) {
		Post_Mouse(window, 512.0, 384.0, true);
		Pump(window, 200);
		Post_Mouse(window, 1023.0, 767.0, false);
		Pump(window, 200);
	}
	Pump(window, wait_ms);

	if (fullscreen_phase) {
		const NSRect screen_frame = [[NSScreen mainScreen] frame];
		WWPlatform::Window_Set_Mode(window, static_cast<int>(screen_frame.size.width),
		                            static_cast<int>(screen_frame.size.height), true);
		Pump(window, 600);
		Print_Metrics("borderless fullscreen via Window_Set_Mode(..., true)", window);
		if (inject && Injection_Safe(window)) {
			Post_Mouse(window, 10.0, 10.0, false);
			Pump(window, 200);
			Post_Mouse(window, static_cast<double>(screen_frame.size.width) - 1.0, 10.0, false);
			Pump(window, 200);
		}
		Pump(window, wait_ms);

		WWPlatform::Window_Set_Mode(window, 800, 600, false);
		Pump(window, 600);
		Print_Metrics("windowed 800x600 again after leaving fullscreen", window);
		Pump(window, wait_ms);
	}

	std::printf("\nevents seen: %d, checks failed: %d\n", Events_Seen, Failures);
	if (Surface != VK_NULL_HANDLE) vkDestroySurfaceKHR(Instance, Surface, nullptr);
	if (Instance != VK_NULL_HANDLE) vkDestroyInstance(Instance, nullptr);
	WWPlatform::Window_Destroy(window);
	return Failures == 0 ? 0 : 1;
}
