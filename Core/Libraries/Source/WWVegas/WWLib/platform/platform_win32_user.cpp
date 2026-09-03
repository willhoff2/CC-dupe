/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 TheSuperHackers
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
 *  The user32 corner of the Win32 compatibility layer that the OS-display and debug-hotkey     *
 *  call sites need: the two MessageBox() entry points, the keyboard poll, and the power        *
 *  request. See docs/porting/win32-runtime-and-crt-gaps.md.                                    *
 *                                                                                             *
 *  MessageBoxA()/MessageBoxW() forward to WWPlatform::Dialog_Message_Box(), the loud stderr    *
 *  stub described in docs/porting/window-event-loop.md: the caption and text are printed and    *
 *  the default button's answer is returned, without drawing or waiting. Win32OSDisplay.cpp     *
 *  reaches the wide one, which is why the conversion to UTF-8 happens here rather than at the   *
 *  call site.                                                                                  *
 *                                                                                             *
 *  GetAsyncKeyState()/GetKeyState() report "not down". Both are hardware polls with no          *
 *  portable equivalent, and the seam that replaces them for the game's real input path is       *
 *  platform_window.h's Window_Key_Is_Down(), which needs the window the poll has no argument    *
 *  for. What is left reaching these is debug tooling -- W3DWaterTracks.cpp's F5-F8 wave        *
 *  editing hotkeys -- so answering "no key" leaves that tooling inert rather than wrong, and    *
 *  each entry point says so on stderr the first time it is reached.                             *
 *                                                                                              *
 *  IsIconic(), GetCursorPos(), ScreenToClient() and SetCursor() are real, and go through          *
 *  platform_window.h: the HWND these take *is* the seam's window handle off Windows, so           *
 *  IsIconic() and ScreenToClient() need no lookup, and GetCursorPos(), which has no window        *
 *  parameter, asks the seam for its one window. ScreenToClient() is the client origin subtracted   *
 *  -- the whole of it for a parentless, unmirrored window, which is the only kind the game        *
 *  creates -- and both it and GetCursorPos() work in POINTS, because this is the mouse path and    *
 *  the renderer converts at its own boundary (docs/porting/decisions-resolved.md).                 *
 *                                                                                              *
 *  LoadCursorFromFile(), SetCursor() and DestroyCursor() are the game's cursor: the retail       *
 *  Data\Cursors\*.ANI is decoded by platform_cursor.cpp and its first frame becomes a backend    *
 *  cursor through Window_Create_Cursor(); SetCursor(null) hides the pointer as Win32 does, and   *
 *  SetCursor(handle) selects the shape and shows it (docs/porting/mouse-cursor-seam.md).         *
 *                                                                                              *
 *  GetClientRect(), GetWindowLongA(), AdjustWindowRect(), MonitorFromWindow(),                  *
 *  GetMonitorInfoA() and SetWindowPos() are the window-sizing path                              *
 *  DX8Wrapper::Resize_And_Position_Window() drives, and they all work in POINTS for the same     *
 *  reason the mouse ones do. Win32 places a FRAME rectangle; the seam places a CLIENT area,      *
 *  because the client area is what the renderer's back buffer must match, so SetWindowPos()      *
 *  converts by the frame insets and AdjustWindowRect() is its exact inverse. HMONITOR off        *
 *  Windows is the seam's display index biased by one, decoded only by GetMonitorInfoA().         *
 *  GetDesktopWindow() returns a handle that is deliberately not the seam's window, since the      *
 *  only caller hands it straight to GetDC(). See docs/porting/window-gdi-seam.md.                *
 *                                                                                              *
 *  SetDeviceGammaRamp() is the one refusal here: it returns FALSE and says why on stderr.         *
 *  There is no portable display gamma ramp, its macOS equivalent changes the whole display        *
 *  without Windows' automatic revert on process exit, and a renderer-side post-process is a       *
 *  renderer decision. The cost is that the brightness slider does nothing off Windows.           *
 *                                                                                              *
 *  SetThreadExecutionState() is a stub for the same reason: keeping the display awake during a  *
 *  long load is a Windows power-management API, its macOS equivalent (IOPMAssertion) is a       *
 *  separate decision, and nothing about the game's correctness depends on it.                   *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#include "WWLib/platform/platform_win32_compat.h"

#ifdef WWPLATFORM_WIN32_COMPAT

