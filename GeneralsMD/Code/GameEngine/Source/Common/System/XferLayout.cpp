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

// FILE: XferLayout.cpp ///////////////////////////////////////////////////////////////////////////
// Desc:   Compile-time ledger of every type that Xfer writes to a save game or a replay as a raw
//         block of bytes, i.e. through xferUser() rather than through a scalar xferInt() and
//         friends.
//
// TheSuperHackers @port A raw block is a hole in the file format: whatever the compiler decides
// the type looks like today is what lands on disk. All of these were measured to be the same
// size under Win32 and under LP64 -- they are built from Int, Real, UnsignedInt and 32-bit enums
// -- so the format survives the move to 64 bits, and these assertions are what keep it that way.
// A member that becomes a pointer, a long, a size_t or a time_t will fail the build here rather
// than silently write a different save file.
//
// This is a ledger, not a policy: it deliberately contains no code. See
// docs/porting/xfer-64bit-audit.md for the measurement, for the raw blocks that are *not* listed
// here, and for why.
///////////////////////////////////////////////////////////////////////////////////////////////////

// USER INCLUDES //////////////////////////////////////////////////////////////////////////////////
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/SerializedDateTime.h"
#include "GameClient/ClientRandomValue.h"
#include "GameClient/Display.h"
#include "GameClient/RadiusDecal.h"
#include "WWMath/matrix3d.h"
#include "WWMath/vector3.h"

#include <Utility/CppMacros.h>

// ------------------------------------------------------------------------------------------------
// Geometry and colour, written raw by dozens of module xfer() implementations.
// ------------------------------------------------------------------------------------------------
STATIC_ASSERT_ALWAYS(sizeof(Coord2D) == 8, "Coord2D is a raw save/replay record");
STATIC_ASSERT_ALWAYS(sizeof(Coord3D) == 12, "Coord3D is a raw save/replay record");
STATIC_ASSERT_ALWAYS(sizeof(ICoord2D) == 8, "ICoord2D is a raw save/replay record");
STATIC_ASSERT_ALWAYS(sizeof(ICoord3D) == 12, "ICoord3D is a raw save/replay record");
STATIC_ASSERT_ALWAYS(sizeof(IRegion2D) == 16, "IRegion2D is a raw save/replay record");
STATIC_ASSERT_ALWAYS(sizeof(Region2D) == 16, "Region2D is a raw save/replay record");
STATIC_ASSERT_ALWAYS(sizeof(RGBColor) == 12, "RGBColor is a raw save/replay record");
STATIC_ASSERT_ALWAYS(sizeof(Vector3) == 12, "Vector3 is a raw save/replay record");
STATIC_ASSERT_ALWAYS(sizeof(Matrix3D) == 48, "Matrix3D is a raw save/replay record");

// ------------------------------------------------------------------------------------------------
// Client state that the deep CRC and the save game both walk.
// ------------------------------------------------------------------------------------------------
STATIC_ASSERT_ALWAYS(sizeof(ShroudLevel) == 4, "ShroudLevel is a raw save/replay record");
STATIC_ASSERT_ALWAYS(sizeof(GameClientRandomVariable) == 12, "GameClientRandomVariable is a raw save/replay record");

// ------------------------------------------------------------------------------------------------
// The replay header timestamp. This one used to be a Win32 SYSTEMTIME; the record is asserted
// field by field in SerializedDateTime.h.
// ------------------------------------------------------------------------------------------------
STATIC_ASSERT_ALWAYS(sizeof(SerializedDateTime) == 16, "The replay header timestamp is sixteen bytes");

// ------------------------------------------------------------------------------------------------
// A counter-example, recorded here on purpose. RadiusDecal holds two pointers, so it is twelve
// bytes on Win32 and twenty-four under LP64. The one place that writes it raw is the version 1
// save path in SpectreGunshipUpdate::xfer(), which now writes a literal twelve bytes and does not
// depend on this layout. Nothing else may start writing one.
// ------------------------------------------------------------------------------------------------
STATIC_ASSERT_ALWAYS(sizeof(void*) != 4 || sizeof(RadiusDecal) == 12, "A version 1 RadiusDecal record is twelve bytes");

// ------------------------------------------------------------------------------------------------
// Not here, and why: DozerAIUpdate::DozerTaskInfo (8 bytes) and ProductionUpdate::DoorInfo
// (16 bytes) are also written raw, and both are LP64-stable, but they are private nested types of
// their module classes and cannot be named from outside. They need an in-class assertion instead.
// ------------------------------------------------------------------------------------------------
