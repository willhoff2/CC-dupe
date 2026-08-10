/*
**	Command & Conquer Generals(tm)
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

// Registry.cpp
// Simple interface for storing/retrieving registry values
// Author: Matthew D. Campbell, December 2001

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

// TheSuperHackers @port Win32 header pushed down from PreRTS.h; see docs/porting/prerts-win32-surgery.md
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <winreg.h>
#else
// TheSuperHackers @port There is no registry off Windows; the same key paths become sections of a
// per user settings file. See docs/porting/filesystem-and-registry.md.
#include "WWLib/platform/platform_settings.h"
#endif

#include "Common/Registry.h"


#ifdef _WIN32

Bool  getStringFromRegistry(HKEY root, AsciiString path, AsciiString key, AsciiString& val)
{
	HKEY handle;
	unsigned char buffer[256];
	DWORD size = 256;
	DWORD type;
	int returnValue;

	if ((returnValue = RegOpenKeyEx( root, path.str(), 0, KEY_READ, &handle )) == ERROR_SUCCESS)
	{
		returnValue = RegQueryValueEx(handle, key.str(), nullptr, &type, (unsigned char *) &buffer, &size);
		RegCloseKey( handle );
	}

	if (returnValue == ERROR_SUCCESS)
	{
		val = (char *)buffer;
		return TRUE;
	}

	return FALSE;
}

Bool getUnsignedIntFromRegistry(HKEY root, AsciiString path, AsciiString key, UnsignedInt& val)
{
	HKEY handle;
	unsigned char buffer[4];
	DWORD size = 4;
	DWORD type;
	int returnValue;

	if ((returnValue = RegOpenKeyEx( root, path.str(), 0, KEY_READ, &handle )) == ERROR_SUCCESS)
	{
		returnValue = RegQueryValueEx(handle, key.str(), nullptr, &type, (unsigned char *) &buffer, &size);
		RegCloseKey( handle );
	}

	if (returnValue == ERROR_SUCCESS)
	{
		val = *(UnsignedInt *)buffer;
		return TRUE;
	}

	return FALSE;
}

Bool setStringInRegistry( HKEY root, AsciiString path, AsciiString key, AsciiString val)
{
	HKEY handle;
	DWORD type;
	unsigned long returnValue;
	int size;
	char lpClass[] = "REG_NONE";

	if ((returnValue = RegCreateKeyEx( root, path.str(), 0, lpClass, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &handle, nullptr )) == ERROR_SUCCESS)
	{
		type = REG_SZ;
		size = val.getLength()+1;
		returnValue = RegSetValueEx(handle, key.str(), 0, type, (unsigned char *)val.str(), size);
		RegCloseKey( handle );
	}

	return (returnValue == ERROR_SUCCESS);
}

Bool setUnsignedIntInRegistry( HKEY root, AsciiString path, AsciiString key, UnsignedInt val)
{
	HKEY handle;
	DWORD type;
	unsigned long returnValue;
	int size;
	char lpClass[] = "REG_NONE";

	if ((returnValue = RegCreateKeyEx( root, path.str(), 0, lpClass, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &handle, nullptr )) == ERROR_SUCCESS)
	{
		type = REG_DWORD;
		size = 4;
		returnValue = RegSetValueEx(handle, key.str(), 0, type, (unsigned char *)&val, size);
		RegCloseKey( handle );
	}

	return (returnValue == ERROR_SUCCESS);
}

#else	// !_WIN32

//
// There is one settings store per user off Windows, so the two registry roots collapse into it:
// a read of HKEY_CURRENT_USER goes to the store and the HKEY_LOCAL_MACHINE fallback that follows
// every read here has nothing further to look in. That is deliberate -- HKLM held values written
// by the retail installer (install path, language, SKU, version) and there is no installer, and
// no machine wide store, on a native build.
//
static Bool getStringFromStore(AsciiString path, AsciiString key, AsciiString& val)
{
	int handle = WWPlatform::Settings::Open_Key(path.str(), false);
	if (handle == 0)
		return FALSE;

	StringClass value;
	Bool found = WWPlatform::Settings::Get_String(handle, key.str(), value) ? TRUE : FALSE;
	WWPlatform::Settings::Close_Key(handle);

	if (found)
		val = value.str();

	return found;
}

static Bool getUnsignedIntFromStore(AsciiString path, AsciiString key, UnsignedInt& val)
{
	int handle = WWPlatform::Settings::Open_Key(path.str(), false);
	if (handle == 0)
		return FALSE;

	int value = 0;
	Bool found = WWPlatform::Settings::Get_Int(handle, key.str(), value) ? TRUE : FALSE;
	WWPlatform::Settings::Close_Key(handle);

	if (found)
		val = (UnsignedInt)value;

	return found;
}

Bool setStringInStore(AsciiString path, AsciiString key, AsciiString val)
{
	int handle = WWPlatform::Settings::Open_Key(path.str(), true);
	if (handle == 0)
		return FALSE;

	WWPlatform::Settings::Set_String(handle, key.str(), val.str());
	WWPlatform::Settings::Close_Key(handle);
	return TRUE;
}

Bool setUnsignedIntInStore(AsciiString path, AsciiString key, UnsignedInt val)
{
	int handle = WWPlatform::Settings::Open_Key(path.str(), true);
	if (handle == 0)
		return FALSE;

	WWPlatform::Settings::Set_Int(handle, key.str(), (int)val);
	WWPlatform::Settings::Close_Key(handle);
	return TRUE;
}

#endif	// _WIN32

//
// The per root entry points the functions below are written in terms of. On Windows they are the
// registry reads that were written inline here before; elsewhere they are the settings store.
//
static Bool getStringFromCurrentUser(AsciiString path, AsciiString key, AsciiString& val)
{
#ifdef _WIN32
	return getStringFromRegistry(HKEY_CURRENT_USER, path, key, val);
#else
	return getStringFromStore(path, key, val);
#endif
}

static Bool getStringFromLocalMachine(AsciiString path, AsciiString key, AsciiString& val)
{
#ifdef _WIN32
	return getStringFromRegistry(HKEY_LOCAL_MACHINE, path, key, val);
#else
	// See above: no machine wide store exists, and the per user one has already been consulted.
	return FALSE;
#endif
}

static Bool getUnsignedIntFromCurrentUser(AsciiString path, AsciiString key, UnsignedInt& val)
{
#ifdef _WIN32
	return getUnsignedIntFromRegistry(HKEY_CURRENT_USER, path, key, val);
#else
	return getUnsignedIntFromStore(path, key, val);
#endif
}

static Bool getUnsignedIntFromLocalMachine(AsciiString path, AsciiString key, UnsignedInt& val)
{
#ifdef _WIN32
	return getUnsignedIntFromRegistry(HKEY_LOCAL_MACHINE, path, key, val);
#else
	return FALSE;
#endif
}

Bool GetStringFromGeneralsRegistry(AsciiString path, AsciiString key, AsciiString& val)
{
	AsciiString fullPath = "SOFTWARE\\Electronic Arts\\EA Games\\Generals";

	fullPath.concat(path);
	DEBUG_LOG(("GetStringFromRegistry - looking in %s for key %s", fullPath.str(), key.str()));
	if (getStringFromCurrentUser(fullPath.str(), key.str(), val))
	{
		return TRUE;
	}

	return getStringFromLocalMachine(fullPath.str(), key.str(), val);
}

Bool GetStringFromRegistry(AsciiString path, AsciiString key, AsciiString& val)
{
#if RTS_GENERALS
	AsciiString fullPath = "SOFTWARE\\Electronic Arts\\EA Games\\Generals";
#elif RTS_ZEROHOUR
	AsciiString fullPath = "SOFTWARE\\Electronic Arts\\EA Games\\Command and Conquer Generals Zero Hour";
#endif

	fullPath.concat(path);
	DEBUG_LOG(("GetStringFromRegistry - looking in %s for key %s", fullPath.str(), key.str()));
	if (getStringFromLocalMachine(fullPath.str(), key.str(), val))
	{
		return TRUE;
	}

	return getStringFromCurrentUser(fullPath.str(), key.str(), val);
}

Bool GetUnsignedIntFromRegistry(AsciiString path, AsciiString key, UnsignedInt& val)
{
#if RTS_GENERALS
	AsciiString fullPath = "SOFTWARE\\Electronic Arts\\EA Games\\Generals";
#elif RTS_ZEROHOUR
	AsciiString fullPath = "SOFTWARE\\Electronic Arts\\EA Games\\Command and Conquer Generals Zero Hour";
#endif

	fullPath.concat(path);
	DEBUG_LOG(("GetUnsignedIntFromRegistry - looking in %s for key %s", fullPath.str(), key.str()));
	if (getUnsignedIntFromCurrentUser(fullPath.str(), key.str(), val))
	{
		return TRUE;
	}

	return getUnsignedIntFromLocalMachine(fullPath.str(), key.str(), val);
}

AsciiString GetRegistryLanguage()
{
	static Bool cached = FALSE;
	// NOTE: static causes a memory leak, but we have to keep it because the value is cached.
	static AsciiString val = "english";
	if (cached) {
		return val;
	} else {
		cached = TRUE;
	}

	GetStringFromRegistry("", "Language", val);
	return val;
}

AsciiString GetRegistryGameName()
{
	AsciiString val = "GeneralsMPTest";
	GetStringFromRegistry("", "SKU", val);
	return val;
}

UnsignedInt GetRegistryVersion()
{
	UnsignedInt val = 65536;
	GetUnsignedIntFromRegistry("", "Version", val);
	return val;
}

UnsignedInt GetRegistryMapPackVersion()
{
	UnsignedInt val = 65536;
	GetUnsignedIntFromRegistry("", "MapPackVersion", val);
	return val;
}
