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
 *  The file half of the Win32 compatibility layer: the FindFirstFile()/FindNextFile() loop the  *
 *  asset scan is built on, the file and directory operations next to it, and the                *
 *  GetLastError() the failure paths report through.                                             *
 *                                                                                             *
 *  Paths arrive spelled the Windows way and go through WWPlatform::Path::Resolve(), which       *
 *  converts separators and matches each component case insensitively, so the mixed case         *
 *  literals in the retail data keep working on a case sensitive filesystem.                     *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#include "WWLib/platform/platform_win32_compat.h"

#ifdef WWPLATFORM_WIN32_COMPAT

#include "WWLib/platform/platform_path.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>

/*
**	ERROR_INVALID_HANDLE is not one of the codes the engine compares against, so the port's
**	<windows.h> does not define it. FindClose() still wants to report it.
*/
static const DWORD WWPLATFORM_ERROR_INVALID_HANDLE = 6L;

namespace WWPlatform
{

namespace Win32
{

/***********************************************************************************************
 *                                                                                             *
 *  Last error. Win32 code, per thread, set by every entry point in this layer the way the      *
 *  real API does: cleared on the success paths that document it, set on failure.                *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

/*
**	Base for errno values with no Win32 counterpart. Win32 reserves 1..0x1FFF for the system, so
**	an unmapped errno lands somewhere no real error code lives and survives the round trip back
**	to strerror() in FormatMessage.
*/
enum { ERRNO_ERROR_BASE = 0x20000000 };

static thread_local DWORD _LastError = ERROR_SUCCESS;

DWORD Error_From_Errno(int error_number)
{
	switch (error_number) {
		case 0:			return ERROR_SUCCESS;
		case ENOENT:	return ERROR_FILE_NOT_FOUND;
		case ENOTDIR:	return ERROR_PATH_NOT_FOUND;
		case EACCES:	return ERROR_ACCESS_DENIED;
		case EPERM:		return ERROR_ACCESS_DENIED;
		case EROFS:		return ERROR_ACCESS_DENIED;
		case EEXIST:	return ERROR_ALREADY_EXISTS;
		case EBADF:		return WWPLATFORM_ERROR_INVALID_HANDLE;
		default:		break;
	}
	return (DWORD)(ERRNO_ERROR_BASE + error_number);
}


int Errno_From_Error(DWORD code)
{
	if (code >= (DWORD)ERRNO_ERROR_BASE) {
		return (int)(code - (DWORD)ERRNO_ERROR_BASE);
	}

	switch (code) {
		case ERROR_SUCCESS:			return 0;
		case ERROR_FILE_NOT_FOUND:	return ENOENT;
		case ERROR_PATH_NOT_FOUND:	return ENOTDIR;
		case ERROR_ACCESS_DENIED:	return EACCES;
		case ERROR_ALREADY_EXISTS:	return EEXIST;
		case ERROR_NO_MORE_FILES:	return ENOENT;
		default:					break;
	}
	if (code == WWPLATFORM_ERROR_INVALID_HANDLE) {
		return EBADF;
	}
	return EINVAL;
}


void Set_Last_Error(DWORD code)
{
	_LastError = code;
}


void Set_Last_Error_From_Errno(int error_number)
{
	_LastError = Error_From_Errno(error_number);
}


void Time_To_File_Time(time_t seconds, long nanoseconds, FILETIME & result)
{
	/*
	**	100ns ticks between 1601-01-01 and 1970-01-01.
	*/
	const unsigned long long EPOCH_DELTA = 116444736000000000ULL;

	unsigned long long ticks = EPOCH_DELTA
		+ (unsigned long long)seconds * 10000000ULL
		+ (unsigned long long)(nanoseconds / 100);

	result.dwLowDateTime = (DWORD)(ticks & 0xFFFFFFFFULL);
	result.dwHighDateTime = (DWORD)(ticks >> 32);
}


void Report_Stub(const char * api, const char * detail)
{
	/*
	**	Deliberately not deduplicated per argument: the point is one line the first time each
	**	entry point is reached, so that a run that depends on one of them says so.
	*/
	fprintf(stderr, "!!! %s() is not implemented off Windows: %s\n", api, detail);
}


/***********************************************************************************************
 *                                                                                             *
 *  Wildcard matching. FindFirstFile() matches the DOS pattern language, not fnmatch()'s: the   *
 *  engine's own patterns are "*", "*.ini", "*.big", "*.*" and "*.", and the last two are the    *
 *  ones a POSIX matcher gets wrong.                                                            *
 *                                                                                             *
 *  Matching is case insensitive on every platform, because it is on Windows and because the     *
 *  data says Maps\\Alpine Assault\\map.ini while the file on disk may say anything. This is the *
 *  same choice Path::Resolve() makes for the directory part of the path.                        *
 *                                                                                             *
 *  Not implemented, because nothing in the engine uses them: the DOS_QM behaviour where a       *
 *  trailing '?' also matches zero characters, and the '<' '>' '"' wildcards that only the       *
 *  native NT API can produce. '?' here matches exactly one character.                          *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static bool Glob(const char * pattern, const char * name)
{
	const char * star = nullptr;
	const char * retry = name;

	while (*name != 0) {
		if (*pattern == '?' ||
				(*pattern != 0 && tolower((unsigned char)*pattern) == tolower((unsigned char)*name))) {
			++pattern;
			++name;
			continue;
		}
		if (*pattern == '*') {
			star = pattern++;
			retry = name;
			continue;
		}
		if (star != nullptr) {
			pattern = star + 1;
			name = ++retry;
			continue;
		}
		return false;
	}

	while (*pattern == '*') {
		++pattern;
	}
	return *pattern == 0;
}


bool Name_Matches(const char * pattern, const char * name)
{
	if (pattern == nullptr || *pattern == 0) {
		return true;
	}

	/*
	**	"*.*" is the DOS spelling of "everything", including names with no extension at all;
	**	ntdll rewrites it to "*" before matching. A literal reading matches only dotted names,
	**	which is how a POSIX matcher silently drops half of a directory listing.
	*/
	if (strcmp(pattern, "*.*") == 0) {
		return true;
	}

