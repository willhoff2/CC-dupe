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
 *  Settings storage for the platforms that have no registry. Keys are the same backslash        *
 *  separated paths RegistryClass passes to RegCreateKeyEx(), and each one becomes a section of  *
 *  a single INI file. Values keep the DWORD_ / STRING_ / BIN_ name prefixes that                *
 *  RegistryClass::Save_Registry() already uses, so the type of a value survives a round trip.  *
 *                                                                                             *
 *  The file lives in the per user settings directory:                                          *
 *                                                                                             *
 *      macOS   ~/Library/Application Support/Command and Conquer Generals Zero Hour/           *
 *      others  $XDG_CONFIG_HOME/CommandAndConquerGeneralsZeroHour/ (~/.config when unset)      *
 *                                                                                             *
 *  Set CNC_SETTINGS_FILE to override the whole path.                                           *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#pragma once

#ifndef _WIN32

#include "WWLib/always.h"
#include "WWLib/Vector.h"
#include "WWLib/wwstring.h"

class INIClass;

namespace WWPlatform
{

namespace Settings
{

/*
**	Key handles are non zero on success, mirroring the way RegistryClass treats an HKEY.
*/
bool Key_Exists(const char * sub_key);
int Open_Key(const char * sub_key, bool create);
void Close_Key(int key);

bool Get_Int(int key, const char * name, int & value);
void Set_Int(int key, const char * name, int value);

bool Get_String(int key, const char * name, StringClass & value);
void Set_String(int key, const char * name, const char * value);

int Get_Bin_Size(int key, const char * name);
void Get_Bin(int key, const char * name, void * buffer, int buffer_size);
void Set_Bin(int key, const char * name, const void * buffer, int buffer_size);

void Get_Value_List(int key, DynamicVectorClass<StringClass> & list);
void Delete_Value(int key, const char * name);

/*
**	Copy every key at or below the given path into the INI, in the layout the store uses. This
**	is what RegistryClass::Save_Registry() needs; the tree walk has already happened here.
*/
void Export_Tree(const char * sub_key, INIClass * ini);
void Delete_Tree(const char * sub_key);

/*
**	Path of the backing file, for diagnostics.
*/
const char * Get_Store_Path();

}	// namespace Settings

}	// namespace WWPlatform

#endif // !_WIN32
