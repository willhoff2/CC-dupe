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

// FILE: PlatformWindowHost.cpp ///////////////////////////////////////////////////////////////////
//
// TheSuperHackers @port The engine side of the window seam. Every branch of the switch below is
// one WndProc case from GeneralsMD/Code/Main/WinMain.cpp, and the cases that are absent are
// absent on purpose - the triage table in docs/porting/window-event-loop.md says which and why.
//
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"

#ifndef _WIN32

#include "GameClient/PlatformWindowHost.h"

#include "Common/GameAudio.h"
#include "Common/GameDefines.h"
#include "Common/GameEngine.h"
#include "Common/GlobalData.h"
#include "Common/MessageStream.h"
#include "GameClient/Keyboard.h"
#include "GameClient/Mouse.h"

// The window handle, under the Win32 name WinMain.cpp defines it with, so that the consumers do
// not have to know which platform they are on.
void * ApplicationHWnd = nullptr;

namespace PlatformWindowHost
{

namespace
{

// One event queue per device, sized like Mouse::NUM_MOUSE_EVENTS: Win32 dropped mouse events on
// a full buffer too, and a queue that grows without bound while the game is loading is worse.
enum { MAX_QUEUED_EVENTS = 256 };

class EventQueue
{
public:
	EventQueue() : m_readIndex(0), m_writeIndex(0) {}

	void add(const WWPlatform::WindowEvent & event)
	{
		const Int nextWrite = (m_writeIndex + 1) % MAX_QUEUED_EVENTS;
		if (nextWrite == m_readIndex)
			return;	// full; drop, as addWin32Event() does

		m_events[m_writeIndex] = event;
		m_writeIndex = nextWrite;
	}

	Bool get(WWPlatform::WindowEvent & event)
	{
		if (m_readIndex == m_writeIndex)
			return FALSE;

		event = m_events[m_readIndex];
		m_readIndex = (m_readIndex + 1) % MAX_QUEUED_EVENTS;
		return TRUE;
	}

