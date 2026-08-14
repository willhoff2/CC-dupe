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

//////////////////////////////////////////////////////////////////////////////
//
// The non-Windows side of the EA/Debug library. Every entry point here stands in for the Win32
// call the library made at that spot, in the same shape, so the call sites keep their original
// Windows code under #ifdef _WIN32 and Windows behaviour is untouched.
//
// This header is only compiled off Windows; on Windows the call sites include <windows.h> as
// they always did. See docs/porting/debug-and-profile-libs.md for what is real here and what is
// a deliberate stub.
//
//////////////////////////////////////////////////////////////////////////////

#pragma once

#ifndef _WIN32

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>   // _exit()

/*
  wsprintf() is user32's sprintf and is spelled that way all over this library. Off Windows it is
  sprintf, with the same 1024 byte contract the Win32 version documents.
*/
inline int wvsprintf(char *buffer, const char *format, va_list args)
{
  return vsprintf(buffer,format,args);
}

inline int wsprintf(char *buffer, const char *format, ...)
{
  va_list args;
  va_start(args,format);
  int result=wvsprintf(buffer,format,args);
  va_end(args);
  return result;
}

/*
  The MSVC CRT radix conversions, which this library uses for every number it prints. They are
  declaration-only in scripts/native-port-shims/stdlib.h, i.e. counted as open CRT debt, and
  Dependencies/Utility belongs to the crt-and-widechar slice, so the definitions live here for now
  rather than in the shared compat headers. Same C linkage trick as string_compat.h, for the same
  reason. If the CRT slice adopts these, delete them from here.
*/
#ifdef __cplusplus
extern "C" {
#endif

inline char *_ui64toa(unsigned long long value, char *buffer, int radix)
{
  // digits come out backwards, so fill from the end of a scratch buffer and copy back
  char scratch[65];
  char *p=scratch+sizeof(scratch);
  *--p=0;
  do
  {
    unsigned digit=(unsigned)(value%(unsigned)radix);
    *--p=(char)(digit<10?'0'+digit:'a'+digit-10);
    value/=(unsigned)radix;
  } while (value);
  strcpy(buffer,p);
  return buffer;
}

inline char *_i64toa(long long value, char *buffer, int radix)
{
  // only base 10 is signed, exactly as the CRT does it
  if (radix==10&&value<0)
  {
    *buffer='-';
    _ui64toa((unsigned long long)(-value),buffer+1,radix);
    return buffer;
  }
  return _ui64toa((unsigned long long)value,buffer,radix);
}

inline char *_ultoa(unsigned long value, char *buffer, int radix)
{
  return _ui64toa((unsigned long long)value,buffer,radix);
}

inline char *_itoa(int value, char *buffer, int radix)
{
  if (radix==10)
    return _i64toa((long long)value,buffer,radix);
  return _ui64toa((unsigned long long)(unsigned)value,buffer,radix);
}

#ifdef __cplusplus
}
#endif

