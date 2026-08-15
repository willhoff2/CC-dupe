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

// The pixel half of the D3DX 8 texture utilities the engine names, exposed so it can be tested
// without a device. See d3dx8texture.cpp for what is and is not reproduced; the whole file is
// non-Windows, because on Windows these entry points come from d3dx8.lib.

#pragma once

#if !defined(_WIN32)

// Pulls the D3DFORMAT/D3DCOLOR types and the D3DX_FILTER_* flags in through the vendored chain,
// which is also the declaration D3DXLoadSurfaceFromSurface() has to match.
#include <d3dx8tex.h>

namespace D3DX8Texture
{

//
//	A locked surface: what D3DXLoadSurfaceFromSurface() has after LockRect(), and the only thing
//	the filtering itself needs. Pitch is in bytes and may exceed Width * bytes per pixel.
//
struct SurfaceView
{
	void *		Bits;
	int			Pitch;
	unsigned		Width;
	unsigned		Height;
	D3DFORMAT	Format;
};

//
//	The uncompressed formats this implementation can read and write. Everything else -- the DXT
//	blocks, the bump/luminance pairs, the depth formats -- makes the entry points fail rather
//	than silently produce a wrong picture.
//
bool Format_Is_Supported(D3DFORMAT format);
unsigned Bytes_Per_Pixel(D3DFORMAT format);

//
//	One pixel, widened to (or narrowed from) the D3DCOLOR A8R8G8B8 layout every conversion here
//	goes through. Channels a format does not carry read back as 0xff for alpha and 0x00 for
//	colour, which is what D3DX does: a surface with no alpha is opaque.
//
D3DCOLOR Read_Pixel(const SurfaceView & view, unsigned x, unsigned y);
void Write_Pixel(const SurfaceView & view, unsigned x, unsigned y, D3DCOLOR argb);

//
//	Stretch `source` onto `destination`, converting format, honouring the D3DX_FILTER_* filter in
//	`filter` (only NONE, POINT and BOX are implemented) and treating source pixels equal to
//	`colour_key` as transparent black when `colour_key` is non-zero. False means the arguments name
//	something this cannot do, and the destination is left untouched.
//
bool Blit(const SurfaceView & destination, const SurfaceView & source, DWORD filter,
	D3DCOLOR colour_key);

} // namespace D3DX8Texture

#endif // !_WIN32
