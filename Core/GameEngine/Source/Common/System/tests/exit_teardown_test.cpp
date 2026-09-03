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
 *  Take objects out of the real WWLib object pool from the quit crash report and let exit()    *
 *  destroy it, the way quitting the game does.                                                *
 *                                                                                             *
 *  WHY THIS EXISTS. Every quit on Apple Silicon wrote a crash report faulting in              *
 *  ObjectPoolClass<MultiListNodeClass,256>::~ObjectPoolClass() under __cxa_finalize_ranges <-  *
 *  exit, at 0x7ab5804c00000000 -- a pointer whose low half is zero                            *
 *  (docs/porting/playability-probe.md 7). That is the block-list walk in the destructor, and   *
 *  the reason the pointer is half-overwritten is arithmetic in Allocate_Object_Memory():       *
 *  the block's first bytes hold the next-block pointer, but the objects used to start at       *
 *  (uint32 *)block + 1, which is four bytes in, not a pointer in. On Win32 those are the same  *
 *  address; on LP64 the first object overlaps the top half of the next-block pointer, and the  *
 *  destructor walks what the object wrote there. It is a PORT DEFECT, not a destruction order  *
 *  problem: the pool is self-contained and the corruption happens during play, at the first    *
 *  write to the first object of every block.                                                  *
 *                                                                                             *
 *  WHAT THIS MEASURES.                                                                        *
 *  1. The retail pool: 300 MultiListNodeClass (two blocks of 256) allocated and freed through  *
 *     AutoPoolClass::operator new/delete, so its file-scope pool still owns both blocks when   *
 *     exit() destroys it. That destruction is the crash. The reporter below is constructed     *
 *     first and therefore destroyed last, so it only prints if the process survived it.        *
 *  2. The block list itself, read out of a pool in this file: the chain has to be walkable and *
 *     terminated, and every object handed out has to be pointer-aligned and outside the        *
 *     header.                                                                                 *
 *  3. THE CONTROL: PreFixPoolClass below is the pre-fix template, arithmetic verbatim. It has  *
 *     to show the next-block pointer overwritten, without dereferencing it. Without the        *
 *     control a clean run proves nothing, because a 32-bit build cannot have the defect at all *
 *     -- which is why Windows never crashed here.                                              *
 *                                                                                             *
 *  Run through scripts/native-exit-teardown-test.py, which links this against the archives     *
 *  scripts/native-build.py links the game from. See docs/porting/memory-shutdown-order.md.     *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#include "Common/CriticalSection.h"
#include "Common/GameMemory.h"

#include "mempool.h"
#include "multilist.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{

int Failures = 0;

void Check(const char * what, bool ok)
{
	if (!ok) Failures++;
	std::printf("%-62s %s\n", what, ok ? "ok" : "FAILED");
	std::fflush(stdout);
}

// More than one block of 256, so every pool here owns two blocks and its destructor has a chain
// to walk rather than a single entry.
const int NODE_COUNT = 300;

/**
	The shape of the objects these pools carry: pointers, written the moment the object is
	constructed, which is what lands on top of the block header when the arithmetic is wrong.
*/
class HarnessNode
{
public:
	HarnessNode() { Next = NULL; Object = NULL; }
	HarnessNode * Next;
	void * Object;
};

/**
	The shipping pool with its block list exposed. BlockListHead is protected, so reading it needs
	a subclass rather than a reinterpret_cast of the layout.
*/
class ProbePoolClass : public ObjectPoolClass<HarnessNode, 256>
{
public:
	uint32 * Block_List_Head() const { return BlockListHead; }
	int Total_Object_Count() const { return TotalObjectCount; }

	/// -> how many blocks the chain has, or -1 if it does not terminate within a sane bound.
	int Block_Count() const
	{
		int count = 0;
		uint32 * block = BlockListHead;
		while (block != NULL) {
			if (++count > 16) return -1;
			block = *(uint32 **)block;
		}
		return count;
	}
};

/**
	The pre-fix pool, for the control case. Everything is the code as it stood, including the
	`(uint32 *)BlockListHead + 1` that this slice fixes; only the frees are dropped, because the
	control must not be a crash to be evidence. Nothing here is used by the game.
*/
template<class T, int BLOCK_SIZE>
class PreFixPoolClass
{
public:
	PreFixPoolClass() : FreeListHead(NULL), BlockListHead(NULL) {}

	T * Allocate_Object_Memory()
	{
		if (FreeListHead == NULL) {
			uint32 * tmp_block_head = BlockListHead;
			BlockListHead = (uint32*)::operator new( sizeof(T) * BLOCK_SIZE + sizeof(uint32 *));
			*(void **)BlockListHead = tmp_block_head;

			FreeListHead = (T*)(BlockListHead + 1);				// the defect, verbatim
			for ( int i = 0; i < BLOCK_SIZE; i++ ) {
				*(T**)(&(FreeListHead[i])) = &(FreeListHead[i+1]);
			}
			*(T**)(&(FreeListHead[BLOCK_SIZE-1])) = NULL;
		}

		T * obj = FreeListHead;
		FreeListHead = *(T**)(FreeListHead);
		return obj;
	}

	uint32 * Block_List_Head() const { return BlockListHead; }

private:
	T * FreeListHead;
	uint32 * BlockListHead;
};

