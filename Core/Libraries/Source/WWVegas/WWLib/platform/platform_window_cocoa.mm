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

/***********************************************************************************************
 *                                                                                             *
 *  Cocoa implementation of the window/event-loop/input seam: NSWindow + CAMetalLayer, with     *
 *  the Vulkan surface created through VK_EXT_metal_surface so MoltenVK presents straight to    *
 *  the layer.                                                                                  *
 *                                                                                             *
 *  ############################################################################               *
 *  ##  WRITTEN BLIND. NOT ONE LINE OF THIS FILE HAS EVER BEEN COMPILED OR RUN.  ##            *
 *  ############################################################################               *
 *                                                                                             *
 *  The session that wrote it had no macOS machine: it was authored on Linux, where nothing     *
 *  here can even be syntax-checked (no Cocoa headers, no clang Objective-C++ SDK). Treat every *
 *  line as a proposal. spikes/renderer/tools/macos-window-check.sh is the standalone check a    *
 *  Mac session runs to find out which parts are wrong; docs/porting/window-event-loop.md lists *
 *  the specific things most likely to be wrong and why.                                        *
 *                                                                                             *
 *  Design notes that are *not* guesses, because they come from Apple's and MoltenVK's          *
 *  documented contracts rather than from having run this:                                       *
 *                                                                                             *
 *   * MoltenVK presents to a CAMetalLayer. VK_EXT_metal_surface's                              *
 *     VkMetalSurfaceCreateInfoEXT takes `const CAMetalLayer *pLayer` directly, which is why    *
 *     the layer - not the NSWindow, not the NSView - is the handle this file has to own.        *
 *     (The older VK_MVK_macos_surface takes the NSView instead and is deprecated.)             *
 *   * AppKit requires the NSApplication to exist and to be on the process's main thread before  *
 *     any window is created, and requires event pumping to happen on that same thread.         *
 *   * The layer's drawableSize is in pixels while the window's frame is in points, so a Retina  *
 *     display needs the contentsScale applied or the swapchain comes out at half resolution.    *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#include "platform_window.h"

#if defined(__APPLE__) && !defined(_WIN32)

#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>

#include <deque>
#include <dlfcn.h>
#include <string>

namespace WWPlatform
{

namespace
{

/*
**	macOS virtual key code -> PC/AT set-1 scan code (the KEYSCAN_* values in
**	Core/GameEngine/Include/GameClient/KeyScanCodes.h). The Cocoa side is a *virtual* key code
**	in the HIToolbox kVK_* numbering, which - unlike Windows' VK_* - is positional, so this is
**	still a physical-key relabelling and not layout dependent.
**
**	The kVK_* values are written as literals with their names alongside rather than included
**	from <Carbon/HIToolbox/Events.h>, so that this file does not drag Carbon into the build.
**	scripts/ci/check-window-scancodes.py checks the set-1 column against KeyScanCodes.h; the
**	kVK_ column is checked in the standalone macOS check, since only a Mac has the header.
*/
struct ScanCodeMapping
{
	unsigned short Virtual_Key;
	int Set1;
	const char * Name;
};

