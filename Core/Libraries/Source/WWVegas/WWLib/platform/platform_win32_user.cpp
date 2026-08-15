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
 *  SetThreadExecutionState() is a stub for the same reason: keeping the display awake during a  *
 *  long load is a Windows power-management API, its macOS equivalent (IOPMAssertion) is a       *
 *  separate decision, and nothing about the game's correctness depends on it.                   *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#include "WWLib/platform/platform_win32_compat.h"

#ifdef WWPLATFORM_WIN32_COMPAT

#include "WWLib/platform/platform_dialog.h"

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
