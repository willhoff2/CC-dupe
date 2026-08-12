// Renderer spike: a thin abstraction shaped like the engine's DX8Wrapper.
//
// The method names, argument order and semantics deliberately mirror
// Core/Libraries/Source/WWVegas/WW3D2/dx8wrapper.h so that the question the spike
// answers is the real one: can the *existing* call pattern be served by Vulkan
// without changing the call sites?
//
// Everything here is static-free and virtual, unlike DX8Wrapper (all-static). That
// is the one shape change a real port needs, and it is mechanical: DX8Wrapper's
// statics become forwarding calls onto a single backend instance.

#pragma once

#include "d3d8_subset.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace spike {

struct Matrix4x4 {
	float m[4][4];
	static Matrix4x4 Identity();
};

// Opaque handles. DX8Wrapper hands out IDirect3DTexture8*/IDirect3DVertexBuffer8*;
// a retargeted wrapper hands out these instead. Both are pointer-sized, which is
// why the 458 engine call sites that only pass them through do not care.
struct TextureHandle;
struct VertexBufferHandle;
struct IndexBufferHandle;

struct SurfaceFormat {
	uint32_t width = 0;
	uint32_t height = 0;
};

// Every surface format ww3dformat.cpp's Get_Valid_Texture_Format can return, plus
// the two source formats targa.cpp/ddsfile.cpp decode from. Named after the
// D3DFMT_* the engine uses so the mapping in vulkan_backend.cpp is checkable.
enum class TextureFormat {
	A8R8G8B8,
	X8R8G8B8,
	R8G8B8,
	A4R4G4B4,
	A1R5G5B5,
	R5G6B5,
	L8,
	A8,
	A8L8,
	V8U8, // bump-map delta pair, D3DFMT_V8U8
	P8,   // 8-bit palettised, needs `palette`
	DXT1,
	DXT2,
	DXT3,
	DXT4,
	DXT5,
};

struct TextureMip {
	const void* data = nullptr;
	size_t bytes = 0;
	uint32_t width = 0;
	uint32_t height = 0;
};

struct TextureDesc {
	TextureFormat format = TextureFormat::A8R8G8B8;
	uint32_t mip_count = 1;
	const TextureMip* mips = nullptr;
	// 256 D3DCOLOR entries; required for TextureFormat::P8, ignored otherwise.
	const uint32_t* palette = nullptr;
};

// D3DLIGHT8, minus the fields the engine never fills.
struct LightState {
	uint32_t type = 0; // D3DLIGHTTYPE; 0 disables the slot
	float diffuse[4]{0.0f, 0.0f, 0.0f, 0.0f};
	float specular[4]{0.0f, 0.0f, 0.0f, 0.0f};
	float ambient[4]{0.0f, 0.0f, 0.0f, 0.0f};
	float position[3]{0.0f, 0.0f, 0.0f};
	float direction[3]{0.0f, 0.0f, 1.0f};
	float range = 1.0e30f;
	float falloff = 1.0f;
	float attenuation0 = 1.0f;
	float attenuation1 = 0.0f;
	float attenuation2 = 0.0f;
	float theta = 0.0f; // inner cone, full angle in radians
	float phi = 0.0f;   // outer cone, full angle in radians
};

// D3DMATERIAL8.
struct MaterialState {
	float diffuse[4]{1.0f, 1.0f, 1.0f, 1.0f};
	float ambient[4]{1.0f, 1.0f, 1.0f, 1.0f};
	float specular[4]{0.0f, 0.0f, 0.0f, 0.0f};
	float emissive[4]{0.0f, 0.0f, 0.0f, 0.0f};
	float power = 0.0f;
};

class RenderBackend {
public:
	virtual ~RenderBackend() = default;

	// --- device lifetime: DX8Wrapper::Init / Shutdown -------------------------
	virtual bool Init(void* window_handle, uint32_t width, uint32_t height) = 0;
	virtual void Shutdown() = 0;

