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
 *                                                                                             *
 *  Table test for D3DXGetFVFVertexSize(), which WW3D2/d3dx8fvf.cpp implements off Windows.     *
 *                                                                                             *
 *  This function is a pure function of a bitmask with exactly one right answer per mask, and   *
 *  the answer is the STRIDE the renderer hands to every DrawPrimitive call: too small and the  *
 *  device reads past each vertex, too large and every vertex after the first comes from the     *
 *  wrong offset. Nothing about that is visible to a compiler or a linker.                      *
 *                                                                                             *
 *  Two independent oracles, because agreeing with itself would prove nothing:                  *
 *                                                                                             *
 *    1. The documented D3D8 layout, written out per FVF as a sum of its components, including   *
 *       every FVF the engine actually declares in dx8fvf.h and the vertex structs whose         *
 *       sizeof() has to match them.                                                            *
 *    2. FVFInfoClass, in dx8fvf.cpp, which derives its member offsets from the same bits        *
 *       without calling this function. The last component's offset plus that component's size   *
 *       must be the vertex size -- so the two readings of the bitmask are cross-checked against *
 *       each other, which is what would catch a disagreement about D3DFVF_LASTBETA_UBYTE4 or    *
 *       about a texture coordinate set's dimension.                                            *
 *                                                                                             *
 *  Run through scripts/native-d3dx8fvf-test.py. Builds on Windows too, against d3dx8.lib.      *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#include "dx8fvf.h"

#include <d3dx8core.h>

#include <stdio.h>

static int _Failures = 0;
static int _Checks = 0;

static void Check_Size(unsigned fvf, unsigned expected, const char * what)
{
	_Checks++;
	const unsigned actual = D3DXGetFVFVertexSize(fvf);
	if (actual != expected) {
		_Failures++;
		printf("FAIL: %s: D3DXGetFVFVertexSize(0x%08x) is %u, expected %u\n", what, fvf, actual,
			expected);
	}
}

static void Check(bool condition, const char * what)
{
	_Checks++;
	if (!condition) {
		_Failures++;
		printf("FAIL: %s\n", what);
	}
}

static const unsigned FLOAT_SIZE = unsigned(sizeof(float));
static const unsigned COLOUR_SIZE = unsigned(sizeof(DWORD));


/***********************************************************************************************
 *  The position field, which is the only part of an FVF that is an enumeration rather than a    *
 *  set of independent flags.                                                                   *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
static void Test_Position()
{
	Check_Size(0, 0, "an empty FVF has no size");
	Check_Size(D3DFVF_XYZ, 3 * FLOAT_SIZE, "XYZ is three floats");
	Check_Size(D3DFVF_XYZRHW, 4 * FLOAT_SIZE, "XYZRHW is four floats");
	Check_Size(D3DFVF_XYZB1, 4 * FLOAT_SIZE, "XYZB1 is three floats and one weight");
	Check_Size(D3DFVF_XYZB2, 5 * FLOAT_SIZE, "XYZB2 is three floats and two weights");
	Check_Size(D3DFVF_XYZB3, 6 * FLOAT_SIZE, "XYZB3 is three floats and three weights");
	Check_Size(D3DFVF_XYZB4, 7 * FLOAT_SIZE, "XYZB4 is three floats and four weights");
	Check_Size(D3DFVF_XYZB5, 8 * FLOAT_SIZE, "XYZB5 is three floats and five weights");

	/*
	**	D3DFVF_LASTBETA_UBYTE4 packs the last weight as four bytes instead of a float. Four bytes
	**	either way, so the size does not move -- and dx8fvf.cpp's own offsets assume exactly that.
	*/
	Check_Size(D3DFVF_XYZB4 | D3DFVF_LASTBETA_UBYTE4, 7 * FLOAT_SIZE,
		"LASTBETA_UBYTE4 packs the last weight without changing the size");
	Check_Size(D3DFVF_XYZB1 | D3DFVF_LASTBETA_UBYTE4, 4 * FLOAT_SIZE,
		"LASTBETA_UBYTE4 with one weight is still four bytes");

	// The reserved bits are not part of the size.
	Check_Size(D3DFVF_XYZ | D3DFVF_RESERVED0, 3 * FLOAT_SIZE, "RESERVED0 adds nothing");
	Check_Size(D3DFVF_XYZ | D3DFVF_RESERVED2, 3 * FLOAT_SIZE, "RESERVED2 adds nothing");
}