const ScanCodeMapping SCAN_CODE_TABLE[] = {
	{0x35, 0x01, "KEYSCAN_ESCAPE"},			// kVK_Escape
	{0x12, 0x02, "KEYSCAN_1"},				// kVK_ANSI_1
	{0x13, 0x03, "KEYSCAN_2"},
	{0x14, 0x04, "KEYSCAN_3"},
	{0x15, 0x05, "KEYSCAN_4"},
	{0x17, 0x06, "KEYSCAN_5"},
	{0x16, 0x07, "KEYSCAN_6"},
	{0x1A, 0x08, "KEYSCAN_7"},
	{0x1C, 0x09, "KEYSCAN_8"},
	{0x19, 0x0A, "KEYSCAN_9"},
	{0x1D, 0x0B, "KEYSCAN_0"},
	{0x1B, 0x0C, "KEYSCAN_MINUS"},			// kVK_ANSI_Minus
	{0x18, 0x0D, "KEYSCAN_EQUALS"},			// kVK_ANSI_Equal
	{0x33, 0x0E, "KEYSCAN_BACK"},			// kVK_Delete, which is backspace
	{0x30, 0x0F, "KEYSCAN_TAB"},
	{0x0C, 0x10, "KEYSCAN_Q"},
	{0x0D, 0x11, "KEYSCAN_W"},
	{0x0E, 0x12, "KEYSCAN_E"},
	{0x0F, 0x13, "KEYSCAN_R"},
	{0x11, 0x14, "KEYSCAN_T"},
	{0x10, 0x15, "KEYSCAN_Y"},
	{0x20, 0x16, "KEYSCAN_U"},
	{0x22, 0x17, "KEYSCAN_I"},
	{0x1F, 0x18, "KEYSCAN_O"},
	{0x23, 0x19, "KEYSCAN_P"},
	{0x21, 0x1A, "KEYSCAN_LBRACKET"},
	{0x1E, 0x1B, "KEYSCAN_RBRACKET"},
	{0x24, 0x1C, "KEYSCAN_RETURN"},
	{0x3B, 0x1D, "KEYSCAN_LCONTROL"},		// kVK_Control
	{0x00, 0x1E, "KEYSCAN_A"},
	{0x01, 0x1F, "KEYSCAN_S"},
	{0x02, 0x20, "KEYSCAN_D"},
	{0x03, 0x21, "KEYSCAN_F"},
	{0x05, 0x22, "KEYSCAN_G"},
	{0x04, 0x23, "KEYSCAN_H"},
	{0x26, 0x24, "KEYSCAN_J"},
	{0x28, 0x25, "KEYSCAN_K"},
	{0x25, 0x26, "KEYSCAN_L"},
	{0x29, 0x27, "KEYSCAN_SEMICOLON"},
	{0x27, 0x28, "KEYSCAN_APOSTROPHE"},		// kVK_ANSI_Quote
	{0x32, 0x29, "KEYSCAN_GRAVE"},
	{0x38, 0x2A, "KEYSCAN_LSHIFT"},			// kVK_Shift
	{0x2A, 0x2B, "KEYSCAN_BACKSLASH"},
	{0x06, 0x2C, "KEYSCAN_Z"},
	{0x07, 0x2D, "KEYSCAN_X"},
	{0x08, 0x2E, "KEYSCAN_C"},
	{0x09, 0x2F, "KEYSCAN_V"},
	{0x0B, 0x30, "KEYSCAN_B"},
	{0x2D, 0x31, "KEYSCAN_N"},
	{0x2E, 0x32, "KEYSCAN_M"},
	{0x2B, 0x33, "KEYSCAN_COMMA"},
	{0x2F, 0x34, "KEYSCAN_PERIOD"},
	{0x2C, 0x35, "KEYSCAN_SLASH"},
	{0x3C, 0x36, "KEYSCAN_RSHIFT"},			// kVK_RightShift
	{0x43, 0x37, "KEYSCAN_NUMPADSTAR"},		// kVK_ANSI_KeypadMultiply
	{0x3A, 0x38, "KEYSCAN_LALT"},			// kVK_Option
	{0x31, 0x39, "KEYSCAN_SPACE"},
	{0x39, 0x3A, "KEYSCAN_CAPSLOCK"},
	{0x7A, 0x3B, "KEYSCAN_F1"},
	{0x78, 0x3C, "KEYSCAN_F2"},
	{0x63, 0x3D, "KEYSCAN_F3"},
	{0x76, 0x3E, "KEYSCAN_F4"},
	{0x60, 0x3F, "KEYSCAN_F5"},
	{0x61, 0x40, "KEYSCAN_F6"},
	{0x62, 0x41, "KEYSCAN_F7"},
	{0x64, 0x42, "KEYSCAN_F8"},
	{0x65, 0x43, "KEYSCAN_F9"},
	{0x6D, 0x44, "KEYSCAN_F10"},
	{0x47, 0x45, "KEYSCAN_NUMLOCK"},		// kVK_ANSI_KeypadClear; macOS has no Num Lock
	{0x6B, 0x46, "KEYSCAN_SCROLL"},			// kVK_F14, which is where Scroll Lock sits
	{0x59, 0x47, "KEYSCAN_NUMPAD7"},
	{0x5B, 0x48, "KEYSCAN_NUMPAD8"},
	{0x5C, 0x49, "KEYSCAN_NUMPAD9"},
	{0x4E, 0x4A, "KEYSCAN_NUMPADMINUS"},
	{0x56, 0x4B, "KEYSCAN_NUMPAD4"},
	{0x57, 0x4C, "KEYSCAN_NUMPAD5"},
	{0x58, 0x4D, "KEYSCAN_NUMPAD6"},
	{0x45, 0x4E, "KEYSCAN_NUMPADPLUS"},
	{0x53, 0x4F, "KEYSCAN_NUMPAD1"},
	{0x54, 0x50, "KEYSCAN_NUMPAD2"},
	{0x55, 0x51, "KEYSCAN_NUMPAD3"},
	{0x52, 0x52, "KEYSCAN_NUMPAD0"},
	{0x41, 0x53, "KEYSCAN_NUMPADPERIOD"},
	{0x0A, 0x56, "KEYSCAN_OEM_102"},		// kVK_ISO_Section
	{0x67, 0x57, "KEYSCAN_F11"},
	{0x6F, 0x58, "KEYSCAN_F12"},
	{0x68, 0x70, "KEYSCAN_KANA"},			// kVK_JIS_Kana
	{0x5D, 0x7D, "KEYSCAN_YEN"},			// kVK_JIS_Yen
	{0x66, 0x94, "KEYSCAN_KANJI"},			// kVK_JIS_Eisu
	{0x4C, 0x9C, "KEYSCAN_NUMPADENTER"},	// kVK_ANSI_KeypadEnter
	{0x3E, 0x9D, "KEYSCAN_RCONTROL"},		// kVK_RightControl
	{0x4B, 0xB5, "KEYSCAN_NUMPADSLASH"},
	{0x69, 0xB7, "KEYSCAN_SYSRQ"},			// kVK_F13, which is where Print Screen sits
	{0x3D, 0xB8, "KEYSCAN_RALT"},			// kVK_RightOption
	{0x73, 0xC7, "KEYSCAN_HOME"},
	{0x7E, 0xC8, "KEYSCAN_UPARROW"},
	{0x74, 0xC9, "KEYSCAN_PGUP"},
	{0x7B, 0xCB, "KEYSCAN_LEFTARROW"},
	{0x7C, 0xCD, "KEYSCAN_RIGHTARROW"},
	{0x77, 0xCF, "KEYSCAN_END"},
	{0x7D, 0xD0, "KEYSCAN_DOWNARROW"},
	{0x79, 0xD1, "KEYSCAN_PGDN"},
	{0x72, 0xD2, "KEYSCAN_INSERT"},			// kVK_Help, the Insert position on a PC keyboard
	{0x75, 0xD3, "KEYSCAN_DELETE"},			// kVK_ForwardDelete
};

