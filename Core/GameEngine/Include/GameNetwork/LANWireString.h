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

// FILE: LANWireString.h //////////////////////////////////////////////////////////////////////////
// Desc:   Unicode text as it appears in a LAN packet: a fixed-width array of little-endian UTF-16
//         code units, the same width on every target.
//
// TheSuperHackers @port LANMessage is `#pragma pack(1)` and is sent as raw bytes, so every member's
// width is part of the wire format. Its Unicode members used to be `WideChar`, which is 2 bytes with
// MSVC and 4 bytes on macOS/Linux; that made sizeof(LANMessage) 471 bytes on Windows and 536 bytes
// on LP64, over the 476 byte packet limit, which is what the assertion in LANAPI.h caught. The
// members are `LANWireChar` now, so the packet has one layout everywhere - and, because MSVC's
// wchar_t is already a UTF-16 code unit, byte for byte the layout Windows always had. See
// docs/porting/lanmessage-64bit.md.
///////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Lib/BaseType.h"
#include "Common/UnicodeString.h"

/// One UTF-16 code unit on the wire. Fixed width on every target, unlike WideChar.
typedef UnsignedShort LANWireChar;

STATIC_ASSERT_ALWAYS(sizeof(LANWireChar) == 2, "LANWireChar must be a 16 bit code unit on the wire");

/**
 * Write `src` into the `destCount` code units at `dest` as UTF-16, always NUL terminating.
 *
 * Text too long for the field is truncated, never with a lone surrogate at the end. Code points
 * that cannot be represented (a lone surrogate in the source, or a code point that no longer fits
 * as a pair) become U+FFFD. On Windows this is a copy: WideChar is already UTF-16 there.
 */
void lanWireStringSet( LANWireChar *dest, Int destCount, const WideChar *src );

/**
 * Read up to `srcCount` code units from `src`, stopping at the first NUL, and return them as a
 * UnicodeString. Surrogate pairs are recombined where WideChar is wide enough to hold the code
 * point, and kept as a pair where it is not (i.e. on Windows, which is what its wide API expects).
 */
UnicodeString lanWireStringGet( const LANWireChar *src, Int srcCount );
