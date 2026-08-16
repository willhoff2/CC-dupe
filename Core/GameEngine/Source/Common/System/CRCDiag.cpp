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

// FILE: CRCDiag.cpp //////////////////////////////////////////////////////////////////////////////
// Desc:   Opt-in decomposition of the light CRC. See CRCDiag.h and docs/porting/crc-divergence.md.
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/CRCDiag.h"
#include "GameLogic/GameLogic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace
{

enum DiagState
{
	DIAG_UNCHECKED = 0,
	DIAG_OFF,
	DIAG_ON
};

DiagState s_state = DIAG_UNCHECKED;
FILE *s_file = nullptr;
Bool s_continue = FALSE;
Int s_bytesFirstFrame = -1;
Int s_bytesLastFrame = -1;

// The environment is read once, so that a run cannot change its own logging halfway through.
void checkEnvironment()
{
	if( s_state != DIAG_UNCHECKED )
		return;

	s_state = DIAG_OFF;

	const char *path = getenv( "CNC_CRC_DIAG" );
	if( path == nullptr || path[0] == 0 )
		return;

	s_file = fopen( path, "w" );
	if( s_file == nullptr )
		return;

	s_state = DIAG_ON;

	const char *cont = getenv( "CNC_CRC_DIAG_CONTINUE" );
	s_continue = ( cont != nullptr && cont[0] != 0 );

	const char *bytes = getenv( "CNC_CRC_DIAG_BYTES" );
	if( bytes != nullptr && bytes[0] != 0 )
	{
		s_bytesFirstFrame = atoi( bytes );
		const char *colon = strchr( bytes, ':' );
		s_bytesLastFrame = ( colon != nullptr ) ? atoi( colon + 1 ) : s_bytesFirstFrame;
	}
}

UnsignedInt currentFrame()
{
	return ( TheGameLogic != nullptr ) ? TheGameLogic->getFrame() : 0;
}

} // namespace

namespace CRCDiag
{

Bool isEnabled()
{
	checkEnvironment();
	return s_state == DIAG_ON;
}

Bool continuesPastMismatch()
{
	return isEnabled() && s_continue;
}

Bool dumpsBytes()
{
	if( !isEnabled() || s_bytesFirstFrame < 0 )
		return FALSE;

	const Int frame = (Int)currentFrame();
	return frame >= s_bytesFirstFrame && frame <= s_bytesLastFrame;
}

void logMarker( const char *name, Int depth, UnsignedInt crc )
{
	if( !isEnabled() )
		return;

	fprintf( s_file, "F %u MARKER %d %s %8.8X\n", currentFrame(), depth, name, crc );
}

void logObject( UnsignedInt objectID, const char *templateName, UnsignedInt crc )
{
	if( !isEnabled() )
		return;

	fprintf( s_file, "F %u OBJECT %u %s %8.8X\n", currentFrame(), objectID,
		( templateName != nullptr ) ? templateName : "?", crc );
}

void logFrameTotal( UnsignedInt crc )
{
	if( !isEnabled() )
		return;

	fprintf( s_file, "F %u TOTAL %8.8X\n", currentFrame(), crc );
	fflush( s_file );
}

void logBytes( const void *data, Int dataSize, UnsignedInt crc )
{
	if( !isEnabled() )
		return;

	fprintf( s_file, "F %u BYTES %d", currentFrame(), dataSize );

	const unsigned char *bytes = (const unsigned char *)data;
	for( Int i = 0; i < dataSize; ++i )
		fprintf( s_file, " %2.2X", bytes[ i ] );

	fprintf( s_file, " %8.8X\n", crc );
}

void logCompare( UnsignedInt frame, UnsignedInt localCRC, UnsignedInt replayCRC )
{
	if( !isEnabled() )
		return;

	fprintf( s_file, "C %u LOCAL %8.8X REPLAY %8.8X %s\n", frame, localCRC, replayCRC,
		( localCRC == replayCRC ) ? "MATCH" : "MISMATCH" );
	fflush( s_file );
}

} // namespace CRCDiag
