/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 TheSuperHackers
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

// TheSuperHackers @port Accessors for the parts of the vendored Direct3D 8 headers whose layout
// depends on `_WIN32`.
//
// `_D3DADAPTER_IDENTIFIER8` is the only such part the engine reads. The vendored header
// (cmake/dx8.cmake pins TheSuperHackers/min-dx8-sdk) is the retail SDK header verbatim, and the
// SDK spells the field twice:
//
//     #ifdef _WIN32
//         LARGE_INTEGER   DriverVersion;        /* Defined for 32 bit components */
//     #else
//         DWORD           DriverVersionLowPart; /* Defined for 16 bit driver components */
//         DWORD           DriverVersionHighPart;
//     #endif
//
// That `#ifdef` is the SDK's own Win32-versus-Win16 split from 2001, not a portability guard: it
// asks "is this a 32 bit component?", and a 64 bit clang build - which deliberately does not
// define `_WIN32`, because in this codebase `_WIN32` means Windows - lands in the Win16 branch
// and sees two DWORDs. Nothing is missing from the vendored header and nothing needs to be added
// to it; both branches describe the same eight bytes, low DWORD first, which is exactly
// LARGE_INTEGER's layout on the little-endian targets the game ships on.
//
// `<d3d8types.h>` needs the Win32 base types, so off Windows this header defines nothing at all
// unless a `<windows.h>` is on the include path -- the same rule
// WWLib/platform/platform_win32_compat.h follows, and for the same reason: the probe's unshimmed
// mode measures what compiles with no Windows headers, and inventing Win32 typedefs here to keep
// it compiling would measure this header instead.
#pragma once

// Nested rather than one `||` expression, because VC6's preprocessor cannot parse the
// `__has_include(<windows.h>)` operand even when the left hand side rules it out.
#ifdef _WIN32
#define UTILITY_D3D8_COMPAT 1
#else
#if defined(__has_include)
#if __has_include(<windows.h>)
#define UTILITY_D3D8_COMPAT 1
#endif
#endif
#endif

#ifdef UTILITY_D3D8_COMPAT

#include <windows.h>
#include <d3d8types.h>

// The driver version as the single 64 bit number the engine compares
// (W3DShaderManager::getCurrentDriverVersion(), GameLODManager's tested-driver table).
inline __int64 D3D8AdapterDriverVersion(const D3DADAPTER_IDENTIFIER8 &identifier)
{
#ifdef _WIN32
	return identifier.DriverVersion.QuadPart;
#else
	return (static_cast<__int64>(identifier.DriverVersionHighPart) << 32)
		| static_cast<__int64>(static_cast<unsigned int>(identifier.DriverVersionLowPart));
#endif
}

#endif