#include "WWLib/platform/platform_cursor.h"
#include "WWLib/platform/platform_dialog.h"
#include "WWLib/platform/platform_path.h"
#include "WWLib/platform/platform_window.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

namespace
{

/*
**	MB_OK / MB_OKCANCEL / MB_ABORTRETRYIGNORE / MB_YESNO live in the low four bits of the flags;
**	the icon and modality bits above them have no meaning without a window to draw.
*/
const UINT MB_TYPEMASK_BITS = 0x0000000F;

WWPlatform::DialogButtons Buttons_From_Flags(UINT flags)
{
	switch (flags & MB_TYPEMASK_BITS) {
		case MB_ABORTRETRYIGNORE:
			return WWPlatform::DIALOG_BUTTONS_ABORT_RETRY_IGNORE;
		case MB_YESNO:
			return WWPlatform::DIALOG_BUTTONS_YES_NO;
		default:
			/*
			**	MB_OK and MB_OKCANCEL both default to OK, which is the answer
			**	Dialog_Message_Box() gives for DIALOG_BUTTONS_OK.
			*/
			return WWPlatform::DIALOG_BUTTONS_OK;
	}
}

std::string Narrow(LPCWSTR text)
{
	if (text == nullptr) {
		return std::string();
	}

	const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
	if (length <= 1) {
		return std::string();
	}

	std::string narrow(static_cast<size_t>(length - 1), '\0');
	WideCharToMultiByte(CP_UTF8, 0, text, -1, &narrow[0], length, nullptr, nullptr);
	return narrow;
}

/*
**	What an HCURSOR is off Windows: the decoded file's first frame turned into a backend cursor
**	by Window_Create_Cursor(), plus what the decoder reported, for diagnostics. Native is null
**	when the file could not be read or decoded, or the backend could not make a cursor of it;
**	selecting such a handle shows the platform's default arrow, so the pointer never vanishes
**	because an asset is missing (docs/porting/mouse-cursor-seam.md, "missing data").
*/
struct Cursor_Handle
{
	void * Native;
	WWPlatform::CursorFile File;
	std::string Path;

	Cursor_Handle() : Native(nullptr) {}
};

Cursor_Handle * Handle_Of(HCURSOR cursor)
{
	return reinterpret_cast<Cursor_Handle *>(cursor);
}

HCURSOR TheCurrentCursor = nullptr;

}	// anonymous namespace

