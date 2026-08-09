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

#include "WWLib/platform/platform_time.h"

#ifndef _WIN32

#include <chrono>

namespace WWPlatform
{

typedef std::chrono::steady_clock ClockType;

/*
**	Microseconds. Callers sample fixed tick counts rather than fixed times, so a tick has to stay
**	close in magnitude to the 1.19 MHz counter this code was written against; nanoseconds would
**	shrink those sampling windows by three orders of magnitude.
*/
typedef std::chrono::duration<unsigned long long, std::micro> TickType;

unsigned long long Get_Performance_Counter()
{
	ClockType::duration since_epoch = ClockType::now().time_since_epoch();
	return std::chrono::duration_cast<TickType>(since_epoch).count();
}

unsigned long long Get_Performance_Frequency()
{
	return TickType::period::den / TickType::period::num;
}

}	// namespace WWPlatform

#endif // !_WIN32
