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

// TheSuperHackers @port MultiByteToWideChar / WideCharToMultiByte for non-Windows platforms.
// See docs/porting/sockets-and-text-encoding.md.
//
// These replace the earlier `#define MultiByteToWideChar(...) mbstowcs(...)` aliases, which had
// neither the signature nor the semantics of the functions they stood in for: mbstowcs()
// ignores the code page, returns (size_t)-1 rather than 0 on failure, does not support the
// "measure the output" call with a zero-sized destination, and converts in the current C
// locale, which is "C" (i.e. ASCII only) unless something calls setlocale().
//
// What is implemented here is the Win32 contract, for the two code pages the engine passes:
//
//   * cbMultiByte / cchWideChar of -1 means "the string is null terminated"; the terminator is
//     converted too and is counted in the return value.
//   * a non-negative count means exactly that many units are converted, and no terminator is
//     written or counted.
//   * a destination size of 0 means "return the number of units required, write nothing".
//   * the return value is 0 on failure, including when the destination is too small.
//
// Differences from Windows that cannot be avoided here, both consequences of wchar_t being
// 4 bytes off Windows and 2 bytes on it:
//
//   * The wide form is UTF-32, not UTF-16. A character outside the Basic Multilingual Plane is
//     one wchar_t here and a surrogate pair on Windows, so the wide length of such a string
//     differs between platforms. Everything the game ships in .csf/.str is BMP.
//   * CP_ACP is the system ANSI code page on Windows (1252 for a Western install). There is no
//     such notion off Windows, and the honest approximation is UTF-8, which agrees with CP_ACP
//     for the ASCII range and disagrees above it.
#pragma once

#ifndef _WIN32

#include <stddef.h>
#include <wchar.h>

#ifndef CP_ACP
#define CP_ACP 0
#endif
#ifndef CP_UTF8
#define CP_UTF8 65001
#endif

namespace unicode_compat_detail
{

// Decodes one UTF-8 sequence. Returns the number of bytes consumed, or 0 if the input is not
// well formed, which is the point at which the Win32 functions fail without MB_ERR_INVALID_CHARS
// only if the caller asked for it -- these fall back to U+FFFD instead, as Windows does.
inline int Decode_Utf8(const unsigned char *input, int available, unsigned int &codePoint)
{
	const unsigned char lead = input[0];

	if (lead < 0x80)
	{
		codePoint = lead;
		return 1;
	}

	int length;
	unsigned int value;
	if ((lead & 0xE0) == 0xC0)      { length = 2; value = lead & 0x1F; }
	else if ((lead & 0xF0) == 0xE0) { length = 3; value = lead & 0x0F; }
	else if ((lead & 0xF8) == 0xF0) { length = 4; value = lead & 0x07; }
	else
	{
		codePoint = 0xFFFD;
		return 1;
	}

	if (length > available)
	{
		codePoint = 0xFFFD;
		return 1;
	}

	for (int i = 1; i < length; ++i)
	{
		if ((input[i] & 0xC0) != 0x80)
		{
			codePoint = 0xFFFD;
			return 1;
		}
		value = (value << 6) | (unsigned int)(input[i] & 0x3F);
	}

	codePoint = value;
	return length;
}

// Encodes one code point as UTF-8. Returns the number of bytes it needs; writes them only if
// there is room, so the same routine serves the measuring and the converting pass.
inline int Encode_Utf8(unsigned int codePoint, char *output, int room)
{
	int length;
	if (codePoint < 0x80)         length = 1;
	else if (codePoint < 0x800)   length = 2;
	else if (codePoint < 0x10000) length = 3;
	else if (codePoint < 0x110000) length = 4;
	else { codePoint = 0xFFFD; length = 3; }

	if (output == nullptr || room < length)
	{
		return length;
	}

	switch (length)
	{
		case 1:
			output[0] = (char)codePoint;
			break;
		case 2:
			output[0] = (char)(0xC0 | (codePoint >> 6));
			output[1] = (char)(0x80 | (codePoint & 0x3F));
			break;
		case 3:
			output[0] = (char)(0xE0 | (codePoint >> 12));
			output[1] = (char)(0x80 | ((codePoint >> 6) & 0x3F));
			output[2] = (char)(0x80 | (codePoint & 0x3F));
			break;
		default:
			output[0] = (char)(0xF0 | (codePoint >> 18));
			output[1] = (char)(0x80 | ((codePoint >> 12) & 0x3F));
			output[2] = (char)(0x80 | ((codePoint >> 6) & 0x3F));
			output[3] = (char)(0x80 | (codePoint & 0x3F));
			break;
	}
	return length;
}

}	// namespace unicode_compat_detail

inline int MultiByteToWideChar(unsigned int codePage, unsigned long flags,
	const char *multiByteString, int multiByteCount, wchar_t *wideCharString, int wideCharCount)
{
	(void)codePage;	// CP_ACP and CP_UTF8 are both read as UTF-8 here; see the note above.
	(void)flags;

	if (multiByteString == nullptr)
	{
		return 0;
	}

	const bool nullTerminated = (multiByteCount < 0);
	int remaining = multiByteCount;
	if (nullTerminated)
	{
		remaining = 0;
		while (multiByteString[remaining] != '\0')
		{
			++remaining;
		}
		++remaining;	// the terminator is converted and counted too
	}

	const unsigned char *input = (const unsigned char *)multiByteString;
	int produced = 0;

	while (remaining > 0)
	{
		unsigned int codePoint = 0;
		const int consumed = unicode_compat_detail::Decode_Utf8(input, remaining, codePoint);
		input += consumed;
		remaining -= consumed;

		if (wideCharCount > 0)
		{
			if (produced >= wideCharCount)
			{
				return 0;	// ERROR_INSUFFICIENT_BUFFER, as on Windows
			}
			wideCharString[produced] = (wchar_t)codePoint;
		}
		++produced;
	}

	return produced;
}

inline int WideCharToMultiByte(unsigned int codePage, unsigned long flags,
	const wchar_t *wideCharString, int wideCharCount, char *multiByteString, int multiByteCount,
	const char *defaultChar, int *usedDefaultChar)
{
	(void)codePage;
	(void)flags;
	(void)defaultChar;

	if (usedDefaultChar != nullptr)
	{
		*usedDefaultChar = 0;	// UTF-8 represents every code point, so nothing is substituted
	}

	if (wideCharString == nullptr)
	{
		return 0;
	}

	int remaining = wideCharCount;
	if (remaining < 0)
	{
		remaining = 0;
		while (wideCharString[remaining] != L'\0')
		{
			++remaining;
		}
		++remaining;	// the terminator is converted and counted too
	}

	int produced = 0;
	for (int i = 0; i < remaining; ++i)
	{
		char encoded[4];
		const int length = unicode_compat_detail::Encode_Utf8(
			(unsigned int)wideCharString[i], encoded, (int)sizeof(encoded));

		if (multiByteCount > 0)
		{
			if (produced + length > multiByteCount)
			{
				return 0;	// ERROR_INSUFFICIENT_BUFFER, as on Windows
			}
			for (int b = 0; b < length; ++b)
			{
				multiByteString[produced + b] = encoded[b];
			}
		}
		produced += length;
	}

	return produced;
}

#endif // !_WIN32
