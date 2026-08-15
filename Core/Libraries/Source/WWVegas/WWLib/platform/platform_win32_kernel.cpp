/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 TheSuperHackers
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
 *  The kernel32 runtime half of the Win32 compatibility layer: the four calls that make a       *
 *  named mutex work, one-shot worker threads, the Interlocked family, the process heap, and the  *
 *  lstr* string helpers. See docs/porting/win32-runtime-and-crt-gaps.md.                        *
 *                                                                                             *
 *  Mutexes. CreateMutex()/WaitForSingleObject()/ReleaseMutex()/CloseHandle() is how             *
 *  MilesAudioDevice's AudioFileCache guards its cache, through Common/ScopedMutex.h. The body   *
 *  is WWPlatform::Mutex_* (platform_mutex.cpp, a std::recursive_timed_mutex), which already     *
 *  matches Win32's recursive, timed semantics; what this file adds is the handle. Two           *
 *  deliberate differences from Windows, neither of which any caller in the engine depends on:   *
 *  the name is process local rather than kernel wide, so two processes asking for               *
 *  "AudioFileCacheMutex" get two different mutexes; and an abandoned mutex (the owner exits     *
 *  while holding it) is not detected, so WAIT_ABANDONED is never returned.                      *
 *                                                                                             *
 *  Threads. CreateThread() over WWPlatform::Thread_Create() (platform_thread.cpp, a               *
 *  std::thread the library detaches), for the fire-and-forget worker W3DScreenshot.cpp starts     *
 *  and then closes the handle of. The thread is detached, so the handle is a token that only      *
 *  CloseHandle() accepts: waiting on it, suspending it or killing it is not supported and says    *
 *  so out loud. CREATE_SUSPENDED therefore fails rather than starting the thread running, which   *
 *  is the safe direction -- its only caller is MiniDumper.cpp, whose crash reporting is already   *
 *  stubbed off Windows (docs/porting/process-and-crash-seam.md).                                         *
 *                                                                                             *
 *  Interlocked. clang's __atomic builtins with seq_cst ordering, which is what the x86 LOCK-    *
 *  prefixed instructions kernel32 and the Win64 intrinsics use give. The pointer-width forms    *
 *  are the ones mpsc_intrusive_queue.h's lock-free push/pop needs; VC6 gets them from           *
 *  Utility/interlocked_adapter.h instead, and that adapter is left alone.                       *
 *                                                                                             *
 *  Process heap. GetProcessHeap()/HeapAlloc()/HeapFree() over malloc(), with the one flag the   *
 *  engine passes (HEAP_ZERO_MEMORY) honoured. The handle is a fixed non-null token: there is    *
 *  only ever one heap here, and no caller does anything with the value but pass it back.        *
 *                                                                                             *
 *  lstr*. The Win32 string helpers, which differ from the CRT ones they look like: lstrcpyn()   *
 *  counts the terminator in its length (so it is strlcpy(), not strncpy()), and lstrcmpi()      *
 *  is a locale comparison on Windows -- approximated here by the ASCII one, which is what the   *
 *  engine's uses (asset filenames) actually need. They live here rather than in                 *
 *  Dependencies/Utility because they are Win32 API entry points, declared by <windows.h> with   *
 *  C linkage, not CRT spellings.                                                                *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#include "WWLib/platform/platform_win32_compat.h"

#ifdef WWPLATFORM_WIN32_COMPAT

#include "WWLib/platform/platform_mutex.h"
#include "WWLib/platform/platform_thread.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <map>
#include <mutex>
#include <string>

namespace
{

/*
**	What a HANDLE from CreateMutex() points at. The signature is checked by the calls that
**	consume a handle, so that a handle of another kind -- a thread handle from CreateThread()
**	below, or one from a call the port's <windows.h> declares but nothing implements yet, such as
**	CreateEvent() -- is refused rather than dereferenced.
*/
const unsigned int MUTEX_SIGNATURE = 0x4D555458;	// 'MUTX'
const unsigned int THREAD_SIGNATURE = 0x54485244;	// 'THRD'

struct MutexHandle
{
	unsigned int Signature;
	void * Body;
	std::string Name;
	int References;
};

struct ThreadHandle
{
	unsigned int Signature;
	unsigned long Token;
};

/*
**	What the thread entry point needs that WWPlatform::Thread_Create()'s void(void *) signature
**	cannot carry. Allocated by CreateThread(), released by the trampoline below.
*/
struct ThreadStart
{
	LPTHREAD_START_ROUTINE Routine;
	LPVOID Parameter;
};

void Thread_Trampoline(void * parameter)
{
	ThreadStart * start = static_cast<ThreadStart *>(parameter);
	/*
	**	Win32 makes the return value the thread's exit code, which nothing here can be asked for
	**	because the thread is detached, so it is dropped.
	*/
	start->Routine(start->Parameter);
	delete start;
}

/*
**	Named mutexes are shared within the process, so that two CreateMutex() calls with the same
**	name hand back the same lock and the second one reports ERROR_ALREADY_EXISTS, as Windows
**	does. Unnamed ones are never shared. The table's own lock is a plain std::mutex: it is held
**	only across the lookup, never across a wait.
*/
std::mutex & Table_Lock()
{
	static std::mutex _lock;
	return _lock;
}

std::map<std::string, MutexHandle *> & Named_Mutexes()
{
	static std::map<std::string, MutexHandle *> _table;
	return _table;
}

MutexHandle * As_Mutex(HANDLE handle)
{
	MutexHandle * mutex = static_cast<MutexHandle *>(handle);
	if (mutex == nullptr || mutex->Signature != MUTEX_SIGNATURE) {
		return nullptr;
	}
	return mutex;
}

ThreadHandle * As_Thread(HANDLE handle)
{
	ThreadHandle * thread = static_cast<ThreadHandle *>(handle);
	if (thread == nullptr || thread->Signature != THREAD_SIGNATURE) {
		return nullptr;
	}
	return thread;
}

}	// anonymous namespace