	size_t length = strlen(pattern);
	if (pattern[length - 1] == '.') {
		/*
		**	A trailing '.' is DOS_DOT: the extension has to be empty, i.e. the name must not
		**	contain a '.' at all. This is the pattern Win32LocalFileSystem uses to list
		**	subdirectories ("*."), so getting it wrong stops the recursive asset scan.
		*/
		bool dot_entry = (strcmp(name, ".") == 0 || strcmp(name, "..") == 0);
		if (!dot_entry && strchr(name, '.') != nullptr) {
			return false;
		}
		std::string trimmed(pattern, length - 1);
		return Glob(trimmed.c_str(), name);
	}

	return Glob(pattern, name);
}

}	// namespace Win32

}	// namespace WWPlatform


/***********************************************************************************************
 *                                                                                             *
 *  The find handle. Win32 hands back an opaque HANDLE and keeps the enumeration state inside    *
 *  the kernel; here it is this structure, read in one pass at FindFirstFile() time. The engine  *
 *  creates and deletes files while it is enumerating (the save game list), and a snapshot is    *
 *  the behaviour it already relies on.                                                          *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

namespace
{

class FindContextClass
{
public:
	FindContextClass() : Next(0) {}

	std::string					Directory;	// resolved directory the names live in
	std::vector<std::string>	Names;		// matches, "." and ".." first
	unsigned					Next;
};


/*
**	Split "Data\\INI\\*.ini" into the directory to open and the pattern to match. An empty
**	directory part means the current one; a leading separator has to survive as "/".
*/
void Split_Path(const char * path, std::string & directory, std::string & pattern)
{
	const char * last_forward = strrchr(path, '/');
	const char * last_back = strrchr(path, '\\');
	const char * separator = (last_forward > last_back) ? last_forward : last_back;

	if (separator == nullptr) {
		directory = ".";
		pattern = path;
		return;
	}

	directory.assign(path, separator - path);
	if (directory.empty()) {
		directory = "/";
	}
	pattern = separator + 1;
}