/**
	Destroyed after the pools below it, and the only thing that can report on them: if the retail
	pool's destructor faults, this never runs and the runner sees no completion marker.
*/
class ExitReporter
{
public:
	~ExitReporter()
	{
		// What OpenAL Soft's and the C++ runtime's statics do on the way out, to show the process
		// is still functional this late and not merely un-crashed.
		char * scratch = new char[512];
		std::memset(scratch, 'x', 512);
		const bool usable = scratch[0] == 'x' && scratch[511] == 'x';
		delete [] scratch;
		std::printf("%-62s %s\n", "an allocation from a static destructor is usable",
			usable ? "yes" : "NO");

		std::printf("exit teardown completed\n");
		std::fflush(stdout);
	}
};

// Reverse of construction order: the reporter is destroyed last, after both pools in this file.
ExitReporter TheReporter;
ProbePoolClass TheHarnessPool;
PreFixPoolClass<HarnessNode, 256> ThePreFixPool;

// main()'s prologue in GeneralsMD/Code/Main/PlatformMain.cpp, in storage that is never destroyed.
ImmortalCriticalSection AsciiStringSection;
ImmortalCriticalSection UnicodeStringSection;
ImmortalCriticalSection DmaSection;
ImmortalCriticalSection MemoryPoolSection;
ImmortalCriticalSection DebugLogSection;

/**
	-> the case to run. The control exercises the pre-fix arithmetic; the shipping case does not
	touch it at all.
*/
bool is_control_case()
{
	const char * name = std::getenv("EXIT_TEARDOWN_CASE");
	return name != NULL && std::strcmp(name, "control") == 0;
}

/// Fill an object the way a constructor would: this is the write that lands on the header.
void write_object(HarnessNode * node, HarnessNode * next)
{
	node->Next = next;
	node->Object = node;
}

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

	if (is_control_case())
	{
		// The pre-fix arithmetic, and the only thing the control asserts: the block's next-block
		// pointer no longer holds what was stored in it, because the first object is on top of it.
		// Read, never dereferenced -- dereferencing it is the crash report.
		HarnessNode * first = ThePreFixPool.Allocate_Object_Memory();
		uint32 * block = ThePreFixPool.Block_List_Head();
		const void * before = *(void **)block;
		write_object(first, NULL);
		const void * after = *(void **)block;

		std::printf("control: the first block's next pointer was %p, and is %p after one object\n",
			before, after);
		Check("the pre-fix arithmetic overwrites the block's next-block pointer", before != after);
		Check("the first object overlaps the block header",
			(char *)first < (char *)block + sizeof(uint32 *));
		std::printf("main returning with %d failed checks\n", Failures);
		std::fflush(stdout);
		return Failures == 0 ? 0 : 1;		// the control passes by finding the defect
	}

	// The real pool from the crash report. AutoPoolClass::operator new/delete run through its
	// file-scope ObjectPoolClass, so it owns two blocks of 256 by the end of this loop and still
	// owns them when exit() destroys it. Every node is written to, which is what corrupted the
	// block header before the fix.
	MultiListNodeClass * nodes[NODE_COUNT];
	for (int index = 0; index < NODE_COUNT; index++)
	{
		nodes[index] = new MultiListNodeClass;
		nodes[index]->Object = (MultiListObjectClass *)nodes[index];
	}
	for (int index = 0; index < NODE_COUNT; index++)
		delete nodes[index];
	Check("the retail MultiListNodeClass pool served the run", true);

	// The same template, in this file, with its block list readable.
	HarnessNode * harness[NODE_COUNT];
	for (int index = 0; index < NODE_COUNT; index++)
	{
		harness[index] = TheHarnessPool.Allocate_Object_Memory();
		write_object(harness[index], index > 0 ? harness[index - 1] : NULL);
	}

	uint32 * block = TheHarnessPool.Block_List_Head();
	Check("the objects start a pointer into the block, clear of the header",
		(char *)harness[NODE_COUNT - 1] >= (char *)block + sizeof(uint32 *));
	Check("the objects are pointer-aligned",
		((uintptr_t)harness[0] % sizeof(void *)) == 0);
	Check("the block list is walkable and terminated after the objects were written",
		TheHarnessPool.Block_Count() == TheHarnessPool.Total_Object_Count() / 256);

	for (int index = 0; index < NODE_COUNT; index++)
		TheHarnessPool.Free_Object_Memory(harness[index]);
	Check("the block list is still intact once the objects are back",
		TheHarnessPool.Block_Count() == TheHarnessPool.Total_Object_Count() / 256);

	// PlatformMain.cpp's teardown, in order: the manager is shut down and three of the five
	// sections are cleared, and only then does exit() destroy the pools above.
	shutdownMemoryManager();
	std::printf("%-62s %s\n", "shutdownMemoryManager() left the allocator",
		TheDynamicMemoryAllocator == NULL ? "destroyed" : "alive (it was inited before main)");
	TheUnicodeStringCriticalSection = NULL;
	TheDmaCriticalSection = NULL;
	TheMemoryPoolCriticalSection = NULL;

	std::printf("main returning with %d failed checks\n", Failures);
	std::fflush(stdout);
	return Failures;
}
