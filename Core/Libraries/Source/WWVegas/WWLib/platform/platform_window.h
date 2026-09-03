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
 *                 Project Name : Westwood Library                                             *
 *                                                                                             *
 *  Window, event loop and input for the platforms that have no HWND, no WndProc and no        *
 *  PeekMessage. This is the seam the engine's Win32 window layer is replaced by: one window,  *
 *  one event queue that is *pulled* rather than dispatched to callbacks, and a native surface *
 *  handle the renderer can present to.                                                        *
 *                                                                                             *
 *  Handles are opaque so that no system headers - Xlib, SDL, Cocoa, Vulkan - leak into engine *
 *  headers. Nothing here includes anything but the C++ standard integer types.                *
 *                                                                                             *
 *  See docs/porting/window-event-loop.md for the measured Win32 surface this replaces, for    *
 *  where the semantics differ from Win32, and for which parts have been executed on which     *
 *  platform. The important divergence: Win32 delivers window messages by calling WndProc from *
 *  inside DispatchMessage(), so the engine's handling runs at an arbitrary depth of the call  *
 *  stack; here the caller drains a queue at a point of its choosing. That is a behaviour       *
 *  change for re-entrant cases (WM_PAINT during device reset, modal message boxes) and is     *
 *  costed in the doc rather than hidden.                                                      *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#pragma once

#ifndef _WIN32

namespace WWPlatform
{

/*
**	Event kinds. The set is exactly the WM_* messages GeneralsMD/Code/Main/WinMain.cpp's
**	WndProc acts on - no more - so the mapping from the existing handler to this queue is
**	one to one. WM_PAINT/WM_ERASEBKGND have no equivalent on purpose: they only exist in
**	WndProc to blit the loading screen while D3D is initialising, and a compositing window
**	server has no "your window content was discarded, redraw it" contract to satisfy.
*/
enum WindowEventType
{
	WINDOW_EVENT_NONE = 0,
	WINDOW_EVENT_CLOSE,				// WM_CLOSE / WM_QUERYENDSESSION
	WINDOW_EVENT_RESIZE,			// WM_SIZE, with Width/Height set to the client area
	WINDOW_EVENT_MOVE,				// WM_MOVE
	WINDOW_EVENT_FOCUS_GAINED,		// WM_SETFOCUS + the wParam!=0 case of WM_ACTIVATEAPP
	WINDOW_EVENT_FOCUS_LOST,		// WM_KILLFOCUS + the wParam==0 case of WM_ACTIVATEAPP
	WINDOW_EVENT_MINIMISED,			// the SIZE_MINIMIZED case of WM_SIZE; see Window_Is_Minimised
	WINDOW_EVENT_RESTORED,
	WINDOW_EVENT_KEY_DOWN,			// WM_KEYDOWN / WM_SYSKEYDOWN
	WINDOW_EVENT_KEY_UP,			// WM_KEYUP / WM_SYSKEYUP
	WINDOW_EVENT_TEXT,				// WM_CHAR, one Unicode code point per event
	WINDOW_EVENT_MOUSE_MOVE,		// WM_MOUSEMOVE
	WINDOW_EVENT_MOUSE_DOWN,		// WM_?BUTTONDOWN
	WINDOW_EVENT_MOUSE_UP,			// WM_?BUTTONUP
	WINDOW_EVENT_MOUSE_WHEEL,		// WM_MOUSEWHEEL
	WINDOW_EVENT_MOUSE_ENTER,		// no WM_ equivalent; WinMain.cpp derives this from
	WINDOW_EVENT_MOUSE_LEAVE,		// hit-testing WM_MOUSEMOVE against GetClientRect()
};

/*
**	Mouse buttons, in the order Mouse.h's MouseIO fields are declared (left, middle, right).
*/
enum MouseButtonIndex
{
	MOUSE_BUTTON_LEFT = 0,
	MOUSE_BUTTON_MIDDLE = 1,
	MOUSE_BUTTON_RIGHT = 2,
	MOUSE_BUTTON_COUNT = 3,
};

/*
**	Modifier bits, matching the sense of GetKeyState(VK_SHIFT) & 0x8000 rather than the
**	KEY_STATE_* bits in KeyDefs.h: this layer has no dependency on the engine's enums.
*/
enum WindowModifierFlags
{
	WINDOW_MODIFIER_NONE = 0,
	WINDOW_MODIFIER_SHIFT = 1 << 0,
	WINDOW_MODIFIER_CONTROL = 1 << 1,
	WINDOW_MODIFIER_ALT = 1 << 2,
	WINDOW_MODIFIER_CAPS_LOCK = 1 << 3,
};

/*
**	One event. Fields not relevant to Type are left at their defaults; nothing is a union,
**	because the struct is 40-odd bytes and the engine's own KeyboardIO/MouseIO are not
**	unions either.
**
**	Scan_Code is a PC/AT set-1 scan code, i.e. exactly the values in
**	Core/GameEngine/Include/GameClient/KeyScanCodes.h (KEYSCAN_*) and therefore exactly what
**	KeyDefType stores. This layer deliberately does not include that header - WWLib must not
**	depend on GameEngine - so the backends carry a numeric table, and
**	scripts/ci/check-window-scancodes.py checks that table against KeyScanCodes.h in CI.
**
**	Wheel_Delta is in WHEEL_DELTA (120) units with the same sign convention as
**	WM_MOUSEWHEEL, because Win32Mouse.cpp already divides by 120.
**
**	Time_Ms is a monotonic millisecond stamp for the event, replacing MSG::time, which the
**	engine plumbs through the global TheMessageTime into MouseIO::time.
*/
struct WindowEvent
{
	WindowEventType Type;
	int Scan_Code;					// KEYSCAN_* value, 0 when the key has no set-1 code
	bool Repeat;					// key auto-repeat; WM_KEYDOWN's lParam bit 30
	unsigned int Character;			// UTF-32 code point for WINDOW_EVENT_TEXT
	int Mouse_X;					// client-area coordinates, top-left origin, as WM_MOUSEMOVE
	int Mouse_Y;
	int Mouse_Button;				// MouseButtonIndex
	int Click_Count;				// 2 means the WM_?BUTTONDBLCLK case
	int Wheel_Delta;
	int Width;						// client area, for WINDOW_EVENT_RESIZE
	int Height;
	unsigned int Modifiers;			// WindowModifierFlags
	unsigned int Time_Ms;

