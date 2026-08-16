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
 *                 Project Name : ww3d                                                         *
 *                                                                                             *
 *                    File Name : d3d8renderbackend.cpp                                        *
 *                                                                                             *
 * The Direct3D 8 implementation of RenderBackendClass.  Every method here is a body that       *
 * used to be expanded inline from a DX8CALL macro inside dx8wrapper.{h,cpp}; the caching,      *
 * statistics and error handling that surrounded those macros stayed on the DX8Wrapper side     *
 * of the seam, so nothing in this file does anything but issue the D3D8 call.                  *
 *---------------------------------------------------------------------------------------------*/

#include "d3d8renderbackend.h"

#ifdef _WIN32

#include "WWLib/DbgHelpGuard.h"

D3D8RenderBackendClass TheD3D8RenderBackend;

D3D8RenderBackendClass::D3D8RenderBackendClass()
	:
	D3DInterface(nullptr),
	D3DDevice(nullptr),
	D3D8Lib(nullptr),
	Direct3DCreate8Ptr(nullptr)
{
}

D3D8RenderBackendClass::~D3D8RenderBackendClass()
{
}

bool D3D8RenderBackendClass::Open()
{
	D3D8Lib = LoadLibrary("D3D8.DLL");
	if (D3D8Lib == nullptr) return false;

	Direct3DCreate8Ptr = (Direct3DCreate8Type)GetProcAddress(D3D8Lib, "Direct3DCreate8");
	if (Direct3DCreate8Ptr == nullptr) return false;

	{
		// TheSuperHackers @bugfix xezon 13/06/2025 Front load the system dbghelp.dll to prevent
		// the graphics driver from potentially loading the old game dbghelp.dll and then crashing the game process.
		DbgHelpGuard dbgHelpGuard;

		D3DInterface = Direct3DCreate8Ptr(D3D_SDK_VERSION);		// TODO: handle failure cases...
	}

	return D3DInterface != nullptr;
}

void D3D8RenderBackendClass::Release_Interface()
{
	if (D3DInterface) {
		D3DInterface->Release();
		D3DInterface = nullptr;
	}
}

void D3D8RenderBackendClass::Free_Library()
{
	if (D3D8Lib) {
		FreeLibrary(D3D8Lib);
		D3D8Lib = nullptr;
		Direct3DCreate8Ptr = nullptr;
	}
}

HRESULT D3D8RenderBackendClass::Create_Device(UINT adapter, D3DDEVTYPE device_type, HWND focus_window, DWORD behavior_flags, D3DPRESENT_PARAMETERS* present_parameters)
{
	// Note: the caller holds a DbgHelpGuard across device creation (including its retry with
	// a different depth format), so this does not front load dbghelp.dll itself.
	return D3DInterface->CreateDevice(
		adapter,
		device_type,
		focus_window,
		behavior_flags,
		present_parameters,
		&D3DDevice);
}

void D3D8RenderBackendClass::Release_Device()
{
	if (D3DDevice) {
		D3DDevice->Release();
		D3DDevice = nullptr;
	}
}

/*
** Frame
*/

HRESULT D3D8RenderBackendClass::BeginScene()
{
	return D3DDevice->BeginScene();
}

HRESULT D3D8RenderBackendClass::EndScene()
{
	return D3DDevice->EndScene();
}

HRESULT D3D8RenderBackendClass::Clear(DWORD count, CONST D3DRECT* rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil)
{
	return D3DDevice->Clear(count, rects, flags, color, z, stencil);
}

HRESULT D3D8RenderBackendClass::Present(CONST RECT* source_rect, CONST RECT* dest_rect, HWND dest_window_override, CONST RGNDATA* dirty_region)
{
	return D3DDevice->Present(source_rect, dest_rect, dest_window_override, dirty_region);
}

/*
** Device state
*/

HRESULT D3D8RenderBackendClass::SetRenderState(D3DRENDERSTATETYPE state, DWORD value)
{
	return D3DDevice->SetRenderState(state, value);
}

HRESULT D3D8RenderBackendClass::GetRenderState(D3DRENDERSTATETYPE state, DWORD* value)
{
	return D3DDevice->GetRenderState(state, value);
}

HRESULT D3D8RenderBackendClass::SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value)
{
	return D3DDevice->SetTextureStageState(stage, type, value);
}

HRESULT D3D8RenderBackendClass::SetTexture(DWORD stage, IDirect3DBaseTexture8* texture)
{
	return D3DDevice->SetTexture(stage, texture);
}

