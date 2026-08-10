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

#pragma once

// Helper class that allows you to start a worker process and retrieve its exit code
// and console output as a string.
// It also makes sure that the started process is killed in case our process exits in any way.
class WorkerProcess
{
public:
	WorkerProcess();

	bool startProcess(UnicodeString command);

	void update();

	bool isRunning() const;

	// returns true iff the process exited.
	bool isDone() const;

	UnsignedInt getExitCode() const;
	AsciiString getStdOutput() const;

	// Terminate Process if it's running
	void kill();

private:
	// returns true if all output has been received
	// returns false if the worker is still running
	bool fetchStdOutput();

private:
	// TheSuperHackers @port The Win32 members are HANDLEs, kept as void* so that this header does
	// not need windows.h; off Windows a single opaque child process replaces all three. See
	// docs/porting/process-and-crash-seam.md.
#ifdef _WIN32
	void* m_processHandle;
	void* m_readHandle;
	void* m_jobHandle;
#else
	void* m_childProcess;
#endif
	AsciiString m_stdOutput;
	UnsignedInt m_exitcode;
	bool m_isDone;
};
