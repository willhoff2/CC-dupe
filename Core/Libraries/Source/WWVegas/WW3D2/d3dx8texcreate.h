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

// The decisions the D3DX 8 texture-creation helpers make before they call the device, separated
// from the device call so they can be asserted without one. See d3dx8texcreate.cpp for what each
// entry point does with them; the whole file is non-Windows, because on Windows these entry points
// come from d3dx8.lib.

#pragma once

#if !defined(_WIN32)

#include <d3d8.h>
#include <d3dx8tex.h>

class RenderBackendClass;

namespace D3DX8TexCreate
{

//
//	Where these helpers create when their device argument is null, which off Windows is every
//	call the engine makes: their device argument is DX8Wrapper::_Get_D3D_Device8(), and a backend
//	that is not D3D8 has no IDirect3DDevice8 to hand out (renderbackend.h documents that hole).
//	The device the engine actually has lives behind RenderBackendClass, so that is what is asked
//	for the caps and for the creation itself.
//
//	Defined in dx8wrapper.cpp, which owns the installed backend. It is a free function rather
//	than a direct DX8Wrapper::Get_Render_Backend() call so that the entry-point tests
//	(scripts/native-d3dx8-entrypoints-test.py) can link this file without the whole wrapper, and
//	so that they can say what backend, if any, is installed.
//
RenderBackendClass * Peek_Render_Backend();

//
//	Which of the three texture shapes is being created. The fitting rules differ: the POW2 and
//	SQUAREONLY caps apply to plain textures only, a cube texture has one edge length, and a volume
//	texture's three extents are bounded by MaxVolumeExtent rather than by MaxTextureWidth/Height.
//
enum KindType
{
	KIND_TEXTURE,
	KIND_CUBE,
	KIND_VOLUME,
};

//
//	What the caller asked for, in the units D3DX takes: D3DX_DEFAULT (or zero) means "you decide".
//
struct RequestType
{
	unsigned		Width;
	unsigned		Height;
	unsigned		Depth;
	unsigned		MipLevels;
	DWORD			Usage;
	D3DFORMAT	Format;
};

//
//	The formats to try, in order. The first is the one the caller asked for whenever that is a
//	real format; the rest are the substitutions D3DX's "closest supported format" behaviour is
//	allowed to make here, and they preserve whether the format carries alpha.
//
enum { MAX_FORMAT_CANDIDATES = 5 };

struct PlanType
{
	unsigned		Width;
	unsigned		Height;
	unsigned		Depth;
	unsigned		MipLevels;
	D3DFORMAT	Formats[MAX_FORMAT_CANDIDATES];
	unsigned		FormatCount;
};

//
//	Fit a request to a device's capabilities: substitute for D3DX_DEFAULT, round the extents up to
//	a power of two and to square when the caps demand it, clamp to the device's maxima, expand a
//	mip level count of zero/D3DX_DEFAULT into the full chain (or collapse it to 1 on a device with
//	no mip support), and list the formats to attempt.
//
PlanType Plan(const D3DCAPS8 & caps, const RequestType & request, KindType kind);

//
//	The number of levels in a complete mip chain down to 1x1(x1), which is what D3DX means by a
//	MipLevels of zero.
//
unsigned Full_Mip_Chain(unsigned width, unsigned height, unsigned depth);

//
//	Whether a failed creation should be retried with the next candidate format. Only a rejection
//	that means "not this format" is retried, and only when the texture is not a render target or
//	depth/stencil surface -- for those the caller inspects the HRESULT itself (dx8wrapper.cpp
//	treats D3DERR_NOTAVAILABLE as "no render target for you" and frees assets on
//	D3DERR_OUTOFVIDEOMEMORY), so swallowing it here would hide the answer it is waiting for.
//
bool Should_Try_Next_Format(HRESULT result, DWORD usage);

//
//	The text D3DXGetErrorString() reports for a D3D or D3DX HRESULT, or nullptr when the code is
//	not one this knows -- the caller then formats the numeric value.
//
const char * Error_String(HRESULT hr);

} // namespace D3DX8TexCreate

#endif // !_WIN32
