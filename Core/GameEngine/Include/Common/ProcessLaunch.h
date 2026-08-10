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

namespace rts
{

// TheSuperHackers @port Starts another executable and does not wait for it, the way
// _spawnl(_P_NOWAIT, ...) does. Returns false if it could not be started. Off Windows the
// detection of a missing executable is weaker than _spawnl's; see
// docs/porting/process-and-crash-seam.md.
bool launchProcessDetached(const char* executablePath);

} // namespace rts
