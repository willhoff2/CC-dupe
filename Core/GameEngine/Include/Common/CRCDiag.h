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

// FILE: CRCDiag.h ////////////////////////////////////////////////////////////////////////////////
// Desc:   Opt-in decomposition of the light CRC, for comparing a native run against a Windows run
//         of the same replay. Dormant unless CNC_CRC_DIAG names an output file, so release
//         behaviour - and therefore the Windows behaviour this port is measured against - is
//         unchanged. See docs/porting/crc-divergence.md.
///////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Lib/BaseType.h"

namespace CRCDiag
{

	// TRUE when CNC_CRC_DIAG names a writable output file. Checked once, on first use.
	Bool isEnabled();

	// TRUE when CNC_CRC_DIAG_CONTINUE is set: replay playback logs every CRC comparison and does
	// not stop at the first mismatch, so a whole run can be compared instead of just its first
	// differing checkpoint.
	Bool continuesPastMismatch();

	// TRUE when the current logic frame is inside the CNC_CRC_DIAG_BYTES=<first>:<last> range, in
	// which case every block of bytes fed to the CRC is dumped. This is very verbose; it is how a
	// differing object becomes a differing field.
	Bool dumpsBytes();

	// The running CRC at a "MARKER:..." label, i.e. at a subsystem boundary of GameLogic::getCRC()
	// or, at a depth above zero, inside one of those subsystems.
	void logMarker( const char *name, Int depth, UnsignedInt crc );

	// The running CRC after one object of the object list, with the identity needed to find the
	// same object in the other platform's log.
	void logObject( UnsignedInt objectID, const char *templateName, UnsignedInt crc );

	// The complete light CRC of the current frame, as GameLogic caches it.
	void logFrameTotal( UnsignedInt crc );

	// One block of bytes fed to the CRC, with the running CRC after it.
	void logBytes( const void *data, Int dataSize, UnsignedInt crc );

	// A replay CRC comparison: the locally computed value against the recorded one.
	void logCompare( UnsignedInt frame, UnsignedInt localCRC, UnsignedInt replayCRC );

} // namespace CRCDiag
