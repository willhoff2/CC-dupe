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

/***********************************************************************************************
 *                                                                                             *
 *                 Project Name : Westwood Library                                             *
 *                                                                                             *
 *  Decoder for the Win32 cursor files the game ships: Data\Cursors\*.ANI, which are RIFF        *
 *  "ACON" animations whose frames are ordinary .CUR files, and bare .CUR files. On Windows       *
 *  LoadCursorFromFile() reads these into an HCURSOR; off Windows nothing does, so this is the    *
 *  part of that call the OS used to own. It is pure byte decoding with no platform dependency,   *
 *  which is what lets it be unit tested against the retail set on a headless runner.             *
 *                                                                                             *
 *  What comes out is what a window backend needs to make a native cursor: the first frame as     *
 *  32-bit BGRA with straight alpha, rows top-down, and the hotspot in that frame's top-left      *
 *  origin pixel space. The frame count and rate are decoded and reported so a caller can see     *
 *  what it is not yet animating. See docs/porting/mouse-cursor-seam.md.                          *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#pragma once

#include <stddef.h>

#include <string>
#include <vector>

namespace WWPlatform
{

/*
**	One decoded cursor frame. Pixels are Width*Height*4 bytes, B G R A per pixel, top row first,
**	alpha straight (not premultiplied): 255 where the frame's AND mask is opaque, 0 where it is
**	transparent, and for a 32-bit frame the frame's own alpha channel.
*/
struct CursorFrame
{
	int Width;
	int Height;
	int Hotspot_X;
	int Hotspot_Y;
	int Bits_Per_Pixel;			// of the source image, before decoding to BGRA
	std::vector<unsigned char> Pixels_BGRA;

	CursorFrame() : Width(0), Height(0), Hotspot_X(0), Hotspot_Y(0), Bits_Per_Pixel(0) {}
};

/*
**	A decoded .ANI or .CUR. Frame_Count is the number of 'icon' chunks the file carries (1 for a
**	bare .CUR); Step_Count and Display_Rate_Jiffies are the animation header's nSteps and
**	iDispRate (a jiffy is 1/60 s), zero for a bare .CUR. Only the first frame is decoded.
*/
struct CursorFile
{
	int Frame_Count;
	int Step_Count;
	int Display_Rate_Jiffies;
	CursorFrame First;

	CursorFile() : Frame_Count(0), Step_Count(0), Display_Rate_Jiffies(0) {}
};

/*
**	Decode a .ANI (RIFF ACON) or a bare .CUR (ICONDIR type 2) from memory. Returns false with
**	`error` describing the first thing that did not parse; on success `error` is left alone.
**	A .ANI whose frames are raw bitmaps rather than embedded .CUR files (AF_ICON clear) is not
**	one the game ships and is rejected rather than guessed at.
*/
bool Cursor_Decode(const unsigned char * data, size_t size, CursorFile & out, std::string & error);

/*
**	Decode a bare .CUR / .ICO image (an ICONDIR with its directory entries and images). The
**	hotspot comes from the directory entry, which is where a type-2 (cursor) file keeps it; for
**	a type-1 (icon) file it is zero. Used by Cursor_Decode() for each embedded frame.
*/
bool Cursor_Decode_Ico(const unsigned char * data, size_t size, CursorFrame & out,
	std::string & error);

}	// namespace WWPlatform