const int SCAN_CODE_COUNT =
	static_cast<int>(sizeof(SCAN_CODE_TABLE) / sizeof(SCAN_CODE_TABLE[0]));

// A JIS keyboard's non-ANSI keys beyond Kana/Yen/Eisu, and the PC keys macOS has no key for
// at all (kVK has no Num Lock, no Pause, no Menu), are absent above rather than guessed at.
// KEYSCAN_CONVERT, KEYSCAN_NOCONVERT and KEYSCAN_CIRCUMFLEX are the three the table does not
// produce; the doc lists them as a known gap.

int Set1_From_Virtual_Key(unsigned short key)
{
	for (int i = 0; i < SCAN_CODE_COUNT; ++i) {
		if (SCAN_CODE_TABLE[i].Virtual_Key == key) return SCAN_CODE_TABLE[i].Set1;
	}
	return 0;
}

unsigned int Modifiers_From_Cocoa(NSEventModifierFlags flags)
{
	unsigned int out = WINDOW_MODIFIER_NONE;
	if ((flags & NSEventModifierFlagShift) != 0) out |= WINDOW_MODIFIER_SHIFT;
	if ((flags & NSEventModifierFlagControl) != 0) out |= WINDOW_MODIFIER_CONTROL;
	// Option is what the game's Alt bindings have to be, since Command belongs to the OS.
	if ((flags & NSEventModifierFlagOption) != 0) out |= WINDOW_MODIFIER_ALT;
	if ((flags & NSEventModifierFlagCapsLock) != 0) out |= WINDOW_MODIFIER_CAPS_LOCK;
	return out;
}

