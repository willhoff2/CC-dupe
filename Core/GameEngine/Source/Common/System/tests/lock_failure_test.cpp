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

/***********************************************************************************************
 *                                                                                             *
 *  What the engine's allocator does when the critical section it takes refuses to lock.        *
 *                                                                                             *
 *  WHY THIS EXISTS. Nine Apple Silicon crash reports and every rendering run's exit share one  *
 *  shape: an allocation takes a CriticalSection, the lock fails, reporting the failure          *
 *  allocates, the allocation takes the same section, and the stack guard page ends the process  *
 *  (docs/porting/real-input-menu-drive.md 4.3, docs/porting/apple-silicon-verification.md 6     *
 *  and 8.5). The failure is intermittent on that hardware; this file makes it happen on         *
 *  demand, so the fix has a failing reproduction rather than a plausible story.                 *
 *                                                                                             *
 *  Each case is a separate process, selected by the LOCK_FAILURE_CASE environment variable      *
 *  rather than by argv, because two of them have to act before main runs. Run them through      *
 *  scripts/native-lock-failure-test.py, which builds this file twice -- once as it ships and    *
 *  once with -DLOCK_FAILURE_NEGATIVE_CONTROL, which restores the allocating failure path the    *
 *  standard library had -- and requires the fixed binary to abort with one diagnostic where     *
 *  the control binary dies of the recursion.                                                   *
 *                                                                                             *
 *  THE CASES.                                                                                  *
 *    platform         what this platform's recursive pthread mutex does in each of the states   *
 *                     a mutex can be in: live, destroyed, never initialised, overwritten. This  *
 *                     is the measurement that says which of those the crash reports can be,     *
 *                     and it is also the control for the two destroyed-section cases: if a      *
 *                     destroyed mutex locks happily here, those cases prove nothing.            *
 *    premain          whether a genuine pre-main allocation can take a critical section at all. *
 *    frames           whether the preMainInitMemoryManager() frame in the crash reports dates   *
 *                     the crash to before main.                                                *
 *    destroyed-dma    the allocator, mid-run, with a destroyed section: the failure itself.     *
 *    exit-destroyed   the same failure reached the way the retail process reached it, through   *
 *                     static destruction order, with no pointer surgery at all.                 *
 *    exit-immortal    the same arrangement with the immortal sections #113 introduced: the       *
 *                     lifetime fix, which is what stops the failure from happening.             *
 *                                                                                             *
 *  It is not a mock. The allocator, the global operator new/delete and the critical sections     *
 *  are the engine's own, out of the archives scripts/native-build.py produced; the only thing    *
 *  this file adds is a section whose lifetime has ended and something that allocates late.       *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#include "Common/CriticalSection.h"
#include "Common/GameMemory.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <cstdio>
#include <new>

#ifdef LOCK_FAILURE_NEGATIVE_CONTROL
#include <system_error>
#endif

namespace
{

int Failures = 0;

void Report(const char * what, const char * answer)
{
	std::printf("%-62s %s\n", what, answer);
	std::fflush(stdout);
}

void Check(const char * what, bool ok)
{
	if (!ok) Failures++;
	Report(what, ok ? "yes" : "NO");
}

const char * Case_Name()
{
	const char * name = getenv("LOCK_FAILURE_CASE");
	return name != NULL ? name : "";
}

bool Case_Is(const char * name)
{
	return strcmp(Case_Name(), name) == 0;
}

/**
	errno as a name. Only the ones a mutex operation can produce.
*/
const char * Error_Name(int error)
{
	switch (error)
	{
		case 0:			return "0 (locked)";
		case EINVAL:	return "EINVAL";
		case EAGAIN:	return "EAGAIN";
		case EDEADLK:	return "EDEADLK";
		case EPERM:		return "EPERM";
		case ENOMEM:	return "ENOMEM";
		case EBUSY:		return "EBUSY";
		default:		return "some other errno";
	}
}

// ----------------------------------------------------------------- the platform's own answers

/**
	Lock a recursive pthread mutex that has been put into one of the states a crash report cannot
	tell apart, and return what the platform says. -> errno, or 0 when the lock was granted.

	`state` is applied to real mutex storage: nothing here inspects or forges the runtime's internal
	fields, so the answers are this platform's, not this file's.
*/
enum MutexState { MUTEX_LIVE, MUTEX_DESTROYED, MUTEX_NEVER_INITIALISED, MUTEX_OVERWRITTEN };

