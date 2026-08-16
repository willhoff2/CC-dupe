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
 *  The module, process and system-information half of the Win32 compatibility layer: dynamic    *
 *  library loading over dlopen(), the process's own path and command line, GlobalAlloc() as     *
 *  malloc(), and the two "how big is this machine" queries.                                    *
 *                                                                                             *
 *  itoa() lives here too. It is a CRT spelling rather than a Win32 one, but it is the same      *
 *  shape of problem -- an MSVC name the engine calls in gameplay code (DisconnectMenu,          *
 *  FirewallHelper) with a body that is trivially portable -- and it is declared extern "C" by    *
 *  the port's <stdlib.h>, so it has to be defined with C linkage like everything else here.     *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#include "WWLib/platform/platform_win32_compat.h"

#ifdef WWPLATFORM_WIN32_COMPAT

#include "WWLib/platform/platform_path.h"

#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include <string>

#ifdef __APPLE__
#include <crt_externs.h>
#include <malloc/malloc.h>
#include <sys/sysctl.h>
#else
#include <malloc.h>
#include <sys/sysinfo.h>
#endif

namespace
{

/*
**	The process command line, rebuilt once as GetCommandLine() hands it back: one string, the
**	arguments separated by spaces, quoted where they contain one. CommandLine.cpp re-splits it.
**	Windows keeps the original, unparsed string; this is a reconstruction, so an argument
**	containing a quote character does not survive the round trip.
*/
std::string & Command_Line()
{
	static std::string _line;
	static bool _built = false;

	if (_built) {
		return _line;
	}
	_built = true;

	int count = 0;
	char ** arguments = nullptr;

#ifdef __APPLE__
	count = *_NSGetArgc();
	arguments = *_NSGetArgv();
#else
	/*
	**	/proc/self/cmdline is NUL separated, and is the only way to see argv from a library.
	*/
	FILE * file = fopen("/proc/self/cmdline", "rb");
	if (file != nullptr) {
		std::string raw;
		int character = fgetc(file);
		while (character != EOF) {
			raw += (char)character;
			character = fgetc(file);
		}
		fclose(file);

		size_t start = 0;
		while (start < raw.size()) {
			std::string argument = raw.c_str() + start;
			if (!_line.empty()) {
				_line += ' ';
			}
			if (argument.find(' ') != std::string::npos) {
				_line += '"';
				_line += argument;
				_line += '"';
			} else {
				_line += argument;
			}
			start += argument.size() + 1;
		}
		return _line;
	}
#endif

	for (int index = 0; index < count && arguments != nullptr; ++index) {
		if (arguments[index] == nullptr) {
			break;
		}
		std::string argument = arguments[index];
		if (!_line.empty()) {
			_line += ' ';
		}
		if (argument.find(' ') != std::string::npos) {
			_line += '"';
			_line += argument;
			_line += '"';
		} else {
			_line += argument;
		}
	}
	return _line;
}


/*
**	"D3D8.DLL" is not a filename on this platform. The name is tried as given first, so that a
**	caller which already knows the platform's name works, then as the library naming convention
**	of the host with the .DLL suffix removed. Failing is a normal outcome: every caller in the
**	engine tests the result -- Except.cpp's IMAGEHLP.DLL, DbgHelpLoader's DBGHELP.DLL.
*/
void * Open_Library(const char * name)
{
	void * handle = dlopen(name, RTLD_LAZY | RTLD_LOCAL);
	if (handle != nullptr) {
		return handle;
	}

	std::string base = name;
	size_t dot = base.rfind('.');
	if (dot != std::string::npos) {
		std::string extension = base.substr(dot);
		for (size_t index = 0; index < extension.size(); ++index) {
			extension[index] = (char)tolower((unsigned char)extension[index]);
		}
		if (extension == ".dll") {
			base.erase(dot);
		}
	}

	/*
	**	Strip a Windows directory part as well, since the platform's loader search path is what
	**	decides where a library comes from here.
	*/
	size_t separator = base.find_last_of("\\/");
	if (separator != std::string::npos) {
		base.erase(0, separator + 1);
	}

	for (size_t index = 0; index < base.size(); ++index) {
		base[index] = (char)tolower((unsigned char)base[index]);
	}

#ifdef __APPLE__
	std::string candidate = "lib" + base + ".dylib";
#else
	std::string candidate = "lib" + base + ".so";
#endif
	return dlopen(candidate.c_str(), RTLD_LAZY | RTLD_LOCAL);
}

}	// anonymous namespace


