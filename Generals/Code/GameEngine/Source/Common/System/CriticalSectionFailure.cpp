/*
**	Command & Conquer Generals(tm)
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

// CriticalSectionFailure.cpp /////////////////////////////////////////////////////////////////
// TheSuperHackers @bugfix Devin 17/08/2026 What CriticalSection::enter() does when the mutex
// refuses to lock, off Windows.
//
// This file exists because of what the old failure path cost. CriticalSection used to hold a
// std::recursive_mutex, and std::recursive_mutex::lock() reports a failure the only way the
// standard library can: it throws std::system_error, whose message is a std::string, whose
// allocation is the engine's global operator new, which is DynamicMemoryAllocator, which enters
// the same CriticalSection that just failed -- and fails again, and allocates again. On Apple
// Silicon that turned one recoverable errno into nine crash reports with the stack guard page in
// them (docs/porting/real-input-menu-drive.md 4.3) and into the exit-time SIGSEGV in OpenAL's
// static destructors (docs/porting/apple-silicon-verification.md 8.5). The allocator's own
// critical-section path must therefore be able to report a problem without allocating.
//
// So: no exception, no std::string, no printf (glibc's stdio takes locks and buffers), no malloc.
// The message is formatted into a stack buffer by hand and handed to one write(2). What it prints
// is chosen to name the cause of the *next* occurrence rather than to guess at this one: the errno,
// which section it was, whether the memory manager was up, and the first bytes of the mutex itself,
// because an all-zero mutex is one that was never constructed, a destroyed one carries its
// runtime's destroyed pattern, and neither looks like garbage from a stray write.
//
// It is deliberately not survivable. Returning from enter() without the lock would let the
// allocator walk its free lists unsynchronised, and quietly wrong memory is worse than a crash;
// this reports and aborts, which is a bounded stack and a diagnosis instead of a stack overflow.
// The lifetimes that stop the failure from happening in the first place are elsewhere
// (ImmortalCriticalSection in Common/CriticalSection.h, docs/porting/memory-shutdown-order.md).
//
// Full account: docs/porting/allocator-lock-failure.md.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#ifndef _WIN32

#include "Common/CriticalSection.h"
#include "Common/GameMemory.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

namespace
{

// One line's worth. A fixed size on the stack: the point of this file is that reporting cannot
// need the allocator.
const size_t MESSAGE_CAPACITY = 640;

void appendText( char *buffer, size_t &length, const char *text )
{
	if ( text == NULL )
		text = "(null)";
	while ( *text != '\0' && length < MESSAGE_CAPACITY - 1 )
		buffer[ length++ ] = *text++;
}

void appendNumber( char *buffer, size_t &length, unsigned long value, unsigned base )
{
	// Longest case is 64 bits of binary; hex and decimal fit comfortably.
	char digits[ 64 ];
	size_t count = 0;
	do
	{
		const unsigned long digit = value % base;
		digits[ count++ ] = (char)( digit < 10 ? '0' + digit : 'a' + ( digit - 10 ) );
		value /= base;
	}
	while ( value != 0 && count < sizeof( digits ) );

	while ( count > 0 && length < MESSAGE_CAPACITY - 1 )
		buffer[ length++ ] = digits[ --count ];
}

void appendPointer( char *buffer, size_t &length, const void *pointer )
{
	appendText( buffer, length, "0x" );
	appendNumber( buffer, length, (unsigned long)(uintptr_t)pointer, 16 );
}

/**
	The errno spelling, so a crash report reads as a cause rather than as a number. Which of these
	appears is the measurement that decides what went wrong: EINVAL is a mutex whose memory is not a
	live mutex (never initialised, already destroyed, or overwritten), EAGAIN is a recursive count
	that cannot go deeper, EDEADLK and EPERM are lock/unlock mistakes in the calling code, and
	ENOMEM is the resource exhaustion this was originally suspected to be.
*/
const char *errorName( int error )
{
	switch ( error )
	{
		case EINVAL:		return "EINVAL";
		case EAGAIN:		return "EAGAIN";
		case EDEADLK:		return "EDEADLK";
		case EPERM:			return "EPERM";
		case ENOMEM:		return "ENOMEM";
		case EBUSY:			return "EBUSY";
		case ETIMEDOUT:		return "ETIMEDOUT";
#ifdef EOWNERDEAD
		case EOWNERDEAD:	return "EOWNERDEAD";
#endif
#ifdef ENOTRECOVERABLE
		case ENOTRECOVERABLE:	return "ENOTRECOVERABLE";
#endif
		default:			return "unknown errno";
	}
}

