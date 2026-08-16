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

// D3DXGetFVFVertexSize() off Windows, where on Windows it comes from d3dx8.lib.
//
// The size of a flexible vertex format is a pure function of its bits, and it is the stride every
// DrawPrimitiveUP/vertex-buffer call in the renderer is made with: too small and the device reads
// past the end of each vertex, too large and every vertex after the first is read from the wrong
// offset. Neither shows up as a compile or link error, which is why
// scripts/native-d3dx8fvf-test.py table-tests this against the documented layout and against
// FVFInfoClass, whose member offsets are computed from the same bits in dx8fvf.cpp and must add up
// to what this returns.
//
// The layout, from the D3D8 documentation of the D3DFVF_* bits:
//
//   * Position, from D3DFVF_POSITION_MASK: XYZ is three floats, XYZRHW is four, and XYZBn is three
//     plus n blend weights of four bytes each. D3DFVF_LASTBETA_UBYTE4 makes the last of those
//     weights four packed bytes rather than a float, which is the same four bytes, so it does not
//     change the size -- it is a decision about how the device reads the last weight, not how much
//     room it takes.
//   * D3DFVF_NORMAL is three floats, D3DFVF_PSIZE one float, and D3DFVF_DIFFUSE and
//     D3DFVF_SPECULAR one packed D3DCOLOR each.
//   * The texture coordinate *count* is (FVF & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT, and
//     each of those sets has its own dimension in two bits of the high half of the word:
//     D3DFVF_TEXCOORDSIZE2(i) is the zero default, so an FVF that names D3DFVF_TEX2 and nothing
//     else is two sets of two floats. Dimension bits above the count are ignored, exactly as they
//     are by the device.

#if !defined(_WIN32)

// The vendored d3dx8core.h is the declaration this has to match, and it pulls in d3d8types.h's
// D3DFVF_* bits through its own include chain.
#include <d3dx8core.h>

UINT WINAPI D3DXGetFVFVertexSize(DWORD FVF)
{
	UINT size = 0;

	switch (FVF & D3DFVF_POSITION_MASK) {
		case D3DFVF_XYZ:		size += 3 * sizeof(float); break;
		case D3DFVF_XYZRHW:	size += 4 * sizeof(float); break;
		case D3DFVF_XYZB1:	size += 3 * sizeof(float) + 1 * sizeof(DWORD); break;
		case D3DFVF_XYZB2:	size += 3 * sizeof(float) + 2 * sizeof(DWORD); break;
		case D3DFVF_XYZB3:	size += 3 * sizeof(float) + 3 * sizeof(DWORD); break;
		case D3DFVF_XYZB4:	size += 3 * sizeof(float) + 4 * sizeof(DWORD); break;
		case D3DFVF_XYZB5:	size += 3 * sizeof(float) + 5 * sizeof(DWORD); break;
		default: break;
	}

	if ((FVF & D3DFVF_NORMAL) != 0) size += 3 * sizeof(float);
	if ((FVF & D3DFVF_PSIZE) != 0) size += sizeof(float);
	if ((FVF & D3DFVF_DIFFUSE) != 0) size += sizeof(D3DCOLOR);
	if ((FVF & D3DFVF_SPECULAR) != 0) size += sizeof(D3DCOLOR);

	const unsigned texture_sets = (FVF & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
	for (unsigned set = 0; set < texture_sets; ++set) {
		// Two bits per set, starting at bit 16. The four values are the D3DFVF_TEXTUREFORMAT*
		// constants, whose numeric order is 2, 3, 4, 1 rather than 1, 2, 3, 4.
		const DWORD dimension_bits = (FVF >> (set * 2 + 16)) & 0x3;
		switch (dimension_bits) {
			case D3DFVF_TEXTUREFORMAT1: size += 1 * sizeof(float); break;
			case D3DFVF_TEXTUREFORMAT2: size += 2 * sizeof(float); break;
			case D3DFVF_TEXTUREFORMAT3: size += 3 * sizeof(float); break;
			case D3DFVF_TEXTUREFORMAT4: size += 4 * sizeof(float); break;
			default: break;
		}
	}

	return size;
}

#endif // !_WIN32