extern "C" {

/***********************************************************************************************
 *                                                                                             *
 *  Modules.                                                                                    *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

HMODULE LoadLibraryA(LPCSTR name)
{
	if (name == nullptr || *name == 0) {
		WWPlatform::Win32::Set_Last_Error(ERROR_FILE_NOT_FOUND);
		return nullptr;
	}

	void * handle = Open_Library(name);
	if (handle == nullptr) {
		WWPlatform::Win32::Set_Last_Error(ERROR_FILE_NOT_FOUND);
	}
	return (HMODULE)handle;
}


BOOL FreeLibrary(HMODULE module)
{
	if (module == nullptr) {
		WWPlatform::Win32::Set_Last_Error(ERROR_FILE_NOT_FOUND);
		return FALSE;
	}
	return dlclose((void *)module) == 0 ? TRUE : FALSE;
}


FARPROC GetProcAddress(HMODULE module, LPCSTR name)
{
	if (module == nullptr || name == nullptr) {
		WWPlatform::Win32::Set_Last_Error(ERROR_FILE_NOT_FOUND);
		return nullptr;
	}

	/*
	**	A function pointer through void* is the documented dlsym() idiom and is what every POSIX
	**	caller does; FARPROC is the Win32 spelling of the same thing.
	*/
	void * symbol = dlsym((void *)module, name);
	if (symbol == nullptr) {
		WWPlatform::Win32::Set_Last_Error(ERROR_FILE_NOT_FOUND);
	}
	return (FARPROC)symbol;
}


/*
**	GetModuleFileName(nullptr, ...) is "where is the running executable", which is what every
**	call site in the engine asks for. A non-null module is not answered: nothing asks, and
**	dladdr() would need an address inside the library rather than its handle.
*/
DWORD GetModuleFileNameA(HMODULE module, LPSTR buffer, DWORD size)
{
	if (buffer == nullptr || size == 0) {
		return 0;
	}

	if (module != nullptr) {
		WWPlatform::Win32::Report_Stub("GetModuleFileNameA",
			"only the running executable (a null module) can be named here");
		buffer[0] = 0;
		return 0;
	}

	if (!WWPlatform::Path::Get_Executable_Path(buffer, (unsigned)size)) {
		buffer[0] = 0;
		return 0;
	}
	return (DWORD)strlen(buffer);
}


/*
**	The wide form, over wchar_t as the engine has it today: 4 bytes here against 2 on MSVC. The
**	"what is a wide string off Windows" question is deliberately still open (see
**	docs/porting/widechar-fallout.md), so this widens byte by byte -- correct for the ASCII paths
**	the executable actually has, and no worse than the mismatch the call sites already carry.
*/
DWORD GetModuleFileNameW(HMODULE module, LPWSTR buffer, DWORD size)
{
	if (buffer == nullptr || size == 0) {
		return 0;
	}

	char narrow[PATH_MAX];
	DWORD length = GetModuleFileNameA(module, narrow, (DWORD)sizeof(narrow));
	if (length == 0) {
		buffer[0] = 0;
		return 0;
	}

	DWORD count = 0;
	while (count < size - 1 && narrow[count] != 0) {
		buffer[count] = (wchar_t)(unsigned char)narrow[count];
		++count;
	}
	buffer[count] = 0;
	return count;
}


