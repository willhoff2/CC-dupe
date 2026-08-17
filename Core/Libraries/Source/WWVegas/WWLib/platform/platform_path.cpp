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

#include "WWLib/platform/platform_path.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32

#include <windows.h>
#include <shlobj.h>
#include <io.h>

#else	// !_WIN32

#include <errno.h>
#include <fnmatch.h>
#include <stdlib.h>
#include <strings.h>
#include <unistd.h>
#include <algorithm>
#include <filesystem>
#include <string>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#endif	// _WIN32

namespace WWPlatform
{

namespace Path
{

#ifdef _WIN32

/***********************************************************************************************
 *                                                                                             *
 *  Windows. Every entry point is the call the call site used to make, so that moving a call    *
 *  site behind this header cannot change what the Windows build does.                          *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

bool Exists(const char * path)
{
	if (path == nullptr || *path == 0) {
		return false;
	}
	return ::_access(path, 0) == 0;
}


bool Create_Directory(const char * path)
{
	if (path == nullptr || *path == 0) {
		return false;
	}
	return ::CreateDirectory(path, nullptr) != 0;
}


FILE * Open_Stream(const char * path, const char * mode)
{
	if (path == nullptr || *path == 0 || mode == nullptr) {
		return nullptr;
	}
	return ::fopen(path, mode);
}


bool Delete_File(const char * path)
{
	if (path == nullptr || *path == 0) {
		return false;
	}
	return ::DeleteFile(path) != 0;
}


bool Copy_File(const char * source, const char * destination, bool fail_if_exists)
{
	if (source == nullptr || destination == nullptr) {
		return false;
	}
	return ::CopyFile(source, destination, fail_if_exists ? TRUE : FALSE) != 0;
}


bool Get_Current_Directory(char * buffer, unsigned size)
{
	if (buffer == nullptr || size == 0) {
		return false;
	}
	return ::GetCurrentDirectory(size, buffer) != 0;
}


bool Set_Current_Directory(const char * path)
{
	if (path == nullptr || *path == 0) {
		return false;
	}
	return ::SetCurrentDirectory(path) != 0;
}


bool Get_Executable_Path(char * buffer, unsigned size)
{
	if (buffer == nullptr || size == 0) {
		return false;
	}
	return ::GetModuleFileName(nullptr, buffer, size) != 0;
}


static void Join(StringClass & joined, const char * directory, const char * pattern)
{
	joined = (directory != nullptr) ? directory : "";
	if (joined.Get_Length() > 0) {
		char last = joined[joined.Get_Length() - 1];
		if (last != '\\' && last != '/') {
			joined += SEPARATOR;
		}
	}
	joined += (pattern != nullptr && *pattern != 0) ? pattern : "*";
}


bool Enumerate(const char * directory, const char * pattern, DynamicVectorClass<EntryClass> & entries)
{
	StringClass search;
	Join(search, directory, pattern);

	WIN32_FIND_DATA item;
	HANDLE handle = ::FindFirstFile(search.Peek_Buffer(), &item);
	if (handle == INVALID_HANDLE_VALUE) {
		return false;
	}

	do {
		EntryClass entry;
		entry.Name = item.cFileName;
		entry.Is_Directory = (item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
		entries.Add(entry);
	} while (::FindNextFile(handle, &item) != 0);

	::FindClose(handle);
	return true;
}


bool Has_Match(const char * directory, const char * pattern)
{
	StringClass search;
	Join(search, directory, pattern);

	WIN32_FIND_DATA item;
	HANDLE handle = ::FindFirstFile(search.Peek_Buffer(), &item);
	if (handle == INVALID_HANDLE_VALUE) {
		return false;
	}

	::FindClose(handle);
	return true;
}


bool Get_User_Data_Root(StringClass & path)
{
	/*
	**	Moved here verbatim from GlobalData::BuildUserDataPathFromRegistry(), including the
	**	runtime lookup of SHGetKnownFolderPath: it exists only from Vista onwards, and it is the
	**	one that honours OneDrive and Group Policy folder redirection.
	*/
#if defined(_MSC_VER) && (_MSC_VER < 1300)
	// VC6 lacks FOLDERID_Documents and KF_FLAG_DEFAULT
	const GUID FOLDERID_Documents = { 0xFDD39AD0, 0x238F, 0x46AF, 0xAD, 0xB4, 0x6C, 0x85, 0x48, 0x03, 0x69, 0xC7 };
	const DWORD KF_FLAG_DEFAULT = 0;
#endif

