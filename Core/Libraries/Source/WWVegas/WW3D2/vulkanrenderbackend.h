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
 *                    File Name : vulkanrenderbackend.h                                        *
 *                                                                                             *
 *---------------------------------------------------------------------------------------------*/

#pragma once

#ifndef _WIN32

#include "renderbackend.h"

/**
** VulkanRenderBackendClass
**
** The non-Windows half of the RenderBackendClass seam: the engine's D3D8-shaped backend
** interface implemented over the Vulkan backend in spikes/renderer, which is built into this
** binary as a support library (scripts/native-build.py, slug support_renderbackend) rather
** than copied here.  Nothing in this file is a second renderer; it is a translation layer,
** and every line of Vulkan lives on the other side of spike::RenderBackend.
**
** What "translation" has to cover, because the two interfaces are not the same shape:
**
**  - D3D8 hands the engine COM resource pointers and the engine calls Lock/Unlock/AddRef on
**    them directly in 213 places (docs/porting/renderer-seam.md).  The spike hands out opaque
**    handles.  So this file implements IDirect3DTexture8/Surface8/VertexBuffer8/IndexBuffer8
**    over those handles, with real reference counting.  They are the only D3D8 interface
**    implementations in the engine that are not the Windows runtime's.
**  - D3D8 answers capability questions before a device exists.  spike::Enumerate_Adapters
**    measures the Vulkan devices without creating one, and Fill_Caps below turns those
**    measurements into the D3DCAPS8 bits DX8Caps reads.
**
** Honesty rules this file obeys, because a renderer that lies is worse than one that fails:
**
**  - An operation the backend cannot perform returns a D3D8 failure code and is counted in the
**    unimplemented-call ledger below.  Nothing returns D3D_OK for work it did not do.
**  - Capability bits are set from measurements or from what this translation layer actually
**    implements; none is set to get past a check.
**  - Pixels here, points in the engine: Create_Device is given the back buffer size the engine
**    computed and passes it to the backend unchanged, which is the renderer boundary the point/
**    pixel policy names (docs/porting/decisions-resolved.md).
*/
class VulkanRenderBackendClass : public RenderBackendClass
{
public:
	VulkanRenderBackendClass();
	virtual ~VulkanRenderBackendClass();

	/*
	** Backend and device lifecycle
	*/
	virtual bool Open();
	virtual void Release_Interface();
	virtual void Free_Library();
	virtual bool Has_Interface() const;
	virtual HRESULT Create_Device(UINT adapter, D3DDEVTYPE device_type, HWND focus_window, DWORD behavior_flags, D3DPRESENT_PARAMETERS* present_parameters);
	virtual void Release_Device();
	virtual bool Has_Device() const;

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
	** Resource creation
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

	/*
	** The unimplemented-call ledger.  Every D3D8 entry point above that this backend cannot
	** perform records itself here the first time the engine reaches it, with a count, so
	** "what does the engine ask for that the Vulkan backend has never been asked for" is
	** answered by running the engine rather than by reading the code.  Reported by
	** Log_Unimplemented_Calls() and by scripts/ci/check-backend-coverage.py's ledger check.
	*/
	struct UnimplementedCallClass
	{
		const char * Name;
		const char * Why;
		unsigned Count;
	};
	static unsigned Unimplemented_Call_Kinds();
	static const UnimplementedCallClass * Unimplemented_Call(unsigned index);
	static void Log_Unimplemented_Calls();
	// A chosen fallback reported from outside (RenderBackendClass) is a ledger entry too.
	virtual void Record_Unserviceable(const char * name, const char * why);

	/*
	** Frame proof.  NOT a D3D8 entry point and never called by the engine: this reads the colour
	** target the engine just drew back to host memory and measures it, so a harness can say what
	** is IN a frame instead of trusting Present's HRESULT.  The project's worst defect to date was
	** 13,500 "successful" frames of an empty map, so a presented frame is only evidence once its
	** contents have been read.
	*/
	struct FrameProofClass
	{
		unsigned Width;
		unsigned Height;
		unsigned long Pixels;			// pixels read back
		unsigned long Matching;			// pixels within Tolerance of the expected colour
		unsigned char MinRGB[3];
		unsigned char MaxRGB[3];
		unsigned char CentreRGBA[4];
	};
	bool Measure_Frame(unsigned char expect_r, unsigned char expect_g, unsigned char expect_b,
		unsigned char tolerance, const char * png_path, FrameProofClass & proof);

	/*
	** Validation-layer messages the backend has seen, so a run can report that the layer was
	** loaded AND silent rather than only that nothing crashed.  -1 before a device exists.
	*/
	long Validation_Message_Count() const;

private:
	/*
	** Everything Vulkan-shaped, in an opaque struct, because spikes/renderer's headers
	** redeclare the D3D8 enum vocabulary in namespace spike and d3d8.h defines some of those
	** same spellings as macros: the two cannot meet in a header the engine includes.  The
	** translation unit includes the spike headers first and this one second, which is the
	** only order in which both parse (docs/porting/renderer-integration.md).
	*/
	struct InternalsStruct;
	InternalsStruct * Internals;

	VulkanRenderBackendClass(const VulkanRenderBackendClass &);
	VulkanRenderBackendClass & operator = (const VulkanRenderBackendClass &);
};

/*
** The instance DX8Wrapper installs off Windows.  One backend, statically constructed, exactly
** as TheD3D8RenderBackend is on Windows: DX8Wrapper::RenderBackend points at it from static
** initialisation, so there is no window in which it is null.
*/
extern VulkanRenderBackendClass TheVulkanRenderBackend;

#endif // !_WIN32