/*
**	GetSystemDirectory() exists here for DbgHelpLoader, which builds "<system>\\DBGHELP.DLL" and
**	loads it. There is no system directory to name off Windows and no DbgHelp to find in it, so
**	this reports an empty name; the loader then fails its LoadLibrary() the way it already does
**	when the DLL is absent, which is the documented state of crash reporting in this port.
*/
UINT GetSystemDirectoryA(LPSTR buffer, UINT size)
{
	WWPlatform::Win32::Report_Stub("GetSystemDirectoryA",
		"there is no Windows system directory; callers get an empty path");
	if (buffer != nullptr && size > 0) {
		buffer[0] = 0;
	}
	return 0;
}


LPSTR GetCommandLineA()
{
	return (LPSTR)Command_Line().c_str();
}


/***********************************************************************************************
 *                                                                                             *
 *  Global memory. GlobalAlloc() is a 16-bit inheritance the engine still uses as a plain        *
 *  allocator (profile.cpp, verchk.cpp, SystemAllocator.h). GMEM_MOVEABLE handles are not        *
 *  actually moveable here, so the handle is the pointer and Lock/Unlock are bookkeeping.        *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

HGLOBAL GlobalAlloc(UINT flags, SIZE_T bytes)
{
	void * memory = ((flags & GMEM_ZEROINIT) != 0) ? calloc(1, bytes ? bytes : 1)
	                                               : malloc(bytes ? bytes : 1);
	if (memory == nullptr) {
		WWPlatform::Win32::Set_Last_Error_From_Errno(ENOMEM);
	}
	return (HGLOBAL)memory;
}


LPVOID GlobalLock(HGLOBAL memory)
{
	return (LPVOID)memory;
}


BOOL GlobalUnlock(HGLOBAL)
{
	/*
	**	Win32 returns FALSE for a fixed block whose lock count is already zero, with
	**	GetLastError() == NO_ERROR. Every call site here ignores the result.
	*/
	WWPlatform::Win32::Set_Last_Error(ERROR_SUCCESS);
	return FALSE;
}


HGLOBAL GlobalHandle(LPCVOID memory)
{
	/*
	**	The inverse of GlobalLock(), which is what <windowsx.h>'s GlobalFreePtr() family is
	**	written in terms of. A handle here is the pointer, so its inverse is the identity too.
	*/
	return (HGLOBAL)memory;
}


/*
**	Win32 reports the size it actually committed, which is the requested size rounded up to the
**	allocator's granularity, and GameMemory.cpp's MEMORYPOOL_DEBUG accounting depends on it being
**	the *usable* size rather than the requested one: it fills the whole block with the filler
**	value and adds the same number to its running total that sysFree() later subtracts. So this
**	asks the C library the same question -- malloc_usable_size()/malloc_size() -- rather than
**	remembering the requested size in a side table. Both return 0 for a null pointer, as
**	GlobalSize() does for an invalid handle.
*/
SIZE_T GlobalSize(HGLOBAL memory)
{
	if (memory == nullptr) {
		WWPlatform::Win32::Set_Last_Error(ERROR_INVALID_HANDLE);
		return 0;
	}

#ifdef __APPLE__
	return (SIZE_T)malloc_size((const void *)memory);
#else
	return (SIZE_T)malloc_usable_size((void *)memory);
#endif
}


HGLOBAL GlobalFree(HGLOBAL memory)
{
	free((void *)memory);
	return nullptr;
}


/***********************************************************************************************
 *                                                                                             *
 *  System information and the local clock.                                                     *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

/*
**	There is no Windows version to report. The call sites are two: a Win9x test that has to come
**	out false (GameState.cpp picks the ANSI date format on Win9x), and cpudetect.cpp's
**	human-readable OS string. So this answers "an NT-family Windows", which is what every
**	behavioural branch in the engine wants, and puts the real host in szCSDVersion where
**	cpudetect prints it.
*/
BOOL GetVersionExA(LPOSVERSIONINFOA info)
{
	if (info == nullptr) {
		return FALSE;
	}

	info->dwMajorVersion = 10;
	info->dwMinorVersion = 0;
	info->dwBuildNumber = 0;
	info->dwPlatformId = VER_PLATFORM_WIN32_NT;
	info->szCSDVersion[0] = 0;

	struct utsname host;
	if (uname(&host) == 0) {
		snprintf(info->szCSDVersion, sizeof(info->szCSDVersion), "%s %s (%s)",
			host.sysname, host.release, host.machine);
	}
	return TRUE;
}