	typedef HRESULT(WINAPI * PFN_SHGetKnownFolderPath)(const GUID & rfid, DWORD dwFlags, HANDLE hToken, PWSTR * ppszPath);

	path = "";

	HMODULE shell32module = ::GetModuleHandleA("shell32.dll");
	PFN_SHGetKnownFolderPath pSHGetKnownFolderPath = nullptr;

	if (shell32module) {
		pSHGetKnownFolderPath = (PFN_SHGetKnownFolderPath)::GetProcAddress(shell32module, "SHGetKnownFolderPath");
	}

	if (pSHGetKnownFolderPath) {
		PWSTR pszPath = nullptr;
		HRESULT hr = pSHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &pszPath);

		if (SUCCEEDED(hr) && pszPath) {
			/*
			**	The caller used to be AsciiString::translate(), which truncates each wide
			**	character to a byte rather than converting it. That is wrong for a Documents
			**	path containing non ASCII characters, but it is what the shipped build does and
			**	fixing it is not this slice's business, so the truncation is reproduced here.
			*/
			int length = (int)::wcslen(pszPath);
			char * narrow = new char[length + 1];
			for (int index = 0; index < length; ++index) {
				narrow[index] = (char)pszPath[index];
			}
			narrow[length] = 0;
			path = narrow;
			delete [] narrow;
			::CoTaskMemFree(pszPath);
		}
	}
	else {
		char temp[MAX_PATH + 1];
		if (::SHGetSpecialFolderPath(nullptr, temp, CSIDL_PERSONAL, true)) {
			path = temp;
		}
	}

	return path.Get_Length() > 0;
}


bool Get_Desktop_Directory(StringClass & path)
{
	path = "";

	LPITEMIDLIST pidl = nullptr;
	if (::SHGetSpecialFolderLocation(nullptr, CSIDL_DESKTOPDIRECTORY, &pidl) != NOERROR) {
		return false;
	}

	char buffer[MAX_PATH + 1];
	buffer[0] = 0;
	BOOL got_it = ::SHGetPathFromIDList(pidl, buffer);
	if (got_it) {
		path = buffer;
	}

	return got_it != FALSE;
}


void Get_Last_Error_Text(char * buffer, unsigned size)
{
	if (buffer == nullptr || size == 0) {
		return;
	}

	buffer[0] = 0;
	::FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, nullptr, ::GetLastError(), 0, buffer, size, nullptr);
}

#else	// !_WIN32