HRESULT D3D8RenderBackendClass::SetTransform(D3DTRANSFORMSTATETYPE state, CONST D3DMATRIX* matrix)
{
	return D3DDevice->SetTransform(state, matrix);
}

HRESULT D3D8RenderBackendClass::GetTransform(D3DTRANSFORMSTATETYPE state, D3DMATRIX* matrix)
{
	return D3DDevice->GetTransform(state, matrix);
}

HRESULT D3D8RenderBackendClass::SetViewport(CONST D3DVIEWPORT8* viewport)
{
	return D3DDevice->SetViewport(viewport);
}

HRESULT D3D8RenderBackendClass::GetViewport(D3DVIEWPORT8* viewport)
{
	return D3DDevice->GetViewport(viewport);
}

HRESULT D3D8RenderBackendClass::SetMaterial(CONST D3DMATERIAL8* material)
{
	return D3DDevice->SetMaterial(material);
}

HRESULT D3D8RenderBackendClass::SetLight(DWORD index, CONST D3DLIGHT8* light)
{
	return D3DDevice->SetLight(index, light);
}

HRESULT D3D8RenderBackendClass::LightEnable(DWORD index, BOOL enable)
{
	return D3DDevice->LightEnable(index, enable);
}

HRESULT D3D8RenderBackendClass::SetClipPlane(DWORD index, CONST float* plane)
{
	return D3DDevice->SetClipPlane(index, plane);
}

/*
** Shaders and shader constants
*/

HRESULT D3D8RenderBackendClass::CreateVertexShader(CONST DWORD* declaration, CONST DWORD* function, DWORD* handle, DWORD usage)
{
	return D3DDevice->CreateVertexShader(declaration, function, handle, usage);
}

HRESULT D3D8RenderBackendClass::DeleteVertexShader(DWORD handle)
{
	return D3DDevice->DeleteVertexShader(handle);
}

HRESULT D3D8RenderBackendClass::SetVertexShader(DWORD handle)
{
	return D3DDevice->SetVertexShader(handle);
}

HRESULT D3D8RenderBackendClass::SetVertexShaderConstant(DWORD reg, CONST void* constant_data, DWORD constant_count)
{
	return D3DDevice->SetVertexShaderConstant(reg, constant_data, constant_count);
}

HRESULT D3D8RenderBackendClass::CreatePixelShader(CONST DWORD* function, DWORD* handle)
{
	return D3DDevice->CreatePixelShader(function, handle);
}

HRESULT D3D8RenderBackendClass::DeletePixelShader(DWORD handle)
{
	return D3DDevice->DeletePixelShader(handle);
}

HRESULT D3D8RenderBackendClass::SetPixelShader(DWORD handle)
{
	return D3DDevice->SetPixelShader(handle);
}

HRESULT D3D8RenderBackendClass::SetPixelShaderConstant(DWORD reg, CONST void* constant_data, DWORD constant_count)
{
	return D3DDevice->SetPixelShaderConstant(reg, constant_data, constant_count);
}

/*
** Resources
*/

HRESULT D3D8RenderBackendClass::CreateTexture(UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DTexture8** texture)
{
	return D3DDevice->CreateTexture(width, height, levels, usage, format, pool, texture);
}

HRESULT D3D8RenderBackendClass::CreateCubeTexture(UINT edge_length, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DCubeTexture8** cube_texture)
{
	return D3DDevice->CreateCubeTexture(edge_length, levels, usage, format, pool, cube_texture);
}

HRESULT D3D8RenderBackendClass::CreateVolumeTexture(UINT width, UINT height, UINT depth, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DVolumeTexture8** volume_texture)
{
	return D3DDevice->CreateVolumeTexture(width, height, depth, levels, usage, format, pool, volume_texture);
}

HRESULT D3D8RenderBackendClass::CreateVertexBuffer(UINT length, DWORD usage, DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer8** vertex_buffer)
{
	return D3DDevice->CreateVertexBuffer(length, usage, fvf, pool, vertex_buffer);
}

HRESULT D3D8RenderBackendClass::CreateIndexBuffer(UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DIndexBuffer8** index_buffer)
{
	return D3DDevice->CreateIndexBuffer(length, usage, format, pool, index_buffer);
}