void Fill_Find_Data(const std::string & directory, const std::string & name, LPWIN32_FIND_DATAA data)
{
	memset(data, 0, sizeof(*data));
	strncpy(data->cFileName, name.c_str(), sizeof(data->cFileName) - 1);

	std::string full = directory;
	if (!full.empty() && full[full.size() - 1] != '/') {
		full += '/';
	}
	full += name;

	struct stat info;
	if (stat(full.c_str(), &info) != 0) {
		data->dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
		return;
	}

	if (S_ISDIR(info.st_mode)) {
		data->dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
	} else if ((info.st_mode & S_IWUSR) == 0) {
		data->dwFileAttributes = FILE_ATTRIBUTE_READONLY;
	} else {
		data->dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
	}

	data->nFileSizeLow = (DWORD)((unsigned long long)info.st_size & 0xFFFFFFFFULL);
	data->nFileSizeHigh = (DWORD)((unsigned long long)info.st_size >> 32);

	/*
	**	There is no portable creation time. macOS has st_birthtime; elsewhere the inode change
	**	time is the closest thing, and is what the engine's "which of these two files is newer"
	**	comparisons see. Only ftLastWriteTime is load bearing (FileInfo::timestamp*).
	*/
#ifdef __APPLE__
	WWPlatform::Win32::Time_To_File_Time(info.st_birthtime, 0, data->ftCreationTime);
#else
	WWPlatform::Win32::Time_To_File_Time(info.st_ctime, 0, data->ftCreationTime);
#endif
	WWPlatform::Win32::Time_To_File_Time(info.st_atime, 0, data->ftLastAccessTime);
	WWPlatform::Win32::Time_To_File_Time(info.st_mtime, 0, data->ftLastWriteTime);
}


bool Case_Insensitive_Less(const std::string & left, const std::string & right)
{
	return strcasecmp(left.c_str(), right.c_str()) < 0;
}


/*
**	Translate a Windows spelled path for reading. Returns the separator converted spelling when
**	nothing on disk matches, so the caller can report the error against a sensible name.
*/
std::string Resolved(const char * path, bool parent_must_exist_only = false)
{
	StringClass resolved;
	WWPlatform::Path::Resolve(path, resolved, parent_must_exist_only);
	return std::string(resolved.Peek_Buffer());
}

}	// anonymous namespace


