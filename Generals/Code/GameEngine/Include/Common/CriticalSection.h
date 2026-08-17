/*
**	Command & Conquer Generals(tm)
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

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////

// CriticalSection.h ///////////////////////////////////////////////////////
// Utility class to use critical sections in areas of code.
// Author: JohnM And MattC, August 13, 2002

#pragma once

// TheSuperHackers @port The Win32 CRITICAL_SECTION is recursive: the same thread may enter it
// again while it holds it. AsciiString::set() and UnicodeString::set() both do exactly that, by
// calling releaseBuffer() while holding the string critical section, so a non-recursive mutex
// would deadlock on the first string assignment and only a recursive one will do.
// The storage is a member rather than a handle on purpose: the five critical sections below are
// objects with static storage duration, constructed before initMemoryManager() runs, and the
// engine replaces global operator new with an allocator that is not usable until then. That is
// also why WWLib/platform/platform_mutex.h is not reused here - it creates its recursive mutex
// with new. See docs/porting/timing-and-threading.md.
//
// TheSuperHackers @bugfix Devin 17/08/2026 The recursive mutex is a pthread_mutex_t rather than a
// std::recursive_mutex, and a failed lock is reported without allocating. std::recursive_mutex
// reports a failure by throwing std::system_error, whose message is a std::string, whose
// allocation is the engine's operator new, which takes this very section again: on Apple Silicon a
// single recoverable mutex error therefore became an unbounded recursion and a stack overflow, at
// both ends of the process lifetime (docs/porting/allocator-lock-failure.md,
// docs/porting/real-input-menu-drive.md 4.3, docs/porting/apple-silicon-verification.md 8.5).
// Windows keeps CRITICAL_SECTION, which does not report failures at all.
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

#include "Common/PerfTimer.h"

#ifndef _WIN32
#include <new>
#endif

#ifdef PERF_TIMERS
extern PerfGather TheCritSecPerfGather;
#endif

#ifndef _WIN32
namespace rts
{
	// Both are defined in Common/System/CriticalSectionFailure.cpp and neither allocates: they
	// format into a stack buffer and write() it to stderr. They are out of line so that the lock
	// path stays a compare and a branch, and so that a test can link a different definition to
	// restore the pre-fix behaviour as a negative control
	// (scripts/native-lock-failure-test.py).
	void reportCriticalSectionError( const char *operation, int error, const void *section, const void *mutex );
	[[noreturn]] void criticalSectionFailure( const char *operation, int error, const void *section, const void *mutex );
}
#endif

class CriticalSection
{
#ifdef _WIN32
	CRITICAL_SECTION m_windowsCriticalSection;
#else
	pthread_mutex_t m_recursiveMutex;
#endif

	public:
		CriticalSection()
		{
			#ifdef PERF_TIMERS
			AutoPerfGather a(TheCritSecPerfGather);
			#endif
			#ifdef _WIN32
			InitializeCriticalSection( &m_windowsCriticalSection );
			#else
			// Recursive by attribute, and no allocation: this runs before initMemoryManager().
			// The attribute call is checked too: a mutex that quietly came out non-recursive would
			// not fail here, it would deadlock or report EDEADLK inside AsciiString::set().
			pthread_mutexattr_t attributes;
			int result = pthread_mutexattr_init( &attributes );
			if ( result == 0 )
			{
				result = pthread_mutexattr_settype( &attributes, PTHREAD_MUTEX_RECURSIVE );
				if ( result == 0 )
					result = pthread_mutex_init( &m_recursiveMutex, &attributes );
				pthread_mutexattr_destroy( &attributes );
			}
			if ( result != 0 )
				rts::criticalSectionFailure( "init", result, this, &m_recursiveMutex );
			#endif
		}

		virtual ~CriticalSection()
		{
			#ifdef PERF_TIMERS
			AutoPerfGather a(TheCritSecPerfGather);
			#endif
			#ifdef _WIN32
			DeleteCriticalSection( &m_windowsCriticalSection );
			#else
			// Reported but not fatal: a destructor that aborts would turn a leftover holder into a
			// crash on the way out, which is the failure mode this file exists to remove.
			const int result = pthread_mutex_destroy( &m_recursiveMutex );
			if ( result != 0 )
				rts::reportCriticalSectionError( "destroy", result, this, &m_recursiveMutex );
			#endif
		}

	public:	// Use these when entering/exiting a critical section.
		void enter()
		{
			#ifdef PERF_TIMERS
			AutoPerfGather a(TheCritSecPerfGather);
			#endif
			#ifdef _WIN32
			EnterCriticalSection( &m_windowsCriticalSection );
			#else
			const int result = pthread_mutex_lock( &m_recursiveMutex );
			if ( result != 0 )
			{
				// Not survivable as a no-op: returning without the lock would leave the caller --
				// the allocator -- running unsynchronised over its free lists, which is worse than
				// the crash. This says which section failed and how, and stops.
				rts::criticalSectionFailure( "lock", result, this, &m_recursiveMutex );
			}
			#endif
		}

		void exit()
		{
			#ifdef PERF_TIMERS
			AutoPerfGather a(TheCritSecPerfGather);
			#endif
			#ifdef _WIN32
			LeaveCriticalSection( &m_windowsCriticalSection );
			#else
			const int result = pthread_mutex_unlock( &m_recursiveMutex );
			if ( result != 0 )
				rts::criticalSectionFailure( "unlock", result, this, &m_recursiveMutex );
			#endif
		}

#ifndef _WIN32
	private:
		// pthread_mutex_t is a plain struct, so unlike the std::recursive_mutex this replaced it
		// would copy silently. Nothing copies a CriticalSection; keep it that way.
		CriticalSection( const CriticalSection & );
		CriticalSection &operator=( const CriticalSection & );
#endif
};

#ifndef _WIN32
// TheSuperHackers @bugfix Devin 17/08/2026 A CriticalSection with static storage duration that is
// never destroyed. Not compiled on Windows: VC6 has neither alignas nor this problem.
//
// Off Windows the five critical sections below are entered by the allocator itself, and static
// destructors keep allocating and freeing after main has returned: a library's static destructor
// builds a std::string, that reaches the engine's operator new, and the allocator takes
// TheDmaCriticalSection on the way. Plain statics are already destroyed by then. Locking a
// destroyed mutex reports EINVAL, and before the lock path was made allocation-free that error
// recursed until the stack was gone -- the SIGSEGV that ended every clean Apple Silicon shutdown
// (docs/porting/apple-silicon-verification.md 8.5, docs/porting/memory-shutdown-order.md,
// docs/porting/allocator-lock-failure.md). It now aborts with a diagnostic instead, which is still
// not a working process: the lifetime below is what keeps the sections usable.
//
// The lifetime is the fix: the section is constructed in this object's own storage and its
// destructor is never run, so the pointers the allocator holds stay valid for as long as any
// destructor can allocate. Nothing leaks that the process does not release on exit anyway, and
// Windows is untouched: WinMain.cpp keeps its plain statics and its DeleteCriticalSection.
class ImmortalCriticalSection
{
	public:
		ImmortalCriticalSection() : m_section(new (m_storage) CriticalSection())
		{
		}

		// No destructor on purpose. Declaring one, even an empty one, would register this object
		// for destruction and defeat the point.

		CriticalSection *get() { return m_section; }

	private:
		ImmortalCriticalSection(const ImmortalCriticalSection &);
		ImmortalCriticalSection &operator=(const ImmortalCriticalSection &);

		alignas(CriticalSection) unsigned char m_storage[sizeof(CriticalSection)];
		CriticalSection *m_section;
};
#endif // !_WIN32

class ScopedCriticalSection
{
	private:
		CriticalSection *m_cs;

	public:
		ScopedCriticalSection( CriticalSection *cs ) : m_cs(cs)
		{
			if (m_cs)
				m_cs->enter();
		}

		virtual ~ScopedCriticalSection()
		{
			if (m_cs)
				m_cs->exit();
		}
};

// These should be null on creation then non-null in WinMain or equivalent.
// This allows us to be silently non-threadsafe for WB and other single-threaded apps.
extern CriticalSection *TheAsciiStringCriticalSection;
extern CriticalSection *TheUnicodeStringCriticalSection;
extern CriticalSection *TheDmaCriticalSection;
extern CriticalSection *TheMemoryPoolCriticalSection;
extern CriticalSection *TheDebugLogCriticalSection;