HRESULT D3D8RenderBackendClass::CreateImageSurface(UINT width, UINT height, D3DFORMAT format, IDirect3DSurface8** surface)
{
	return D3DDevice->CreateImageSurface(width, height, format, surface);
}

HRESULT D3D8RenderBackendClass::CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS* present_parameters, IDirect3DSwapChain8** swap_chain)
{
	return D3DDevice->CreateAdditionalSwapChain(present_parameters, swap_chain);
}

HRESULT D3D8RenderBackendClass::UpdateTexture(IDirect3DBaseTexture8* source, IDirect3DBaseTexture8* destination)
{
	return D3DDevice->UpdateTexture(source, destination);
}

HRESULT D3D8RenderBackendClass::CopyRects(IDirect3DSurface8* source_surface, CONST RECT* source_rects, UINT rect_count, IDirect3DSurface8* destination_surface, CONST POINT* dest_points)
{
	return D3DDevice->CopyRects(source_surface, source_rects, rect_count, destination_surface, dest_points);
}

/*
** Submission
*/

HRESULT D3D8RenderBackendClass::SetStreamSource(UINT stream_number, IDirect3DVertexBuffer8* stream_data, UINT stride)
{
	return D3DDevice->SetStreamSource(stream_number, stream_data, stride);
}

HRESULT D3D8RenderBackendClass::SetIndices(IDirect3DIndexBuffer8* index_data, UINT base_vertex_index)
{
	return D3DDevice->SetIndices(index_data, base_vertex_index);
}

HRESULT D3D8RenderBackendClass::DrawPrimitive(D3DPRIMITIVETYPE primitive_type, UINT start_vertex, UINT primitive_count)
{
	return D3DDevice->DrawPrimitive(primitive_type, start_vertex, primitive_count);
}

HRESULT D3D8RenderBackendClass::DrawIndexedPrimitive(D3DPRIMITIVETYPE primitive_type, UINT min_index, UINT num_vertices, UINT start_index, UINT primitive_count)
{
	return D3DDevice->DrawIndexedPrimitive(primitive_type, min_index, num_vertices, start_index, primitive_count);
}

HRESULT D3D8RenderBackendClass::DrawPrimitiveUP(D3DPRIMITIVETYPE primitive_type, UINT primitive_count, CONST void* vertex_stream_zero_data, UINT vertex_stream_zero_stride)
{
	return D3DDevice->DrawPrimitiveUP(primitive_type, primitive_count, vertex_stream_zero_data, vertex_stream_zero_stride);
}

HRESULT D3D8RenderBackendClass::ProcessVertices(UINT src_start_index, UINT dest_index, UINT vertex_count, IDirect3DVertexBuffer8* dest_buffer, DWORD flags)
{
	return D3DDevice->ProcessVertices(src_start_index, dest_index, vertex_count, dest_buffer, flags);
}

/*
** Render targets and back/front buffers
*/

HRESULT D3D8RenderBackendClass::GetRenderTarget(IDirect3DSurface8** render_target)
{
	return D3DDevice->GetRenderTarget(render_target);
}

HRESULT D3D8RenderBackendClass::GetDepthStencilSurface(IDirect3DSurface8** depth_stencil_surface)
{
	return D3DDevice->GetDepthStencilSurface(depth_stencil_surface);
}

HRESULT D3D8RenderBackendClass::SetRenderTarget(IDirect3DSurface8* render_target, IDirect3DSurface8* new_z_stencil)
{
	return D3DDevice->SetRenderTarget(render_target, new_z_stencil);
}

HRESULT D3D8RenderBackendClass::GetFrontBuffer(IDirect3DSurface8* dest_surface)
{
	return D3DDevice->GetFrontBuffer(dest_surface);
}

HRESULT D3D8RenderBackendClass::GetBackBuffer(UINT back_buffer, D3DBACKBUFFER_TYPE type, IDirect3DSurface8** surface)
{
	return D3DDevice->GetBackBuffer(back_buffer, type, surface);
}

/*
** Device status, capabilities and memory
*/

HRESULT D3D8RenderBackendClass::TestCooperativeLevel()
{
	return D3DDevice->TestCooperativeLevel();
}

HRESULT D3D8RenderBackendClass::Reset(D3DPRESENT_PARAMETERS* present_parameters)
{
	return D3DDevice->Reset(present_parameters);
}

HRESULT D3D8RenderBackendClass::ValidateDevice(DWORD* num_passes)
{
	return D3DDevice->ValidateDevice(num_passes);
}

