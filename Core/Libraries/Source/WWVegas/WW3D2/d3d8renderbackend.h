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
 *                    File Name : d3d8renderbackend.h                                          *
 *                                                                                             *
 *---------------------------------------------------------------------------------------------*/

#pragma once

#include "renderbackend.h"

#ifdef _WIN32

/**
** D3D8RenderBackendClass
**
** The one and only RenderBackendClass implementation: Direct3D 8.  This is the only place
** in the engine that owns an IDirect3D8 or an IDirect3DDevice8, and every operation below
** is the body that used to sit behind a DX8CALL macro in dx8wrapper.{h,cpp}.
**
** Everything above the seam (state caching, statistics, error handling, device selection
** policy, reset orchestration) stays in DX8Wrapper, so this class has no state beyond the
** D3D8 objects themselves and the loaded D3D8.DLL.
*/
class D3D8RenderBackendClass : public RenderBackendClass
{
public:
	D3D8RenderBackendClass();
	virtual ~D3D8RenderBackendClass();

	/*
	** Direct access to the underlying D3D8 objects.
	**
	** This is the documented escape hatch out of the seam.  It is declared on
	** RenderBackendClass (returning nullptr there) and overridden here, so that
	** DX8Wrapper::_Get_D3D_Device8() can be spelled against the seam instead of against this
	** Windows-only class; see docs/porting/renderer-seam.md §6.
	** It exists for three groups of callers that this slice does not move:
	**   - engine code that only asks "do we have a device yet?" before rendering,
	**   - the D3DX helper calls (D3DXCreateTexture and friends) which take a device,
	**   - the embedded browser and the WorldBuilder/W3DView tools.
	** See docs/porting/renderer-seam.md; a second backend has to make these callers go away.
	*/
	virtual IDirect3DDevice8* Peek_D3D_Device8() const { return D3DDevice; }
	virtual IDirect3D8* Peek_D3D8() const { return D3DInterface; }

	/*
	** Lifecycle
	*/
	virtual bool Open();
	virtual void Release_Interface();
	virtual void Free_Library();
	virtual bool Has_Interface() const { return D3DInterface != nullptr; }

	virtual HRESULT Create_Device(UINT adapter, D3DDEVTYPE device_type, HWND focus_window, DWORD behavior_flags, D3DPRESENT_PARAMETERS* present_parameters);
	virtual void Release_Device();
	virtual bool Has_Device() const { return D3DDevice != nullptr; }

	/*
	** Frame
	*/
	virtual HRESULT BeginScene();
	virtual HRESULT EndScene();
	virtual HRESULT Clear(DWORD count, CONST D3DRECT* rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil);
	virtual HRESULT Present(CONST RECT* source_rect, CONST RECT* dest_rect, HWND dest_window_override, CONST RGNDATA* dirty_region);

