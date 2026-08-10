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
 *                 Project Name : Westwood Library                                             *
 *                                                                                             *
 *  Child processes and single instance detection for the platforms that have no CreateProcess, *
 *  no _spawnl and no named kernel objects. Handles are opaque so that no system headers leak   *
 *  into the engine headers that use them.                                                      *
 *                                                                                             *
 *  See docs/porting/process-and-crash-seam.md for where the Win32 semantics and these ones     *
 *  differ. The important one: the single instance lock is an advisory flock() on a file, and   *
 *  a file, unlike a Win32 named mutex, outlives the process that made it.                      *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#pragma once

#ifndef _WIN32

namespace WWPlatform
{

/*
**	Single instance detection. Takes an exclusive advisory lock on a file named after the
**	instance name in the per user runtime directory, which is the closest portable equivalent
**	of CreateMutex() on a globally unique name plus a GetLastError() == ERROR_ALREADY_EXISTS
**	test.
**
**	Returns null if another live process already holds the lock, i.e. the caller is not the
**	first instance. Any other failure - the directory is not writable, the platform does not
**	support flock() - also returns null, so the caller behaves as if an instance was already
**	running rather than silently allowing two.
*/
void * Instance_Lock_Acquire(const char * name);
void Instance_Lock_Release(void * handle);

/*
**	Equivalent of _spawnl(_P_NOWAIT, path, path, nullptr). Starts the executable detached from
**	this process and does not wait for it. Returns false if the process could not be started.
**	Unlike _spawnl() the search is not extended with PATHEXT, and unlike Win32 the child is not
**	killed when this process exits.
*/
bool Process_Spawn_Detached(const char * path);

/*
**	A child process whose standard output and standard error are captured through a pipe.
**	This is the equivalent of CreatePipe() plus CreateProcessW() with STARTF_USESTDHANDLES,
**	and the reading side is non blocking so that the caller can poll it the way
**	PeekNamedPipe() lets it poll on Win32.
**
**	Command_Line is parsed with the POSIX shell's quoting rules by /bin/sh -c, because the
**	engine's only caller builds one string rather than an argument vector. Win32 parses the
**	command line in the child; the shell does it here.
*/
void * Child_Process_Start(const char * command_line);

/*
**	Reads whatever output is available without blocking. Returns the number of bytes placed in
**	the buffer, zero if the child is alive and has produced nothing since the last call, and
**	-1 once the pipe has been closed at the far end, which is how the caller learns that the
**	child has finished writing.
*/
int Child_Process_Read(void * handle, char * buffer, int size);

/*
**	Waits for the child to exit and reports its exit code. Matches WaitForSingleObject() plus
**	GetExitCodeProcess(). A child killed by a signal reports 128 + the signal number, the
**	convention every POSIX shell uses, because there is no Win32 style exit code for it.
*/
unsigned int Child_Process_Wait(void * handle);

/*
**	Equivalent of TerminateProcess(handle, 1). Sends SIGKILL, so the child gets no chance to
**	clean up, which is what TerminateProcess() does too.
*/
void Child_Process_Kill(void * handle);

/*
**	Closes the pipe and forgets the child. Does not wait for it.
*/
void Child_Process_Close(void * handle);

}	// namespace WWPlatform

#endif // !_WIN32
