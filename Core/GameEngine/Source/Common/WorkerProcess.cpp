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

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#ifdef _WIN32
// TheSuperHackers @port Win32 header pushed down from PreRTS.h; see docs/porting/prerts-win32-surgery.md
#include <windows.h>
#else
#include "WWLib/platform/platform_process.h"
#endif

#include "Common/WorkerProcess.h"

#ifdef _WIN32

// We need Job-related functions, but these aren't defined in the Windows-headers that VC6 uses.
// So we define them here and load them dynamically.
#if defined(_MSC_VER) && _MSC_VER < 1300
struct JOBOBJECT_BASIC_LIMIT_INFORMATION2
{
	LARGE_INTEGER PerProcessUserTimeLimit;
	LARGE_INTEGER PerJobUserTimeLimit;
	DWORD LimitFlags;
	SIZE_T MinimumWorkingSetSize;
	SIZE_T MaximumWorkingSetSize;
	DWORD ActiveProcessLimit;
	ULONG_PTR Affinity;
	DWORD PriorityClass;
	DWORD SchedulingClass;
};
struct IO_COUNTERS
{
	ULONGLONG ReadOperationCount;
	ULONGLONG WriteOperationCount;
	ULONGLONG OtherOperationCount;
	ULONGLONG ReadTransferCount;
	ULONGLONG WriteTransferCount;
	ULONGLONG OtherTransferCount;
};
struct JOBOBJECT_EXTENDED_LIMIT_INFORMATION
{
	JOBOBJECT_BASIC_LIMIT_INFORMATION2 BasicLimitInformation;
	IO_COUNTERS IoInfo;
	SIZE_T ProcessMemoryLimit;
	SIZE_T JobMemoryLimit;
	SIZE_T PeakProcessMemoryUsed;
	SIZE_T PeakJobMemoryUsed;
};

#define JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE 0x00002000
const int JobObjectExtendedLimitInformation = 9;

typedef HANDLE (WINAPI *PFN_CreateJobObjectW)(LPSECURITY_ATTRIBUTES, LPCWSTR);
typedef BOOL (WINAPI *PFN_SetInformationJobObject)(HANDLE, JOBOBJECTINFOCLASS, LPVOID, DWORD);
typedef BOOL (WINAPI *PFN_AssignProcessToJobObject)(HANDLE, HANDLE);

static PFN_CreateJobObjectW CreateJobObjectW = (PFN_CreateJobObjectW)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "CreateJobObjectW");
static PFN_SetInformationJobObject SetInformationJobObject = (PFN_SetInformationJobObject)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetInformationJobObject");
static PFN_AssignProcessToJobObject AssignProcessToJobObject = (PFN_AssignProcessToJobObject)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "AssignProcessToJobObject");
#endif

WorkerProcess::WorkerProcess()
{
	m_processHandle = nullptr;
	m_readHandle = nullptr;
	m_jobHandle = nullptr;
	m_exitcode = 0;
	m_isDone = false;
}


bool WorkerProcess::startProcess(UnicodeString command)
{
	m_stdOutput.clear();
	m_isDone = false;

	// Create pipe for reading console output
	SECURITY_ATTRIBUTES saAttr = { sizeof(SECURITY_ATTRIBUTES) };
	saAttr.bInheritHandle = TRUE;
	HANDLE writeHandle = nullptr;
	HANDLE readHandle = nullptr;
	if (!CreatePipe(&readHandle, &writeHandle, &saAttr, 0))
		return false;
	m_readHandle = readHandle;
	SetHandleInformation(readHandle, HANDLE_FLAG_INHERIT, 0);

	STARTUPINFOW si = { sizeof(STARTUPINFOW) };
	si.dwFlags = STARTF_FORCEOFFFEEDBACK; // Prevent cursor wait animation
	si.dwFlags |= STARTF_USESTDHANDLES;
	si.hStdError = writeHandle;
	si.hStdOutput = writeHandle;

	PROCESS_INFORMATION pi = { nullptr };

	if (!CreateProcessW(nullptr, (LPWSTR)command.str(),
			nullptr, nullptr, /*bInheritHandles=*/TRUE, 0,
			nullptr, nullptr, &si, &pi))
	{
		CloseHandle(writeHandle);
		CloseHandle(readHandle);
		m_readHandle = nullptr;
		return false;
	}

	CloseHandle(pi.hThread);
	CloseHandle(writeHandle);
	m_processHandle = pi.hProcess;

	// We want to make sure that when our process is killed, our workers automatically terminate as well.
	// In Windows, the way to do this is to attach the worker to a job we own.
	m_jobHandle = CreateJobObjectW != nullptr ? CreateJobObjectW(nullptr, nullptr) : nullptr;
	if (m_jobHandle != nullptr)
	{
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobInfo = { 0 };
		jobInfo.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		SetInformationJobObject(m_jobHandle, (JOBOBJECTINFOCLASS)JobObjectExtendedLimitInformation, &jobInfo, sizeof(jobInfo));
		AssignProcessToJobObject(m_jobHandle, m_processHandle);
	}

	return true;
}

bool WorkerProcess::isRunning() const
{
	return m_processHandle != nullptr;
}

bool WorkerProcess::isDone() const
{
	return m_isDone;
}

