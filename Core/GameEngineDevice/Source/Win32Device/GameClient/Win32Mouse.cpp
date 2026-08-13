/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
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

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////

// FILE: Win32Mouse.cpp ///////////////////////////////////////////////////////////////////////////
// Created:    Colin Day, July 2001
// Desc:       Interface for the mouse using only the Win32 messages
///////////////////////////////////////////////////////////////////////////////////////////////////

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "Common/Debug.h"
#include "Common/GlobalData.h"
#include "Common/LocalFileSystem.h"
#include "GameClient/GameClient.h"
#include "Win32Device/GameClient/Win32Mouse.h"
#ifdef _WIN32
#include "WinMain.h"
#else
// TheSuperHackers @port The window seam replaces WinMain.cpp's WndProc as the source of the events
// buffered here, and ClipCursor()/GetClientRect() for the capture. The cursor image is still a
// Win32 HCURSOR loaded from an .ANI file, which has no portable equivalent - off Windows the
// game's own W3D cursor is the only one there is. See docs/porting/window-event-loop.md.
#include "GameClient/PlatformWindowHost.h"
#endif


// EXTERN /////////////////////////////////////////////////////////////////////////////////////////
extern Win32Mouse *TheWin32Mouse;

#ifndef _WIN32
typedef void * HCURSOR;
#endif

HCURSOR cursorResources[Mouse::NUM_MOUSE_CURSORS][MAX_2D_CURSOR_DIRECTIONS];
///////////////////////////////////////////////////////////////////////////////////////////////////
// PRIVATE FUNCTIONS //////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------------------------------------
/** Get a mouse event from the buffer if available, we need to translate
	* from the windows message meanings to our own internal mouse
	* structure */
//-------------------------------------------------------------------------------------------------
UnsignedByte Win32Mouse::getMouseEvent( MouseIO *result, Bool flush )
{

	// if there is nothing here there is no event data to do
#ifdef _WIN32
	if( m_eventBuffer[ m_nextGetIndex ].msg == 0 )
#else
	if( m_eventBuffer[ m_nextGetIndex ].event.Type == WWPlatform::WINDOW_EVENT_NONE )
#endif
		return MOUSE_NONE;

	// translate the win32 mouse message to our own system
	translateEvent( m_nextGetIndex, result );

	// remove this event from the buffer by setting msg to zero
#ifdef _WIN32
	m_eventBuffer[ m_nextGetIndex ].msg = 0;
#else
	m_eventBuffer[ m_nextGetIndex ].event.Type = WWPlatform::WINDOW_EVENT_NONE;
#endif

	//
	// our next get index will now be advanced to the next index, wrapping at
	// the mad
	//
	m_nextGetIndex++;
	if( m_nextGetIndex >= Mouse::NUM_MOUSE_EVENTS )
		m_nextGetIndex = 0;

	// got event OK and all done with this one
	return MOUSE_OK;

}

