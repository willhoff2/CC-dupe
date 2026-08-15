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
 *                    File Name : renderbackend.h                                              *
 *                                                                                             *
 *---------------------------------------------------------------------------------------------*/

#pragma once

#include "WWLib/always.h"
#include "d3d8.h"

/**
** RenderBackendClass
**
** The abstraction seam between DX8Wrapper and the graphics API.  DX8Wrapper owns all of
** the renderer's *policy* - the shadow state cache, the redundant state early-outs, the
** deferred render state, the statistics and the error reporting - and a backend owns all
** of the *API calls*.  Exactly one backend implementation exists today
** (D3D8RenderBackendClass); it is the only place in the engine that holds an
** IDirect3DDevice8 or an IDirect3D8.
**
** Shape
** -----
** The operation set below is not invented: it is the 63 distinct IDirect3DDevice8 /
** IDirect3D8 methods that the 155 call sites inside dx8wrapper.{h,cpp} reach, one virtual
** per method, deliberately keeping the D3D8 method name and signature so that the mapping
** from call site to backend entry point is one-for-one auditable and so that the migration
** could not silently change an argument.  Renaming these into engine vocabulary is a
** separate, mechanical slice that is better done when a second backend exists to say what
** the vocabulary should be.
**
** Two consequences of that decision, both deliberate for this slice:
**
**  - The interface speaks D3D8 types (D3DRENDERSTATETYPE, D3DMATRIX, D3DCAPS8, ...) and
**    hands out raw D3D8 resource pointers (IDirect3DTexture8*, IDirect3DSurface8*,
**    IDirect3D*Buffer8*).  The engine calls methods on those resources directly in 213
**    places across 19 files (see docs/porting/renderer-seam.md), so abstracting resources
**    is a slice of its own; a half-abstracted resource model would be worse than an
**    honestly un-abstracted one.
**  - Capabilities stay D3D8-shaped.  DX8Caps stores a D3DCAPS8 and the engine branches on
**    its fields all over, so a backend fills in a D3DCAPS8.
**
** Performance
** -----------
** Every method here is virtual, so every call costs one indirect call.  That is only
** acceptable because DX8Wrapper never reaches the backend on the hot redundant-state path:
** Set_DX8_Render_State / Set_DX8_Texture_Stage_State / Set_DX8_Texture compare against the
** shadow state and return *before* touching this interface, exactly as they did before the
** seam existed.  A virtual call happens only where a real D3D8 call would have happened
** anyway, i.e. where the previous code already paid for a COM vtable dispatch.
**
** Failure handling
** ----------------
** Methods return what D3D8 returns and never assert; the caller (the DX8CALL / DX8CALL_RAW
** macros in dx8wrapper.h) decides whether a failure is fatal, which preserves the
** distinction between checked calls and the capability probes that are expected to fail.
*/
class RenderBackendClass
{
public:
	virtual ~RenderBackendClass() {}

	/*
	** The documented hole in the seam: the raw D3D8 objects, for the callers this seam did
	** not move (the "is there a device yet?" tests, the D3DX helpers that take a device, the
	** embedded browser and the Win32 tools).  DX8Wrapper::_Get_D3D_Device8() / _Get_D3D8()
	** forward to these, so those callers reach the *installed* backend rather than naming a
	** D3D8 backend that only exists on Windows.
	**
	** A backend that is not D3D8 has no such objects and returns nullptr, which is the same
	** answer the D3D8 backend gives before Create_Device().  Every existing caller already
	** handles a null device, because that is the state it tests for.  These are the only
	** methods here with a default implementation, and deliberately so: an implementation must
	** not have to know about D3D8 in order to say "no D3D8 here".
	*/
	virtual IDirect3DDevice8* Peek_D3D_Device8() const { return nullptr; }
	virtual IDirect3D8* Peek_D3D8() const { return nullptr; }

	/*
	** Backend and device lifecycle.  These are the only entry points that do not mirror a
	** D3D8 method, because device *ownership* is what moved across the seam.
	**
	** Open() acquires the API (for D3D8: LoadLibrary + Direct3DCreate8) and must be called
	** before anything else; Release_Interface() and Free_Library() undo it in the two steps
	** DX8Wrapper::Shutdown() needs, since it releases the interface before dropping its
	** cached texture references but frees the library after.
	*/
	virtual bool Open() = 0;
	virtual void Release_Interface() = 0;
	virtual void Free_Library() = 0;
	virtual bool Has_Interface() const = 0;