/*
**	VK_EXT_metal_surface, declared here rather than included, so that this file needs no
**	Vulkan headers and no link-time dependency on the loader: the entry point is resolved
**	through vkGetInstanceProcAddr, which is itself found with dlsym() in whatever loader the
**	process already has (libvulkan.dylib, or MoltenVK linked directly).
*/
typedef void * VkInstanceOpaque;
typedef unsigned long long VkSurfaceHandle;
typedef void (*PFN_vkVoidFunction_Local)(void);
typedef PFN_vkVoidFunction_Local (*PFN_vkGetInstanceProcAddr_Local)(VkInstanceOpaque,
                                                                    const char *);

struct VkMetalSurfaceCreateInfoEXT_Local
{
	int sType;						// VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT
	const void * pNext;
	unsigned int flags;
	const void * pLayer;			// const CAMetalLayer *
};

const int VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT_Local = 1000217000;

typedef int (*PFN_vkCreateMetalSurfaceEXT_Local)(VkInstanceOpaque,
                                                 const VkMetalSurfaceCreateInfoEXT_Local *,
                                                 const void *, VkSurfaceHandle *);

struct WindowState
{
	NSWindow * Window;
	NSView * View;
	CAMetalLayer * Layer;
	std::deque<WindowEvent> Queue;
	NSEventModifierFlags Last_Modifier_Flags;
	bool Key_Down[256];
	bool Active;
	bool Minimised;
	bool Closed;
	bool Cursor_Clipped;
	int Width;
	int Height;

	WindowState()
		: Window(nil), View(nil), Layer(nil), Last_Modifier_Flags(0), Active(false),
		  Minimised(false), Closed(false), Cursor_Clipped(false), Width(0), Height(0)
	{
		for (int i = 0; i < 256; ++i) Key_Down[i] = false;
	}
};

WindowState * TheWindow = nullptr;
std::string TheLastError;

WindowState * State(void * window)
{
	if (window == nullptr || window != TheWindow) return nullptr;
	return TheWindow;
}

unsigned int Now_Ms()
{
	// Same clock the events are stamped with: NSEvent.timestamp is seconds since boot.
	return static_cast<unsigned int>([NSProcessInfo processInfo].systemUptime * 1000.0);
}

/*
**	Cocoa's origin is bottom-left and the engine's, like Win32's, is top-left, so every mouse
**	coordinate is flipped here rather than at the call sites.
*/
void Mouse_Position_In_Client(WindowState * state, NSEvent * event, int & x, int & y)
{
	NSPoint point = [state->View convertPoint:[event locationInWindow] fromView:nil];
	NSRect bounds = [state->View bounds];
	x = static_cast<int>(point.x);
	y = static_cast<int>(bounds.size.height - point.y);
}

void Push(WindowState * state, const WindowEvent & event)
{
	state->Queue.push_back(event);
}

void Translate_Text(WindowState * state, NSEvent * event, unsigned int time_ms)
{
	NSString * characters = [event characters];
	for (NSUInteger i = 0; i < [characters length]; ++i) {
		unichar unit = [characters characterAtIndex:i];
		// Skip the private-use area AppKit puts the function/arrow keys in: those are
		// delivered as key events, not as text.
		if (unit >= 0xF700 && unit <= 0xF8FF) continue;
		if (unit < 0x20 && unit != '\r' && unit != '\t' && unit != 0x08) continue;
		WindowEvent out;
		out.Type = WINDOW_EVENT_TEXT;
		out.Character = unit;
		out.Modifiers = Modifiers_From_Cocoa([event modifierFlags]);
		out.Time_Ms = time_ms;
		Push(state, out);
	}
}

