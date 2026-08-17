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
#include <vector>

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
// IDirect3DSurface8: a render target, a depth/stencil buffer, a texture's level or a
// system-memory image surface. D3D8 hands the engine one interface for all four and
// CopyRects/SetRenderTarget take them interchangeably, so this handle does too.
struct SurfaceHandle;

// D3D8 hands out DWORD shader handles, and the engine stores them as DWORD
// (W3DShaderManager::m_dwBasePixelShader and friends), so the seam does the same.
using ShaderHandle = uint32_t;
constexpr ShaderHandle kNullShader = 0;

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

// --- D3D8-shaped resource locking -------------------------------------------
// The engine's 95 Lock/Unlock call sites hand out a raw pointer and a pitch and
// expect D3D8's contract. These entry points keep that shape deliberately; the
// classes they have to serve, and what each costs over Vulkan, are in
// docs/porting/renderer-resource-seam.md.
enum LockFlags : uint32_t {
	LOCK_NONE = 0,
	LOCK_READONLY = 0x00000010,    // D3DLOCK_READONLY
	LOCK_NOOVERWRITE = 0x00001000, // D3DLOCK_NOOVERWRITE
	LOCK_DISCARD = 0x00002000,     // D3DLOCK_DISCARD
};

// D3DLOCKED_RECT.
struct LockedRect {
	void* bits = nullptr;
	uint32_t pitch = 0;
};

// RECT, in texels, right/bottom exclusive, as D3D8's LockRect takes it.
struct LockRect {
	uint32_t left = 0, top = 0, right = 0, bottom = 0;
};

// POINT, as CopyRects takes the destination corner.
struct SurfacePoint {
	uint32_t x = 0, y = 0;
};

