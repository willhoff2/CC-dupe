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

// TheSuperHackers @port The MSVC floating point control CRT, over <fenv.h>.
//
// setFPMode() (GameLogic.cpp) and SimulationMathCrc::calculate() call _fpreset(), _statusfp() and
// _controlfp() to put the x87 unit into a known rounding mode and 24-bit precision. Only part of
// that is expressible off Windows, and the difference matters, so it is spelled out here rather
// than hidden:
//
//   * rounding mode  -- portable. _MCW_RC maps onto fesetround()/fegetround() exactly.
//   * exception mask -- not mapped, and _MCW_EM is not even defined here. Every caller in this
//                       codebase passes _MCW_PC|_MCW_RC; a call site that wanted the exception
//                       mask should fail to compile rather than be quietly ignored.
//   * precision      -- NOT expressible, and not a no-op we can hide. _PC_24 asks the x87 unit to
//                       round every intermediate result to single precision. SSE2 (and NEON) have
//                       no equivalent control: on x86-64 and arm64 the width of an operation is a
//                       property of the instruction, not of a mode register. Requests are
//                       accepted and dropped. This is a *behavioural* difference from the Windows
//                       build, and it is the reason a native build cannot be assumed to produce
//                       bit-identical simulation results; see
//                       docs/porting/crt-and-widechar-compat.md.
#pragma once

#ifndef _WIN32

#include <fenv.h>

// The MSVC <float.h> bit layout, reproduced so that call sites which OR these together and hand
// the result back to _controlfp() keep working unchanged.
#ifndef _MCW_RC
#define _MCW_RC   0x00000300
#endif
#ifndef _MCW_PC
#define _MCW_PC   0x00030000
#endif

#ifndef _RC_NEAR
#define _RC_NEAR  0x00000000
#define _RC_DOWN  0x00000100
#define _RC_UP    0x00000200
#define _RC_CHOP  0x00000300
#endif

#ifndef _PC_64
#define _PC_64    0x00000000
#define _PC_53    0x00010000
#define _PC_24    0x00020000
#endif

namespace FPUCompat
{

inline unsigned int rounding_mode_to_mcw(int fe_mode)
{
	switch (fe_mode)
	{
		case FE_DOWNWARD:   return _RC_DOWN;
		case FE_UPWARD:     return _RC_UP;
		case FE_TOWARDZERO: return _RC_CHOP;
		default:            return _RC_NEAR;
	}
}

inline int mcw_to_rounding_mode(unsigned int mcw)
{
	switch (mcw & _MCW_RC)
	{
		case _RC_DOWN: return FE_DOWNWARD;
		case _RC_UP:   return FE_UPWARD;
		case _RC_CHOP: return FE_TOWARDZERO;
		default:       return FE_TONEAREST;
	}
}

}	// namespace FPUCompat

// Reset the floating point environment to the startup default, as _fpreset() does: rounding to
// nearest, all exceptions masked, status flags cleared.
inline void _fpreset()
{
	fesetenv(FE_DFL_ENV);
}

// The current control word, in MSVC's encoding. Only the rounding field is real; the precision
// field always reads back as _PC_64, which is what the hardware actually does.
inline unsigned int _statusfp()
{
	return FPUCompat::rounding_mode_to_mcw(fegetround()) | _PC_64;
}

inline unsigned int _controlfp(unsigned int newValue, unsigned int mask)
{
	if (mask & _MCW_RC)
	{
		fesetround(FPUCompat::mcw_to_rounding_mode(newValue));
	}
	// _MCW_PC is deliberately not honoured: see the note at the top of this file.
	return _statusfp();
}

#endif	// !_WIN32