	WindowEvent()
		: Type(WINDOW_EVENT_NONE), Scan_Code(0), Repeat(false), Character(0), Mouse_X(0),
		  Mouse_Y(0), Mouse_Button(MOUSE_BUTTON_LEFT), Click_Count(1), Wheel_Delta(0),
		  Width(0), Height(0), Modifiers(WINDOW_MODIFIER_NONE), Time_Ms(0)
	{
	}
};

/*
**	What initializeAppWindows() in WinMain.cpp passes to CreateWindow(), minus the Win32
**	spellings. Borderless is WS_POPUP without WS_CAPTION, which is what the game uses in
**	fullscreen.
*/
struct WindowConfig
{
	const char * Title;
	int Width;
	int Height;
	bool Fullscreen;
	bool Resizable;
	bool Hide_System_Cursor;		// the game draws its own cursor

	WindowConfig()
		: Title("Command and Conquer Generals"), Width(800), Height(600), Fullscreen(false),
		  Resizable(false), Hide_System_Cursor(false)
	{
	}
};

/*
**	The native handle the renderer needs. On macOS this is a CAMetalLayer, which is what
**	MoltenVK's VK_EXT_metal_surface / vkCreateMetalSurfaceEXT takes; on Linux it is the
**	X11 Display and Window pair, or the Wayland wl_display and wl_surface pair. Most callers
**	should use Window_Create_Vulkan_Surface() instead and never look at this.
*/
enum NativeSurfaceKind
{
	NATIVE_SURFACE_NONE = 0,
	NATIVE_SURFACE_X11,				// Handle_A = Display *, Handle_B = Window (as uintptr)
	NATIVE_SURFACE_WAYLAND,			// Handle_A = wl_display *, Handle_B = wl_surface *
	NATIVE_SURFACE_METAL_LAYER,		// Handle_A = CAMetalLayer *, Handle_B = NSWindow *
};

struct NativeSurface
{
	NativeSurfaceKind Kind;
	void * Handle_A;
	void * Handle_B;