extern "C" {

/***********************************************************************************************
 *                                                                                             *
 *  Mutex handles.                                                                              *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

HANDLE CreateMutexA(LPSECURITY_ATTRIBUTES, BOOL initial_owner, LPCSTR name)
{
	std::lock_guard<std::mutex> guard(Table_Lock());

	if (name != nullptr) {
		std::map<std::string, MutexHandle *>::iterator existing = Named_Mutexes().find(name);
		if (existing != Named_Mutexes().end()) {
			existing->second->References++;
			WWPlatform::Win32::Set_Last_Error(ERROR_ALREADY_EXISTS);
			if (initial_owner != FALSE) {
				WWPlatform::Mutex_Lock(existing->second->Body, -1);
			}
			return existing->second;
		}
	}

	MutexHandle * mutex = new MutexHandle;
	mutex->Signature = MUTEX_SIGNATURE;
	mutex->Body = WWPlatform::Mutex_Create();
	mutex->Name = (name != nullptr) ? name : "";
	mutex->References = 1;

	if (name != nullptr) {
		Named_Mutexes()[mutex->Name] = mutex;
	}
	if (initial_owner != FALSE) {
		WWPlatform::Mutex_Lock(mutex->Body, -1);
	}

	WWPlatform::Win32::Set_Last_Error(ERROR_SUCCESS);
	return mutex;
}


DWORD WaitForSingleObject(HANDLE handle, DWORD milliseconds)
{
	if (As_Thread(handle) != nullptr) {
		WWPlatform::Win32::Report_Stub("WaitForSingleObject(thread)",
			"threads are started detached here, so joining one is not supported");
		WWPlatform::Win32::Set_Last_Error(ERROR_INVALID_HANDLE);
		return WAIT_FAILED;
	}

	MutexHandle * mutex = As_Mutex(handle);
	if (mutex == nullptr) {
		WWPlatform::Win32::Set_Last_Error(ERROR_INVALID_HANDLE);
		return WAIT_FAILED;
	}

	const int time = (milliseconds == INFINITE) ? -1 : static_cast<int>(milliseconds);
	if (!WWPlatform::Mutex_Lock(mutex->Body, time)) {
		return WAIT_TIMEOUT;
	}
	return WAIT_OBJECT_0;
}


BOOL ReleaseMutex(HANDLE handle)
{
	MutexHandle * mutex = As_Mutex(handle);
	if (mutex == nullptr) {
		WWPlatform::Win32::Set_Last_Error(ERROR_INVALID_HANDLE);
		return FALSE;
	}

	WWPlatform::Mutex_Unlock(mutex->Body);
	return TRUE;
}


BOOL CloseHandle(HANDLE handle)
{
	ThreadHandle * thread = As_Thread(handle);
	if (thread != nullptr) {
		/*
		**	As on Windows, closing the handle does not stop the thread; the thread was started
		**	detached, so there is nothing left to release but the token.
		*/
		thread->Signature = 0;
		delete thread;
		return TRUE;
	}

	MutexHandle * mutex = As_Mutex(handle);
	if (mutex == nullptr) {
		WWPlatform::Win32::Set_Last_Error(ERROR_INVALID_HANDLE);
		return FALSE;
	}

	std::lock_guard<std::mutex> guard(Table_Lock());
	mutex->References--;
	if (mutex->References > 0) {
		return TRUE;
	}

	if (!mutex->Name.empty()) {
		Named_Mutexes().erase(mutex->Name);
	}
	WWPlatform::Mutex_Destroy(mutex->Body);
	mutex->Signature = 0;
	delete mutex;
	return TRUE;
}