	void clear() { m_readIndex = m_writeIndex = 0; }

private:
	WWPlatform::WindowEvent m_events[MAX_QUEUED_EVENTS];
	Int m_readIndex;
	Int m_writeIndex;
};

EventQueue theMouseEvents;
EventQueue theKeyEvents;
Bool theIsActive = FALSE;
Bool theIsMinimized = FALSE;
UnsignedInt theMessageTime = 0;
UnsignedInt theModifiers = WWPlatform::WINDOW_MODIFIER_NONE;

// WM_CLOSE and WM_QUERYENDSESSION share this body; the seam reports both as a close, because a
// session ending and a close request are the same event to everything but Windows.
void requestQuit(void)
{
	if (TheGameEngine == nullptr || TheGameEngine->getQuitting())
		return;

	if (TheMessageStream != nullptr && TheMessageStream->isReadyForMessages())
		TheMessageStream->appendMessage(GameMessage::MSG_META_DEMO_INSTANT_QUIT);
	else
		TheGameEngine->setQuitting(TRUE);
}

// WM_SETFOCUS, WM_KILLFOCUS, WM_ACTIVATEAPP and the WA_INACTIVE test of WM_ACTIVATE collapse into
// this, because the seam reports one focus transition where Win32 reports up to three. The guard
// on the current state is WM_ACTIVATEAPP's, and it is what makes the collapse safe: the bodies
// are idempotent and none of them ordered themselves against the others.
void setActive(Bool active)
{
	if (active == theIsActive)
		return;

	theIsActive = active;

	if (TheKeyboard != nullptr)
		TheKeyboard->resetKeys();

	if (TheGameEngine != nullptr)
		TheGameEngine->setIsActive(theIsActive);

	if (active)
	{
		if (TheMouse != nullptr)
		{
			TheMouse->regainFocus();
			// Cursor can only be captured after one of the activation events.
			TheMouse->refreshCursorCapture();
			// restore mouse cursor to our custom version.
			TheMouse->setCursor(TheMouse->getMouseCursor());
		}

		if (TheAudio != nullptr)
			TheAudio->unmuteAudio(AudioManager::MuteAudioReason_WindowFocus);
	}
	else
	{
		theKeyEvents.clear();
		theMouseEvents.clear();

		if (TheMouse != nullptr)
		{
			TheMouse->loseFocus();

			if (TheMouse->isCursorInside())
				TheMouse->onCursorMovedOutside();
		}

		if (TheAudio != nullptr)
			TheAudio->muteAudio(AudioManager::MuteAudioReason_WindowFocus);
	}
}

void handleEvent(const WWPlatform::WindowEvent & event)
{
	switch (event.Type)
	{
		// ------------------------------------------------------------------------ WM_CLOSE
		case WWPlatform::WINDOW_EVENT_CLOSE:
			requestQuit();
			break;

		// ------------------------------------------------------- WM_MOVE, WM_SIZE
		case WWPlatform::WINDOW_EVENT_MOVE:
		case WWPlatform::WINDOW_EVENT_RESIZE:
			if (TheMouse != nullptr)
				TheMouse->refreshCursorCapture();
			break;

		// -------------------------------------------- the SIZE_MINIMIZED case of WM_SIZE
		case WWPlatform::WINDOW_EVENT_MINIMISED:
			theIsMinimized = TRUE;
			break;

		case WWPlatform::WINDOW_EVENT_RESTORED:
			theIsMinimized = FALSE;
			if (TheMouse != nullptr)
				TheMouse->refreshCursorCapture();
			break;

		// ----------------------------- WM_SETFOCUS, WM_KILLFOCUS, WM_ACTIVATEAPP, WM_ACTIVATE
		case WWPlatform::WINDOW_EVENT_FOCUS_GAINED:
			setActive(TRUE);
			break;

		case WWPlatform::WINDOW_EVENT_FOCUS_LOST:
			setActive(FALSE);
			break;

		// ----------------------------------------------------------------------- the keyboard
		// WndProc saw WM_KEYDOWN too, but only to catch Escape, and the DirectInput device is
		// what fed KeyboardIO. Here the same queue does both jobs.
		case WWPlatform::WINDOW_EVENT_KEY_DOWN:
		case WWPlatform::WINDOW_EVENT_KEY_UP:
		case WWPlatform::WINDOW_EVENT_KEY_DOWN:
		case WWPlatform::WINDOW_EVENT_KEY_UP:
			// DirectInput's buffered device data reports transitions only, and Keyboard::checkKeyRepeat()
			// synthesises the engine's own repeats, so the platform's auto-repeats are dropped here.
			if (event.Scan_Code != 0 && !event.Repeat)
				theKeyEvents.add(event);
			break;

		// -------------------------------------------------------------------------- the mouse
		case WWPlatform::WINDOW_EVENT_MOUSE_MOVE:
			// WndProc ignored mouse moves while the window was inactive, and hit-tested the
			// position against the client rect to derive enter/leave. The seam reports enter and
			// leave itself, so only the active test is left.
			if (!theIsActive)
				break;

			if (TheMouse != nullptr && !TheMouse->isCursorInside())
				TheMouse->onCursorMovedInside();

			theMouseEvents.add(event);
			break;

		case WWPlatform::WINDOW_EVENT_MOUSE_DOWN:
		case WWPlatform::WINDOW_EVENT_MOUSE_UP:
			theMouseEvents.add(event);
			break;

		case WWPlatform::WINDOW_EVENT_MOUSE_WHEEL:
			// WM_MOUSEWHEEL arrives in screen coordinates and WndProc dropped it when the
			// pointer was outside the window. The seam only reports it for our window.
			if (TheMouse == nullptr || TheMouse->isCursorInside())
				theMouseEvents.add(event);
			break;

		case WWPlatform::WINDOW_EVENT_MOUSE_ENTER:
			if (TheMouse != nullptr && !TheMouse->isCursorInside())
				TheMouse->onCursorMovedInside();
			break;

		case WWPlatform::WINDOW_EVENT_MOUSE_LEAVE:
			if (TheMouse != nullptr && TheMouse->isCursorInside())
				TheMouse->onCursorMovedOutside();
			break;

		// WINDOW_EVENT_TEXT has no consumer yet: the character that IMEManager and the layout
		// dependent part of Keyboard.cpp need is the IME slice, not this one.
		case WWPlatform::WINDOW_EVENT_TEXT:
		case WWPlatform::WINDOW_EVENT_NONE:
		default:
			break;
	}
}

}	// anonymous namespace

//-------------------------------------------------------------------------------------------------
Bool createAppWindow(Bool runWindowed)
{
	if (ApplicationHWnd != nullptr)
		return TRUE;

	WWPlatform::WindowConfig config;
	config.Title = "Command and Conquer Generals";
	config.Width = DEFAULT_DISPLAY_WIDTH;
	config.Height = DEFAULT_DISPLAY_HEIGHT;
	// "Fullscreen" is borderless: the Win32 path never changed the video mode either, it made a
	// screen sized WS_POPUP and reset the device.
	config.Fullscreen = !runWindowed;
	config.Resizable = FALSE;
	// The window class Win32 registered had a null cursor, because the game draws its own.
	config.Hide_System_Cursor = TRUE;

	ApplicationHWnd = WWPlatform::Window_Create(config);

	if (ApplicationHWnd == nullptr)
	{
		DEBUG_LOG(("ERROR - createAppWindow: %s backend failed: %s",
			WWPlatform::Window_Backend_Name(), WWPlatform::Window_Last_Error()));
		return FALSE;
	}

	theIsActive = WWPlatform::Window_Is_Active(ApplicationHWnd);
	theIsMinimized = WWPlatform::Window_Is_Minimised(ApplicationHWnd);

	if (TheGameEngine != nullptr)
		TheGameEngine->setIsActive(theIsActive);

	return TRUE;
}

//-------------------------------------------------------------------------------------------------
void destroyAppWindow(void)
{
	if (ApplicationHWnd == nullptr)
		return;

	WWPlatform::Window_Destroy(ApplicationHWnd);
	ApplicationHWnd = nullptr;
	theKeyEvents.clear();
	theMouseEvents.clear();
}

//-------------------------------------------------------------------------------------------------
void serviceOS(void)
{
	if (ApplicationHWnd == nullptr)
		return;

	WWPlatform::WindowEvent event;
	while (WWPlatform::Window_Poll_Event(ApplicationHWnd, event))
	{
		// TheMessageTime's job: the mouse stamps its events with the time of the message that
		// produced them.
		theMessageTime = event.Time_Ms;
		theModifiers = event.Modifiers;
		handleEvent(event);
	}

	theMessageTime = 0;
}

//-------------------------------------------------------------------------------------------------
Bool isMinimized(void)
{
	return ApplicationHWnd != nullptr && WWPlatform::Window_Is_Minimised(ApplicationHWnd);
}

//-------------------------------------------------------------------------------------------------
Bool isActive(void)
{
	return theIsActive;
}

//-------------------------------------------------------------------------------------------------
UnsignedInt getMessageTime(void)
{
	return theMessageTime;
}

//-------------------------------------------------------------------------------------------------
Bool getNextMouseEvent(WWPlatform::WindowEvent & event)
{
	return theMouseEvents.get(event);
}

//-------------------------------------------------------------------------------------------------
Bool getNextKeyEvent(WWPlatform::WindowEvent & event)
{
	return theKeyEvents.get(event);
}

//-------------------------------------------------------------------------------------------------
Bool isCapsLockOn(void)
{
	return (getModifierState() & WWPlatform::WINDOW_MODIFIER_CAPS_LOCK) != 0;
}

//-------------------------------------------------------------------------------------------------
UnsignedInt getModifierState(void)
{
	if (ApplicationHWnd == nullptr)
		return theModifiers;

	theModifiers = WWPlatform::Window_Modifier_State(ApplicationHWnd);
	return theModifiers;
}

//-------------------------------------------------------------------------------------------------
void setCursorClip(Bool clip)
{
	if (ApplicationHWnd != nullptr)
		WWPlatform::Window_Set_Cursor_Clip(ApplicationHWnd, clip != FALSE);
}

//-------------------------------------------------------------------------------------------------
void warpCursor(Int x, Int y)
{
	if (ApplicationHWnd != nullptr)
		WWPlatform::Window_Warp_Cursor(ApplicationHWnd, x, y);
}

//-------------------------------------------------------------------------------------------------
void getClientSize(Int & width, Int & height)
{
	width = DEFAULT_DISPLAY_WIDTH;
	height = DEFAULT_DISPLAY_HEIGHT;

	if (ApplicationHWnd != nullptr)
		WWPlatform::Window_Client_Size(ApplicationHWnd, width, height);
}

//-------------------------------------------------------------------------------------------------
Bool setDisplayMode(Int width, Int height, Bool fullscreen)
{
	if (ApplicationHWnd == nullptr)
		return FALSE;

	return WWPlatform::Window_Set_Mode(ApplicationHWnd, width, height, fullscreen != FALSE)
		? TRUE : FALSE;
}

}	// namespace PlatformWindowHost

#endif // !_WIN32
