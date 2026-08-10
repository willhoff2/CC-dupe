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

/* $Header: /G/wwlib/bittype.h 4     4/02/99 1:37p Eric_c $ */
/***************************************************************************
 ***                  Confidential - Westwood Studios                    ***
 ***************************************************************************
 *                                                                         *
 *                 Project Name : Voxel Technology                         *
 *                                                                         *
 *                    File Name : BITTYPE.h                                *
 *                                                                         *
 *                   Programmer : Greg Hjelstrom                           *
 *                                                                         *
 *                   Start Date : 02/24/97                                 *
 *                                                                         *
 *                  Last Update : February 24, 1997 [GH]                   *
 *                                                                         *
 *-------------------------------------------------------------------------*
 * Functions:                                                              *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#pragma once

// TheSuperHackers @port The width of these types is part of the on-disk format of every
// .w3d asset and of the chunk headers in chunkio. `unsigned long` is 4 bytes under both
// ILP32 and Windows' LLP64, but 8 bytes under LP64 (macOS/Linux 64-bit), so the widths are
// spelled explicitly here. `stdint_adapter.h` supplies the C99 exact-width types on VC6,
// which has no <cstdint>.
#include <Utility/stdint_adapter.h>
#include <Utility/CppMacros.h>

typedef uint8_t			uint8;
typedef uint16_t		uint16;
typedef uint32_t		uint32;
typedef unsigned int    uint;

typedef int8_t			sint8;
typedef int16_t			sint16;
typedef int32_t			sint32;
typedef signed int      sint;

typedef float				float32;
typedef double				float64;

// The Win32 vocabulary types below are also declared by <windows.h>. A typedef may be
// repeated only if it names the same type, so on Windows these must stay spelled exactly
// as the platform SDK spells them - the SDK definitions have to keep winning. Off Windows
// there is no SDK to agree with, and the LLP64 widths are what the file formats assume.
#ifdef _WIN32
typedef unsigned long   DWORD;
typedef unsigned long   ULONG;
#else
typedef uint32_t        DWORD;
typedef uint32_t        ULONG;
#endif
typedef unsigned short	WORD;
typedef unsigned char   BYTE;
typedef int             BOOL;
typedef unsigned short	USHORT;
typedef const char *		LPCSTR;
typedef unsigned int    UINT;

#if defined(_MSC_VER) && _MSC_VER < 1300
#ifndef _WCHAR_T_DEFINED
typedef unsigned short wchar_t;
#define _WCHAR_T_DEFINED
#endif
#endif

// TheSuperHackers @port The on-disk formats depend on these widths. Enforce them here so a
// platform whose types differ fails to build rather than silently misreading every asset.
STATIC_ASSERT_ALWAYS(sizeof(uint8) == 1, "uint8 must be 1 byte");
STATIC_ASSERT_ALWAYS(sizeof(sint8) == 1, "sint8 must be 1 byte");
STATIC_ASSERT_ALWAYS(sizeof(uint16) == 2, "uint16 must be 2 bytes");
STATIC_ASSERT_ALWAYS(sizeof(sint16) == 2, "sint16 must be 2 bytes");
STATIC_ASSERT_ALWAYS(sizeof(uint32) == 4, "uint32 must be 4 bytes");
STATIC_ASSERT_ALWAYS(sizeof(sint32) == 4, "sint32 must be 4 bytes");
STATIC_ASSERT_ALWAYS(sizeof(float32) == 4, "float32 must be 4 bytes");
STATIC_ASSERT_ALWAYS(sizeof(float64) == 8, "float64 must be 8 bytes");
STATIC_ASSERT_ALWAYS(sizeof(DWORD) == 4, "DWORD must be 4 bytes");
STATIC_ASSERT_ALWAYS(sizeof(ULONG) == 4, "ULONG must be 4 bytes");
STATIC_ASSERT_ALWAYS(sizeof(WORD) == 2, "WORD must be 2 bytes");
STATIC_ASSERT_ALWAYS(sizeof(BYTE) == 1, "BYTE must be 1 byte");
