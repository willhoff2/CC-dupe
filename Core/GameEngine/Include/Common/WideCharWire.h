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

// FILE: WideCharWire.h ///////////////////////////////////////////////////////////////////////////
// Desc:   Unicode text as it appears outside this process: little-endian UTF-16 code units of a
//         fixed 16 bit width, on disk, in a save game, in a CRC and on the wire.
//
// TheSuperHackers @port WideChar is wchar_t, so it is 2 bytes with MSVC and 4 bytes on macOS and
// Linux. Every external format the game has - .csf string tables, .map chunks, save games, replays,
// network packets - stores 16 bit units, because that is what the Windows build wrote. Code that
// sized those units with sizeof(WideChar) therefore read and wrote twice as many bytes as exist
// natively; the first native run walked out of the middle of the next .csf record that way. Convert
// at the crossing instead: these functions are the only place the width changes.
//
// This is the LANWireChar seam of docs/porting/lanmessage-64bit.md generalised - LANWireChar is
// WideWireChar now and LANWireString.cpp calls through here, so there is one conversion, not two.
// See docs/porting/widechar-wire.md.
///////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Lib/BaseType.h"

/// One UTF-16 code unit outside the process. Fixed width on every target, unlike WideChar.
typedef UnsignedShort WideWireChar;

STATIC_ASSERT_ALWAYS(sizeof(WideWireChar) == 2, "WideWireChar must be a 16 bit code unit");

/// Pass as the source or destination length where the buffer is NUL terminated instead of counted.
const Int WIDECHAR_WIRE_NUL_TERMINATED = -1;

/// A staging buffer big enough for the longest text any of these formats carries: a save game and a
/// CRC cap a string at 255 WideChar, i.e. at most 510 code units.
const Int WIDECHAR_WIRE_MAX_UNITS = 1024;

namespace WideCharWire
{

const UnsignedInt REPLACEMENT		= 0xFFFD;	///< U+FFFD REPLACEMENT CHARACTER
const UnsignedInt SURROGATE_FIRST	= 0xD800;
const UnsignedInt SURROGATE_SPLIT	= 0xDC00;	///< first low surrogate
const UnsignedInt SURROGATE_LAST		= 0xDFFF;
const UnsignedInt MAX_CODEPOINT		= 0x10FFFF;

inline Bool isHighSurrogate( UnsignedInt unit )
{
	return unit >= SURROGATE_FIRST && unit < SURROGATE_SPLIT;
}

inline Bool isLowSurrogate( UnsignedInt unit )
{
	return unit >= SURROGATE_SPLIT && unit <= SURROGATE_LAST;
}

/// True where WideChar already holds UTF-16 code units rather than whole code points, i.e. on
/// Windows. A function rather than a `sizeof` in the condition, which MSVC warns about.
inline Bool wideCharIsUtf16Unit()
{
	return sizeof(WideChar) == 2;
}

/// The value of one WideChar, without sign extension where wchar_t is a signed 16 bit type.
inline UnsignedInt wideCharValue( WideChar c )
{
	if (wideCharIsUtf16Unit())
		return (UnsignedInt)(UnsignedShort)c;
	return (UnsignedInt)c;
}

}	// namespace WideCharWire

/**
 * The number of bytes `units` UTF-16 code units occupy outside the process.
 *
 * Use this, never `sizeof(WideChar)`, to size a read or a write of external text: the two agree
 * only on Windows.
 */
inline Int wideCharWireBytes( Int units )
{
	return units * (Int)sizeof(WideWireChar);
}

/**
 * The number of UTF-16 code units `src` needs outside the process, not counting a terminator.
 *
 * `srcCount` is a count of WideChar, or WIDECHAR_WIRE_NUL_TERMINATED. On Windows this is the
 * WideChar count; natively a code point above U+FFFF counts as the two units of its surrogate pair.
 */
inline Int wideCharWireUnitCount( const WideChar *src, Int srcCount )
{
	if (src == NULL)
		return 0;

	Int units = 0;
	for (Int i = 0; srcCount == WIDECHAR_WIRE_NUL_TERMINATED || i < srcCount; ++i)
	{
		if (src[i] == 0)
			break;
		const UnsignedInt cp = WideCharWire::wideCharValue(src[i]);
		if (!WideCharWire::wideCharIsUtf16Unit() && cp > 0xFFFF && cp <= WideCharWire::MAX_CODEPOINT)
			units += 2;
		else
			++units;
	}
	return units;
}