	NativeSurface() : Kind(NATIVE_SURFACE_NONE), Handle_A(nullptr), Handle_B(nullptr) {}
};

/*
**	Lifetime. Window_Create() returns null on failure, with the reason available from
**	Window_Last_Error(); there is no equivalent of CreateWindow()'s "returns null, call
**	GetLastError()" split because there is no Win32 error code to report.
**
**	Only one window is supported, which is all the game has ever created. Creating a second
**	one fails rather than silently returning the first.
*/
void * Window_Create(const WindowConfig & config);
void Window_Destroy(void * window);

/*
**	The event loop. This is what replaces
**
**	    while (PeekMessage(&msg, nullptr, 0, 0, PM_NOREMOVE)) {
**	        GetMessage(&msg, nullptr, 0, 0);
**	        TheMessageTime = msg.time;
**	        TranslateMessage(&msg);
**	        DispatchMessage(&msg);
**	    }
**
**	in Win32GameEngine::serviceWindowsOS(). Returns false when the queue is empty, so the
**	call site keeps its `while` shape. Unlike DispatchMessage() nothing is called back:
**	the caller sees every event, in order, at the point it asked for them.
**
**	Window_Pump() drains the platform's own queue into this one without returning anything.
**	It exists because a compositor may need servicing even when the engine does not want
**	events (during a long load), which is the other thing serviceWindowsOS() is for.
*/
bool Window_Poll_Event(void * window, WindowEvent & event);
void Window_Pump(void * window);

/*
**	State queries. Window_Is_Minimised() is IsIconic(), which Win32GameEngine::update()
**	spins on while alt-tabbed out. Window_Is_Active() is the isWinMainActive flag WinMain.cpp
**	maintains from WM_ACTIVATEAPP.
*/
bool Window_Is_Minimised(void * window);
bool Window_Is_Active(void * window);
void Window_Client_Size(void * window, int & width, int & height);

/*
**	How many device pixels one point is on the display this window is currently on:
**	[NSWindow backingScaleFactor] on macOS, the drawable-size/window-size ratio on SDL2,
**	1.0 wherever the platform has no notion of one or cannot say. It changes at run time when
**	the window is dragged between a Retina and a non-Retina display.
**
**	This is the ONE place points become pixels, and the renderer is its only consumer: it sizes
**	its colour target at client size x scale so the game rasterises every pixel of the panel
**	(docs/porting/hidpi-scale.md). Everything else in this header stays in points - the mouse,
**	the GUI and the window geometry - and multiplying any of those by this is the bug this
**	function exists to fix, not a use of it.
*/
float Window_Backing_Scale(void * window);

/*
**	Placement and the mode change. Window_Set_Mode() covers what W3DDisplay's
**	setDisplayMode does through SetWindowPos()/ShowWindow() plus D3D8's device reset:
**	resize the client area and switch borderless-fullscreen on or off. It does *not* change
**	the display's video mode - the Win32 path's ChangeDisplaySettings() equivalent - because
**	both macOS and Wayland deliberately have no such call; see the doc.
*/
bool Window_Set_Mode(void * window, int width, int height, bool fullscreen);
void Window_Show(void * window, bool show);
void Window_Set_Title(void * window, const char * title);

/*
**	The rest of what DX8Wrapper::Resize_And_Position_Window() needs, which is a Win32
**	GetClientRect()/GetWindowLong()/AdjustWindowRect()/SetWindowPos() sequence: it sizes the
**	*frame* to fit a given client area and then centres the frame on the monitor's work area.
**
**	Window_Frame_Insets() is the thickness the platform adds around the client area - title bar
**	and borders - so a client size can be turned into a frame size and back. It is all zeros for
**	a borderless or fullscreen window, and false when the platform cannot say.
**	Window_Is_Fullscreen() is which of the two the window currently is.
**	Window_Set_Position() moves the window by its client area's top-left corner, in the same
**	screen coordinates Window_Client_Origin() reports.
**	Window_Set_Always_On_Top() is SetWindowPos()'s HWND_TOPMOST, which the fullscreen path asks
**	for so the game is not covered by the shell.
**
**	POINTS, again and for the same reason: the frame metrics and the monitor geometry the game
**	compares against a resolution are both in points here, and the renderer converts to pixels
**	at its own boundary. Do not "fix" a half-size window by scaling these
**	(docs/porting/decisions-resolved.md).
*/
bool Window_Frame_Insets(void * window, int & left, int & top, int & right, int & bottom);
bool Window_Is_Fullscreen(void * window);
bool Window_Set_Position(void * window, int x, int y);
bool Window_Set_Client_Size(void * window, int width, int height);
void Window_Set_Always_On_Top(void * window, bool on_top);

/*
**	The display the window is on, and its geometry - MonitorFromWindow() and GetMonitorInfo().
**	Displays are numbered from zero and the numbering is the platform's; index 0 is the primary
**	display. Window_Display_Bounds() is the whole display and Window_Display_Work_Area() is what
**	is left after the menu bar, the Dock or the taskbar, which is the rectangle a window is
**	centred in. Both are in screen points with a top-left origin, like Window_Client_Origin().
*/
int Window_Display_Count();
int Window_Display_For_Window(void * window);
bool Window_Display_Bounds(int display, int & x, int & y, int & width, int & height);
bool Window_Display_Work_Area(int display, int & x, int & y, int & width, int & height);

/*
**	Mouse. Window_Set_Cursor_Clip() is ClipCursor() (Win32Mouse::refreshCursorCapture);
**	Window_Warp_Cursor() is SetCursorPos() in client coordinates; Window_Show_System_Cursor()
**	is ShowCursor().
*/
void Window_Set_Cursor_Clip(void * window, bool clip);
void Window_Warp_Cursor(void * window, int x, int y);
void Window_Show_System_Cursor(void * window, bool show);

/*
**	Cursor shape, the other half of SetCursor(). Windows gets the game's cursors from
**	LoadCursorFromFile() on Data\Cursors\*.ANI; off Windows platform_cursor.cpp decodes the
**	file and the backend turns the first frame into a native cursor here.
**
**	CursorImage is Width*Height 32-bit BGRA pixels with straight (unpremultiplied) alpha, rows
**	top-down, hotspot in that same top-left-origin pixel space; the pixels are copied, the
**	caller keeps ownership. Window_Create_Cursor() returns an opaque handle, or null with
**	Window_Last_Error() set. Window_Set_Cursor() makes that shape the one shown over the
**	window (SetCursor(non-null)); a null cursor restores the platform's default arrow. It does
**	not change visibility: that stays with Window_Show_System_Cursor(). Destroying the cursor
**	that is currently set restores the default arrow first.
**
**	Only the first frame is presented; the .ANI's animation is not reproduced (see
**	docs/porting/mouse-cursor-seam.md).
*/
struct CursorImage
{
	int Width;
	int Height;
	int Hotspot_X;
	int Hotspot_Y;
	const unsigned char * Pixels_BGRA;