void Translate_Flags_Changed(WindowState * state, NSEvent * event, unsigned int time_ms)
{
	// Cocoa does not send key down/up for the modifier keys; it sends one flagsChanged whose
	// keyCode names the key that moved. Whether it went down or up has to be recovered from
	// the new flag set.
	const NSEventModifierFlags flags = [event modifierFlags];
	const unsigned short key = [event keyCode];
	const int set1 = Set1_From_Virtual_Key(key);
	if (set1 == 0) return;

	NSEventModifierFlags mask = 0;
	switch (key) {
		case 0x38: case 0x3C: mask = NSEventModifierFlagShift; break;		// Shift
		case 0x3B: case 0x3E: mask = NSEventModifierFlagControl; break;		// Control
		case 0x3A: case 0x3D: mask = NSEventModifierFlagOption; break;		// Option
		case 0x39: mask = NSEventModifierFlagCapsLock; break;
		default: return;
	}
	const bool down = (flags & mask) != 0;
	// With both Shifts held, releasing one leaves the flag set, so the per-key state is
	// tracked rather than derived from the flag alone.
	if (state->Key_Down[set1 & 0xFF] == down && mask != NSEventModifierFlagCapsLock) return;
	state->Key_Down[set1 & 0xFF] = down;
	state->Last_Modifier_Flags = flags;

	WindowEvent out;
	out.Type = down ? WINDOW_EVENT_KEY_DOWN : WINDOW_EVENT_KEY_UP;
	out.Scan_Code = set1;
	out.Modifiers = Modifiers_From_Cocoa(flags);
	out.Time_Ms = time_ms;
	Push(state, out);
}

void Translate(WindowState * state, NSEvent * event)
{
	const unsigned int time_ms = static_cast<unsigned int>([event timestamp] * 1000.0);
	WindowEvent out;
	out.Time_Ms = time_ms;

	switch ([event type]) {
		case NSEventTypeKeyDown: {
			const int set1 = Set1_From_Virtual_Key([event keyCode]);
			if (set1 != 0) {
				state->Key_Down[set1 & 0xFF] = true;
				out.Type = WINDOW_EVENT_KEY_DOWN;
				out.Scan_Code = set1;
				out.Repeat = [event isARepeat] ? true : false;
				out.Modifiers = Modifiers_From_Cocoa([event modifierFlags]);
				Push(state, out);
			}
			Translate_Text(state, event, time_ms);
			return;
		}

		case NSEventTypeKeyUp: {
			const int set1 = Set1_From_Virtual_Key([event keyCode]);
			if (set1 == 0) return;
			state->Key_Down[set1 & 0xFF] = false;
			out.Type = WINDOW_EVENT_KEY_UP;
			out.Scan_Code = set1;
			out.Modifiers = Modifiers_From_Cocoa([event modifierFlags]);
			Push(state, out);
			return;
		}

		case NSEventTypeFlagsChanged:
			Translate_Flags_Changed(state, event, time_ms);
			return;

		case NSEventTypeMouseMoved:
		case NSEventTypeLeftMouseDragged:
		case NSEventTypeRightMouseDragged:
		case NSEventTypeOtherMouseDragged:
			out.Type = WINDOW_EVENT_MOUSE_MOVE;
			Mouse_Position_In_Client(state, event, out.Mouse_X, out.Mouse_Y);
			out.Modifiers = Modifiers_From_Cocoa([event modifierFlags]);
			Push(state, out);
			return;

		case NSEventTypeLeftMouseDown:
		case NSEventTypeRightMouseDown:
		case NSEventTypeOtherMouseDown:
		case NSEventTypeLeftMouseUp:
		case NSEventTypeRightMouseUp:
		case NSEventTypeOtherMouseUp: {
			const NSEventType type = [event type];
			const bool down = type == NSEventTypeLeftMouseDown ||
			                  type == NSEventTypeRightMouseDown ||
			                  type == NSEventTypeOtherMouseDown;
			out.Type = down ? WINDOW_EVENT_MOUSE_DOWN : WINDOW_EVENT_MOUSE_UP;
			if (type == NSEventTypeRightMouseDown || type == NSEventTypeRightMouseUp) {
				out.Mouse_Button = MOUSE_BUTTON_RIGHT;
			} else if (type == NSEventTypeOtherMouseDown || type == NSEventTypeOtherMouseUp) {
				out.Mouse_Button = MOUSE_BUTTON_MIDDLE;
			} else {
				out.Mouse_Button = MOUSE_BUTTON_LEFT;
			}
			out.Click_Count = static_cast<int>([event clickCount]);
			Mouse_Position_In_Client(state, event, out.Mouse_X, out.Mouse_Y);
			out.Modifiers = Modifiers_From_Cocoa([event modifierFlags]);
			Push(state, out);
			return;
		}

		case NSEventTypeScrollWheel:
			out.Type = WINDOW_EVENT_MOUSE_WHEEL;
			Mouse_Position_In_Client(state, event, out.Mouse_X, out.Mouse_Y);
			// scrollingDeltaY is in lines for a wheel and in points for a trackpad; either
			// way one wheel notch is scaled to WM_MOUSEWHEEL's WHEEL_DELTA of 120.
			out.Wheel_Delta = static_cast<int>([event scrollingDeltaY] * 120.0);
			out.Modifiers = Modifiers_From_Cocoa([event modifierFlags]);
			if (out.Wheel_Delta != 0) Push(state, out);
			return;

		default:
			return;
	}
}