//-------------------------------------------------------------------------------------------------
/** Translate a win32 mouse event to our own event info */
//-------------------------------------------------------------------------------------------------
#ifndef _WIN32
void Win32Mouse::translateEvent( UnsignedInt eventIndex, MouseIO *result )
{
	const WWPlatform::WindowEvent & event = m_eventBuffer[ eventIndex ].event;

	// set these to defaults
	result->leftState = result->middleState = result->rightState = MBS_None;
	result->pos.x = result->pos.y = result->wheelPos = 0;

	// Time is the same for all events
	result->time = event.Time_Ms;

	// The seam reports client coordinates already, which is what LOWORD/HIWORD of lParam gave for
	// every message except the wheel, and it converts the wheel's position for us.
	result->pos.x = event.Mouse_X;
	result->pos.y = event.Mouse_Y;

	MouseButtonState state = MBS_None;

	switch( event.Type )
	{
		case WWPlatform::WINDOW_EVENT_MOUSE_MOVE:
			return;

		case WWPlatform::WINDOW_EVENT_MOUSE_WHEEL:
			// Win32Mouse divided WM_MOUSEWHEEL's WHEEL_DELTA units down; the seam keeps those units.
			result->wheelPos = event.Wheel_Delta;
			return;

		case WWPlatform::WINDOW_EVENT_MOUSE_DOWN:
			// The double click messages were separate WM_*BUTTONDBLCLK messages; the seam counts the
			// clicks on the press instead.
			state = (event.Click_Count > 1) ? MBS_DoubleClick : MBS_Down;
			break;

		case WWPlatform::WINDOW_EVENT_MOUSE_UP:
			state = MBS_Up;
			break;

		default:
			DEBUG_CRASH(( "translateEvent: Unknown window event [%d]", event.Type ));
			return;
	}

	switch( event.Mouse_Button )
	{
		case WWPlatform::MOUSE_BUTTON_LEFT:
			result->leftState = state;
			break;

		case WWPlatform::MOUSE_BUTTON_MIDDLE:
			result->middleState = state;
			break;

		case WWPlatform::MOUSE_BUTTON_RIGHT:
			result->rightState = state;
			break;

		default:
			// The seam reports buttons the game has no use for, e.g. the thumb buttons.
			break;
	}
}
#else
void Win32Mouse::translateEvent( UnsignedInt eventIndex, MouseIO *result )
{
	UINT msg = m_eventBuffer[ eventIndex ].msg;
	WPARAM wParam = m_eventBuffer[ eventIndex ].wParam;
	LPARAM lParam = m_eventBuffer[ eventIndex ].lParam;

	// set these to defaults
	result->leftState = result->middleState = result->rightState = MBS_None;
	result->pos.x = result->pos.y = result->wheelPos = 0;

	// Time is the same for all events
	result->time = m_eventBuffer[ eventIndex ].time;

	switch( msg )
	{

		// ------------------------------------------------------------------------
		case WM_LBUTTONDOWN:
		{

			result->leftState = MBS_Down;
			result->pos.x = LOWORD( lParam );
			result->pos.y = HIWORD( lParam );
			break;

		}

		// ------------------------------------------------------------------------
		case WM_LBUTTONUP:
		{

			result->leftState = MBS_Up;
			result->pos.x = LOWORD( lParam );
			result->pos.y = HIWORD( lParam );
			break;

		}

		// ------------------------------------------------------------------------
		case WM_LBUTTONDBLCLK:
		{

			result->leftState = MBS_DoubleClick;
			result->pos.x = LOWORD( lParam );
			result->pos.y = HIWORD( lParam );
			break;

		}

		// ------------------------------------------------------------------------
		case WM_MBUTTONDOWN:
		{

			result->middleState = MBS_Down;
			result->pos.x = LOWORD( lParam );
			result->pos.y = HIWORD( lParam );
			break;

		}

		// ------------------------------------------------------------------------
		case WM_MBUTTONUP:
		{

			result->middleState = MBS_Up;
			result->pos.x = LOWORD( lParam );
			result->pos.y = HIWORD( lParam );
			break;

		}

		// ------------------------------------------------------------------------
		case WM_MBUTTONDBLCLK:
		{

			result->middleState = MBS_DoubleClick;
			result->pos.x = LOWORD( lParam );
			result->pos.y = HIWORD( lParam );
			break;

		}

		// ------------------------------------------------------------------------
		case WM_RBUTTONDOWN:
		{

			result->rightState = MBS_Down;
			result->pos.x = LOWORD( lParam );
			result->pos.y = HIWORD( lParam );
			break;

		}

		// ------------------------------------------------------------------------
		case WM_RBUTTONUP:
		{

			result->rightState = MBS_Up;
			result->pos.x = LOWORD( lParam );
			result->pos.y = HIWORD( lParam );
			break;

		}

		// ------------------------------------------------------------------------
		case WM_RBUTTONDBLCLK:
		{

			result->rightState = MBS_DoubleClick;
			result->pos.x = LOWORD( lParam );
			result->pos.y = HIWORD( lParam );
			break;

		}

		// ------------------------------------------------------------------------
		case WM_MOUSEMOVE:
		{

			result->pos.x = LOWORD( lParam );
			result->pos.y = HIWORD( lParam );
			break;

		}

		// ------------------------------------------------------------------------
		case 0x020A:	// WM_MOUSEWHEEL
		{
			POINT p;

			// translate the screen mouse position to be relative to the application window
			p.x = LOWORD( lParam );
			p.y = HIWORD( lParam );
			ScreenToClient( ApplicationHWnd, &p );

			// note the short cast here to keep signed information in tact
			result->wheelPos = (Short)HIWORD( wParam );
			result->pos.x = p.x;
			result->pos.y = p.y;
			break;

		}

		// ------------------------------------------------------------------------
		default:
		{

			DEBUG_CRASH(( "translateEvent: Unknown Win32 mouse event [%d,%d,%d]",
							 msg, wParam, lParam ));
			return;

		}

	}

}
#endif // !_WIN32