// What emulating the D3D8 lock contract actually costs, counted rather than
// estimated. The resource-lock tests print these so the cost model in the doc is
// measured on at least one implementation.
struct ResourceStats {
	// The staging pool behind Lock_Texture: host-visible blocks that are recycled
	// across locks instead of one permanent allocation per resource. Nothing is freed
	// before Shutdown, so `staging_bytes` is the pool's resident cost: the sum of
	// every block it ever had to allocate.
	uint32_t staging_allocations = 0;
	uint64_t staging_bytes = 0;
	// What the pool holds free right now, what is checked out to a lock right now,
	// and the worst moment so far -- the peak is the number a frame's worth of
	// overlapping locks actually costs.
	uint32_t staging_pool_blocks = 0;
	uint64_t staging_pool_bytes = 0;
	uint64_t staging_live_bytes = 0;
	uint64_t staging_live_peak_bytes = 0;
	uint32_t staging_live_blocks = 0;
	uint32_t staging_live_blocks_peak = 0;
	// Lock calls that needed a block, and how many of those the free list served.
	// staging_allocations == staging_acquires - staging_reuses, always.
	uint32_t staging_acquires = 0;
	uint32_t staging_reuses = 0;
	// Blocks pinned for a resource's lifetime rather than recycled: ZH_SPIKE_STAGING_
	// RETAIN, the pre-pool behaviour, kept so the two can be measured against each
	// other.
	uint32_t staging_retained_blocks = 0;
	// Dynamic vertex-buffer memory, which is host-visible for the resource's whole
	// life by design (D3D8's D3DUSAGE_DYNAMIC ring) and so is *not* poolable. Counted
	// separately because it would otherwise be mistaken for staging that failed to
	// recycle.
	uint32_t dynamic_buffer_allocations = 0;
	uint64_t dynamic_buffer_bytes = 0;
	// vkCmdCopyBufferToImage regions issued from Unlock, and the submits they cost.
	uint32_t texture_upload_regions = 0;
	uint32_t upload_submits = 0;
	// Lock(READONLY) round trips: copy-to-buffer, submit, fence wait.
	uint32_t readback_stalls = 0;
	// The GPU-write hazard (renderer-resource-seam.md §4.4). `gpu_write_marks` is
	// how often a write funnel set a resource's dirty bit; `dirty_reads` the host
	// reads that had to pay a transfer or a wait because of it; `clean_reads` the
	// host reads that transferred nothing at all. A clean read that costs a
	// transfer shows up as a `dirty_reads` that should have been a `clean_reads`,
	// which is what the resource-lock tests assert on.
	uint32_t gpu_write_marks = 0;
	uint32_t dirty_reads = 0;
	uint32_t clean_reads = 0;
	uint64_t surface_readback_bytes = 0;
	// What preserving a pooled staging block's previous contents costs: the levels
	// brought back from the image because the lock was not a DISCARD, and the
	// locks that skipped it because nothing was there to preserve or the caller
	// proved it overwrites every byte.
	uint32_t staging_preserve_readbacks = 0;
	uint64_t staging_preserve_bytes = 0;
	uint32_t staging_preserve_skips = 0;
	// CPU channel expansions forced by the absence of view swizzle (MoltenVK).
	uint32_t cpu_expansions = 0;
	// Dynamic buffer ring: DISCARD renames, NOOVERWRITE appends, bytes handed out,
	// and the times a DISCARD had to wait because the ring wrapped onto a region the
	// GPU had not finished reading.
	uint32_t ring_discards = 0;
	uint32_t ring_appends = 0;
	uint64_t ring_bytes = 0;
	uint32_t ring_wrap_waits = 0;
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

// D3DVIEWPORT8.
struct ViewportRect {
	int32_t x = 0;
	int32_t y = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	float min_z = 0.0f;
	float max_z = 1.0f;
};

// D3DMATERIAL8.
struct MaterialState {
	float diffuse[4]{1.0f, 1.0f, 1.0f, 1.0f};
	float ambient[4]{1.0f, 1.0f, 1.0f, 1.0f};
	float specular[4]{0.0f, 0.0f, 0.0f, 0.0f};
	float emissive[4]{0.0f, 0.0f, 0.0f, 0.0f};
	float power = 0.0f;
};

// What the engine's D3D8 adapter enumeration and D3DCAPS8 need, measured from the
// Vulkan device rather than invented: DX8Wrapper::Enumerate_Devices asks IDirect3D8 for
// an adapter count, an identifier and a device caps block *before* it creates anything,
// so a backend has to be able to answer those questions without a device. The engine-side
// translation into D3DCAPS8 bits lives in the engine's backend adapter, which is the only
// layer that speaks D3D8; this struct is deliberately Vulkan-shaped numbers only.
struct AdapterInfo {
	std::string name;              // VkPhysicalDeviceProperties::deviceName
	std::string driver;            // driverName/driverInfo when VK_KHR_driver_properties is there
	uint32_t vendor_id = 0;
	uint32_t device_id = 0;
	uint32_t driver_version = 0;
	uint32_t api_version = 0;
	uint64_t device_memory_bytes = 0;   // sum of the DEVICE_LOCAL heaps
	uint32_t max_texture_dimension = 0; // limits.maxImageDimension2D
	uint32_t max_texture_stages = 0;    // sampled images per stage, clamped to kMaxTextureStages
	uint32_t max_vertex_index = 0;      // limits.maxDrawIndexedIndexValue, clamped to 16-bit reality
	uint32_t max_primitive_count = 0;
	float max_anisotropy = 1.0f;
	bool anisotropic_filtering = false;
	bool discrete = false;
	// One bit per TextureFormat this device can sample, as 1u << static_cast<int>(format).
	uint32_t sampled_formats = 0;
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
	// D3D8 splits EndScene from Present and the engine uses both halves -- it ends the
	// scene, then decides whether to present at all (DX8Wrapper::End_Scene(flip_frame)).
	// End_Scene(false) followed by Present() is that split; End_Scene(true) is the spike's
	// own convenience for a one-frame test. False when the swapchain could not be presented.
	virtual bool Present() = 0;

	// --- state: DX8Wrapper::Set_DX8_Render_State / _Texture_Stage_State -------
	// Raw D3D8 enum values, exactly as the engine's 370 SetRenderState and 865
	// SetTextureStageState sites pass them.
	virtual void Set_DX8_Render_State(D3DRENDERSTATETYPE state, uint32_t value) = 0;
	virtual void Set_DX8_Texture_Stage_State(uint32_t stage,
	                                         D3DTEXTURESTAGESTATETYPE state,
	                                         uint32_t value) = 0;
	virtual void Set_Transform(D3DTRANSFORMSTATETYPE transform, const Matrix4x4& m) = 0;
	virtual void Set_Texture(uint32_t stage, TextureHandle* texture) = 0;
	// GetRenderState/GetTransform answer from the shadowed copy, which is what the
	// D3D8 runtime does too rather than asking the device.
	virtual uint32_t Get_DX8_Render_State(D3DRENDERSTATETYPE state) const = 0;
	virtual void Get_Transform(D3DTRANSFORMSTATETYPE transform, Matrix4x4& out) const = 0;

