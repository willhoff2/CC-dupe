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

// FILE: SerializedDateTime.cpp ///////////////////////////////////////////////////////////////////
// Desc:   The current local civil time, in the layout the replay header stores it in.
///////////////////////////////////////////////////////////////////////////////////////////////////

// USER INCLUDES //////////////////////////////////////////////////////////////////////////////////
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine
#include "Common/SerializedDateTime.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/time.h>
#include <time.h>
#endif

// ------------------------------------------------------------------------------------------------
SerializedDateTime getLocalSerializedDateTime()
{
	SerializedDateTime when;

#if defined(_WIN32)

	SYSTEMTIME systemTime;
	GetLocalTime( &systemTime );

	when.year = systemTime.wYear;
	when.month = systemTime.wMonth;
	when.dayOfWeek = systemTime.wDayOfWeek;
	when.day = systemTime.wDay;
	when.hour = systemTime.wHour;
	when.minute = systemTime.wMinute;
	when.second = systemTime.wSecond;
	when.milliseconds = systemTime.wMilliseconds;

#else

	// TheSuperHackers @port GetLocalTime() reports milliseconds; gettimeofday() reports
	// microseconds against the same clock localtime_r() reads, so the two agree to the second.
	struct timeval now;
	gettimeofday( &now, nullptr );

	struct tm local;
	localtime_r( &now.tv_sec, &local );

	when.year = (UnsignedShort)(local.tm_year + 1900);
	when.month = (UnsignedShort)(local.tm_mon + 1);
	when.dayOfWeek = (UnsignedShort)local.tm_wday;
	when.day = (UnsignedShort)local.tm_mday;
	when.hour = (UnsignedShort)local.tm_hour;
	when.minute = (UnsignedShort)local.tm_min;
	when.second = (UnsignedShort)local.tm_sec;
	when.milliseconds = (UnsignedShort)(now.tv_usec / 1000);

#endif

	return when;
}
