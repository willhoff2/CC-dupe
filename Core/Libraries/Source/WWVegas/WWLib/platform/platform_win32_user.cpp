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
 *  SetCursor() is hide-or-show: every call site reachable here passes null, meaning "no           *
 *  cursor", because W3DMouse draws the game's own. A non-null HCURSOR would be a handle from      *
 *  LoadCursor()/LoadCursorFromFile() reading a .CUR/.ANI, which do not exist here, so it says so   *
 *  once and shows the system pointer rather than guessing a shape.                                *
 *                                                                                              *
 *  SetThreadExecutionState() is a stub for the same reason: keeping the display awake during a  *
 *  long load is a Windows power-management API, its macOS equivalent (IOPMAssertion) is a       *
 *  separate decision, and nothing about the game's correctness depends on it.                   *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#include "WWLib/platform/platform_win32_compat.h"

#ifdef WWPLATFORM_WIN32_COMPAT

#include "WWLib/platform/platform_dialog.h"
#include "WWLib/platform/platform_window.h"

#include <stdlib.h>
#include <string.h>

#include <string>

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


HCURSOR SetCursor(HCURSOR cursor)
{
	/*
	**	The game's own cursor is drawn by W3DMouse, and what it wants from Win32 is for the
	**	system pointer to be out of the way: every reachable call site off Windows passes null,
	**	which in Win32 means "no cursor", i.e. hide it. That maps exactly onto the seam's
	**	Window_Show_System_Cursor().
	**
	**	A non-null HCURSOR cannot be honoured: the handle would have come from LoadCursor() or
	**	LoadCursorFromFile() reading a Win32 .CUR/.ANI, neither of which exists here, so there is
	**	nothing to identify the requested shape by. Those call sites are in Win32DIMouse.cpp,
	**	which is not on the native path. Rather than pick an arbitrary shape, this says so once
	**	and shows the system pointer, so the symptom is "the wrong cursor is visible" rather than
	**	"the pointer vanished".
	*/
	static HCURSOR previous = nullptr;

	if (cursor != nullptr) {
		WWPlatform::Win32::Report_Stub("SetCursor",
			"a non-null HCURSOR is a Win32 .CUR/.ANI handle and cannot be resolved to a shape "
			"here; showing the system pointer instead");
	}

	WWPlatform::Window_Show_System_Cursor(WWPlatform::Window_Current(), cursor != nullptr);

	/*
	**	Win32 returns the cursor that was set before, and null if there was none. Nothing in the
	**	engine uses the result, but returning the previous value is free and is what the
	**	documented contract says.
	*/
	HCURSOR was = previous;
	previous = cursor;
	return was;
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