int Lock_In_State(MutexState state)
{
	pthread_mutex_t mutex;

	if (state == MUTEX_NEVER_INITIALISED)
	{
		// The static-initialisation-order case: storage that a constructor has not reached yet is
		// zero, because it is in bss.
		memset(&mutex, 0, sizeof(mutex));
		return pthread_mutex_lock(&mutex);
	}

	pthread_mutexattr_t attributes;
	pthread_mutexattr_init(&attributes);
	pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&mutex, &attributes);
	pthread_mutexattr_destroy(&attributes);

	if (state == MUTEX_DESTROYED)
	{
		// The __cxa_finalize case: a mutex whose destructor has run.
		pthread_mutex_destroy(&mutex);
		return pthread_mutex_lock(&mutex);
	}

	if (state == MUTEX_OVERWRITTEN)
	{
		// The stray-write case: live storage scribbled over by something else.
		memset(&mutex, 0xff, sizeof(mutex));
		return pthread_mutex_lock(&mutex);
	}

	const int result = pthread_mutex_lock(&mutex);
	if (result == 0)
		pthread_mutex_unlock(&mutex);
	pthread_mutex_destroy(&mutex);
	return result;
}

void Measure_The_Platform()
{
	const int live = Lock_In_State(MUTEX_LIVE);
	const int destroyed = Lock_In_State(MUTEX_DESTROYED);
	const int uninitialised = Lock_In_State(MUTEX_NEVER_INITIALISED);
	const int overwritten = Lock_In_State(MUTEX_OVERWRITTEN);

	Report("a live recursive mutex locks", Error_Name(live));
	Report("a destroyed one reports", Error_Name(destroyed));
	Report("a never-initialised (zeroed) one reports", Error_Name(uninitialised));
	Report("an overwritten one reports", Error_Name(overwritten));

	Check("a live mutex locks, so this measurement is of a working runtime", live == 0);
	// The control for the two destroyed-section cases below. A platform that grants the lock on a
	// destroyed mutex cannot show the crash at all, and saying so is the point: locking a destroyed
	// mutex is undefined, and the answer differs between C++ runtimes.
	Check("a destroyed mutex refuses to lock, so the cases below can fail",
		destroyed != 0);
}

// ------------------------------------------------------------- what happens before main runs

/**
	One genuine allocation before main, and what the engine's globals looked like around it.

	The crash reports show CriticalSection::enter() underneath operator new and
	preMainInitMemoryManager() at the top, and were read as a pre-main failure. Whether an
	allocation before main can enter a critical section at all is a question with a measurable
	answer, and this is it: the five section pointers are set by main's prologue, and
	ScopedCriticalSection does nothing with a null one.
*/
class PreMainProbe
{
public:
	PreMainProbe() : m_bytes(NULL), m_ran(false), m_sectionBefore(NULL), m_sectionAfter(NULL),
		m_allocatorBefore(NULL), m_allocatorAfter(NULL)
	{
		// Only in the case that asks for it: an allocation before main sends
		// shutdownMemoryManager() down its pre-main path and would change what the other cases
		// measure (docs/porting/memory-shutdown-order.md).
		if (!Case_Is("premain"))
			return;

		m_ran = true;
		m_sectionBefore = TheDmaCriticalSection;
		m_allocatorBefore = TheDynamicMemoryAllocator;
		m_bytes = new char[64];
		m_bytes[0] = 'x';
		m_sectionAfter = TheDmaCriticalSection;
		m_allocatorAfter = TheDynamicMemoryAllocator;
	}

	void Report_It()
	{
		Check("an allocation before main ran", m_ran && m_bytes != NULL);
		Check("no allocator existed before it", m_allocatorBefore == NULL);
		Check("it brought the memory manager up by itself", m_allocatorAfter != NULL);
		Check("TheDmaCriticalSection was null before it", m_sectionBefore == NULL);
		Check("TheDmaCriticalSection was still null after it", m_sectionAfter == NULL);
		Report("so a pre-main allocation can enter a critical section",
			(m_sectionBefore == NULL && m_sectionAfter == NULL) ? "no" : "YES");
		delete [] m_bytes;
		m_bytes = NULL;
	}

private:
	char * m_bytes;
	bool m_ran;
	CriticalSection * m_sectionBefore;
	CriticalSection * m_sectionAfter;
	DynamicMemoryAllocator * m_allocatorBefore;
	DynamicMemoryAllocator * m_allocatorAfter;
};

// --------------------------------------------------------------- a section past its lifetime

