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
#include <process.h>
#else
#include "WWLib/platform/platform_process.h"
#endif

#include "Common/ProcessLaunch.h"

namespace rts
{

bool launchProcessDetached(const char* executablePath)
{
#ifdef _WIN32
	return _spawnl(_P_NOWAIT, executablePath, executablePath, nullptr) >= 0;
#else
	return WWPlatform::Process_Spawn_Detached(executablePath);
#endif
}

} // namespace rts