/**
	Which of the engine's sections this is, by identity. The five the allocator and the strings take
	are the only ones whose failure can recurse, so naming them is what makes a report actionable;
	anything else is a section the caller owns and is reported by address.
*/
const char *sectionName( const void *section )
{
	if ( section == NULL )								return "null";
	if ( section == TheAsciiStringCriticalSection )		return "TheAsciiStringCriticalSection";
	if ( section == TheUnicodeStringCriticalSection )	return "TheUnicodeStringCriticalSection";
	if ( section == TheDmaCriticalSection )				return "TheDmaCriticalSection";
	if ( section == TheMemoryPoolCriticalSection )		return "TheMemoryPoolCriticalSection";
	if ( section == TheDebugLogCriticalSection )		return "TheDebugLogCriticalSection";
	return "a section the engine does not publish";
}

void writeAll( const char *buffer, size_t length )
{
	size_t written = 0;
	while ( written < length )
	{
		const ssize_t result = write( STDERR_FILENO, buffer + written, length - written );
		if ( result <= 0 )
			return;
		written += (size_t)result;
	}
}

}  // namespace

namespace rts
{

void reportCriticalSectionError( const char *operation, int error, const void *section, const void *mutex )
{
	char buffer[ MESSAGE_CAPACITY ];
	size_t length = 0;

	appendText( buffer, length, "CriticalSection::" );
	appendText( buffer, length, operation );
	appendText( buffer, length, " failed: " );
	appendText( buffer, length, errorName( error ) );
	appendText( buffer, length, " (" );
	appendNumber( buffer, length, (unsigned long)error, 10 );
	appendText( buffer, length, ") on " );
	appendText( buffer, length, sectionName( section ) );
	appendText( buffer, length, " at " );
	appendPointer( buffer, length, section );

	// Up or down tells the two ends of the process apart: a failure with the manager gone is the
	// static-destruction window, a failure with it live is not.
	appendText( buffer, length, ", memory manager " );
	appendText( buffer, length, TheDynamicMemoryAllocator != NULL ? "live" : "gone" );

	appendText( buffer, length, ", pid " );
	appendNumber( buffer, length, (unsigned long)getpid(), 10 );

	// The mutex's own bytes, which say which of the possible causes this was.
	appendText( buffer, length, ", mutex " );
	appendPointer( buffer, length, mutex );
	appendText( buffer, length, " = " );
	if ( mutex != NULL )
	{
		const unsigned char *bytes = (const unsigned char *)mutex;
		const size_t count = sizeof( pthread_mutex_t ) < 24 ? sizeof( pthread_mutex_t ) : 24;
		for ( size_t index = 0; index < count; index++ )
		{
			if ( bytes[ index ] < 0x10 )
				appendText( buffer, length, "0" );
			appendNumber( buffer, length, (unsigned long)bytes[ index ], 16 );
		}
	}
	appendText( buffer, length, "\n" );

	writeAll( buffer, length );
}

void criticalSectionFailure( const char *operation, int error, const void *section, const void *mutex )
{
	reportCriticalSectionError( operation, error, section, mutex );

	static const char *explanation =
		"the allocator cannot report a lock failure by allocating, so this aborts instead of "
		"recursing; see docs/porting/allocator-lock-failure.md\n";
	writeAll( explanation, strlen( explanation ) );

	abort();
}

}  // namespace rts

#endif // !_WIN32
