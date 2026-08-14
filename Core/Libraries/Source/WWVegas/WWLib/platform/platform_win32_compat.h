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
 *  Shared internals of the Win32 file/module/locale compatibility layer. Unlike the other      *
 *  platform_* modules this layer defines the Win32 entry points *themselves*, under their own   *
 *  names and with C linkage, so that the several hundred call sites that spell them            *
 *  FindFirstFile()/CopyFile()/GetDateFormat() link off Windows without being touched. See      *
 *  docs/porting/win32-file-api-seam.md.                                                        *
 *                                                                                             *
 *  Nothing here is compiled on Windows: there the real API is the implementation, and defining *
 *  these names would collide with kernel32. The whole layer is inside #ifndef _WIN32.          *
 *                                                                                             *
 *  The declarations these definitions must match come from the port's own <windows.h>, which    *
 *  today is scripts/native-port-shims/windows.h and is on the include path in the probe's and   *
 *  the native build's shimmed mode. Where it is absent (the unshimmed mode, which measures      *
 *  "what compiles with no Windows headers at all") every translation unit in the layer compiles *
 *  to nothing rather than inventing a second, possibly divergent, set of Win32 typedefs. The    *
 *  linkage of every definition therefore comes from that header's own extern "C" block, which   *
 *  is the point: a C++-linkage definition of a C-linkage declaration links against nothing.     *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#pragma once

#ifndef _WIN32

#if defined(__has_include)
#if __has_include(<windows.h>)
#define WWPLATFORM_WIN32_COMPAT 1
#endif
#endif

#ifdef WWPLATFORM_WIN32_COMPAT

#include <stdarg.h>
#include <time.h>
#include <windows.h>

namespace WWPlatform
{

namespace Win32
{

/*
**	The GetLastError()/SetLastError() value, per thread. Only the codes the engine actually
**	compares against are mapped; anything else keeps its errno value shifted into the
**	application range, and is turned back into text by FormatMessage through Errno_From_Error().
*/
void Set_Last_Error(DWORD code);
void Set_Last_Error_From_Errno(int error_number);
DWORD Error_From_Errno(int error_number);
int Errno_From_Error(DWORD code);

/*
**	FILETIME is 100ns ticks since 1601; time_t is seconds since 1970.
*/
void Time_To_File_Time(time_t seconds, long nanoseconds, FILETIME & result);

/*
**	Win32 wildcard matching for one filename, case insensitively, as FindFirstFile() does it.
*/
bool Name_Matches(const char * pattern, const char * name);

/*
**	One line on stderr the first time a deliberately unimplemented entry point is called, and
**	nothing on the calls after that. Loud once rather than loud every frame, because some of
**	these sit in code that retries.
*/
void Report_Stub(const char * api, const char * detail);

}	// namespace Win32

}	// namespace WWPlatform

#endif	// WWPLATFORM_WIN32_COMPAT

#endif	// !_WIN32
