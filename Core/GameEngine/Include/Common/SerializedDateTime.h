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

// FILE: SerializedDateTime.h /////////////////////////////////////////////////////////////////////
// Desc:   A civil (wall clock) date and time in the exact layout that the replay header has always
//         written it in: eight little-endian 16-bit fields, sixteen bytes, no padding.
//
// TheSuperHackers @port The replay header used to write a Win32 SYSTEMTIME straight to disk
// (`m_file->write(&systemTime, sizeof(systemTime))`), which made a Windows type part of the file
// format. This structure has SYSTEMTIME's member order and widths, so the bytes on disk are
// unchanged on Windows, and the format no longer names a type that only exists there.
//
// The fields are local civil time, not UTC, because that is what the format has always held and
// what the replay browser displays; converting would change file content, not layout. See
// docs/porting/xfer-64bit-audit.md.
///////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Lib/BaseType.h"

#include <stddef.h>

#include <Utility/CppMacros.h>

// ------------------------------------------------------------------------------------------------
/** A date and time as it appears on disk. Member order, widths and the absence of padding are
	* part of the save/replay file format and are asserted below. */
// ------------------------------------------------------------------------------------------------
struct SerializedDateTime
{
	UnsignedShort year;
	UnsignedShort month;
	UnsignedShort dayOfWeek;			///< 0 = Sunday, as Win32 GetLocalTime() reports it
	UnsignedShort day;
	UnsignedShort hour;
	UnsignedShort minute;
	UnsignedShort second;
	UnsignedShort milliseconds;
};

STATIC_ASSERT_ALWAYS(sizeof(SerializedDateTime) == 16, "SerializedDateTime must be 16 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(SerializedDateTime, year) == 0, "SerializedDateTime::year must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(SerializedDateTime, month) == 2, "SerializedDateTime::month must be at offset 2");
STATIC_ASSERT_ALWAYS(offsetof(SerializedDateTime, dayOfWeek) == 4, "SerializedDateTime::dayOfWeek must be at offset 4");
STATIC_ASSERT_ALWAYS(offsetof(SerializedDateTime, day) == 6, "SerializedDateTime::day must be at offset 6");
STATIC_ASSERT_ALWAYS(offsetof(SerializedDateTime, hour) == 8, "SerializedDateTime::hour must be at offset 8");
STATIC_ASSERT_ALWAYS(offsetof(SerializedDateTime, minute) == 10, "SerializedDateTime::minute must be at offset 10");
STATIC_ASSERT_ALWAYS(offsetof(SerializedDateTime, second) == 12, "SerializedDateTime::second must be at offset 12");
STATIC_ASSERT_ALWAYS(offsetof(SerializedDateTime, milliseconds) == 14, "SerializedDateTime::milliseconds must be at offset 14");

/** The current local civil time. Equivalent of Win32 GetLocalTime(), and implemented with it
	* where it exists. */
SerializedDateTime getLocalSerializedDateTime();
