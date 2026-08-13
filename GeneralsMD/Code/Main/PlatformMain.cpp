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

// FILE: PlatformMain.cpp /////////////////////////////////////////////////////////////////////////
//
// TheSuperHackers @port The non-Windows twin of WinMain.cpp: main() instead of WinMain(), the
// window seam instead of RegisterClass()/CreateWindow(), and no WndProc at all - the events are
// pulled in Win32GameEngine::serviceWindowsOS() through PlatformWindowHost::serviceOS(). Only the
// globals and the startup order WinMain.cpp is responsible for live here; everything that WndProc
// did lives in Core/GameEngine/Source/GameClient/PlatformWindowHost.cpp.
//
// The pieces WinMain.cpp does that are deliberately absent, all triaged in
// docs/porting/window-event-loop.md: the splash bitmap (a Win32 HBITMAP blitted from WM_PAINT),
// SetUnhandledExceptionFilter(), the CRT debug heap flags, and raising an already running
// instance's window through FindWindow().
//
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "Lib/BaseType.h"
#include "Common/CommandLine.h"
#include "Common/CriticalSection.h"
#include "Common/Debug.h"
#include "Common/GameEngine.h"
#include "Common/GameMemory.h"
#include "Common/GlobalData.h"
#include "Common/version.h"
#include "GameClient/ClientInstance.h"
#include "GameClient/PlatformWindowHost.h"
#include "Win32Device/Common/Win32GameEngine.h"
#include "Win32Device/GameClient/Win32Mouse.h"
#include "BuildVersion.h"
#include "GeneratedVersion.h"

#ifdef RTS_ENABLE_CRASHDUMP
#include "Common/MiniDumper.h"
#endif

#include <string.h>
#include <unistd.h>


// GLOBALS ////////////////////////////////////////////////////////////////////
// The same globals WinMain.cpp defines, so that the engine's externs resolve either way.
// ApplicationHWnd itself belongs to PlatformWindowHost.cpp, which owns the window.
Win32Mouse *TheWin32Mouse = nullptr;	///< the mouse the window seam feeds

const Char *g_strFile = "data\\Generals.str";
const Char *g_csfFile = "data\\%s\\Generals.csf";
const char *gAppPrefix = ""; /// So WB can have a different debug log file name.

static CriticalSection critSec1, critSec2, critSec3, critSec4, critSec5;


// main =======================================================================
/** Application entry point */
//=============================================================================
int main( int argc, char *argv[] )
{
	Int exitcode = 1;

	TheAsciiStringCriticalSection = &critSec1;
	TheUnicodeStringCriticalSection = &critSec2;
	TheDmaCriticalSection = &critSec3;
	TheMemoryPoolCriticalSection = &critSec4;
	TheDebugLogCriticalSection = &critSec5;

	// initialize the memory manager early
	initMemoryManager();

	// WinMain forces the working directory to the one holding the .exe, because DevStudio would
	// not. argv[0]'s directory is the equivalent of GetModuleFileName() here.
	if( argc > 0 && argv[0] != nullptr )
	{
		char directory[ _MAX_PATH ];
		strlcpy( directory, argv[0], ARRAY_SIZE(directory) );
		if( char *end = strrchr( directory, '/' ) )
		{
			*end = 0;
			if( chdir( directory ) != 0 )
			{
				DEBUG_LOG(( "Could not change the working directory to '%s'", directory ));
			}
		}
	}

	CommandLine::parseCommandLineForStartup();

#ifdef RTS_ENABLE_CRASHDUMP
	// requires TheGlobalData, so performed after parseCommandLineForStartup
	MiniDumper::initMiniDumper(TheGlobalData->getPath_UserData());
#endif

	// create the application window, the seam's equivalent of initializeAppWindows()
	if( !TheGlobalData->m_headless
			&& PlatformWindowHost::createAppWindow( TheGlobalData->m_windowed ) == FALSE )
	{
		shutdownMemoryManager();
		return exitcode;
	}

	// Set up version info
	TheVersion = NEW Version;
	TheVersion->setVersion(VERSION_MAJOR, VERSION_MINOR, VERSION_BUILDNUM, VERSION_LOCALBUILDNUM,
		AsciiString(VERSION_BUILDUSER), AsciiString(VERSION_BUILDLOC),
		AsciiString(__TIME__), AsciiString(__DATE__));

	if (!rts::ClientInstance::initialize())
	{
		// WinMain raises the running instance's window with FindWindow(); there is no equivalent
		// through the seam, so the second instance only says so.
		DEBUG_LOG(("Generals is already running...Bail!"));
		delete TheVersion;
		TheVersion = nullptr;
		PlatformWindowHost::destroyAppWindow();
		shutdownMemoryManager();
		return exitcode;
	}

	DEBUG_LOG(("CRC message is %d", GameMessage::MSG_LOGIC_CRC));

	// run the game main loop
	exitcode = GameMain();

	delete TheVersion;
	TheVersion = nullptr;

#ifdef MEMORYPOOL_DEBUG
	TheMemoryPoolFactory->debugMemoryReport(REPORT_POOLINFO | REPORT_POOL_OVERFLOW | REPORT_SIMPLE_LEAKS, 0, 0);
#endif
#if defined(RTS_DEBUG)
	TheMemoryPoolFactory->memoryPoolUsageReport("AAAMemStats");
#endif

	PlatformWindowHost::destroyAppWindow();

	shutdownMemoryManager();

#ifdef RTS_ENABLE_CRASHDUMP
	MiniDumper::shutdownMiniDumper();
#endif
	TheUnicodeStringCriticalSection = nullptr;
	TheDmaCriticalSection = nullptr;
	TheMemoryPoolCriticalSection = nullptr;

	return exitcode;
}

// CreateGameEngine ===========================================================
/** Create the game engine we're going to use */
//=============================================================================
GameEngine *CreateGameEngine()
{
	Win32GameEngine *engine;

	engine = NEW Win32GameEngine;
	//game engine may not have existed when the window got focus so make sure it
	//knows about the current focus state.
	engine->setIsActive(PlatformWindowHost::isActive());

	return engine;
}
