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

// FILE: LANWireString.cpp ////////////////////////////////////////////////////////////////////////
// Desc:   UnicodeString to and from the fixed-width UTF-16 text fields of a LAN packet.
///////////////////////////////////////////////////////////////////////////////////////////////////

// USER INCLUDES //////////////////////////////////////////////////////////////////////////////////
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine
#include "GameNetwork/LANWireString.h"

// TheSuperHackers @port The conversion itself lives in Common/WideCharWire.h now, so the LAN wire,
// the .csf reader, save games, the CRC and .map chunks all share one implementation. These two
// functions are only the LAN-shaped interface to it: a fixed size field that is always terminated,
// and a UnicodeString out. See docs/porting/widechar-wire.md.

// ------------------------------------------------------------------------------------------------
void lanWireStringSet( LANWireChar *dest, Int destCount, const WideChar *src )
{
	DEBUG_ASSERTCRASH(dest != nullptr && destCount > 0, ("lanWireStringSet: no destination"));
	if (dest == nullptr || destCount < 1)
		return;

	// The last unit of the field is always the terminator, so only destCount - 1 units are text.
	const Int units = wideCharToWire( dest, destCount - 1, src, WIDECHAR_WIRE_NUL_TERMINATED );
	dest[units] = 0;
}

// ------------------------------------------------------------------------------------------------
UnicodeString lanWireStringGet( const LANWireChar *src, Int srcCount )
{
	UnicodeString out;

	if (src == nullptr || srcCount <= 0)
		return out;

	WideChar buffer[WIDECHAR_WIRE_MAX_UNITS + 1];
	const Int cap = srcCount < WIDECHAR_WIRE_MAX_UNITS ? srcCount : WIDECHAR_WIRE_MAX_UNITS;
	const Int chars = wireToWideChar( buffer, cap, src, cap );
	buffer[chars] = 0;
	out.set( buffer );

	return out;
}
