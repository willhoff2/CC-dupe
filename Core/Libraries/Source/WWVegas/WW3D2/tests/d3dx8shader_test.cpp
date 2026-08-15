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
 *  D3DXAssembleShader() off Windows is a deliberate refusal, and this test is what stops it     *
 *  from quietly becoming something worse.                                                      *
 *                                                                                             *
 *  The only caller is W3DWater.cpp, which assembles three ps.1.1 pixel shaders and tests        *
 *  `if (hr == 0)` before it creates a shader from the returned bytecode. A refusal is therefore  *
 *  safe -- the handles stay zero and every use of them is guarded -- but ONLY if the refusal is  *
 *  a failing HRESULT and the out-parameters are not left holding anything. A stub that returned  *
 *  D3D_OK, or that returned a failure while leaving *ppCompiledShader uninitialised, would put   *
 *  the water renderer into a path where it believes it has a shader. That is exactly the class   *
 *  of silent stub this port refuses to ship, so the contract is asserted here:                  *
 *                                                                                             *
 *    - the HRESULT is a failure, and specifically E_NOTIMPL rather than a made-up code;         *
 *    - every out-parameter is written null, including from a caller that passed rubbish in;      *
 *    - a null out-parameter pointer is accepted rather than dereferenced;                       *
 *    - `hr == 0`, which is the exact test W3DWater.cpp makes, is false.                          *
 *                                                                                             *
 *  It does NOT assert anything about the stderr line -- that is a diagnostic, not a contract --  *
 *  but the refusal does print one, which is what makes the missing shader visible in a log       *
 *  rather than showing up as flat water three months later.                                     *
 *                                                                                             *
 *  Run through scripts/native-d3dx8shader-test.py. This file is native-only: on Windows          *
 *  d3dx8.lib assembles the shader for real and none of these assertions apply.                  *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#include <d3dx8core.h>

#include <stdio.h>
#include <string.h>

static int _Failures = 0;
static int _Checks = 0;

static void Check(bool condition, const char * what)
{
	_Checks++;
	if (!condition) {
		_Failures++;
		printf("FAIL: %s\n", what);
	}
}

/*
**	W3DWater.cpp's river-water shader, verbatim, so that what is refused is a shader the engine
**	really asks for rather than an invented string.
*/
static const char * const RIVER_WATER_SHADER =
	"ps.1.1\n \
	tex t0 \n\
	tex t1	\n\
	tex t2	\n\
	tex t3\n\
	mul r0.rgb, v0, t0 ; blend vertex color into t0. \n\
	mov r0.a, t0 ; keep vertex alpha from fading the base water. \n\
	mul r1, t1, t2 ; mul\n\
	add r1.rgb, r1, t3\n\
	mul r1.rgb, r1, v0.a\n\
	+mul r0.a, r0, t3\n\
	add r0.rgb, r0, r1\n";


static void Test_Refuses_A_Real_Shader()
{
	LPD3DXBUFFER constants = reinterpret_cast<LPD3DXBUFFER>(0x1);
	LPD3DXBUFFER compiled = reinterpret_cast<LPD3DXBUFFER>(0x2);
	LPD3DXBUFFER errors = reinterpret_cast<LPD3DXBUFFER>(0x3);

	const HRESULT hr = D3DXAssembleShader(RIVER_WATER_SHADER,
		UINT(strlen(RIVER_WATER_SHADER)), 0, &constants, &compiled, &errors);

	Check(FAILED(hr), "assembling a shader fails off Windows");
	Check(hr == E_NOTIMPL, "the failure is E_NOTIMPL rather than an invented HRESULT");
	Check(!(hr == 0), "W3DWater.cpp's own `hr == 0` test is false, so it keeps a null handle");
	Check(compiled == nullptr,
		"the compiled-shader out-parameter is written null, not left as the caller found it");
	Check(constants == nullptr, "the constants out-parameter is written null");
	Check(errors == nullptr, "the error-buffer out-parameter is written null");
}


static void Test_Accepts_Null_Out_Parameters()
{
	/*
	**	W3DWater.cpp passes null for the constants and error buffers. A refusal that dereferenced
	**	them to null them would crash on the first frame of the first map with water.
	*/
	LPD3DXBUFFER compiled = reinterpret_cast<LPD3DXBUFFER>(0x4);
	const HRESULT hr = D3DXAssembleShader(RIVER_WATER_SHADER,
		UINT(strlen(RIVER_WATER_SHADER)), 0, nullptr, &compiled, nullptr);

	Check(FAILED(hr), "a call with null constants and error buffers still fails");
	Check(compiled == nullptr, "the compiled-shader out-parameter is still nulled");

	// And every out-parameter null, which is a caller that only wants the status.
	Check(FAILED(D3DXAssembleShader(RIVER_WATER_SHADER,
		UINT(strlen(RIVER_WATER_SHADER)), 0, nullptr, nullptr, nullptr)),
		"a call with no out-parameters at all fails without dereferencing them");
}


static void Test_Degenerate_Input()
{
	// A null or empty source is still a refusal, and still not a crash while logging it.
	LPD3DXBUFFER compiled = reinterpret_cast<LPD3DXBUFFER>(0x5);
	Check(FAILED(D3DXAssembleShader(nullptr, 0, 0, nullptr, &compiled, nullptr)),
		"a null source fails");
	Check(compiled == nullptr, "a null source still nulls the out-parameter");

	compiled = reinterpret_cast<LPD3DXBUFFER>(0x6);
	Check(FAILED(D3DXAssembleShader("", 0, 0, nullptr, &compiled, nullptr)),
		"an empty source fails");
	Check(compiled == nullptr, "an empty source still nulls the out-parameter");

	/*
	**	A source with no newline and a length shorter than the buffer: the diagnostic must respect
	**	SrcDataLen rather than reading to a terminator that is not there.
	*/
	const char unterminated[4] = { 'p', 's', '.', '1' };
	Check(FAILED(D3DXAssembleShader(unterminated, sizeof(unterminated), 0, nullptr, nullptr,
		nullptr)), "an unterminated source fails without reading past its length");
}


int main()
{
	Test_Refuses_A_Real_Shader();
	Test_Accepts_Null_Out_Parameters();
	Test_Degenerate_Input();

	printf("%d checks, %d failure(s)\n", _Checks, _Failures);
	return (_Failures == 0) ? 0 : 1;
}
