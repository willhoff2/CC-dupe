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

// FILE: W3DGameEngine.cpp ////////////////////////////////////////////////////////////////////////
// Author: Colin Day, April 2001
// Description:
//   Implementation of the Win32 game engine, this is the highest level of
//   the game application, it creates all the devices we will use for the game
///////////////////////////////////////////////////////////////////////////////////////////////////

#ifdef _WIN32
#include <windows.h>
#else
// TheSuperHackers @port The window/message/input seam, see docs/porting/window-event-loop.md.
#include "GameClient/PlatformWindowHost.h"
#endif

#include "Win32Device/Common/Win32GameEngine.h"
#include "Common/PerfTimer.h"

#include "GameNetwork/LANAPICallbacks.h"

#ifdef _WIN32
extern DWORD TheMessageTime;
#endif

//-------------------------------------------------------------------------------------------------
/** IsIconic() on the application window. Off Windows the window seam answers, because there is
	* no HWND to ask. */
//-------------------------------------------------------------------------------------------------
static Bool isApplicationWindowIconic()
{
#ifdef _WIN32
	extern HWND ApplicationHWnd;
	return ApplicationHWnd != nullptr && ::IsIconic(ApplicationHWnd);
#else
	return PlatformWindowHost::isMinimized();
#endif
}

//-------------------------------------------------------------------------------------------------
/** Constructor for Win32GameEngine */
//-------------------------------------------------------------------------------------------------
Win32GameEngine::Win32GameEngine()
{
#ifdef _WIN32
	// Stop blue screen
	m_previousErrorMode = SetErrorMode( SEM_FAILCRITICALERRORS );
#else
	// TheSuperHackers @port SetErrorMode() suppresses the Win32 "there is no disk in the drive"
	// dialog. Nothing off Windows shows such a dialog, so there is nothing to suppress.
	m_previousErrorMode = 0;
#endif
}

//-------------------------------------------------------------------------------------------------
/** Destructor for Win32GameEngine */
//-------------------------------------------------------------------------------------------------
Win32GameEngine::~Win32GameEngine()
{
#ifdef _WIN32
	// restore it (this isn't really necessary, but feels good.)
	SetErrorMode( m_previousErrorMode );
#endif
}


//-------------------------------------------------------------------------------------------------
/** Initialize the game engine */
//-------------------------------------------------------------------------------------------------
void Win32GameEngine::init()
{

	// extending functionality
	GameEngine::init();

}

//-------------------------------------------------------------------------------------------------
/** Reset the system */
//-------------------------------------------------------------------------------------------------
void Win32GameEngine::reset()
{

	// extending functionality
	GameEngine::reset();

}

//-------------------------------------------------------------------------------------------------
/** Update the game engine by updating the GameClient and
	* GameLogic singletons. */
//-------------------------------------------------------------------------------------------------
void Win32GameEngine::update()
{


	// call the engine normal update
	GameEngine::update();

	if (isApplicationWindowIconic()) {
		while (isApplicationWindowIconic()) {
			// We are alt-tabbed out here.  Sleep a bit, & process windows
			// so that we can become un-alt-tabbed out.
			Sleep(5);
			serviceWindowsOS();

			if (TheLAN != nullptr) {
				// BGC - need to update TheLAN so we can process and respond to other
				// people's messages who may not be alt-tabbed out like we are.
				TheLAN->setIsActive(isActive());
				TheLAN->update();
			}

			// If we are running a multiplayer game, keep running the logic.
			// There is code in the client to skip client redraw if we are
			// iconic.  jba.
			if (TheGameEngine->getQuitting() || TheGameLogic->isInInternetGame() || TheGameLogic->isInLanGame()) {
				break; // keep running.
			}
		}

    // When we are alt-tabbed out... the MilesAudioManager seems to go into a coma sometimes
    // and not regain focus properly when we come back. This seems to wake it up nicely.
    AudioAffect aa = (AudioAffect)0x10;
		TheAudio->setVolume(TheAudio->getVolume( aa ), aa );

	}

	// allow windows to perform regular windows maintenance stuff like msgs
	serviceWindowsOS();

}

//-------------------------------------------------------------------------------------------------
/** This function may be called from within this application to let
  * Microsoft Windows do its message processing and dispatching.  Presumably
	* we would call this at least once each time around the game loop to keep
	* Windows services from backing up */
//-------------------------------------------------------------------------------------------------
void Win32GameEngine::serviceWindowsOS()
{
#ifndef _WIN32

	// TheSuperHackers @port There is no message queue and no WndProc off Windows: the events are
	// pulled from the platform here and applied at this point in the frame, rather than being
	// dispatched to a callback at an arbitrary stack depth. docs/porting/window-event-loop.md
	// records what that costs.
	PlatformWindowHost::serviceOS();

#else

	MSG msg;
  Int returnValue;

	//
	// see if we have any messages to process, a nullptr window handle tells the
	// OS to look at the main window associated with the calling thread, us!
	//
	while( PeekMessage( &msg, nullptr, 0, 0, PM_NOREMOVE ) )
	{

		// get the message
		returnValue = GetMessage( &msg, nullptr, 0, 0 );

		// this is one possible way to check for quitting conditions as a message
		// of WM_QUIT will cause GetMessage() to return 0
/*
		if( returnValue == 0 )
		{

			setQuitting( true );
			break;

		}
*/

		TheMessageTime = msg.time;
		// translate and dispatch the message
		TranslateMessage( &msg );
		DispatchMessage( &msg );
		TheMessageTime = 0;

	}

#endif // !_WIN32
}

