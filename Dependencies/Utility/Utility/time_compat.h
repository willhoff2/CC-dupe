/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 TheSuperHackers
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

// This file contains the time functions for compatibility with non-windows platforms.
#pragma once
#include <time.h>

// CLOCK_BOOTTIME is Linux-only. Elsewhere (macOS, BSD) CLOCK_MONOTONIC is the closest
// equivalent; it differs only in whether time spent suspended is counted.
#ifndef CLOCK_BOOTTIME
#define CLOCK_BOOTTIME CLOCK_MONOTONIC
#endif

#define TIMERR_NOERROR 0
typedef int MMRESULT;
static inline MMRESULT timeBeginPeriod(int) { return TIMERR_NOERROR; }
static inline MMRESULT timeEndPeriod(int) { return TIMERR_NOERROR; }

// TheSuperHackers @port timeGetTime() and GetTickCount() are 32-bit millisecond counters that
// wrap every ~49.7 days, and callers such as SysTimeClass::Get() and the network layer's
// timeout arithmetic depend on that unsigned wraparound. Truncating a 64-bit millisecond count
// to unsigned int reproduces it exactly (the result is the true count modulo 2^32), so the
// return type must stay exactly 32 bits wide. The epoch is time since boot on both platforms;
// no engine code depends on its absolute value, only on differences and on ordering.
inline unsigned int timeGetTime()
{
  struct timespec ts;
  clock_gettime(CLOCK_BOOTTIME, &ts);
  return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
inline unsigned int GetTickCount()
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  // Return ms since boot
  return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// TheSuperHackers @port The high resolution counter. Callers pass either a LARGE_INTEGER or a
// pointer to their own 64-bit integer cast to LARGE_INTEGER*, so the type has to keep the Win32
// layout: QuadPart at offset zero, LowPart/HighPart overlaid on it, little endian.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wnested-anon-types"
#pragma clang diagnostic ignored "-Wgnu-anonymous-struct"
#endif
typedef union _LARGE_INTEGER
{
  struct { unsigned int LowPart; int HighPart; };
  struct { unsigned int LowPart; int HighPart; } u;
  long long QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

// TheSuperHackers @port A monotonic clock in place of QueryPerformanceCounter(). The epoch is
// time since boot rather than the Win32 one, and the frequency is fixed at one tick per
// nanosecond rather than whatever the hardware reports; both are safe because every call site
// divides a difference of two counters by the reported frequency. Never fails, so the BOOL
// result is always non-zero, as it is on every Windows version the game supports.
inline int QueryPerformanceCounter(LARGE_INTEGER *counter)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  counter->QuadPart = (long long)ts.tv_sec * 1000000000LL + (long long)ts.tv_nsec;
  return 1;
}
inline int QueryPerformanceFrequency(LARGE_INTEGER *frequency)
{
  frequency->QuadPart = 1000000000LL;
  return 1;
}