extern "C" {

/***********************************************************************************************
 *                                                                                             *
 *  Last error.                                                                                 *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

DWORD GetLastError()
{
	return WWPlatform::Win32::_LastError;
}


void SetLastError(DWORD code)
{
	WWPlatform::Win32::Set_Last_Error(code);
}


/***********************************************************************************************
 *                                                                                             *
 *  Directory enumeration.                                                                      *
 *                                                                                             *
 *  Win32 returns "." and ".." first when the pattern matches them, and every call site in the   *
 *  engine skips them by name, so they are produced here in the same place rather than filtered  *
 *  out -- a call site that counts entries counts the same number on both platforms. The order   *
 *  of the rest is not Windows' order (which is the filesystem's); it is case insensitive        *
 *  alphabetical, which is at least the same on every run.                                       *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

HANDLE FindFirstFileA(LPCSTR path, LPWIN32_FIND_DATAA data)
{
	if (path == nullptr || *path == 0 || data == nullptr) {
		WWPlatform::Win32::Set_Last_Error(ERROR_FILE_NOT_FOUND);
		return INVALID_HANDLE_VALUE;
	}

	std::string directory;
	std::string pattern;
	Split_Path(Resolved(path, true).c_str(), directory, pattern);

	DIR * handle = opendir(directory.c_str());
	if (handle == nullptr) {
		WWPlatform::Win32::Set_Last_Error(ERROR_PATH_NOT_FOUND);
		return INVALID_HANDLE_VALUE;
	}

	FindContextClass * context = new FindContextClass;
	context->Directory = directory;

	std::vector<std::string> names;
	bool dot = false;
	bool dot_dot = false;

	for (const struct dirent * entry = readdir(handle); entry != nullptr; entry = readdir(handle)) {
		if (!WWPlatform::Win32::Name_Matches(pattern.c_str(), entry->d_name)) {
			continue;
		}
		if (strcmp(entry->d_name, ".") == 0) {
			dot = true;
		} else if (strcmp(entry->d_name, "..") == 0) {
			dot_dot = true;
		} else {
			names.push_back(entry->d_name);
		}
	}
	closedir(handle);

	std::sort(names.begin(), names.end(), Case_Insensitive_Less);
	if (dot_dot) {
		names.insert(names.begin(), "..");
	}
	if (dot) {
		names.insert(names.begin(), ".");
	}
	context->Names.swap(names);

	if (context->Names.empty()) {
		delete context;
		WWPlatform::Win32::Set_Last_Error(ERROR_FILE_NOT_FOUND);
		return INVALID_HANDLE_VALUE;
	}

	Fill_Find_Data(context->Directory, context->Names[0], data);
	context->Next = 1;
	WWPlatform::Win32::Set_Last_Error(ERROR_SUCCESS);
	return (HANDLE)context;
}


BOOL FindNextFileA(HANDLE handle, LPWIN32_FIND_DATAA data)
{
	if (handle == nullptr || handle == INVALID_HANDLE_VALUE || data == nullptr) {
		WWPlatform::Win32::Set_Last_Error(WWPLATFORM_ERROR_INVALID_HANDLE);
		return FALSE;
	}

	FindContextClass * context = (FindContextClass *)handle;
	if (context->Next >= context->Names.size()) {
		WWPlatform::Win32::Set_Last_Error(ERROR_NO_MORE_FILES);
		return FALSE;
	}

	Fill_Find_Data(context->Directory, context->Names[context->Next], data);
	++context->Next;
	return TRUE;
}


BOOL FindClose(HANDLE handle)
{
	if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
		/*
		**	Win32 fails this too, and the engine passes INVALID_HANDLE_VALUE here: the
		**	FindFirstFile() loops call FindClose() outside the "did it open" test.
		*/
		WWPlatform::Win32::Set_Last_Error(WWPLATFORM_ERROR_INVALID_HANDLE);
		return FALSE;
	}

	delete (FindContextClass *)handle;
	return TRUE;
}


/***********************************************************************************************
 *                                                                                             *
 *  File and directory operations.                                                              *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

BOOL DeleteFileA(LPCSTR path)
{
	if (path == nullptr || *path == 0) {
		WWPlatform::Win32::Set_Last_Error(ERROR_FILE_NOT_FOUND);
		return FALSE;
	}

	if (unlink(Resolved(path).c_str()) != 0) {
		WWPlatform::Win32::Set_Last_Error_From_Errno(errno);
		return FALSE;
	}
	return TRUE;
}


BOOL CopyFileA(LPCSTR source, LPCSTR destination, BOOL fail_if_exists)
{
	if (source == nullptr || destination == nullptr || *source == 0 || *destination == 0) {
		WWPlatform::Win32::Set_Last_Error(ERROR_FILE_NOT_FOUND);
		return FALSE;
	}

	std::string from = Resolved(source);
	std::string to = Resolved(destination, true);

	if (fail_if_exists && access(to.c_str(), F_OK) == 0) {
		WWPlatform::Win32::Set_Last_Error(ERROR_ALREADY_EXISTS);
		return FALSE;
	}

	FILE * input = fopen(from.c_str(), "rb");
	if (input == nullptr) {
		WWPlatform::Win32::Set_Last_Error_From_Errno(errno);
		return FALSE;
	}

	FILE * output = fopen(to.c_str(), "wb");
	if (output == nullptr) {
		WWPlatform::Win32::Set_Last_Error_From_Errno(errno);
		fclose(input);
		return FALSE;
	}

	char buffer[64 * 1024];
	bool failed = false;
	for (;;) {
		size_t read = fread(buffer, 1, sizeof(buffer), input);
		if (read == 0) {
			failed = (ferror(input) != 0);
			break;
		}
		if (fwrite(buffer, 1, read, output) != read) {
			failed = true;
			break;
		}
	}

	if (failed) {
		WWPlatform::Win32::Set_Last_Error_From_Errno(errno);
	}
	if (fclose(output) != 0) {
		WWPlatform::Win32::Set_Last_Error_From_Errno(errno);
		failed = true;
	}
	fclose(input);

	if (failed) {
		unlink(to.c_str());
		return FALSE;
	}

	/*
	**	CopyFile() copies the file attributes with the contents; the permission bits are the
	**	only part of that which has a meaning here.
	*/
	struct stat info;
	if (stat(from.c_str(), &info) == 0) {
		chmod(to.c_str(), info.st_mode & 07777);
	}
	return TRUE;
}


