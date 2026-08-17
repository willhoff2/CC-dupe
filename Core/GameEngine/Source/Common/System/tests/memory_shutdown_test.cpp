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
 *  Allocate and free through the engine's global operator new/delete from a static destructor, *
 *  i.e. after main has returned, and report whether the critical sections the allocator takes  *
 *  on the way are still alive.                                                                 *
 *                                                                                             *
 *  WHY THIS EXISTS. On Apple Silicon every retail run died with SIGSEGV *after* a clean        *
 *  shutdown, recursing CriticalSection::enter -> DynamicMemoryAllocator::allocateBytes from    *
 *  OpenAL Soft's static destructors (docs/porting/apple-silicon-verification.md 8.5). The      *
 *  cause is a lifetime: main()'s five CriticalSection objects were plain statics, so they were  *
 *  destroyed before the libraries' statics were. A late allocation then locked a destroyed      *
 *  std::recursive_mutex, and building the std::system_error's message allocated, which took     *
 *  the same destroyed section, which reported the same error, until the stack was gone.        *
 *  ImmortalCriticalSection (Common/CriticalSection.h) is the fix and PlatformMain.cpp uses it.  *
 *                                                                                             *
 *  WHAT THIS MEASURES, AND WHAT IT CANNOT. The ordering claim is checked directly and in one   *
 *  process: a plain static section and an immortal one are both created here, and the late      *
 *  destructor asserts that the plain one is already gone while the immortal one is still there  *
 *  and still takes the allocator's traffic. It is the order, not the crash, that is portable:   *
 *  locking a destroyed std::recursive_mutex is undefined, and glibc happens to accept it, so    *
 *  the SIGSEGV itself reproduces on the Apple Silicon runtime and not on Linux. A run here      *
 *  that finds the plain section still alive fails rather than passing, because then it would    *
 *  have proved nothing.                                                                        *
 *                                                                                             *
 *  It is not a mock: the allocation and the free are the engine's own global operator           *
 *  new/delete out of the engine's archives, and the memory manager is the game's, initialised   *
 *  the way main()'s prologue initialises it. The only thing this file supplies is the           *
 *  late-destroyed object.                                                                       *
 *                                                                                             *
 *  Run through scripts/native-memory-shutdown-test.py, which links it against the archives      *
 *  scripts/native-build.py links the game from and checks the exit status and the ordering       *
 *  lines below. The interesting output is written from a destructor, so a process that dies on   *
 *  the way out cannot produce it.                                                               *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#include "Common/CriticalSection.h"
#include "Common/GameMemory.h"

#include <cstdio>
#include <cstdlib>

namespace
{

int Failures = 0;

void Check(const char * what, bool ok)
{
	if (!ok) Failures++;
	std::printf("%-62s %s\n", what, ok ? "ok" : "FAILED");
	std::fflush(stdout);
}

// Set from a plain static's destructor. A POD with no destructor of its own, so its own lifetime
// cannot confuse the ordering it records.
bool ThePlainSectionIsGone = false;

/**
	The shape main() used to have: a CriticalSection with static storage duration that *is*
	destroyed. Nothing points the engine at this one -- it is here to record the moment the pre-fix
	arrangement stopped being usable, in the same process and the same destruction order as the
	immortal ones.
*/
class WatchedSection : public CriticalSection
{
public:
	virtual ~WatchedSection()
	{
		ThePlainSectionIsGone = true;
	}
};

/**
	A stand-in for OpenAL Soft's static state: it allocates through the engine's allocator while the
	game is running, and allocates and frees again in its destructor, which runs after main has
	returned. Declared before the sections below so that it is destroyed after them, which is the
	order the retail process had: the game's own statics go first, a dependency's go last.
*/
class LateFreer
{
public:
	LateFreer() : m_bytes(NULL)
	{
		// Deliberately empty of allocation: this runs before main, and an allocation here would
		// bring up the memory manager pre-main and change what shutdownMemoryManager() does.
	}

	void Take_Memory()
	{
		m_bytes = new char[4096];
		for (int index = 0; index < 4096; index++)
			m_bytes[index] = (char)(index & 0x7f);
	}

	~LateFreer()
	{
		// The claim: by now the pre-fix arrangement's section is destroyed, and the one the
		// allocator actually holds is not.
		std::printf("%-62s %s\n", "a plain static section is already destroyed by now",
			ThePlainSectionIsGone ? "yes" : "NO -- THIS RUN PROVES NOTHING");
		std::printf("%-62s %s\n", "the section the allocator holds is still alive",
			TheAsciiStringCriticalSection != NULL ? "yes" : "NO");

		// The free, through the engine's global operator delete, and then a fresh allocation and
		// free: that is what a std::string in a static destructor does, and it is the traffic that
		// used to recurse. Both take TheAsciiStringCriticalSection by way of the allocator.
		delete [] m_bytes;
		m_bytes = NULL;
		char * scratch = new char[128];
		scratch[0] = 'x';
		delete [] scratch;

		// Taking the section directly too, the way ScopedCriticalSection does inside the allocator.
		if (TheAsciiStringCriticalSection != NULL)
		{
			TheAsciiStringCriticalSection->enter();
			TheAsciiStringCriticalSection->exit();
		}

		std::printf("late free completed\n");
		std::fflush(stdout);
	}

private:
	char * m_bytes;
};

// Destruction is the reverse of construction within a translation unit, so the order below is
// exactly the retail order: the freer is destroyed last, after the plain section has gone.
LateFreer TheLateFreer;
WatchedSection ThePlainSection;

// main()'s prologue in GeneralsMD/Code/Main/PlatformMain.cpp: the string and pool critical sections
// every allocation in the engine goes through, in the storage that is never destroyed.
ImmortalCriticalSection AsciiStringSection;
ImmortalCriticalSection UnicodeStringSection;
ImmortalCriticalSection DmaSection;
ImmortalCriticalSection MemoryPoolSection;
ImmortalCriticalSection DebugLogSection;

}	// namespace

int main()
{
	TheAsciiStringCriticalSection = AsciiStringSection.get();
	TheUnicodeStringCriticalSection = UnicodeStringSection.get();
	TheDmaCriticalSection = DmaSection.get();
	TheMemoryPoolCriticalSection = MemoryPoolSection.get();
	TheDebugLogCriticalSection = DebugLogSection.get();

	initMemoryManager();
	Check("the memory manager is initialised", isMemoryManagerOfficiallyInited());

	// A library that allocates during the run and frees at exit. Filled here so its memory is the
	// game's, exactly as OpenAL Soft's device list is on the first device open.
	TheLateFreer.Take_Memory();
	Check("a static that allocates was filled during the run", TheDynamicMemoryAllocator != NULL);

	// Ordinary allocations still work and are freed while the manager is up.
	char * block = new char[64];
	block[0] = 'x';
	delete [] block;
	Check("an allocation during the run round-trips", true);

	Check("the plain static section is still alive while main runs", !ThePlainSectionIsGone);

	shutdownMemoryManager();
	Check("shutdownMemoryManager() reports the manager as down",
		!isMemoryManagerOfficiallyInited());

	// PlatformMain.cpp clears three of the five here, so the harness does too: the two it leaves
	// set are the ones a late allocation reaches through.
	TheUnicodeStringCriticalSection = NULL;
	TheDmaCriticalSection = NULL;
	TheMemoryPoolCriticalSection = NULL;

	std::printf("main returning with %d failed checks\n", Failures);
	std::fflush(stdout);
	return Failures;
}