/***********************************************************************************************
 *                                                                                             *
 *  Everything else. The engine's path literals are Windows shaped, so each entry point         *
 *  converts separators and then resolves the spelling against what is on disk.                 *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static std::string Slashed(const char * path)
{
	std::string result((path != nullptr) ? path : "");
	std::replace(result.begin(), result.end(), '\\', '/');
	return result;
}


bool Resolve(const char * path, StringClass & resolved, bool parent_must_exist_only)
{
	std::string slashed = Slashed(path);
	resolved = slashed.c_str();

	if (slashed.empty()) {
		return false;
	}

	std::error_code ec;
	std::filesystem::path wanted(slashed);

	if (std::filesystem::exists(wanted, ec)) {
		return true;
	}
	if (parent_must_exist_only && std::filesystem::exists(wanted.parent_path(), ec)) {
		return true;
	}

	/*
	**	Walk the path a component at a time, matching each one case insensitively against the
	**	directory that the previous components resolved to. The retail data and the INI files
	**	that reference it disagree about case in both directions, so this has to work for a
	**	lower case literal naming an upper case file and the other way round.
	*/
	std::filesystem::path fixed;
	bool first = true;
	bool complete = true;

	for (std::filesystem::path::iterator it = wanted.begin(); it != wanted.end(); ++it) {
		const std::filesystem::path & part = *it;

		if (first) {
			fixed /= part;
			first = false;
			continue;
		}

		if (!complete) {
			fixed /= part;
			continue;
		}

		if (std::filesystem::exists(fixed / part, ec)) {
			fixed /= part;
			continue;
		}

		std::filesystem::path match;
		std::filesystem::directory_iterator scan(fixed, ec);
		if (!ec) {
			for (const std::filesystem::directory_entry & entry : scan) {
				if (::strcasecmp(entry.path().filename().string().c_str(), part.string().c_str()) == 0) {
					match = entry.path().filename();
					break;
				}
			}
		}

		if (match.empty()) {
			/*
			**	Nothing on disk answers to this component. Keep the literal spelling for the
			**	rest of the path so the caller can still create it.
			*/
			complete = false;
			fixed /= part;
		} else {
			fixed /= match;
		}
	}

	resolved = fixed.string().c_str();

	if (complete) {
		return true;
	}
	if (parent_must_exist_only) {
		return std::filesystem::exists(std::filesystem::path(fixed).parent_path(), ec);
	}
	return false;
}


bool Exists(const char * path)
{
	StringClass resolved;
	return Resolve(path, resolved, false);
}


bool Create_Directory(const char * path)
{
	StringClass resolved;
	Resolve(path, resolved, false);

	if (resolved.Get_Length() == 0) {
		return false;
	}

	std::error_code ec;
	/*
	**	CreateDirectory() only makes the leaf, but every caller in the engine is creating a
	**	folder whose parents it has already made, and create_directories() is the only portable
	**	way to get "make it if the parents happen to be missing too" without a hand rolled walk.
	*/
	return std::filesystem::create_directories(std::filesystem::path(resolved.Peek_Buffer()), ec) && !ec;
}


FILE * Open_Stream(const char * path, const char * mode)
{
	if (path == nullptr || *path == 0 || mode == nullptr) {
		return nullptr;
	}

	/*
	**	A writing mode is allowed to name a file that does not exist yet, so only the directory
	**	holding it has to resolve. A reading mode needs the file itself.
	*/
	const bool writing = (::strpbrk(mode, "wa+") != nullptr);

	StringClass resolved;
	if (!Resolve(path, resolved, writing) && !writing) {
		return nullptr;
	}

	return ::fopen(resolved.Peek_Buffer(), mode);
}


bool Delete_File(const char * path)
{
	StringClass resolved;
	if (!Resolve(path, resolved, false)) {
		return false;
	}

	std::error_code ec;
	return std::filesystem::remove(std::filesystem::path(resolved.Peek_Buffer()), ec) && !ec;
}


bool Copy_File(const char * source, const char * destination, bool fail_if_exists)
{
	StringClass from;
	if (!Resolve(source, from, false)) {
		return false;
	}

	StringClass to;
	Resolve(destination, to, true);
	if (to.Get_Length() == 0) {
		return false;
	}

	std::error_code ec;
	std::filesystem::copy_options options = fail_if_exists
		? std::filesystem::copy_options::none
		: std::filesystem::copy_options::overwrite_existing;

	return std::filesystem::copy_file(std::filesystem::path(from.Peek_Buffer()),
		std::filesystem::path(to.Peek_Buffer()), options, ec) && !ec;
}


bool Get_Current_Directory(char * buffer, unsigned size)
{
	if (buffer == nullptr || size == 0) {
		return false;
	}
	return ::getcwd(buffer, size) != nullptr;
}