BOOL CreateDirectoryA(LPCSTR path, LPSECURITY_ATTRIBUTES)
{
	if (path == nullptr || *path == 0) {
		WWPlatform::Win32::Set_Last_Error(ERROR_PATH_NOT_FOUND);
		return FALSE;
	}

	if (mkdir(Resolved(path, true).c_str(), 0777) != 0) {
		WWPlatform::Win32::Set_Last_Error_From_Errno(errno);
		return FALSE;
	}
	return TRUE;
}


/*
**	Both current directory calls are the process wide ones, and both are what
**	Path::Get_Current_Directory()/Path::Set_Current_Directory() already do; they are spelled out
**	again here rather than delegated because Win32's return values carry the buffer size
**	protocol: the required size, including the terminator, when the buffer is too small.
*/
DWORD GetCurrentDirectoryA(DWORD size, LPSTR buffer)
{
	char path[PATH_MAX];
	if (getcwd(path, sizeof(path)) == nullptr) {
		WWPlatform::Win32::Set_Last_Error_From_Errno(errno);
		return 0;
	}

	DWORD length = (DWORD)strlen(path);
	if (buffer == nullptr || size <= length) {
		return length + 1;
	}

	memcpy(buffer, path, length + 1);
	return length;
}


BOOL SetCurrentDirectoryA(LPCSTR path)
{
	if (path == nullptr || *path == 0) {
		WWPlatform::Win32::Set_Last_Error(ERROR_PATH_NOT_FOUND);
		return FALSE;
	}

	if (chdir(Resolved(path).c_str()) != 0) {
		WWPlatform::Win32::Set_Last_Error_From_Errno(errno);
		return FALSE;
	}
	return TRUE;
}


/*
**	GetFileTime() is called on what FileClass::Get_File_Handle() returns, which off Windows is
**	the RAWFILE stdio stream rather than a kernel handle -- see verchk.cpp's
**	GetFileCreationTime(). Anything else is refused instead of being reinterpreted.
*/
BOOL GetFileTime(HANDLE handle, LPFILETIME creation, LPFILETIME last_access, LPFILETIME last_write)
{
	if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
		WWPlatform::Win32::Set_Last_Error(WWPLATFORM_ERROR_INVALID_HANDLE);
		return FALSE;
	}

	int descriptor = fileno((FILE *)handle);
	struct stat info;
	if (descriptor < 0 || fstat(descriptor, &info) != 0) {
		WWPlatform::Win32::Set_Last_Error_From_Errno(errno);
		return FALSE;
	}

	if (creation != nullptr) {
#ifdef __APPLE__
		WWPlatform::Win32::Time_To_File_Time(info.st_birthtime, 0, *creation);
#else
		WWPlatform::Win32::Time_To_File_Time(info.st_ctime, 0, *creation);
#endif
	}
	if (last_access != nullptr) {
		WWPlatform::Win32::Time_To_File_Time(info.st_atime, 0, *last_access);
	}
	if (last_write != nullptr) {
		WWPlatform::Win32::Time_To_File_Time(info.st_mtime, 0, *last_write);
	}
	return TRUE;
}

}	// extern "C"

#endif	// WWPLATFORM_WIN32_COMPAT