///////////////////////////////////////////////////////////////////////////////////////////////////
// PUBLIC FUNCTIONS ///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
Win32Mouse::Win32Mouse()
{

	// zero our event list
	memset( &m_eventBuffer, 0, sizeof( m_eventBuffer ) );

	m_nextFreeIndex = 0;
	m_nextGetIndex = 0;
	m_currentWin32Cursor = NONE;
	for (Int i=0; i<NUM_MOUSE_CURSORS; i++)
		for (Int j=0; j<MAX_2D_CURSOR_DIRECTIONS; j++)
			cursorResources[i][j]=nullptr;
	m_directionFrame=0; //points up.
	m_lostFocus = FALSE;
}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
Win32Mouse::~Win32Mouse()
{

	// remove our global reference that was for the WndProc() only
	TheWin32Mouse = nullptr;

}

//-------------------------------------------------------------------------------------------------
/** Initialize our device */
//-------------------------------------------------------------------------------------------------
void Win32Mouse::init()
{

	// extending functionality
	Mouse::init();

	//
	// when we receive messages from a Windows message procedure, the mouse
	// moves report the current cursor position and not deltas, our mouse
	// needs to process those positions as absolute and not relative
	//
	m_inputMovesAbsolute = TRUE;

}

//-------------------------------------------------------------------------------------------------
/** Reset */
//-------------------------------------------------------------------------------------------------
void Win32Mouse::reset()
{

	// extend
	Mouse::reset();

}

//-------------------------------------------------------------------------------------------------
/** Update, called once per frame */
//-------------------------------------------------------------------------------------------------
void Win32Mouse::update()
{

#ifndef _WIN32
	// TheSuperHackers @port The pull side of the seam. On Windows WndProc pushes into the buffer at
	// whatever moment the message arrives; here the events are collected once per frame, from the
	// queue Win32GameEngine::serviceWindowsOS() filled.
	WWPlatform::WindowEvent event;
	while( PlatformWindowHost::getNextMouseEvent( event ) )
		addWin32Event( event );
#endif

	// extend
	Mouse::update();

}

//-------------------------------------------------------------------------------------------------
/** Add a window message event along with its WPARAM and LPARAM parameters
	* to our input storage buffer */
