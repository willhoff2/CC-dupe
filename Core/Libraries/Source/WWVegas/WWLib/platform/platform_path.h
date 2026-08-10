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

/***********************************************************************************************
 *                                                                                             *
 *                 Project Name : Westwood Library                                             *
 *                                                                                             *
 *  Directory and path operations, in the shape of the Win32 calls the engine already makes.    *
 *  Unlike the other platform_* modules this one is compiled on every platform: on Windows      *
 *  each entry point is a direct forward to the same Win32 call the call site used before, so   *
 *  the Windows build keeps its exact behaviour and the call sites stop being conditional.      *
 *                                                                                             *
 *  Paths may be spelled the Windows way. Off Windows every entry point converts '\\' to '/'    *
 *  and, when the literal spelling does not exist, resolves each component case insensitively   *
 *  (see Resolve()). That is the single choke point for the mixed case, backslash separated     *
 *  path literals in the retail data; nothing in the data has to change.                        *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#pragma once

#include "WWLib/always.h"
#include "WWLib/Vector.h"
#include "WWLib/wwstring.h"

namespace WWPlatform
{

namespace Path
{

/*
**	The separator the host filesystem uses. Path literals in the engine and in the retail data
**	are written with backslashes; this is what they are translated to.
*/
#ifdef _WIN32
const char SEPARATOR = '\\';
#else
const char SEPARATOR = '/';
#endif

/*
**	One item from a directory listing. Only the two fields the engine looks at are carried;
**	sizes and timestamps stay with FileSystem, which already has its own FileInfo for them.
*/
class EntryClass
{
public:
	EntryClass() : Is_Directory(false) {}

	bool operator== (const EntryClass & other) const { return Name.Compare(other.Name) == 0; }
	bool operator!= (const EntryClass & other) const { return !(*this == other); }

	StringClass	Name;
	bool		Is_Directory;
};

/*
**	Existence, creation and removal. Equivalent of _access(path, 0), CreateDirectory(),
**	DeleteFile() and CopyFile().
*/
bool Exists(const char * path);
bool Create_Directory(const char * path);
bool Delete_File(const char * path);
bool Copy_File(const char * source, const char * destination, bool fail_if_exists);

/*
**	Process wide current directory, and the running executable. Equivalent of
**	GetCurrentDirectory(), SetCurrentDirectory() and GetModuleFileName(nullptr, ...).
*/
bool Get_Current_Directory(char * buffer, unsigned size);
bool Set_Current_Directory(const char * path);
bool Get_Executable_Path(char * buffer, unsigned size);

/*
**	Directory listing. The pattern is a Win32 wildcard ("*", "*.ini"); an empty or null pattern
**	means everything. Equivalent of the FindFirstFile()/FindNextFile()/FindClose() loop, with
**	the handle and the WIN32_FIND_DATA kept on this side of the seam.
*/
bool Enumerate(const char * directory, const char * pattern, DynamicVectorClass<EntryClass> & entries);

/*
**	True if at least one name in the directory matches the pattern. This is the "is there
**	anything to load here" test, which used to be a FindFirstFile() whose handle was leaked.
*/
bool Has_Match(const char * directory, const char * pattern);

/*
**	Per user locations.
**
**	Get_User_Data_Root() is where the game's own writable folder is created; the game appends
**	its own leaf name to it. On Windows that is the Documents folder, exactly as before. Off
**	Windows there is no Documents folder to speak of, so:
**
**	    macOS   ~/Library/Application Support/
**	    others  $XDG_DATA_HOME/ (~/.local/share when unset)
**
**	Set CNC_USER_DATA to override the whole thing. This is a decision, not a translation of a
**	Win32 behaviour -- see docs/porting/filesystem-and-registry.md.
*/
bool Get_User_Data_Root(StringClass & path);
bool Get_Desktop_Directory(StringClass & path);

/*
**	Text of the last failure, for the message boxes that used FormatMessage(GetLastError()).
*/
void Get_Last_Error_Text(char * buffer, unsigned size);

#ifndef _WIN32
/*
**	Turn a Windows shaped path into one this filesystem will accept: separators converted, and
**	if the result does not exist, each component matched case insensitively against what is
**	really on disk. Returns false when no such match exists, in which case 'resolved' still
**	holds the separator converted spelling so it can be used to create the file.
**
**	When 'parent_must_exist_only' is true, only the parent directory has to resolve -- that is
**	the "opening for write, the file is allowed not to exist yet" case.
*/
bool Resolve(const char * path, StringClass & resolved, bool parent_must_exist_only = false);
#endif // !_WIN32

}	// namespace Path

}	// namespace WWPlatform