	// --- fixed function: SetLight/LightEnable, SetMaterial, SetScissorRect ----
	// `light` may be null, which is D3D8's LightEnable(index, FALSE).
	virtual void Set_Light(uint32_t index, const LightState* light) = 0;
	virtual void Set_Material(const MaterialState& material) = 0;
	virtual void Set_Scissor(bool enable, int32_t x, int32_t y, int32_t width,
	                         int32_t height) = 0;
	// SetViewport/GetViewport: both the NDC-to-pixel mapping and the depth range.
	// The engine changes it per shadow and reflection pass.
	virtual void Set_Viewport(const ViewportRect& viewport) = 0;
	virtual void Get_Viewport(ViewportRect& out) const = 0;

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
	// The engine never creates an index buffer with its contents in hand: DX8IndexBufferClass
	// creates an empty one and fills it through Lock/Unlock, dynamic or not, so the D3D8
	// CreateIndexBuffer + LockRange pattern needs the same host-mapped ring the vertex path
	// already has. `dynamic` false is a static buffer written once and never renamed.
	virtual IndexBufferHandle* Create_Lockable_Index_Buffer(size_t count, bool dynamic) = 0;
	virtual bool Lock_Index_Buffer(IndexBufferHandle* ib, size_t offset_indices,
	                               size_t count, uint32_t flags, void** out_bits) = 0;
	virtual bool Unlock_Index_Buffer(IndexBufferHandle* ib) = 0;

	// --- lockable resources: the seam under investigation ---------------------
	// A texture whose contents arrive through Lock/Unlock rather than at creation,
	// i.e. what CreateTexture(D3DPOOL_MANAGED) + LockRect gives the engine.
	virtual TextureHandle* Create_Lockable_Texture(uint32_t width, uint32_t height,
	                                               TextureFormat format,
	                                               uint32_t mip_count) = 0;
	// `rect` null locks the whole level. Flags are the D3DLOCK_* subset above.
	virtual bool Lock_Texture(TextureHandle* texture, uint32_t level, const LockRect* rect,
	                          uint32_t flags, LockedRect& out) = 0;
	virtual bool Unlock_Texture(TextureHandle* texture, uint32_t level) = 0;

	// A dynamic vertex buffer, i.e. D3DUSAGE_DYNAMIC|WRITEONLY, locked with
	// DISCARD/NOOVERWRITE many times per frame.
	virtual VertexBufferHandle* Create_Dynamic_Vertex_Buffer(size_t bytes,
	                                                         uint32_t fvf) = 0;
	// The same buffer with the renaming ring made optional: D3D8's CreateVertexBuffer
	// without D3DUSAGE_DYNAMIC is still filled through Lock/Unlock but is never
	// DISCARDed, so one copy behind the handle is the whole difference.
	virtual VertexBufferHandle* Create_Lockable_Vertex_Buffer(size_t bytes, uint32_t fvf,
	                                                          bool dynamic) = 0;
	virtual bool Lock_Vertex_Buffer(VertexBufferHandle* vb, size_t offset, size_t size,
	                                uint32_t flags, void** out_bits) = 0;
	virtual bool Unlock_Vertex_Buffer(VertexBufferHandle* vb) = 0;

	virtual ResourceStats Get_Resource_Stats() const = 0;

