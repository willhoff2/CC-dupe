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

/////////////////////////////////////////////////////////////////////////EA-V1
// $File: //depot/GeneralsMD/Staging/code/Libraries/Source/debug/internal_except.h $
// $Author: mhoffe $
// $Revision: #1 $
// $DateTime: 2003/07/03 11:55:26 $
//
// (c) 2003 Electronic Arts
//
// Unhandled exception handler
//////////////////////////////////////////////////////////////////////////////

#pragma once

/// \internal exception handler
class DebugExceptionhandler
{
  DebugExceptionhandler(const DebugExceptionhandler&);
  DebugExceptionhandler& operator=(const DebugExceptionhandler&);

  // nobody can instantiate us
  DebugExceptionhandler();

#ifdef _WIN32

  /** \internal

    \brief Log exception location.

    \param dbg debug instance
    \param exptr exception pointers
  */
  static void LogExceptionLocation(Debug &dbg, struct _EXCEPTION_POINTERS *exptr);

  /** \internal

    \brief Log regular registers.

    \param dbg debug instance
    \param exptr exception pointers
  */
  static void LogRegisters(Debug &dbg, struct _EXCEPTION_POINTERS *exptr);

  /** \internal

    \brief Log FPU registers.

    \param dbg debug instance
    \param exptr exception pointers
  */
  static void LogFPURegisters(Debug &dbg, struct _EXCEPTION_POINTERS *exptr);

#endif // _WIN32

public:

#ifdef _WIN32

  /** \internal

    \brief Determine exception type.

    \param exptr exception pointers
    \param explanation exception explanation, buffer must be 512 chars
    \return exception type as string
  */
  static const char *GetExceptionType(struct _EXCEPTION_POINTERS *exptr, char *explanation);

  /** \internal

    \brief System exception filter
  */
  static long __stdcall ExceptionFilter(struct _EXCEPTION_POINTERS* pExPtrs);

#else // !_WIN32

  /** \internal

    \brief Last chance handler for the fatal signals.

    This is what stands in for ExceptionFilter() off Windows: same log, same real stack walk, but
    without anything that needs a Win32 EXCEPTION_RECORD (no register or FPU dump, no exception
    code, no minidump, no dialog). Called on the faulting thread, from the signal handler, and
    returns so that the signal can be re-raised with the default disposition.

    \param signalNumber the signal that was raised
    \param signalName its name, for the log
  */
  static void FatalSignalHandler(int signalNumber, const char *signalName);

#endif // _WIN32
};
