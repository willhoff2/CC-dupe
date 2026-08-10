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

// FILE: INIWebpageURL.cpp /////////////////////////////////////////////////////////////////////////////
// Author: Bryan Cleveland, November 2001
// Desc:   Parsing Webpage URL INI entries
///////////////////////////////////////////////////////////////////////////////////////////////////

// INCLUDES ///////////////////////////////////////////////////////////////////////////////////////
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

// TheSuperHackers @port Current directory moved behind the platform path API;
// see docs/porting/filesystem-and-registry.md
#include "WWLib/platform/platform_path.h"

#include "Common/INI.h"
#include "Common/Registry.h"
#include "GameNetwork/WOLBrowser/WebBrowser.h"


///////////////////////////////////////////////////////////////////////////////////////////////////
// PRIVATE DATA ///////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////////////////////////
// PUBLIC FUNCTIONS ///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////

AsciiString encodeURL(AsciiString source)
{
	if (source.isEmpty())
	{
		return AsciiString::TheEmptyString;
	}

	AsciiString target;
	AsciiString allowedChars = "$-_.+!*'(),\\";
	const char *ptr = source.str();
	while (*ptr)
	{
		if (isalnum(*ptr) || allowedChars.find(*ptr))
		{
			target.concat(*ptr);
		}
		else
		{
			AsciiString tmp;
			target.concat('%');
			tmp.format("%2.2x", ((int)*ptr));
			target.concat(tmp);
		}
		++ptr;
	}

	return target;
}

//-------------------------------------------------------------------------------------------------
/** Parse Music entry */
//-------------------------------------------------------------------------------------------------
void INI::parseWebpageURLDefinition( INI* ini )
{
	AsciiString tag;
	WebBrowserURL *url;

	// read the name
	const char* c = ini->getNextToken();
	tag.set( c );

	if (TheWebBrowser != nullptr)
	{
		url = TheWebBrowser->findURL(tag);

		if (url == nullptr)
		{
			url = TheWebBrowser->makeNewURL(tag);
		}
	}

	// find existing item if present
//	track = TheAudio->Music->getTrack( name );
//	if( track == nullptr )
//	{

		// allocate a new track
//		track = TheAudio->Music->newMusicTrack( name );

//	}  // end if

//	DEBUG_ASSERTCRASH( track, ("parseMusicTrackDefinition: Unable to allocate track '%s'",
//										 name.str()) );

	// parse the ini definition
	ini->initFromINI( url, url->getFieldParse() );

	if (url->m_url.startsWith("file://"))
	{
		char cwd[_MAX_PATH] = { WWPlatform::Path::SEPARATOR, 0 };
		WWPlatform::Path::Get_Current_Directory(cwd, _MAX_PATH);

		url->m_url.format("file://%s%cData%c%s%c%s", encodeURL(cwd).str(),
			WWPlatform::Path::SEPARATOR, WWPlatform::Path::SEPARATOR, GetRegistryLanguage().str(),
			WWPlatform::Path::SEPARATOR, url->m_url.str()+7);
		DEBUG_LOG(("INI::parseWebpageURLDefinition() - converted URL to [%s]", url->m_url.str()));
	}
}


