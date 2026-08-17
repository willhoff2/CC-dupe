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
// calling releaseBuffer() while holding the string critical section, so std::mutex would deadlock
// on the first string assignment and std::recursive_mutex is the only correct stand-in.
// The storage is a member rather than a handle on purpose: the five critical sections below are
// objects with static storage duration, constructed before initMemoryManager() runs, and the
// engine replaces global operator new with an allocator that is not usable until then. That is
// also why WWLib/platform/platform_mutex.h is not reused here - it creates its recursive mutex
// with new. See docs/porting/timing-and-threading.md.
#ifdef _WIN32
#include <windows.h>
#else
#include <mutex>
#endif

#include "Common/PerfTimer.h"

#ifndef _WIN32
#include <new>
#endif

#ifdef PERF_TIMERS
extern PerfGather TheCritSecPerfGather;
#endif

class CriticalSection
{
#ifdef _WIN32
	CRITICAL_SECTION m_windowsCriticalSection;
#else
	std::recursive_mutex m_recursiveMutex;
#endif

	public:
		CriticalSection()
		{
			#ifdef PERF_TIMERS
			AutoPerfGather a(TheCritSecPerfGather);
			#endif
			#ifdef _WIN32
			InitializeCriticalSection( &m_windowsCriticalSection );
			#endif
		}

		virtual ~CriticalSection()
		{
			#ifdef PERF_TIMERS
			AutoPerfGather a(TheCritSecPerfGather);
			#endif
			#ifdef _WIN32
			DeleteCriticalSection( &m_windowsCriticalSection );
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
			m_recursiveMutex.lock();
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
			m_recursiveMutex.unlock();
			#endif
		}
};

#ifndef _WIN32
// TheSuperHackers @bugfix Devin 17/08/2026 A CriticalSection with static storage duration that is
// never destroyed. Not compiled on Windows: VC6 has neither alignas nor this problem.
//
// Off Windows the five critical sections below are entered by the allocator itself, and static
// destructors keep allocating and freeing after main has returned: a library's static destructor
// builds a std::string, that reaches the engine's operator new, and the allocator takes
// TheDmaCriticalSection on the way. Plain statics are already destroyed by then. Locking a
// destroyed std::recursive_mutex reports an error, std::system_error's message allocates, that
// allocation takes the same dead section, and the process recurses until the stack is gone -- the
// SIGSEGV that ended every clean Apple Silicon shutdown
// (docs/porting/apple-silicon-verification.md 8.5, docs/porting/memory-shutdown-order.md).
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