void Sync_Window_State(WindowState * state)
{
	const bool active = [state->Window isKeyWindow] ? true : false;
	if (active != state->Active) {
		state->Active = active;
		WindowEvent out;
		out.Type = active ? WINDOW_EVENT_FOCUS_GAINED : WINDOW_EVENT_FOCUS_LOST;
		out.Time_Ms = Now_Ms();
		Push(state, out);
	}
	const bool minimised = [state->Window isMiniaturized] ? true : false;
	if (minimised != state->Minimised) {
		state->Minimised = minimised;
		WindowEvent out;
		out.Type = minimised ? WINDOW_EVENT_MINIMISED : WINDOW_EVENT_RESTORED;
		out.Time_Ms = Now_Ms();
		Push(state, out);
	}
	if (![state->Window isVisible] && !state->Closed) {
		// The close button removes the window rather than sending anything this loop can
		// see, so a vanished window is reported as WINDOW_EVENT_CLOSE exactly once.
		state->Closed = true;
		WindowEvent out;
		out.Type = WINDOW_EVENT_CLOSE;
		out.Time_Ms = Now_Ms();
		Push(state, out);
	}
	NSRect bounds = [state->View bounds];
	const int width = static_cast<int>(bounds.size.width);
	const int height = static_cast<int>(bounds.size.height);
	if (width != state->Width || height != state->Height) {
		state->Width = width;
		state->Height = height;
		const CGFloat scale = [state->Window backingScaleFactor];
		state->Layer.drawableSize = CGSizeMake(width * scale, height * scale);
		WindowEvent out;
		out.Type = WINDOW_EVENT_RESIZE;
		out.Width = width;
		out.Height = height;
		out.Time_Ms = Now_Ms();
		Push(state, out);
	}
}

} // namespace