bool Set_Current_Directory(const char * path)
{
	StringClass resolved;
	if (!Resolve(path, resolved, false)) {
		return false;
	}
	return ::chdir(resolved.Peek_Buffer()) == 0;
}


bool Get_Executable_Path(char * buffer, unsigned size)
{
	if (buffer == nullptr || size == 0) {
		return false;
	}

	buffer[0] = 0;

#ifdef __APPLE__
	uint32_t needed = size;
	if (::_NSGetExecutablePath(buffer, &needed) != 0) {
		return false;
	}
	return buffer[0] != 0;
#else
	ssize_t written = ::readlink("/proc/self/exe", buffer, size - 1);
	if (written <= 0) {
		return false;
	}
	buffer[written] = 0;
	return true;
#endif
}


bool Enumerate(const char * directory, const char * pattern, DynamicVectorClass<EntryClass> & entries)
{
	StringClass resolved;
	std::string slashed = Slashed(directory);
	if (slashed.empty()) {
		slashed = ".";
	}
	Resolve(slashed.c_str(), resolved, false);

	const char * match = (pattern != nullptr && *pattern != 0) ? pattern : "*";

	std::error_code ec;
	std::filesystem::directory_iterator scan(std::filesystem::path(resolved.Peek_Buffer()), ec);
	if (ec) {
		return false;
	}

	bool found_any = false;
	for (const std::filesystem::directory_entry & entry : scan) {
		std::string name = entry.path().filename().string();

		/*
		**	FNM_CASEFOLD because the pattern comes from a Windows path literal and the names on
		**	disk come from a retail install whose case nobody controls.
		*/
		if (::fnmatch(match, name.c_str(), FNM_CASEFOLD) != 0) {
			continue;
		}

		EntryClass item;
		item.Name = name.c_str();
		item.Is_Directory = entry.is_directory(ec);
		entries.Add(item);
		found_any = true;
	}

	return found_any;
}


bool Has_Match(const char * directory, const char * pattern)
{
	DynamicVectorClass<EntryClass> entries;
	return Enumerate(directory, pattern, entries);
}


bool Get_User_Data_Root(StringClass & path)
{
	path = "";

	const char * override_path = ::getenv("CNC_USER_DATA");
	if (override_path != nullptr && *override_path != 0) {
		path = override_path;
		return true;
	}

	const char * home = ::getenv("HOME");
	if (home == nullptr || *home == 0) {
		return false;
	}

#ifdef __APPLE__
	path.Format("%s/Library/Application Support", home);
#else
	const char * data_home = ::getenv("XDG_DATA_HOME");
	if (data_home != nullptr && *data_home != 0) {
		path = data_home;
	} else {
		path.Format("%s/.local/share", home);
	}
#endif

	return true;
}


bool Get_Desktop_Directory(StringClass & path)
{
	path = "";

	const char * desktop = ::getenv("XDG_DESKTOP_DIR");
	if (desktop != nullptr && *desktop != 0) {
		path = desktop;
		return true;
	}

	const char * home = ::getenv("HOME");
	if (home == nullptr || *home == 0) {
		return false;
	}

	/*
	**	The real answer is in $XDG_CONFIG_HOME/user-dirs.dirs, which is shell syntax and needs a
	**	parser. ~/Desktop is what that file says on a default install of every desktop this is
	**	likely to run on, and the only cost of being wrong is that a copied replay lands in the
	**	wrong folder.
	*/
	path.Format("%s/Desktop", home);
	return true;
}


void Get_Last_Error_Text(char * buffer, unsigned size)
{
	if (buffer == nullptr || size == 0) {
		return;
	}

	const char * text = ::strerror(errno);
	::snprintf(buffer, size, "%s", (text != nullptr) ? text : "");
}

#endif	// _WIN32

}	// namespace Path

}	// namespace WWPlatform
