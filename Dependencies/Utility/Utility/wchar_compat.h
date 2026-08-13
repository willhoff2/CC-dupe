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

// This file contains WCHAR and related macros for compatibility with non-windows platforms.
#pragma once

// WCHAR
typedef wchar_t WCHAR;
typedef const WCHAR* LPCWSTR;
typedef WCHAR* LPWSTR;

#include <wchar.h>
#include <wctype.h>

#define _wcsicmp wcscasecmp
#define wcsicmp wcscasecmp

// TheSuperHackers @port The MSVC wide-character CRT spellings. These are real definitions rather
// than declarations: they are CRT functions, not Win32 entry points, so nothing has to be
// implemented behind them later. C linkage because that is the linkage a C library gives the names
// they impersonate -- no vendor header in the tree declares them today, but _strlwr in
// string_compat.h is what a C++-linkage definition of a reserved CRT name costs when one does, and
// scripts/ci/check-crt-compat.py exists to catch the next instance.
#ifdef __cplusplus
extern "C" {
#endif

// _wtoi(): wcstol() with base 10, which is what MSVC's implementation is.
inline int _wtoi(const wchar_t *str) { return (int)wcstol(str, NULL, 10); }

// iswascii(): MSVC defines it as a range test on the value, independent of locale, and so does
// this. Not iswctype()-based: the point of the call site (GameWindowGlobal.cpp's edit-box filter)
// is "is this one of the 128 ASCII code points", which is a property of the number.
//
// The BSD C library, and so macOS, already supplies iswascii -- as a macro on some releases and
// as an inline function on others, which is why the guard tests both. Not compiled on macOS by
// anyone yet.
#if !defined(iswascii) && !defined(__APPLE__) && !defined(__FreeBSD__)
inline int iswascii(wint_t c) { return ((unsigned)(c) < 0x80u); }
#endif

#ifdef __cplusplus
}
#endif

// MultiByteToWideChar / WideCharToMultiByte, with the Win32 signature and semantics rather
// than the mbstowcs()/wcstombs() aliases that used to stand in for them here.
#include "unicode_compat.h"

