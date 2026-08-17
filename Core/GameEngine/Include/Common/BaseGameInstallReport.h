/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
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

// FILE: BaseGameInstallReport.h ////////////////////////////////////////////////////////////////
// TheSuperHackers @port Why Zero Hour could not mount the base game's archives, said out loud.
//
// Zero Hour reads both archive sets: its own from the working directory, and Generals' from the
// `InstallPath` setting (`Win32BIGFileSystem::init()` -- the one the ported engine creates -- and
// its C++17 twin `StdBIGFileSystem::init()`, both under `RTS_ZEROHOUR`). When that mount does not
// happen the engine keeps running with a shell whose backdrop is a magenta placeholder and no
// `m_skyBox`, and the first symptom is a crash in a sky-drawing map. Before this seam the only
// diagnostic was a debug-build assertion on the empty case, so a release build said nothing at
// all -- see docs/porting/base-game-install-path.md and docs/porting/init-failure-reporting.md.
//
// The three failures are different repairs, so they are different messages: nothing configured,
// a path that is not there, and a path that is there but holds no archives (the depot's Zero Hour
// directory rather than the base game's, typically). The classification is separated from the
// reporting so it can be exercised without a filesystem or an engine -- see
// scripts/native-base-game-install-test.py.
//
// Reporting is deliberately not fatal: the game does run without the base game's archives, and
// making a missing setting fatal would break installs that work today.
///////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <stdio.h>
#include <string.h>

namespace rts
{

	enum BaseGameInstallStatus
	{
		BASE_GAME_INSTALL_MOUNTED = 0,   ///< archives were mounted; nothing to report
		BASE_GAME_INSTALL_NOT_CONFIGURED,///< the setting is absent or empty
		BASE_GAME_INSTALL_PATH_MISSING,  ///< the setting names a directory that could not be read
		BASE_GAME_INSTALL_NO_ARCHIVES,   ///< the directory is readable and holds no `.big` files
	};

	// Which of the three failures happened, from what the mount attempt saw. `installPath` is the
	// setting's value, `pathReadable` whether the directory could be enumerated, and
	// `archivesMounted` whether at least one archive was actually added.
	inline BaseGameInstallStatus classifyBaseGameInstall(const char* installPath, bool pathReadable,
		bool archivesMounted)
	{
		if (installPath == nullptr || installPath[0] == 0)
			return BASE_GAME_INSTALL_NOT_CONFIGURED;
		if (!pathReadable)
			return BASE_GAME_INSTALL_PATH_MISSING;
		if (!archivesMounted)
			return BASE_GAME_INSTALL_NO_ARCHIVES;
		return BASE_GAME_INSTALL_MOUNTED;
	}

	// Whether the mount has to add a separator to this directory before the search mask goes on
	// the end of it. The mount concatenates directory and mask with nothing in between
	// (Win32LocalFileSystem::getFileListInDirectory(), then FindFirstFile()), so
	// "/depot/Generals" + "*.big" searches the *parent* for "Generals*.big" and finds no archives
	// at all. Windows' registry value ends with a separator, which is why the concatenation was
	// written that way and why this only ever fires on a hand-set value.
	inline bool baseGameInstallNeedsSeparator(const char* installPath)
	{
		if (installPath == nullptr || installPath[0] == 0)
			return false;
		const char last = installPath[strlen(installPath) - 1];
		return last != '\\' && last != '/';
	}

	// The message for a status, written into `out`. Returns false for BASE_GAME_INSTALL_MOUNTED,
	// which has no message. Each message names the setting, the repair, and the consequence of not
	// making it, because the consequence (placeholder art, then a crash in a sky-drawing map) is
	// what the reader is actually looking at when they read this.
	inline bool describeBaseGameInstall(BaseGameInstallStatus status, const char* installPath,
		char* out, size_t outSize)
	{
		if (out == nullptr || outSize == 0)
			return false;
		out[0] = 0;

		const char* path = installPath != nullptr ? installPath : "";
		switch (status)
		{
			case BASE_GAME_INSTALL_NOT_CONFIGURED:
				snprintf(out, outSize,
					"ERROR: the original Generals install path is not configured, so the base "
					"game's archives are not mounted. Zero Hour needs both sets: set InstallPath "
					"to the directory holding the base game's .big files. Until then the main "
					"menu backdrop and the sky are missing, and a mission that draws a sky will "
					"crash.");
				break;
			case BASE_GAME_INSTALL_PATH_MISSING:
				snprintf(out, outSize,
					"ERROR: the original Generals install path '%s' could not be read, so the "
					"base game's archives are not mounted. Point InstallPath at the directory "
					"holding the base game's .big files. Until then the main menu backdrop and "
					"the sky are missing, and a mission that draws a sky will crash.", path);
				break;
			case BASE_GAME_INSTALL_NO_ARCHIVES:
				snprintf(out, outSize,
					"ERROR: the original Generals install path '%s' holds no .big archives, so "
					"the base game's archives are not mounted. InstallPath must name the base "
					"game's own directory, not Zero Hour's. Until then the main menu backdrop "
					"and the sky are missing, and a mission that draws a sky will crash.", path);
				break;
			case BASE_GAME_INSTALL_MOUNTED:
			default:
				return false;
		}
		out[outSize - 1] = 0;
		return true;
	}

}  // namespace rts
