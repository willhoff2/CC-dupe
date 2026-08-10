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

#include "WWLib/platform/platform_process.h"

#ifndef _WIN32

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/prctl.h>
#endif

namespace WWPlatform
{

namespace
{

/*
**	Where the single instance lock files live. A per user, per boot directory is wanted, so
**	that a stale file cannot deny a later login and so that two users can each run their own
**	copy. XDG_RUNTIME_DIR is exactly that on Linux; TMPDIR is the closest macOS has, since its
**	per user temporary directory is already private to the user.
*/
const char * Runtime_Directory()
{
	const char * directory = getenv("XDG_RUNTIME_DIR");
	if (directory == nullptr || *directory == 0) {
		directory = getenv("TMPDIR");
	}
	if (directory == nullptr || *directory == 0) {
		directory = "/tmp";
	}
	return directory;
}

struct ChildProcess
{
	pid_t Pid;
	int ReadDescriptor;
	bool Reaped;
	unsigned int ExitCode;

	ChildProcess() : Pid(-1), ReadDescriptor(-1), Reaped(false), ExitCode(0) {}
};

}	// anonymous namespace

void * Instance_Lock_Acquire(const char * name)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/%s.lock", Runtime_Directory(), name);

	int descriptor = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (descriptor < 0) {
		return nullptr;
	}

	/*
	**	The lock, not the file, is what says an instance is running. flock() releases when the
	**	descriptor closes, including when the process dies for any reason, so a crashed game
	**	does not lock out the next one. The file itself is left behind on purpose: deleting it
	**	races with the next process opening it.
	*/
	if (flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
		close(descriptor);
		return nullptr;
	}

	/*
	**	Record the owner, for a human looking at the file rather than for the code.
	*/
	if (ftruncate(descriptor, 0) == 0) {
		char text[32];
		int length = snprintf(text, sizeof(text), "%ld\n", (long)getpid());
		if (write(descriptor, text, length) != length) {
			// Nothing reads this, so a short write is not worth failing the lock over.
		}
	}

	int * handle = new int(descriptor);
	return handle;
}

void Instance_Lock_Release(void * handle)
{
	if (handle == nullptr) {
		return;
	}

	int * descriptor = static_cast<int *>(handle);
	flock(*descriptor, LOCK_UN);
	close(*descriptor);
	delete descriptor;
}

bool Process_Spawn_Detached(const char * path)
{
	if (path == nullptr || *path == 0) {
		return false;
	}

	pid_t pid = fork();
	if (pid < 0) {
		return false;
	}

	if (pid == 0) {
		/*
		**	The intermediate child leaves the session and forks again, so that the grandchild
		**	is inherited by init and never becomes a zombie this process has to reap.
		**	_spawnl(_P_NOWAIT) needs no such dance because Win32 processes are not parented.
		*/
		setsid();
		pid_t grandchild = fork();
		if (grandchild == 0) {
			char * const argv[] = { const_cast<char *>(path), nullptr };
			execv(path, argv);
			_exit(127);
		}
		_exit(grandchild < 0 ? 1 : 0);
	}

	int status = 0;
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
	}

	/*
	**	This only reports whether the fork succeeded. Whether the executable exists is known to
	**	the grandchild alone, exactly as _spawnl() would report it only if it could not start
	**	the image at all - and unlike _spawnl(), a missing file is not detected here.
	*/
	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

void * Child_Process_Start(const char * command_line)
{
	if (command_line == nullptr || *command_line == 0) {
		return nullptr;
	}

	int descriptors[2];
	if (pipe(descriptors) != 0) {
		return nullptr;
	}

	pid_t pid = fork();
	if (pid < 0) {
		close(descriptors[0]);
		close(descriptors[1]);
		return nullptr;
	}

	if (pid == 0) {
		close(descriptors[0]);
		dup2(descriptors[1], STDOUT_FILENO);
		dup2(descriptors[1], STDERR_FILENO);
		close(descriptors[1]);

#if defined(__linux__)
		/*
		**	The Win32 code puts the child in a job object with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
		**	so that it dies with the parent however the parent dies. PR_SET_PDEATHSIG is the
		**	Linux equivalent. macOS has no equivalent at all; see the seam document.
		*/
		prctl(PR_SET_PDEATHSIG, SIGKILL);
#endif

		execl("/bin/sh", "sh", "-c", command_line, (char *)nullptr);
		_exit(127);
	}

	close(descriptors[1]);
	fcntl(descriptors[0], F_SETFL, O_NONBLOCK);

	ChildProcess * child = new ChildProcess;
	child->Pid = pid;
	child->ReadDescriptor = descriptors[0];
	return child;
}

int Child_Process_Read(void * handle, char * buffer, int size)
{
	if (handle == nullptr || buffer == nullptr || size <= 0) {
		return -1;
	}

	ChildProcess * child = static_cast<ChildProcess *>(handle);
	if (child->ReadDescriptor < 0) {
		return -1;
	}

	while (true) {
		ssize_t got = read(child->ReadDescriptor, buffer, (size_t)size);
		if (got > 0) {
			return (int)got;
		}
		if (got == 0) {
			// Every writing end is closed, so the child is done writing.
			return -1;
		}
		if (errno == EINTR) {
			continue;
		}
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return 0;
		}
		return -1;
	}
}

unsigned int Child_Process_Wait(void * handle)
{
	if (handle == nullptr) {
		return 0;
	}

	ChildProcess * child = static_cast<ChildProcess *>(handle);
	if (child->Reaped || child->Pid < 0) {
		return child->ExitCode;
	}

	int status = 0;
	while (waitpid(child->Pid, &status, 0) < 0) {
		if (errno != EINTR) {
			child->Reaped = true;
			return child->ExitCode;
		}
	}

	if (WIFEXITED(status)) {
		child->ExitCode = (unsigned int)WEXITSTATUS(status);
	} else if (WIFSIGNALED(status)) {
		child->ExitCode = 128u + (unsigned int)WTERMSIG(status);
	}

	child->Reaped = true;
	return child->ExitCode;
}

void Child_Process_Kill(void * handle)
{
	if (handle == nullptr) {
		return;
	}

	ChildProcess * child = static_cast<ChildProcess *>(handle);
	if (child->Pid > 0 && !child->Reaped) {
		kill(child->Pid, SIGKILL);
		int status = 0;
		while (waitpid(child->Pid, &status, 0) < 0 && errno == EINTR) {
		}
		child->Reaped = true;
		child->ExitCode = 1;
	}
}

void Child_Process_Close(void * handle)
{
	if (handle == nullptr) {
		return;
	}

	ChildProcess * child = static_cast<ChildProcess *>(handle);
	if (child->ReadDescriptor >= 0) {
		close(child->ReadDescriptor);
		child->ReadDescriptor = -1;
	}
	delete child;
}

}	// namespace WWPlatform

#endif // !_WIN32
