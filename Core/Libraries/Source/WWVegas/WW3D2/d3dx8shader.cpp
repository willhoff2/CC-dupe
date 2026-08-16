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

// D3DXAssembleShader() off Windows: a DELIBERATE, LOUD non-implementation.
//
// This is the one D3DX entry point the port does not reproduce, so it says exactly what it is
// rather than pretending. On Windows it assembles DirectX shader assembly -- "ps.1.1", "tex t0",
// "texbem t2, t1" -- into a DWORD bytecode stream in an ID3DXBuffer. That is a whole assembler for
// a language whose instruction encoding is not published in this repository's dependencies, and
// whose output would then have to be accepted by a fixed-function-first renderer that has no
// pixel-shader stage at all: the Vulkan spike covers the fixed-function pipeline, and
// SetPixelShader on the D3D8 side of the backend is not a shader compiler.
//
// So this returns E_NOTIMPL, writes null through every out-parameter, and prints one line naming
// the shader that was refused. It cannot be mistaken for success: HRESULT 0x80004001 fails
// FAILED(), it is not 0, and the caller's own test is `if (hr == 0)`.
//
// WHAT THAT COSTS AT RUNTIME, precisely, because that is the deliverable here
//
// The only caller is WaterRenderObjClass::ReAcquireResources() in
// Core/GameEngineDevice/Source/W3DDevice/GameClient/Water/W3DWater.cpp, which assembles three
// `ps.1.1` pixel shaders and stores each handle in m_riverWaterPixelShader, m_waterPixelShader and
// m_trapezoidWaterPixelShader. On failure it never reaches Create_DX8_Pixel_Shader(), never
// releases a buffer it does not have, and leaves those three handles at the 0 they are initialised
// to -- and every use of all three is guarded by `if (handle)`:
//
//   * the river water draws without its pixel-shader pass: no sparkle layer (the `doSparkles`
//     branch is `if (m_riverWaterPixelShader && doSparkles)`), and no per-pixel reflection
//     constant;
//   * the trapezoidal (mesh) water applies the shroud in texture stage 1 instead of inside the
//     shader -- the `if (TheTerrainRenderObject->getShroud() && !m_trapezoidWaterPixelShader)`
//     branch exists precisely for hardware without pixel shaders;
//   * the environment-mapped water reflection (`texbem`) is not applied.
//
// Water still renders, in its fixed-function form, which is the form the port targets. Nothing
// leaks, nothing is dereferenced, and no code path assumes a handle it did not get.
//
// The caller is additionally behind `if (W3DShaderManager::getChipset() >=
// DC_GENERIC_PIXEL_SHADER_1_1)`, and that chipset is computed from the device's reported
// D3DCAPS8::PixelShaderVersion (or overridden by TheGlobalData->m_chipSetType). A backend that
// reports no pixel-shader support never reaches this function at all; the line below is therefore
// also the signal that a backend has started claiming pixel-shader capability it cannot honour.
//
// If the port ever wants these three shaders, the honest route is the same one W3DShaderManager
// already uses for shaders\\wave.pso -- precompiled bytecode as an asset, or a translation to the
// backend's own shading language -- not an assembler bolted in here.

#if !defined(_WIN32)

#include <d3dx8core.h>

#include <stdio.h>

HRESULT WINAPI D3DXAssembleShader(
	LPCVOID pSrcData,
	UINT SrcDataLen,
	DWORD Flags,
	LPD3DXBUFFER * ppConstants,
	LPD3DXBUFFER * ppCompiledShader,
	LPD3DXBUFFER * ppCompilationErrors)
{
	(void)Flags;

	/*
	**	Null before the report, so that a caller which ignores the HRESULT still cannot read a
	**	buffer pointer that was never written.
	*/
	if (ppConstants != nullptr) *ppConstants = nullptr;
	if (ppCompiledShader != nullptr) *ppCompiledShader = nullptr;
	if (ppCompilationErrors != nullptr) *ppCompilationErrors = nullptr;

	/*
	**	The first line of the source identifies the shader ("ps.1.1", "vs.1.1"), which is the only
	**	part worth printing and is enough to say which of the water shaders was refused. Reported
	**	on every call rather than once: the three calls happen together when the device's
	**	resources are (re)acquired, and a run that silently lost its water shaders on a device
	**	reset should say so again.
	*/
	unsigned length = 0;
	const char * source = static_cast<const char *>(pSrcData);
	if (source != nullptr) {
		while (length < SrcDataLen && length < 32 && source[length] != '\n'
				&& source[length] != '\r') {
			++length;
		}
	}

	fprintf(stderr, "!!! D3DXAssembleShader() is not implemented off Windows: no shader assembler "
		"in a fixed-function renderer; refused \"%.*s\" (%u bytes). The caller keeps a null shader "
		"handle and falls back to its fixed-function path.\n",
		int(length), (source != nullptr) ? source : "", unsigned(SrcDataLen));

	return E_NOTIMPL;
}

#endif // !_WIN32
