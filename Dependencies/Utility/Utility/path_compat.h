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

// TheSuperHackers @port The MSVC CRT path/permission spellings the engine calls that the other
// compat headers do not cover: _splitpath() (<stdlib.h> on MSVC), _mkdir() (<direct.h>) and
// _chmod()/_S_IREAD/_S_IWRITE (<io.h> and <sys/stat.h>). Off Windows _splitpath has no POSIX
// equivalent -- basename()/dirname() neither split the extension nor write into caller buffers --
// so it is reimplemented here to MSVC's documented behaviour; _mkdir() and _chmod() map onto
// POSIX mkdir()/chmod(), which mean the same thing.
//
// Like Utility/socket_compat.h this header is deliberately *not* included from Utility/compat.h:
// compat.h reaches every translation unit through BaseTypeCore.h and <sys/stat.h> has no business
// being there. Include it directly, in the few files that split paths or change file modes.
#pragma once

#ifdef _WIN32

#include <io.h>
#include <stdlib.h>
#include <sys/stat.h>

#else

#include <string.h>
#include <sys/stat.h>

#define _chmod chmod

// MSVC's _mkdir takes no mode: a new directory gets the default permissions. 0777 is that,
// modulo the process umask, which is what every other POSIX directory creation in the engine
// (WWLib/platform/platform_path.cpp) passes.
inline int _mkdir(const char *path)
{
	return mkdir(path, 0777);
}

#ifndef _S_IREAD
#define _S_IREAD S_IRUSR
#endif
#ifndef _S_IWRITE
#define _S_IWRITE S_IWUSR
#endif

// MSVC's _splitpath: any of the four output buffers may be null, meaning "do not want that
// piece", and each one that is not null is always written, empty string included. There are no
// drive letters off Windows, so the drive component is always empty; both separators are
// accepted because the paths the engine splits are as often authored with backslashes as read
// back from the filesystem. The extension includes its dot, as it does on Windows.
inline void _splitpath(const char *path, char *drive, char *directory, char *filename, char *extension)
{
	if (drive != nullptr)
	{
		drive[0] = '\0';
	}

	const char *start = (path != nullptr) ? path : "";

	const char *last_slash = nullptr;
	for (const char *cursor = start; *cursor != '\0'; ++cursor)
	{
		if (*cursor == '/' || *cursor == '\\')
		{
			last_slash = cursor;
		}
	}

	const char *name = (last_slash != nullptr) ? last_slash + 1 : start;

	if (directory != nullptr)
	{
		const size_t length = (size_t)(name - start);
		memcpy(directory, start, length);
		directory[length] = '\0';
	}

	// A leading dot is part of the name, not the start of an extension, which is what MSVC does
	// with ".gitignore" and what the engine's ".BIG"-style names rely on.
	const char *dot = nullptr;
	for (const char *cursor = name + (*name != '\0' ? 1 : 0); *cursor != '\0'; ++cursor)
	{
		if (*cursor == '.')
		{
			dot = cursor;
		}
	}

	const char *name_end = (dot != nullptr) ? dot : name + strlen(name);

	if (filename != nullptr)
	{
		const size_t length = (size_t)(name_end - name);
		memcpy(filename, name, length);
		filename[length] = '\0';
	}

	if (extension != nullptr)
	{
		strcpy(extension, name_end);
	}
}

#endif // _WIN32