/***********************************************************************************************
 *  The independent component flags.                                                            *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
static void Test_Components()
{
	Check_Size(D3DFVF_XYZ | D3DFVF_NORMAL, 6 * FLOAT_SIZE, "a normal is three floats");
	Check_Size(D3DFVF_XYZ | D3DFVF_PSIZE, 4 * FLOAT_SIZE, "a point size is one float");
	Check_Size(D3DFVF_XYZ | D3DFVF_DIFFUSE, 3 * FLOAT_SIZE + COLOUR_SIZE,
		"a diffuse colour is one packed D3DCOLOR");
	Check_Size(D3DFVF_XYZ | D3DFVF_SPECULAR, 3 * FLOAT_SIZE + COLOUR_SIZE,
		"a specular colour is one packed D3DCOLOR");
	Check_Size(D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_SPECULAR, 3 * FLOAT_SIZE + 2 * COLOUR_SIZE,
		"both colours are two packed D3DCOLORs");
	Check_Size(D3DFVF_NORMAL, 3 * FLOAT_SIZE, "a normal with no position is still counted");
}


/***********************************************************************************************
 *  Texture coordinate sets: a count in bits 8-11 and a dimension per set in the high half.      *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
static void Test_Texture_Coordinates()
{
	Check_Size(D3DFVF_XYZ | D3DFVF_TEX0, 3 * FLOAT_SIZE, "TEX0 is no texture coordinates");
	Check_Size(D3DFVF_XYZ | D3DFVF_TEX1, 5 * FLOAT_SIZE, "an unqualified set is two floats");
	Check_Size(D3DFVF_XYZ | D3DFVF_TEX2, 7 * FLOAT_SIZE, "two unqualified sets are four floats");
	Check_Size(D3DFVF_XYZ | D3DFVF_TEX8, 3 * FLOAT_SIZE + 16 * FLOAT_SIZE,
		"eight unqualified sets are sixteen floats");

	Check_Size(D3DFVF_XYZ | D3DFVF_TEX1 | D3DFVF_TEXCOORDSIZE1(0), 4 * FLOAT_SIZE,
		"TEXCOORDSIZE1 is one float");
	Check_Size(D3DFVF_XYZ | D3DFVF_TEX1 | D3DFVF_TEXCOORDSIZE2(0), 5 * FLOAT_SIZE,
		"TEXCOORDSIZE2 is the zero default");
	Check_Size(D3DFVF_XYZ | D3DFVF_TEX1 | D3DFVF_TEXCOORDSIZE3(0), 6 * FLOAT_SIZE,
		"TEXCOORDSIZE3 is three floats");
	Check_Size(D3DFVF_XYZ | D3DFVF_TEX1 | D3DFVF_TEXCOORDSIZE4(0), 7 * FLOAT_SIZE,
		"TEXCOORDSIZE4 is four floats");

	// Per-set dimensions, mixed, and in a set other than the first.
	Check_Size(D3DFVF_XYZ | D3DFVF_TEX2 | D3DFVF_TEXCOORDSIZE4(1), 3 * FLOAT_SIZE + 6 * FLOAT_SIZE,
		"the second set has its own dimension");
	Check_Size(D3DFVF_XYZ | D3DFVF_TEX3 | D3DFVF_TEXCOORDSIZE1(0) | D3DFVF_TEXCOORDSIZE4(1)
		| D3DFVF_TEXCOORDSIZE3(2), 3 * FLOAT_SIZE + 8 * FLOAT_SIZE,
		"three sets of 1, 4 and 3 floats");

	/*
	**	Dimension bits above the declared count are ignored: the count decides how many sets the
	**	device reads. An implementation that walked the dimension bits instead would add a set
	**	that is not there.
	*/
	Check_Size(D3DFVF_XYZ | D3DFVF_TEX1 | D3DFVF_TEXCOORDSIZE4(3), 5 * FLOAT_SIZE,
		"a dimension above the count is ignored");
}