	// --- frame: DX8Wrapper::Begin_Scene / End_Scene / Clear -------------------
	virtual void Begin_Scene() = 0;
	virtual void End_Scene(bool flip_frame = true) = 0;
	virtual void Clear(bool clear_color, bool clear_z_stencil,
	                   float r, float g, float b, float dest_alpha = 0.0f,
	                   float z = 1.0f, uint32_t stencil = 0) = 0;

	// --- state: DX8Wrapper::Set_DX8_Render_State / _Texture_Stage_State -------
	// Raw D3D8 enum values, exactly as the engine's 370 SetRenderState and 865
	// SetTextureStageState sites pass them.
	virtual void Set_DX8_Render_State(D3DRENDERSTATETYPE state, uint32_t value) = 0;
	virtual void Set_DX8_Texture_Stage_State(uint32_t stage,
	                                         D3DTEXTURESTAGESTATETYPE state,
	                                         uint32_t value) = 0;
	virtual void Set_Transform(D3DTRANSFORMSTATETYPE transform, const Matrix4x4& m) = 0;
	virtual void Set_Texture(uint32_t stage, TextureHandle* texture) = 0;

	// --- fixed function: SetLight/LightEnable, SetMaterial, SetScissorRect ----
	// `light` may be null, which is D3D8's LightEnable(index, FALSE).
	virtual void Set_Light(uint32_t index, const LightState* light) = 0;
	virtual void Set_Material(const MaterialState& material) = 0;
	virtual void Set_Scissor(bool enable, int32_t x, int32_t y, int32_t width,
	                         int32_t height) = 0;

	// --- geometry: DX8Wrapper::Set_Vertex_Buffer / Set_Index_Buffer -----------
	// fvf is the raw D3DFVF_* bitfield the engine feeds to SetVertexShader.
	virtual TextureHandle* Create_Texture(uint32_t width, uint32_t height,
	                                      const uint8_t* argb_pixels) = 0;
	// Full form: any format the engine's loaders can produce, with a mip chain.
	virtual TextureHandle* Create_Texture(const TextureDesc& desc) = 0;
	// False when the device cannot sample the format at all (as opposed to the
	// backend converting it on upload). Lets a test report a negative finding
	// instead of asserting on a substituted format.
	virtual bool Supports_Texture_Format(TextureFormat format) const = 0;
	virtual VertexBufferHandle* Create_Vertex_Buffer(const void* data, size_t bytes,
	                                                 uint32_t fvf) = 0;
	virtual IndexBufferHandle* Create_Index_Buffer(const uint16_t* data,
	                                               size_t count) = 0;

	virtual void Set_Vertex_Buffer(VertexBufferHandle* vb, uint32_t stream = 0) = 0;
	virtual void Set_Index_Buffer(IndexBufferHandle* ib, uint32_t index_base_offset) = 0;

	// --- draw: DX8Wrapper::Draw_Triangles -> DrawIndexedPrimitive ------------
	virtual void Draw_Triangles(uint32_t start_index, uint32_t polygon_count,
	                            uint32_t min_vertex_index, uint32_t vertex_count) = 0;

	// --- windowed presentation: the DX8Wrapper::Reset_Device shape ------------
	// The window's client area changed size, so the swapchain no longer matches it. The
	// colour target keeps its own resolution and is scaled on present, which is the cheap
	// half of a device reset. A headless backend has nothing to do.
	virtual bool Resize_Presentation(uint32_t width, uint32_t height) {
		(void)width;
		(void)height;
		return true;
	}

	// --- spike-only: prove what was rasterised -------------------------------
	// Reads the colour target back to host memory as tightly packed RGBA8.
	virtual bool Read_Back_Color_Target(std::string& out_rgba,
	                                    SurfaceFormat& out_format) = 0;

	virtual const char* Device_Description() const = 0;
	virtual uint32_t Pipeline_Count() const = 0;

	// Number of validation-layer warnings/errors seen. Zero is the point.
	virtual uint32_t Validation_Message_Count() const = 0;
};

// Backing implementation lives in vulkan_backend.cpp.
RenderBackend* Create_Vulkan_Backend(bool enable_validation, bool headless);

} // namespace spike