	/*
	** Device state
	*/
	virtual HRESULT SetRenderState(D3DRENDERSTATETYPE state, DWORD value);
	virtual HRESULT GetRenderState(D3DRENDERSTATETYPE state, DWORD* value);
	virtual HRESULT SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value);
	virtual HRESULT SetTexture(DWORD stage, IDirect3DBaseTexture8* texture);
	virtual HRESULT SetTransform(D3DTRANSFORMSTATETYPE state, CONST D3DMATRIX* matrix);
	virtual HRESULT GetTransform(D3DTRANSFORMSTATETYPE state, D3DMATRIX* matrix);
	virtual HRESULT SetViewport(CONST D3DVIEWPORT8* viewport);
	virtual HRESULT GetViewport(D3DVIEWPORT8* viewport);
	virtual HRESULT SetMaterial(CONST D3DMATERIAL8* material);
	virtual HRESULT SetLight(DWORD index, CONST D3DLIGHT8* light);
	virtual HRESULT LightEnable(DWORD index, BOOL enable);
	virtual HRESULT SetClipPlane(DWORD index, CONST float* plane);

	/*
	** Shaders and shader constants
	*/
	virtual HRESULT CreateVertexShader(CONST DWORD* declaration, CONST DWORD* function, DWORD* handle, DWORD usage);
	virtual HRESULT DeleteVertexShader(DWORD handle);
	virtual HRESULT SetVertexShader(DWORD handle);
	virtual HRESULT SetVertexShaderConstant(DWORD reg, CONST void* constant_data, DWORD constant_count);
	virtual HRESULT CreatePixelShader(CONST DWORD* function, DWORD* handle);
	virtual HRESULT DeletePixelShader(DWORD handle);
	virtual HRESULT SetPixelShader(DWORD handle);
	virtual HRESULT SetPixelShaderConstant(DWORD reg, CONST void* constant_data, DWORD constant_count);

	/*
	** Resources
	*/
	virtual HRESULT CreateTexture(UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DTexture8** texture);
	virtual HRESULT CreateCubeTexture(UINT edge_length, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DCubeTexture8** cube_texture);
	virtual HRESULT CreateVolumeTexture(UINT width, UINT height, UINT depth, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DVolumeTexture8** volume_texture);
	virtual HRESULT CreateVertexBuffer(UINT length, DWORD usage, DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer8** vertex_buffer);
	virtual HRESULT CreateIndexBuffer(UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DIndexBuffer8** index_buffer);
	virtual HRESULT CreateImageSurface(UINT width, UINT height, D3DFORMAT format, IDirect3DSurface8** surface);
	virtual HRESULT CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS* present_parameters, IDirect3DSwapChain8** swap_chain);
	virtual HRESULT UpdateTexture(IDirect3DBaseTexture8* source, IDirect3DBaseTexture8* destination);
	virtual HRESULT CopyRects(IDirect3DSurface8* source_surface, CONST RECT* source_rects, UINT rect_count, IDirect3DSurface8* destination_surface, CONST POINT* dest_points);

	/*
	** Submission
	*/
	virtual HRESULT SetStreamSource(UINT stream_number, IDirect3DVertexBuffer8* stream_data, UINT stride);
	virtual HRESULT SetIndices(IDirect3DIndexBuffer8* index_data, UINT base_vertex_index);
	virtual HRESULT DrawPrimitive(D3DPRIMITIVETYPE primitive_type, UINT start_vertex, UINT primitive_count);
	virtual HRESULT DrawIndexedPrimitive(D3DPRIMITIVETYPE primitive_type, UINT min_index, UINT num_vertices, UINT start_index, UINT primitive_count);
	virtual HRESULT DrawPrimitiveUP(D3DPRIMITIVETYPE primitive_type, UINT primitive_count, CONST void* vertex_stream_zero_data, UINT vertex_stream_zero_stride);
	virtual HRESULT ProcessVertices(UINT src_start_index, UINT dest_index, UINT vertex_count, IDirect3DVertexBuffer8* dest_buffer, DWORD flags);

	/*
	** Render targets and back/front buffers
	*/
	virtual HRESULT GetRenderTarget(IDirect3DSurface8** render_target);
	virtual HRESULT GetDepthStencilSurface(IDirect3DSurface8** depth_stencil_surface);
	virtual HRESULT SetRenderTarget(IDirect3DSurface8* render_target, IDirect3DSurface8* new_z_stencil);
	virtual HRESULT GetFrontBuffer(IDirect3DSurface8* dest_surface);
	virtual HRESULT GetBackBuffer(UINT back_buffer, D3DBACKBUFFER_TYPE type, IDirect3DSurface8** surface);

	/*
	** Device status, capabilities and memory
	*/
	virtual HRESULT TestCooperativeLevel();
	virtual HRESULT Reset(D3DPRESENT_PARAMETERS* present_parameters);
	virtual HRESULT ValidateDevice(DWORD* num_passes);
	virtual UINT GetAvailableTextureMem();
	virtual HRESULT ResourceManagerDiscardBytes(DWORD bytes);
	virtual HRESULT GetDeviceCaps(D3DCAPS8* caps);
	virtual HRESULT GetDisplayMode(D3DDISPLAYMODE* mode);

	/*
	** Cursor and gamma
	*/
	virtual BOOL ShowCursor(BOOL show);
	virtual HRESULT SetCursorProperties(UINT x_hotspot, UINT y_hotspot, IDirect3DSurface8* cursor_bitmap);
	virtual void SetCursorPosition(UINT x_screen_space, UINT y_screen_space, DWORD flags);
	virtual void SetGammaRamp(DWORD flags, CONST D3DGAMMARAMP* ramp);

	/*
	** Adapter enumeration and format probes
	*/
	virtual UINT GetAdapterCount();
	virtual HRESULT GetAdapterIdentifier(UINT adapter, DWORD flags, D3DADAPTER_IDENTIFIER8* identifier);
	virtual UINT GetAdapterModeCount(UINT adapter);
	virtual HRESULT EnumAdapterModes(UINT adapter, UINT mode, D3DDISPLAYMODE* display_mode);
	virtual HRESULT GetAdapterDisplayMode(UINT adapter, D3DDISPLAYMODE* display_mode);
	virtual HRESULT CheckDeviceType(UINT adapter, D3DDEVTYPE check_type, D3DFORMAT display_format, D3DFORMAT backbuffer_format, BOOL windowed);
	virtual HRESULT CheckDeviceFormat(UINT adapter, D3DDEVTYPE device_type, D3DFORMAT adapter_format, DWORD usage, D3DRESOURCETYPE resource_type, D3DFORMAT check_format);
	virtual HRESULT CheckDeviceMultiSampleType(UINT adapter, D3DDEVTYPE device_type, D3DFORMAT surface_format, BOOL windowed, D3DMULTISAMPLE_TYPE multi_sample_type);
	virtual HRESULT CheckDepthStencilMatch(UINT adapter, D3DDEVTYPE device_type, D3DFORMAT adapter_format, D3DFORMAT render_target_format, D3DFORMAT depth_stencil_format);
	virtual HRESULT GetDeviceCaps(UINT adapter, D3DDEVTYPE device_type, D3DCAPS8* caps);

private:
	typedef IDirect3D8* (WINAPI *Direct3DCreate8Type)(UINT sdk_version);

	IDirect3D8* D3DInterface;
	IDirect3DDevice8* D3DDevice;
	HINSTANCE D3D8Lib;
	Direct3DCreate8Type Direct3DCreate8Ptr;
};

/*
** The single backend instance.  DX8Wrapper is all-static and callers ask it whether a
** device exists before it has been initialised, so the instance has static storage
** duration rather than being allocated in DX8Wrapper::Init(); Open() and Create_Device()
** are what actually acquire anything.
*/
extern D3D8RenderBackendClass TheD3D8RenderBackend;

#endif // _WIN32
