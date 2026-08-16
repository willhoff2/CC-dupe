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

#include "WWLib/platform/platform_settings.h"

#ifndef _WIN32

#include "WWLib/INI.h"
#include "WWLib/inisup.h"
#include "WWLib/RAWFILE.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

namespace WWPlatform
{

namespace Settings
{

/*
**	Value name prefixes. RegistryClass::Save_Registry() already writes registry values into an
**	INI this way, so the store just uses the same layout.
*/
static const char * const INT_PREFIX = "DWORD_";
static const char * const STRING_PREFIX = "STRING_";
static const char * const BIN_PREFIX = "BIN_";

/*
**	Largest binary value the store round trips. RegistryClass reads and writes registry blobs
**	through 8K buffers, so nothing bigger can reach here anyway.
*/
enum { MAX_BIN_SIZE = 8192 };

/*
**	The store's state lives in function local statics rather than file scope ones. The debug
**	configuration reaches this store from DebugInit(), which the memory manager runs before main and
**	therefore before this translation unit's own static constructors have run; a file scope
**	StringClass is still all zeroes at that point and dereferences its null buffer. Function local
**	statics are constructed on first use instead, so the order cannot bite. The Win32 spelling is
**	ordering safe for the same reason: RegOpenKeyEx keeps no C++ state.
*/
static StringClass & Store_Path()
{
	static StringClass _store_path;
	return _store_path;
}

static StringClass & Store_Directory()
{
	static StringClass _store_directory;
	return _store_directory;
}

static INIClass * _Store = nullptr;

/*
**	One entry per key handle, the handle being the index plus one. A closed handle leaves an
**	empty entry behind for the next open to claim, so live handles never move.
*/
static DynamicVectorClass<StringClass> & Open_Keys()
{
	static DynamicVectorClass<StringClass> _open_keys;
	return _open_keys;
}


/***********************************************************************************************
 * Build_Store_Path -- Work out where the settings file lives                                  *
 *=============================================================================================*/
static void Build_Store_Path()
{
	const char * override_path = getenv("CNC_SETTINGS_FILE");
	if (override_path != nullptr && *override_path != 0) {
		Store_Path() = override_path;
		Store_Directory() = "";
		const char * slash = strrchr(override_path, '/');
		if (slash != nullptr) {
			Store_Directory() = Store_Path();
			Store_Directory().Peek_Buffer()[slash - override_path] = 0;
		}
		return;
	}

	const char * home = getenv("HOME");
	if (home == nullptr) {
		home = ".";
	}

#ifdef __APPLE__
	Store_Directory().Format("%s/Library/Application Support/Command and Conquer Generals Zero Hour", home);
#else
	const char * config = getenv("XDG_CONFIG_HOME");
	if (config != nullptr && *config != 0) {
		Store_Directory().Format("%s/CommandAndConquerGeneralsZeroHour", config);
	} else {
		Store_Directory().Format("%s/.config/CommandAndConquerGeneralsZeroHour", home);
	}
#endif

	Store_Path().Format("%s/Registry.ini", Store_Directory().Peek_Buffer());
}


/***********************************************************************************************
 * Get_Store -- Fetch the loaded settings, reading the file on the first call                   *
 *=============================================================================================*/
static INIClass * Get_Store()
{
	if (_Store == nullptr) {
		Build_Store_Path();

		_Store = W3DNEW INIClass;

		RawFileClass file(Store_Path().Peek_Buffer());
		if (file.Is_Available()) {
			_Store->Load(file);
		}
	}

	return _Store;
}


/***********************************************************************************************
 * Flush_Store -- Write the settings back out                                                  *
 *=============================================================================================*/
static void Flush_Store()
{
	INIClass * store = Get_Store();

	/*
	**	Create the settings directory a component at a time; there is no portable mkdir -p.
	*/
	if (Store_Directory().Get_Length() > 0) {
		StringClass partial = Store_Directory();
		char * cursor = partial.Peek_Buffer();
		for (char * scan = cursor + 1; *scan != 0; ++scan) {
			if (*scan == '/') {
				*scan = 0;
				mkdir(cursor, 0755);
				*scan = '/';
			}
		}
		mkdir(cursor, 0755);
	}

	RawFileClass file(Store_Path().Peek_Buffer());
	store->Save(file);
}


/***********************************************************************************************
 * Section_Of -- The section name a key handle refers to                                       *
 *=============================================================================================*/
static const char * Section_Of(int key)
{
	if (key <= 0 || key > Open_Keys().Count()) {
		return nullptr;
	}
	if (Open_Keys()[key - 1].Get_Length() == 0) {
		return nullptr;
	}
	return Open_Keys()[key - 1].Peek_Buffer();
}


static void Prefixed_Name(StringClass & full_name, const char * prefix, const char * name)
{
	full_name = prefix;
	full_name += name;
}


bool Key_Exists(const char * sub_key)
{
	return Get_Store()->Section_Present(sub_key);
}


int Open_Key(const char * sub_key, bool create)
{
	if (!create && !Get_Store()->Section_Present(sub_key)) {
		return 0;
	}

	/*
	**	The store has no empty sections, so a key that is created but never written to is simply
	**	absent from the file until the first value lands in it.
	*/
	for (int index = 0; index < Open_Keys().Count(); index++) {
		if (Open_Keys()[index].Get_Length() == 0) {
			Open_Keys()[index] = sub_key;
			return index + 1;
		}
	}

	Open_Keys().Add(sub_key);
	return Open_Keys().Count();
}


void Close_Key(int key)
{
	if (key > 0 && key <= Open_Keys().Count()) {
		Open_Keys()[key - 1] = "";
	}
}


bool Get_Int(int key, const char * name, int & value)
{
	const char * section = Section_Of(key);
	if (section == nullptr) return false;

	StringClass full_name;
	Prefixed_Name(full_name, INT_PREFIX, name);
	if (!Get_Store()->Is_Present(section, full_name.Peek_Buffer())) {
		return false;
	}

	value = Get_Store()->Get_Int(section, full_name.Peek_Buffer());
	return true;
}


void Set_Int(int key, const char * name, int value)
{
	const char * section = Section_Of(key);
	if (section == nullptr) return;

	StringClass full_name;
	Prefixed_Name(full_name, INT_PREFIX, name);
	Get_Store()->Put_Int(section, full_name.Peek_Buffer(), value);
	Flush_Store();
}


bool Get_String(int key, const char * name, StringClass & value)
{
	const char * section = Section_Of(key);
	if (section == nullptr) return false;

	StringClass full_name;
	Prefixed_Name(full_name, STRING_PREFIX, name);
	if (!Get_Store()->Is_Present(section, full_name.Peek_Buffer())) {
		return false;
	}

	Get_Store()->Get_String(value, section, full_name.Peek_Buffer());
	return true;
}


void Set_String(int key, const char * name, const char * value)
{
	const char * section = Section_Of(key);
	if (section == nullptr) return;

	StringClass full_name;
	Prefixed_Name(full_name, STRING_PREFIX, name);
	Get_Store()->Put_String(section, full_name.Peek_Buffer(), value);
	Flush_Store();
}


int Get_Bin_Size(int key, const char * name)
{
	const char * section = Section_Of(key);
	if (section == nullptr) return 0;

	StringClass full_name;
	Prefixed_Name(full_name, BIN_PREFIX, name);

	char scratch[MAX_BIN_SIZE];
	return Get_Store()->Get_UUBlock(section, full_name.Peek_Buffer(), scratch, sizeof(scratch));
}


void Get_Bin(int key, const char * name, void * buffer, int buffer_size)
{
	const char * section = Section_Of(key);
	if (section == nullptr) return;

	StringClass full_name;
	Prefixed_Name(full_name, BIN_PREFIX, name);
	Get_Store()->Get_UUBlock(section, full_name.Peek_Buffer(), buffer, buffer_size);
}


void Set_Bin(int key, const char * name, const void * buffer, int buffer_size)
{
	const char * section = Section_Of(key);
	if (section == nullptr) return;

	StringClass full_name;
	Prefixed_Name(full_name, BIN_PREFIX, name);
	Get_Store()->Put_UUBlock(section, full_name.Peek_Buffer(), buffer, buffer_size);
	Flush_Store();
}


void Get_Value_List(int key, DynamicVectorClass<StringClass> & list)
{
	const char * section = Section_Of(key);
	if (section == nullptr) return;

	int count = Get_Store()->Entry_Count(section);
	for (int index = 0; index < count; index++) {
		const char * entry = Get_Store()->Get_Entry(section, index);
		if (entry == nullptr) continue;

		/*
		**	Hand back the name the caller stored, without the type prefix.
		*/
		if (strncmp(entry, INT_PREFIX, strlen(INT_PREFIX)) == 0) {
			list.Add(entry + strlen(INT_PREFIX));
		} else if (strncmp(entry, STRING_PREFIX, strlen(STRING_PREFIX)) == 0) {
			list.Add(entry + strlen(STRING_PREFIX));
		} else if (strncmp(entry, BIN_PREFIX, strlen(BIN_PREFIX)) == 0) {
			list.Add(entry + strlen(BIN_PREFIX));
		}
	}
}


void Delete_Value(int key, const char * name)
{
	const char * section = Section_Of(key);
	if (section == nullptr) return;

	StringClass full_name;
	Prefixed_Name(full_name, INT_PREFIX, name);
	Get_Store()->Clear(section, full_name.Peek_Buffer());
	Prefixed_Name(full_name, STRING_PREFIX, name);
	Get_Store()->Clear(section, full_name.Peek_Buffer());
	Prefixed_Name(full_name, BIN_PREFIX, name);
	Get_Store()->Clear(section, full_name.Peek_Buffer());
	Flush_Store();
}


/***********************************************************************************************
 * Is_At_Or_Below -- Is the section the given key, or one of its sub keys?                      *
 *=============================================================================================*/
static bool Is_At_Or_Below(const char * section, const char * sub_key)
{
	size_t length = strlen(sub_key);
	if (strncmp(section, sub_key, length) != 0) {
		return false;
	}
	return section[length] == 0 || section[length] == '\\';
}


void Export_Tree(const char * sub_key, INIClass * ini)
{
	List<INISection *> & section_list = Get_Store()->Get_Section_List();

	for (INISection * section = section_list.First(); section != nullptr; section = section->Next_Valid()) {
		if (!Is_At_Or_Below(section->Section, sub_key)) continue;

		for (INIEntry * entry = section->EntryList.First(); entry != nullptr; entry = entry->Next_Valid()) {
			ini->Put_String(section->Section, entry->Entry, entry->Value);
		}
	}
}


void Delete_Tree(const char * sub_key)
{
	/*
	**	Collect first, since clearing a section unlinks it from the list being walked.
	*/
	DynamicVectorClass<StringClass> doomed;
	List<INISection *> & section_list = Get_Store()->Get_Section_List();
	for (INISection * section = section_list.First(); section != nullptr; section = section->Next_Valid()) {
		if (Is_At_Or_Below(section->Section, sub_key)) {
			doomed.Add(section->Section);
		}
	}

	for (int index = 0; index < doomed.Count(); index++) {
		Get_Store()->Clear(doomed[index].Peek_Buffer());
	}

	Flush_Store();
}


const char * Get_Store_Path()
{
	Get_Store();
	return Store_Path().Peek_Buffer();
}

}	// namespace Settings

}	// namespace WWPlatform

#endif // !_WIN32