	// --- render targets and surfaces: the SetRenderTarget/CopyRects group ------
	// D3D8's own shape, which the engine relies on: DX8Wrapper::Set_Render_Target
	// saves the device's current target with GetRenderTarget/GetDepthStencilSurface,
	// binds a texture's surface, and later restores the saved pair
	// (dx8wrapper.cpp:3336). What that costs over Vulkan render passes is in
	// docs/porting/renderer-surface.md.
	// A texture that can be both rendered into and sampled, i.e. D3D8's
	// CreateTexture(D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT).
	virtual TextureHandle* Create_Render_Target_Texture(uint32_t width,
	                                                    uint32_t height) = 0;
	// IDirect3DTexture8::GetSurfaceLevel. The surface stays owned by the texture.
	virtual SurfaceHandle* Get_Surface_Level(TextureHandle* texture, uint32_t level) = 0;
	virtual SurfaceHandle* Get_Render_Target() = 0;
	virtual SurfaceHandle* Get_Depth_Stencil_Target() = 0;
	// `color` null restores the default target, which is what the engine's restore
	// path passes. `depth_stencil` null renders with no depth buffer, as D3D8 allows.
	virtual bool Set_Render_Target(SurfaceHandle* color, SurfaceHandle* depth_stencil) = 0;
	// CreateImageSurface: a system-memory surface, the staging half of CopyRects.
	virtual SurfaceHandle* Create_Image_Surface(uint32_t width, uint32_t height,
	                                            TextureFormat format) = 0;
	// CopyRects. `rects`/`points` null copies the whole surface, as D3D8 defines it;
	// no stretching, and the two surfaces' D3D8 formats must be equal, also as D3D8
	// defines it. Equal D3D8 formats are not necessarily equal bytes in a backend that
	// emulates a format the device lacks (A4R4G4B4 held as B8G8R8A8, say), so an
	// implementation may have to convert *representations* on the way through; what it
	// must never do is reinterpret one representation as the other.
	virtual bool Copy_Rects(SurfaceHandle* source, const LockRect* rects,
	                        uint32_t rect_count, SurfaceHandle* destination,
	                        const SurfacePoint* points) = 0;
	// UpdateTexture: the managed-pool system-memory-to-video copy, level by level.
	virtual bool Update_Texture(TextureHandle* source, TextureHandle* destination) = 0;
	// IDirect3DSurface8::LockRect minus the flags, which is the shape
	// SurfaceClass::Lock has: one read-write pointer, no way for the caller to say
	// whether it reads. It is therefore treated as a read: a surface the GPU has
	// written since the host last saw it pays a readback (a copy for a
	// video-memory surface, a wait for a system-memory one whose CopyRects is
	// still queued), and a clean surface pays nothing at all.
	virtual bool Surface_Bits(SurfaceHandle* surface, LockedRect& out) = 0;

	// --- user clip planes: SetClipPlane ---------------------------------------
	// `plane` is A,B,C,D of Ax+By+Cz+Dw >= 0, in world space, as D3D8 defines it for
	// the fixed-function pipeline. D3DRS_CLIPPLANEENABLE selects which are applied.
	virtual void Set_Clip_Plane(uint32_t index, const float plane[4]) = 0;

	// --- programmable shaders: the ps.1.1/vs.1.1 group ------------------------
	// `function` is the D3D8 token stream the engine loads from a .pso/.vso file
	// (W3DShaderManager::LoadAndCreateD3DShader), not source text: a port has to
	// consume D3D8 shader tokens at runtime. Returns kNullShader when the program
	// uses something the backend cannot serve, rather than rendering it wrongly.
	virtual ShaderHandle Create_Pixel_Shader(const uint32_t* function) = 0;
	virtual void Delete_Pixel_Shader(ShaderHandle shader) = 0;
	// kNullShader returns to the fixed-function texture-stage cascade.
	virtual void Set_Pixel_Shader(ShaderHandle shader) = 0;
	virtual void Set_Pixel_Shader_Constant(uint32_t start_register, const void* data,
	                                       uint32_t vector4_count) = 0;
	// `declaration` is the D3DVSD_* token stream that maps v-registers onto the
	// vertex's elements; `function` may be null, which is D3D8's declaration-only
	// vertex shader (the fixed-function path, already served by the FVF).
	virtual ShaderHandle Create_Vertex_Shader(const uint32_t* declaration,
	                                          const uint32_t* function,
	                                          uint32_t usage) = 0;
	virtual void Delete_Vertex_Shader(ShaderHandle shader) = 0;
	virtual void Set_Vertex_Shader(ShaderHandle shader) = 0;
	virtual void Set_Vertex_Shader_Constant(uint32_t start_register, const void* data,
	                                        uint32_t vector4_count) = 0;

	virtual void Set_Vertex_Buffer(VertexBufferHandle* vb, uint32_t stream = 0) = 0;
	virtual void Set_Index_Buffer(IndexBufferHandle* ib, uint32_t index_base_offset) = 0;

