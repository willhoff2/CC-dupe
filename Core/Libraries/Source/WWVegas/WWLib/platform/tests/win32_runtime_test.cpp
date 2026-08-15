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
 *  Behaviour test for the kernel32 runtime seam: the mutex handle, CreateThread(), the           *
 *  Interlocked family, the process heap and the lstr* helpers. The native build proves the seam  *
 *  links; it says nothing about whether the mutex is recursive the way ScopedMutex.h assumes,    *
 *  whether InterlockedCompareExchange() returns the initial value the lock-free queue compares   *
 *  against, or whether lstrcpyn() counts the terminator the way W3DAssetManager.cpp relies on.   *
 *  Those were argued from documentation; this asserts them.                                     *
 *                                                                                             *
 *  Windows is the oracle: every expectation here is what kernel32 does, so the test builds and   *
 *  passes on Windows too and a divergence shows up as a failure. See                             *
 *  docs/porting/win32-runtime-and-crt-gaps.md.                                                  *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <chrono>
#include <thread>

static int _Failures = 0;
static int _Checks = 0;

static void Check(bool condition, const char * what)
{
	_Checks++;
	if (!condition) {
		_Failures++;
		printf("FAIL: %s\n", what);
	}
}


/***********************************************************************************************
 *                                                                                             *
 *  Mutex handles. What Common/ScopedMutex.h and MilesAudioDevice's AudioFileCache do with them:  *
 *  create one by name, take it, take it again from the same thread, hand it back.                *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void Test_Mutex()
{
	HANDLE mutex = CreateMutex(nullptr, FALSE, "Win32RuntimeTestMutex");
	Check(mutex != nullptr, "CreateMutex() returns a handle");
	if (mutex == nullptr) {
		return;
	}

	Check(WaitForSingleObject(mutex, INFINITE) == WAIT_OBJECT_0,
		"WaitForSingleObject() takes an unowned mutex");

	/*
	**	Win32 mutexes are recursive per thread, which is what makes a ScopedMutex inside a call
	**	that already holds the same mutex safe rather than a deadlock.
	*/
	Check(WaitForSingleObject(mutex, INFINITE) == WAIT_OBJECT_0,
		"a mutex is recursive for the thread that owns it");
	Check(ReleaseMutex(mutex) != FALSE, "ReleaseMutex() releases the inner take");
	Check(ReleaseMutex(mutex) != FALSE, "ReleaseMutex() releases the outer take");

	/*
	**	A timed wait is the only reason the body is a recursive_timed_mutex; nothing in the
	**	engine uses one yet, but the handle promises it.
	*/
	Check(WaitForSingleObject(mutex, 0) == WAIT_OBJECT_0,
		"a zero timeout still takes a free mutex");
	Check(ReleaseMutex(mutex) != FALSE, "ReleaseMutex() after the zero timeout wait");

	Check(CloseHandle(mutex) != FALSE, "CloseHandle() accepts a mutex handle");
}


/***********************************************************************************************
 *                                                                                             *
 *  Threads. W3DScreenshot.cpp starts one, closes the handle, and never looks again; the only    *
 *  thing it needs is that the routine runs with its parameter.                                  *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static LONG _Thread_Saw = 0;

static DWORD Thread_Body(LPVOID parameter)
{
	InterlockedExchange(&_Thread_Saw, static_cast<LONG>(reinterpret_cast<intptr_t>(parameter)));
	return 0;
}

static void Test_Thread()
{
	DWORD thread_id = 0;
	HANDLE thread = CreateThread(nullptr, 0, Thread_Body,
		reinterpret_cast<LPVOID>(static_cast<intptr_t>(1234)), 0, &thread_id);
	Check(thread != nullptr, "CreateThread() starts a thread");
	if (thread == nullptr) {
		return;
	}
	Check(thread_id != 0, "CreateThread() reports a thread id");
	Check(CloseHandle(thread) != FALSE, "CloseHandle() accepts a thread handle");

	/*
	**	Polling rather than waiting on the handle, because the thread runs detached here and the
	**	handle is deliberately not waitable. Ten seconds is a hang, not a slow machine.
	*/
	LONG seen = 0;
	for (int attempt = 0; attempt < 1000 && seen == 0; attempt++) {
		seen = InterlockedExchangeAdd(&_Thread_Saw, 0);
		if (seen == 0) {
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
	}
	Check(seen == 1234, "the thread body ran with its parameter");
}


