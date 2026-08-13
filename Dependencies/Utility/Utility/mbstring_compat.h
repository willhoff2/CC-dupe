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

// TheSuperHackers @port The single MSVC <mbstring.h> entry point this codebase uses.
//
// IMEManager.cpp calls _mbsnccnt() to convert a character position in the IME composition string
// into a byte position. MSVC's multibyte CRT works in the process code page; there is no code page
// off Windows, so this works in the C locale's multibyte encoding via mblen(), which is UTF-8 in
// any locale a native build will run under. For the ASCII subset the two agree exactly; for CJK
// composition strings they do not, and that is a real difference rather than a hidden one -- the
// IME path is Windows-only anyway (it is driven by ImmGetCompositionStringW and friends), so this
// exists to let the translation unit compile, not to make Asian input work off Windows.
#pragma once

#ifndef _WIN32

#include <stdlib.h>
#include <string.h>

// Byte count of the first `count` multibyte characters of `str`, stopping at the terminator.
// MSVC returns the byte count; a malformed sequence stops the walk, as it does there.
inline size_t _mbsnccnt(const unsigned char *str, size_t count)
{
	const char *p = (const char *)str;
	size_t bytes = 0;
	for (size_t i = 0; i < count && p[bytes] != '\0'; ++i)
	{
		const int len = mblen(p + bytes, MB_CUR_MAX);
		if (len <= 0)
		{
			break;
		}
		bytes += (size_t)len;
	}
	return bytes;
}

#endif	// !_WIN32