void * Window_Create(const WindowConfig & config)
{
	if (TheWindow != nullptr) {
		TheLastError = "a window already exists; the seam supports one window";
		return nullptr;
	}
	if (![NSThread isMainThread]) {
		TheLastError = "Window_Create must be called on the main thread (AppKit requirement)";
		return nullptr;
	}

	// A process that was not launched from a .app bundle has no NSApplication and no
	// activation policy, and a window created without both never becomes key.
	[NSApplication sharedApplication];
	[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
	[NSApp finishLaunching];

	const NSRect frame = NSMakeRect(0, 0, config.Width, config.Height);
	NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
	                          NSWindowStyleMaskMiniaturizable;
	if (config.Resizable) style |= NSWindowStyleMaskResizable;
	if (config.Fullscreen) style = NSWindowStyleMaskBorderless;

	NSWindow * window = [[NSWindow alloc] initWithContentRect:frame
	                                               styleMask:style
	                                                 backing:NSBackingStoreBuffered
	                                                   defer:NO];
	if (window == nil) {
		TheLastError = "NSWindow initWithContentRect returned nil";
		return nullptr;
	}
	[window setTitle:[NSString stringWithUTF8String:
	                      config.Title != nullptr ? config.Title : ""]];
	[window setAcceptsMouseMovedEvents:YES];
	[window setRestorable:NO];
	[window center];

	NSView * view = [[NSView alloc] initWithFrame:frame];
	CAMetalLayer * layer = [CAMetalLayer layer];
	if (layer == nil) {
		TheLastError = "CAMetalLayer layer returned nil (no Metal device?)";
		return nullptr;
	}
	const CGFloat scale = [window backingScaleFactor];
	layer.contentsScale = scale;
	layer.drawableSize = CGSizeMake(config.Width * scale, config.Height * scale);
	// The renderer owns the drawable; without this MoltenVK cannot acquire one.
	layer.framebufferOnly = YES;
	[view setWantsLayer:YES];
	[view setLayer:layer];
	[window setContentView:view];
	[window makeFirstResponder:view];

	if (config.Fullscreen) {
		[window setLevel:NSMainMenuWindowLevel + 1];
		[window setFrame:[[NSScreen mainScreen] frame] display:YES];
	}

	TheWindow = new WindowState();
	TheWindow->Window = window;
	TheWindow->View = view;
	TheWindow->Layer = layer;
	TheWindow->Width = config.Width;
	TheWindow->Height = config.Height;

	[window makeKeyAndOrderFront:nil];
	[NSApp activateIgnoringOtherApps:YES];
	if (config.Hide_System_Cursor) [NSCursor hide];

	TheLastError.clear();
	return TheWindow;
}

void Window_Destroy(void * window)
{
	WindowState * state = State(window);
	if (state == nullptr) return;
	[state->Window orderOut:nil];
	[state->Window close];
	delete state;
	TheWindow = nullptr;
}

void Window_Pump(void * window)
{
	WindowState * state = State(window);
	if (state == nullptr) return;
	// distantPast, so this never blocks: the game loop, not AppKit, owns the frame rate.
	// This is the direct replacement for PeekMessage/GetMessage/DispatchMessage, and unlike
	// [NSApp run] it hands control straight back.
	for (;;) {
		NSEvent * event = [NSApp nextEventMatchingMask:NSEventMaskAny
		                                     untilDate:[NSDate distantPast]
		                                        inMode:NSDefaultRunLoopMode
		                                       dequeue:YES];
		if (event == nil) break;
		Translate(state, event);
		// Still forwarded to AppKit, so the title bar, the menu and Cmd-Q keep working.
		[NSApp sendEvent:event];
	}
	Sync_Window_State(state);
}

bool Window_Poll_Event(void * window, WindowEvent & event)
{
	WindowState * state = State(window);
	if (state == nullptr) return false;
	Window_Pump(window);
	if (state->Queue.empty()) return false;
	event = state->Queue.front();
	state->Queue.pop_front();
	return true;
}

bool Window_Is_Minimised(void * window)
{
	WindowState * state = State(window);
	return state != nullptr && state->Minimised;
}

bool Window_Is_Active(void * window)
{
	WindowState * state = State(window);
	return state != nullptr && state->Active;
}

void Window_Client_Size(void * window, int & width, int & height)
{
	WindowState * state = State(window);
	if (state == nullptr) {
		width = 0;
		height = 0;
		return;
	}
	NSRect bounds = [state->View bounds];
	width = static_cast<int>(bounds.size.width);
	height = static_cast<int>(bounds.size.height);
}

bool Window_Set_Mode(void * window, int width, int height, bool fullscreen)
{
	WindowState * state = State(window);
	if (state == nullptr) return false;
	if (fullscreen) {
		[state->Window setStyleMask:NSWindowStyleMaskBorderless];
		[state->Window setFrame:[[NSScreen mainScreen] frame] display:YES];
	} else {
		[state->Window setStyleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
		                            NSWindowStyleMaskMiniaturizable];
		[state->Window setContentSize:NSMakeSize(width, height)];
		[state->Window center];
	}
	NSRect bounds = [state->View bounds];
	const CGFloat scale = [state->Window backingScaleFactor];
	state->Layer.drawableSize =
		CGSizeMake(bounds.size.width * scale, bounds.size.height * scale);
	return true;
}

void Window_Show(void * window, bool show)
{
	WindowState * state = State(window);
	if (state == nullptr) return;
	if (show) {
		[state->Window makeKeyAndOrderFront:nil];
	} else {
		[state->Window orderOut:nil];
	}
}

void Window_Set_Title(void * window, const char * title)
{
	WindowState * state = State(window);
	if (state == nullptr || title == nullptr) return;
	[state->Window setTitle:[NSString stringWithUTF8String:title]];
}

void Window_Set_Cursor_Clip(void * window, bool clip)
{
	WindowState * state = State(window);
	if (state == nullptr) return;
	// macOS has no ClipCursor. The closest thing is dissociating the pointer from the mouse
	// so it cannot leave, which is what CGAssociateMouseAndMouseCursorPosition(false) does -
	// and it also stops the pointer moving at all, so the game must then draw its own.
	state->Cursor_Clipped = clip;
	CGAssociateMouseAndMouseCursorPosition(clip ? false : true);
}

void Window_Warp_Cursor(void * window, int x, int y)
{
	WindowState * state = State(window);
	if (state == nullptr) return;
	// CGWarpMouseCursorPosition is in global, top-left-origin display coordinates, so the
	// client point is converted through the window's frame rather than passed through.
	NSRect content = [state->Window convertRectToScreen:[state->View frame]];
	NSRect screen = [[NSScreen mainScreen] frame];
	const CGFloat global_x = content.origin.x + x;
	const CGFloat global_y = screen.size.height - (content.origin.y + content.size.height) + y;
	CGWarpMouseCursorPosition(CGPointMake(global_x, global_y));
}

void Window_Show_System_Cursor(void * window, bool show)
{
	if (State(window) == nullptr) return;
	if (show) {
		[NSCursor unhide];
	} else {
		[NSCursor hide];
	}
}

bool Window_Key_Is_Down(void * window, int scan_code)
{
	WindowState * state = State(window);
	if (state == nullptr || scan_code <= 0 || scan_code > 0xFF) return false;
	return state->Key_Down[scan_code];
}

unsigned int Window_Modifier_State(void * window)
{
	WindowState * state = State(window);
	if (state == nullptr) return WINDOW_MODIFIER_NONE;
	return Modifiers_From_Cocoa([NSEvent modifierFlags]);
}

int Window_Vulkan_Instance_Extensions(void * window, const char ** names, int max_names)
{
	if (State(window) == nullptr) return -1;
	// VK_EXT_metal_surface, not the deprecated VK_MVK_macos_surface: the former takes the
	// CAMetalLayer this file owns.
	static const char * const REQUIRED[] = {"VK_KHR_surface", "VK_EXT_metal_surface"};
	const int count = static_cast<int>(sizeof(REQUIRED) / sizeof(REQUIRED[0]));
	if (names == nullptr) return count;
	if (max_names < count) return -1;
	for (int i = 0; i < count; ++i) names[i] = REQUIRED[i];
	return count;
}

bool Window_Create_Vulkan_Surface(void * window, void * vk_instance, void * out_vk_surface)
{
	WindowState * state = State(window);
	if (state == nullptr || vk_instance == nullptr || out_vk_surface == nullptr) return false;

	PFN_vkGetInstanceProcAddr_Local get_proc =
		reinterpret_cast<PFN_vkGetInstanceProcAddr_Local>(
			dlsym(RTLD_DEFAULT, "vkGetInstanceProcAddr"));
	if (get_proc == nullptr) {
		TheLastError = "vkGetInstanceProcAddr not found in this process; is the Vulkan "
		               "loader or MoltenVK linked?";
		return false;
	}
	PFN_vkCreateMetalSurfaceEXT_Local create =
		reinterpret_cast<PFN_vkCreateMetalSurfaceEXT_Local>(
			get_proc(vk_instance, "vkCreateMetalSurfaceEXT"));
	if (create == nullptr) {
		TheLastError = "vkCreateMetalSurfaceEXT unavailable; the instance must enable "
		               "VK_EXT_metal_surface";
		return false;
	}

	VkMetalSurfaceCreateInfoEXT_Local info;
	info.sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT_Local;
	info.pNext = nullptr;
	info.flags = 0;
	info.pLayer = state->Layer;

	VkSurfaceHandle * surface = static_cast<VkSurfaceHandle *>(out_vk_surface);
	const int result = create(vk_instance, &info, nullptr, surface);
	if (result != 0) {
		TheLastError = "vkCreateMetalSurfaceEXT failed (VkResult " + std::to_string(result) + ")";
		return false;
	}
	return true;
}

NativeSurface Window_Native_Surface(void * window)
{
	NativeSurface surface;
	WindowState * state = State(window);
	if (state == nullptr) return surface;
	surface.Kind = NATIVE_SURFACE_METAL_LAYER;
	surface.Handle_A = state->Layer;
	surface.Handle_B = state->Window;
	return surface;
}

const char * Window_Backend_Name()
{
	return "cocoa";
}

const char * Window_Last_Error()
{
	return TheLastError.c_str();
}

}	// namespace WWPlatform

#endif // __APPLE__ && !_WIN32
