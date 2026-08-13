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

static const UnsignedInt LANWIRE_REPLACEMENT		= 0xFFFD;	///< U+FFFD REPLACEMENT CHARACTER
static const UnsignedInt LANWIRE_SURROGATE_FIRST	= 0xD800;
static const UnsignedInt LANWIRE_SURROGATE_LAST		= 0xDFFF;
static const UnsignedInt LANWIRE_SURROGATE_SPLIT	= 0xDC00;	///< first low surrogate
static const UnsignedInt LANWIRE_MAX_CODEPOINT		= 0x10FFFF;

static inline Bool isHighSurrogate( UnsignedInt unit )
{
	return unit >= LANWIRE_SURROGATE_FIRST && unit < LANWIRE_SURROGATE_SPLIT;
}

static inline Bool isLowSurrogate( UnsignedInt unit )
{
	return unit >= LANWIRE_SURROGATE_SPLIT && unit <= LANWIRE_SURROGATE_LAST;
}

/// True where WideChar already holds UTF-16 code units rather than whole code points, i.e. on
/// Windows. A function rather than a `sizeof` in the condition, which MSVC warns about.
static inline Bool wideCharIsUtf16Unit()
{
	return sizeof(WideChar) == 2;
}

/// The value of one WideChar, without sign extension where wchar_t is a signed 16 bit type.
static inline UnsignedInt wideCharValue( WideChar c )
{
	if (wideCharIsUtf16Unit())
		return (UnsignedInt)(UnsignedShort)c;
	return (UnsignedInt)c;
}

// ------------------------------------------------------------------------------------------------
void lanWireStringSet( LANWireChar *dest, Int destCount, const WideChar *src )
{
	DEBUG_ASSERTCRASH(dest != nullptr && destCount > 0, ("lanWireStringSet: no destination"));
	if (dest == nullptr || destCount < 1)
		return;

	const Int limit = destCount - 1;	// the last unit is always the terminator
	Int out = 0;

	while (src != nullptr && *src != 0 && out < limit)
	{
		UnsignedInt cp = wideCharValue(*src);

		if (wideCharIsUtf16Unit())
		{
			// The source is already a sequence of UTF-16 code units, as it is with MSVC. Copy it
			// through, but never leave a high surrogate as the last unit of the field.
			if (isHighSurrogate(cp))
			{
				const UnsignedInt low = wideCharValue(src[1]);
				if (!isLowSurrogate(low) || out + 1 >= limit)
					break;
				dest[out++] = (LANWireChar)cp;
				dest[out++] = (LANWireChar)low;
				++src;
				continue;
			}
			if (isLowSurrogate(cp))
				cp = LANWIRE_REPLACEMENT;
		}
		else
		{
			// The source holds whole code points. Encode them.
			if (cp > LANWIRE_MAX_CODEPOINT || (cp >= LANWIRE_SURROGATE_FIRST && cp <= LANWIRE_SURROGATE_LAST))
				cp = LANWIRE_REPLACEMENT;

			if (cp > 0xFFFF)
			{
				if (out + 1 >= limit)
					break;
				const UnsignedInt offset = cp - 0x10000;
				dest[out++] = (LANWireChar)(LANWIRE_SURROGATE_FIRST + (offset >> 10));
				dest[out++] = (LANWireChar)(LANWIRE_SURROGATE_SPLIT + (offset & 0x3FF));
				++src;
				continue;
			}
		}

		dest[out++] = (LANWireChar)cp;
		++src;
	}

	dest[out] = 0;
}

// ------------------------------------------------------------------------------------------------
UnicodeString lanWireStringGet( const LANWireChar *src, Int srcCount )
{
	UnicodeString out;

	if (src == nullptr)
		return out;

	for (Int i = 0; i < srcCount && src[i] != 0; ++i)
	{
		UnsignedInt cp = src[i];

		// A pair only becomes one WideChar where WideChar is wide enough to hold the code point.
		// Where it is not - Windows - the pair is what the wide API wants, so it is left alone.
		if (!wideCharIsUtf16Unit() && isHighSurrogate(cp) && i + 1 < srcCount && isLowSurrogate(src[i + 1]))
		{
			cp = 0x10000 + ((cp - LANWIRE_SURROGATE_FIRST) << 10) + (src[i + 1] - LANWIRE_SURROGATE_SPLIT);
			++i;
		}
		else if (!wideCharIsUtf16Unit() && (isHighSurrogate(cp) || isLowSurrogate(cp)))
		{
			cp = LANWIRE_REPLACEMENT;
		}

		out.concat((WideChar)cp);
	}

	return out;
}