	CursorImage() : Width(0), Height(0), Hotspot_X(0), Hotspot_Y(0), Pixels_BGRA(nullptr) {}
};

void * Window_Create_Cursor(const CursorImage & image);
void Window_Destroy_Cursor(void * cursor);
void Window_Set_Cursor(void * window, void * cursor);

/*
**	The read side of the same seam, which is what GetCursorPos() and ScreenToClient() need.
**	Window_Cursor_Position() is GetCursorPos(): where the pointer is now, in the platform's own
**	screen coordinates with a top-left origin, whether or not it is over our window.
**	Window_Client_Origin() is where the client area's top-left corner sits in those same
**	coordinates, which is exactly the offset ScreenToClient() subtracts. Both return false when
**	the window is gone, and neither touches its outputs in that case.
**
**	Both are in POINTS, like every other coordinate in this seam and like the engine's mouse
**	(see docs/porting/decisions-resolved.md): the renderer converts to pixels at its own
**	boundary. On a Retina display the difference is a factor of two, and on the CI runner it is
**	a factor of one, so getting this wrong is invisible in CI.
*/
bool Window_Cursor_Position(void * window, int & x, int & y);
bool Window_Client_Origin(void * window, int & x, int & y);

/*
**	The one window, or null before Window_Create() and after Window_Destroy(). The engine holds
**	the handle itself (ApplicationHWnd) and should keep passing it; this exists for the Win32
**	compatibility layer, where GetCursorPos() has no HWND parameter to take one from and cannot
**	see an engine global.
*/
void * Window_Current();

/*
**	Polled keyboard state, replacing GetAsyncKeyState()/GetKeyState() and the DirectInput
**	GetDeviceState() that Win32DIKeyboard uses. Takes a KEYSCAN_* set-1 code. Unlike
**	GetAsyncKeyState() this reflects the state as of the last Window_Pump(), not a hardware
**	read, so it cannot report a keypress the event queue has not seen yet.
*/
bool Window_Key_Is_Down(void * window, int scan_code);
unsigned int Window_Modifier_State(void * window);

/*
**	Renderer handoff. Window_Vulkan_Instance_Extensions() fills `names` with the instance
**	extensions this window needs (VK_KHR_surface plus the platform one, e.g.
**	VK_EXT_metal_surface on macOS) and returns how many, or -1 if `max_names` is too small.
**	The strings are statically allocated and outlive the window.
**
**	Window_Create_Vulkan_Surface() takes a VkInstance and writes a VkSurfaceKHR, both passed
**	as opaque void pointers so that no Vulkan header is needed here. It returns false
**	with Window_Last_Error() set on failure. On macOS it is the *only* supported route,
**	because it is where the CAMetalLayer is handed to vkCreateMetalSurfaceEXT.
*/
int Window_Vulkan_Instance_Extensions(void * window, const char ** names, int max_names);
bool Window_Create_Vulkan_Surface(void * window, void * vk_instance, void * out_vk_surface);
NativeSurface Window_Native_Surface(void * window);

/*
**	Which backend was compiled in ("sdl2", "cocoa"), and the last failure. Both are for
**	diagnostics and for the standalone checks; the engine has no reason to branch on them.
*/
const char * Window_Backend_Name();
const char * Window_Last_Error();

}	// namespace WWPlatform

#endif // !_WIN32
