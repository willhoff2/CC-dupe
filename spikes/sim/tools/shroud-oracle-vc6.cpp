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

// shroud-oracle-vc6.cpp
//
// The Windows oracle for one question: does this port compute the same shroud cell as the Windows
// build for a river vertex authored into a map's border ring? It is not part of the game and not
// built by CMake -- it is compiled by the repo's own VC6 container, with the engine's own float
// conversions copied verbatim from Core/Libraries/Include/Lib/BaseType.h (fast_float_trunc and
// fast_float_floor, inline asm and all), so the answer comes from VC6's code generation rather than
// from an argument about it.
//
//   ./scripts/docker-build.sh --game zh          # once, to have the image
//   docker run -u "$(id -u):$(id -g)" -v "$PWD:/build/cnc" --rm --entrypoint bash zerohour-build \
//     -c 'cd /build/cnc/spikes/sim/tools && wineboot >/dev/null 2>&1;
//         wine cmd /c "set TMP=Z:\build\tmp& set TEMP=Z:\build\tmp&
//           Z:\build\tools\vs6\vc98\bin\cl.exe /nologo /Ox shroud-oracle-vc6.cpp";
//         wine shroud-oracle-vc6.exe'
//
// Compare its output with `sim_probe rivers` (the truncating conversion the water path shipped with)
// and `sim_probe shroudbounds` (the engine's flooring conversion, through W3DShroud itself). See
// docs/porting/shroud-river-water-bounds.md section 2.

#include <stdio.h>

typedef int Int;
typedef float Real;

// Verbatim from Core/Libraries/Include/Lib/BaseType.h; VC6 takes the _asm path there too.
__forceinline float fast_float_trunc(float f)
{
	_asm
	{
		mov ecx,[f]
		shr ecx,23
		mov eax,0xff800000
		xor ebx,ebx
		sub cl,127
		cmovc eax,ebx
		sar eax,cl
		and [f],eax
	}
	return f;
}

__forceinline float fast_float_floor(float f)
{
	static unsigned almost1=(126<<23)|0x7fffff;
	if (*(unsigned *)&f &0x80000000)
		f-=*(float *)&almost1;
	return fast_float_trunc(f);
}

int main()
{
	// The MD_USA01 'Water Area 5' vertices measured with `sim_probe rivers`, then the grid's own
	// corners; the grid MD_USA01 produces is 118x80 cells of 40, source pitch 236.
	static const Real pts[][2] =
	{
		{1923.0f, -79.0f}, {1910.0f, -149.0f}, {2069.0f, -158.0f}, {-320.0f, 1759.0f},
		{-1.0f, -1.0f}, {-40.5f, 10.0f}, {0.0f, 0.0f}, {100.0f, 100.0f},
		{4719.0f, 3199.0f}, {4720.0f, 3200.0f}
	};
	const Real cell = 40.0f;
	const Int pitch = 236;
	int i;

	printf("ORACLE build=vc6 cell=%.2f pitch=%d cells=118x80\n", cell, pitch);
	for (i = 0; i < (int)(sizeof(pts)/sizeof(pts[0])); ++i)
	{
		const Real wx = pts[i][0], wy = pts[i][1];
		// What getRiverVertexDiffuse did, and what the engine's REAL_TO_INT_FLOOR does.
		const Int tx = (Int)(wx / cell), ty = (Int)(wy / cell);
		const Int fx = (Int)fast_float_floor(wx / cell), fy = (Int)fast_float_floor(wy / cell);
		// And the byte getShroudLevel would have indexed, relative to m_srcTextureData.
		printf("ORACLE world=(%.2f,%.2f) trunc=(%d,%d) floor=(%d,%d) offsetTrunc=%d\n",
			wx, wy, tx, ty, fx, fy, tx*2 + ty*pitch);
	}
	return 0;
}
