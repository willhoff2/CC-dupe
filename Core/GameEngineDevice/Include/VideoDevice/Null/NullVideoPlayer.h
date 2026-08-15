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

//////// NullVideoPlayer.h ///////////////////////////
// The video player used when a build has no video backend at all.
/////////////////////////////////////////////////

#pragma once

#include "GameClient/VideoPlayer.h"
#include "Common/Debug.h"

//===============================
// NullVideoPlayer
//===============================
/**
	*	The video backend of last resort: the Bink SDK is Windows-only and RTS_HAS_FFMPEG is off,
	* so there is nothing that can decode a movie. It complains once per attempt and opens
	* nothing, rather than pretending to decode. Not on the path to running the game: the port's
	* video route is RTS_HAS_FFMPEG. See docs/porting/video-and-harness-headers.md.
	*/
//===============================

class NullVideoPlayer : public VideoPlayer
{
	public:

		virtual void	init() override
		{
			VideoPlayer::init();
			DEBUG_CRASH(("NullVideoPlayer: this build has no video backend (neither the Bink SDK "
				"nor RTS_HAS_FFMPEG); movies will not play"));
		}

		virtual VideoStreamInterface*	open( AsciiString movieTitle ) override
		{
			DEBUG_CRASH(("NullVideoPlayer::open('%s'): no video backend in this build",
				movieTitle.str()));
			return NULL;
		}

		virtual VideoStreamInterface*	load( AsciiString movieTitle ) override
		{
			DEBUG_CRASH(("NullVideoPlayer::load('%s'): no video backend in this build",
				movieTitle.str()));
			return NULL;
		}
};