UINT D3D8RenderBackendClass::GetAvailableTextureMem()
{
	return D3DDevice->GetAvailableTextureMem();
}

HRESULT D3D8RenderBackendClass::ResourceManagerDiscardBytes(DWORD bytes)
{
	return D3DDevice->ResourceManagerDiscardBytes(bytes);
}

HRESULT D3D8RenderBackendClass::GetDeviceCaps(D3DCAPS8* caps)
{
	return D3DDevice->GetDeviceCaps(caps);
}

HRESULT D3D8RenderBackendClass::GetDisplayMode(D3DDISPLAYMODE* mode)
{
	return D3DDevice->GetDisplayMode(mode);
}

/*
** Cursor and gamma
*/

BOOL D3D8RenderBackendClass::ShowCursor(BOOL show)
{
	return D3DDevice->ShowCursor(show);
}

HRESULT D3D8RenderBackendClass::SetCursorProperties(UINT x_hotspot, UINT y_hotspot, IDirect3DSurface8* cursor_bitmap)
{
	return D3DDevice->SetCursorProperties(x_hotspot, y_hotspot, cursor_bitmap);
}

void D3D8RenderBackendClass::SetCursorPosition(UINT x_screen_space, UINT y_screen_space, DWORD flags)
{
	D3DDevice->SetCursorPosition(x_screen_space, y_screen_space, flags);
}

void D3D8RenderBackendClass::SetGammaRamp(DWORD flags, CONST D3DGAMMARAMP* ramp)
{
	D3DDevice->SetGammaRamp(flags, ramp);
}

/*
** Adapter enumeration and format probes
*/

UINT D3D8RenderBackendClass::GetAdapterCount()
{
	return D3DInterface->GetAdapterCount();
}

HRESULT D3D8RenderBackendClass::GetAdapterIdentifier(UINT adapter, DWORD flags, D3DADAPTER_IDENTIFIER8* identifier)
{
	return D3DInterface->GetAdapterIdentifier(adapter, flags, identifier);
}

UINT D3D8RenderBackendClass::GetAdapterModeCount(UINT adapter)
{
	return D3DInterface->GetAdapterModeCount(adapter);
}

HRESULT D3D8RenderBackendClass::EnumAdapterModes(UINT adapter, UINT mode, D3DDISPLAYMODE* display_mode)
{
	return D3DInterface->EnumAdapterModes(adapter, mode, display_mode);
}

HRESULT D3D8RenderBackendClass::GetAdapterDisplayMode(UINT adapter, D3DDISPLAYMODE* display_mode)
{
	return D3DInterface->GetAdapterDisplayMode(adapter, display_mode);
}

HRESULT D3D8RenderBackendClass::CheckDeviceType(UINT adapter, D3DDEVTYPE check_type, D3DFORMAT display_format, D3DFORMAT backbuffer_format, BOOL windowed)
{
	return D3DInterface->CheckDeviceType(adapter, check_type, display_format, backbuffer_format, windowed);
}

HRESULT D3D8RenderBackendClass::CheckDeviceFormat(UINT adapter, D3DDEVTYPE device_type, D3DFORMAT adapter_format, DWORD usage, D3DRESOURCETYPE resource_type, D3DFORMAT check_format)
{
	return D3DInterface->CheckDeviceFormat(adapter, device_type, adapter_format, usage, resource_type, check_format);
}

HRESULT D3D8RenderBackendClass::CheckDeviceMultiSampleType(UINT adapter, D3DDEVTYPE device_type, D3DFORMAT surface_format, BOOL windowed, D3DMULTISAMPLE_TYPE multi_sample_type)
{
	return D3DInterface->CheckDeviceMultiSampleType(adapter, device_type, surface_format, windowed, multi_sample_type);
}

HRESULT D3D8RenderBackendClass::CheckDepthStencilMatch(UINT adapter, D3DDEVTYPE device_type, D3DFORMAT adapter_format, D3DFORMAT render_target_format, D3DFORMAT depth_stencil_format)
{
	return D3DInterface->CheckDepthStencilMatch(adapter, device_type, adapter_format, render_target_format, depth_stencil_format);
}

HRESULT D3D8RenderBackendClass::GetDeviceCaps(UINT adapter, D3DDEVTYPE device_type, D3DCAPS8* caps)
{
	return D3DInterface->GetDeviceCaps(adapter, device_type, caps);
}

#endif // _WIN32