extern "C" {

int MessageBoxA(HWND, LPCSTR text, LPCSTR caption, UINT flags)
{
	return WWPlatform::Dialog_Message_Box((caption != nullptr) ? caption : "",
		(text != nullptr) ? text : "", Buttons_From_Flags(flags));
}


int MessageBoxW(HWND, LPCWSTR text, LPCWSTR caption, UINT flags)
{
	const std::string narrow_text = Narrow(text);
	const std::string narrow_caption = Narrow(caption);
	return WWPlatform::Dialog_Message_Box(narrow_caption.c_str(), narrow_text.c_str(),
		Buttons_From_Flags(flags));
}


BOOL SetRect(LPRECT rect, int left, int top, int right, int bottom)
{
	/*
	**	Pure arithmetic on a RECT, with no display involved. Win32 fails only for a null pointer.
	*/
	if (rect == nullptr) {
		return FALSE;
	}
	rect->left = left;
	rect->top = top;
	rect->right = right;
	rect->bottom = bottom;
	return TRUE;
}


short GetAsyncKeyState(int)
{
	WWPlatform::Win32::Report_Stub("GetAsyncKeyState",
		"there is no hardware key poll here; use platform_window.h's Window_Key_Is_Down()");
	return 0;
}


short GetKeyState(int)
{
	WWPlatform::Win32::Report_Stub("GetKeyState",
		"there is no hardware key poll here; use platform_window.h's Window_Modifier_State()");
	return 0;
}


BOOL IsIconic(HWND window)
{
	/*
	**	Win32GameEngine::update() spins on this while the game is alt-tabbed out, and
	**	W3DDisplay::draw() uses it to skip a frame. Both pass ApplicationHWnd, which off Windows
	**	*is* the seam's window handle, so there is nothing to look up. A null window is not
	**	minimised, which is what Win32 answers for a handle it cannot find.
	*/
	if (window == nullptr) {
		return FALSE;
	}
	return WWPlatform::Window_Is_Minimised(window) ? TRUE : FALSE;
}


BOOL GetCursorPos(LPPOINT point)
{
	/*
	**	GetCursorPos() has no window parameter, so the seam's single window is found through
	**	Window_Current() rather than through an engine global WWLib cannot see. The result is the
	**	pointer in screen points; every caller immediately passes it to ScreenToClient(), below.
	*/
	if (point == nullptr) {
		return FALSE;
	}

	int x = 0;
	int y = 0;
	if (!WWPlatform::Window_Cursor_Position(WWPlatform::Window_Current(), x, y)) {
		/*
		**	Before the window exists there is no screen coordinate space to answer in. Win32
		**	fails the same way for a session with no desktop, and every caller tests the result:
		**	W3DWaterTracks.cpp's `if (GetCursorPos(&p))` and W3DMouse.cpp's, which only reaches
		**	it inside its own window-active branch.
		*/
		return FALSE;
	}

	point->x = x;
	point->y = y;
	return TRUE;
}


BOOL ScreenToClient(HWND window, LPPOINT point)
{
	/*
	**	Subtracting the client area's origin is the whole of ScreenToClient() for a window with
	**	no parent chain and no mirrored layout, which is the only kind the game creates. Points
	**	in, points out: this is the mouse path, and the renderer converts to pixels at its own
	**	boundary (docs/porting/decisions-resolved.md).
	*/
	if (point == nullptr) {
		return FALSE;
	}

	int origin_x = 0;
	int origin_y = 0;
	if (!WWPlatform::Window_Client_Origin(window, origin_x, origin_y)) {
		return FALSE;
	}

	point->x -= origin_x;
	point->y -= origin_y;
	return TRUE;
}



HCURSOR LoadCursorFromFileA(LPCSTR path)
{
	/*
	**	The Win32 spelling Win32Mouse::initCursorResources() uses on "data\cursors\Name.ANI".
	**	The path is Windows shaped and possibly the wrong case for this filesystem, which is what
	**	Path::Open_Stream() exists to absorb.
	**
	**	Unlike Win32 this never returns null for a bad or missing file: the handle comes back
	**	with no native cursor and SetCursor() on it shows the default arrow. Returning null would
	**	make the caller pass null to SetCursor(), which hides the pointer, and a hidden pointer is
	**	the defect this replaces. The failure is said on stderr.
	*/
	Cursor_Handle * handle = new Cursor_Handle();
	handle->Path = path != nullptr ? path : "";

	std::string error;
	std::vector<unsigned char> bytes;
	FILE * stream = path != nullptr ? WWPlatform::Path::Open_Stream(path, "rb") : nullptr;
	if (stream == nullptr) {
		error = "file not found";
	} else {
		unsigned char chunk[4096];
		size_t got;
		while ((got = fread(chunk, 1, sizeof(chunk), stream)) > 0) {
			bytes.insert(bytes.end(), chunk, chunk + got);
		}
		fclose(stream);
		if (WWPlatform::Cursor_Decode(bytes.data(), bytes.size(), handle->File, error)) {
			WWPlatform::CursorImage image;
			image.Width = handle->File.First.Width;
			image.Height = handle->File.First.Height;
			image.Hotspot_X = handle->File.First.Hotspot_X;
			image.Hotspot_Y = handle->File.First.Hotspot_Y;
			image.Pixels_BGRA = handle->File.First.Pixels_BGRA.data();
			handle->Native = WWPlatform::Window_Create_Cursor(image);
			if (handle->Native == nullptr) error = WWPlatform::Window_Last_Error();
		}
	}

	if (handle->Native == nullptr) {
		fprintf(stderr, "!!! LoadCursorFromFile(\"%s\"): %s; the default arrow is shown instead\n",
			handle->Path.c_str(), error.c_str());
	}
	return reinterpret_cast<HCURSOR>(handle);
}


BOOL DestroyCursor(HCURSOR cursor)
{
	Cursor_Handle * handle = Handle_Of(cursor);
	if (handle == nullptr) return FALSE;
	if (TheCurrentCursor == cursor) TheCurrentCursor = nullptr;
	WWPlatform::Window_Destroy_Cursor(handle->Native);
	delete handle;
	return TRUE;
}


HCURSOR SetCursor(HCURSOR cursor)
{
	/*
	**	Win32Mouse::setCursor() passes null for "no cursor" (NONE, or the mouse made invisible),
	**	which in Win32 hides the pointer, and otherwise a handle from LoadCursorFromFile(). Both
	**	halves go through the seam: the shape to Window_Set_Cursor(), the visibility to
	**	Window_Show_System_Cursor(), so that the pointer is shown exactly when Windows would
	**	show it and with the shape Windows would give it. A handle without a native cursor (see
	**	LoadCursorFromFileA) selects the default arrow.
	*/
	static HCURSOR previous = nullptr;

	void * window = WWPlatform::Window_Current();
	if (cursor != nullptr && cursor != TheCurrentCursor) {
		WWPlatform::Window_Set_Cursor(window, Handle_Of(cursor)->Native);
		TheCurrentCursor = cursor;
	}
	WWPlatform::Window_Show_System_Cursor(window, cursor != nullptr);

	/*
	**	Win32 returns the cursor that was set before, and null if there was none. Nothing in the
	**	engine uses the result, but returning the previous value is free and is what the
	**	documented contract says.
	*/
	HCURSOR was = previous;
	previous = cursor;
	return was;
}


BOOL GetClientRect(HWND window, LPRECT rect)
{
	/*
	**	The client area, origin at zero, in POINTS -- Win32's own units for this call and the
	**	units the seam keeps window geometry in. The renderer's backing size in pixels is a
	**	different number on a Retina display and is obtained at the renderer boundary, not here
	**	(docs/porting/decisions-resolved.md).
	*/
	if (rect == nullptr) {
		return FALSE;
	}

	int width = 0;
	int height = 0;
	WWPlatform::Window_Client_Size(window, width, height);
	if (width <= 0 || height <= 0) {
		return FALSE;
	}

	rect->left = 0;
	rect->top = 0;
	rect->right = width;
	rect->bottom = height;
	return TRUE;
}


LONG GetWindowLongA(HWND window, int index)
{
	/*
	**	Off Windows there is no per-window LONG store, so the only honest answer is one
	**	assembled from the seam's actual state. GWL_STYLE is the index that matters:
	**	DX8Wrapper::Resize_And_Position_Window() reads it purely to hand to AdjustWindowRect(),
	**	and Debug.cpp tests it against WS_CAPTION, so both callers need the frame bits to say
	**	whether this window has a frame -- which the seam knows.
	*/
	if (window == nullptr) {
		return 0;
	}

	switch (index) {
		case GWL_STYLE:
		{
			LONG style = WS_VISIBLE;
			if (WWPlatform::Window_Is_Fullscreen(window)) {
				/*
				**	A borderless, undecorated window: Win32's fullscreen device window is
				**	created WS_POPUP, and it has no frame for AdjustWindowRect() to add.
				*/
				style |= WS_POPUP;
			} else {
				style |= WS_OVERLAPPEDWINDOW;
			}
			if (WWPlatform::Window_Is_Minimised(window)) {
				style |= WS_MINIMIZE;
			}
			return style;
		}

		case GWL_EXSTYLE:
			/*
			**	The seam creates no extended styles -- no topmost-at-creation, no layered, no
			**	tool window -- so zero is the truth rather than a placeholder.
			*/
			return 0;

		default:
			WWPlatform::Win32::Report_Stub("GetWindowLongA",
				"only GWL_STYLE and GWL_EXSTYLE exist off Windows; there is no per-window "
				"LONG store, no WndProc and no HINSTANCE to report");
			return 0;
	}
}


BOOL AdjustWindowRect(LPRECT rect, DWORD style, BOOL menu)
{
	/*
	**	Grow a client rectangle into the frame rectangle that contains it, in points. Win32
	**	computes this from the style alone because it knows every theme's border metrics; here
	**	the border metrics belong to the one window the seam owns, so they come from
	**	Window_Frame_Insets(). That is the same window whose style the caller just read with
	**	GetWindowLong(GWL_STYLE), which is why the two compose correctly: a frameless
	**	(fullscreen, WS_POPUP) window reports no frame bits AND has zero insets.
	**
	**	Win32 subtracts from left/top and adds to right/bottom, i.e. the returned rectangle is
	**	in a space where the client origin stays at (0,0) and left/top go negative.
	**	DX8Wrapper::Resize_And_Position_Window() depends on exactly that: it uses rect.left and
	**	rect.top as the offset from the frame corner to the client corner.
	*/
	if (rect == nullptr) {
		return FALSE;
	}

	if (menu != FALSE) {
		WWPlatform::Win32::Report_Stub("AdjustWindowRect",
			"there is no menu bar off Windows, so no menu height is added; the frame will be "
			"one menu bar short of the Windows answer");
	}

	const DWORD FRAME_BITS = (DWORD)(WS_BORDER | WS_DLGFRAME | WS_THICKFRAME);
	if ((style & FRAME_BITS) == 0) {
		/*
		**	No border of any kind: frame rectangle == client rectangle. Win32 returns the
		**	rectangle unchanged here too.
		*/
		return TRUE;
	}

	int left = 0;
	int top = 0;
	int right = 0;
	int bottom = 0;
	if (!WWPlatform::Window_Frame_Insets(WWPlatform::Window_Current(), left, top, right, bottom)) {
		/*
		**	The platform would not say how thick its own decorations are (an SDL2 older than
		**	2.0.5, or a window that has not been shown yet). Zero insets keep the client SIZE
		**	right -- which is what the renderer's back buffer is matched to -- and leave only
		**	the window POSITION off by the title bar, so this says so rather than refusing to
		**	size the render window at all.
		*/
		WWPlatform::Win32::Report_Stub("AdjustWindowRect",
			"the platform did not report its frame insets; sizing as if the window had no "
			"decorations, so the window may sit one title bar out of place");
		return TRUE;
	}

	rect->left -= left;
	rect->top -= top;
	rect->right += right;
	rect->bottom += bottom;
	return TRUE;
}


HMONITOR MonitorFromWindow(HWND window, DWORD flags)
{
	/*
	**	HMONITOR off Windows is the seam's display index, biased by one so that a valid handle
	**	is never null -- null is Win32's "no monitor", which is what MONITOR_DEFAULTTONULL asks
	**	for when the window is on no display. GetMonitorInfoA() below is the only thing that
	**	decodes it.
	*/
	if (window == nullptr) {
		if ((flags & (MONITOR_DEFAULTTOPRIMARY | MONITOR_DEFAULTTONEAREST)) == 0) {
			return nullptr;
		}
		return (HMONITOR)(uintptr_t)1;
	}

	const int display = WWPlatform::Window_Display_For_Window(window);
	return (HMONITOR)(uintptr_t)(display + 1);
}


BOOL GetMonitorInfoA(HMONITOR monitor, LPMONITORINFO info)
{
	/*
	**	The display's bounds and its work area, in points. rcWork excludes the menu bar and the
	**	Dock on macOS, exactly as it excludes the taskbar on Windows, which is what
	**	DX8Wrapper::Resize_And_Position_Window() centres a windowed render device inside.
	**
	**	Win32 fails if cbSize was not filled in, and callers rely on that to detect a struct
	**	from a different SDK, so the check is kept.
	*/
	if (info == nullptr || info->cbSize < sizeof(MONITORINFO)) {
		return FALSE;
	}

	const uintptr_t encoded = (uintptr_t)monitor;
	if (encoded == 0) {
		return FALSE;
	}
	const int display = (int)(encoded - 1);

	int x = 0;
	int y = 0;
	int width = 0;
	int height = 0;
	if (!WWPlatform::Window_Display_Bounds(display, x, y, width, height)) {
		return FALSE;
	}
	info->rcMonitor.left = x;
	info->rcMonitor.top = y;
	info->rcMonitor.right = x + width;
	info->rcMonitor.bottom = y + height;

	if (!WWPlatform::Window_Display_Work_Area(display, x, y, width, height)) {
		return FALSE;
	}
	info->rcWork.left = x;
	info->rcWork.top = y;
	info->rcWork.right = x + width;
	info->rcWork.bottom = y + height;

	info->dwFlags = (display == 0) ? MONITORINFOF_PRIMARY : 0;
	return TRUE;
}


BOOL SetWindowPos(HWND window, HWND insert_after, int x, int y, int cx, int cy, UINT flags)
{
	/*
	**	Win32 places and sizes the FRAME rectangle in screen points; the seam places and sizes
	**	the CLIENT area, because that is the rectangle the renderer's back buffer has to match.
	**	The conversion is the frame insets, i.e. the inverse of AdjustWindowRect() above, and it
	**	has to be the inverse: DX8Wrapper hands this the frame size AdjustWindowRect() produced
	**	and expects the client area to come out at ResolutionWidth x ResolutionHeight.
	*/
	if (window == nullptr) {
		return FALSE;
	}

	int left = 0;
	int top = 0;
	int right = 0;
	int bottom = 0;
	if (!WWPlatform::Window_Frame_Insets(window, left, top, right, bottom)) {
		WWPlatform::Win32::Report_Stub("SetWindowPos",
			"the platform did not report its frame insets; placing and sizing the client area "
			"as if the window had no decorations");
		left = 0;
		top = 0;
		right = 0;
		bottom = 0;
	}

	BOOL result = TRUE;

	if ((flags & SWP_NOZORDER) == 0) {
		if (insert_after == HWND_TOPMOST) {
			WWPlatform::Window_Set_Always_On_Top(window, true);
		} else if (insert_after == HWND_NOTOPMOST) {
			WWPlatform::Window_Set_Always_On_Top(window, false);
		} else if (insert_after != HWND_TOP && insert_after != nullptr) {
			/*
			**	"Put me directly behind that other window" needs a second window to be relative
			**	to, and the seam owns exactly one.
			*/
			WWPlatform::Win32::Report_Stub("SetWindowPos",
				"there is only one window off Windows, so it cannot be ordered relative to "
				"another; the Z order is left alone");
		}
	}

	if ((flags & SWP_NOMOVE) == 0) {
		if (!WWPlatform::Window_Set_Position(window, x + left, y + top)) {
			result = FALSE;
		}
	}

	if ((flags & SWP_NOSIZE) == 0) {
		const int client_width = cx - left - right;
		const int client_height = cy - top - bottom;
		if (!WWPlatform::Window_Set_Client_Size(window, client_width, client_height)) {
			result = FALSE;
		}
	}

	if ((flags & SWP_SHOWWINDOW) != 0) {
		WWPlatform::Window_Show(window, true);
	}

	return result;
}


HWND GetDesktopWindow()
{
	/*
	**	There is no desktop window off Windows, and the one call site -- DX8Wrapper's gamma
	**	fallback -- only wants something to pass to GetDC()/ReleaseDC(), which ignore it. This
	**	returns a handle that is deliberately NOT the seam's window, so that code which mistakes
	**	it for a real window operates on nothing rather than on the game's window: the seam's
	**	lookups do not recognise it, so Window_* calls on it fail.
	*/
	static HWND__ desktop = { 0 };
	return &desktop;
}


BOOL SetDeviceGammaRamp(HDC, LPVOID)
{
	/*
	**	Refused, loudly, and to a caller that checks the result -- and the caller does:
	**	DX8Wrapper::Set_Gamma() only reaches this when the D3D8 device itself reports no gamma
	**	support, so this is already the fallback of a fallback, and FALSE means "the ramp was
	**	not applied".
	**
	**	Why not emulate it: a gamma ramp on Windows is a property of the display, set through
	**	the DC and reverted when the process exits. The macOS equivalent
	**	(CGSetDisplayTransferByTable) has the same whole-display reach but no such automatic
	**	revert, so a crash leaves the user's screen miscoloured until they log out; and doing it
	**	in the renderer instead means a post-process pass over every frame, which is a renderer
	**	decision and not a Win32 compatibility one. Until that decision is made, the honest
	**	behaviour is that the game's brightness slider does nothing off Windows, which is a
	**	cosmetic loss, and it is recorded in docs/porting/window-gdi-seam.md.
	*/
	WWPlatform::Win32::Report_Stub("SetDeviceGammaRamp",
		"there is no portable display gamma ramp; the brightness setting has no effect off "
		"Windows (see docs/porting/window-gdi-seam.md)");
	return FALSE;
}


EXECUTION_STATE SetThreadExecutionState(EXECUTION_STATE)
{
	WWPlatform::Win32::Report_Stub("SetThreadExecutionState",
		"keeping the display awake is a Windows power API; the macOS equivalent is undecided");
	/*
	**	Win32 returns the previous state, and zero for failure. ES_CONTINUOUS is the state a
	**	process starts in, so it is the honest answer to "what was it before".
	*/
	return ES_CONTINUOUS;
}

}	// extern "C"

#endif // WWPLATFORM_WIN32_COMPAT