/**
	A CriticalSection whose destructor has run, in storage that outlives it.

	This is not a stand-in for the crash's section: it is a CriticalSection, constructed and
	destroyed by the engine's own header, in memory that is still readable afterwards -- which is
	exactly the state a file-scope static is in once static destruction has passed it.
*/
class ExpiredSection
{
public:
	ExpiredSection() : m_section(new (m_storage) CriticalSection())
	{
		m_section->~CriticalSection();
	}

	CriticalSection * get() { return m_section; }

private:
	alignas(CriticalSection) unsigned char m_storage[sizeof(CriticalSection)];
	CriticalSection * m_section;
};

/**
	A library's static state: it takes memory from the engine during the run and gives it back from
	its destructor, after main has returned. OpenAL Soft's device list and WW3D2's
	SimpleVecClass<Vector4> both do this; it is the traffic the exit crash arrived on.
*/
class LateAllocator
{
public:
	LateAllocator() : m_bytes(NULL) {}

	void Take_Memory()
	{
		m_bytes = new char[4096];
		m_bytes[0] = 'x';
	}

	~LateAllocator()
	{
		if (m_bytes == NULL)
			return;

		// Before the free, so a process that dies inside it has still said where it was.
		std::printf("the late destructor is about to free through the engine's allocator\n");
		std::fflush(stdout);

		delete [] m_bytes;
		m_bytes = NULL;

		char * scratch = new char[128];
		scratch[0] = 'x';
		delete [] scratch;

		std::printf("late free completed\n");
		std::fflush(stdout);
	}

private:
	char * m_bytes;
};

// Destruction is the reverse of construction inside a translation unit, so this is the retail
// order: the game's own sections go first and a dependency's statics go last.
LateAllocator TheLateAllocator;
CriticalSection ThePlainDmaSection;

// main()'s prologue in GeneralsMD/Code/Main/PlatformMain.cpp.
ImmortalCriticalSection AsciiStringSection;
ImmortalCriticalSection UnicodeStringSection;
ImmortalCriticalSection DmaSection;
ImmortalCriticalSection MemoryPoolSection;
ImmortalCriticalSection DebugLogSection;

PreMainProbe ThePreMainProbe;

void Engine_Prologue(CriticalSection * dmaSection)
{
	TheAsciiStringCriticalSection = AsciiStringSection.get();
	TheUnicodeStringCriticalSection = UnicodeStringSection.get();
	TheDmaCriticalSection = dmaSection;
	TheMemoryPoolCriticalSection = MemoryPoolSection.get();
	TheDebugLogCriticalSection = DebugLogSection.get();
	initMemoryManager();
}

}	// namespace

// ------------------------------------------------------------------- the negative control

#ifdef LOCK_FAILURE_NEGATIVE_CONTROL
/**
	The failure path as it was before this slice: the standard library's.

	std::recursive_mutex::lock() reports a failure by calling __throw_system_error, which builds a
	std::system_error whose message is a std::string -- and the engine's operator new is where that
	string's memory comes from. This is that path, spelled out, and it is linked in place of
	Common/System/CriticalSectionFailure.cpp (the runner removes that member from its copy of the
	archive) so that the only difference between the two binaries is the fix itself.

	The depth counter is written with write(2) and hand-formatted for the same reason the real
	reporter is: a printf here would allocate and take part in the recursion it is measuring.
*/
namespace
{
unsigned long TheControlDepth = 0;

void Write_Depth(unsigned long depth)
{
	char buffer[64];
	size_t length = 0;
	const char * prefix = "control: recursion depth ";
	while (*prefix != '\0')
		buffer[length++] = *prefix++;
	char digits[24];
	size_t count = 0;
	unsigned long value = depth;
	do
	{
		digits[count++] = (char)('0' + (value % 10));
		value /= 10;
	}
	while (value != 0);
	while (count > 0)
		buffer[length++] = digits[--count];
	buffer[length++] = '\n';
	ssize_t ignored = write(STDERR_FILENO, buffer, length);
	(void)ignored;
}
}	// namespace

namespace rts
{

void reportCriticalSectionError(const char *, int, const void *, const void *)
{
	// Unused in the control: the pre-fix code had nothing like it.
}

void criticalSectionFailure(const char * operation, int error, const void *, const void *)
{
	TheControlDepth++;
	if (TheControlDepth % 1000 == 0)
		Write_Depth(TheControlDepth);
	// What libc++ does: throw, with a message that has to be allocated.
	(void)operation;
	throw std::system_error(std::error_code(error, std::generic_category()),
		"recursive_mutex lock failed");
}

}	// namespace rts
#endif // LOCK_FAILURE_NEGATIVE_CONTROL