namespace DebugPlatform
{

//----------------------------------------------------------------------------
// Memory. Equivalent of GlobalAlloc(GMEM_FIXED)/GlobalReAlloc()/GlobalFree().
//----------------------------------------------------------------------------

void *Alloc(unsigned numBytes);
void *ReAlloc(void *oldPtr, unsigned newSize);
void Free(void *ptr);

//----------------------------------------------------------------------------
// Process and environment.
//----------------------------------------------------------------------------

/// Equivalent of GetModuleFileName(nullptr,...). Empty string if it cannot be determined.
void GetExecutablePath(char *buf, unsigned bufSize);

/// Equivalent of GetComputerName().
void GetComputerName(char *buf, unsigned bufSize);

/// Equivalent of GetUserName().
void GetUserName(char *buf, unsigned bufSize);

/// Equivalent of GetLocalTime(), broken out into the fields this library formats.
void GetLocalTime(unsigned &year, unsigned &month, unsigned &day,
                  unsigned &hour, unsigned &minute, unsigned &second, unsigned &milliseconds);

/// Equivalent of GetTickCount(): milliseconds off a monotonic clock, wrapped to 32 bits.
unsigned GetTickCount();

/// Equivalent of TerminateProcess(GetCurrentProcess(),code). Does not return.
void TerminateProcess(unsigned exitCode);

/**
  The loud stand-in for the MessageBox() calls this library makes on its fatal paths. There is no
  dialog seam at this layer off Windows, so the text goes to stderr (and to the log the caller has
  already written) instead of to a window, and there is nothing to click: every caller of this
  either terminates or is already dying.
*/
void ReportFatal(const char *title, const char *text);

/// Equivalent of OutputDebugString(): the 'ods' I/O backend's destination.
void WriteDebuggerString(const char *text);

//----------------------------------------------------------------------------
// Pointer probes. Equivalent of IsBadReadPtr()/IsBadCodePtr(): true if the range cannot be read.
// Implemented by letting the kernel do the checking, so a bad pointer costs an EFAULT and not a
// signal, exactly as the Win32 calls promise.
//----------------------------------------------------------------------------

bool IsBadReadPtr(const void *ptr, unsigned numBytes);
bool IsBadCodePtr(const void *ptr);

//----------------------------------------------------------------------------
// Files, in the shape of the Win32 handle calls the library used.
//----------------------------------------------------------------------------

typedef int FileHandle;

const FileHandle INVALID_FILE_HANDLE = -1;

/// Equivalent of CreateFile(..,OPEN_EXISTING,..) for reading.
FileHandle OpenExisting(const char *fileName);

/// Equivalent of CreateFile(..,CREATE_ALWAYS,..) for writing.
FileHandle CreateAlways(const char *fileName);

/// Equivalent of CreateFile(..,OPEN_ALWAYS,..) plus SetFilePointer(..,FILE_END) for appending.
FileHandle OpenAppend(const char *fileName);

/// Equivalent of ReadFile()/WriteFile(). Returns the number of bytes transferred.
unsigned ReadFile(FileHandle handle, void *buf, unsigned numBytes);
unsigned WriteFile(FileHandle handle, const void *buf, unsigned numBytes);

/// Equivalent of CloseHandle() on a file handle.
void CloseFile(FileHandle handle);

/// Equivalent of CopyFile(src,dst,TRUE): fails if the destination exists.
enum CopyResult
{
  COPY_OK,             ///< destination written
  COPY_EXISTS,         ///< destination is already there (ERROR_FILE_EXISTS)
  COPY_FAILED          ///< anything else
};

CopyResult CopyFileNoOverwrite(const char *source, const char *destination);

//----------------------------------------------------------------------------
// Console. There is no AllocConsole() off Windows: a process either inherits a terminal or it
// does not, so the 'con' backend writes to stdout and never reads. See the doc.
//----------------------------------------------------------------------------

/// True if this process has a terminal that output can go to.
bool HasConsole();

/// Writes to the standard output.
void ConsoleWrite(const char *text);

//----------------------------------------------------------------------------
// Stack walking. This is a real implementation, not a stub: backtrace() to capture and
// dladdr() plus __cxa_demangle() to symbolise. See DebugStackwalk in debug_stack.h.
//----------------------------------------------------------------------------

/**
  Captures the return addresses of the calling thread's stack, innermost first.

  \param frames buffer to fill
  \param maxFrames size of that buffer
  \param skip number of innermost frames to drop (this function itself is always dropped)
  \return number of addresses written
*/
unsigned CaptureStack(void **frames, unsigned maxFrames, unsigned skip);

/**
  Resolves one code address to the module and symbol containing it.

  There is no line number: that needs the DWARF tables, which is what an external addr2line/atos
  does with the module name and the module relative offset this returns. Both are reported so the
  offset can be fed to those tools.

  \param addr address to resolve
  \param bufMod module (shared object) file name without path, may be nullptr
  \param sizeMod size of that buffer
  \param relMod address relative to the module's load address, may be nullptr
  \param bufSym symbol name, demangled when possible, may be nullptr
  \param sizeSym size of that buffer
  \param relSym address relative to the symbol, may be nullptr
  \return false if the address belongs to no known module
*/
bool ResolveAddress(unsigned long long addr,
                    char *bufMod, unsigned sizeMod, unsigned long long *relMod,
                    char *bufSym, unsigned sizeSym, unsigned long long *relSym);

//----------------------------------------------------------------------------
// Crash handling. The Win32 side of this library installs an unhandled exception filter and a
// per-thread SE translator, walks the faulting context and writes a minidump. Off Windows there
// is no SEH and no minidump; what there is, is a signal handler that logs a real backtrace.
//----------------------------------------------------------------------------

/**
  Installs handlers for the fatal signals (SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT).

  \param handler called with the signal number and its name; must not return normally
*/
void InstallFatalSignalHandlers(void (*handler)(int signalNumber, const char *signalName));

/// Restores the default disposition for one signal and re-raises it, so the process dies the way
/// it would have without a handler installed (core file, debugger, exit status).
void ReRaiseFatalSignal(int signalNumber);

}	// namespace DebugPlatform

#endif // !_WIN32