//-------------------------------------------------------------------------------------------------
#ifndef _WIN32
void Win32Mouse::addWin32Event( const WWPlatform::WindowEvent & event )
{

	if( m_eventBuffer[ m_nextFreeIndex ].event.Type != WWPlatform::WINDOW_EVENT_NONE )
		return;	// buffer full, this input event is lost

	m_eventBuffer[ m_nextFreeIndex ].event = event;
#else
void Win32Mouse::addWin32Event( UINT msg, WPARAM wParam, LPARAM lParam, DWORD time )
{

	//
	// we can only add this event if our next free index does not already
	// have an event in it, if it does ... our buffer is full and this input
	// event will be lost
	//
	if( m_eventBuffer[ m_nextFreeIndex ].msg != 0 )
		return;

	// add to this index
	m_eventBuffer[ m_nextFreeIndex ].msg = msg;
	m_eventBuffer[ m_nextFreeIndex ].wParam = wParam;
	m_eventBuffer[ m_nextFreeIndex ].lParam = lParam;
	m_eventBuffer[ m_nextFreeIndex ].time = time;
#endif

	// wrap index at max
	m_nextFreeIndex++;
	if( m_nextFreeIndex >= Mouse::NUM_MOUSE_EVENTS )
		m_nextFreeIndex = 0;

}

#ifdef _WIN32
extern HINSTANCE ApplicationHInstance;
#endif

void Win32Mouse::setVisibility(Bool visible)
{
	//Extend
	Mouse::setVisibility(visible);
	//Maybe need to set cursor to force hiding of some cursors.
	Win32Mouse::setCursor(getMouseCursor());
}

//-------------------------------------------------------------------------------------------------
void Win32Mouse::loseFocus()
{
	Mouse::loseFocus();
	m_lostFocus = true;
}

//-------------------------------------------------------------------------------------------------
void Win32Mouse::regainFocus()
{
	Mouse::regainFocus();
	m_lostFocus = false;
}

/**Preload all the cursors we may need during the game.  This must be done before the D3D device
is created to avoid cursor corruption on buggy ATI Radeon cards. */
void Win32Mouse::initCursorResources()
{
#ifndef _WIN32

	// TheSuperHackers @port LoadCursorFromFile() reads a Win32 .ANI, and SetCursor() hands the
	// system cursor to the window manager. Neither has an equivalent through the seam, and neither
	// is needed: the game's own RM_W3D/RM_POLYGON cursor is drawn by the renderer. The system
	// cursor stays hidden, as the Win32 window class made it.
	return;

#else

	for (Int cursor=FIRST_CURSOR; cursor<NUM_MOUSE_CURSORS; cursor++)
	{
		for (Int direction=0; direction<m_cursorInfo[cursor].numDirections; direction++)
		{	if (!cursorResources[cursor][direction] && !m_cursorInfo[cursor].textureName.isEmpty())
			{	//this cursor has never been loaded before.
				char resourcePath[256];
				//Check if this is a directional cursor
				if (m_cursorInfo[cursor].numDirections > 1)
					snprintf(resourcePath, ARRAY_SIZE(resourcePath), "data\\cursors\\%s%d.ANI",
						m_cursorInfo[cursor].textureName.str(), direction);
				else
					snprintf(resourcePath, ARRAY_SIZE(resourcePath), "data\\cursors\\%s.ANI",
						m_cursorInfo[cursor].textureName.str());

				// check for a MOD cursor.
				Bool loaded = FALSE;
				if (TheGlobalData->m_modDir.isNotEmpty())
				{
					AsciiString fname;
					if (m_cursorInfo[cursor].numDirections > 1)
						fname.format("%sdata\\cursors\\%s%d.ANI", TheGlobalData->m_modDir.str(), m_cursorInfo[cursor].textureName.str(), direction);
					else
						fname.format("%sdata\\cursors\\%s.ANI", TheGlobalData->m_modDir.str(), m_cursorInfo[cursor].textureName.str());

					if (TheLocalFileSystem->doesFileExist(fname.str()))
					{
						cursorResources[cursor][direction]=LoadCursorFromFile(fname.str());
						loaded = TRUE;
					}
				}

				if (!loaded)
					cursorResources[cursor][direction]=LoadCursorFromFile(resourcePath);
				DEBUG_ASSERTCRASH(cursorResources[cursor][direction], ("MissingCursor %s",resourcePath));
			}
		}
//		SetCursor(cursorResources[cursor][m_directionFrame]);
	}

#endif // !_WIN32
}

//-------------------------------------------------------------------------------------------------
/** Super basic simplistic cursor */
//-------------------------------------------------------------------------------------------------
void Win32Mouse::setCursor( MouseCursor cursor )
{
	// extend
	Mouse::setCursor( cursor );

	if (m_lostFocus)
		return;	//stop messing with mouse cursor if we don't have focus.

#ifdef _WIN32
	if (cursor == NONE || !m_visible)
		SetCursor( nullptr );
	else
	{
		SetCursor(cursorResources[cursor][m_directionFrame]);
	}
#endif

	// save current cursor
	m_currentWin32Cursor=m_currentCursor = cursor;

}

//-------------------------------------------------------------------------------------------------
/** Capture the mouse to our application */
//-------------------------------------------------------------------------------------------------
void Win32Mouse::capture()
{

#ifndef _WIN32

	// The seam clips to the window's client area itself, so the rect arithmetic is not needed.
	PlatformWindowHost::setCursorClip( TRUE );
	onCursorCaptured( true );

#else

	RECT rect;
	::GetClientRect(ApplicationHWnd, &rect);

	POINT leftTop;
	leftTop.x = rect.left;
	leftTop.y = rect.top;

	POINT rightBottom;
	rightBottom.x = rect.right;
	rightBottom.y = rect.bottom;

	::ClientToScreen(ApplicationHWnd, &leftTop);
	::ClientToScreen(ApplicationHWnd, &rightBottom);

	rect.left = leftTop.x;
	rect.top = leftTop.y;
	rect.right = rightBottom.x;
	rect.bottom = rightBottom.y;

	if (::ClipCursor(&rect))
	{
		onCursorCaptured(true);
	}

#endif // !_WIN32
}

//-------------------------------------------------------------------------------------------------
/** Release the mouse capture for our app window */
//-------------------------------------------------------------------------------------------------
void Win32Mouse::releaseCapture()
{

#ifndef _WIN32

	PlatformWindowHost::setCursorClip( FALSE );
	onCursorCaptured( false );

#else

	if (::ClipCursor(nullptr))
	{
		onCursorCaptured(false);
	}

#endif // !_WIN32
}