// ------------------------------------------------------------------------------------ cases

int main()
{
#ifdef LOCK_FAILURE_NEGATIVE_CONTROL
	std::printf("binary: negative control (the allocating failure path, as it was)\n");
#else
	std::printf("binary: as it ships (the non-allocating failure path)\n");
#endif
	std::printf("case: %s\n", Case_Name());
	std::fflush(stdout);

	if (Case_Is("platform"))
	{
		Measure_The_Platform();
		std::printf("main returning with %d failed checks\n", Failures);
		return Failures;
	}

	if (Case_Is("premain"))
	{
		ThePreMainProbe.Report_It();
		std::printf("main returning with %d failed checks\n", Failures);
		return Failures;
	}

	if (Case_Is("frames"))
	{
		// Where the preMainInitMemoryManager() frame in the crash reports comes from. operator new
		// in GameMemory.cpp calls it before every single allocation -- all thirteen global
		// allocation entry points do -- so the frame is present in allocations at any age of the
		// process and dates nothing. What this case measures is the state that call sees.
		const bool allocatorBeforeMain = TheDynamicMemoryAllocator != NULL;
		Report("an engine static already allocated before main",
			allocatorBeforeMain ? "yes" : "no");

		Engine_Prologue(DmaSection.get());
		Check("the memory manager is up during the run", TheDynamicMemoryAllocator != NULL);

		shutdownMemoryManager();
		// Not a check: which way this goes is a property of the link, not of the fix. An engine
		// static that allocates before main puts the manager on its pre-main path, and
		// shutdownMemoryManager() then deliberately does nothing -- which is also why the retail
		// process still has a live allocator while its static destructors run.
		Report("shutdownMemoryManager() tore the manager down",
			TheDynamicMemoryAllocator == NULL ? "yes" : "no (it was inited before main)");

		char * block = new char[64];
		block[0] = 'x';
		Check("an allocation after shutdownMemoryManager() is still served", true);
		Check("the allocator serving it is the engine's", TheDynamicMemoryAllocator != NULL);
		delete [] block;

		std::printf("main returning with %d failed checks\n", Failures);
		return Failures;
	}

	if (Case_Is("destroyed-dma"))
	{
		Engine_Prologue(DmaSection.get());
		Check("the memory manager is up", TheDynamicMemoryAllocator != NULL);

		char * warmup = new char[64];
		warmup[0] = 'x';
		delete [] warmup;
		Check("an allocation through a live section round-trips", true);

		// The lifetime the pre-#113 entry point had at exit, brought forward to where it can be
		// watched: the pointer the allocator uses names a section whose destructor has run.
		static ExpiredSection expired;
		TheDmaCriticalSection = expired.get();

		std::printf("about to allocate with a destroyed TheDmaCriticalSection\n");
		std::fflush(stdout);

		char * doomed = new char[64];
		doomed[0] = 'x';

		// Only reached if entering a destroyed section succeeded, which the platform case says
		// whether to believe.
		std::printf("the allocation returned; this platform accepts a destroyed section\n");
		// Back to a live section: what the static destructors do on the way out is not this case's
		// business.
		TheDmaCriticalSection = DmaSection.get();
		delete [] doomed;
		std::fflush(stdout);
		return 0;
	}

	if (Case_Is("exit-destroyed"))
	{
		// No pointer surgery: the section the allocator takes is a plain file-scope static, the way
		// PlatformMain.cpp's five were before #113, and the static that allocates late is destroyed
		// after it.
		Engine_Prologue(&ThePlainDmaSection);
		Check("the memory manager is up", TheDynamicMemoryAllocator != NULL);
		TheLateAllocator.Take_Memory();
		Check("a static took memory during the run", true);

		std::printf("main returning; the sections are destroyed next, the late free after that\n");
		std::fflush(stdout);
		return Failures;
	}

	if (Case_Is("exit-immortal"))
	{
		// The same arrangement with the lifetime fix: the section outlives static destruction.
		Engine_Prologue(DmaSection.get());
		Check("the memory manager is up", TheDynamicMemoryAllocator != NULL);
		TheLateAllocator.Take_Memory();
		Check("a static took memory during the run", true);

		std::printf("main returning; the late free happens after this\n");
		std::fflush(stdout);
		return Failures;
	}

	std::printf("no case selected: set LOCK_FAILURE_CASE (platform, premain, frames, "
		"destroyed-dma, exit-destroyed, exit-immortal)\n");
	return 2;
}