/***********************************************************************************************
 *                                                                                             *
 *  Interlocked. The return values matter more than the effects: mpsc_intrusive_queue.h decides   *
 *  whether its compare exchange won by comparing the result against the comparand.              *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void Test_Interlocked()
{
	LONG value = 7;
	Check(InterlockedIncrement(&value) == 8, "InterlockedIncrement() returns the new value");
	Check(InterlockedDecrement(&value) == 7, "InterlockedDecrement() returns the new value");
	Check(InterlockedExchange(&value, 3) == 7, "InterlockedExchange() returns the old value");
	Check(value == 3, "InterlockedExchange() stored the new value");
	Check(InterlockedExchangeAdd(&value, 4) == 3, "InterlockedExchangeAdd() returns the old value");
	Check(value == 7, "InterlockedExchangeAdd() added");

	Check(InterlockedCompareExchange(&value, 9, 7) == 7,
		"InterlockedCompareExchange() returns the initial value when it wins");
	Check(value == 9, "InterlockedCompareExchange() stored on a match");
	Check(InterlockedCompareExchange(&value, 5, 7) == 9,
		"InterlockedCompareExchange() returns the initial value when it loses");
	Check(value == 9, "InterlockedCompareExchange() left the value alone on a mismatch");

	int first = 0;
	int second = 0;
	PVOID pointer = &first;
	Check(InterlockedExchangePointer(&pointer, &second) == &first,
		"InterlockedExchangePointer() returns the old pointer");
	Check(pointer == &second, "InterlockedExchangePointer() stored the new pointer");

	Check(InterlockedCompareExchangePointer(&pointer, &first, &second) == &second,
		"InterlockedCompareExchangePointer() returns the initial pointer when it wins");
	Check(pointer == &first, "InterlockedCompareExchangePointer() stored on a match");
	Check(InterlockedCompareExchangePointer(&pointer, nullptr, &second) == &first,
		"InterlockedCompareExchangePointer() returns the initial pointer when it loses");
	Check(pointer == &first, "InterlockedCompareExchangePointer() left the pointer alone");
}


/***********************************************************************************************
 *                                                                                             *
 *  The process heap.                                                                            *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void Test_Heap()
{
	HANDLE heap = GetProcessHeap();
	Check(heap != nullptr, "GetProcessHeap() returns a usable handle");

	unsigned char * block = static_cast<unsigned char *>(HeapAlloc(heap, HEAP_ZERO_MEMORY, 64));
	Check(block != nullptr, "HeapAlloc() returns a block");
	if (block != nullptr) {
		bool zeroed = true;
		for (int index = 0; index < 64; index++) {
			zeroed = zeroed && (block[index] == 0);
		}
		Check(zeroed, "HEAP_ZERO_MEMORY zeroes the block");
		Check(HeapFree(heap, 0, block) != FALSE, "HeapFree() releases the block");
	}

	/*
	**	Win32 hands back a usable pointer for a zero byte request; malloc(0) may not, which a
	**	caller would read as failure.
	*/
	LPVOID empty = HeapAlloc(heap, 0, 0);
	Check(empty != nullptr, "a zero byte HeapAlloc() still succeeds");
	HeapFree(heap, 0, empty);
}


/***********************************************************************************************
 *                                                                                             *
 *  lstr*. The one that matters is lstrcpyn(): its length counts the terminator, so it is        *
 *  strlcpy() and not strncpy(). W3DAssetManager.cpp cuts a mesh name out of "container.mesh"     *
 *  with it and would keep the separator if this were off by one.                                *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void Test_Strings()
{
	char buffer[32];

	memset(buffer, 'x', sizeof(buffer));
	Check(lstrcpyn(buffer, "container.mesh", 10) == buffer, "lstrcpyn() returns its destination");
	Check(strcmp(buffer, "container") == 0, "lstrcpyn(n) copies n - 1 characters and terminates");

	memset(buffer, 'x', sizeof(buffer));
	lstrcpyn(buffer, "ab", 10);
	Check(strcmp(buffer, "ab") == 0, "lstrcpyn() stops at the source terminator");

	lstrcpy(buffer, "mesh");
	Check(strcmp(buffer, "mesh") == 0, "lstrcpy() copies the whole string");
	Check(lstrlen(buffer) == 4, "lstrlen() counts without the terminator");
	lstrcat(buffer, ".gr2");
	Check(strcmp(buffer, "mesh.gr2") == 0, "lstrcat() appends");

	Check(lstrcmp("a", "a") == 0, "lstrcmp() reports equal strings");
	Check(lstrcmp("a", "b") < 0, "lstrcmp() orders by character");
	Check(lstrcmpi("MESH.GR2", "mesh.gr2") == 0, "lstrcmpi() ignores case");
	Check(lstrcmpi("mesh", "mesi") < 0, "lstrcmpi() still orders");
}


int main()
{
	Test_Mutex();
	Test_Thread();
	Test_Interlocked();
	Test_Heap();
	Test_Strings();

	printf("%d checks, %d failures\n", _Checks, _Failures);
	return (_Failures == 0) ? 0 : 1;
}