UnsignedInt WorkerProcess::getExitCode() const
{
	return m_exitcode;
}

AsciiString WorkerProcess::getStdOutput() const
{
	return m_stdOutput;
}

bool WorkerProcess::fetchStdOutput()
{
	while (true)
	{
		// Call PeekNamedPipe to make sure ReadFile won't block
		DWORD bytesAvailable = 0;
		DEBUG_ASSERTCRASH(m_readHandle != nullptr, ("Is not expected null"));
		BOOL success = PeekNamedPipe((HANDLE)m_readHandle, nullptr, 0, nullptr, &bytesAvailable, nullptr);
		if (!success)
			return true;
		if (bytesAvailable == 0)
		{
			// Child process is still running and we have all output so far
			return false;
		}

		DWORD readBytes = 0;
		char buffer[1024];
		success = ReadFile((HANDLE)m_readHandle, buffer, ARRAY_SIZE(buffer)-1, &readBytes, nullptr);
		if (!success)
			return true;
		DEBUG_ASSERTCRASH(readBytes != 0, ("expected readBytes to be non null"));

		// Remove \r, otherwise each new line is doubled when we output it again
		for (int i = 0; i < readBytes; i++)
			if (buffer[i] == '\r')
				buffer[i] = ' ';
		buffer[readBytes] = 0;
		m_stdOutput.concat(buffer);
	}
}

void WorkerProcess::update()
{
	if (!isRunning())
		return;

	if (!fetchStdOutput())
	{
		// There is still potential output pending
		return;
	}

	// Pipe broke, that means the process already exited. But we call this just to make sure
	WaitForSingleObject((HANDLE)m_processHandle, INFINITE);
	DWORD exitcode = 0;
	GetExitCodeProcess((HANDLE)m_processHandle, &exitcode);
	m_exitcode = exitcode;
	CloseHandle((HANDLE)m_processHandle);
	m_processHandle = nullptr;

	CloseHandle((HANDLE)m_readHandle);
	m_readHandle = nullptr;

	CloseHandle((HANDLE)m_jobHandle);
	m_jobHandle = nullptr;

	m_isDone = true;
}

void WorkerProcess::kill()
{
	if (!isRunning())
		return;

	if (m_processHandle != nullptr)
	{
		TerminateProcess((HANDLE)m_processHandle, 1);
		CloseHandle((HANDLE)m_processHandle);
		m_processHandle = nullptr;
	}

	if (m_readHandle != nullptr)
	{
		CloseHandle((HANDLE)m_readHandle);
		m_readHandle = nullptr;
	}

	if (m_jobHandle != nullptr)
	{
		CloseHandle((HANDLE)m_jobHandle);
		m_jobHandle = nullptr;
	}

	m_stdOutput.clear();
	m_isDone = false;
}

#else // !_WIN32

// TheSuperHackers @port The same class over an anonymous pipe and a forked child. The Win32 job
// object has no portable equivalent; see docs/porting/process-and-crash-seam.md for what that
// costs.

WorkerProcess::WorkerProcess()
{
	m_childProcess = nullptr;
	m_exitcode = 0;
	m_isDone = false;
}

bool WorkerProcess::startProcess(UnicodeString command)
{
	m_stdOutput.clear();
	m_isDone = false;

	AsciiString commandLine;
	commandLine.translate(command);

	m_childProcess = WWPlatform::Child_Process_Start(commandLine.str());
	return m_childProcess != nullptr;
}

bool WorkerProcess::isRunning() const
{
	return m_childProcess != nullptr;
}

bool WorkerProcess::isDone() const
{
	return m_isDone;
}

UnsignedInt WorkerProcess::getExitCode() const
{
	return m_exitcode;
}

AsciiString WorkerProcess::getStdOutput() const
{
	return m_stdOutput;
}

bool WorkerProcess::fetchStdOutput()
{
	while (true)
	{
		char buffer[1024];
		Int readBytes = WWPlatform::Child_Process_Read(m_childProcess, buffer, ARRAY_SIZE(buffer)-1);
		if (readBytes < 0)
		{
			// The pipe is closed, so the child is done writing.
			return true;
		}
		if (readBytes == 0)
		{
			// Child process is still running and we have all output so far
			return false;
		}

		// Remove \r, otherwise each new line is doubled when we output it again
		for (Int i = 0; i < readBytes; i++)
			if (buffer[i] == '\r')
				buffer[i] = ' ';
		buffer[readBytes] = 0;
		m_stdOutput.concat(buffer);
	}
}

void WorkerProcess::update()
{
	if (!isRunning())
		return;

	if (!fetchStdOutput())
	{
		// There is still potential output pending
		return;
	}

	m_exitcode = WWPlatform::Child_Process_Wait(m_childProcess);
	WWPlatform::Child_Process_Close(m_childProcess);
	m_childProcess = nullptr;

	m_isDone = true;
}

void WorkerProcess::kill()
{
	if (!isRunning())
		return;

	WWPlatform::Child_Process_Kill(m_childProcess);
	WWPlatform::Child_Process_Close(m_childProcess);
	m_childProcess = nullptr;

	m_stdOutput.clear();
	m_isDone = false;
}

#endif // _WIN32