	// --- draw: DX8Wrapper::Draw_Triangles -> DrawIndexedPrimitive ------------
	virtual void Draw_Triangles(uint32_t start_index, uint32_t polygon_count,
	                            uint32_t min_vertex_index, uint32_t vertex_count) = 0;
	// The general form of the same call, for the primitive types other than
	// D3DPT_TRIANGLELIST the engine draws: strips for terrain, points for snow.
	virtual void Draw_Indexed_Primitive(uint32_t primitive_type, uint32_t start_index,
	                                    uint32_t primitive_count,
	                                    uint32_t min_vertex_index,
	                                    uint32_t vertex_count) = 0;
	// DrawPrimitive: the bound vertex buffer, no index buffer.
	virtual void Draw_Primitive(uint32_t primitive_type, uint32_t start_vertex,
	                            uint32_t primitive_count) = 0;
	// DrawPrimitiveUP: vertices straight from host memory, no vertex buffer at all.
	// D3D8 leaves stream 0 unbound afterwards, and so does this.
	virtual void Draw_Primitive_UP(uint32_t primitive_type, uint32_t primitive_count,
	                               const void* vertex_data, uint32_t vertex_stride,
	                               uint32_t fvf) = 0;

	// --- windowed presentation: the DX8Wrapper::Reset_Device shape ------------
	// The window's client area changed size, so the swapchain no longer matches it. The
	// colour target keeps its own resolution in points and is scaled on present, which is the
	// cheap half of a device reset; what it does not keep is its *pixel* size, because the
	// window may have moved to a display with a different backing scale, so this re-reads the
	// scale and rebuilds the targets when it changed. A headless backend has nothing to do.
	virtual bool Resize_Presentation(uint32_t width, uint32_t height) {
		(void)width;
		(void)height;
		return true;
	}

	// --- HiDPI: points in, pixels out (docs/porting/hidpi-scale.md) ----------
	// Every size in this interface is in D3D8's own units - the client area's points - and the
	// backend multiplies by the backing scale at its own boundary: the colour target, the
	// viewport, the scissor and the readback are in pixels, the coordinates the engine passes
	// are not. At scale 1 the two are the same number and nothing below changes.
	//
	// Init() takes the scale from the window seam (Window_Backing_Scale) when it has one.
	// Set_Render_Scale() overrides it, which is how a headless run - CI, on Linux, at a scale no
	// Linux display has - exercises the pixel path; called before Init() it chooses the initial
	// scale, and after Init() it rebuilds the targets at the new one, which is what a window
	// dragged between a Retina and a non-Retina display needs.
	virtual bool Set_Render_Scale(float scale) { return scale == 1.0f; }
	virtual float Render_Scale() const { return 1.0f; }
	// The colour target's real size in pixels: client size x scale, rounded the way the
	// platform rounds its drawable. Zero before Init().
	virtual void Device_Pixel_Size(uint32_t& out_width, uint32_t& out_height) const {
		out_width = 0;
		out_height = 0;
	}

	// --- spike-only: prove what was rasterised -------------------------------
	// Reads the colour target back to host memory as tightly packed RGBA8.
	virtual bool Read_Back_Color_Target(std::string& out_rgba,
	                                    SurfaceFormat& out_format) = 0;

	// The chosen device's measured properties, for the D3DCAPS8 the engine asks the device
	// for after creation. False before Init().
	virtual bool Get_Adapter_Info(AdapterInfo& out) const = 0;

	virtual const char* Device_Description() const = 0;
	virtual uint32_t Pipeline_Count() const = 0;

	// Number of validation-layer warnings/errors seen. Zero is the point.
	virtual uint32_t Validation_Message_Count() const = 0;
	// Whether validation was actually running: the layer was found, the instance was created
	// with it, and the debug messenger that counts its messages exists. Without this,
	// Validation_Message_Count() == 0 cannot be told apart from "the layer never loaded", which
	// is the failure SIP produces on macOS (apple-silicon-verification.md 8.1). False when
	// validation was not requested.
	virtual bool Validation_Active() const = 0;
};

// Backing implementation lives in vulkan_backend.cpp.
RenderBackend* Create_Vulkan_Backend(bool enable_validation, bool headless);

// Every Vulkan device on the machine, in Vulkan's own order, measured through a temporary
// instance that is destroyed before this returns. This is what the engine's
// IDirect3D8::GetAdapterCount / GetAdapterIdentifier / GetDeviceCaps(adapter) need, and it
// deliberately does not create a rendering device: an adapter query that created a device
// would answer a different question from the one D3D8 asks.
bool Enumerate_Adapters(std::vector<AdapterInfo>& out, bool enable_validation);

} // namespace spike
