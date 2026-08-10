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

/***********************************************************************************************
 ***              C O N F I D E N T I A L  ---  W E S T W O O D  S T U D I O S               ***
 ***********************************************************************************************
 *                                                                                             *
 *                 Project Name : WWLib                                                        *
 *                                                                                             *
 *                     $Archive:: /G/wwlib/iostruct.h                                         $*
 *                                                                                             *
 *                       Author:: Greg Hjelstrom                                               *
 *                                                                                             *
 *                     $Modtime:: 4/02/99 11:59a                                              $*
 *                                                                                             *
 *                    $Revision:: 3                                                           $*
 *                                                                                             *
 *---------------------------------------------------------------------------------------------*
 * Functions:                                                                                  *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#pragma once

#include "bittype.h"
#include <stddef.h>

/*
** Some useful structures for writing/writing (safe from changes).
** The chunk IO classes contain code for reading and writing these.
*/
struct IOVector2Struct
{
	float32		X;
	float32		Y;
};

struct IOVector3Struct
{
	float32		X;							// X,Y,Z coordinates
	float32		Y;
	float32		Z;
};

struct IOVector4Struct
{
	float32		X;
	float32		Y;
	float32		Z;
	float32		W;
};

struct IOQuaternionStruct
{
	float32		Q[4];
};

// TheSuperHackers @port These structures are read from and written to disk as raw blobs.
// Their layout is part of the file format, so it is asserted rather than assumed.
STATIC_ASSERT_ALWAYS(sizeof(IOVector2Struct) == 8, "IOVector2Struct must be 8 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(IOVector2Struct, X) == 0, "IOVector2Struct::X must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(IOVector2Struct, Y) == 4, "IOVector2Struct::Y must be at offset 4");

STATIC_ASSERT_ALWAYS(sizeof(IOVector3Struct) == 12, "IOVector3Struct must be 12 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(IOVector3Struct, X) == 0, "IOVector3Struct::X must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(IOVector3Struct, Y) == 4, "IOVector3Struct::Y must be at offset 4");
STATIC_ASSERT_ALWAYS(offsetof(IOVector3Struct, Z) == 8, "IOVector3Struct::Z must be at offset 8");

STATIC_ASSERT_ALWAYS(sizeof(IOVector4Struct) == 16, "IOVector4Struct must be 16 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(IOVector4Struct, X) == 0, "IOVector4Struct::X must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(IOVector4Struct, Y) == 4, "IOVector4Struct::Y must be at offset 4");
STATIC_ASSERT_ALWAYS(offsetof(IOVector4Struct, Z) == 8, "IOVector4Struct::Z must be at offset 8");
STATIC_ASSERT_ALWAYS(offsetof(IOVector4Struct, W) == 12, "IOVector4Struct::W must be at offset 12");

STATIC_ASSERT_ALWAYS(sizeof(IOQuaternionStruct) == 16, "IOQuaternionStruct must be 16 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(IOQuaternionStruct, Q) == 0, "IOQuaternionStruct::Q must be at offset 0");