/**
 * Encode `src` into the `destUnits` code units at `dest` as UTF-16, and return how many units were
 * written. Does NOT terminate: callers whose format has a terminator write it themselves.
 *
 * `srcCount` is a count of WideChar, or WIDECHAR_WIRE_NUL_TERMINATED. Text too long for `destUnits`
 * is truncated at a code point boundary, never leaving a lone surrogate. Code points that have no
 * UTF-16 representation - a lone surrogate in the source, or a value above U+10FFFF - become
 * U+FFFD. On Windows this is a copy: WideChar is already UTF-16 there.
 */
inline Int wideCharToWire( WideWireChar *dest, Int destUnits, const WideChar *src, Int srcCount )
{
	if (dest == NULL || src == NULL || destUnits <= 0)
		return 0;

	Int out = 0;
	for (Int i = 0; srcCount == WIDECHAR_WIRE_NUL_TERMINATED || i < srcCount; ++i)
	{
		if (src[i] == 0 || out >= destUnits)
			break;

		UnsignedInt cp = WideCharWire::wideCharValue(src[i]);

		if (WideCharWire::wideCharIsUtf16Unit())
		{
			// The source is already a sequence of UTF-16 code units, as it is with MSVC. Copy it
			// through, but never leave a high surrogate as the last unit written.
			if (WideCharWire::isHighSurrogate(cp))
			{
				const Bool haveLow = (srcCount == WIDECHAR_WIRE_NUL_TERMINATED || i + 1 < srcCount);
				const UnsignedInt low = haveLow ? WideCharWire::wideCharValue(src[i + 1]) : 0;
				if (!WideCharWire::isLowSurrogate(low) || out + 1 >= destUnits)
					break;
				dest[out++] = (WideWireChar)cp;
				dest[out++] = (WideWireChar)low;
				++i;
				continue;
			}
			if (WideCharWire::isLowSurrogate(cp))
				cp = WideCharWire::REPLACEMENT;
		}
		else
		{
			// The source holds whole code points. Encode them.
			if (cp > WideCharWire::MAX_CODEPOINT
					|| (cp >= WideCharWire::SURROGATE_FIRST && cp <= WideCharWire::SURROGATE_LAST))
				cp = WideCharWire::REPLACEMENT;

			if (cp > 0xFFFF)
			{
				if (out + 1 >= destUnits)
					break;
				const UnsignedInt offset = cp - 0x10000;
				dest[out++] = (WideWireChar)(WideCharWire::SURROGATE_FIRST + (offset >> 10));
				dest[out++] = (WideWireChar)(WideCharWire::SURROGATE_SPLIT + (offset & 0x3FF));
				continue;
			}
		}

		dest[out++] = (WideWireChar)cp;
	}

	return out;
}

/**
 * Decode up to `srcUnits` code units from `src`, stopping at the first NUL, into the `destCount`
 * WideChars at `dest`, and return how many WideChars were written. Does NOT terminate.
 *
 * Surrogate pairs are recombined where WideChar is wide enough to hold the code point, and kept as
 * a pair where it is not (i.e. on Windows, which is what its wide API expects). A malformed
 * surrogate becomes U+FFFD. On Windows this is a copy.
 */
inline Int wireToWideChar( WideChar *dest, Int destCount, const WideWireChar *src, Int srcUnits )
{
	if (dest == NULL || src == NULL || destCount <= 0)
		return 0;

	Int out = 0;
	for (Int i = 0; i < srcUnits && src[i] != 0 && out < destCount; ++i)
	{
		UnsignedInt cp = src[i];

		// A pair only becomes one WideChar where WideChar is wide enough to hold the code point.
		// Where it is not - Windows - the pair is what the wide API wants, so it is left alone.
		if (!WideCharWire::wideCharIsUtf16Unit() && WideCharWire::isHighSurrogate(cp)
				&& i + 1 < srcUnits && WideCharWire::isLowSurrogate(src[i + 1]))
		{
			cp = 0x10000 + ((cp - WideCharWire::SURROGATE_FIRST) << 10)
					+ (src[i + 1] - WideCharWire::SURROGATE_SPLIT);
			++i;
		}
		else if (!WideCharWire::wideCharIsUtf16Unit()
				&& (WideCharWire::isHighSurrogate(cp) || WideCharWire::isLowSurrogate(cp)))
		{
			cp = WideCharWire::REPLACEMENT;
		}

		dest[out++] = (WideChar)cp;
	}

	return out;
}
