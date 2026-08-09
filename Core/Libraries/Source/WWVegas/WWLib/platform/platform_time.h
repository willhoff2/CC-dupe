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
 *  High resolution timing for the platforms that have no QueryPerformanceCounter. The shape    *
 *  matches the Win32 calls so the call sites stay recognisable.                                *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#pragma once

#ifndef _WIN32

namespace WWPlatform
{

/*
**	Ticks of a monotonic clock, and the number of those ticks in a second. Equivalent of
**	QueryPerformanceCounter() and QueryPerformanceFrequency(), which never fail here.
*/
unsigned long long Get_Performance_Counter();
unsigned long long Get_Performance_Frequency();

}	// namespace WWPlatform

#endif // !_WIN32