void GlobalMemoryStatus(LPMEMORYSTATUS status)
{
	if (status == nullptr) {
		return;
	}

	memset(status, 0, sizeof(*status));
	status->dwLength = sizeof(*status);

	unsigned long long total = 0;
	unsigned long long available = 0;

#ifdef __APPLE__
	int name[2] = { CTL_HW, HW_MEMSIZE };
	unsigned long long memory = 0;
	size_t length = sizeof(memory);
	if (sysctl(name, 2, &memory, &length, nullptr, 0) == 0) {
		total = memory;
	}
	/*
	**	macOS has no cheap "free physical memory" figure that means anything (the page cache
	**	owns whatever is not in use), so the available figure is the total. GameClient.cpp only
	**	uses the difference between two samples, and cpudetect.cpp only prints it.
	*/
	available = total;
#else
	struct sysinfo information;
	if (sysinfo(&information) == 0) {
		unsigned long long unit = information.mem_unit ? information.mem_unit : 1;
		total = (unsigned long long)information.totalram * unit;
		available = (unsigned long long)information.freeram * unit;
	}
#endif

	status->dwTotalPhys = (SIZE_T)total;
	status->dwAvailPhys = (SIZE_T)available;
	status->dwTotalVirtual = (SIZE_T)total;
	status->dwAvailVirtual = (SIZE_T)available;
	status->dwMemoryLoad = (total != 0) ? (DWORD)(((total - available) * 100) / total) : 0;
}


void GetLocalTime(LPSYSTEMTIME system_time)
{
	if (system_time == nullptr) {
		return;
	}

	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);

	struct tm parts;
	localtime_r(&now.tv_sec, &parts);

	system_time->wYear = (WORD)(parts.tm_year + 1900);
	system_time->wMonth = (WORD)(parts.tm_mon + 1);
	system_time->wDayOfWeek = (WORD)parts.tm_wday;
	system_time->wDay = (WORD)parts.tm_mday;
	system_time->wHour = (WORD)parts.tm_hour;
	system_time->wMinute = (WORD)parts.tm_min;
	system_time->wSecond = (WORD)parts.tm_sec;
	system_time->wMilliseconds = (WORD)(now.tv_nsec / 1000000);
}


/*
**	The system double click time, which is a mouse setting the platform layer does not read
**	anywhere yet. 500ms is the Windows default, and is what the engine's own default INI value
**	was tuned against.
*/
UINT GetDoubleClickTime()
{
	return 500;
}


/***********************************************************************************************
 *                                                                                             *
 *  CRT spelling.                                                                               *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

/*
**	itoa(). MSVC's takes any radix from 2 to 36 and, for radix 10, writes a leading '-'. The
**	buffer contract is the caller's: 33 bytes is enough for every radix, which is what the call
**	sites allocate.
*/
char * itoa(int value, char * buffer, int radix)
{
	if (buffer == nullptr) {
		return buffer;
	}
	if (radix < 2 || radix > 36) {
		buffer[0] = 0;
		return buffer;
	}

	/*
	**	Negative values are only signed in base 10, exactly as MSVC does it; in every other
	**	radix the value is formatted as unsigned.
	*/
	unsigned int magnitude;
	bool negative = false;
	if (radix == 10 && value < 0) {
		negative = true;
		magnitude = (unsigned int)(-(long long)value);
	} else {
		magnitude = (unsigned int)value;
	}

	char digits[33];
	int count = 0;
	do {
		unsigned int digit = magnitude % (unsigned int)radix;
		digits[count++] = (char)((digit < 10) ? ('0' + digit) : ('a' + digit - 10));
		magnitude /= (unsigned int)radix;
	} while (magnitude != 0);

	char * write = buffer;
	if (negative) {
		*write++ = '-';
	}
	while (count > 0) {
		*write++ = digits[--count];
	}
	*write = 0;
	return buffer;
}

}	// extern "C"

#endif	// WWPLATFORM_WIN32_COMPAT