/***********************************************************************************************
 *  The FVFs this engine actually declares, against the vertex structures it casts buffers to.   *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
static void Test_Engine_Formats()
{
	Check_Size(DX8_FVF_XYZ, unsigned(sizeof(VertexFormatXYZ)), "DX8_FVF_XYZ matches its struct");
	Check_Size(DX8_FVF_XYZN, unsigned(sizeof(VertexFormatXYZN)),
		"DX8_FVF_XYZN matches its struct");
	Check_Size(DX8_FVF_XYZNUV1, unsigned(sizeof(VertexFormatXYZNUV1)),
		"DX8_FVF_XYZNUV1 matches its struct");
	Check_Size(DX8_FVF_XYZNUV2, unsigned(sizeof(VertexFormatXYZNUV2)),
		"DX8_FVF_XYZNUV2 matches its struct");
	Check_Size(DX8_FVF_XYZNDUV1, unsigned(sizeof(VertexFormatXYZNDUV1)),
		"DX8_FVF_XYZNDUV1 matches its struct");
	Check_Size(DX8_FVF_XYZNDUV2, unsigned(sizeof(VertexFormatXYZNDUV2)),
		"DX8_FVF_XYZNDUV2 matches its struct");
	Check_Size(DX8_FVF_XYZDUV1, unsigned(sizeof(VertexFormatXYZDUV1)),
		"DX8_FVF_XYZDUV1 matches its struct");
	Check_Size(DX8_FVF_XYZDUV2, unsigned(sizeof(VertexFormatXYZDUV2)),
		"DX8_FVF_XYZDUV2 matches its struct");
	Check_Size(DX8_FVF_XYZUV1, unsigned(sizeof(VertexFormatXYZUV1)),
		"DX8_FVF_XYZUV1 matches its struct");
	Check_Size(DX8_FVF_XYZUV2, unsigned(sizeof(VertexFormatXYZUV2)),
		"DX8_FVF_XYZUV2 matches its struct");

	// Position, normal, diffuse and four sets of 2, 3, 3, 3 floats: the tangent-space format.
	Check_Size(DX8_FVF_XYZNDUV1TG3, 6 * FLOAT_SIZE + COLOUR_SIZE + 11 * FLOAT_SIZE,
		"DX8_FVF_XYZNDUV1TG3 is 2+3+3+3 texture floats");

	// Position, normal and three sets of 1, 4 and 2 floats: the displacement-mapping format.
	Check_Size(DX8_FVF_XYZNUV2DMAP, 6 * FLOAT_SIZE + 7 * FLOAT_SIZE,
		"DX8_FVF_XYZNUV2DMAP is 1+4+2 texture floats");

	Check_Size(DX8_FVF_XYZNDCUBEMAP, 6 * FLOAT_SIZE + COLOUR_SIZE,
		"DX8_FVF_XYZNDCUBEMAP has its texture coordinates commented out");
}


/***********************************************************************************************
 *  The second oracle: FVFInfoClass in dx8fvf.cpp, which reads the same bits to place its        *
 *  members and takes only its total from this function.                                         *
 *                                                                                              *
 *  The texture coordinate sets are last in a vertex, so the offset FVFInfoClass computes for     *
 *  the set one past the declared count is where it thinks the vertex ends, and for a correct     *
 *  reading of the bits that is exactly the vertex size.                                         *
 *                                                                                              *
 *  Where every texture coordinate set is 2-dimensional -- which is every FVF the engine draws     *
 *  with -- they agree exactly, and that is asserted below.                                       *
 *                                                                                              *
 *  Where any set declares a dimension they do NOT agree, and the fault is dx8fvf.cpp's, present  *
 *  identically on Windows, so this test pins the disagreement rather than hiding it. Two         *
 *  independent defects combine:                                                                  *
 *                                                                                              *
 *   1. D3DFVF_TEXCOORDSIZE1(CoordIndex) expands to                                              *
 *      `(D3DFVF_TEXTUREFORMAT1 << (CoordIndex*2 + 16))` -- with no parentheses around            *
 *      CoordIndex. dx8fvf.cpp passes the expression `i-1`, so the shift is `i - 1*2 + 16`,       *
 *      that is `i + 14`, not `2*(i-1) + 16`. The mask it tests is in the wrong place for every    *
 *      set but the second.                                                                       *
 *   2. D3DFVF_TEXCOORDSIZE2(CoordIndex) is *zero* -- two floats is the default encoding rather   *
 *      than a bit pattern -- so `(FVF & 0) == 0` is true for every FVF and the second branch of  *
 *      the chain fires whenever the first did not. No set is ever advanced by three or four       *
 *      floats.                                                                                   *
 *                                                                                              *
 *  So FVFInfoClass::Get_Tex_Offset() is only right for formats built out of 2-dimensional sets,  *
 *  and DX8_FVF_XYZNDUV1TG3 and DX8_FVF_XYZNUV2DMAP -- the tangent-space and displacement-map     *
 *  formats -- have offsets that point at the wrong floats. D3DXGetFVFVertexSize() must not copy  *
 *  either defect: the stride it returns is what the *device* reads, and the device reads the      *
 *  bits as documented. Both readings are therefore transcribed here and asserted separately.     *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

// Where dx8fvf.cpp's else-if chain, transcribed with its two defects intact, puts the end of the
// vertex. Only valid for the plain-D3DFVF_XYZ formats used below, which is all the engine has.
static unsigned FVFInfo_Vertex_End(unsigned fvf)
{
	unsigned offset = D3DXGetFVFVertexSize(fvf & ~unsigned(D3DFVF_TEXCOUNT_MASK));
	const unsigned sets = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
	for (unsigned i = 1; i <= sets; ++i) {
		const unsigned shift = i + 14;					// (i-1)*2 + 16 as the macro actually expands it
		const unsigned mask = unsigned(D3DFVF_TEXTUREFORMAT1) << shift;
		offset += ((fvf & mask) == mask) ? FLOAT_SIZE : 2 * FLOAT_SIZE;
	}
	return offset;
}


static void Check_Against_FVFInfo(unsigned fvf, const char * what)
{
	const FVFInfoClass info(fvf);
	const unsigned size = D3DXGetFVFVertexSize(fvf);

	_Checks++;
	if (info.Get_FVF_Size() != size) {
		_Failures++;
		printf("FAIL: %s: FVFInfoClass took its size from somewhere else (%u vs %u)\n", what,
			info.Get_FVF_Size(), size);
		return;
	}

	const unsigned sets = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
	if (sets >= D3DDP_MAXTEXCOORD) return;

	_Checks++;
	if (info.Get_Tex_Offset(sets) != FVFInfo_Vertex_End(fvf)) {
		_Failures++;
		printf("FAIL: %s: FVFInfoClass ends the vertex at %u; dx8fvf.cpp's arithmetic says %u. "
			"If dx8fvf.cpp's texture-coordinate offsets were fixed, this is the expected "
			"failure -- delete the exception, not the size.\n",
			what, info.Get_Tex_Offset(sets), FVFInfo_Vertex_End(fvf));
	}

	/*
	**	And the offsets themselves, in declaration order, which is what a wrong reading of the
	**	position field would move.
	*/
	_Checks++;
	if (!(info.Get_Location_Offset() == 0
			&& info.Get_Normal_Offset() <= info.Get_Diffuse_Offset()
			&& info.Get_Diffuse_Offset() <= info.Get_Specular_Offset()
			&& info.Get_Specular_Offset() <= info.Get_Tex_Offset(0)
			&& info.Get_Tex_Offset(0) <= size)) {
		_Failures++;
		printf("FAIL: %s: FVFInfoClass offsets are not in declaration order\n", what);
	}
}