/***********************************************************************************************
 *                                                                                             *
 *  Threads.                                                                                    *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

HANDLE CreateThread(LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE routine,
	LPVOID parameter, DWORD flags, LPDWORD thread_id)
{
	if (routine == nullptr) {
		WWPlatform::Win32::Set_Last_Error(ERROR_INVALID_PARAMETER);
		return nullptr;
	}

	if ((flags & CREATE_SUSPENDED) != 0) {
		WWPlatform::Win32::Report_Stub("CreateThread(CREATE_SUSPENDED)",
			"a thread cannot be started suspended here, so none is started");
		WWPlatform::Win32::Set_Last_Error(ERROR_INVALID_PARAMETER);
		return nullptr;
	}

	ThreadStart * start = new ThreadStart;
	start->Routine = routine;
	start->Parameter = parameter;

	const unsigned long token = WWPlatform::Thread_Create(Thread_Trampoline, start);
	if (token == 0) {
		delete start;
		return nullptr;
	}

	ThreadHandle * thread = new ThreadHandle;
	thread->Signature = THREAD_SIGNATURE;
	thread->Token = token;

	if (thread_id != nullptr) {
		*thread_id = static_cast<DWORD>(token);
	}

	WWPlatform::Win32::Set_Last_Error(ERROR_SUCCESS);
	return thread;
}


/***********************************************************************************************
 *                                                                                             *
 *  Interlocked.                                                                                *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

LONG InterlockedIncrement(LONG volatile * addend)
{
	return __atomic_add_fetch(addend, 1, __ATOMIC_SEQ_CST);
}


LONG InterlockedDecrement(LONG volatile * addend)
{
	return __atomic_sub_fetch(addend, 1, __ATOMIC_SEQ_CST);
}


LONG InterlockedExchange(LONG volatile * target, LONG value)
{
	return __atomic_exchange_n(target, value, __ATOMIC_SEQ_CST);
}


LONG InterlockedExchangeAdd(LONG volatile * addend, LONG value)
{
	return __atomic_fetch_add(addend, value, __ATOMIC_SEQ_CST);
}


/*
**	Win32 returns the initial value, not whether the exchange happened, so the caller compares
**	the result against the comparand. __atomic_compare_exchange_n writes the observed value back
**	into the expected operand, which is that same answer.
*/
LONG InterlockedCompareExchange(LONG volatile * destination, LONG exchange, LONG comparand)
{
	LONG expected = comparand;
	__atomic_compare_exchange_n(destination, &expected, exchange, false,
		__ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
	return expected;
}


PVOID InterlockedExchangePointer(PVOID volatile * target, PVOID value)
{
	return __atomic_exchange_n(target, value, __ATOMIC_SEQ_CST);
}


PVOID InterlockedCompareExchangePointer(PVOID volatile * destination, PVOID exchange,
	PVOID comparand)
{
	PVOID expected = comparand;
	__atomic_compare_exchange_n(destination, &expected, exchange, false,
		__ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
	return expected;
}


/***********************************************************************************************
 *                                                                                             *
 *  The process heap.                                                                           *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

HANDLE GetProcessHeap()
{
	/*
	**	A token, not an object: there is one heap and nothing reads the value. Not null, because
	**	callers test for failure.
	*/
	static int _heap = 0;
	return &_heap;
}


LPVOID HeapAlloc(HANDLE, DWORD flags, SIZE_T bytes)
{
	/*
	**	Win32 allows a zero byte allocation and returns a usable pointer for it; malloc(0) may
	**	return null, which callers would read as failure.
	*/
	if (bytes == 0) {
		bytes = 1;
	}
	void * block = malloc(bytes);
	if (block != nullptr && (flags & HEAP_ZERO_MEMORY) != 0) {
		memset(block, 0, bytes);
	}
	return block;
}


BOOL HeapFree(HANDLE, DWORD, LPVOID block)
{
	free(block);
	return TRUE;
}


/*
**	malloc() does not remember a usable size portably, and the engine only ever calls this for
**	bookkeeping it can do without, so it reports "unknown" rather than a wrong number.
*/
SIZE_T HeapSize(HANDLE, DWORD, LPCVOID)
{
	return static_cast<SIZE_T>(-1);
}


/***********************************************************************************************
 *                                                                                             *
 *  lstr*.                                                                                      *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

int lstrlenA(LPCSTR string)
{
	return (string != nullptr) ? static_cast<int>(strlen(string)) : 0;
}


LPSTR lstrcpyA(LPSTR destination, LPCSTR source)
{
	if (destination == nullptr || source == nullptr) {
		return nullptr;
	}
	strcpy(destination, source);
	return destination;
}


/*
**	Win32's length counts the terminator: lstrcpyn(d, s, 5) writes at most four characters and
**	always terminates, unlike strncpy(). W3DAssetManager.cpp relies on that to cut a mesh name
**	out of a "container.mesh" string.
*/
LPSTR lstrcpynA(LPSTR destination, LPCSTR source, int length)
{
	if (destination == nullptr || source == nullptr || length <= 0) {
		return destination;
	}

	int index = 0;
	for (; index < length - 1 && source[index] != '\0'; index++) {
		destination[index] = source[index];
	}
	destination[index] = '\0';
	return destination;
}


LPSTR lstrcatA(LPSTR destination, LPCSTR source)
{
	if (destination == nullptr || source == nullptr) {
		return destination;
	}
	strcat(destination, source);
	return destination;
}


int lstrcmpA(LPCSTR left, LPCSTR right)
{
	return strcmp(left, right);
}


int lstrcmpiA(LPCSTR left, LPCSTR right)
{
	return strcasecmp(left, right);
}

}	// extern "C"

#endif // WWPLATFORM_WIN32_COMPAT