	/*
	** Create_Device() keeps the device internally instead of handing it back - that is the
	** point of the seam - so unlike the pass-throughs below it does not take an out
	** parameter.  Release_Device() releases it; DX8Wrapper is responsible for unbinding
	** state and shutting down device dependent subsystems first.
	*/
	virtual HRESULT Create_Device(UINT adapter, D3DDEVTYPE device_type, HWND focus_window, DWORD behavior_flags, D3DPRESENT_PARAMETERS* present_parameters) = 0;
	virtual void Release_Device() = 0;
	virtual bool Has_Device() const = 0;

	/*
	** Frame
	*/
	virtual HRESULT BeginScene() = 0;
	virtual HRESULT EndScene() = 0;
	virtual HRESULT Clear(DWORD count, CONST D3DRECT* rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil) = 0;
	virtual HRESULT Present(CONST RECT* source_rect, CONST RECT* dest_rect, HWND dest_window_override, CONST RGNDATA* dirty_region) = 0;

	/*
	** Device state.  DX8Wrapper caches all of these; the backend is reached only when the
	** cached value actually changes (or through the deliberately uncached entry points).
	*/
	virtual HRESULT SetRenderState(D3DRENDERSTATETYPE state, DWORD value) = 0;
	virtual HRESULT GetRenderState(D3DRENDERSTATETYPE state, DWORD* value) = 0;
	virtual HRESULT SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value) = 0;
	virtual HRESULT SetTexture(DWORD stage, IDirect3DBaseTexture8* texture) = 0;
	virtual HRESULT SetTransform(D3DTRANSFORMSTATETYPE state, CONST D3DMATRIX* matrix) = 0;
	virtual HRESULT GetTransform(D3DTRANSFORMSTATETYPE state, D3DMATRIX* matrix) = 0;
	virtual HRESULT SetViewport(CONST D3DVIEWPORT8* viewport) = 0;
	virtual HRESULT GetViewport(D3DVIEWPORT8* viewport) = 0;
	virtual HRESULT SetMaterial(CONST D3DMATERIAL8* material) = 0;
	virtual HRESULT SetLight(DWORD index, CONST D3DLIGHT8* light) = 0;
	virtual HRESULT LightEnable(DWORD index, BOOL enable) = 0;
	virtual HRESULT SetClipPlane(DWORD index, CONST float* plane) = 0;

	/*
	** Shaders and shader constants
	*/
	virtual HRESULT CreateVertexShader(CONST DWORD* declaration, CONST DWORD* function, DWORD* handle, DWORD usage) = 0;
	virtual HRESULT DeleteVertexShader(DWORD handle) = 0;
	virtual HRESULT SetVertexShader(DWORD handle) = 0;
	virtual HRESULT SetVertexShaderConstant(DWORD reg, CONST void* constant_data, DWORD constant_count) = 0;
	virtual HRESULT CreatePixelShader(CONST DWORD* function, DWORD* handle) = 0;
	virtual HRESULT DeletePixelShader(DWORD handle) = 0;
	virtual HRESULT SetPixelShader(DWORD handle) = 0;
	virtual HRESULT SetPixelShaderConstant(DWORD reg, CONST void* constant_data, DWORD constant_count) = 0;

	/*
	** Resource creation.  The engine owns the returned resources and calls AddRef/Release
	** and lock/unlock on them itself; see the note above about resources being outside this
	** slice.
	*/
	virtual HRESULT CreateTexture(UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DTexture8** texture) = 0;
	virtual HRESULT CreateVertexBuffer(UINT length, DWORD usage, DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer8** vertex_buffer) = 0;
	virtual HRESULT CreateIndexBuffer(UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DIndexBuffer8** index_buffer) = 0;
	virtual HRESULT CreateImageSurface(UINT width, UINT height, D3DFORMAT format, IDirect3DSurface8** surface) = 0;
	virtual HRESULT CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS* present_parameters, IDirect3DSwapChain8** swap_chain) = 0;
	virtual HRESULT UpdateTexture(IDirect3DBaseTexture8* source, IDirect3DBaseTexture8* destination) = 0;
	virtual HRESULT CopyRects(IDirect3DSurface8* source_surface, CONST RECT* source_rects, UINT rect_count, IDirect3DSurface8* destination_surface, CONST POINT* dest_points) = 0;

	/*
	** Submission
	*/
	virtual HRESULT SetStreamSource(UINT stream_number, IDirect3DVertexBuffer8* stream_data, UINT stride) = 0;
	virtual HRESULT SetIndices(IDirect3DIndexBuffer8* index_data, UINT base_vertex_index) = 0;
	virtual HRESULT DrawPrimitive(D3DPRIMITIVETYPE primitive_type, UINT start_vertex, UINT primitive_count) = 0;
	virtual HRESULT DrawIndexedPrimitive(D3DPRIMITIVETYPE primitive_type, UINT min_index, UINT num_vertices, UINT start_index, UINT primitive_count) = 0;
	virtual HRESULT DrawPrimitiveUP(D3DPRIMITIVETYPE primitive_type, UINT primitive_count, CONST void* vertex_stream_zero_data, UINT vertex_stream_zero_stride) = 0;
	virtual HRESULT ProcessVertices(UINT src_start_index, UINT dest_index, UINT vertex_count, IDirect3DVertexBuffer8* dest_buffer, DWORD flags) = 0;

	/*
	** Render targets and back/front buffers
	*/
	virtual HRESULT GetRenderTarget(IDirect3DSurface8** render_target) = 0;
	virtual HRESULT GetDepthStencilSurface(IDirect3DSurface8** depth_stencil_surface) = 0;
	virtual HRESULT SetRenderTarget(IDirect3DSurface8* render_target, IDirect3DSurface8* new_z_stencil) = 0;
	virtual HRESULT GetFrontBuffer(IDirect3DSurface8* dest_surface) = 0;
	virtual HRESULT GetBackBuffer(UINT back_buffer, D3DBACKBUFFER_TYPE type, IDirect3DSurface8** surface) = 0;

	/*
	** Device status, capabilities and memory
	*/
	virtual HRESULT TestCooperativeLevel() = 0;
	virtual HRESULT Reset(D3DPRESENT_PARAMETERS* present_parameters) = 0;
	virtual HRESULT ValidateDevice(DWORD* num_passes) = 0;
	virtual UINT GetAvailableTextureMem() = 0;
	virtual HRESULT ResourceManagerDiscardBytes(DWORD bytes) = 0;
	virtual HRESULT GetDeviceCaps(D3DCAPS8* caps) = 0;
	virtual HRESULT GetDisplayMode(D3DDISPLAYMODE* mode) = 0;

	/*
	** Cursor and gamma
	*/
	virtual BOOL ShowCursor(BOOL show) = 0;
	virtual HRESULT SetCursorProperties(UINT x_hotspot, UINT y_hotspot, IDirect3DSurface8* cursor_bitmap) = 0;
	virtual void SetCursorPosition(UINT x_screen_space, UINT y_screen_space, DWORD flags) = 0;
	virtual void SetGammaRamp(DWORD flags, CONST D3DGAMMARAMP* ramp) = 0;

	/*
	** Adapter enumeration and format probes.  These are the IDirect3D8 (as opposed to
	** device) operations; they are all expected to fail for unsupported combinations.
	*/
	virtual UINT GetAdapterCount() = 0;
	virtual HRESULT GetAdapterIdentifier(UINT adapter, DWORD flags, D3DADAPTER_IDENTIFIER8* identifier) = 0;
	virtual UINT GetAdapterModeCount(UINT adapter) = 0;
	virtual HRESULT EnumAdapterModes(UINT adapter, UINT mode, D3DDISPLAYMODE* display_mode) = 0;
	virtual HRESULT GetAdapterDisplayMode(UINT adapter, D3DDISPLAYMODE* display_mode) = 0;
	virtual HRESULT CheckDeviceType(UINT adapter, D3DDEVTYPE check_type, D3DFORMAT display_format, D3DFORMAT backbuffer_format, BOOL windowed) = 0;
	virtual HRESULT CheckDeviceFormat(UINT adapter, D3DDEVTYPE device_type, D3DFORMAT adapter_format, DWORD usage, D3DRESOURCETYPE resource_type, D3DFORMAT check_format) = 0;
	virtual HRESULT CheckDeviceMultiSampleType(UINT adapter, D3DDEVTYPE device_type, D3DFORMAT surface_format, BOOL windowed, D3DMULTISAMPLE_TYPE multi_sample_type) = 0;
	virtual HRESULT CheckDepthStencilMatch(UINT adapter, D3DDEVTYPE device_type, D3DFORMAT adapter_format, D3DFORMAT render_target_format, D3DFORMAT depth_stencil_format) = 0;
	virtual HRESULT GetDeviceCaps(UINT adapter, D3DDEVTYPE device_type, D3DCAPS8* caps) = 0;
};