// The formats whose sets are all 1- or 2-dimensional, where the two readings must agree exactly.
static void Check_Agrees_With_FVFInfo(unsigned fvf, const char * what)
{
	Check_Against_FVFInfo(fvf, what);

	const FVFInfoClass info(fvf);
	const unsigned sets = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
	if (sets >= D3DDP_MAXTEXCOORD) return;

	_Checks++;
	if (info.Get_Tex_Offset(sets) != D3DXGetFVFVertexSize(fvf)) {
		_Failures++;
		printf("FAIL: %s: FVFInfoClass ends the vertex at %u, the size says %u\n", what,
			info.Get_Tex_Offset(sets), D3DXGetFVFVertexSize(fvf));
	}
}


static void Test_Against_FVFInfoClass()
{
	Check_Agrees_With_FVFInfo(DX8_FVF_XYZ, "DX8_FVF_XYZ");
	Check_Agrees_With_FVFInfo(DX8_FVF_XYZN, "DX8_FVF_XYZN");
	Check_Agrees_With_FVFInfo(DX8_FVF_XYZNUV1, "DX8_FVF_XYZNUV1");
	Check_Agrees_With_FVFInfo(DX8_FVF_XYZNUV2, "DX8_FVF_XYZNUV2");
	Check_Agrees_With_FVFInfo(DX8_FVF_XYZNDUV1, "DX8_FVF_XYZNDUV1");
	Check_Agrees_With_FVFInfo(DX8_FVF_XYZNDUV2, "DX8_FVF_XYZNDUV2");
	Check_Agrees_With_FVFInfo(DX8_FVF_XYZDUV1, "DX8_FVF_XYZDUV1");
	Check_Agrees_With_FVFInfo(DX8_FVF_XYZDUV2, "DX8_FVF_XYZDUV2");
	Check_Agrees_With_FVFInfo(DX8_FVF_XYZUV1, "DX8_FVF_XYZUV1");
	Check_Agrees_With_FVFInfo(DX8_FVF_XYZUV2, "DX8_FVF_XYZUV2");
	Check_Agrees_With_FVFInfo(D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX2,
		"XYZ with both colours and two texture sets");

	/*
	**	And the formats with a declared dimension, where dx8fvf.cpp is wrong by construction: the
	**	size still comes from the bits, and Get_Tex_Offset() still matches dx8fvf.cpp's own
	**	arithmetic, which is the distinction that matters.
	*/
	Check_Against_FVFInfo(D3DFVF_XYZ | D3DFVF_TEX2 | D3DFVF_TEXCOORDSIZE1(0)
		| D3DFVF_TEXCOORDSIZE1(1), "two 1-dimensional texture sets");
	Check_Against_FVFInfo(DX8_FVF_XYZNDUV1TG3, "DX8_FVF_XYZNDUV1TG3");
	Check_Against_FVFInfo(DX8_FVF_XYZNUV2DMAP, "DX8_FVF_XYZNUV2DMAP");
	Check_Against_FVFInfo(D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX3 | D3DFVF_TEXCOORDSIZE1(0)
		| D3DFVF_TEXCOORDSIZE4(1) | D3DFVF_TEXCOORDSIZE3(2), "three mixed texture sets");

	_Checks++;
	if (FVFInfo_Vertex_End(DX8_FVF_XYZNDUV1TG3) == D3DXGetFVFVertexSize(DX8_FVF_XYZNDUV1TG3)) {
		_Failures++;
		printf("FAIL: dx8fvf.cpp's texture-coordinate offsets now agree with the FVF bits for "
			"DX8_FVF_XYZNDUV1TG3, which means the transcription above is stale\n");
	}
}


int main()
{
	Check(FLOAT_SIZE == 4, "a float is four bytes, which every FVF layout assumes");
	Check(COLOUR_SIZE == 4, "a D3DCOLOR is four bytes");

	Test_Position();
	Test_Components();
	Test_Texture_Coordinates();
	Test_Engine_Formats();
	Test_Against_FVFInfoClass();

	printf("%d checks, %d failure(s)\n", _Checks, _Failures);
	return (_Failures == 0) ? 0 : 1;
}
