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

// FILE: PlatformWindowHost.h /////////////////////////////////////////////////////////////////////
//
// TheSuperHackers @port The engine side of the window/event-loop/input seam. This is what
// WinMain.cpp's WndProc(), initializeAppWindows() and Win32GameEngine::serviceWindowsOS() become
// where there is no HWND: the window is created through
// Core/Libraries/Source/WWVegas/WWLib/platform/platform_window.h, and its events are *pulled* at
// one known point in the frame instead of being dispatched to a callback at arbitrary stack
// depth. See docs/porting/window-event-loop.md, which triages every WM_* case WndProc handles
// and records what this loses.
//
///////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

#ifndef _WIN32

#include "Lib/BaseType.h"
#include "WWLib/platform/platform_window.h"

// The one application window, under the name WinMain.cpp gives it on Windows, so the consumers
// that reach for it - Debug.cpp, Win32Mouse.cpp - are spelled the same on both platforms. Null
// until createAppWindow() succeeds, and when running headless.
extern void * ApplicationHWnd;

namespace PlatformWindowHost
{

/*
**	initializeAppWindows(). Borderless fullscreen when runWindowed is false, because that is all
**	the Win32 path ever did: a screen sized WS_POPUP plus a device reset, never
**	ChangeDisplaySettings(). The system cursor is hidden either way, since the game draws its own
**	and Win32 did that by giving the window class a null cursor.
*/
Bool createAppWindow(Bool runWindowed);
void destroyAppWindow(void);

/*
**	serviceWindowsOS(). Drains the platform's queue and applies each event the way the
**	corresponding WndProc case does, then hands input events to the device layer's queues below.
**	Unlike DispatchMessage() this runs entirely at the point of the call.
*/
void serviceOS(void);

/*
**	IsIconic() and the isWinMainActive flag that WndProc maintained from WM_ACTIVATEAPP.
*/
Bool isMinimized(void);
Bool isActive(void);

/*
**	MSG::time for the event being processed, which WndProc smuggled to the mouse through the
**	global TheMessageTime.
*/
UnsignedInt getMessageTime(void);

/*
**	Input handoff to the device layer. Win32 pushed into Win32Mouse from inside WndProc and read
**	the keyboard from DirectInput; here both are queues the device drains, so that the seam has
**	exactly one producer and the translation to MouseIO/KeyboardIO stays in the device layer.
**	Both return false when the queue is empty.
*/
Bool getNextMouseEvent(WWPlatform::WindowEvent & event);
Bool getNextKeyEvent(WWPlatform::WindowEvent & event);

/*
**	GetKeyState(VK_CAPITAL) and friends, as of the last serviceOS().
*/
Bool isCapsLockOn(void);
UnsignedInt getModifierState(void);

/*
**	ClipCursor()/SetCursorPos() and GetClientRect(), for Mouse::capture() and the mouse limits.
*/
void setCursorClip(Bool clip);
void warpCursor(Int x, Int y);
void getClientSize(Int & width, Int & height);

/*
**	SetWindowPos()+ShowWindow() as W3DDisplay::setDisplayMode uses them. No video mode change:
**	see docs/porting/window-event-loop.md.
*/
Bool setDisplayMode(Int width, Int height, Bool fullscreen);

}	// namespace PlatformWindowHost

#endif // !_WIN32
