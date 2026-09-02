// Renderer spike: a Vulkan implementation of the DX8Wrapper-shaped interface.
//
// Deliberately written in the style a real port would need: the engine's
// one-state-at-a-time D3D8 calls are recorded into a shadow state block, and the
// pipeline is materialised lazily at draw time from a hash of that block. Nothing
// here is engine-specific; it is the machinery every strategy in the write-up needs.
//
// What this file does NOT do, and a real backend must: mipmap generation,
// depth-stencil readback, DXT decode, device-lost handling, or multiple streams.

#include "render_backend.h"
#include "state_translate.h"

#include <vulkan/vulkan.h>

#ifdef SPIKE_WITH_SDL
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#endif

// The window/event-loop/input seam. When it is present the window handle passed to Init() is
// a WWPlatform window rather than an SDL_Window, and the surface comes from the seam - which
// is how the CAMetalLayer reaches MoltenVK on macOS. See
// Core/Libraries/Source/WWVegas/WWLib/platform/platform_window.h.
#ifdef SPIKE_WITH_PLATFORM_WINDOW
#include "platform/platform_window.h"
#endif

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace spike {

Matrix4x4 Matrix4x4::Identity() {
	Matrix4x4 r{};
	for (int i = 0; i < 4; ++i) r.m[i][i] = 1.0f;
	return r;
}

namespace {

// Descriptor sets and draw-uniform slices are allocated in blocks of this many draws. This
// is a growth granularity, not a limit: a frame that needs more allocates more blocks and
// keeps them for the frames that follow (docs/porting/draws-per-frame.md).
constexpr uint32_t kDrawsPerBlock = 256;
// Renamed copies behind one dynamic vertex buffer: one per frame that can be in
// flight, plus the one being written. D3D8's DISCARD promises the driver hands back
// memory the GPU is not reading, and this is how many copies that costs.
constexpr uint32_t kDynamicRingRegions = 3;
constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;
// The engine asks for D3DFMT_D24S8 and uses the stencil for shadow volumes, so the
// depth target must have a stencil component. MoltenVK does not expose
// D24_UNORM_S8_UINT (docs/porting/moltenvk-findings.md), hence the fallback -- one
// of the two is required to be supported by every Vulkan implementation.
VkFormat Pick_Depth_Stencil_Format(VkPhysicalDevice physical) {
	const VkFormat candidates[] = {VK_FORMAT_D24_UNORM_S8_UINT,
	                               VK_FORMAT_D32_SFLOAT_S8_UINT};
	for (VkFormat format : candidates) {
		VkFormatProperties props{};
		vkGetPhysicalDeviceFormatProperties(physical, format, &props);
		if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
			return format;
	}
	return VK_FORMAT_D32_SFLOAT_S8_UINT;
}

#define VK_CHECK(expr)                                                              \
	do {                                                                            \
		VkResult vk_check_result = (expr);                                          \
		if (vk_check_result != VK_SUCCESS) {                                        \
			std::fprintf(stderr, "%s:%d: %s failed with VkResult %d\n", __FILE__,   \
			             __LINE__, #expr, static_cast<int>(vk_check_result));       \
			return false;                                                           \
		}                                                                           \
	} while (0)

// Row-vector (D3D/Westwood) matrix product: out = a * b.
Matrix4x4 Multiply(const Matrix4x4& a, const Matrix4x4& b) {
	Matrix4x4 r{};
	for (int i = 0; i < 4; ++i)
		for (int j = 0; j < 4; ++j) {
			float s = 0.0f;
			for (int k = 0; k < 4; ++k) s += a.m[i][k] * b.m[k][j];
			r.m[i][j] = s;
		}
	return r;
}

// D3D8 matrices are row-vector (v * M); GLSL's mat4 is column-vector (M * v), so
// uploading one is a transpose. flip_y negates the row that produces clip-space y,
// because Vulkan's clip space has +y down where D3D's has +y up.
void Store_Matrix(const Matrix4x4& m, float* out, bool flip_y = false) {
	for (int i = 0; i < 4; ++i)
		for (int j = 0; j < 4; ++j) out[i * 4 + j] = m.m[i][j];
	if (flip_y)
		for (int column = 0; column < 4; ++column) out[column * 4 + 1] = -out[column * 4 + 1];
}

struct Buffer {
	VkBuffer buffer = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkDeviceSize size = 0;
};

struct Image {
	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t mip_levels = 1;
	// The GPU-write dirty bit (renderer-resource-seam.md §4.4): the image holds
	// pixels no host copy has. Set at every write funnel, cleared by the readback a
	// host read performs. Read locks on a clean image transfer nothing, which is the
	// whole point of tracking it rather than reading back unconditionally.
	bool gpu_dirty = false;
	// The lockable texture this image belongs to, so a write funnel that only has
	// the image (or a surface view of it) can invalidate that texture's staging
	// copy. Null for the default targets and for non-lockable images.
	TextureHandle* owner = nullptr;
};

// Sampler state in D3D8 is texture *stage* state, not part of the texture object.
// In Vulkan it is a VkSampler bound alongside the image, so it needs its own cache.
struct SamplerKey {
	uint32_t min_filter, mag_filter, mip_filter, address_u, address_v, address_w;
	// D3DTSS_BORDERCOLOR as the engine passes it (a D3DCOLOR) and D3DTSS_MAXANISOTROPY.
	uint32_t border_color, max_anisotropy;
	bool operator==(const SamplerKey& o) const {
		return min_filter == o.min_filter && mag_filter == o.mag_filter &&
		       mip_filter == o.mip_filter && address_u == o.address_u &&
		       address_v == o.address_v && address_w == o.address_w &&
		       border_color == o.border_color && max_anisotropy == o.max_anisotropy;
	}
};

struct SamplerKeyHash {
	size_t operator()(const SamplerKey& k) const {
		size_t h = 1469598103934665603ull;
		for (uint32_t v : {k.min_filter, k.mag_filter, k.mip_filter, k.address_u,
		                   k.address_v, k.address_w, k.border_color, k.max_anisotropy}) {
			h = (h ^ v) * 1099511628211ull;
		}
		return h;
	}
};

// One D3D8 texture stage's worth of SetTextureStageState. Field for field, this is
// DX8Wrapper::TextureStageStates[stage][*] restricted to the states the engine
// writes (tools/texture-stage-scan.py: 23 of D3D8's 32 state types).
struct PerStage {
	uint32_t color_op = D3DTOP_DISABLE;
	uint32_t color_arg1 = D3DTA_TEXTURE;
	uint32_t color_arg2 = D3DTA_CURRENT;
	uint32_t color_arg0 = D3DTA_CURRENT; // D3DTOP_MULTIPLYADD/LERP third input
	uint32_t alpha_op = D3DTOP_DISABLE;
	uint32_t alpha_arg1 = D3DTA_TEXTURE;
	uint32_t alpha_arg2 = D3DTA_CURRENT;
	uint32_t alpha_arg0 = D3DTA_CURRENT;
	uint32_t texcoord_index = 0;
	uint32_t transform_flags = D3DTTFF_DISABLE;
	uint32_t result_arg = D3DTA_CURRENT;
	uint32_t min_filter = D3DTEXF_LINEAR;
	uint32_t mag_filter = D3DTEXF_LINEAR;
	uint32_t mip_filter = D3DTEXF_NONE;
	uint32_t address_u = D3DTADDRESS_WRAP;
	uint32_t address_v = D3DTADDRESS_WRAP;
	uint32_t address_w = D3DTADDRESS_WRAP;
	uint32_t border_color = 0;   // D3DTSS_BORDERCOLOR, a D3DCOLOR
	uint32_t max_anisotropy = 1; // D3DTSS_MAXANISOTROPY
	// D3DTSS_BUMPENVMAT00/01/10/11 and the luminance pair. The engine passes these
	// as float bit patterns through its F2DW macro (W3DWater.cpp), so they are
	// stored as floats here after the bit-cast.
	float bump_matrix[4]{0.0f, 0.0f, 0.0f, 0.0f};
	float bump_luminance_scale = 0.0f;
	float bump_luminance_offset = 0.0f;
};

// D3D8 hands floats through SetTextureStageState/SetRenderState as raw DWORDs.
float Dword_To_Float(uint32_t value) {
	float f = 0.0f;
	std::memcpy(&f, &value, sizeof(f));
	return f;
}

uint32_t Float_To_Dword(float value) {
	uint32_t d = 0;
	std::memcpy(&d, &value, sizeof(d));
	return d;
}

void Unpack_D3dcolor(uint32_t color, float* out_rgba) {
	out_rgba[0] = ((color >> 16) & 0xff) / 255.0f;
	out_rgba[1] = ((color >> 8) & 0xff) / 255.0f;
	out_rgba[2] = (color & 0xff) / 255.0f;
	out_rgba[3] = ((color >> 24) & 0xff) / 255.0f;
}

std::vector<uint32_t> Read_Spirv(const std::string& path) {
	std::vector<uint32_t> code;
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f) return code;
	std::fseek(f, 0, SEEK_END);
	long size = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	if (size > 0 && size % 4 == 0) {
		code.resize(static_cast<size_t>(size) / 4);
		if (std::fread(code.data(), 1, static_cast<size_t>(size), f) != static_cast<size_t>(size)) {
			code.clear();
		}
	}
	std::fclose(f);
	return code;
}

} // namespace

// A host-visible, mapped block of staging memory the pool hands to a lock and takes
// back at the matching unlock. `capacity` is what was allocated (rounded up to a
// size class so blocks are interchangeable); `size` is what the lock asked for.
struct StagingBlock {
	Buffer buffer;
	void* mapped = nullptr;
	VkDeviceSize size = 0;
	VkDeviceSize capacity = 0;
	bool valid() const { return mapped != nullptr; }
};

// One mip level of a lockable texture: where in its staging block the texels live,
// and the pitch handed to the caller. Tightly packed rows, so the pitch is the level
// width in bytes -- D3D8 does not promise any particular pitch, only that the caller
// uses the one it is given.
struct LockableLevel {
	VkDeviceSize offset = 0;
	uint32_t pitch = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	bool locked = false;
	uint32_t lock_flags = 0;
	LockRect lock_rect{};
	// Whether the staging block currently holds this level's contents. False after
	// the block goes back to the pool (another resource's lock may have written it)
	// and after a GPU write to the image. A lock that is not a DISCARD and finds
	// this false has to bring the contents back, because D3D8's Lock preserves them.
	bool staging_synced = false;
	// Whether anything has ever been written to this level. A level nobody has
	// written has no contents to preserve -- D3D8 leaves a fresh texture's texels
	// undefined -- so the first lock of it skips the readback.
	bool ever_written = false;
};

struct TextureHandle {
	Image image;
	// D3DUSAGE_RENDERTARGET: the image can be a colour attachment as well as sampled,
	// and Get_Surface_Level hands out a SurfaceHandle for it.
	bool render_target = false;

	// --- lockable path (see docs/porting/renderer-resource-seam.md) ------------
	bool lockable = false;
	TextureFormat format = TextureFormat::A8R8G8B8;
	VkFormat vk_format = VK_FORMAT_B8G8R8A8_UNORM;
	bool expand_on_unlock = false;
	// Bytes per texel in the format the *caller* writes, which is the D3D8 format,
	// not necessarily the VkFormat the image has.
	uint32_t src_texel_bytes = 4;
	uint32_t dst_texel_bytes = 4;
	// Staging for the whole mip chain, taken from the pool at the first Lock and
	// returned at the last matching Unlock, so a resource nobody is locking costs no
	// host-visible memory. A D3D8 lock may hand out a pointer that outlives the Lock
	// call (class C4): the block is held while *any* level of the chain is locked,
	// which is exactly that lifetime. `retain_staging` pins the block for the
	// resource's lifetime instead, which is what classes C7 and C8 need.
	StagingBlock staging;
	void* staging_mapped = nullptr;
	bool retain_staging = false;
	uint32_t locked_levels = 0;
	// Bytes the CPU-expanded upload block needs (whole chain as BGRA8). Only the
	// no-view-swizzle path uses it, and only for the duration of one unlock's copy.
	VkDeviceSize upload_size = 0;
	std::vector<LockableLevel> levels;
	VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

struct VertexBufferHandle {
	Buffer buffer;
	VertexLayout layout;
	uint32_t fvf = 0;
	// CreateVertexBuffer with FVF 0: D3D8 lets a buffer carry bytes whose layout is named
	// later, by the FVF (or declaration) bound through SetVertexShader when it is drawn.
	// `layout` and `fvf` are meaningless for such a buffer; Prepare_Draw resolves them from
	// the fixed-function FVF in force at the draw.
	bool untyped = false;

	// --- dynamic ring (class C5) ----------------------------------------------
	bool dynamic = false;
	VkDeviceSize capacity = 0; // bytes the engine thinks the buffer has
	uint32_t region_count = 1; // renamed copies behind that one buffer
	uint32_t region = 0;       // which copy the next draw reads
	// Frame each region was last drawn from, so a DISCARD knows whether renaming
	// onto it would overwrite bytes the GPU has not read yet.
	std::vector<uint64_t> region_last_use;
	void* mapped = nullptr;
	VkDeviceSize bind_offset = 0;
};

struct IndexBufferHandle {
	Buffer buffer;
	uint32_t count = 0;

	// --- lockable / dynamic ring, the same shape the vertex path has --------------
	bool dynamic = false;
	VkDeviceSize capacity = 0; // bytes the engine thinks the buffer has
	uint32_t region_count = 1;
	uint32_t region = 0;
	std::vector<uint64_t> region_last_use;
	void* mapped = nullptr;
	VkDeviceSize bind_offset = 0;
};

// IDirect3DSurface8. Two shapes behind one handle, exactly as D3D8 has it:
//   video memory   a view of an image the backend owns -- the default colour or
//                  depth target, or level 0 of a render-target texture
//   system memory  CreateImageSurface: host-visible bytes with no image at all,
//                  which is the staging half of CopyRects
struct SurfaceHandle {
	Image* image = nullptr;        // null for a system-memory surface
	TextureHandle* owner = nullptr; // null for the default targets
	bool depth_stencil = false;
	Buffer bits;                   // system-memory surface only
	void* mapped = nullptr;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t pitch = 0;
	uint32_t texel_bytes = 4;
	// The D3D8 format the caller sees, which is not derivable from vk_format: every
	// CPU-expanded format becomes VK_FORMAT_B8G8R8A8_UNORM, so two surfaces can share a
	// VkFormat and still have different texel layouts. CopyRects has to compare the D3D8
	// formats, which is also the pair D3D8 itself requires to match.
	TextureFormat format = TextureFormat::A8R8G8B8;
	VkFormat vk_format = VK_FORMAT_B8G8R8A8_UNORM;
	// Tracked rather than assumed: a surface is an attachment, a transfer source, a
	// transfer destination and a sampled image at different points in one frame, and
	// every one of those is a different Vulkan layout.
	VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
	// Whether anything has been drawn into it since Begin_Scene, which is what
	// decides between a LOAD and a DONT_CARE render pass when it is bound again.
	bool written_this_frame = false;
	// System-memory surface: a CopyRects into it has been recorded but the host has
	// not waited for it. This is the screenshot/movie-capture hazard -- the copy is
	// a queue operation and the mapped bytes are stale until it has executed. A
	// video-memory surface's dirty bit lives on the image it views, so that the
	// texture, its surface and any other view of it agree.
	bool host_gpu_dirty = false;

	bool system_memory() const { return image == nullptr; }
	bool gpu_dirty() const {
		return system_memory() ? host_gpu_dirty : image->gpu_dirty;
	}
};

// One parsed D3D8 shader token stream. The tokens travel to the shader as they
// arrived; what the parse establishes is that the program is one the interpreter
// can run, and how long it is.
struct ShaderProgram {
	bool pixel = false;
	uint32_t version = 0;
	uint32_t instruction_count = 0;
	int32_t tokens[kMaxShaderInstructions][8]{};
	// `def cN, x, y, z, w`: constants the shader carries itself, which D3D8 applies
	// when the shader is set rather than through SetPixelShaderConstant.
	std::vector<std::pair<uint32_t, std::array<float, 4>>> defs;
	// The v-registers the D3DVSD_* declaration names, in declaration order. The k-th
	// one is fed by the k-th element the bound FVF supplies, which is D3D8's mapping.
	std::vector<uint32_t> declared_inputs;
	// The stream layout the same declaration describes, which is what an *untyped*
	// (FVF 0) buffer is read with while this program is bound; a typed buffer keeps its
	// own FVF's layout.
	VertexLayout declared_layout;
	uint32_t declared_layout_hash = 0;
};

class VulkanBackend final : public RenderBackend {
public:
	VulkanBackend(bool enable_validation, bool headless)
	    : validation_(enable_validation), headless_(headless) {}
	~VulkanBackend() override { Shutdown(); }

	bool Init(void* window_handle, uint32_t width, uint32_t height) override;
	void Shutdown() override;

	void Begin_Scene() override;
	void End_Scene(bool flip_frame) override;
	void Clear(bool clear_color, bool clear_z_stencil, float r, float g, float b,
	           float dest_alpha, float z, uint32_t stencil) override;

	void Set_DX8_Render_State(D3DRENDERSTATETYPE state, uint32_t value) override;
	void Set_DX8_Texture_Stage_State(uint32_t stage, D3DTEXTURESTAGESTATETYPE state,
	                                 uint32_t value) override;
	void Set_Transform(D3DTRANSFORMSTATETYPE transform, const Matrix4x4& m) override;
	void Set_Texture(uint32_t stage, TextureHandle* texture) override;

	void Set_Light(uint32_t index, const LightState* light) override;
	void Set_Material(const MaterialState& material) override;
	void Set_Scissor(bool enable, int32_t x, int32_t y, int32_t width,
	                 int32_t height) override;
	void Set_Viewport(const ViewportRect& viewport) override;
	void Get_Viewport(ViewportRect& out) const override { out = viewport_; }
	uint32_t Get_DX8_Render_State(D3DRENDERSTATETYPE state) const override {
		return static_cast<uint32_t>(state) < D3DRS_MAX ? render_states_[state] : 0;
	}
	void Get_Transform(D3DTRANSFORMSTATETYPE transform, Matrix4x4& out) const override;

	TextureHandle* Create_Texture(uint32_t width, uint32_t height,
	                              const uint8_t* argb_pixels) override;
	TextureHandle* Create_Texture(const TextureDesc& desc) override;
	bool Supports_Texture_Format(TextureFormat format) const override;
	VertexBufferHandle* Create_Vertex_Buffer(const void* data, size_t bytes,
	                                         uint32_t fvf) override;
	IndexBufferHandle* Create_Index_Buffer(const uint16_t* data, size_t count) override;
	IndexBufferHandle* Create_Lockable_Index_Buffer(size_t count, bool dynamic) override;
	bool Lock_Index_Buffer(IndexBufferHandle* ib, size_t offset_indices, size_t count,
	                       uint32_t flags, void** out_bits) override;
	bool Unlock_Index_Buffer(IndexBufferHandle* ib) override;
	bool Get_Adapter_Info(AdapterInfo& out) const override;

	TextureHandle* Create_Lockable_Texture(uint32_t width, uint32_t height,
	                                       TextureFormat format,
	                                       uint32_t mip_count) override;
	bool Lock_Texture(TextureHandle* texture, uint32_t level, const LockRect* rect,
	                  uint32_t flags, LockedRect& out) override;
	bool Unlock_Texture(TextureHandle* texture, uint32_t level) override;
	VertexBufferHandle* Create_Dynamic_Vertex_Buffer(size_t bytes, uint32_t fvf) override;
	VertexBufferHandle* Create_Lockable_Vertex_Buffer(size_t bytes, uint32_t fvf,
	                                                 bool dynamic) override;
	bool Lock_Vertex_Buffer(VertexBufferHandle* vb, size_t offset, size_t size,
	                        uint32_t flags, void** out_bits) override;
	bool Unlock_Vertex_Buffer(VertexBufferHandle* vb) override;
	ResourceStats Get_Resource_Stats() const override {
		std::lock_guard<std::mutex> guard(resource_mutex_);
		return resource_stats_;
	}

	void Set_Fixed_Function_Fvf(uint32_t fvf) override;
	void Set_Vertex_Buffer(VertexBufferHandle* vb, uint32_t stream, uint32_t stride) override;
	void Set_Index_Buffer(IndexBufferHandle* ib, uint32_t index_base_offset) override;

	void Draw_Triangles(uint32_t start_index, uint32_t polygon_count,
	                    uint32_t min_vertex_index, uint32_t vertex_count) override;
	void Draw_Indexed_Primitive(uint32_t primitive_type, uint32_t start_index,
	                            uint32_t primitive_count, uint32_t min_vertex_index,
	                            uint32_t vertex_count) override;
	void Draw_Primitive(uint32_t primitive_type, uint32_t start_vertex,
	                    uint32_t primitive_count) override;
	void Draw_Primitive_UP(uint32_t primitive_type, uint32_t primitive_count,
	                       const void* vertex_data, uint32_t vertex_stride,
	                       uint32_t fvf) override;

	bool Read_Back_Color_Target(std::string& out_rgba, SurfaceFormat& out_format) override;

	TextureHandle* Create_Render_Target_Texture(uint32_t width, uint32_t height) override;
	SurfaceHandle* Get_Surface_Level(TextureHandle* texture, uint32_t level) override;
	SurfaceHandle* Get_Render_Target() override { return current_color_; }
	SurfaceHandle* Get_Depth_Stencil_Target() override { return current_depth_; }
	bool Set_Render_Target(SurfaceHandle* color, SurfaceHandle* depth_stencil) override;
	SurfaceHandle* Create_Image_Surface(uint32_t width, uint32_t height,
	                                    TextureFormat format) override;
	bool Copy_Rects(SurfaceHandle* source, const LockRect* rects, uint32_t rect_count,
	                SurfaceHandle* destination, const SurfacePoint* points) override;
	bool Update_Texture(TextureHandle* source, TextureHandle* destination) override;
	bool Surface_Bits(SurfaceHandle* surface, LockedRect& out) override;

	void Set_Clip_Plane(uint32_t index, const float plane[4]) override;

	ShaderHandle Create_Pixel_Shader(const uint32_t* function) override;
	void Delete_Pixel_Shader(ShaderHandle shader) override;
	void Set_Pixel_Shader(ShaderHandle shader) override;
	void Set_Pixel_Shader_Constant(uint32_t start_register, const void* data,
	                               uint32_t vector4_count) override;
	ShaderHandle Create_Vertex_Shader(const uint32_t* declaration, const uint32_t* function,
	                                  uint32_t usage) override;
	void Delete_Vertex_Shader(ShaderHandle shader) override;
	void Set_Vertex_Shader(ShaderHandle shader) override;
	void Set_Vertex_Shader_Constant(uint32_t start_register, const void* data,
	                                uint32_t vector4_count) override;

	const char* Device_Description() const override { return device_description_.c_str(); }
	uint32_t Pipeline_Count() const override {
		return static_cast<uint32_t>(pipelines_.size());
	}

	bool Present() override;

	bool Resize_Presentation(uint32_t width, uint32_t height) override;

	bool Set_Render_Scale(float scale) override;
	float Render_Scale() const override { return render_scale_; }
	void Device_Pixel_Size(uint32_t& out_width, uint32_t& out_height) const override {
		out_width = device_width_;
		out_height = device_height_;
	}

	void Get_Draw_Stats(DrawStats& out) const override { out = draw_stats_; }

	uint32_t Validation_Message_Count() const override { return validation_messages_; }
	bool Validation_Active() const override {
		return validation_layer_loaded_ && messenger_ != VK_NULL_HANDLE;
	}

private:
	static VKAPI_ATTR VkBool32 VKAPI_CALL Debug_Callback(
	    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
	    VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data,
	    void* user) {
		auto* self = static_cast<VulkanBackend*>(user);
		if (self != nullptr) ++self->validation_messages_;
		std::fprintf(stderr, "[vk %s] %s\n",
		             (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ? "error"
		                                                                       : "warning",
		             data->pMessage != nullptr ? data->pMessage : "(no message)");
		return VK_FALSE;
	}

	bool Create_Instance(void* window_handle);
	bool Pick_Device();
	bool Create_Render_Targets();
	bool Create_Descriptor_Machinery();
	// Adds one block of kDrawsPerBlock draws' descriptor sets and uniform slices, or fails
	// when the device is out of memory or a test limit forbids it.
	bool Add_Draw_Block();
	// The descriptor set and uniform-buffer slice belonging to draw `index`, growing the
	// pool if this is the first frame to reach that far. False means the draw cannot be
	// recorded at all, which is a dropped draw and is counted as one.
	bool Draw_Slot(uint32_t index, VkDescriptorSet& out_set, VkBuffer& out_buffer,
	               VkDeviceSize& out_offset, void*& out_mapped);
	bool Create_Shaders();
	bool Create_Swapchain(void* window_handle);
	bool Build_Swapchain();
	void Destroy_Swapchain();
	// --- HiDPI (docs/porting/hidpi-scale.md) ------------------------------------
	// The scale to render at: the one Set_Render_Scale() was given before Init(), else the
	// window seam's backing scale, else 1. `window_handle` may be null (headless).
	float Initial_Render_Scale(void* window_handle) const;
	// A point extent in pixels, rounded up so an odd client size at a fractional scale keeps
	// covering the whole drawable rather than leaving a column short.
	static uint32_t Scale_Extent(uint32_t points, float scale);
	// The factor between a surface's advertised (D3D8, points) size and the pixels its image
	// really holds: render_scale_ for the device's own targets, 1 for everything else. A
	// surface whose two sizes differ cannot be read or copied by a path that works in its
	// advertised units, which Resolve_Surface_Read() and Copy_Rects() refuse rather than
	// silently handing back the top-left corner.
	float Surface_Render_Scale(const SurfaceHandle* surface) const;
	void Destroy_Render_Targets();
	// Rebuilds the device's colour and depth targets at the current scale, dropping the
	// framebuffers that named the old images. Refuses while a scene is open, because the
	// engine holds the old target as its render target inside a frame.
	bool Rebuild_Render_Targets();
	// Re-reads the window's backing scale and rebuilds the targets when it changed, which is
	// what dragging a window between a Retina and a non-Retina display produces.
	bool Follow_Window_Scale(void* window_handle);

	bool Allocate_Buffer(VkDeviceSize size, VkBufferUsageFlags usage,
	                     VkMemoryPropertyFlags props, Buffer& out);
	bool Upload_Buffer(const void* data, VkDeviceSize size, VkBufferUsageFlags usage,
	                   Buffer& out);
	bool Find_Memory_Type(uint32_t type_bits, VkMemoryPropertyFlags props, uint32_t& out);
	// --- staging pool (docs/porting/renderer-resource-seam.md §4.1) -------------
	bool Acquire_Staging(VkDeviceSize size, StagingBlock& out);
	void Release_Staging(StagingBlock& block);
	bool Ensure_Texture_Staging(TextureHandle* texture);
	// Copy_Rects for a format whose host layout and image layout differ, i.e. one the
	// device has no equivalent of and the seam emulates with B8G8R8A8.
	bool Copy_Rects_Converting(SurfaceHandle* source, const LockRect* rects,
	                           uint32_t count, SurfaceHandle* destination,
	                           const SurfacePoint* points);
	// --- the GPU-write dirty bit (renderer-resource-seam.md §4.4) ---------------
	// Called from every funnel that lets the GPU write an image: after it, a host
	// read of that image or of any surface viewing it has to pay a readback, and
	// the lockable texture's staging copy is no longer the contents.
	void Mark_Gpu_Write(Image* image);
	void Mark_Gpu_Write(SurfaceHandle* surface);
	// Brings one level of a lockable texture back from the image into its staging
	// block, contracting the channels again on the no-view-swizzle path. This is
	// what makes a lock preserve what was there, and what a read lock on a dirty
	// resource pays. Caller holds resource_mutex_.
	bool Readback_Level(TextureHandle* texture, uint32_t level);
	// Puts the locked level's current contents in the staging block, or proves
	// nothing has to be: a DISCARD, or a level nobody has ever written.
	bool Prepare_Lock_Contents(TextureHandle* texture, uint32_t level, uint32_t flags);
	// A host read of a surface: copies a video-memory surface's pixels into its own
	// host-visible buffer, or waits for the queued CopyRects that wrote a
	// system-memory one. Does nothing when the surface is clean.
	bool Resolve_Surface_Read(SurfaceHandle* surface);
	// Submits and waits for what the open frame has recorded so far, then reopens
	// the pass. A host read mid-frame needs it: the copy it depends on is sitting
	// in the frame's command buffer, unsubmitted. `end_pass_first` is false when the
	// caller has already ended the pass (Begin_Transfer does).
	bool Flush_Frame_Commands(bool end_pass_first = true);
	VkCommandBuffer Begin_One_Shot();
	bool End_One_Shot(VkCommandBuffer cmd);

	// Collapses the whole shadowed D3D8 state block into the uniform block the
	// uber-shader interprets. This is Apply_Render_State_Changes for everything
	// Vulkan cannot bake into a pipeline.
	void Fill_Draw_Uniforms(uint32_t primitive_type, const VertexLayout& layout,
	                        DrawUniforms& out) const;
	VkRect2D Clamp_Scissor(const VkRect2D& rect) const;

	VkPipeline Get_Or_Create_Pipeline(const PipelineKey& key, const VertexLayout& layout);
	VkSampler Get_Or_Create_Sampler(const SamplerKey& key);
	// Everything a draw needs before the vkCmdDraw* itself: pipeline, uniforms,
	// descriptors and the dynamic state D3D8 changes without a pipeline notion.
	// False when the draw cannot be issued at all.
	bool Prepare_Draw(uint32_t primitive_type, const VertexBufferHandle& vb);
	// The layout a draw from `vb` uses: the buffer's own for a typed buffer, the
	// fixed-function FVF's for an untyped one, at the stride SetStreamSource named if it
	// named a larger one. False, with the reason counted, when there is none.
	bool Resolve_Draw_Layout(const VertexBufferHandle& vb, uint32_t& out_fvf,
	                         uint32_t& out_declaration, const VertexLayout*& out_layout);
	// Fits `layout` to SetStreamSource's stride: refused when the stride cannot hold a
	// vertex, widened when it is larger.
	bool Apply_Stream_Stride(VertexLayout& layout, uint32_t stride, const char* what);
	void Transition(VkCommandBuffer cmd, VkImage image, VkImageLayout from,
	                VkImageLayout to, VkImageAspectFlags aspect,
	                uint32_t mip_levels = 1);

	// --- render-target machinery ------------------------------------------------
	// A render pass per (has depth, load or discard) combination, and a framebuffer
	// per attachment pair. D3D8's SetRenderTarget is a state setter with no notion of
	// either, so both have to be cached behind it.
	VkRenderPass Get_Or_Create_Render_Pass(bool has_depth, bool load_color);
	VkFramebuffer Get_Or_Create_Framebuffer(VkRenderPass pass, SurfaceHandle* color,
	                                        SurfaceHandle* depth);
	// Ends and restarts the render pass around something that cannot be recorded
	// inside one (a layout transition, a copy, a target switch).
	void End_Current_Pass();
	bool Begin_Current_Pass();
	// Moves an image surface to `to`, recording into `cmd`, and remembers the layout.
	void Transition_Surface(VkCommandBuffer cmd, SurfaceHandle* surface, VkImageLayout to);
	// Records that a texture's image now *is* in `layout`, for the paths that
	// transition the image directly rather than through Transition_Surface. A
	// GetSurfaceLevel surface is a second name for the same image, so leaving its
	// layout behind makes the next Transition_Surface transition from a layout the
	// image left: correct bytes, wrong barrier, and the validation layer says so.
	void Note_Texture_Layout(TextureHandle* texture, VkImageLayout layout);
	// Records into the frame's command buffer when a scene is open, and into a
	// one-shot submission otherwise, so a copy stays ordered against the draws.
	VkCommandBuffer Begin_Transfer(bool& one_shot);
	bool End_Transfer(VkCommandBuffer cmd, bool one_shot);
	ShaderProgram* Find_Shader(ShaderHandle handle);

	bool validation_ = false;
	bool headless_ = true;
	// Whether this backend was asked to put frames on a screen: constructed non-headless AND
	// given a window. When it was, a missing surface or swapchain is a failure at Init() and
	// Present() cannot report success, because there is nothing it could have presented to.
	// Without this, a build compiled with neither SPIKE_WITH_PLATFORM_WINDOW nor SPIKE_WITH_SDL
	// created no swapchain, presented nothing, and returned true from Present() - which is how
	// the native engine build was measured "presenting" on Apple Silicon while showing nothing.
	bool presentation_required_ = false;
	bool present_refusal_reported_ = false;
	// The back buffer in D3D8's units: the window's client area, in points. Every size that
	// crosses this interface is in these units, the width/height Init() is given included.
	uint32_t width_ = 0, height_ = 0;
	// Device pixels per point, and the back buffer's real size in pixels. On a Retina display
	// the two differ by a factor of 2, and rendering at width_ x height_ paints a quarter of
	// the panel's pixels for the presentation blit to upscale, which is the defect this pair
	// exists to fix (docs/porting/hidpi-scale.md).
	float render_scale_ = 1.0f;
	uint32_t device_width_ = 0, device_height_ = 0;
	// A scale asked for through Set_Render_Scale() before Init(), which is how a headless run
	// picks one. Zero means "ask the window seam", which is what a real window does.
	float requested_render_scale_ = 0.0f;
	// Whether VK_LAYER_KHRONOS_validation was really enabled on the instance, as opposed to
	// requested: zero messages from a layer that never loaded proves nothing.
	bool validation_layer_loaded_ = false;
	// The window Init() was given, kept so a resize can re-read its backing scale. Null for a
	// headless backend, and not owned.
	void* window_handle_ = nullptr;
	std::string device_description_ = "<uninitialised>";

	VkInstance instance_ = VK_NULL_HANDLE;
	VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
	uint32_t validation_messages_ = 0;
	VkPhysicalDevice physical_ = VK_NULL_HANDLE;
	VkDevice device_ = VK_NULL_HANDLE;
	uint32_t queue_family_ = 0;
	VkQueue queue_ = VK_NULL_HANDLE;
	VkCommandPool command_pool_ = VK_NULL_HANDLE;
	VkCommandBuffer frame_cmd_ = VK_NULL_HANDLE;
	VkFence frame_fence_ = VK_NULL_HANDLE;

	Image color_target_;
	Image depth_target_;
	VkFormat depth_format_ = VK_FORMAT_D32_SFLOAT_S8_UINT;
	// Non-identity VkImageView component mappings; false under MoltenVK.
	bool view_swizzle_ = true;
	// The default targets as surfaces, so SetRenderTarget's save/restore -- which
	// hands back whatever GetRenderTarget returned -- has something to name them by.
	SurfaceHandle default_color_surface_;
	SurfaceHandle default_depth_surface_;
	SurfaceHandle* current_color_ = nullptr;
	SurfaceHandle* current_depth_ = nullptr;
	// The current colour target's size. Distinct from width_/height_, which stay the
	// device's back-buffer size: a render-to-texture target is usually smaller, and
	// the viewport, the scissor clamp and Clear all follow the *target*.
	uint32_t target_width_ = 0, target_height_ = 0;
	// The same target in pixels. Equal to target_width_/target_height_ for every
	// render-to-texture target, because a texture is created in pixels and has no points to
	// scale from; scaled only for the device's own colour target, the one whose size came from
	// a window. This is the pair the viewport, the scissor, the render area and Clear use.
	uint32_t device_target_width_ = 0, device_target_height_ = 0;
	// {has depth, load colour} -> render pass, and attachment pair -> framebuffer.
	std::unordered_map<uint32_t, VkRenderPass> render_passes_;
	std::unordered_map<uint64_t, VkFramebuffer> framebuffers_;
	std::vector<SurfaceHandle*> owned_surfaces_;

	VkShaderModule vert_module_ = VK_NULL_HANDLE;
	VkShaderModule frag_module_ = VK_NULL_HANDLE;
	VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
	VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
	// Per-draw resources in blocks of kDrawsPerBlock draws, grown on demand: a real mission
	// frame's draw count is unbounded, so it cannot be a constant. A block owns its own
	// descriptor pool, its sets and the uniform buffer they point into, because neither a
	// VkBuffer nor a VkDescriptorPool can be enlarged after creation.
	struct DrawBlock {
		VkDescriptorPool pool = VK_NULL_HANDLE;
		std::vector<VkDescriptorSet> sets;
		Buffer uniforms;
		void* mapped = nullptr; // persistently mapped; the memory is host-coherent
	};
	std::vector<DrawBlock> draw_blocks_;
	VkDeviceSize ubo_stride_ = 0;
	uint32_t draw_index_ = 0;
	// Per-frame draw accounting, published at End_Scene. A frame that could not allocate a
	// draw's resources has lost geometry, and that has to be a number the caller can read
	// rather than a line in a log.
	uint32_t frame_draws_requested_ = 0;
	uint32_t frame_draws_dropped_ = 0;
	uint32_t frame_untyped_draws_issued_ = 0;
	uint32_t frame_untyped_draws_dropped_ = 0;
	DrawStats draw_stats_;
	// Negative control (ZH_RENDER_NO_UNTYPED_VB): refuse FVF-0 buffers at creation, which is
	// what the backend did before it had this path. Exists so the gate that proves the path
	// draws can be shown to fail without it.
	bool untyped_vertex_buffers_ = true;
	// Negative control (ZH_RENDER_NO_VERTEX_DECLARATION): draw untyped buffers only through
	// the fixed-function FVF, never through the bound program's declaration, which is what
	// the backend did before it had a declaration path.
	bool vertex_declarations_ = true;
	// Test hook (ZH_RENDER_MAX_DRAWS): refuses to grow past this many draws in a frame, which
	// is how a negative control reproduces the old fixed preallocation. 0 means no limit.
	uint32_t draw_limit_ = 0;
	// Test/diagnostic hook (ZH_RENDER_DRAW_REPORT): print the accounting every N frames.
	uint32_t draw_report_interval_ = 0;

	Buffer dummy_vertex_buffer_; // feeds attributes the FVF does not supply
	TextureHandle* white_texture_ = nullptr;

	std::unordered_map<uint64_t, VkPipeline> pipelines_;
	std::unordered_map<SamplerKey, VkSampler, SamplerKeyHash> samplers_;
	ResourceStats resource_stats_;
	// Frame being recorded, and the last frame known to have finished on the GPU, so
	// a DISCARD can tell whether renaming onto a ring region is safe. The spike keeps
	// one frame in flight.
	uint64_t frame_counter_ = 0;
	uint64_t completed_frame_ = 0;

	// Free staging blocks, and the mutex that makes the lock path callable from the
	// engine's loader thread as well as the render thread (class C4). It guards the
	// pool, the per-resource lock bookkeeping, the stats and the one-shot command
	// submissions the lock path makes.
	std::vector<StagingBlock> staging_free_;
	mutable std::mutex resource_mutex_;
	bool staging_retain_ = false;

	std::vector<TextureHandle*> owned_textures_;
	std::vector<VertexBufferHandle*> owned_vbs_;
	std::vector<IndexBufferHandle*> owned_ibs_;

	// --- shadow state, mirroring DX8Wrapper::RenderStates / TextureStageStates ---
	uint32_t render_states_[D3DRS_MAX]{};
	PerStage stages_[kMaxTextureStages];
	TextureHandle* bound_textures_[kMaxTextureStages]{};
	Matrix4x4 world_ = Matrix4x4::Identity();
	Matrix4x4 view_ = Matrix4x4::Identity();
	Matrix4x4 projection_ = Matrix4x4::Identity();
	Matrix4x4 texture_transform_[kMaxTexCoordSets] = {
	    Matrix4x4::Identity(), Matrix4x4::Identity(), Matrix4x4::Identity(),
	    Matrix4x4::Identity()};
	LightState lights_[kMaxLights];
	MaterialState material_;
	bool scissor_enabled_ = false;
	VkRect2D scissor_{{0, 0}, {0, 0}};
	ViewportRect viewport_;
	// DrawPrimitiveUP's vertices: one host-visible buffer, bump-allocated within the
	// frame and rewound in Begin_Scene. D3D8 copies UP vertices into its own scratch
	// buffer for exactly the same reason.
	Buffer up_ring_;
	void* up_mapped_ = nullptr;
	VkDeviceSize up_offset_ = 0;
	// One decoded VertexLayout per FVF DrawPrimitiveUP has been called with; the
	// layout is what the pipeline's vertex input is built from.
	std::unordered_map<uint32_t, VertexLayout> up_layouts_;
	float max_anisotropy_ = 1.0f; // 1.0 when the device has no samplerAnisotropy
	float max_point_size_ = 1.0f;
	VertexBufferHandle* bound_vb_ = nullptr;
	// SetStreamSource's explicit stride for stream 0; 0 when the caller left it to the FVF.
	uint32_t bound_vb_stride_ = 0;
	// The D3DFVF_* last passed to SetVertexShader: the layout every untyped buffer draws with.
	uint32_t fixed_function_fvf_ = 0;
	// Layouts resolved for untyped draws, keyed by (fvf << 32 | stride) for the
	// fixed-function ones and (1 << 63 | declaration hash << 32 | stride) for the declared.
	std::unordered_map<uint64_t, VertexLayout> untyped_layouts_;
	IndexBufferHandle* bound_ib_ = nullptr;

	// --- programmable shaders and clip planes ----------------------------------
	// D3D8 shader handles are DWORDs the device hands out, so the backend owns the
	// numbering. 0 is D3D8's "no shader", i.e. back to fixed function. Handles keep bit 0
	// (D3DFVF_RESERVED0) set, as D3D8's do, which is how SetVertexShader's caller tells a
	// handle from an FVF bitfield.
	std::unordered_map<ShaderHandle, ShaderProgram> shaders_;
	ShaderHandle next_shader_ = 1;
	ShaderHandle Allocate_Shader_Handle() {
		const ShaderHandle handle = (next_shader_ << 1) | 1u;
		++next_shader_;
		return handle;
	}
	ShaderHandle bound_pixel_shader_ = kNullShader;
	ShaderHandle bound_vertex_shader_ = kNullShader;
	float pixel_shader_constants_[kMaxPixelShaderConstants][4]{};
	float vertex_shader_constants_[kMaxVertexShaderConstants][4]{};
	float clip_planes_[kMaxClipPlanes][4]{};
	// VkPhysicalDeviceFeatures::shaderClipDistance; without it gl_ClipDistance may
	// not be written at all, so the planes are dropped rather than mis-rendered.
	bool clip_distance_ = false;
	uint32_t index_base_offset_ = 0;
	bool in_scene_ = false;

	// --- optional presentation -------------------------------------------------
	VkSurfaceKHR surface_ = VK_NULL_HANDLE;
	VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
	std::vector<VkImage> swapchain_images_;
	VkFormat swapchain_format_ = VK_FORMAT_UNDEFINED;
	VkExtent2D swapchain_extent_ = {0, 0};
	// The image is acquired with a fence rather than a semaphore: every submission in this
	// spike is CPU-waited (End_One_Shot waits the queue idle), so a fence is the whole of the
	// synchronisation, and one reusable semaphore would be signalled again while the previous
	// present's wait was still pending.
	VkFence acquire_fence_ = VK_NULL_HANDLE;
};

// ---------------------------------------------------------------------------
// initialisation
// ---------------------------------------------------------------------------

namespace {

bool Instance_Extension_Available(const char* name) {
	uint32_t count = 0;
	vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
	std::vector<VkExtensionProperties> available(count);
	vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data());
	for (const auto& e : available) {
		if (std::strcmp(e.extensionName, name) == 0) return true;
	}
	return false;
}

// VK_KHR_portability_subset is a provisional extension, so its structures only
// exist in vulkan_beta.h behind VK_ENABLE_BETA_EXTENSIONS, which not every SDK
// ships. Declaring the one structure the backend reads keeps the Linux and macOS
// builds on the same headers. Field order is the extension's, and must stay so.
struct PortabilitySubsetFeatures {
	VkStructureType sType;
	void* pNext;
	VkBool32 constantAlphaColorBlendFactors;
	VkBool32 events;
	VkBool32 imageViewFormatReinterpretation;
	VkBool32 imageViewFormatSwizzle;
	VkBool32 imageView2DOn3DImage;
	VkBool32 multisampleArrayImage;
	VkBool32 mutableComparisonSamplers;
	VkBool32 pointPolygons;
	VkBool32 samplerMipLodBias;
	VkBool32 separateStencilMaskRef;
	VkBool32 shaderSampleRateInterpolationFunctions;
	VkBool32 tessellationIsolines;
	VkBool32 tessellationPointMode;
	VkBool32 triangleFans;
	VkBool32 vertexAttributeAccessBeyondStride;
};
constexpr VkStructureType kPortabilitySubsetFeaturesType =
    static_cast<VkStructureType>(1000163000); // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PORTABILITY_SUBSET_FEATURES_KHR

bool Device_Extension_Available(VkPhysicalDevice device, const char* name) {
	uint32_t count = 0;
	vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
	std::vector<VkExtensionProperties> available(count);
	vkEnumerateDeviceExtensionProperties(device, nullptr, &count, available.data());
	for (const auto& e : available) {
		if (std::strcmp(e.extensionName, name) == 0) return true;
	}
	return false;
}

} // namespace

bool VulkanBackend::Create_Instance(void* window_handle) {
	std::vector<const char*> extensions;
	std::vector<const char*> layers;
	VkInstanceCreateFlags instance_flags = 0;

	// MoltenVK is a non-conformant "portability" driver. A current Khronos loader hides
	// such drivers from vkEnumeratePhysicalDevices unless the instance opts in here, and
	// vkCreateInstance itself fails with VK_ERROR_INCOMPATIBLE_DRIVER. Conditional on the
	// extension being advertised, so the Linux path is byte-for-byte unchanged.
	if (Instance_Extension_Available(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
		extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
		instance_flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
	}

#ifdef SPIKE_WITH_PLATFORM_WINDOW
	if (!headless_ && window_handle != nullptr) {
		const char* names[8] = {nullptr};
		const int count = WWPlatform::Window_Vulkan_Instance_Extensions(window_handle, names, 8);
		for (int i = 0; i < count; ++i) extensions.push_back(names[i]);
	}
#elif defined(SPIKE_WITH_SDL)
	if (!headless_ && window_handle != nullptr) {
		unsigned count = 0;
		SDL_Vulkan_GetInstanceExtensions(static_cast<SDL_Window*>(window_handle), &count, nullptr);
		std::vector<const char*> sdl_ext(count);
		SDL_Vulkan_GetInstanceExtensions(static_cast<SDL_Window*>(window_handle), &count, sdl_ext.data());
		extensions.insert(extensions.end(), sdl_ext.begin(), sdl_ext.end());
	}
#else
	(void)window_handle;
#endif

	if (validation_) {
		uint32_t layer_count = 0;
		vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
		std::vector<VkLayerProperties> available(layer_count);
		vkEnumerateInstanceLayerProperties(&layer_count, available.data());
		for (const auto& l : available) {
			if (std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0) {
				layers.push_back("VK_LAYER_KHRONOS_validation");
			}
		}
		if (layers.empty()) {
			// Stated positively where a CI wrapper reads it: "validation messages: 0" from a run
			// the loader never gave a layer is the failure mode
			// docs/porting/apple-silicon-verification.md 8.1 measured on macOS, and it looks
			// exactly like a clean run unless the run says which of the two it was. On stderr,
			// because zh-staging-workload's stdout is JSON and nothing else.
			std::fprintf(stderr, "validation layer: absent\n");
			std::fprintf(stderr, "note: VK_LAYER_KHRONOS_validation not present, continuing without\n");
		} else {
			std::fprintf(stderr, "validation layer: loaded\n");
			extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
			// Recorded, not just noted: Validation_Active() is what lets a caller tell zero
			// messages from an unvalidated run.
			validation_layer_loaded_ = true;
		}
	}

	VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.pApplicationName = "zh-renderer-spike";
	app.apiVersion = VK_API_VERSION_1_1;

	VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ci.pApplicationInfo = &app;
	ci.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
	ci.ppEnabledExtensionNames = extensions.empty() ? nullptr : extensions.data();
	ci.enabledLayerCount = static_cast<uint32_t>(layers.size());
	ci.ppEnabledLayerNames = layers.empty() ? nullptr : layers.data();
	ci.flags = instance_flags;
	VK_CHECK(vkCreateInstance(&ci, nullptr, &instance_));

	// Route validation output through an explicit messenger rather than relying on
	// the layer's default logging, so "clean run" is something the spike can assert.
	if (!layers.empty()) {
		auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
		    vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
		if (create != nullptr) {
			VkDebugUtilsMessengerCreateInfoEXT dci{
			    VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
			dci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
			                      VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
			dci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
			                  VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
			                  VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
			dci.pfnUserCallback = Debug_Callback;
			dci.pUserData = this;
			create(instance_, &dci, nullptr, &messenger_);
		}
	}
	return true;
}

bool VulkanBackend::Pick_Device() {
	uint32_t count = 0;
	VK_CHECK(vkEnumeratePhysicalDevices(instance_, &count, nullptr));
	if (count == 0) {
		std::fprintf(stderr, "no Vulkan physical devices\n");
		return false;
	}
	std::vector<VkPhysicalDevice> devices(count);
	VK_CHECK(vkEnumeratePhysicalDevices(instance_, &count, devices.data()));

	// Prefer a discrete GPU, then integrated, then whatever is left (lavapipe).
	int best_score = -1;
	for (VkPhysicalDevice d : devices) {
		VkPhysicalDeviceProperties props;
		vkGetPhysicalDeviceProperties(d, &props);
		int score = 0;
		if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) score = 3;
		else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) score = 2;
		else score = 1;
		if (score > best_score) {
			best_score = score;
			physical_ = d;
			device_description_ = props.deviceName;
		}
	}

	uint32_t family_count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(physical_, &family_count, nullptr);
	std::vector<VkQueueFamilyProperties> families(family_count);
	vkGetPhysicalDeviceQueueFamilyProperties(physical_, &family_count, families.data());
	bool found = false;
	for (uint32_t i = 0; i < family_count; ++i) {
		if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
			queue_family_ = i;
			found = true;
			break;
		}
	}
	if (!found) {
		std::fprintf(stderr, "no graphics queue family\n");
		return false;
	}

	float priority = 1.0f;
	VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	qci.queueFamilyIndex = queue_family_;
	qci.queueCount = 1;
	qci.pQueuePriorities = &priority;

	std::vector<const char*> device_extensions;
	if (surface_ != VK_NULL_HANDLE) {
		device_extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
	}
	// Required by spec whenever the physical device advertises it (MoltenVK always does).
	// The subset also says which of Vulkan's guarantees the device does not keep; the
	// only one this backend depends on is the image-view swizzle.
	PortabilitySubsetFeatures portability{};
	portability.sType = kPortabilitySubsetFeaturesType;
	bool portability_subset = false;
	if (Device_Extension_Available(physical_, "VK_KHR_portability_subset")) {
		device_extensions.push_back("VK_KHR_portability_subset");
		portability_subset = true;

		VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
		features.pNext = &portability;
		vkGetPhysicalDeviceFeatures2(physical_, &features);
		portability.pNext = nullptr;
		view_swizzle_ = portability.imageViewFormatSwizzle == VK_TRUE;
	}
	// Lets a swizzle-capable device (any Linux driver) run the CPU expansion path
	// that MoltenVK forces, so the two are covered by the same tests.
	if (std::getenv("ZH_SPIKE_NO_VIEW_SWIZZLE") != nullptr) view_swizzle_ = false;

	// The pre-pool behaviour, kept as a mode rather than deleted: every lockable
	// resource pins its staging block for its whole lifetime. That is what classes
	// C7 (pointer used after Unlock) and C8 (read-write hand-out to arbitrary engine
	// code) rely on, and it is the "before" the pooled numbers are measured against.
	staging_retain_ = std::getenv("ZH_SPIKE_STAGING_RETAIN") != nullptr;
	untyped_vertex_buffers_ = std::getenv("ZH_RENDER_NO_UNTYPED_VB") == nullptr;
	vertex_declarations_ = std::getenv("ZH_RENDER_NO_VERTEX_DECLARATION") == nullptr;

	// D3D8 states that need a Vulkan *feature*, not just a pipeline field:
	// D3DRS_POINTSIZE > 1 needs largePoints, D3DTSS_MAXANISOTROPY needs
	// samplerAnisotropy, D3DFILL_WIREFRAME/POINT need fillModeNonSolid. Each is
	// enabled only when the device has it, and what was actually enabled decides
	// what the translation clamps to (D3D8 caps work the same way).
	VkPhysicalDeviceFeatures available{};
	vkGetPhysicalDeviceFeatures(physical_, &available);
	VkPhysicalDeviceProperties device_props{};
	vkGetPhysicalDeviceProperties(physical_, &device_props);
	VkPhysicalDeviceFeatures enabled{};
	enabled.samplerAnisotropy = available.samplerAnisotropy;
	enabled.largePoints = available.largePoints;
	enabled.fillModeNonSolid = available.fillModeNonSolid;
	// D3D8's SetClipPlane needs gl_ClipDistance, which is a Vulkan *feature*.
	enabled.shaderClipDistance = available.shaderClipDistance;
	clip_distance_ = available.shaderClipDistance == VK_TRUE;
	max_anisotropy_ = available.samplerAnisotropy == VK_TRUE
	                      ? device_props.limits.maxSamplerAnisotropy
	                      : 1.0f;
	max_point_size_ = available.largePoints == VK_TRUE
	                      ? device_props.limits.pointSizeRange[1]
	                      : 1.0f;

	VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	dci.pEnabledFeatures = &enabled;
	dci.enabledExtensionCount = static_cast<uint32_t>(device_extensions.size());
	dci.ppEnabledExtensionNames = device_extensions.empty() ? nullptr : device_extensions.data();
	// A portability-subset feature the device has must also be *enabled* here before it
	// may be used; passing back what was queried enables exactly what the device has.
	if (portability_subset) dci.pNext = &portability;
	VK_CHECK(vkCreateDevice(physical_, &dci, nullptr, &device_));
	vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

	VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pci.queueFamilyIndex = queue_family_;
	VK_CHECK(vkCreateCommandPool(device_, &pci, nullptr, &command_pool_));

	VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	cbai.commandPool = command_pool_;
	cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cbai.commandBufferCount = 1;
	VK_CHECK(vkAllocateCommandBuffers(device_, &cbai, &frame_cmd_));

	VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	fci.flags = VK_FENCE_CREATE_SIGNALED_BIT; // so the first Begin_Scene's wait returns
	VK_CHECK(vkCreateFence(device_, &fci, nullptr, &frame_fence_));
	return true;
}

bool VulkanBackend::Find_Memory_Type(uint32_t type_bits, VkMemoryPropertyFlags props,
                                     uint32_t& out) {
	VkPhysicalDeviceMemoryProperties mem;
	vkGetPhysicalDeviceMemoryProperties(physical_, &mem);
	for (uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
		if ((type_bits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & props) == props) {
			out = i;
			return true;
		}
	}
	return false;
}

bool VulkanBackend::Allocate_Buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                    VkMemoryPropertyFlags props, Buffer& out) {
	VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bci.size = size;
	bci.usage = usage;
	bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VK_CHECK(vkCreateBuffer(device_, &bci, nullptr, &out.buffer));

	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(device_, out.buffer, &req);
	uint32_t type = 0;
	if (!Find_Memory_Type(req.memoryTypeBits, props, type)) {
		std::fprintf(stderr, "no memory type for buffer\n");
		return false;
	}
	VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	mai.allocationSize = req.size;
	mai.memoryTypeIndex = type;
	VK_CHECK(vkAllocateMemory(device_, &mai, nullptr, &out.memory));
	VK_CHECK(vkBindBufferMemory(device_, out.buffer, out.memory, 0));
	out.size = size;
	return true;
}

bool VulkanBackend::Upload_Buffer(const void* data, VkDeviceSize size,
                                  VkBufferUsageFlags usage, Buffer& out) {
	// Host-visible for simplicity. D3D8's D3DPOOL_MANAGED semantics (driver keeps a
	// system-memory shadow copy and re-uploads on device loss) have no Vulkan
	// equivalent -- a real backend has to implement that shadowing itself.
	if (!Allocate_Buffer(size, usage,
	                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	                     out)) {
		return false;
	}
	void* mapped = nullptr;
	VK_CHECK(vkMapMemory(device_, out.memory, 0, size, 0, &mapped));
	if (data != nullptr) {
		std::memcpy(mapped, data, static_cast<size_t>(size));
	} else {
		std::memset(mapped, 0xff, static_cast<size_t>(size));
	}
	vkUnmapMemory(device_, out.memory);
	return true;
}

VkCommandBuffer VulkanBackend::Begin_One_Shot() {
	VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	cbai.commandPool = command_pool_;
	cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cbai.commandBufferCount = 1;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	if (vkAllocateCommandBuffers(device_, &cbai, &cmd) != VK_SUCCESS) return VK_NULL_HANDLE;
	VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd, &bi);
	return cmd;
}

bool VulkanBackend::End_One_Shot(VkCommandBuffer cmd) {
	VK_CHECK(vkEndCommandBuffer(cmd));
	VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cmd;
	VK_CHECK(vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE));
	VK_CHECK(vkQueueWaitIdle(queue_));
	vkFreeCommandBuffers(device_, command_pool_, 1, &cmd);
	return true;
}

void VulkanBackend::Transition(VkCommandBuffer cmd, VkImage image, VkImageLayout from,
                               VkImageLayout to, VkImageAspectFlags aspect,
                               uint32_t mip_levels) {
	VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	b.oldLayout = from;
	b.newLayout = to;
	b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = image;
	b.subresourceRange = {aspect, 0, mip_levels, 0, 1};
	b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
	b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
	                     VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}

uint32_t VulkanBackend::Scale_Extent(uint32_t points, float scale) {
	if (points == 0) return 0;
	if (!(scale > 0.0f)) return points;
	const float scaled = static_cast<float>(points) * scale;
	const uint32_t pixels = static_cast<uint32_t>(std::ceil(scaled - 0.0001f));
	return pixels == 0 ? 1u : pixels;
}

float VulkanBackend::Surface_Render_Scale(const SurfaceHandle* surface) const {
	if (surface == &default_color_surface_ || surface == &default_depth_surface_) {
		return render_scale_;
	}
	return 1.0f;
}

float VulkanBackend::Initial_Render_Scale(void* window_handle) const {
	// An explicit request wins over the platform, so a headless run can render at a scale no
	// display attached to the machine has -- which is the only way Linux CI can exercise this.
	if (requested_render_scale_ > 0.0f) return requested_render_scale_;
#ifdef SPIKE_WITH_PLATFORM_WINDOW
	if (window_handle != nullptr) {
		const float scale = WWPlatform::Window_Backing_Scale(window_handle);
		if (scale > 0.0f) return scale;
	}
#else
	(void)window_handle;
#endif
	return 1.0f;
}

bool VulkanBackend::Create_Render_Targets() {
	device_width_ = Scale_Extent(width_, render_scale_);
	device_height_ = Scale_Extent(height_, render_scale_);
	// The images are in pixels: this is where a point becomes a pixel, and the only place.
	color_target_.width = device_width_;
	color_target_.height = device_height_;
	depth_target_.width = device_width_;
	depth_target_.height = device_height_;
	auto make_image = [&](VkFormat format, VkImageUsageFlags usage,
	                      VkImageAspectFlags aspect, Image& out) -> bool {
		if (out.width == 0) out.width = width_;
		if (out.height == 0) out.height = height_;
		VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		ici.imageType = VK_IMAGE_TYPE_2D;
		ici.format = format;
		ici.extent = {out.width, out.height, 1};
		ici.mipLevels = 1;
		ici.arrayLayers = 1;
		ici.samples = VK_SAMPLE_COUNT_1_BIT;
		ici.tiling = VK_IMAGE_TILING_OPTIMAL;
		ici.usage = usage;
		ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		VK_CHECK(vkCreateImage(device_, &ici, nullptr, &out.image));

		VkMemoryRequirements req;
		vkGetImageMemoryRequirements(device_, out.image, &req);
		uint32_t type = 0;
		if (!Find_Memory_Type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, type)) {
			return false;
		}
		VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		mai.allocationSize = req.size;
		mai.memoryTypeIndex = type;
		VK_CHECK(vkAllocateMemory(device_, &mai, nullptr, &out.memory));
		VK_CHECK(vkBindImageMemory(device_, out.image, out.memory, 0));

		VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		vci.image = out.image;
		vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vci.format = format;
		vci.subresourceRange = {aspect, 0, 1, 0, 1};
		VK_CHECK(vkCreateImageView(device_, &vci, nullptr, &out.view));
		return true;
	};

	// TRANSFER_DST as well as SRC: CopyRects may copy *into* a render target, which
	// is what the engine's shroud and reflection restore paths do.
	if (!make_image(kColorFormat,
	                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
	                    VK_IMAGE_USAGE_TRANSFER_DST_BIT,
	                VK_IMAGE_ASPECT_COLOR_BIT, color_target_)) {
		return false;
	}
	depth_format_ = Pick_Depth_Stencil_Format(physical_);
	if (!make_image(depth_format_, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
	                VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
	                depth_target_)) {
		return false;
	}

	// The surfaces keep advertising the back buffer's size in points, because that is what
	// D3D8's GetDesc reports and what the engine compares against its own resolution; the
	// pixels live in the Image they point at. The two differ only at a scale other than 1, and
	// Surface_Render_Scale() is how every path that needs the difference asks for it.
	default_color_surface_ = SurfaceHandle{};
	default_color_surface_.image = &color_target_;
	default_color_surface_.width = width_;
	default_color_surface_.height = height_;
	default_color_surface_.vk_format = kColorFormat;
	default_depth_surface_ = SurfaceHandle{};
	default_depth_surface_.image = &depth_target_;
	default_depth_surface_.depth_stencil = true;
	default_depth_surface_.width = width_;
	default_depth_surface_.height = height_;
	default_depth_surface_.vk_format = depth_format_;
	current_color_ = &default_color_surface_;
	current_depth_ = &default_depth_surface_;
	target_width_ = width_;
	target_height_ = height_;
	device_target_width_ = device_width_;
	device_target_height_ = device_height_;
	return true;
}

void VulkanBackend::Destroy_Render_Targets() {
	auto free_image = [&](Image& i) {
		if (i.view) vkDestroyImageView(device_, i.view, nullptr);
		if (i.image) vkDestroyImage(device_, i.image, nullptr);
		if (i.memory) vkFreeMemory(device_, i.memory, nullptr);
		i = Image{};
	};
	// The host copy the default colour target grows on its first read describes the old size.
	if (default_color_surface_.mapped != nullptr) {
		vkUnmapMemory(device_, default_color_surface_.bits.memory);
		default_color_surface_.mapped = nullptr;
	}
	if (default_color_surface_.bits.buffer) {
		vkDestroyBuffer(device_, default_color_surface_.bits.buffer, nullptr);
	}
	if (default_color_surface_.bits.memory) {
		vkFreeMemory(device_, default_color_surface_.bits.memory, nullptr);
	}
	default_color_surface_.bits = Buffer{};
	free_image(color_target_);
	free_image(depth_target_);
}

bool VulkanBackend::Rebuild_Render_Targets() {
	if (device_ == VK_NULL_HANDLE) return false;
	if (in_scene_) {
		// Between Begin_Scene and End_Scene the engine holds the target it is drawing into;
		// swapping the image under it would drop the frame's recorded commands.
		std::fprintf(stderr,
		             "Vulkan backend: the render scale cannot change inside a scene\n");
		return false;
	}
	// Whether a render-to-texture target was bound or not, the device's targets are what is
	// being replaced, so the state that named them has to go back to them.
	const bool default_bound = (current_color_ == &default_color_surface_);
	vkDeviceWaitIdle(device_);
	for (auto& fb : framebuffers_) vkDestroyFramebuffer(device_, fb.second, nullptr);
	framebuffers_.clear();
	Destroy_Render_Targets();
	if (!Create_Render_Targets()) return false;
	viewport_ = ViewportRect{0, 0, target_width_, target_height_, 0.0f, 1.0f};
	scissor_enabled_ = false;
	if (!default_bound) {
		std::fprintf(stderr,
		             "Vulkan backend: the render scale changed while a render-to-texture "
		             "target was bound; the device's targets are now current\n");
	}
	return true;
}

bool VulkanBackend::Follow_Window_Scale(void* window_handle) {
	if (window_handle == nullptr) return true;
	const float scale = Initial_Render_Scale(window_handle);
	if (scale == render_scale_) return true;
	std::fprintf(stderr, "Vulkan backend: backing scale %.2f -> %.2f, colour target %ux%u -> "
	                     "%ux%u pixels for a %ux%u point client area\n",
	             static_cast<double>(render_scale_), static_cast<double>(scale), device_width_,
	             device_height_, Scale_Extent(width_, scale), Scale_Extent(height_, scale),
	             width_, height_);
	render_scale_ = scale;
	return Rebuild_Render_Targets();
}

bool VulkanBackend::Set_Render_Scale(float scale) {
	if (!(scale > 0.0f)) return false;
	// Before Init() this only records the choice: there is no device to build targets on yet.
	if (device_ == VK_NULL_HANDLE) {
		requested_render_scale_ = scale;
		render_scale_ = scale;
		return true;
	}
	// An override after Init() pins the scale: Follow_Window_Scale() honours it too, so a test
	// that asked for 2 does not lose it to the next resize event.
	requested_render_scale_ = scale;
	if (scale == render_scale_) return true;
	render_scale_ = scale;
	return Rebuild_Render_Targets();
}

// The render pass carries no clear: the engine clears with an explicit
// DX8Wrapper::Clear() inside Begin_Scene and D3D8's Clear() takes flags per call,
// so it becomes vkCmdClearAttachments. What the pass does have to encode is
// whether the target's existing contents survive -- which is the whole difference
// between D3D8, where a target simply keeps its pixels across a SetRenderTarget
// round trip, and Vulkan, where saying so is mandatory.
VkRenderPass VulkanBackend::Get_Or_Create_Render_Pass(bool has_depth, bool load_color) {
	const uint32_t key = (has_depth ? 1u : 0u) | (load_color ? 2u : 0u);
	auto it = render_passes_.find(key);
	if (it != render_passes_.end()) return it->second;

	VkAttachmentDescription attachments[2]{};
	attachments[0].format = kColorFormat;
	attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
	attachments[0].loadOp = load_color ? VK_ATTACHMENT_LOAD_OP_LOAD
	                                   : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	// COLOR_ATTACHMENT_OPTIMAL both ends: the transition to a transfer source (for
	// presentation, readback or CopyRects) or to a sampled image (for a
	// render-to-texture the cascade then reads) is explicit, because which one it is
	// depends on what the engine does next rather than on the pass.
	attachments[0].initialLayout = load_color ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
	                                          : VK_IMAGE_LAYOUT_UNDEFINED;
	attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	attachments[1].format = depth_format_;
	attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
	attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
	VkAttachmentReference depth_ref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
	VkSubpassDescription subpass{};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &color_ref;
	// D3D8 allows SetRenderTarget(surface, nullptr), which renders with no depth
	// buffer at all; in Vulkan that is a different render pass.
	subpass.pDepthStencilAttachment = has_depth ? &depth_ref : nullptr;

	VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
	rpci.attachmentCount = has_depth ? 2 : 1;
	rpci.pAttachments = attachments;
	rpci.subpassCount = 1;
	rpci.pSubpasses = &subpass;
	VkRenderPass pass = VK_NULL_HANDLE;
	if (vkCreateRenderPass(device_, &rpci, nullptr, &pass) != VK_SUCCESS) {
		return VK_NULL_HANDLE;
	}
	render_passes_[key] = pass;
	return pass;
}

VkFramebuffer VulkanBackend::Get_Or_Create_Framebuffer(VkRenderPass pass,
                                                       SurfaceHandle* color,
                                                       SurfaceHandle* depth) {
	const uint64_t key = (reinterpret_cast<uint64_t>(color) * 1099511628211ull) ^
	                     (reinterpret_cast<uint64_t>(depth) * 14695981039346656037ull) ^
	                     (reinterpret_cast<uint64_t>(pass) << 1);
	auto it = framebuffers_.find(key);
	if (it != framebuffers_.end()) return it->second;

	VkImageView views[2] = {color->image->view,
	                        depth != nullptr ? depth->image->view : VK_NULL_HANDLE};
	VkFramebufferCreateInfo fbci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
	fbci.renderPass = pass;
	fbci.attachmentCount = depth != nullptr ? 2 : 1;
	fbci.pAttachments = views;
	// The colour target's size, not the device's: Vulkan allows an attachment larger
	// than the framebuffer, which is what lets a small render-to-texture target keep
	// using the device's depth buffer, the way D3D8 does. In pixels, because that is
	// what an attachment is measured in -- and a mip-level surface of a texture is
	// smaller than the image it views, so this cannot come from the image.
	fbci.width = Scale_Extent(color->width, Surface_Render_Scale(color));
	fbci.height = Scale_Extent(color->height, Surface_Render_Scale(color));
	fbci.layers = 1;
	VkFramebuffer framebuffer = VK_NULL_HANDLE;
	if (vkCreateFramebuffer(device_, &fbci, nullptr, &framebuffer) != VK_SUCCESS) {
		return VK_NULL_HANDLE;
	}
	framebuffers_[key] = framebuffer;
	return framebuffer;
}

void VulkanBackend::Transition_Surface(VkCommandBuffer cmd, SurfaceHandle* surface,
                                       VkImageLayout to) {
	if (surface == nullptr || surface->system_memory() || surface->layout == to) return;
	const VkImageAspectFlags aspect =
	    surface->depth_stencil ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
	                           : VK_IMAGE_ASPECT_COLOR_BIT;
	Transition(cmd, surface->image->image, surface->layout, to, aspect);
	surface->layout = to;
	if (surface->owner != nullptr) surface->owner->layout = to;
}

void VulkanBackend::End_Current_Pass() {
	if (!in_scene_) return;
	vkCmdEndRenderPass(frame_cmd_);
}

bool VulkanBackend::Begin_Current_Pass() {
	if (!in_scene_) return true;
	if (current_color_ == nullptr) return false;
	Transition_Surface(frame_cmd_, current_color_,
	                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	Transition_Surface(frame_cmd_, current_depth_,
	                   VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
	VkRenderPass pass =
	    Get_Or_Create_Render_Pass(current_depth_ != nullptr,
	                              current_color_->written_this_frame);
	if (pass == VK_NULL_HANDLE) return false;
	VkFramebuffer framebuffer =
	    Get_Or_Create_Framebuffer(pass, current_color_, current_depth_);
	if (framebuffer == VK_NULL_HANDLE) return false;

	VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
	rpbi.renderPass = pass;
	rpbi.framebuffer = framebuffer;
	rpbi.renderArea = {{0, 0}, {device_target_width_, device_target_height_}};
	vkCmdBeginRenderPass(frame_cmd_, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
	// Anything the pass records lands in the target, so a later pass on it has to
	// LOAD rather than discard.
	current_color_->written_this_frame = true;
	return true;
}

VkCommandBuffer VulkanBackend::Begin_Transfer(bool& one_shot) {
	if (in_scene_) {
		// Inside the frame: the copy has to see the draws that came before it, so it
		// is recorded into the same command buffer, between render passes.
		one_shot = false;
		End_Current_Pass();
		return frame_cmd_;
	}
	one_shot = true;
	return Begin_One_Shot();
}

bool VulkanBackend::End_Transfer(VkCommandBuffer cmd, bool one_shot) {
	if (one_shot) return End_One_Shot(cmd);
	return Begin_Current_Pass();
}

bool VulkanBackend::Flush_Frame_Commands(bool end_pass_first) {
	if (!in_scene_) return true;
	// Everything recorded so far has to have executed before the host can look at
	// what it produced. D3D8 hides this inside LockRect on a surface the device has
	// rendered into; here it is an explicit mid-frame submit.
	if (end_pass_first) End_Current_Pass();
	if (vkEndCommandBuffer(frame_cmd_) != VK_SUCCESS) return false;
	VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &frame_cmd_;
	if (vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS) return false;
	if (vkQueueWaitIdle(queue_) != VK_SUCCESS) return false;
	vkResetCommandBuffer(frame_cmd_, 0);
	VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	if (vkBeginCommandBuffer(frame_cmd_, &bi) != VK_SUCCESS) return false;
	// The target keeps its pixels across the split, so the reopened pass loads
	// rather than discards: written_this_frame is deliberately left alone.
	return Begin_Current_Pass();
}

// ---------------------------------------------------------------------------
// The GPU-write funnels, enumerated (renderer-resource-seam.md §4.4)
//
// Every path by which the GPU can make an image's contents newer than any host
// copy of them goes through Mark_Gpu_Write:
//
//   1. Set_Render_Target      binds a surface as the colour or depth attachment,
//                             which is what makes the following draws write it.
//   2. Clear                  vkCmdClearAttachments into the current target.
//   3. Prepare_Draw           a draw into the current target.
//   4. Copy_Rects             the destination half, image or host buffer.
//   5. Update_Texture         the destination texture's image.
//
// Two GPU writes are deliberately *not* funnels, and both are safe:
//
//   * Unlock_Texture's vkCmdCopyBufferToImage writes the image from the staging
//     bytes the host has just written, so the two agree afterwards rather than
//     diverging. It marks the level synced instead of dirty.
//   * the vkCmdClearColorImage in Create_Lockable_Texture and
//     Create_Render_Target_Texture writes zeroes, and every host copy of a
//     never-written level starts zeroed to match, so no readback can tell the
//     difference.
//
// Present's blit writes a swapchain image, which no lock can reach.
// ---------------------------------------------------------------------------

void VulkanBackend::Note_Texture_Layout(TextureHandle* texture, VkImageLayout layout) {
	if (texture == nullptr) return;
	texture->layout = layout;
	for (SurfaceHandle* surface : owned_surfaces_) {
		if (surface->owner == texture) surface->layout = layout;
	}
}

void VulkanBackend::Mark_Gpu_Write(Image* image) {
	if (image == nullptr) return;
	image->gpu_dirty = true;
	++resource_stats_.gpu_write_marks;
	// A lockable texture's staging copy is no longer its contents. Levels stay
	// individually tracked because a lock brings back one level, not the chain.
	if (image->owner != nullptr) {
		for (LockableLevel& l : image->owner->levels) l.staging_synced = false;
	}
}

void VulkanBackend::Mark_Gpu_Write(SurfaceHandle* surface) {
	if (surface == nullptr) return;
	if (surface->system_memory()) {
		surface->host_gpu_dirty = true;
		++resource_stats_.gpu_write_marks;
		return;
	}
	Mark_Gpu_Write(surface->image);
}

bool VulkanBackend::Create_Descriptor_Machinery() {
	// Binding 1 is an array of kMaxTextureStages samplers rather than one binding
	// per stage: D3D8's stage count is fixed at 8, and an array keeps the descriptor
	// set layout independent of how many stages a given draw enables.
	VkDescriptorSetLayoutBinding bindings[2]{};
	bindings[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
	               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
	bindings[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxTextureStages,
	               VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};

	VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	lci.bindingCount = 2;
	lci.pBindings = bindings;
	VK_CHECK(vkCreateDescriptorSetLayout(device_, &lci, nullptr, &set_layout_));

	// No push constants: the fixed-function state outgrew the 128-byte guaranteed
	// minimum once transforms, lights and 8 stages had to travel with each draw.
	VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	plci.setLayoutCount = 1;
	plci.pSetLayouts = &set_layout_;
	VK_CHECK(vkCreatePipelineLayout(device_, &plci, nullptr, &pipeline_layout_));

	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(physical_, &props);
	const VkDeviceSize align = props.limits.minUniformBufferOffsetAlignment;
	ubo_stride_ = ((sizeof(DrawUniforms) + align - 1) / (align ? align : 1)) * (align ? align : 1);
	if (ubo_stride_ == 0) ubo_stride_ = sizeof(DrawUniforms);

	// A test limit smaller than one block sizes the first block down to it, so the negative
	// control allocates exactly what the old fixed preallocation did.
	if (const char* limit = std::getenv("ZH_RENDER_MAX_DRAWS")) {
		const long value = std::strtol(limit, nullptr, 10);
		if (value > 0) draw_limit_ = static_cast<uint32_t>(value);
	}
	if (const char* report = std::getenv("ZH_RENDER_DRAW_REPORT")) {
		const long value = std::strtol(report, nullptr, 10);
		if (value > 0) draw_report_interval_ = static_cast<uint32_t>(value);
	}
	// One block up front, so the common frame allocates nothing at draw time.
	if (!Add_Draw_Block()) return false;

	// Backing store for the shader inputs an FVF does not supply, filled with the
	// values D3D8 substitutes (see DummyVertex). A single element suffices because
	// the binding steps per instance and every draw issues one instance.
	{
		const DummyVertex dummy;
		if (!Upload_Buffer(&dummy, sizeof(dummy), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
		                   dummy_vertex_buffer_)) {
			return false;
		}
	}
	return true;
}

bool VulkanBackend::Add_Draw_Block() {
	// How many draws this block carries. Under a test limit the blocks stop exactly at it,
	// so the negative control's capacity is the number it asked for and not a rounded-up one.
	uint32_t draws = kDrawsPerBlock;
	if (draw_limit_ != 0) {
		if (draw_stats_.descriptor_capacity >= draw_limit_) return false;
		draws = draw_limit_ - draw_stats_.descriptor_capacity;
		if (draws > kDrawsPerBlock) draws = kDrawsPerBlock;
	}

	DrawBlock block;
	VkDescriptorPoolSize sizes[2]{};
	sizes[0] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, draws};
	sizes[1] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, draws * kMaxTextureStages};
	VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	dpci.maxSets = draws;
	dpci.poolSizeCount = 2;
	dpci.pPoolSizes = sizes;
	if (vkCreateDescriptorPool(device_, &dpci, nullptr, &block.pool) != VK_SUCCESS) {
		std::fprintf(stderr, "draw resources: vkCreateDescriptorPool failed for %u draws\n",
		             draws);
		return false;
	}

	std::vector<VkDescriptorSetLayout> layouts(draws, set_layout_);
	VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	dsai.descriptorPool = block.pool;
	dsai.descriptorSetCount = draws;
	dsai.pSetLayouts = layouts.data();
	block.sets.resize(draws);
	if (vkAllocateDescriptorSets(device_, &dsai, block.sets.data()) != VK_SUCCESS) {
		std::fprintf(stderr, "draw resources: vkAllocateDescriptorSets failed for %u draws\n",
		             draws);
		vkDestroyDescriptorPool(device_, block.pool, nullptr);
		return false;
	}

	if (!Allocate_Buffer(ubo_stride_ * draws, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
	                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	                     block.uniforms)) {
		vkDestroyDescriptorPool(device_, block.pool, nullptr);
		return false;
	}
	// Mapped once for the block's lifetime rather than per draw: at a mission frame's draw
	// count the map/unmap pair is thousands of calls a frame, and the memory is coherent, so
	// the write needs no flush.
	if (vkMapMemory(device_, block.uniforms.memory, 0, VK_WHOLE_SIZE, 0, &block.mapped) !=
	    VK_SUCCESS) {
		std::fprintf(stderr, "draw resources: vkMapMemory failed for %u draws\n", draws);
		vkDestroyDescriptorPool(device_, block.pool, nullptr);
		return false;
	}

	draw_blocks_.push_back(block);
	draw_stats_.descriptor_capacity += draws;
	draw_stats_.descriptor_blocks = static_cast<uint32_t>(draw_blocks_.size());
	return true;
}

bool VulkanBackend::Draw_Slot(uint32_t index, VkDescriptorSet& out_set, VkBuffer& out_buffer,
                              VkDeviceSize& out_offset, void*& out_mapped) {
	while (index >= draw_stats_.descriptor_capacity) {
		if (!Add_Draw_Block()) return false;
	}
	// Blocks are uniform in size except the last one under a test limit, and a draw only
	// reaches a block once every earlier block is full, so the arithmetic is exact.
	uint32_t remaining = index;
	for (DrawBlock& block : draw_blocks_) {
		const uint32_t draws = static_cast<uint32_t>(block.sets.size());
		if (remaining < draws) {
			out_set = block.sets[remaining];
			out_buffer = block.uniforms.buffer;
			out_offset = ubo_stride_ * remaining;
			out_mapped = static_cast<uint8_t*>(block.mapped) + out_offset;
			return true;
		}
		remaining -= draws;
	}
	return false;
}

bool VulkanBackend::Create_Shaders() {
	const char* dir = std::getenv("SPIKE_SHADER_DIR");
	const std::string base = dir ? std::string(dir) : std::string(SPIKE_SHADER_DIR);
	for (const auto& [path, module] : {std::pair<std::string, VkShaderModule*>{base + "/fixedfunc.vert.spv", &vert_module_},
	                                   std::pair<std::string, VkShaderModule*>{base + "/fixedfunc.frag.spv", &frag_module_}}) {
		std::vector<uint32_t> code = Read_Spirv(path);
		if (code.empty()) {
			std::fprintf(stderr, "cannot read SPIR-V module %s\n", path.c_str());
			return false;
		}
		VkShaderModuleCreateInfo sci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
		sci.codeSize = code.size() * 4;
		sci.pCode = code.data();
		VK_CHECK(vkCreateShaderModule(device_, &sci, nullptr, module));
	}
	return true;
}

bool VulkanBackend::Create_Swapchain(void* window_handle) {
#if defined(SPIKE_WITH_SDL) || defined(SPIKE_WITH_PLATFORM_WINDOW)
	if (!presentation_required_) return true;
	(void)window_handle;
	if (!Build_Swapchain()) return false;
	if (swapchain_ == VK_NULL_HANDLE) {
		std::fprintf(stderr, "Vulkan backend: no swapchain was created for a window, so nothing "
		                     "could be presented; failing initialisation rather than running "
		                     "blind.\n");
		return false;
	}

	VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	VK_CHECK(vkCreateFence(device_, &fci, nullptr, &acquire_fence_));
	return true;
#else
	(void)window_handle;
	if (presentation_required_) {
		// The build-time half of the guard. This translation unit has no surface code at all, so
		// a windowed run cannot present and must not start: it would draw every frame and show
		// none of them. Compile the backend with SPIKE_WITH_PLATFORM_WINDOW (what
		// scripts/native-build.py defines for the engine) or SPIKE_WITH_SDL.
		std::fprintf(stderr, "Vulkan backend: compiled without SPIKE_WITH_PLATFORM_WINDOW and "
		                     "without SPIKE_WITH_SDL, so it has no surface and no swapchain and "
		                     "cannot present to a window.\n");
		return false;
	}
	return true;
#endif
}

void VulkanBackend::Destroy_Swapchain() {
	if (swapchain_ == VK_NULL_HANDLE) return;
	vkDeviceWaitIdle(device_);
	vkDestroySwapchainKHR(device_, swapchain_, nullptr);
	swapchain_ = VK_NULL_HANDLE;
	swapchain_images_.clear();
}

bool VulkanBackend::Build_Swapchain() {
#if defined(SPIKE_WITH_SDL) || defined(SPIKE_WITH_PLATFORM_WINDOW)
	if (surface_ == VK_NULL_HANDLE) {
		// A windowed backend with no surface has nothing to build a swapchain on. Succeeding here
		// is what let Present() report success with no swapchain.
		if (presentation_required_) {
			std::fprintf(stderr, "Vulkan backend: no Vulkan surface for the window, so no "
			                     "swapchain can be built.\n");
			return false;
		}
		return true;
	}

	VkSurfaceCapabilitiesKHR caps{};
	VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps));
	uint32_t format_count = 0;
	VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &format_count, nullptr));
	std::vector<VkSurfaceFormatKHR> formats(format_count);
	VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &format_count, formats.data()));
	if (formats.empty()) return false;
	VkSurfaceFormatKHR chosen = formats[0];
	swapchain_format_ = chosen.format;

	VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
	sci.surface = surface_;
	sci.minImageCount = caps.minImageCount < 2 ? 2 : caps.minImageCount;
	sci.imageFormat = chosen.format;
	sci.imageColorSpace = chosen.colorSpace;
	// The window, not the render target, decides the presented extent: the colour target keeps
	// its own resolution and is scaled on present, which is what lets the window be resized
	// without re-creating every render target.
	VkExtent2D extent = caps.currentExtent;
	// The swapchain is in pixels, so a surface that leaves the extent to the application
	// gets the colour target's pixel size, not the client area's points.
	if (extent.width == 0xFFFFFFFFu) extent = {device_width_, device_height_};
	if (extent.width == 0 || extent.height == 0) return false;
	swapchain_extent_ = extent;
	sci.imageExtent = extent;
	sci.imageArrayLayers = 1;
	sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	sci.preTransform = caps.currentTransform;
	sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
	sci.clipped = VK_TRUE;
	VK_CHECK(vkCreateSwapchainKHR(device_, &sci, nullptr, &swapchain_));

	uint32_t image_count = 0;
	VK_CHECK(vkGetSwapchainImagesKHR(device_, swapchain_, &image_count, nullptr));
	swapchain_images_.resize(image_count);
	VK_CHECK(vkGetSwapchainImagesKHR(device_, swapchain_, &image_count, swapchain_images_.data()));
	return true;
#else
	return !presentation_required_;
#endif
}

bool VulkanBackend::Init(void* window_handle, uint32_t width, uint32_t height) {
	width_ = width;
	height_ = height;
	window_handle_ = window_handle;
	presentation_required_ = (!headless_ && window_handle != nullptr);

	if (!Create_Instance(window_handle)) return false;

#ifdef SPIKE_WITH_PLATFORM_WINDOW
	if (!headless_ && window_handle != nullptr) {
		if (!WWPlatform::Window_Create_Vulkan_Surface(window_handle, instance_, &surface_)) {
			std::fprintf(stderr, "Window_Create_Vulkan_Surface failed: %s\n",
			             WWPlatform::Window_Last_Error());
			return false;
		}
	}
#elif defined(SPIKE_WITH_SDL)
	if (!headless_ && window_handle != nullptr) {
		if (!SDL_Vulkan_CreateSurface(static_cast<SDL_Window*>(window_handle), instance_, &surface_)) {
			std::fprintf(stderr, "SDL_Vulkan_CreateSurface failed: %s\n", SDL_GetError());
			return false;
		}
	}
#endif

	if (!Pick_Device()) return false;
	// The scale has to be known before the targets are created, because it is their size.
	render_scale_ = Initial_Render_Scale(window_handle);
	if (!Create_Render_Targets()) return false;
	if (render_scale_ != 1.0f) {
		std::fprintf(stderr,
		             "Vulkan backend: backing scale %.2f, so a %ux%u point back buffer renders "
		             "at %ux%u pixels\n",
		             static_cast<double>(render_scale_), width_, height_, device_width_,
		             device_height_);
	}
	if (!Create_Descriptor_Machinery()) return false;
	if (!Create_Shaders()) return false;
	if (!Create_Swapchain(window_handle)) return false;

	// D3D8 device defaults, as DX8Wrapper::Set_Default_Global_Render_States assumes.
	render_states_[D3DRS_ZENABLE] = 1;
	render_states_[D3DRS_ZWRITEENABLE] = 1;
	render_states_[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
	render_states_[D3DRS_CULLMODE] = D3DCULL_CCW;
	render_states_[D3DRS_FILLMODE] = D3DFILL_SOLID;
	render_states_[D3DRS_SHADEMODE] = 2;
	render_states_[D3DRS_SRCBLEND] = D3DBLEND_ONE;
	render_states_[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
	render_states_[D3DRS_COLORWRITEENABLE] = 0xf;
	render_states_[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS;
	// D3D8's stencil defaults. The masks matter: a zero compare mask makes every
	// comparison compare 0 with 0, so the stencil test passes everywhere.
	render_states_[D3DRS_STENCILFUNC] = D3DCMP_ALWAYS;
	render_states_[D3DRS_STENCILFAIL] = D3DSTENCILOP_KEEP;
	render_states_[D3DRS_STENCILZFAIL] = D3DSTENCILOP_KEEP;
	render_states_[D3DRS_STENCILPASS] = D3DSTENCILOP_KEEP;
	render_states_[D3DRS_STENCILMASK] = 0xffffffff;
	render_states_[D3DRS_STENCILWRITEMASK] = 0xffffffff;
	render_states_[D3DRS_TEXTUREFACTOR] = 0xffffffff;
	// dx8wrapper.cpp:3771 (Set_Default_Global_Render_States) turns lighting off and
	// colour-vertex on, so an unlit vertex colour passes straight through; that is
	// the state the engine draws most of its geometry in, not D3D8's own defaults.
	render_states_[D3DRS_LIGHTING] = 0;
	render_states_[D3DRS_COLORVERTEX] = 1;
	render_states_[D3DRS_LOCALVIEWER] = 1;
	render_states_[D3DRS_NORMALIZENORMALS] = 0;
	render_states_[D3DRS_DIFFUSEMATERIALSOURCE] = D3DMCS_COLOR1;
	render_states_[D3DRS_SPECULARMATERIALSOURCE] = D3DMCS_COLOR2;
	render_states_[D3DRS_AMBIENTMATERIALSOURCE] = D3DMCS_MATERIAL;
	render_states_[D3DRS_EMISSIVEMATERIALSOURCE] = D3DMCS_MATERIAL;
	// dx8wrapper.cpp:402 sets table fog off and leaves vertex fog to the caller.
	render_states_[D3DRS_FOGTABLEMODE] = D3DFOG_NONE;
	render_states_[D3DRS_FOGVERTEXMODE] = D3DFOG_NONE;
	// Point-sprite defaults, as D3D8 documents them: one-pixel points, no scaling.
	// These are float-valued render states, so they arrive as bit patterns.
	render_states_[D3DRS_POINTSIZE] = Float_To_Dword(1.0f);
	render_states_[D3DRS_POINTSIZE_MIN] = Float_To_Dword(0.0f);
	render_states_[D3DRS_POINTSIZE_MAX] = Float_To_Dword(max_point_size_);
	render_states_[D3DRS_POINTSCALE_A] = Float_To_Dword(1.0f);
	render_states_[D3DRS_POINTSCALE_B] = Float_To_Dword(0.0f);
	render_states_[D3DRS_POINTSCALE_C] = Float_To_Dword(0.0f);
	render_states_[D3DRS_BLENDOP] = D3DBLENDOP_ADD;
	// The four states the engine sets that a Vulkan backend has nothing to serve them
	// with. They are shadowed so a GetRenderState still answers, and declared here so
	// the coverage gate reports them as a decision rather than as a hole.
	// COVERAGE-IGNORE: D3DRS_CLIPPING - Vulkan always clips to the view volume; there
	// is no pipeline bit to turn primitive clipping off, and the engine only ever sets
	// it to TRUE (its D3D8 default).
	// COVERAGE-IGNORE: D3DRS_DITHERENABLE - no Vulkan equivalent. D3D8 dithered when
	// blending to a 16-bit target; the backend renders 8 bits per channel, which is
	// what the dithering existed to hide.
	// COVERAGE-IGNORE: D3DRS_SOFTWAREVERTEXPROCESSING - a D3D8 device-model switch
	// between the driver's vertex pipeline and D3D8's own; the Vulkan backend has one
	// vertex path.
	// COVERAGE-IGNORE: D3DRS_PATCHSEGMENTS - N-patch tessellation, which no D3D8 driver
	// the game shipped against implemented either.
	render_states_[D3DRS_CLIPPING] = 1;
	render_states_[D3DRS_DITHERENABLE] = 0;
	render_states_[D3DRS_SOFTWAREVERTEXPROCESSING] = 0;
	render_states_[D3DRS_PATCHSEGMENTS] = Float_To_Dword(1.0f);

	// D3D8's implicit SetViewport at device creation: the whole render target.
	viewport_ = ViewportRect{0, 0, width_, height_, 0.0f, 1.0f};

	// DrawPrimitiveUP's scratch vertices. 1 MiB is well above the largest UP draw
	// the engine issues (debug lines and the 2D UI batches).
	if (!Allocate_Buffer(1u << 20, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
	                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	                     up_ring_)) {
		return false;
	}
	if (vkMapMemory(device_, up_ring_.memory, 0, up_ring_.size, 0, &up_mapped_) != VK_SUCCESS) {
		return false;
	}

	const uint32_t white = 0xffffffffu;
	white_texture_ = Create_Texture(1, 1, reinterpret_cast<const uint8_t*>(&white));
	return white_texture_ != nullptr;
}

void VulkanBackend::Shutdown() {
	if (device_ == VK_NULL_HANDLE) return;
	vkDeviceWaitIdle(device_);

	for (auto& [key, pipeline] : pipelines_) vkDestroyPipeline(device_, pipeline, nullptr);
	pipelines_.clear();
	for (auto& [key, sampler] : samplers_) vkDestroySampler(device_, sampler, nullptr);
	samplers_.clear();

	auto free_buffer = [&](Buffer& b) {
		if (b.buffer) vkDestroyBuffer(device_, b.buffer, nullptr);
		if (b.memory) vkFreeMemory(device_, b.memory, nullptr);
		b = Buffer{};
	};
	auto free_image = [&](Image& i) {
		if (i.view) vkDestroyImageView(device_, i.view, nullptr);
		if (i.image) vkDestroyImage(device_, i.image, nullptr);
		if (i.memory) vkFreeMemory(device_, i.memory, nullptr);
		i = Image{};
	};

	auto free_staging_block = [&](StagingBlock& b) {
		if (b.mapped != nullptr) vkUnmapMemory(device_, b.buffer.memory);
		free_buffer(b.buffer);
		b = StagingBlock{};
	};

	for (auto* t : owned_textures_) {
		free_image(t->image);
		// A block still held here belongs to a resource that was never unlocked, or
		// one whose block is pinned for C7/C8. Everything else is in the pool.
		free_staging_block(t->staging);
		t->staging_mapped = nullptr;
		delete t;
	}
	owned_textures_.clear();
	for (StagingBlock& b : staging_free_) free_staging_block(b);
	staging_free_.clear();
	for (auto* vb : owned_vbs_) {
		if (vb->mapped != nullptr) vkUnmapMemory(device_, vb->buffer.memory);
		free_buffer(vb->buffer);
		delete vb;
	}
	owned_vbs_.clear();
	for (auto* ib : owned_ibs_) {
		if (ib->mapped != nullptr) vkUnmapMemory(device_, ib->buffer.memory);
		free_buffer(ib->buffer);
		delete ib;
	}
	owned_ibs_.clear();
	for (auto* s : owned_surfaces_) {
		// A system-memory surface's bytes stay mapped for its whole life, the same
		// contract a lockable texture's staging memory has.
		if (s->mapped != nullptr) vkUnmapMemory(device_, s->bits.memory);
		free_buffer(s->bits);
		delete s;
	}
	owned_surfaces_.clear();
	// The default colour target grows a host-visible buffer the first time anything
	// reads it (Surface_Bits), and it is not in owned_surfaces_.
	if (default_color_surface_.mapped != nullptr) {
		vkUnmapMemory(device_, default_color_surface_.bits.memory);
		default_color_surface_.mapped = nullptr;
	}
	free_buffer(default_color_surface_.bits);
	current_color_ = nullptr;
	current_depth_ = nullptr;

	for (DrawBlock& block : draw_blocks_) {
		if (block.mapped != nullptr) {
			vkUnmapMemory(device_, block.uniforms.memory);
			block.mapped = nullptr;
		}
		free_buffer(block.uniforms);
		if (block.pool) vkDestroyDescriptorPool(device_, block.pool, nullptr);
	}
	draw_blocks_.clear();
	draw_stats_.descriptor_capacity = 0;
	draw_stats_.descriptor_blocks = 0;
	free_buffer(dummy_vertex_buffer_);
	if (up_mapped_ != nullptr) {
		vkUnmapMemory(device_, up_ring_.memory);
		up_mapped_ = nullptr;
	}
	free_buffer(up_ring_);
	free_image(color_target_);
	free_image(depth_target_);

	if (acquire_fence_) vkDestroyFence(device_, acquire_fence_, nullptr);
	if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
	for (auto& fb : framebuffers_) vkDestroyFramebuffer(device_, fb.second, nullptr);
	framebuffers_.clear();
	for (auto& rp : render_passes_) vkDestroyRenderPass(device_, rp.second, nullptr);
	render_passes_.clear();
	if (pipeline_layout_) vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
	if (set_layout_) vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
	if (vert_module_) vkDestroyShaderModule(device_, vert_module_, nullptr);
	if (frag_module_) vkDestroyShaderModule(device_, frag_module_, nullptr);
	if (frame_fence_) vkDestroyFence(device_, frame_fence_, nullptr);
	if (command_pool_) vkDestroyCommandPool(device_, command_pool_, nullptr);
	vkDestroyDevice(device_, nullptr);
	device_ = VK_NULL_HANDLE;
	if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
	if (messenger_ != VK_NULL_HANDLE) {
		auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
		    vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
		if (destroy != nullptr) destroy(instance_, messenger_, nullptr);
		messenger_ = VK_NULL_HANDLE;
	}
	if (instance_) vkDestroyInstance(instance_, nullptr);
	instance_ = VK_NULL_HANDLE;
}

// ---------------------------------------------------------------------------
// resources
// ---------------------------------------------------------------------------

namespace {

// How one of the engine's surface formats reaches the GPU.
struct FormatPlan {
	VkFormat vk = VK_FORMAT_B8G8R8A8_UNORM;
	// D3DFMT_L8/A8/A8L8 have no Vulkan format with the same channel semantics; the
	// data goes in as R8/R8G8 and the view swizzle reproduces D3D8's expansion
	// (L8 -> (L,L,L,1), A8 -> (1,1,1,A), A8L8 -> (L,L,L,A)).
	VkComponentMapping swizzle{VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
	                           VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
	// Formats with no Vulkan equivalent at all are expanded to B8G8R8A8 on upload.
	bool expand_to_bgra8 = false;
	uint32_t block_size = 1;  // texels per block edge (4 for BC)
	uint32_t block_bytes = 4; // bytes per block (or per texel when block_size == 1)
};

// view_swizzle: whether the device permits a non-identity VkImageView component
// mapping. MoltenVK's portability subset reports imageViewFormatSwizzle as false
// -- Metal has no equivalent -- so on Apple the channel expansion D3D8 defines
// for L8/A8/A8L8/X8R8G8B8 has to happen on the CPU at upload instead.
FormatPlan Plan_For(TextureFormat format, bool view_swizzle) {
	FormatPlan p;
	const VkComponentMapping lll1{VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R,
	                              VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ONE};
	// D3D8's documentation does not say what colour an alpha-only texture samples
	// as. (0,0,0,A) is what D3D9 and DXGI define for the same format, so that is
	// what this reproduces; the fixed-function tests report the colour as
	// unverified rather than asserting it is D3D8's behaviour.
	const VkComponentMapping zero_a{VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO,
	                                VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_R};
	const VkComponentMapping lllg{VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R,
	                              VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G};
	switch (format) {
	case TextureFormat::A8R8G8B8:
		// D3DFMT_A8R8G8B8 is B,G,R,A in memory, so B8G8R8A8 is the matching Vulkan
		// format -- not R8G8B8A8. Getting this wrong swaps red and blue everywhere.
		p.vk = VK_FORMAT_B8G8R8A8_UNORM;
		p.block_bytes = 4;
		break;
	case TextureFormat::X8R8G8B8:
		// Same bits, but the X channel is not alpha: D3D8 samples 1.0 for it, so the
		// stored byte has to be swizzled away or a stale byte becomes transparency.
		p.vk = VK_FORMAT_B8G8R8A8_UNORM;
		if (view_swizzle) {
			p.swizzle.a = VK_COMPONENT_SWIZZLE_ONE;
		} else {
			p.expand_to_bgra8 = true;
		}
		p.block_bytes = 4;
		break;
	case TextureFormat::R8G8B8:
	case TextureFormat::P8:
	case TextureFormat::A4R4G4B4:
		// R8G8B8: no 24-bit Vulkan format is required to be sampleable.
		// P8: Vulkan has no palettised format at all.
		// A4R4G4B4: VK_FORMAT_A4R4G4B4_UNORM_PACK16 is Vulkan 1.3/EXT_4444_formats
		// and MoltenVK does not expose it (docs/porting/moltenvk-findings.md); the
		// 1.0 core B4G4R4A4 has the channels in the other order.
		p.vk = VK_FORMAT_B8G8R8A8_UNORM;
		p.expand_to_bgra8 = true;
		p.block_bytes = 4;
		break;
	case TextureFormat::A1R5G5B5:
		p.vk = VK_FORMAT_A1R5G5B5_UNORM_PACK16;
		p.block_bytes = 2;
		break;
	case TextureFormat::R5G6B5:
		p.vk = VK_FORMAT_R5G6B5_UNORM_PACK16;
		p.block_bytes = 2;
		break;
	case TextureFormat::L8:
		p.vk = view_swizzle ? VK_FORMAT_R8_UNORM : VK_FORMAT_B8G8R8A8_UNORM;
		p.swizzle = view_swizzle ? lll1 : p.swizzle;
		p.expand_to_bgra8 = !view_swizzle;
		p.block_bytes = view_swizzle ? 1 : 4;
		break;
	case TextureFormat::A8:
		p.vk = view_swizzle ? VK_FORMAT_R8_UNORM : VK_FORMAT_B8G8R8A8_UNORM;
		p.swizzle = view_swizzle ? zero_a : p.swizzle;
		p.expand_to_bgra8 = !view_swizzle;
		p.block_bytes = view_swizzle ? 1 : 4;
		break;
	case TextureFormat::A8L8:
		p.vk = view_swizzle ? VK_FORMAT_R8G8_UNORM : VK_FORMAT_B8G8R8A8_UNORM;
		p.swizzle = view_swizzle ? lllg : p.swizzle;
		p.expand_to_bgra8 = !view_swizzle;
		p.block_bytes = view_swizzle ? 2 : 4;
		break;
	case TextureFormat::V8U8:
		p.vk = VK_FORMAT_R8G8_SNORM;
		p.block_bytes = 2;
		break;
	case TextureFormat::DXT1:
		p.vk = VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
		p.block_size = 4;
		p.block_bytes = 8;
		break;
	case TextureFormat::DXT2:
	case TextureFormat::DXT3:
		p.vk = VK_FORMAT_BC2_UNORM_BLOCK;
		p.block_size = 4;
		p.block_bytes = 16;
		break;
	case TextureFormat::DXT4:
	case TextureFormat::DXT5:
		p.vk = VK_FORMAT_BC3_UNORM_BLOCK;
		p.block_size = 4;
		p.block_bytes = 16;
		break;
	}
	return p;
}

// CPU expansion for the formats with no Vulkan equivalent, and for the
// swizzle-expanded ones when the device has no view swizzle. Output is
// B8G8R8A8, i.e. D3DFMT_A8R8G8B8 byte order.
void Expand_To_Bgra8(TextureFormat format, const TextureMip& mip, const uint32_t* palette,
                     std::vector<uint8_t>& out) {
	const size_t texels = static_cast<size_t>(mip.width) * mip.height;
	out.resize(texels * 4);
	const auto* src = static_cast<const uint8_t*>(mip.data);
	for (size_t i = 0; i < texels; ++i) {
		uint8_t b = 0, g = 0, r = 0, a = 255;
		switch (format) {
		case TextureFormat::R8G8B8:
			b = src[i * 3 + 0];
			g = src[i * 3 + 1];
			r = src[i * 3 + 2];
			break;
		case TextureFormat::P8: {
			const uint32_t entry = palette != nullptr ? palette[src[i]] : 0u;
			b = static_cast<uint8_t>(entry & 0xff);
			g = static_cast<uint8_t>((entry >> 8) & 0xff);
			r = static_cast<uint8_t>((entry >> 16) & 0xff);
			a = static_cast<uint8_t>((entry >> 24) & 0xff);
			break;
		}
		case TextureFormat::X8R8G8B8:
			b = src[i * 4 + 0];
			g = src[i * 4 + 1];
			r = src[i * 4 + 2];
			// a stays 255: the X byte is not alpha.
			break;
		case TextureFormat::L8:
			b = g = r = src[i];
			break;
		case TextureFormat::A8:
			// See Plan_For: (0,0,0,A) is D3D9/DXGI's rule, not a documented D3D8 one.
			b = g = r = 0;
			a = src[i];
			break;
		case TextureFormat::A8L8:
			b = g = r = src[i * 2 + 0];
			a = src[i * 2 + 1];
			break;
		case TextureFormat::A4R4G4B4: {
			uint16_t v = 0;
			std::memcpy(&v, src + i * 2, sizeof(v));
			// 4-bit -> 8-bit by bit replication (n*17), which is the exact
			// round-trip: 0xf -> 0xff, 0x0 -> 0x00.
			b = static_cast<uint8_t>((v & 0xf) * 17);
			g = static_cast<uint8_t>(((v >> 4) & 0xf) * 17);
			r = static_cast<uint8_t>(((v >> 8) & 0xf) * 17);
			a = static_cast<uint8_t>(((v >> 12) & 0xf) * 17);
			break;
		}
		default: break;
		}
		out[i * 4 + 0] = b;
		out[i * 4 + 1] = g;
		out[i * 4 + 2] = r;
		out[i * 4 + 3] = a;
	}
}

// The inverse of Expand_To_Bgra8, for the readback half of the no-view-swizzle
// path: the image holds B8G8R8A8 and the caller's lock has to see the D3D8 format
// it wrote. Every expansion above is invertible (4-bit replication n*17 contracts
// by >>4, L8/A8/A8L8 pick the channel they came from) with one documented
// exception: X8R8G8B8's X byte is not stored in the image, because D3D8 does not
// sample it, so it contracts back as zero. Returns false for a format the
// expansion pass does not produce, so a caller can refuse rather than guess.
// Asked separately from the conversion itself so a caller that must refuse before it
// copies anything can find out without running a dummy pass.
bool Contractable_From_Bgra8(TextureFormat format) {
	switch (format) {
	case TextureFormat::R8G8B8:
	case TextureFormat::X8R8G8B8:
	case TextureFormat::L8:
	case TextureFormat::A8:
	case TextureFormat::A8L8:
	case TextureFormat::A4R4G4B4: return true;
	default: return false;
	}
}

bool Contract_From_Bgra8(TextureFormat format, const uint8_t* bgra, uint32_t width,
                         uint32_t height, uint8_t* dst, uint32_t dst_pitch) {
	if (!Contractable_From_Bgra8(format)) return false;
	for (uint32_t y = 0; y < height; ++y) {
		const uint8_t* src = bgra + static_cast<size_t>(y) * width * 4;
		uint8_t* row = dst + static_cast<size_t>(y) * dst_pitch;
		for (uint32_t x = 0; x < width; ++x) {
			const uint8_t b = src[x * 4 + 0];
			const uint8_t g = src[x * 4 + 1];
			const uint8_t r = src[x * 4 + 2];
			const uint8_t a = src[x * 4 + 3];
			switch (format) {
			case TextureFormat::R8G8B8:
				row[x * 3 + 0] = b;
				row[x * 3 + 1] = g;
				row[x * 3 + 2] = r;
				break;
			case TextureFormat::X8R8G8B8:
				row[x * 4 + 0] = b;
				row[x * 4 + 1] = g;
				row[x * 4 + 2] = r;
				row[x * 4 + 3] = 0; // the X byte D3D8 ignores; not stored
				break;
			case TextureFormat::L8: row[x] = b; break;
			case TextureFormat::A8: row[x] = a; break;
			case TextureFormat::A8L8:
				row[x * 2 + 0] = b;
				row[x * 2 + 1] = a;
				break;
			case TextureFormat::A4R4G4B4: {
				const uint16_t v = static_cast<uint16_t>(
				    ((a >> 4) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4));
				std::memcpy(row + x * 2, &v, sizeof(v));
				break;
			}
			default: return false;
			}
		}
	}
	return true;
}

} // namespace

bool VulkanBackend::Supports_Texture_Format(TextureFormat format) const {
	const FormatPlan plan = Plan_For(format, view_swizzle_);
	VkFormatProperties props{};
	vkGetPhysicalDeviceFormatProperties(physical_, plan.vk, &props);
	return (props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
}

TextureHandle* VulkanBackend::Create_Texture(const TextureDesc& desc) {
	if (desc.mips == nullptr || desc.mip_count == 0) return nullptr;
	const FormatPlan plan = Plan_For(desc.format, view_swizzle_);
	if (!Supports_Texture_Format(desc.format)) {
		std::fprintf(stderr, "Create_Texture: device cannot sample VkFormat %d\n",
		             static_cast<int>(plan.vk));
		return nullptr;
	}

	auto* handle = new TextureHandle();
	handle->image.width = desc.mips[0].width;
	handle->image.height = desc.mips[0].height;
	handle->image.mip_levels = desc.mip_count;

	VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = plan.vk;
	ici.extent = {handle->image.width, handle->image.height, 1};
	ici.mipLevels = desc.mip_count;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	// TRANSFER_SRC as well: any D3D8 surface can be a CopyRects source, and the
	// engine does blit out of ordinary textures.
	ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
	            VK_IMAGE_USAGE_SAMPLED_BIT;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (vkCreateImage(device_, &ici, nullptr, &handle->image.image) != VK_SUCCESS) {
		delete handle;
		return nullptr;
	}

	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(device_, handle->image.image, &req);
	uint32_t type = 0;
	if (!Find_Memory_Type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, type)) {
		delete handle;
		return nullptr;
	}
	VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	mai.allocationSize = req.size;
	mai.memoryTypeIndex = type;
	if (vkAllocateMemory(device_, &mai, nullptr, &handle->image.memory) != VK_SUCCESS ||
	    vkBindImageMemory(device_, handle->image.image, handle->image.memory, 0) != VK_SUCCESS) {
		delete handle;
		return nullptr;
	}

	// One staging buffer for the whole mip chain, tightly packed.
	std::vector<uint8_t> staging_bytes;
	std::vector<VkBufferImageCopy> copies;
	std::vector<uint8_t> expanded;
	for (uint32_t level = 0; level < desc.mip_count; ++level) {
		const TextureMip& mip = desc.mips[level];
		const uint8_t* src = static_cast<const uint8_t*>(mip.data);
		size_t bytes = mip.bytes;
		if (plan.expand_to_bgra8) {
			Expand_To_Bgra8(desc.format, mip, desc.palette, expanded);
			src = expanded.data();
			bytes = expanded.size();
		}
		VkBufferImageCopy copy{};
		copy.bufferOffset = staging_bytes.size();
		copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
		copy.imageExtent = {mip.width, mip.height, 1};
		copies.push_back(copy);
		staging_bytes.insert(staging_bytes.end(), src, src + bytes);
	}

	Buffer staging;
	if (!Upload_Buffer(staging_bytes.data(), staging_bytes.size(),
	                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging)) {
		delete handle;
		return nullptr;
	}

	VkCommandBuffer cmd = Begin_One_Shot();
	Transition(cmd, handle->image.image, VK_IMAGE_LAYOUT_UNDEFINED,
	           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
	           desc.mip_count);
	vkCmdCopyBufferToImage(cmd, staging.buffer, handle->image.image,
	                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                       static_cast<uint32_t>(copies.size()), copies.data());
	Transition(cmd, handle->image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
	           desc.mip_count);
	End_One_Shot(cmd);

	vkDestroyBuffer(device_, staging.buffer, nullptr);
	vkFreeMemory(device_, staging.memory, nullptr);

	VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	vci.image = handle->image.image;
	vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vci.format = plan.vk;
	vci.components = plan.swizzle;
	vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, desc.mip_count, 0, 1};
	if (vkCreateImageView(device_, &vci, nullptr, &handle->image.view) != VK_SUCCESS) {
		delete handle;
		return nullptr;
	}

	owned_textures_.push_back(handle);
	return handle;
}

TextureHandle* VulkanBackend::Create_Texture(uint32_t width, uint32_t height,
                                             const uint8_t* argb_pixels) {
	const TextureMip mip{argb_pixels, static_cast<size_t>(width) * height * 4, width, height};
	TextureDesc desc;
	desc.format = TextureFormat::A8R8G8B8;
	desc.mip_count = 1;
	desc.mips = &mip;
	return Create_Texture(desc);
}

VertexBufferHandle* VulkanBackend::Create_Vertex_Buffer(const void* data, size_t bytes,
                                                        uint32_t fvf) {
	auto* handle = new VertexBufferHandle();
	handle->fvf = fvf;
	if (!Decode_Fvf(fvf, handle->layout)) {
		std::fprintf(stderr, "Decode_Fvf: unsupported FVF 0x%x\n", fvf);
		delete handle;
		return nullptr;
	}
	if (!Upload_Buffer(data, bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, handle->buffer)) {
		delete handle;
		return nullptr;
	}
	owned_vbs_.push_back(handle);
	return handle;
}

IndexBufferHandle* VulkanBackend::Create_Index_Buffer(const uint16_t* data, size_t count) {
	auto* handle = new IndexBufferHandle();
	handle->count = static_cast<uint32_t>(count);
	if (!Upload_Buffer(data, count * sizeof(uint16_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
	                   handle->buffer)) {
		delete handle;
		return nullptr;
	}
	owned_ibs_.push_back(handle);
	return handle;
}

// ---------------------------------------------------------------------------
// lockable resources: emulating the D3D8 Lock/Unlock contract
//
// The engine's resource interfaces stay D3D8-shaped (see
// docs/porting/renderer-resource-seam.md), so the cost of that contract lands
// here: a permanently mapped host-visible allocation per lockable resource, a
// buffer-to-image copy per unlock, and a submit-and-wait per read-only lock.
// ---------------------------------------------------------------------------

namespace {

// Bytes per texel in the *D3D8* format, i.e. the layout the caller writes through
// the pointer Lock hands out. Zero for the block-compressed formats, which this
// path does not serve.
uint32_t Source_Texel_Bytes(TextureFormat format) {
	switch (format) {
	case TextureFormat::A8R8G8B8:
	case TextureFormat::X8R8G8B8: return 4;
	case TextureFormat::R8G8B8: return 3;
	case TextureFormat::A4R4G4B4:
	case TextureFormat::A1R5G5B5:
	case TextureFormat::R5G6B5:
	case TextureFormat::A8L8:
	case TextureFormat::V8U8: return 2;
	case TextureFormat::L8:
	case TextureFormat::A8:
	case TextureFormat::P8: return 1;
	default: return 0; // DXT1..DXT5
	}
}

// Staging blocks are rounded up to a power-of-two size class, with a 4 KiB floor, so
// a block freed by one lock can serve the next lock of a different resource. The
// price is at most 2x the requested bytes per block; the gain is that the number of
// host-visible allocations stops scaling with the number of locks.
VkDeviceSize Staging_Size_Class(VkDeviceSize bytes) {
	VkDeviceSize capacity = 4096;
	while (capacity < bytes) capacity *= 2;
	return capacity;
}

} // namespace

// Caller holds resource_mutex_.
bool VulkanBackend::Acquire_Staging(VkDeviceSize size, StagingBlock& out) {
	if (size == 0) return false;
	++resource_stats_.staging_acquires;

	// Smallest free block that fits: keeps a 1 MiB block available for the next lock
	// that actually needs 1 MiB instead of spending it on a 1 KiB one.
	size_t best = staging_free_.size();
	for (size_t i = 0; i < staging_free_.size(); ++i) {
		if (staging_free_[i].capacity < size) continue;
		if (best == staging_free_.size() ||
		    staging_free_[i].capacity < staging_free_[best].capacity) {
			best = i;
		}
	}
	if (best != staging_free_.size()) {
		out = staging_free_[best];
		staging_free_.erase(staging_free_.begin() + static_cast<long>(best));
		out.size = size;
		++resource_stats_.staging_reuses;
		--resource_stats_.staging_pool_blocks;
		resource_stats_.staging_pool_bytes -= out.capacity;
	} else {
		StagingBlock block;
		block.capacity = Staging_Size_Class(size);
		block.size = size;
		const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
		                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
		if (!Allocate_Buffer(block.capacity,
		                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
		                         VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		                     host, block.buffer) ||
		    vkMapMemory(device_, block.buffer.memory, 0, block.capacity, 0,
		                &block.mapped) != VK_SUCCESS) {
			return false;
		}
		++resource_stats_.staging_allocations;
		resource_stats_.staging_bytes += block.capacity;
		out = block;
	}

	// The block's contents are whatever the previous holder left, and the pool does
	// *not* publish them: a lock either has the level read back into it (the D3D8
	// preserve contract) or has it zeroed, and Prepare_Lock_Contents decides which
	// per level. Zeroing here instead would make "a lock loses what was there" the
	// contract, which is not what Windows does.
	resource_stats_.staging_live_bytes += out.capacity;
	++resource_stats_.staging_live_blocks;
	if (resource_stats_.staging_live_bytes > resource_stats_.staging_live_peak_bytes) {
		resource_stats_.staging_live_peak_bytes = resource_stats_.staging_live_bytes;
	}
	if (resource_stats_.staging_live_blocks > resource_stats_.staging_live_blocks_peak) {
		resource_stats_.staging_live_blocks_peak = resource_stats_.staging_live_blocks;
	}
	return true;
}

// Caller holds resource_mutex_.
void VulkanBackend::Release_Staging(StagingBlock& block) {
	if (!block.valid()) return;
	// Whoever takes the block next may write any of it, so no level may still be
	// believed to be in it: the release sites clear staging_synced.
	resource_stats_.staging_live_bytes -= block.capacity;
	--resource_stats_.staging_live_blocks;
	++resource_stats_.staging_pool_blocks;
	resource_stats_.staging_pool_bytes += block.capacity;
	staging_free_.push_back(block);
	block = StagingBlock{};
}

// Caller holds resource_mutex_. Gives the texture a staging block if it does not
// already hold one, i.e. if this is the first level of the chain being locked.
bool VulkanBackend::Ensure_Texture_Staging(TextureHandle* texture) {
	if (texture->staging.valid()) return true;
	VkDeviceSize needed = 0;
	for (const LockableLevel& l : texture->levels) {
		needed += static_cast<VkDeviceSize>(l.pitch) * l.height;
	}
	if (!Acquire_Staging(needed, texture->staging)) return false;
	texture->staging_mapped = texture->staging.mapped;
	// A block off the free list holds someone else's texels, so no level of this
	// texture is in it until something puts it there.
	for (LockableLevel& l : texture->levels) l.staging_synced = false;
	if (texture->retain_staging) ++resource_stats_.staging_retained_blocks;
	return true;
}

// Caller holds resource_mutex_.
bool VulkanBackend::Readback_Level(TextureHandle* texture, uint32_t level) {
	LockableLevel& l = texture->levels[level];
	const VkDeviceSize level_bytes = static_cast<VkDeviceSize>(l.pitch) * l.height;

	// On the no-view-swizzle path the image holds expanded B8G8R8A8, so the copy
	// lands in a scratch block and is contracted back into the caller's format. The
	// scratch block comes from the same pool and is returned immediately, so it costs
	// a concurrent block rather than a permanent one.
	StagingBlock scratch;
	VkBuffer target = texture->staging.buffer.buffer;
	VkDeviceSize target_offset = l.offset;
	const VkDeviceSize bgra_bytes = static_cast<VkDeviceSize>(l.width) * l.height * 4;
	if (texture->expand_on_unlock) {
		if (!Acquire_Staging(bgra_bytes, scratch)) return false;
		target = scratch.buffer.buffer;
		target_offset = 0;
	}

	VkCommandBuffer cmd = Begin_One_Shot();
	if (cmd == VK_NULL_HANDLE) {
		Release_Staging(scratch);
		return false;
	}
	Transition(cmd, texture->image.image, texture->layout,
	           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
	           texture->image.mip_levels);
	VkBufferImageCopy copy{};
	copy.bufferOffset = target_offset;
	copy.bufferRowLength = l.width;
	copy.bufferImageHeight = l.height;
	copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
	copy.imageExtent = {l.width, l.height, 1};
	vkCmdCopyImageToBuffer(cmd, texture->image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	                       target, 1, &copy);
	Transition(cmd, texture->image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
	           texture->image.mip_levels);
	const bool submitted = End_One_Shot(cmd);
	if (submitted) texture->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	if (submitted && texture->expand_on_unlock) {
		if (!Contract_From_Bgra8(texture->format,
		                        static_cast<const uint8_t*>(scratch.mapped), l.width,
		                        l.height,
		                        static_cast<uint8_t*>(texture->staging_mapped) + l.offset,
		                        l.pitch)) {
			std::fprintf(stderr,
			             "Readback_Level: no contraction for this format; contents "
			             "cannot be preserved\n");
			Release_Staging(scratch);
			return false;
		}
		++resource_stats_.cpu_expansions;
	}
	Release_Staging(scratch);
	if (!submitted) return false;

	l.staging_synced = true;
	++resource_stats_.readback_stalls;
	++resource_stats_.staging_preserve_readbacks;
	resource_stats_.staging_preserve_bytes += level_bytes;
	// The dirty bit is cleared once every level the host could read has been brought
	// back, which for the single-level surfaces the hazard is about is this one.
	bool all_synced = true;
	for (const LockableLevel& other : texture->levels) {
		if (!other.staging_synced) all_synced = false;
	}
	if (all_synced) texture->image.gpu_dirty = false;
	return true;
}

// Caller holds resource_mutex_.
bool VulkanBackend::Prepare_Lock_Contents(TextureHandle* texture, uint32_t level,
                                          uint32_t flags) {
	LockableLevel& l = texture->levels[level];
	// The two cases where skipping preservation is proven safe, and the only two:
	//   * D3DLOCK_DISCARD, which is D3D8's own statement that the caller overwrites
	//     everything and the previous contents may be thrown away;
	//   * a level nothing has ever written -- neither a previous unlock nor a GPU
	//     write funnel -- whose contents D3D8 leaves undefined.
	// Everything else preserves, because a whole-level lock without DISCARD is
	// exactly what W3DRadar, W3DShroud and Render2DSentenceClass do before drawing a
	// few pixels.
	const bool nothing_to_preserve = !l.ever_written && !texture->image.gpu_dirty;
	if ((flags & LOCK_DISCARD) == 0 && l.staging_synced && !texture->image.gpu_dirty) {
		// The block still holds this level and no GPU write has happened since, so the
		// host copy *is* the contents: no transfer, no submit, no wait. This is what
		// the dirty bit buys, and what keeps a retained block's locks free.
		return true;
	}
	if ((flags & LOCK_DISCARD) != 0 || nothing_to_preserve) {
		// Zeroed rather than left holding the previous lock's texels: the block is
		// pooled, and another resource's pixels are not "undefined contents".
		std::memset(static_cast<uint8_t*>(texture->staging_mapped) + l.offset, 0,
		            static_cast<size_t>(l.pitch) * l.height);
		++resource_stats_.staging_preserve_skips;
		return true;
	}
	return Readback_Level(texture, level);
}

TextureHandle* VulkanBackend::Create_Lockable_Texture(uint32_t width, uint32_t height,
                                                      TextureFormat format,
                                                      uint32_t mip_count) {
	std::lock_guard<std::mutex> guard(resource_mutex_);
	const uint32_t src_texel_bytes = Source_Texel_Bytes(format);
	if (width == 0 || height == 0 || mip_count == 0 || src_texel_bytes == 0) {
		std::fprintf(stderr,
		             "Create_Lockable_Texture: block-compressed formats are not served "
		             "by this path\n");
		return nullptr;
	}
	if (format == TextureFormat::P8) {
		// P8 needs the palette at upload time, and a lockable P8 texture could have
		// its palette changed between locks. No engine lock site uses P8, so rather
		// than guess a policy this path refuses it.
		std::fprintf(stderr, "Create_Lockable_Texture: P8 is not served by this path\n");
		return nullptr;
	}
	if (!Supports_Texture_Format(format)) return nullptr;

	const FormatPlan plan = Plan_For(format, view_swizzle_);
	auto* handle = new TextureHandle();
	handle->lockable = true;
	// So a write funnel holding only the image (a surface view of it, for instance)
	// can invalidate this texture's staging copy.
	handle->image.owner = handle;
	handle->format = format;
	handle->vk_format = plan.vk;
	handle->expand_on_unlock = plan.expand_to_bgra8;
	handle->src_texel_bytes = src_texel_bytes;
	handle->dst_texel_bytes = plan.block_bytes;
	handle->image.width = width;
	handle->image.height = height;
	handle->image.mip_levels = mip_count;

	VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = plan.vk;
	ici.extent = {width, height, 1};
	ici.mipLevels = mip_count;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	// TRANSFER_SRC as well as DST: D3DLOCK_READONLY has to copy the image back out.
	ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
	            VK_IMAGE_USAGE_SAMPLED_BIT;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (vkCreateImage(device_, &ici, nullptr, &handle->image.image) != VK_SUCCESS) {
		delete handle;
		return nullptr;
	}

	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(device_, handle->image.image, &req);
	uint32_t type = 0;
	if (!Find_Memory_Type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, type)) {
		delete handle;
		return nullptr;
	}
	VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	mai.allocationSize = req.size;
	mai.memoryTypeIndex = type;
	if (vkAllocateMemory(device_, &mai, nullptr, &handle->image.memory) != VK_SUCCESS ||
	    vkBindImageMemory(device_, handle->image.image, handle->image.memory, 0) != VK_SUCCESS) {
		delete handle;
		return nullptr;
	}

	// Staging covers the whole mip chain in one block, because a texture-loader lock
	// (class C4) locks every level at once and keeps all the pointers. The block
	// itself is not taken until the first Lock.
	VkDeviceSize staging_size = 0;
	VkDeviceSize upload_size = 0;
	handle->levels.resize(mip_count);
	for (uint32_t level = 0; level < mip_count; ++level) {
		const uint32_t level_width = width >> level ? width >> level : 1u;
		const uint32_t level_height = height >> level ? height >> level : 1u;
		LockableLevel& l = handle->levels[level];
		l.width = level_width;
		l.height = level_height;
		l.pitch = level_width * src_texel_bytes;
		l.offset = staging_size;
		staging_size += static_cast<VkDeviceSize>(l.pitch) * level_height;
		upload_size += static_cast<VkDeviceSize>(level_width) * level_height * 4;
	}

	// No view swizzle (MoltenVK): the caller still writes D3D8's L8/A8/A8L8/X8R8G8B8
	// layout, so Unlock has to expand into a *second* host-visible block that the
	// image is actually copied from. That block is taken from the same pool for the
	// duration of the unlock's copy, so it costs a concurrent block rather than a
	// second permanent allocation, and it still adds a CPU pass over the rectangle.
	handle->upload_size = upload_size;
	handle->retain_staging = staging_retain_;

	VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	vci.image = handle->image.image;
	vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vci.format = plan.vk;
	vci.components = plan.swizzle;
	vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mip_count, 0, 1};
	if (vkCreateImageView(device_, &vci, nullptr, &handle->image.view) != VK_SUCCESS) {
		delete handle;
		return nullptr;
	}

	// D3D8 hands out a lockable texture whose contents are undefined; the image here
	// starts UNDEFINED and only gets a layout at the first unlock. Sampling it before
	// then is a Vulkan error where D3D8 merely gives garbage, so the whole image is
	// cleared once to make the two behave the same.
	VkCommandBuffer cmd = Begin_One_Shot();
	if (cmd == VK_NULL_HANDLE) {
		delete handle;
		return nullptr;
	}
	Transition(cmd, handle->image.image, VK_IMAGE_LAYOUT_UNDEFINED,
	           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, mip_count);
	const VkClearColorValue black{{0.0f, 0.0f, 0.0f, 0.0f}};
	const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, mip_count, 0, 1};
	vkCmdClearColorImage(cmd, handle->image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                     &black, 1, &all);
	Transition(cmd, handle->image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
	           mip_count);
	if (!End_One_Shot(cmd)) {
		delete handle;
		return nullptr;
	}
	handle->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	++resource_stats_.upload_submits;

	owned_textures_.push_back(handle);
	return handle;
}

bool VulkanBackend::Lock_Texture(TextureHandle* texture, uint32_t level,
                                 const LockRect* rect, uint32_t flags, LockedRect& out) {
	std::lock_guard<std::mutex> guard(resource_mutex_);
	if (texture == nullptr || !texture->lockable || level >= texture->levels.size()) {
		return false;
	}
	LockableLevel& l = texture->levels[level];
	if (l.locked) return false; // D3D8 does not allow a nested lock of one level

	LockRect whole{0, 0, l.width, l.height};
	const LockRect r = rect != nullptr ? *rect : whole;
	if (r.right > l.width || r.bottom > l.height || r.left >= r.right || r.top >= r.bottom) {
		return false;
	}

	// The staging block arrives here rather than at creation: an unlocked resource
	// holds no host-visible memory at all.
	const bool had_staging = texture->staging.valid();
	if (!Ensure_Texture_Staging(texture)) return false;

	// D3D8's Lock without D3DLOCK_DISCARD hands back the texels that were there, and
	// a read lock has to see what the GPU wrote. Both are the same question -- is the
	// level's data in the block? -- so both go through one path, and it transfers
	// only when the answer is no.
	const uint32_t stalls_before = resource_stats_.readback_stalls;
	if (!Prepare_Lock_Contents(texture, level, flags)) {
		if (!had_staging) {
			Release_Staging(texture->staging);
			texture->staging_mapped = nullptr;
		}
		return false;
	}
	if ((flags & LOCK_READONLY) != 0) {
		if (resource_stats_.readback_stalls != stalls_before) {
			++resource_stats_.dirty_reads;
		} else {
			// A read of a resource nothing has written since the host last saw it:
			// no copy, no submit, no fence wait.
			++resource_stats_.clean_reads;
		}
	}

	l.locked = true;
	l.lock_flags = flags;
	l.lock_rect = r;
	++texture->locked_levels;
	// The pointer is into the persistent mapping, offset to the rectangle's first
	// texel, exactly as D3D8 documents pBits for a sub-rect lock.
	out.bits = static_cast<uint8_t*>(texture->staging_mapped) + l.offset +
	           static_cast<size_t>(r.top) * l.pitch +
	           static_cast<size_t>(r.left) * texture->src_texel_bytes;
	out.pitch = l.pitch;
	return true;
}

bool VulkanBackend::Unlock_Texture(TextureHandle* texture, uint32_t level) {
	std::lock_guard<std::mutex> guard(resource_mutex_);
	if (texture == nullptr || !texture->lockable || level >= texture->levels.size()) {
		return false;
	}
	LockableLevel& l = texture->levels[level];
	if (!l.locked) return false;
	// Whether the block still holds the whole level, which decides whether the
	// upload can leave it that way. A GPU write during the lock clears it.
	const bool was_synced = l.staging_synced;
	l.locked = false;
	if (texture->locked_levels > 0) --texture->locked_levels;
	// Once no level of the chain is locked the block goes back to the pool, unless the
	// resource is retained: a class C7/C8 caller keeps reading `pBits` after Unlock,
	// which only a pinned block can serve.
	const bool release_after = texture->locked_levels == 0 && !texture->retain_staging;
	auto release_staging = [&]() {
		if (!release_after) return;
		// The block goes back to the pool, so what it holds stops being this
		// texture's business; the image remains the authoritative copy and the next
		// lock brings the level back from it.
		for (LockableLevel& other : texture->levels) other.staging_synced = false;
		Release_Staging(texture->staging);
		texture->staging_mapped = nullptr;
	};

	// A read-only lock uploads nothing. This is why the write-only/read-only
	// distinction matters: it is the difference between a copy and no copy.
	if ((l.lock_flags & LOCK_READONLY) != 0) {
		release_staging();
		return true;
	}

	const LockRect r = l.lock_rect;
	const uint32_t rect_width = r.right - r.left;
	const uint32_t rect_height = r.bottom - r.top;

	VkBuffer source = texture->staging.buffer.buffer;
	VkDeviceSize source_offset = l.offset + static_cast<VkDeviceSize>(r.top) * l.pitch +
	                             static_cast<VkDeviceSize>(r.left) * texture->src_texel_bytes;
	uint32_t row_length = l.pitch / texture->src_texel_bytes;

	StagingBlock upload_block;
	if (texture->expand_on_unlock) {
		// CPU channel expansion, row by row, into a pooled upload block. Only the
		// locked rectangle is expanded, so a partial-rect unlock does not cost a pass
		// over the whole level.
		if (!Acquire_Staging(texture->upload_size, upload_block)) {
			release_staging();
			return false;
		}
		VkDeviceSize level_upload_offset = 0;
		for (uint32_t i = 0; i < level; ++i) {
			level_upload_offset += static_cast<VkDeviceSize>(texture->levels[i].width) *
			                       texture->levels[i].height * 4;
		}
		auto* dst = static_cast<uint8_t*>(upload_block.mapped) + level_upload_offset;
		const auto* src = static_cast<const uint8_t*>(texture->staging_mapped) + l.offset;
		std::vector<uint8_t> row;
		for (uint32_t y = 0; y < rect_height; ++y) {
			const TextureMip mip{src + static_cast<size_t>(r.top + y) * l.pitch +
			                         static_cast<size_t>(r.left) * texture->src_texel_bytes,
			                     static_cast<size_t>(rect_width) * texture->src_texel_bytes,
			                     rect_width, 1};
			Expand_To_Bgra8(texture->format, mip, nullptr, row);
			std::memcpy(dst + (static_cast<size_t>(r.top + y) * l.width + r.left) * 4,
			            row.data(), row.size());
		}
		++resource_stats_.cpu_expansions;
		source = upload_block.buffer.buffer;
		source_offset = level_upload_offset +
		                (static_cast<VkDeviceSize>(r.top) * l.width + r.left) * 4;
		row_length = l.width;
	}

	VkCommandBuffer cmd = Begin_One_Shot();
	if (cmd == VK_NULL_HANDLE) {
		Release_Staging(upload_block);
		release_staging();
		return false;
	}
	Transition(cmd, texture->image.image, texture->layout,
	           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
	           texture->image.mip_levels);
	VkBufferImageCopy copy{};
	copy.bufferOffset = source_offset;
	copy.bufferRowLength = row_length;
	copy.bufferImageHeight = rect_height;
	copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
	copy.imageOffset = {static_cast<int32_t>(r.left), static_cast<int32_t>(r.top), 0};
	copy.imageExtent = {rect_width, rect_height, 1};
	vkCmdCopyBufferToImage(cmd, source, texture->image.image,
	                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
	Transition(cmd, texture->image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
	           texture->image.mip_levels);
	// End_One_Shot waits the queue idle, so by the time it returns the copy has read
	// every byte it needs and both blocks are safe to hand to the next lock.
	const bool submitted = End_One_Shot(cmd);
	Release_Staging(upload_block);
	if (submitted) {
		// The image now holds what the host wrote, so the two agree: this upload is a
		// GPU write that does *not* dirty the resource. It leaves the level synced
		// only if the block holds all of it -- a partial rect on a level that was not
		// synced leaves the rest of the block still foreign.
		l.ever_written = true;
		if (was_synced || (r.left == 0 && r.top == 0 && r.right == l.width &&
		                   r.bottom == l.height)) {
			l.staging_synced = true;
		}
	}
	release_staging();
	if (!submitted) return false;
	Note_Texture_Layout(texture, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	++resource_stats_.texture_upload_regions;
	++resource_stats_.upload_submits;
	return true;
}

VertexBufferHandle* VulkanBackend::Create_Dynamic_Vertex_Buffer(size_t bytes,
                                                                uint32_t fvf) {
	return Create_Lockable_Vertex_Buffer(bytes, fvf, true);
}

VertexBufferHandle* VulkanBackend::Create_Lockable_Vertex_Buffer(size_t bytes, uint32_t fvf,
                                                                 bool dynamic) {
	if (bytes == 0) return nullptr;
	auto* handle = new VertexBufferHandle();
	handle->fvf = fvf;
	if (fvf == 0 && untyped_vertex_buffers_) {
		handle->untyped = true;
		std::fprintf(stderr, "untyped vertex buffer: %zu bytes created with FVF 0\n", bytes);
	} else if (!Decode_Fvf(fvf, handle->layout)) {
		std::fprintf(stderr, "Decode_Fvf: unsupported FVF 0x%x\n", fvf);
		delete handle;
		return nullptr;
	}
	handle->dynamic = dynamic;
	handle->capacity = bytes;
	// D3DLOCK_DISCARD is "rename this buffer": the driver hands back memory the GPU
	// is not reading. Reproducing that needs more than one copy behind the handle,
	// one per frame that can be in flight, plus one being written.
	handle->region_count = dynamic ? kDynamicRingRegions : 1;
	handle->region_last_use.assign(handle->region_count, 0);
	const VkMemoryPropertyFlags host =
	    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	const VkDeviceSize total = handle->capacity * handle->region_count;
	if (!Allocate_Buffer(total, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, host, handle->buffer) ||
	    vkMapMemory(device_, handle->buffer.memory, 0, total, 0, &handle->mapped) !=
	        VK_SUCCESS) {
		delete handle;
		return nullptr;
	}
	if (dynamic) {
		++resource_stats_.dynamic_buffer_allocations;
		resource_stats_.dynamic_buffer_bytes += total;
	}
	owned_vbs_.push_back(handle);
	return handle;
}

bool VulkanBackend::Lock_Vertex_Buffer(VertexBufferHandle* vb, size_t offset, size_t size,
                                      uint32_t flags, void** out_bits) {
	if (vb == nullptr || out_bits == nullptr || vb->mapped == nullptr) return false;
	if (offset + size > vb->capacity) return false;

	if ((flags & LOCK_DISCARD) != 0) {
		vb->region = (vb->region + 1) % vb->region_count;
		if (vb->region_last_use[vb->region] > completed_frame_) {
			// The ring has wrapped onto bytes a submitted frame may still be reading.
			// D3D8's DISCARD promises this never blocks, so a real port has to grow
			// the ring rather than wait; the spike counts it and continues, and a
			// within-frame wrap is reported because there is no fence to wait on.
			++resource_stats_.ring_wrap_waits;
			if (vb->region_last_use[vb->region] < frame_counter_) {
				vkWaitForFences(device_, 1, &frame_fence_, VK_TRUE, UINT64_MAX);
				completed_frame_ = frame_counter_ - 1;
			}
		}
		++resource_stats_.ring_discards;
	} else if ((flags & LOCK_NOOVERWRITE) != 0) {
		// Append: the earlier bytes of this region stay valid, because draws already
		// recorded against them have not been submitted or have not finished.
		++resource_stats_.ring_appends;
	}
	vb->bind_offset = vb->capacity * vb->region;
	resource_stats_.ring_bytes += size;
	*out_bits = static_cast<uint8_t*>(vb->mapped) + vb->bind_offset + offset;
	return true;
}

bool VulkanBackend::Unlock_Vertex_Buffer(VertexBufferHandle* vb) {
	// Host-coherent memory, so there is nothing to flush and nothing to copy: this
	// is the one D3D8 lock class that maps onto Vulkan for free.
	return vb != nullptr && vb->mapped != nullptr;
}

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------

void VulkanBackend::Set_DX8_Render_State(D3DRENDERSTATETYPE state, uint32_t value) {
	if (static_cast<uint32_t>(state) < D3DRS_MAX) render_states_[state] = value;
}

void VulkanBackend::Set_DX8_Texture_Stage_State(uint32_t stage,
                                                D3DTEXTURESTAGESTATETYPE state,
                                                uint32_t value) {
	if (stage >= kMaxTextureStages) return;
	PerStage& s = stages_[stage];
	switch (state) {
	case D3DTSS_COLOROP: s.color_op = value; break;
	case D3DTSS_COLORARG1: s.color_arg1 = value; break;
	case D3DTSS_COLORARG2: s.color_arg2 = value; break;
	case D3DTSS_COLORARG0: s.color_arg0 = value; break;
	case D3DTSS_ALPHAOP: s.alpha_op = value; break;
	case D3DTSS_ALPHAARG1: s.alpha_arg1 = value; break;
	case D3DTSS_ALPHAARG2: s.alpha_arg2 = value; break;
	case D3DTSS_ALPHAARG0: s.alpha_arg0 = value; break;
	case D3DTSS_TEXCOORDINDEX: s.texcoord_index = value; break;
	case D3DTSS_TEXTURETRANSFORMFLAGS: s.transform_flags = value; break;
	case D3DTSS_RESULTARG: s.result_arg = value; break;
	case D3DTSS_MINFILTER: s.min_filter = value; break;
	case D3DTSS_MAGFILTER: s.mag_filter = value; break;
	case D3DTSS_MIPFILTER: s.mip_filter = value; break;
	case D3DTSS_ADDRESSU: s.address_u = value; break;
	case D3DTSS_ADDRESSV: s.address_v = value; break;
	case D3DTSS_ADDRESSW: s.address_w = value; break;
	case D3DTSS_BORDERCOLOR: s.border_color = value; break;
	case D3DTSS_MAXANISOTROPY: s.max_anisotropy = value; break;
	case D3DTSS_BUMPENVMAT00: s.bump_matrix[0] = Dword_To_Float(value); break;
	case D3DTSS_BUMPENVMAT01: s.bump_matrix[1] = Dword_To_Float(value); break;
	case D3DTSS_BUMPENVMAT10: s.bump_matrix[2] = Dword_To_Float(value); break;
	case D3DTSS_BUMPENVMAT11: s.bump_matrix[3] = Dword_To_Float(value); break;
	case D3DTSS_BUMPENVLSCALE: s.bump_luminance_scale = Dword_To_Float(value); break;
	case D3DTSS_BUMPENVLOFFSET: s.bump_luminance_offset = Dword_To_Float(value); break;
	default: break; // the 9 D3DTSS_* the engine never writes
	}
}

void VulkanBackend::Set_Transform(D3DTRANSFORMSTATETYPE transform, const Matrix4x4& m) {
	switch (transform) {
	case D3DTS_WORLD: world_ = m; break;
	case D3DTS_VIEW: view_ = m; break;
	case D3DTS_PROJECTION: projection_ = m; break;
	case D3DTS_TEXTURE0:
	case D3DTS_TEXTURE1:
	case D3DTS_TEXTURE2:
	case D3DTS_TEXTURE3:
		texture_transform_[transform - D3DTS_TEXTURE0] = m;
		break;
	default: break;
	}
}

void VulkanBackend::Get_Transform(D3DTRANSFORMSTATETYPE transform, Matrix4x4& out) const {
	switch (transform) {
	case D3DTS_WORLD: out = world_; break;
	case D3DTS_VIEW: out = view_; break;
	case D3DTS_PROJECTION: out = projection_; break;
	case D3DTS_TEXTURE0:
	case D3DTS_TEXTURE1:
	case D3DTS_TEXTURE2:
	case D3DTS_TEXTURE3:
		out = texture_transform_[transform - D3DTS_TEXTURE0];
		break;
	default: out = Matrix4x4::Identity(); break;
	}
}

void VulkanBackend::Set_Texture(uint32_t stage, TextureHandle* texture) {
	if (stage < kMaxTextureStages) bound_textures_[stage] = texture;
}

void VulkanBackend::Set_Light(uint32_t index, const LightState* light) {
	if (index >= kMaxLights) return;
	// A null light is LightEnable(index, FALSE); type 0 is the shader's "off".
	lights_[index] = light != nullptr ? *light : LightState{};
}

void VulkanBackend::Set_Material(const MaterialState& material) { material_ = material; }

void VulkanBackend::Set_Scissor(bool enable, int32_t x, int32_t y, int32_t width,
                                int32_t height) {
	scissor_enabled_ = enable;
	scissor_.offset = {x, y};
	scissor_.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
}

void VulkanBackend::Set_Viewport(const ViewportRect& viewport) {
	// A zero-sized viewport is what D3D8 reports before the first SetViewport; it is
	// also invalid in Vulkan, so it falls back to the whole target.
	viewport_ = viewport;
	if (viewport_.width == 0 || viewport_.height == 0) {
		viewport_ = ViewportRect{0, 0, width_, height_, viewport.min_z, viewport.max_z};
	}
}

void VulkanBackend::Set_Vertex_Buffer(VertexBufferHandle* vb, uint32_t stream, uint32_t stride) {
	if (stream != 0) return;
	bound_vb_ = vb;
	bound_vb_stride_ = stride;
}

void VulkanBackend::Set_Fixed_Function_Fvf(uint32_t fvf) {
	fixed_function_fvf_ = fvf;
}

bool VulkanBackend::Apply_Stream_Stride(VertexLayout& layout, uint32_t stride,
                                        const char* what) {
	// D3D8 reads the buffer at SetStreamSource's stride when one is given; a stride
	// smaller than the layout's own vertex cannot hold the vertex and is refused rather
	// than read past.
	if (stride != 0 && stride < layout.stride) {
		if (frame_untyped_draws_dropped_ == 0) {
			std::fprintf(stderr,
			             "untyped vertex buffer bound at stride %u, smaller than %s's %u bytes: "
			             "the frame will be missing geometry\n",
			             stride, what, layout.stride);
		}
		return false;
	}
	if (stride > layout.stride) layout.stride = stride;
	return true;
}

bool VulkanBackend::Resolve_Draw_Layout(const VertexBufferHandle& vb, uint32_t& out_fvf,
                                        uint32_t& out_declaration,
                                        const VertexLayout*& out_layout) {
	out_declaration = 0;
	if (!vb.untyped) {
		out_fvf = vb.fvf;
		out_layout = &vb.layout;
		return true;
	}
	const uint32_t stride = bound_vb_stride_;

	// A bound program reads the buffer with its declaration's layout: that is what
	// SetVertexShader(handle) means for a stream whose buffer has no FVF.
	const ShaderProgram* vs =
	    bound_vertex_shader_ != kNullShader ? Find_Shader(bound_vertex_shader_) : nullptr;
	if (vs != nullptr && vs->declared_layout_hash != 0 && vertex_declarations_) {
		const uint64_t cache_key = (1ull << 63) |
		                           (static_cast<uint64_t>(vs->declared_layout_hash) << 32) | stride;
		auto it = untyped_layouts_.find(cache_key);
		if (it == untyped_layouts_.end()) {
			VertexLayout layout = vs->declared_layout;
			if (!Apply_Stream_Stride(layout, stride, "the vertex declaration")) return false;
			it = untyped_layouts_.emplace(cache_key, layout).first;
			std::fprintf(stderr,
			             "untyped vertex buffer layout: declaration %08x stride %u "
			             "(%u bytes/vertex)\n",
			             vs->declared_layout_hash, stride, layout.stride);
		}
		out_fvf = 0;
		out_declaration = vs->declared_layout_hash;
		out_layout = &it->second;
		return true;
	}

	const uint32_t fvf = fixed_function_fvf_;
	const uint64_t cache_key = (static_cast<uint64_t>(fvf) << 32) | stride;
	auto it = untyped_layouts_.find(cache_key);
	if (it == untyped_layouts_.end()) {
		VertexLayout layout;
		if (!Decode_Fvf(fvf, layout)) {
			if (frame_untyped_draws_dropped_ == 0) {
				std::fprintf(stderr,
				             "untyped vertex buffer drawn with no decodable fixed-function FVF "
				             "bound (0x%x)%s: the frame will be missing geometry\n",
				             fvf, vs != nullptr ? " and a program whose declaration is unusable"
				                                : "");
			}
			return false;
		}
		char what[32];
		std::snprintf(what, sizeof(what), "FVF 0x%x", fvf);
		if (!Apply_Stream_Stride(layout, stride, what)) return false;
		it = untyped_layouts_.emplace(cache_key, layout).first;
		// Once per distinct layout: the runtime enumeration the source enumeration in
		// docs/porting/untyped-vertex-buffers.md is checked against.
		std::fprintf(stderr, "untyped vertex buffer layout: FVF 0x%x stride %u (%u bytes/vertex)\n",
		             fvf, stride, layout.stride);
	}
	out_fvf = fvf;
	out_layout = &it->second;
	return true;
}

void VulkanBackend::Set_Index_Buffer(IndexBufferHandle* ib, uint32_t index_base_offset) {
	bound_ib_ = ib;
	index_base_offset_ = index_base_offset;
}

VkSampler VulkanBackend::Get_Or_Create_Sampler(const SamplerKey& key) {
	auto it = samplers_.find(key);
	if (it != samplers_.end()) return it->second;

	VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	sci.magFilter = To_Vk_Filter(key.mag_filter);
	sci.minFilter = To_Vk_Filter(key.min_filter);
	sci.mipmapMode = To_Vk_Mipmap_Mode(key.mip_filter);
	sci.addressModeU = To_Vk_Address_Mode(key.address_u);
	sci.addressModeV = To_Vk_Address_Mode(key.address_v);
	sci.addressModeW = To_Vk_Address_Mode(key.address_w);
	sci.maxLod = VK_LOD_CLAMP_NONE;
	// D3DTSS_MAXANISOTROPY only means anything with an ANISOTROPIC filter, and only
	// up to what the device allows -- D3D8 clamps to D3DCAPS8::MaxAnisotropy the
	// same way. A device without samplerAnisotropy leaves this off, which is the
	// D3D8 behaviour when the cap is 1.
	const bool anisotropic =
	    key.min_filter == D3DTEXF_ANISOTROPIC || key.mag_filter == D3DTEXF_ANISOTROPIC;
	if (anisotropic && max_anisotropy_ > 1.0f && key.max_anisotropy > 1) {
		sci.anisotropyEnable = VK_TRUE;
		sci.maxAnisotropy =
		    std::min(static_cast<float>(key.max_anisotropy), max_anisotropy_);
	}
	// D3DTSS_BORDERCOLOR is an arbitrary D3DCOLOR; core Vulkan only has the four
	// fixed border colours, so anything that is not one of them is rounded to the
	// nearest of transparent-black, opaque-black and opaque-white. A full port needs
	// VK_EXT_custom_border_color, which MoltenVK does not expose.
	if (key.address_u == D3DTADDRESS_BORDER || key.address_v == D3DTADDRESS_BORDER ||
	    key.address_w == D3DTADDRESS_BORDER) {
		float rgba[4];
		Unpack_D3dcolor(key.border_color, rgba);
		const float luminance = (rgba[0] + rgba[1] + rgba[2]) / 3.0f;
		sci.borderColor = rgba[3] < 0.5f ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK
		                                 : (luminance >= 0.5f ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
		                                                      : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK);
	}
	VkSampler sampler = VK_NULL_HANDLE;
	if (vkCreateSampler(device_, &sci, nullptr, &sampler) != VK_SUCCESS) return VK_NULL_HANDLE;
	samplers_[key] = sampler;
	return sampler;
}

VkPipeline VulkanBackend::Get_Or_Create_Pipeline(const PipelineKey& key,
                                                 const VertexLayout& layout) {
	const uint64_t hash = Hash_Pipeline_Key(key);
	auto it = pipelines_.find(hash);
	if (it != pipelines_.end()) return it->second;

	VkPipelineShaderStageCreateInfo stages[2]{};
	stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vert_module_;
	stages[0].pName = "main";
	stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = frag_module_;
	stages[1].pName = "main";

	// Binding 0 is the engine's vertex buffer; binding 1 is the constant dummy that
	// backs shader inputs the FVF does not supply. The dummy binding steps per
	// instance rather than per vertex so that every vertex of the single instance
	// reads element 0. A stride of zero would express the same thing in core Vulkan
	// but Metal, and so the portability subset, forbids a stride smaller than the
	// attributes it feeds.
	VkVertexInputBindingDescription bindings[2]{};
	bindings[0] = {0, layout.stride, VK_VERTEX_INPUT_RATE_VERTEX};
	bindings[1] = {1, sizeof(DummyVertex), VK_VERTEX_INPUT_RATE_INSTANCE};

	VkVertexInputAttributeDescription attributes[VA_COUNT]{};
	uint32_t attribute_count = 0;
	static const VkFormat kDummyFormats[VA_COUNT] = {
	    VK_FORMAT_R32G32B32_SFLOAT,    // VA_POSITION (unused: always supplied)
	    VK_FORMAT_R32G32B32_SFLOAT,    // VA_BLENDWEIGHT
	    VK_FORMAT_R8G8B8A8_UINT,       // VA_BLENDINDICES
	    VK_FORMAT_R32G32B32_SFLOAT,    // VA_NORMAL
	    VK_FORMAT_B8G8R8A8_UNORM,      // VA_DIFFUSE
	    VK_FORMAT_B8G8R8A8_UNORM,      // VA_SPECULAR
	    VK_FORMAT_R32G32B32A32_SFLOAT, // VA_TEXCOORD0
	    VK_FORMAT_R32G32B32A32_SFLOAT, // VA_TEXCOORD1
	    VK_FORMAT_R32G32B32A32_SFLOAT, // VA_TEXCOORD2
	    VK_FORMAT_R32G32B32A32_SFLOAT, // VA_TEXCOORD3
	};
	for (uint32_t loc = 0; loc < VA_COUNT; ++loc) {
		if (layout.supplies[loc]) {
			for (uint32_t i = 0; i < layout.attribute_count; ++i) {
				if (layout.attributes[i].location == loc) {
					attributes[attribute_count++] = layout.attributes[i];
					break;
				}
			}
		} else {
			attributes[attribute_count++] = {loc, 1, kDummyFormats[loc], kDummyOffsets[loc]};
		}
	}

	VkPipelineVertexInputStateCreateInfo vertex_input{
	    VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	vertex_input.vertexBindingDescriptionCount = 2;
	vertex_input.pVertexBindingDescriptions = bindings;
	vertex_input.vertexAttributeDescriptionCount = attribute_count;
	vertex_input.pVertexAttributeDescriptions = attributes;

	VkPipelineInputAssemblyStateCreateInfo input_assembly{
	    VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
	input_assembly.topology = To_Vk_Topology(key.topology);
	// Metal cannot turn primitive restart off, and MoltenVK reports
	// VK_ERROR_FEATURE_NOT_PRESENT for a strip or fan pipeline that asks it to. D3D8 has
	// no restart index at all, so restart is requested exactly where Metal forces it and
	// nowhere else. The cost is that 0xffff inside a strip -- a legal D3D8 index --
	// restarts it; the engine's strips come from index buffers orders of magnitude
	// smaller than 65536 vertices.
	switch (input_assembly.topology) {
	case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
	case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
	case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:
		input_assembly.primitiveRestartEnable = VK_TRUE;
		break;
	default:
		input_assembly.primitiveRestartEnable = VK_FALSE;
		break;
	}

	// Both are dynamic state, reset per draw in Prepare_Draw(); these only have to be legal.
	// In pixels, like everything an attachment is measured in.
	VkViewport viewport{0.0f, 0.0f, static_cast<float>(device_width_),
	                    static_cast<float>(device_height_), 0.0f, 1.0f};
	VkRect2D scissor{{0, 0}, {device_width_, device_height_}};
	VkPipelineViewportStateCreateInfo viewport_state{
	    VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
	viewport_state.viewportCount = 1;
	viewport_state.pViewports = &viewport;
	viewport_state.scissorCount = 1;
	viewport_state.pScissors = &scissor;

	VkPipelineRasterizationStateCreateInfo raster{
	    VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
	raster.polygonMode = To_Vk_Polygon_Mode(key.fill_mode);
	raster.cullMode = To_Vk_Cull_Mode(key.cull_mode);
	raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	raster.lineWidth = 1.0f;
	// Only whether bias is on is baked in; the amount is dynamic state, so the
	// engine's five distinct ZBIAS values cost one pipeline, not five.
	raster.depthBiasEnable = key.depth_bias_enable ? VK_TRUE : VK_FALSE;

	VkPipelineMultisampleStateCreateInfo multisample{
	    VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineDepthStencilStateCreateInfo depth{
	    VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
	depth.depthTestEnable = key.z_enable ? VK_TRUE : VK_FALSE;
	depth.depthWriteEnable = key.z_write_enable ? VK_TRUE : VK_FALSE;
	depth.depthCompareOp = To_Vk_Compare_Op(key.z_func);
	depth.stencilTestEnable = key.stencil_enable ? VK_TRUE : VK_FALSE;
	// D3D8 has one stencil state for both faces (there is no two-sided stencil
	// before D3D9), so front and back get the same VkStencilOpState.
	VkStencilOpState stencil{};
	stencil.failOp = To_Vk_Stencil_Op(key.stencil_fail);
	stencil.passOp = To_Vk_Stencil_Op(key.stencil_pass);
	stencil.depthFailOp = To_Vk_Stencil_Op(key.stencil_zfail);
	stencil.compareOp = To_Vk_Compare_Op(key.stencil_func);
	stencil.compareMask = key.stencil_mask;
	stencil.writeMask = key.stencil_write_mask;
	stencil.reference = key.stencil_ref;
	depth.front = stencil;
	depth.back = stencil;

	VkPipelineColorBlendAttachmentState blend{};
	blend.blendEnable = key.alpha_blend_enable ? VK_TRUE : VK_FALSE;
	blend.srcColorBlendFactor = To_Vk_Blend_Factor(key.src_blend);
	blend.dstColorBlendFactor = To_Vk_Blend_Factor(key.dest_blend);
	blend.colorBlendOp = To_Vk_Blend_Op(key.blend_op);
	blend.srcAlphaBlendFactor = To_Vk_Blend_Factor(key.src_blend);
	blend.dstAlphaBlendFactor = To_Vk_Blend_Factor(key.dest_blend);
	// D3D8 has one D3DRS_BLENDOP for colour and alpha alike.
	blend.alphaBlendOp = To_Vk_Blend_Op(key.blend_op);
	blend.colorWriteMask = To_Vk_Color_Write_Mask(key.color_write_enable);

	VkPipelineColorBlendStateCreateInfo blend_state{
	    VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
	blend_state.attachmentCount = 1;
	blend_state.pAttachments = &blend;

	// Scissor, viewport and depth-bias amount are dynamic: D3D8 changes all three
	// without any notion of pipeline identity, and baking them in would multiply the
	// cache by every viewport rectangle the engine sets.
	const VkDynamicState kDynamic[] = {VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS,
	                                   VK_DYNAMIC_STATE_VIEWPORT};
	VkPipelineDynamicStateCreateInfo dynamic_state{
	    VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
	dynamic_state.dynamicStateCount = 3;
	dynamic_state.pDynamicStates = kDynamic;

	VkGraphicsPipelineCreateInfo gpci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
	gpci.stageCount = 2;
	gpci.pStages = stages;
	gpci.pVertexInputState = &vertex_input;
	gpci.pInputAssemblyState = &input_assembly;
	gpci.pViewportState = &viewport_state;
	gpci.pRasterizationState = &raster;
	gpci.pMultisampleState = &multisample;
	gpci.pDepthStencilState = &depth;
	gpci.pColorBlendState = &blend_state;
	gpci.pDynamicState = &dynamic_state;
	gpci.layout = pipeline_layout_;
	// Compatible with, not identical to, the pass the draw runs in: the load op does
	// not affect render-pass compatibility, the attachment set does.
	VkRenderPass compatible = Get_Or_Create_Render_Pass(key.has_depth_attachment != 0, false);
	if (compatible == VK_NULL_HANDLE) return VK_NULL_HANDLE;
	gpci.renderPass = compatible;
	gpci.subpass = 0;

	VkPipeline pipeline = VK_NULL_HANDLE;
	if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipeline) != VK_SUCCESS) {
		std::fprintf(stderr, "vkCreateGraphicsPipelines failed\n");
		return VK_NULL_HANDLE;
	}
	pipelines_[hash] = pipeline;
	return pipeline;
}

// ---------------------------------------------------------------------------
// frame
// ---------------------------------------------------------------------------

void VulkanBackend::Begin_Scene() {
	vkWaitForFences(device_, 1, &frame_fence_, VK_TRUE, UINT64_MAX);
	vkResetFences(device_, 1, &frame_fence_);
	// Everything submitted before this point has finished, which is what makes a
	// dynamic-buffer region safe to rename.
	completed_frame_ = frame_counter_;
	++frame_counter_;
	vkResetCommandBuffer(frame_cmd_, 0);
	// The UP scratch bytes of the previous frame have been consumed by the draws that
	// have now finished, so the bump allocator restarts.
	up_offset_ = 0;

	VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(frame_cmd_, &bi);

	// A new frame's targets start undefined: D3D8's back buffer is not guaranteed to
	// survive a Present either, and the engine clears.
	default_color_surface_.written_this_frame = false;
	for (SurfaceHandle* surface : owned_surfaces_) surface->written_this_frame = false;

	draw_index_ = 0;
	frame_draws_requested_ = 0;
	frame_draws_dropped_ = 0;
	frame_untyped_draws_issued_ = 0;
	frame_untyped_draws_dropped_ = 0;
	in_scene_ = true;
	if (!Begin_Current_Pass()) {
		in_scene_ = false;
		vkEndCommandBuffer(frame_cmd_);
	}
}

void VulkanBackend::Clear(bool clear_color, bool clear_z_stencil, float r, float g,
                          float b, float dest_alpha, float z, uint32_t stencil) {
	if (!in_scene_) return;
	// vkCmdClearAttachments, not a render-pass load op: D3D8's Clear() is a command
	// the engine issues mid-frame with per-call flags, and it does so more than once
	// per frame in the render-to-texture paths.
	VkClearAttachment clears[2]{};
	uint32_t count = 0;
	if (clear_color) {
		clears[count].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		clears[count].colorAttachment = 0;
		clears[count].clearValue.color = {{r, g, b, dest_alpha}};
		++count;
	}
	if (clear_z_stencil) {
		// D3D8's D3DCLEAR_ZBUFFER and D3DCLEAR_STENCIL arrive together in the
		// engine's Clear(); omitting the stencil aspect leaves the stencil buffer
		// undefined, which shows up as a shadow-volume pass that covers everything.
		clears[count].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
		clears[count].clearValue.depthStencil = {z, stencil};
		++count;
	}
	if (count == 0) return;
	// The current target's extent in pixels, which after a SetRenderTarget is not the
	// device's. D3D8's Clear() has no rectangle here, so the whole target is cleared and
	// there is nothing in point space to convert.
	VkClearRect rect{{{0, 0}, {device_target_width_, device_target_height_}}, 0, 1};
	vkCmdClearAttachments(frame_cmd_, count, clears, 1, &rect);
	// Write funnel 2.
	if (clear_color) Mark_Gpu_Write(current_color_);
	if (clear_z_stencil) Mark_Gpu_Write(current_depth_);
}

VkRect2D VulkanBackend::Clamp_Scissor(const VkRect2D& rect) const {
	// D3D8's SetScissors rectangle is in the same space as the viewport but is not
	// required to lie inside the render target; Vulkan requires that it does.
	//
	// The rectangle arrives in points, like the viewport, and is scaled to the target's
	// pixels here: at scale 2 a 400x300 UI clip has to keep clipping the same half of the
	// panel, not its top-left quarter.
	const float scale = Surface_Render_Scale(current_color_);
	const auto scale_coord = [scale](int64_t v) -> int64_t {
		return static_cast<int64_t>(std::llround(static_cast<double>(v) * scale));
	};
	int32_t x0 = rect.offset.x < 0 ? 0 : static_cast<int32_t>(scale_coord(rect.offset.x));
	int32_t y0 = rect.offset.y < 0 ? 0 : static_cast<int32_t>(scale_coord(rect.offset.y));
	int64_t x1 = scale_coord(static_cast<int64_t>(rect.offset.x) + rect.extent.width);
	int64_t y1 = scale_coord(static_cast<int64_t>(rect.offset.y) + rect.extent.height);
	if (x1 > device_target_width_) x1 = device_target_width_;
	if (y1 > device_target_height_) y1 = device_target_height_;
	if (x1 < x0) x1 = x0;
	if (y1 < y0) y1 = y0;
	return VkRect2D{{x0, y0},
	                {static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(y1 - y0)}};
}

void VulkanBackend::Fill_Draw_Uniforms(uint32_t primitive_type, const VertexLayout& layout,
                                       DrawUniforms& out) const {
	// D3D8 lights are in world space and fog and lighting are evaluated in camera
	// space, so the shader needs world_view and view separately, not just the
	// composite.
	const Matrix4x4 world_view = Multiply(world_, view_);
	Store_Matrix(Multiply(world_view, projection_), out.wvp, true);
	Store_Matrix(world_, out.world);
	Store_Matrix(world_view, out.world_view);
	Store_Matrix(view_, out.view);
	for (uint32_t i = 0; i < kMaxTexCoordSets; ++i)
		Store_Matrix(texture_transform_[i], out.tex_matrix[i]);

	for (uint32_t i = 0; i < kMaxTextureStages; ++i) {
		const PerStage& s = stages_[i];
		out.stage_color[i][0] = static_cast<int32_t>(s.color_op);
		out.stage_color[i][1] = static_cast<int32_t>(s.color_arg1);
		out.stage_color[i][2] = static_cast<int32_t>(s.color_arg2);
		out.stage_color[i][3] = static_cast<int32_t>(s.color_arg0);
		out.stage_alpha[i][0] = static_cast<int32_t>(s.alpha_op);
		out.stage_alpha[i][1] = static_cast<int32_t>(s.alpha_arg1);
		out.stage_alpha[i][2] = static_cast<int32_t>(s.alpha_arg2);
		out.stage_alpha[i][3] = static_cast<int32_t>(s.alpha_arg0);
		out.stage_misc[i][0] = static_cast<int32_t>(s.texcoord_index);
		out.stage_misc[i][1] = static_cast<int32_t>(s.transform_flags);
		out.stage_misc[i][2] = bound_textures_[i] != nullptr ? 1 : 0;
		out.stage_misc[i][3] = static_cast<int32_t>(s.result_arg);
		for (int j = 0; j < 4; ++j) out.stage_bump[i][j] = s.bump_matrix[j];
		out.stage_bump_lum[i][0] = s.bump_luminance_scale;
		out.stage_bump_lum[i][1] = s.bump_luminance_offset;
	}

	for (uint32_t i = 0; i < kMaxLights; ++i) {
		const LightState& l = lights_[i];
		for (int j = 0; j < 4; ++j) {
			out.light_diffuse[i][j] = l.diffuse[j];
			out.light_specular[i][j] = l.specular[j];
			out.light_ambient[i][j] = l.ambient[j];
		}
		for (int j = 0; j < 3; ++j) {
			out.light_position[i][j] = l.position[j];
			out.light_direction[i][j] = l.direction[j];
		}
		out.light_position[i][3] = static_cast<float>(l.type);
		out.light_direction[i][3] = l.range;
		out.light_attenuation[i][0] = l.attenuation0;
		out.light_attenuation[i][1] = l.attenuation1;
		out.light_attenuation[i][2] = l.attenuation2;
		out.light_attenuation[i][3] = l.falloff;
		// D3DLIGHT8's Theta and Phi are full cone angles; the falloff is computed
		// against the cosines of the half-angles.
		out.light_spot[i][0] = std::cos(l.theta * 0.5f);
		out.light_spot[i][1] = std::cos(l.phi * 0.5f);
	}

	for (int j = 0; j < 4; ++j) {
		out.material_diffuse[j] = material_.diffuse[j];
		out.material_ambient[j] = material_.ambient[j];
		out.material_specular[j] = material_.specular[j];
		out.material_emissive[j] = material_.emissive[j];
	}
	out.material_power[0] = material_.power;

	Unpack_D3dcolor(render_states_[D3DRS_AMBIENT], out.global_ambient);
	Unpack_D3dcolor(render_states_[D3DRS_TEXTUREFACTOR], out.tfactor);
	Unpack_D3dcolor(render_states_[D3DRS_FOGCOLOR], out.fog_color);
	out.fog_params[0] = Dword_To_Float(render_states_[D3DRS_FOGSTART]);
	out.fog_params[1] = Dword_To_Float(render_states_[D3DRS_FOGEND]);
	out.fog_params[2] = Dword_To_Float(render_states_[D3DRS_FOGDENSITY]);

	out.misc[0] = (render_states_[D3DRS_ALPHAREF] & 0xff) / 255.0f;
	out.misc[1] = static_cast<float>(target_width_);
	out.misc[2] = static_cast<float>(target_height_);

	out.flags[0] = static_cast<int32_t>(render_states_[D3DRS_ALPHATESTENABLE]);
	out.flags[1] = static_cast<int32_t>(render_states_[D3DRS_ALPHAFUNC]);
	out.flags[2] = static_cast<int32_t>(render_states_[D3DRS_LIGHTING]);
	out.flags[3] = static_cast<int32_t>(render_states_[D3DRS_FOGENABLE]);

	// Pretransformed-ness is a property of the vertices being drawn, not of whatever
	// happens to be bound to stream 0: a DrawPrimitiveUP draw has no bound buffer at
	// all.
	out.flags2[0] = layout.pretransformed ? 1 : 0;
	out.flags2[1] = static_cast<int32_t>(render_states_[D3DRS_FOGVERTEXMODE]);
	out.flags2[2] = static_cast<int32_t>(render_states_[D3DRS_FOGTABLEMODE]);
	out.flags2[3] = static_cast<int32_t>(render_states_[D3DRS_SPECULARENABLE]);

	out.sources[0] = static_cast<int32_t>(render_states_[D3DRS_DIFFUSEMATERIALSOURCE]);
	out.sources[1] = static_cast<int32_t>(render_states_[D3DRS_SPECULARMATERIALSOURCE]);
	out.sources[2] = static_cast<int32_t>(render_states_[D3DRS_AMBIENTMATERIALSOURCE]);
	out.sources[3] = static_cast<int32_t>(render_states_[D3DRS_EMISSIVEMATERIALSOURCE]);

	out.flags3[0] = static_cast<int32_t>(render_states_[D3DRS_COLORVERTEX]);
	out.flags3[1] = static_cast<int32_t>(render_states_[D3DRS_NORMALIZENORMALS]);
	out.flags3[2] = static_cast<int32_t>(render_states_[D3DRS_LOCALVIEWER]);
	out.flags3[3] = static_cast<int32_t>(render_states_[D3DRS_RANGEFOGENABLE]);

	// Point sprites. D3D8's size states are floats in DWORD clothing, and the size
	// is clamped to [POINTSIZE_MIN, POINTSIZE_MAX] and then to what the device can
	// rasterise, because a point wider than pointSizeRange is undefined in Vulkan
	// rather than clamped.
	out.point_size[0] = Dword_To_Float(render_states_[D3DRS_POINTSIZE]);
	out.point_size[1] = Dword_To_Float(render_states_[D3DRS_POINTSIZE_MIN]);
	out.point_size[2] = std::min(Dword_To_Float(render_states_[D3DRS_POINTSIZE_MAX]),
	                             max_point_size_);
	// D3DRS_POINTSPRITEENABLE only means anything for a point primitive, and the
	// shader may only read gl_PointCoord when one is being rasterised, so the two
	// conditions are folded together here.
	out.point_size[3] = (render_states_[D3DRS_POINTSPRITEENABLE] != 0 &&
	                     primitive_type == D3DPT_POINTLIST)
	                        ? 1.0f
	                        : 0.0f;
	out.point_scale[0] = Dword_To_Float(render_states_[D3DRS_POINTSCALE_A]);
	out.point_scale[1] = Dword_To_Float(render_states_[D3DRS_POINTSCALE_B]);
	out.point_scale[2] = Dword_To_Float(render_states_[D3DRS_POINTSCALE_C]);
	out.point_scale[3] = render_states_[D3DRS_POINTSCALEENABLE] != 0 ? 1.0f : 0.0f;

	// --- programmable shaders -------------------------------------------------
	const ShaderProgram* ps = nullptr;
	const ShaderProgram* vs = nullptr;
	if (bound_pixel_shader_ != kNullShader) {
		auto it = shaders_.find(bound_pixel_shader_);
		if (it != shaders_.end()) ps = &it->second;
	}
	if (bound_vertex_shader_ != kNullShader) {
		auto it = shaders_.find(bound_vertex_shader_);
		if (it != shaders_.end()) vs = &it->second;
	}
	if (ps != nullptr) {
		std::memcpy(out.ps_program, ps->tokens, sizeof(out.ps_program));
		out.shader_counts[0] = static_cast<int32_t>(ps->instruction_count);
	}
	if (vs != nullptr) {
		std::memcpy(out.vs_program, vs->tokens, sizeof(out.vs_program));
		out.shader_counts[1] = static_cast<int32_t>(vs->instruction_count);
		// D3D8 maps the k-th D3DVSD_REG the declaration names onto the k-th vertex
		// element the stream supplies, so the mapping needs the bound FVF and can only
		// be resolved here, at draw time.
		for (uint32_t i = 0; i < kMaxVertexShaderInputs; ++i)
			out.vs_inputs[i / 4][i % 4] = -1;
		uint32_t element = 0;
		for (uint32_t va = 0; va < VA_COUNT && element < vs->declared_inputs.size(); ++va) {
			if (!layout.supplies[va]) continue;
			const uint32_t reg = vs->declared_inputs[element++];
			if (reg < kMaxVertexShaderInputs)
				out.vs_inputs[reg / 4][reg % 4] = static_cast<int32_t>(va);
		}
	}
	std::memcpy(out.ps_constants, pixel_shader_constants_, sizeof(out.ps_constants));
	std::memcpy(out.vs_constants, vertex_shader_constants_, sizeof(out.vs_constants));

	// --- user clip planes -----------------------------------------------------
	std::memcpy(out.clip_planes, clip_planes_, sizeof(out.clip_planes));
	// The pipeline always declares kMaxClipPlanes clip distances, so a disabled plane
	// has to be written as "inside" rather than left unwritten.
	out.clip_enable[0] = clip_distance_
	                         ? static_cast<int32_t>(render_states_[D3DRS_CLIPPLANEENABLE])
	                         : 0;
}

bool VulkanBackend::Prepare_Draw(uint32_t primitive_type, const VertexBufferHandle& vb) {
	if (!in_scene_) return false;
	++frame_draws_requested_;

	// An untyped buffer has no layout of its own; without one there is nothing to build a
	// pipeline from, and that is a lost draw like any other, counted under its own name.
	uint32_t fvf = 0;
	uint32_t declaration = 0;
	const VertexLayout* layout = nullptr;
	if (!Resolve_Draw_Layout(vb, fvf, declaration, layout)) {
		++frame_draws_dropped_;
		++frame_untyped_draws_dropped_;
		return false;
	}

	// The draw's own descriptor set and uniform slice, allocating a block if this frame is
	// the first to get this far. Failure here is a *lost draw*, not a slow one: the frame
	// will be missing geometry, so it is counted and reported rather than logged and
	// forgotten (docs/porting/draws-per-frame.md).
	VkDescriptorSet set = VK_NULL_HANDLE;
	VkBuffer ubo_buffer = VK_NULL_HANDLE;
	VkDeviceSize ubo_offset = 0;
	void* mapped = nullptr;
	if (!Draw_Slot(draw_index_, set, ubo_buffer, ubo_offset, mapped)) {
		if (frame_draws_dropped_ == 0) {
			std::fprintf(stderr,
			             "draw resources exhausted at draw %u (capacity %u): the frame will "
			             "be missing geometry\n",
			             draw_index_, draw_stats_.descriptor_capacity);
		}
		++frame_draws_dropped_;
		return false;
	}

	// This is DX8Wrapper::Apply_Render_State_Changes' job, moved to draw time
	// because Vulkan has no per-state setters.
	PipelineKey key;
	key.fvf = fvf;
	key.declaration = declaration;
	key.vertex_stride = layout->stride;
	key.topology = primitive_type;
	key.z_enable = render_states_[D3DRS_ZENABLE];
	key.z_write_enable = render_states_[D3DRS_ZWRITEENABLE];
	key.z_func = render_states_[D3DRS_ZFUNC];
	key.cull_mode = render_states_[D3DRS_CULLMODE];
	key.fill_mode = render_states_[D3DRS_FILLMODE];
	key.shade_mode = render_states_[D3DRS_SHADEMODE];
	key.alpha_blend_enable = render_states_[D3DRS_ALPHABLENDENABLE];
	key.src_blend = render_states_[D3DRS_SRCBLEND];
	key.dest_blend = render_states_[D3DRS_DESTBLEND];
	key.blend_op = render_states_[D3DRS_BLENDOP];
	key.color_write_enable = render_states_[D3DRS_COLORWRITEENABLE];
	key.stencil_enable = render_states_[D3DRS_STENCILENABLE];
	key.stencil_func = render_states_[D3DRS_STENCILFUNC];
	key.stencil_fail = render_states_[D3DRS_STENCILFAIL];
	key.stencil_zfail = render_states_[D3DRS_STENCILZFAIL];
	key.stencil_pass = render_states_[D3DRS_STENCILPASS];
	key.stencil_ref = render_states_[D3DRS_STENCILREF];
	key.stencil_mask = render_states_[D3DRS_STENCILMASK];
	key.stencil_write_mask = render_states_[D3DRS_STENCILWRITEMASK];
	key.depth_bias_enable = render_states_[D3DRS_ZBIAS] != 0 ? 1 : 0;
	key.has_depth_attachment = current_depth_ != nullptr ? 1 : 0;

	VkPipeline pipeline = Get_Or_Create_Pipeline(key, *layout);
	if (pipeline == VK_NULL_HANDLE) {
		// No pipeline is the same loss of geometry as no descriptor set.
		++frame_draws_dropped_;
		return false;
	}
	if (vb.untyped) ++frame_untyped_draws_issued_;

	DrawUniforms uniforms;
	Fill_Draw_Uniforms(primitive_type, *layout, uniforms);

	std::memcpy(mapped, &uniforms, sizeof(DrawUniforms));

	VkDescriptorBufferInfo buffer_info{ubo_buffer, ubo_offset, sizeof(DrawUniforms)};
	// Binding 1 is an array of kMaxTextureStages samplers written in one go. Stages
	// with nothing bound get the 1x1 white texture: D3D8 lets a cascade name a stage
	// with no texture, and Vulkan requires every descriptor in the array to be valid
	// whether or not the shader ends up sampling it.
	VkDescriptorImageInfo image_info[kMaxTextureStages]{};
	for (uint32_t i = 0; i < kMaxTextureStages; ++i) {
		TextureHandle* tex = bound_textures_[i] ? bound_textures_[i] : white_texture_;
		const SamplerKey sk{stages_[i].min_filter,  stages_[i].mag_filter,
		                    stages_[i].mip_filter, stages_[i].address_u,
		                    stages_[i].address_v,  stages_[i].address_w,
		                    stages_[i].border_color, stages_[i].max_anisotropy};
		image_info[i] = {Get_Or_Create_Sampler(sk), tex->image.view,
		                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
	}
	VkWriteDescriptorSet writes[2]{};
	writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
	writes[0].dstSet = set;
	writes[0].dstBinding = 0;
	writes[0].descriptorCount = 1;
	writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	writes[0].pBufferInfo = &buffer_info;
	writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
	writes[1].dstSet = set;
	writes[1].dstBinding = 1;
	writes[1].descriptorCount = kMaxTextureStages;
	writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	writes[1].pImageInfo = image_info;
	vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);

	vkCmdBindPipeline(frame_cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	// D3D8 has no pipeline object, so scissor, viewport and depth bias are dynamic
	// here; baking them in would give every rectangle its own VkPipeline.
	const VkRect2D scissor = scissor_enabled_
	                             ? Clamp_Scissor(scissor_)
	                             : VkRect2D{{0, 0}, {device_target_width_, device_target_height_}};
	vkCmdSetScissor(frame_cmd_, 0, 1, &scissor);
	// D3D8's viewport is y-down from the top-left of the target and so is Vulkan's,
	// so the rectangle carries over unchanged; the y flip lives in the projection
	// matrix, not here.
	//
	// It arrives in points and is scaled to the target's pixels, which is the whole of the
	// HiDPI fix at the draw level: the same triangles, rasterised over every pixel of the
	// panel rather than a quarter of them (docs/porting/hidpi-scale.md).
	const float viewport_scale = Surface_Render_Scale(current_color_);
	const VkViewport vk_viewport{static_cast<float>(viewport_.x) * viewport_scale,
	                             static_cast<float>(viewport_.y) * viewport_scale,
	                             static_cast<float>(viewport_.width) * viewport_scale,
	                             static_cast<float>(viewport_.height) * viewport_scale,
	                             viewport_.min_z,
	                             viewport_.max_z};
	vkCmdSetViewport(frame_cmd_, 0, 1, &vk_viewport);
	vkCmdSetDepthBias(
	    frame_cmd_, Z_Bias_To_Depth_Bias_Constant_Factor(render_states_[D3DRS_ZBIAS]), 0.0f,
	    0.0f);
	vkCmdBindDescriptorSets(frame_cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_,
	                        0, 1, &set, 0, nullptr);
	VkBuffer vertex_buffers[2] = {vb.buffer.buffer, dummy_vertex_buffer_.buffer};
	// A dynamic buffer's current ring region is applied here, so the engine's vertex
	// numbering is unchanged by the renaming DISCARD does behind the handle.
	VkDeviceSize offsets[2] = {vb.bind_offset, 0};
	vkCmdBindVertexBuffers(frame_cmd_, 0, 2, vertex_buffers, offsets);
	// Write funnel 3: the draw about to be recorded writes the current target, so a
	// host read of it has to pay a readback afterwards.
	Mark_Gpu_Write(current_color_);
	if (current_depth_ != nullptr) Mark_Gpu_Write(current_depth_);
	return true;
}

void VulkanBackend::Draw_Triangles(uint32_t start_index, uint32_t polygon_count,
                                   uint32_t min_vertex_index, uint32_t vertex_count) {
	Draw_Indexed_Primitive(D3DPT_TRIANGLELIST, start_index, polygon_count, min_vertex_index,
	                       vertex_count);
}

void VulkanBackend::Draw_Indexed_Primitive(uint32_t primitive_type, uint32_t start_index,
                                           uint32_t primitive_count,
                                           uint32_t min_vertex_index,
                                           uint32_t vertex_count) {
	// D3D8 passes MinIndex/NumVertices only so the driver can bound its software
	// vertex processing; Vulkan derives the same range from the indices it reads.
	(void)min_vertex_index;
	(void)vertex_count;
	if (bound_vb_ == nullptr || bound_ib_ == nullptr) return;
	if (!Prepare_Draw(primitive_type, *bound_vb_)) return;
	if (bound_vb_->dynamic) bound_vb_->region_last_use[bound_vb_->region] = frame_counter_;

	vkCmdBindIndexBuffer(frame_cmd_, bound_ib_->buffer.buffer, bound_ib_->bind_offset,
	                     VK_INDEX_TYPE_UINT16);
	if (bound_ib_->dynamic) bound_ib_->region_last_use[bound_ib_->region] = frame_counter_;
	vkCmdDrawIndexed(frame_cmd_, Vertex_Count_For_Primitives(primitive_type, primitive_count),
	                 1, start_index, index_base_offset_, 0);
	++draw_index_;
}

void VulkanBackend::Draw_Primitive(uint32_t primitive_type, uint32_t start_vertex,
                                   uint32_t primitive_count) {
	if (bound_vb_ == nullptr) return;
	if (!Prepare_Draw(primitive_type, *bound_vb_)) return;
	if (bound_vb_->dynamic) bound_vb_->region_last_use[bound_vb_->region] = frame_counter_;

	vkCmdDraw(frame_cmd_, Vertex_Count_For_Primitives(primitive_type, primitive_count), 1,
	          start_vertex, 0);
	++draw_index_;
}

void VulkanBackend::Draw_Primitive_UP(uint32_t primitive_type, uint32_t primitive_count,
                                      const void* vertex_data, uint32_t vertex_stride,
                                      uint32_t fvf) {
	if (vertex_data == nullptr || vertex_stride == 0 || up_mapped_ == nullptr) return;
	const uint32_t vertices = Vertex_Count_For_Primitives(primitive_type, primitive_count);
	if (vertices == 0) return;

	// The FVF's layout is what the pipeline's vertex input is built from, and a UP
	// draw is the only path where it arrives without a vertex buffer to hold it.
	auto layout_it = up_layouts_.find(fvf);
	if (layout_it == up_layouts_.end()) {
		VertexLayout layout;
		if (!Decode_Fvf(fvf, layout)) return;
		// The caller's stride wins: D3D8 lets a UP draw pass a stride larger than the
		// FVF needs, and the attribute offsets are relative to it either way.
		layout.stride = vertex_stride;
		layout_it = up_layouts_.emplace(fvf, layout).first;
	}

	const VkDeviceSize bytes = static_cast<VkDeviceSize>(vertices) * vertex_stride;
	// 16-byte alignment keeps every vertex's float4s naturally aligned.
	const VkDeviceSize offset = (up_offset_ + 15u) & ~VkDeviceSize{15u};
	if (offset + bytes > up_ring_.size) {
		std::fprintf(stderr, "spike limit: DrawPrimitiveUP ring exhausted (%llu bytes)\n",
		             static_cast<unsigned long long>(bytes));
		return;
	}
	std::memcpy(static_cast<uint8_t*>(up_mapped_) + offset, vertex_data,
	            static_cast<size_t>(bytes));

	VertexBufferHandle scratch;
	scratch.buffer = up_ring_;
	scratch.layout = layout_it->second;
	scratch.fvf = fvf;
	scratch.bind_offset = offset;
	if (!Prepare_Draw(primitive_type, scratch)) return;
	up_offset_ = offset + bytes;

	vkCmdDraw(frame_cmd_, vertices, 1, 0, 0);
	++draw_index_;
	// D3D8's DrawPrimitiveUP leaves stream 0 unbound, so the engine must call
	// SetStreamSource again before the next buffered draw. Matching that here means
	// a port cannot accidentally depend on the stronger guarantee.
	bound_vb_ = nullptr;
}

void VulkanBackend::End_Scene(bool flip_frame) {
	if (!in_scene_) return;
	// The frame's draw accounting, published before it is submitted so a caller that reads it
	// after End_Scene sees the frame it just ended.
	draw_stats_.draws_requested = frame_draws_requested_;
	draw_stats_.draws_issued = draw_index_;
	draw_stats_.draws_dropped = frame_draws_dropped_;
	draw_stats_.untyped_draws_issued = frame_untyped_draws_issued_;
	draw_stats_.untyped_draws_dropped = frame_untyped_draws_dropped_;
	draw_stats_.draws_dropped_total += frame_draws_dropped_;
	if (draw_index_ > draw_stats_.peak_draws_per_frame)
		draw_stats_.peak_draws_per_frame = draw_index_;
	// ZH_RENDER_DRAW_REPORT=N prints the accounting every N frames. The game has no way to
	// read DrawStats, and a mission's real draws-per-frame is the number this slice exists
	// for, so it has to be observable from a normal run rather than only from a spike.
	if (draw_report_interval_ != 0 && (frame_counter_ % draw_report_interval_) == 0) {
		std::fprintf(stderr,
		             "draws/frame: requested %u issued %u dropped %u untyped %u/%u (peak %u, "
		             "capacity %u in %u block(s), dropped total %llu)\n",
		             draw_stats_.draws_requested, draw_stats_.draws_issued,
		             draw_stats_.draws_dropped, draw_stats_.untyped_draws_issued,
		             draw_stats_.untyped_draws_dropped, draw_stats_.peak_draws_per_frame,
		             draw_stats_.descriptor_capacity, draw_stats_.descriptor_blocks,
		             static_cast<unsigned long long>(draw_stats_.draws_dropped_total));
	}
	End_Current_Pass();
	vkEndCommandBuffer(frame_cmd_);

	VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &frame_cmd_;
	vkQueueSubmit(queue_, 1, &si, frame_fence_);

	in_scene_ = false;
	if (flip_frame) Present();
}

bool VulkanBackend::Resize_Presentation(uint32_t width, uint32_t height) {
	if (swapchain_ == VK_NULL_HANDLE) return !presentation_required_;
	(void)width;
	(void)height;
	// A resize is also how a window arrives on a display with a different backing scale, and
	// the client area in points can come back unchanged across that move, so the scale is
	// re-read here rather than compared against the reported size.
	if (!Follow_Window_Scale(window_handle_)) return false;
	// The surface's own currentExtent is authoritative; the reported size is only the trigger.
	Destroy_Swapchain();
	return Build_Swapchain();
}

bool VulkanBackend::Present() {
	if (swapchain_ == VK_NULL_HANDLE) {
		// The run-time half of the guard: no swapchain means no image was handed to a
		// presentation engine, so this reports failure whatever the reason. A headless backend
		// reaches this too - Read_Back_Color_Target() is how a headless frame is observed, and a
		// caller that asked to flip instead deserves to be told it did not happen.
		if (!present_refusal_reported_) {
			present_refusal_reported_ = true;
			std::fprintf(stderr, "Vulkan backend: Present() with no swapchain presents nothing and "
			                     "reports failure%s.\n",
			             headless_ ? " (this backend is headless)" : "");
		}
		return false;
	}

	uint32_t index = 0;
	VK_CHECK(vkResetFences(device_, 1, &acquire_fence_));
	VkResult acquired = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
	                                          VK_NULL_HANDLE, acquire_fence_, &index);
	if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
		// The window changed under us without the seam reporting it yet.
		if (!Resize_Presentation(0, 0)) return false;
		VK_CHECK(vkResetFences(device_, 1, &acquire_fence_));
		acquired = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
		                                 VK_NULL_HANDLE, acquire_fence_, &index);
	}
	if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) return false;
	VK_CHECK(vkWaitForFences(device_, 1, &acquire_fence_, VK_TRUE, UINT64_MAX));

	VkCommandBuffer cmd = Begin_One_Shot();
	if (cmd == VK_NULL_HANDLE) return false;
	// The render pass leaves the target a colour attachment, because what happens to
	// it next is the engine's business: presentation is one of several possibilities.
	Transition_Surface(cmd, &default_color_surface_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	Transition(cmd, swapchain_images_[index], VK_IMAGE_LAYOUT_UNDEFINED,
	           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
	VkImageBlit blit{};
	blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	// The colour target's pixels, which at a backing scale of 2 are the swapchain's own
	// count: the blit stays, but it now copies 1:1 instead of upscaling a quarter-resolution
	// image (docs/porting/hidpi-scale.md).
	blit.srcOffsets[1] = {static_cast<int32_t>(device_width_),
	                      static_cast<int32_t>(device_height_), 1};
	// Stretched to the window, so a resized window is filled rather than painted in one corner.
	blit.dstOffsets[1] = {static_cast<int32_t>(swapchain_extent_.width),
	                      static_cast<int32_t>(swapchain_extent_.height), 1};
	vkCmdBlitImage(cmd, color_target_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	               swapchain_images_[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
	               VK_FILTER_LINEAR);
	Transition(cmd, swapchain_images_[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	           VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_ASPECT_COLOR_BIT);
	if (!End_One_Shot(cmd)) return false;

	VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
	// No wait semaphore: End_One_Shot() has already waited the queue idle, so the blit is
	// complete on the host's timeline before the image is handed to the presentation engine.
	pi.swapchainCount = 1;
	pi.pSwapchains = &swapchain_;
	pi.pImageIndices = &index;
	VkResult presented = vkQueuePresentKHR(queue_, &pi);
	return presented == VK_SUCCESS || presented == VK_SUBOPTIMAL_KHR;
}

bool VulkanBackend::Read_Back_Color_Target(std::string& out_rgba, SurfaceFormat& out_format) {
	// The pixels that exist, not the points they are addressed in: a caller that asks for the
	// frame at a backing scale of 2 gets the 1600x1200 it was rendered at, with the format
	// saying so, rather than a 800x600 crop of its top-left corner. The spike's PNG writer and
	// the pixel-comparison gate read the size from out_format for exactly this reason.
	const VkDeviceSize bytes = static_cast<VkDeviceSize>(device_width_) * device_height_ * 4;
	Buffer staging;
	if (!Allocate_Buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	                     staging)) {
		return false;
	}

	VkCommandBuffer cmd = Begin_One_Shot();
	if (cmd == VK_NULL_HANDLE) return false;
	Transition_Surface(cmd, &default_color_surface_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	VkBufferImageCopy copy{};
	copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	copy.imageExtent = {device_width_, device_height_, 1};
	vkCmdCopyImageToBuffer(cmd, color_target_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	                       staging.buffer, 1, &copy);
	if (!End_One_Shot(cmd)) return false;

	void* mapped = nullptr;
	VK_CHECK(vkMapMemory(device_, staging.memory, 0, bytes, 0, &mapped));
	out_rgba.assign(static_cast<const char*>(mapped), static_cast<size_t>(bytes));
	vkUnmapMemory(device_, staging.memory);

	vkDestroyBuffer(device_, staging.buffer, nullptr);
	vkFreeMemory(device_, staging.memory, nullptr);

	out_format.width = device_width_;
	out_format.height = device_height_;
	return true;
}

// ---------------------------------------------------------------------------
// render targets, surfaces and blits
//
// This is the group where D3D8 and Vulkan disagree most. D3D8's SetRenderTarget
// is a state setter: the target keeps its pixels, the viewport resets to the new
// target, and nothing is said about layouts or passes. Vulkan has no state setter
// at all -- a target change is a new render pass with a framebuffer, an explicit
// load-or-discard decision and an image layout on both sides. What that costs
// here is Get_Or_Create_Render_Pass/Framebuffer (two caches D3D8 does not need),
// per-surface layout tracking, and ending and restarting the frame's pass around
// every switch and every copy. See docs/porting/renderer-surface.md.
// ---------------------------------------------------------------------------

TextureHandle* VulkanBackend::Create_Render_Target_Texture(uint32_t width, uint32_t height) {
	if (width == 0 || height == 0) return nullptr;
	auto* handle = new TextureHandle();
	handle->render_target = true;
	handle->image.owner = handle;
	handle->format = TextureFormat::A8R8G8B8;
	handle->vk_format = kColorFormat;
	handle->image.width = width;
	handle->image.height = height;
	handle->image.mip_levels = 1;

	VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = kColorFormat;
	ici.extent = {width, height, 1};
	ici.mipLevels = 1;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	// D3DUSAGE_RENDERTARGET on a texture means all four of these: the engine renders
	// into it, samples it, copies out of it and copies into it.
	ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
	            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (vkCreateImage(device_, &ici, nullptr, &handle->image.image) != VK_SUCCESS) {
		delete handle;
		return nullptr;
	}

	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(device_, handle->image.image, &req);
	uint32_t type = 0;
	if (!Find_Memory_Type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, type)) {
		delete handle;
		return nullptr;
	}
	VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	mai.allocationSize = req.size;
	mai.memoryTypeIndex = type;
	if (vkAllocateMemory(device_, &mai, nullptr, &handle->image.memory) != VK_SUCCESS ||
	    vkBindImageMemory(device_, handle->image.image, handle->image.memory, 0) != VK_SUCCESS) {
		delete handle;
		return nullptr;
	}

	VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	vci.image = handle->image.image;
	vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vci.format = kColorFormat;
	vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	if (vkCreateImageView(device_, &vci, nullptr, &handle->image.view) != VK_SUCCESS) {
		delete handle;
		return nullptr;
	}

	// Cleared once, for the same reason a lockable texture is: D3D8 hands back a
	// render target with undefined contents, and sampling it before the first render
	// is legal there and a layout error here.
	VkCommandBuffer cmd = Begin_One_Shot();
	if (cmd == VK_NULL_HANDLE) {
		delete handle;
		return nullptr;
	}
	Transition(cmd, handle->image.image, VK_IMAGE_LAYOUT_UNDEFINED,
	           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
	const VkClearColorValue black{{0.0f, 0.0f, 0.0f, 0.0f}};
	const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdClearColorImage(cmd, handle->image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                     &black, 1, &all);
	Transition(cmd, handle->image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
	if (!End_One_Shot(cmd)) {
		delete handle;
		return nullptr;
	}
	handle->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	owned_textures_.push_back(handle);
	return handle;
}

SurfaceHandle* VulkanBackend::Get_Surface_Level(TextureHandle* texture, uint32_t level) {
	if (texture == nullptr) return nullptr;
	if (level != 0) {
		// GetSurfaceLevel(n>0) would need a per-level image view and a per-level
		// layout, and no engine render-target or CopyRects site asks for one.
		std::fprintf(stderr, "Get_Surface_Level: only level 0 is served\n");
		return nullptr;
	}
	// The same surface pointer every time: the engine compares the surface it saved
	// against the one it restores, and the framebuffer cache is keyed on identity.
	for (SurfaceHandle* existing : owned_surfaces_) {
		if (existing->owner == texture) return existing;
	}
	auto* surface = new SurfaceHandle();
	surface->image = &texture->image;
	surface->owner = texture;
	surface->width = texture->image.width;
	surface->height = texture->image.height;
	// The D3D8 format of the texture, not of its image: a level of an A4R4G4B4 texture is
	// an A4R4G4B4 surface to the caller even though the image behind it is B8G8R8A8.
	surface->format = texture->format;
	surface->vk_format = texture->vk_format;
	surface->layout = texture->layout;
	surface->pitch = texture->image.width * 4;
	owned_surfaces_.push_back(surface);
	return surface;
}

bool VulkanBackend::Set_Render_Target(SurfaceHandle* color, SurfaceHandle* depth_stencil) {
	// D3D8 takes NULL for "keep the device's own": SetRenderTarget(surface, NULL)
	// renders with no depth buffer, and the engine's save/restore passes back the
	// surfaces GetRenderTarget/GetDepthStencilSurface returned.
	SurfaceHandle* new_color = color != nullptr ? color : &default_color_surface_;
	if (new_color->system_memory()) {
		std::fprintf(stderr, "Set_Render_Target: a system-memory surface is not a target\n");
		return false;
	}
	if (depth_stencil != nullptr &&
	    (depth_stencil->system_memory() || !depth_stencil->depth_stencil)) {
		return false;
	}
	if (new_color == current_color_ && depth_stencil == current_depth_) return true;

	End_Current_Pass();
	// Leaving a render-target texture: the cascade may sample it in the very next
	// draw, which is the read-after-write D3D8 does not make the caller think about.
	if (in_scene_ && current_color_ != nullptr && current_color_->owner != nullptr &&
	    current_color_ != new_color) {
		Transition_Surface(frame_cmd_, current_color_,
		                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	}

	current_color_ = new_color;
	current_depth_ = depth_stencil;
	target_width_ = new_color->width;
	target_height_ = new_color->height;
	device_target_width_ = Scale_Extent(target_width_, Surface_Render_Scale(new_color));
	device_target_height_ = Scale_Extent(target_height_, Surface_Render_Scale(new_color));
	// D3D8 resets the viewport to the whole of the new target.
	viewport_ = ViewportRect{0, 0, target_width_, target_height_, 0.0f, 1.0f};
	scissor_enabled_ = false;
	// Write funnel 1: binding a target is what makes the draws that follow write it,
	// and the engine's render-to-texture paths read those pixels back through
	// SurfaceClass::Lock (screenshot, movie capture).
	Mark_Gpu_Write(new_color);
	if (depth_stencil != nullptr) Mark_Gpu_Write(depth_stencil);
	return Begin_Current_Pass();
}

SurfaceHandle* VulkanBackend::Create_Image_Surface(uint32_t width, uint32_t height,
                                                   TextureFormat format) {
	const uint32_t texel_bytes = Source_Texel_Bytes(format);
	if (width == 0 || height == 0 || texel_bytes == 0) {
		std::fprintf(stderr, "Create_Image_Surface: unsupported format\n");
		return nullptr;
	}
	auto* surface = new SurfaceHandle();
	surface->width = width;
	surface->height = height;
	surface->format = format;
	surface->texel_bytes = texel_bytes;
	surface->pitch = width * texel_bytes;
	surface->vk_format = Plan_For(format, view_swizzle_).vk;
	const VkDeviceSize bytes = static_cast<VkDeviceSize>(surface->pitch) * height;
	// D3DPOOL_SYSTEMMEM: host memory with no image behind it. Permanently mapped,
	// because the engine locks it, keeps the pointer and copies out of it later.
	if (!Allocate_Buffer(bytes,
	                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
	                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	                     surface->bits) ||
	    vkMapMemory(device_, surface->bits.memory, 0, bytes, 0, &surface->mapped) !=
	        VK_SUCCESS) {
		delete surface;
		return nullptr;
	}
	std::memset(surface->mapped, 0, static_cast<size_t>(bytes));
	owned_surfaces_.push_back(surface);
	return surface;
}

// The read half of the GPU-write hazard. SurfaceClass::Lock has no read/write flag
// -- and its signature is not changing, because that would edit 21 call sites -- so
// every lock is treated as a read and the dirty bit decides what that costs.
bool VulkanBackend::Resolve_Surface_Read(SurfaceHandle* surface) {
	if (Surface_Render_Scale(surface) != 1.0f) {
		// The device's back buffer at a backing scale other than 1: its pitch and height
		// describe points and its image holds pixels, so a copy driven by either would hand
		// back a crop or overrun the buffer. Read_Back_Color_Target(), which reports the
		// pixel size alongside the bytes, is the path for this surface
		// (docs/porting/hidpi-scale.md).
		std::fprintf(stderr, "Resolve_Surface_Read: the back buffer is %ux%u pixels for a %ux%u "
		                     "point surface; use Read_Back_Color_Target\n",
		             device_width_, device_height_, surface->width, surface->height);
		return false;
	}
	// A video-memory surface with no host buffer yet has to be copied out whatever
	// its dirty bit says: there is no host copy of it at all to hand back.
	const bool have_host_copy = surface->system_memory() || surface->mapped != nullptr;
	if (!surface->gpu_dirty() && have_host_copy) {
		// The whole point: a clean surface issues no copy, no submit and no wait.
		++resource_stats_.clean_reads;
		return true;
	}

	if (surface->system_memory()) {
		// The bytes are already addressed to this buffer -- CopyRects put them there --
		// but the copy is a queue operation, so the host has to wait for it. This is
		// the screenshot and movie-capture path.
		if (in_scene_) {
			if (!Flush_Frame_Commands()) return false;
		} else if (vkQueueWaitIdle(queue_) != VK_SUCCESS) {
			return false;
		}
		surface->host_gpu_dirty = false;
		++resource_stats_.dirty_reads;
		++resource_stats_.readback_stalls;
		return true;
	}

	// A video-memory surface has no host bytes at all until one is read: the buffer
	// is allocated on the first read and kept, because a caller that reads a render
	// target once reads it every frame (movie capture).
	const VkDeviceSize bytes = static_cast<VkDeviceSize>(surface->pitch) * surface->height;
	if (surface->mapped == nullptr) {
		if (!Allocate_Buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
		                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		                     surface->bits) ||
		    vkMapMemory(device_, surface->bits.memory, 0, bytes, 0, &surface->mapped) !=
		        VK_SUCCESS) {
			return false;
		}
	}

	bool one_shot = false;
	VkCommandBuffer cmd = Begin_Transfer(one_shot);
	if (cmd == VK_NULL_HANDLE) return false;
	const VkImageLayout was = surface->layout;
	Transition_Surface(cmd, surface, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	VkBufferImageCopy copy{};
	copy.bufferRowLength = surface->pitch / surface->texel_bytes;
	copy.bufferImageHeight = surface->height;
	copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	copy.imageExtent = {surface->width, surface->height, 1};
	vkCmdCopyImageToBuffer(cmd, surface->image->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	                       surface->bits.buffer, 1, &copy);
	if (was != VK_IMAGE_LAYOUT_UNDEFINED) Transition_Surface(cmd, surface, was);
	if (one_shot) {
		if (!End_One_Shot(cmd)) return false;
	} else if (!Flush_Frame_Commands(false)) {
		// Recorded into the open frame, so it has to be submitted and waited for
		// before the mapping holds anything.
		return false;
	}

	// Cleared only now that the copy has executed: a failure above leaves the bit
	// set, so the next read tries again rather than handing back stale pixels.
	surface->image->gpu_dirty = false;
	++resource_stats_.dirty_reads;
	++resource_stats_.readback_stalls;
	resource_stats_.surface_readback_bytes += bytes;
	return true;
}

bool VulkanBackend::Surface_Bits(SurfaceHandle* surface, LockedRect& out) {
	if (surface == nullptr) return false;
	if (surface->depth_stencil) {
		// D3D8 refuses LockRect on a depth-stencil surface too, and no engine site
		// asks for one.
		std::fprintf(stderr, "Surface_Bits: a depth-stencil surface cannot be locked\n");
		return false;
	}
	if (!Resolve_Surface_Read(surface)) return false;
	out.bits = surface->mapped;
	out.pitch = surface->pitch;
	return out.bits != nullptr;
}

// CopyRects between a host surface and an image of a format the device has no
// equivalent of. The host side holds the D3D8 texel layout, the image side holds the
// B8G8R8A8 the seam emulates it with, so the rectangle is expanded (or contracted)
// on the CPU on its way through, by the same two passes Unlock and Readback_Level
// use and through the same staging pool.
// Takes resource_mutex_, which is what the staging pool is guarded by; Copy_Rects
// does not hold it.
bool VulkanBackend::Copy_Rects_Converting(SurfaceHandle* source, const LockRect* rects,
                                          uint32_t count, SurfaceHandle* destination,
                                          const SurfacePoint* points) {
	std::lock_guard<std::mutex> guard(resource_mutex_);
	const LockRect whole{0, 0, source->width, source->height};
	const SurfacePoint origin{0, 0};

	// One block serves the whole call, so the pool sees one acquire rather than
	// rect_count of them. The write direction stages every rectangle at once and submits
	// once, so it needs the sum; the read direction submits and contracts one rectangle
	// at a time, so it needs the largest.
	VkDeviceSize scratch_bytes = 0;
	for (uint32_t i = 0; i < count; ++i) {
		const LockRect r = rects != nullptr ? rects[i] : whole;
		const SurfacePoint p = points != nullptr ? points[i] : origin;
		const uint32_t w = r.right - r.left;
		const uint32_t h = r.bottom - r.top;
		if (r.right > source->width || r.bottom > source->height || r.left >= r.right ||
		    r.top >= r.bottom || p.x + w > destination->width ||
		    p.y + h > destination->height) {
			return false;
		}
		const VkDeviceSize rect_bytes = static_cast<VkDeviceSize>(w) * h * 4;
		scratch_bytes = source->system_memory() ? scratch_bytes + rect_bytes
		                                        : std::max(scratch_bytes, rect_bytes);
	}
	if (scratch_bytes == 0) return true;

	SurfaceHandle* host = source->system_memory() ? source : destination;
	SurfaceHandle* image = source->system_memory() ? destination : source;
	if (host->mapped == nullptr || image->image == nullptr) return false;
	if (!source->system_memory() && !Contractable_From_Bgra8(source->format)) {
		// The read direction needs the inverse pass. A format the expansion is not
		// invertible for is refused here, before anything is copied, rather than
		// having plausible bytes written into the caller's surface.
		std::fprintf(stderr,
		             "Copy_Rects: no contraction for format %d; the read direction "
		             "cannot be served\n",
		             static_cast<int>(source->format));
		return false;
	}

	StagingBlock scratch;
	if (!Acquire_Staging(scratch_bytes, scratch)) return false;

	bool one_shot = false;
	VkCommandBuffer cmd = Begin_Transfer(one_shot);
	if (cmd == VK_NULL_HANDLE) {
		Release_Staging(scratch);
		return false;
	}
	const VkImageLayout image_layout = image->layout;
	Transition_Surface(cmd, image,
	                   source->system_memory() ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
	                                           : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

	// The engine's 2D composition passes a null rect array (the whole surface), so this
	// loop runs once there.
	std::vector<uint8_t> row;
	VkDeviceSize scratch_offset = 0;
	bool ok = true;
	for (uint32_t i = 0; i < count && ok; ++i) {
		const LockRect r = rects != nullptr ? rects[i] : whole;
		const SurfacePoint p = points != nullptr ? points[i] : origin;
		const uint32_t w = r.right - r.left;
		const uint32_t h = r.bottom - r.top;

		if (source->system_memory()) {
			// Expand the rectangle into the scratch block, tightly packed, then upload
			// it as its own image: bufferRowLength is the rectangle's width, not the
			// surface's, because the block holds only the rectangle.
			const auto* src = static_cast<const uint8_t*>(host->mapped);
			auto* dst = static_cast<uint8_t*>(scratch.mapped) + scratch_offset;
			for (uint32_t y = 0; y < h; ++y) {
				const TextureMip mip{src + static_cast<size_t>(r.top + y) * host->pitch +
				                         static_cast<size_t>(r.left) * host->texel_bytes,
				                     static_cast<size_t>(w) * host->texel_bytes, w, 1};
				Expand_To_Bgra8(host->format, mip, nullptr, row);
				std::memcpy(dst + static_cast<size_t>(y) * w * 4, row.data(), row.size());
			}
			++resource_stats_.cpu_expansions;
			VkBufferImageCopy copy{};
			copy.bufferOffset = scratch_offset;
			scratch_offset += static_cast<VkDeviceSize>(w) * h * 4;
			copy.bufferRowLength = w;
			copy.bufferImageHeight = h;
			copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			copy.imageOffset = {static_cast<int32_t>(p.x), static_cast<int32_t>(p.y), 0};
			copy.imageExtent = {w, h, 1};
			vkCmdCopyBufferToImage(cmd, scratch.buffer.buffer, image->image->image,
			                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
			continue;
		}

		// Read direction: the rectangle comes back as B8G8R8A8 and is contracted into
		// the caller's surface. It needs the copy to have executed, so the submit is
		// inside the loop.
		VkBufferImageCopy copy{};
		copy.bufferRowLength = w;
		copy.bufferImageHeight = h;
		copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		copy.imageOffset = {static_cast<int32_t>(r.left), static_cast<int32_t>(r.top), 0};
		copy.imageExtent = {w, h, 1};
		vkCmdCopyImageToBuffer(cmd, image->image->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		                       scratch.buffer.buffer, 1, &copy);
		ok = one_shot ? End_One_Shot(cmd) : Flush_Frame_Commands(false);
		if (ok) {
			ok = Contract_From_Bgra8(host->format,
			                         static_cast<const uint8_t*>(scratch.mapped), w, h,
			                         static_cast<uint8_t*>(host->mapped) +
			                             static_cast<size_t>(p.y) * host->pitch +
			                             static_cast<size_t>(p.x) * host->texel_bytes,
			                         host->pitch);
			++resource_stats_.cpu_expansions;
		}
		if (ok && i + 1 < count) {
			cmd = Begin_Transfer(one_shot);
			if (cmd == VK_NULL_HANDLE) ok = false;
			else Transition_Surface(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		} else {
			cmd = VK_NULL_HANDLE;
		}
	}

	if (cmd != VK_NULL_HANDLE) {
		// Back to what the image was in, then submitted and waited for: the scratch
		// block goes back to the pool, so nothing may still be reading it.
		if (image_layout != VK_IMAGE_LAYOUT_UNDEFINED) {
			Transition_Surface(cmd, image, image_layout);
		} else if (image->owner != nullptr) {
			Transition_Surface(cmd, image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		}
		const bool submitted = one_shot ? End_One_Shot(cmd) : Flush_Frame_Commands(false);
		ok = ok && submitted;
	}
	Release_Staging(scratch);
	if (!ok) return false;

	// Write funnel 4, the destination half. The host destination is not marked: the
	// contraction wrote it and the copy it read has already executed, so host and
	// image agree.
	if (!destination->system_memory()) Mark_Gpu_Write(destination);
	return true;
}

bool VulkanBackend::Copy_Rects(SurfaceHandle* source, const LockRect* rects,
                               uint32_t rect_count, SurfaceHandle* destination,
                               const SurfacePoint* points) {
	if (source == nullptr || destination == nullptr || source == destination) return false;
	if (Surface_Render_Scale(source) != 1.0f || Surface_Render_Scale(destination) != 1.0f) {
		// Every rectangle here is in the surface's advertised units, and for the device's
		// back buffer at a scale other than 1 those are points while the image is in pixels.
		// Scaling the rectangles would resample the copy, which CopyRects does not do, so
		// this refuses instead (docs/porting/hidpi-scale.md).
		std::fprintf(stderr, "Copy_Rects: the back buffer at backing scale %.2f cannot take a "
		                     "rectangle copy in points\n",
		             static_cast<double>(render_scale_));
		return false;
	}
	// Scale first, then formats: the refusal above leaves every surface that reaches the
	// conversion below advertising exactly the pixels its image holds, so the rectangles the
	// conversion walks are in one unit and there is no point/pixel ambiguity to reintroduce.
	// The device's own targets are the only surfaces whose scale can differ from 1
	// (Surface_Render_Scale) and they are B8G8R8A8, not an emulated format, so the two rules
	// cannot both apply to one copy.
	if (source->format != destination->format) {
		// D3D8 requires matching formats too; a mismatch here would silently
		// reinterpret bytes. The pair compared is the *D3D8* pair, because the
		// CPU-expanded formats all share VK_FORMAT_B8G8R8A8_UNORM: comparing VkFormats
		// passed an A4R4G4B4 surface into a B8G8R8A8 image as if it needed no
		// conversion.
		std::fprintf(stderr, "Copy_Rects: source and destination formats differ (%d vs %d)\n",
		             static_cast<int>(source->format), static_cast<int>(destination->format));
		return false;
	}
	const LockRect whole{0, 0, source->width, source->height};
	const SurfacePoint origin{0, 0};
	const uint32_t count = rects != nullptr ? rect_count : 1;
	if (count == 0) return true;

	// One side host, the other an image, and the D3D8 format is one Vulkan has no
	// equivalent for: the host bytes are in the D3D8 layout and the image holds the
	// B8G8R8A8 that layout is emulated with, so the bytes cannot travel unchanged. This
	// is the same CPU expansion the resource seam's Unlock does (Expand_To_Bgra8 /
	// Contract_From_Bgra8), run over the copied rectangle and staged through the same
	// pool -- not a second upload path, the same one addressed by rectangle. The
	// engine reaches this with the shell's text composition: a system-memory
	// A4R4G4B4 surface of rasterised glyphs copied into level 0 of an A4R4G4B4 texture
	// (render2dsentence.cpp, Render2DSentenceClass::Build_Textures).
	if (Plan_For(source->format, view_swizzle_).expand_to_bgra8 &&
	    source->system_memory() != destination->system_memory()) {
		return Copy_Rects_Converting(source, rects, count, destination, points);
	}

	// The all-host case needs no queue at all.
	if (source->system_memory() && destination->system_memory()) {
		for (uint32_t i = 0; i < count; ++i) {
			const LockRect r = rects != nullptr ? rects[i] : whole;
			const SurfacePoint p = points != nullptr ? points[i] : origin;
			if (r.right > source->width || r.bottom > source->height ||
			    p.x + (r.right - r.left) > destination->width ||
			    p.y + (r.bottom - r.top) > destination->height) {
				return false;
			}
			for (uint32_t y = r.top; y < r.bottom; ++y) {
				const uint8_t* src = static_cast<const uint8_t*>(source->mapped) +
				                     static_cast<size_t>(y) * source->pitch +
				                     static_cast<size_t>(r.left) * source->texel_bytes;
				uint8_t* dst = static_cast<uint8_t*>(destination->mapped) +
				               static_cast<size_t>(p.y + (y - r.top)) * destination->pitch +
				               static_cast<size_t>(p.x) * destination->texel_bytes;
				std::memcpy(dst, src, static_cast<size_t>(r.right - r.left) *
				                          source->texel_bytes);
			}
		}
		return true;
	}

	bool one_shot = false;
	VkCommandBuffer cmd = Begin_Transfer(one_shot);
	if (cmd == VK_NULL_HANDLE) return false;
	const VkImageLayout source_layout = source->layout;
	const VkImageLayout destination_layout = destination->layout;
	Transition_Surface(cmd, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	Transition_Surface(cmd, destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

	for (uint32_t i = 0; i < count; ++i) {
		const LockRect r = rects != nullptr ? rects[i] : whole;
		const SurfacePoint p = points != nullptr ? points[i] : origin;
		const uint32_t w = r.right - r.left;
		const uint32_t h = r.bottom - r.top;
		if (r.right > source->width || r.bottom > source->height ||
		    p.x + w > destination->width || p.y + h > destination->height) {
			continue;
		}
		if (!source->system_memory() && !destination->system_memory()) {
			VkImageCopy copy{};
			copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			copy.srcOffset = {static_cast<int32_t>(r.left), static_cast<int32_t>(r.top), 0};
			copy.dstOffset = {static_cast<int32_t>(p.x), static_cast<int32_t>(p.y), 0};
			copy.extent = {w, h, 1};
			vkCmdCopyImage(cmd, source->image->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			               destination->image->image,
			               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
		} else if (source->system_memory()) {
			// Host bytes into an image. bufferRowLength carries the source surface's
			// pitch, and the offset carries the source rectangle's corner, so no
			// intermediate copy of the rectangle is needed.
			VkBufferImageCopy copy{};
			copy.bufferOffset = static_cast<VkDeviceSize>(r.top) * source->pitch +
			                    static_cast<VkDeviceSize>(r.left) * source->texel_bytes;
			copy.bufferRowLength = source->pitch / source->texel_bytes;
			copy.bufferImageHeight = source->height;
			copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			copy.imageOffset = {static_cast<int32_t>(p.x), static_cast<int32_t>(p.y), 0};
			copy.imageExtent = {w, h, 1};
			vkCmdCopyBufferToImage(cmd, source->bits.buffer, destination->image->image,
			                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
		} else {
			VkBufferImageCopy copy{};
			copy.bufferOffset =
			    static_cast<VkDeviceSize>(p.y) * destination->pitch +
			    static_cast<VkDeviceSize>(p.x) * destination->texel_bytes;
			copy.bufferRowLength = destination->pitch / destination->texel_bytes;
			copy.bufferImageHeight = destination->height;
			copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			copy.imageOffset = {static_cast<int32_t>(r.left), static_cast<int32_t>(r.top), 0};
			copy.imageExtent = {w, h, 1};
			vkCmdCopyImageToBuffer(cmd, source->image->image,
			                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			                       destination->bits.buffer, 1, &copy);
		}
	}

	// Write funnel 4: the destination now holds pixels the host has not seen. For a
	// system-memory destination -- the screenshot and movie-capture staging surface --
	// the bytes are not even visible yet, because the copy is a queue operation that
	// may not have executed.
	Mark_Gpu_Write(destination);

	// Back to what each surface was in: a sampled texture keeps being sampled, and a
	// render target keeps being rendered into, neither of which the copy changed.
	if (source_layout != VK_IMAGE_LAYOUT_UNDEFINED)
		Transition_Surface(cmd, source, source_layout);
	if (destination_layout != VK_IMAGE_LAYOUT_UNDEFINED)
		Transition_Surface(cmd, destination, destination_layout);
	else if (destination->owner != nullptr)
		Transition_Surface(cmd, destination, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	return End_Transfer(cmd, one_shot);
}

bool VulkanBackend::Update_Texture(TextureHandle* source, TextureHandle* destination) {
	if (source == nullptr || destination == nullptr) return false;
	// D3D8's UpdateTexture is the managed-pool copy: a D3DPOOL_SYSTEMMEM texture's
	// levels into a D3DPOOL_DEFAULT texture's. The system-memory half here is a
	// lockable texture, whose bytes already live in a host-visible buffer, so the
	// copy is buffer-to-image and needs nothing from the video-memory source path.
	if (!source->lockable) {
		std::fprintf(stderr, "Update_Texture: the source must be a lockable texture\n");
		return false;
	}
	if (source->expand_on_unlock) {
		// Without a view swizzle the staging bytes are still in the D3D8 layout and
		// only the CPU expansion pass produces what the image wants; that pass
		// belongs to the resource seam's Unlock, not here.
		std::fprintf(stderr, "Update_Texture: CPU-expanded formats are not served\n");
		return false;
	}
	if (source->vk_format != destination->vk_format ||
	    source->image.width != destination->image.width ||
	    source->image.height != destination->image.height) {
		return false;
	}
	const uint32_t levels = std::min(source->image.mip_levels, destination->image.mip_levels);
	if (levels == 0 || source->levels.size() < levels) return false;

	bool one_shot = false;
	VkCommandBuffer cmd = Begin_Transfer(one_shot);
	if (cmd == VK_NULL_HANDLE) return false;
	Transition(cmd, destination->image.image, destination->layout,
	           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
	           destination->image.mip_levels);
	Transition(cmd, source->image.image, source->layout,
	           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
	           source->image.mip_levels);
	std::vector<VkImageCopy> copies;
	copies.reserve(levels);
	for (uint32_t level = 0; level < levels; ++level) {
		const LockableLevel& l = source->levels[level];
		VkImageCopy copy{};
		copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
		copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
		copy.extent = {l.width, l.height, 1};
		copies.push_back(copy);
	}
	vkCmdCopyImage(cmd, source->image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	               destination->image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	               static_cast<uint32_t>(copies.size()), copies.data());
	Transition(cmd, source->image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
	           source->image.mip_levels);
	Note_Texture_Layout(source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	Transition(cmd, destination->image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
	           destination->image.mip_levels);
	Note_Texture_Layout(destination, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	// Write funnel 5.
	Mark_Gpu_Write(&destination->image);
	return End_Transfer(cmd, one_shot);
}

void VulkanBackend::Set_Clip_Plane(uint32_t index, const float plane[4]) {
	if (index >= kMaxClipPlanes || plane == nullptr) return;
	// D3D8's clip planes are in world space when the fixed-function pipeline
	// transforms the vertices, which is the only case the engine has; the vertex
	// shader evaluates them against the world-space position for that reason.
	for (uint32_t i = 0; i < 4; ++i) clip_planes_[index][i] = plane[i];
}

// ---------------------------------------------------------------------------
// ps.1.1 / vs.1.1
//
// The engine loads compiled D3D8 token streams from .pso/.vso files
// (W3DShaderManager::LoadAndCreateD3DShader), so what arrives here is tokens, not
// source. They are validated here and interpreted by the uber-shader, which is the
// same choice the texture-stage cascade already makes.
// ---------------------------------------------------------------------------

namespace {

// Source operands per opcode. A table rather than the parameter-token high bit,
// because the encoding of that bit in D3D8 (as opposed to D3D9) is not something
// this spike can verify against a document it has.
int Source_Count(uint32_t opcode, bool& has_destination) {
	has_destination = true;
	switch (opcode) {
	case kSioNop: has_destination = false; return 0;
	case kSioTex:
	case kSioTexCoord:
	case kSioTexKill: return 0;
	case kSioMov:
	case kSioRcp:
	case kSioRsq:
	case kSioExp:
	case kSioLog:
	case kSioExpp:
	case kSioLogp:
	case kSioFrc:
	case kSioLit:
	case kSioTexBem:
	case kSioTexBemL: return 1;
	case kSioAdd:
	case kSioSub:
	case kSioMul:
	case kSioDp3:
	case kSioDp4:
	case kSioMin:
	case kSioMax:
	case kSioSlt:
	case kSioSge:
	case kSioDst:
	case kSioM4x4:
	case kSioM4x3:
	case kSioM3x4:
	case kSioM3x3:
	case kSioM3x2: return 2;
	case kSioMad:
	case kSioLrp:
	case kSioCnd: return 3;
	default: return -1; // not interpretable
	}
}

bool Parse_D3d8_Shader(const uint32_t* function, bool pixel, ShaderProgram& out) {
	if (function == nullptr) return false;
	out.pixel = pixel;
	out.version = function[0];
	const uint32_t magic = out.version >> 16;
	if (magic != (pixel ? 0xffffu : 0xfffeu)) {
		std::fprintf(stderr, "shader: not a %s token stream\n", pixel ? "pixel" : "vertex");
		return false;
	}
	const uint32_t major = (out.version >> 8) & 0xff;
	const uint32_t minor = out.version & 0xff;
	if (major != 1 || minor > 1) {
		// ps.1.4's phases and second address register, and anything 2.0 or later,
		// are a different language; the engine ships neither.
		std::fprintf(stderr, "shader: version %u.%u is not interpreted\n", major, minor);
		return false;
	}

	const uint32_t* p = function + 1;
	while (true) {
		const uint32_t token = *p++;
		const uint32_t opcode = token & kD3DSI_OpcodeMask;
		if (opcode == kSioEnd) break;
		if (opcode == kSioComment) {
			p += (token & kD3DSI_CommentSizeMask) >> kD3DSI_CommentSizeShift;
			continue;
		}
		if (opcode == kSioDef) {
			const uint32_t dst = *p++;
			std::array<float, 4> value{};
			for (int i = 0; i < 4; ++i) {
				float f = 0.0f;
				std::memcpy(&f, p + i, sizeof(f));
				value[static_cast<size_t>(i)] = f;
			}
			p += 4;
			out.defs.emplace_back(dst & kD3DSP_RegnumMask, value);
			continue;
		}
		bool has_destination = false;
		const int sources = Source_Count(opcode, has_destination);
		if (sources < 0) {
			std::fprintf(stderr, "shader: opcode %u is not interpreted\n", opcode);
			return false;
		}
		if (opcode == kSioNop) continue;
		if (out.instruction_count >= kMaxShaderInstructions) {
			std::fprintf(stderr, "shader: more than %u instructions\n",
			             kMaxShaderInstructions);
			return false;
		}
		int32_t* slot = out.tokens[out.instruction_count];
		slot[0] = static_cast<int32_t>(token);
		slot[1] = has_destination ? static_cast<int32_t>(*p++) : 0;
		for (int i = 0; i < sources; ++i) slot[2 + i] = static_cast<int32_t>(*p++);
		++out.instruction_count;
	}
	return out.instruction_count > 0;
}

} // namespace

ShaderProgram* VulkanBackend::Find_Shader(ShaderHandle handle) {
	auto it = shaders_.find(handle);
	return it == shaders_.end() ? nullptr : &it->second;
}

ShaderHandle VulkanBackend::Create_Pixel_Shader(const uint32_t* function) {
	ShaderProgram program;
	if (!Parse_D3d8_Shader(function, true, program)) return kNullShader;
	const ShaderHandle handle = Allocate_Shader_Handle();
	shaders_[handle] = std::move(program);
	return handle;
}

void VulkanBackend::Delete_Pixel_Shader(ShaderHandle shader) {
	if (bound_pixel_shader_ == shader) bound_pixel_shader_ = kNullShader;
	shaders_.erase(shader);
}

void VulkanBackend::Set_Pixel_Shader(ShaderHandle shader) {
	// D3D8's SetPixelShader(0) is "back to the texture-stage cascade".
	if (shader != kNullShader && Find_Shader(shader) == nullptr) return;
	bound_pixel_shader_ = shader;
	const ShaderProgram* program = shader != kNullShader ? Find_Shader(shader) : nullptr;
	if (program == nullptr) return;
	// `def` constants belong to the shader, and D3D8 applies them when it is set.
	for (const auto& def : program->defs) {
		if (def.first >= kMaxPixelShaderConstants) continue;
		for (uint32_t i = 0; i < 4; ++i)
			pixel_shader_constants_[def.first][i] = def.second[i];
	}
}

void VulkanBackend::Set_Pixel_Shader_Constant(uint32_t start_register, const void* data,
                                              uint32_t vector4_count) {
	if (data == nullptr) return;
	const auto* src = static_cast<const float*>(data);
	for (uint32_t v = 0; v < vector4_count; ++v) {
		const uint32_t reg = start_register + v;
		if (reg >= kMaxPixelShaderConstants) return;
		for (uint32_t i = 0; i < 4; ++i) pixel_shader_constants_[reg][i] = src[v * 4 + i];
	}
}

ShaderHandle VulkanBackend::Create_Vertex_Shader(const uint32_t* declaration,
                                                 const uint32_t* function, uint32_t usage) {
	// D3DUSAGE_SOFTWAREPROCESSING is the engine's fallback when the device has no
	// hardware vertex processing; the interpreter runs on the GPU either way.
	(void)usage;
	ShaderProgram program;
	if (!Parse_D3d8_Shader(function, false, program)) return kNullShader;
	// The declaration is refused, not approximated, when it is outside the set the
	// decoder is bounded to: a shader with a guessed layout would draw wrong geometry
	// silently, which is the defect class this path exists to remove.
	uint32_t regs[kMaxVertexShaderInputs];
	uint32_t reg_count = 0;
	const char* reason = nullptr;
	if (!Decode_Vertex_Declaration(declaration, program.declared_layout, regs, reg_count,
	                               reason)) {
		std::fprintf(stderr, "Create_Vertex_Shader: refused, the declaration has %s\n", reason);
		return kNullShader;
	}
	program.declared_inputs.assign(regs, regs + reg_count);
	program.declared_layout_hash = Hash_Vertex_Layout(program.declared_layout);
	// Once per program: the declarations the engine actually creates, for the enumeration
	// in docs/porting/untyped-vertex-buffers.md. Format: "vN:<vk format>@<offset>".
	std::string elements;
	for (uint32_t i = 0; i < reg_count; ++i) {
		const VkVertexInputAttributeDescription& a = program.declared_layout.attributes[i];
		const char* type = a.format == VK_FORMAT_R32_SFLOAT            ? "FLOAT1"
		                   : a.format == VK_FORMAT_R32G32_SFLOAT       ? "FLOAT2"
		                   : a.format == VK_FORMAT_R32G32B32_SFLOAT    ? "FLOAT3"
		                   : a.format == VK_FORMAT_R32G32B32A32_SFLOAT ? "FLOAT4"
		                                                               : "D3DCOLOR";
		if (!elements.empty()) elements += ' ';
		elements += 'v' + std::to_string(regs[i]) + ':' + type + '@' + std::to_string(a.offset);
	}
	std::fprintf(stderr, "vertex declaration %08x: %u input(s) stride %u [%s]\n",
	             program.declared_layout_hash, reg_count, program.declared_layout.stride,
	             elements.c_str());
	const ShaderHandle handle = Allocate_Shader_Handle();
	shaders_[handle] = std::move(program);
	return handle;
}

void VulkanBackend::Delete_Vertex_Shader(ShaderHandle shader) {
	if (bound_vertex_shader_ == shader) bound_vertex_shader_ = kNullShader;
	shaders_.erase(shader);
}

void VulkanBackend::Set_Vertex_Shader(ShaderHandle shader) {
	// The engine also passes plain FVF codes to SetVertexShader; those are not
	// programs and the FVF path already handles them, so only handles it issued are
	// accepted here.
	if (shader != kNullShader && Find_Shader(shader) == nullptr) return;
	bound_vertex_shader_ = shader;
}

void VulkanBackend::Set_Vertex_Shader_Constant(uint32_t start_register, const void* data,
                                               uint32_t vector4_count) {
	if (data == nullptr) return;
	const auto* src = static_cast<const float*>(data);
	for (uint32_t v = 0; v < vector4_count; ++v) {
		const uint32_t reg = start_register + v;
		if (reg >= kMaxVertexShaderConstants) return;
		for (uint32_t i = 0; i < 4; ++i) vertex_shader_constants_[reg][i] = src[v * 4 + i];
	}
}

// ---------------------------------------------------------------------------
// lockable index buffers (class C6 static / C5 dynamic ring)
// ---------------------------------------------------------------------------
// D3D8's CreateIndexBuffer hands back an empty buffer and the engine fills it through
// Lock/Unlock; nothing in the engine ever creates one with its contents in hand. The
// static and dynamic cases differ only in how many renamed copies sit behind the handle,
// exactly as in the vertex path above, so this mirrors it rather than inventing a second
// policy.

IndexBufferHandle* VulkanBackend::Create_Lockable_Index_Buffer(size_t count, bool dynamic) {
	if (count == 0) return nullptr;
	auto* handle = new IndexBufferHandle();
	handle->count = static_cast<uint32_t>(count);
	handle->dynamic = dynamic;
	handle->capacity = count * sizeof(uint16_t);
	handle->region_count = dynamic ? kDynamicRingRegions : 1;
	handle->region_last_use.assign(handle->region_count, 0);
	const VkMemoryPropertyFlags host =
	    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	const VkDeviceSize total = handle->capacity * handle->region_count;
	if (!Allocate_Buffer(total, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, host, handle->buffer) ||
	    vkMapMemory(device_, handle->buffer.memory, 0, total, 0, &handle->mapped) !=
	        VK_SUCCESS) {
		delete handle;
		return nullptr;
	}
	if (dynamic) {
		++resource_stats_.dynamic_buffer_allocations;
		resource_stats_.dynamic_buffer_bytes += total;
	}
	owned_ibs_.push_back(handle);
	return handle;
}

bool VulkanBackend::Lock_Index_Buffer(IndexBufferHandle* ib, size_t offset_indices,
                                      size_t count, uint32_t flags, void** out_bits) {
	if (ib == nullptr || out_bits == nullptr || ib->mapped == nullptr) return false;
	const VkDeviceSize offset = offset_indices * sizeof(uint16_t);
	const VkDeviceSize size = count * sizeof(uint16_t);
	if (offset + size > ib->capacity) return false;

	if ((flags & LOCK_DISCARD) != 0 && ib->region_count > 1) {
		ib->region = (ib->region + 1) % ib->region_count;
		if (ib->region_last_use[ib->region] > completed_frame_) {
			++resource_stats_.ring_wrap_waits;
			if (ib->region_last_use[ib->region] < frame_counter_) {
				vkWaitForFences(device_, 1, &frame_fence_, VK_TRUE, UINT64_MAX);
				completed_frame_ = frame_counter_ - 1;
			}
		}
		++resource_stats_.ring_discards;
	} else if ((flags & LOCK_NOOVERWRITE) != 0) {
		++resource_stats_.ring_appends;
	}
	ib->bind_offset = ib->capacity * ib->region;
	resource_stats_.ring_bytes += size;
	*out_bits = static_cast<uint8_t*>(ib->mapped) + ib->bind_offset + offset;
	return true;
}

bool VulkanBackend::Unlock_Index_Buffer(IndexBufferHandle* ib) {
	// Host-coherent, so nothing to flush -- the same free ride the vertex path gets.
	return ib != nullptr && ib->mapped != nullptr;
}

// ---------------------------------------------------------------------------
// adapter enumeration and measured capabilities
// ---------------------------------------------------------------------------
namespace {

// Every TextureFormat the device can sample, as a bitmask, measured one format at a time.
uint32_t Sampled_Format_Mask(VkPhysicalDevice physical, bool view_swizzle) {
	static const TextureFormat kAll[] = {
	    TextureFormat::A8R8G8B8, TextureFormat::X8R8G8B8, TextureFormat::R8G8B8,
	    TextureFormat::A4R4G4B4, TextureFormat::A1R5G5B5, TextureFormat::R5G6B5,
	    TextureFormat::L8,       TextureFormat::A8,       TextureFormat::A8L8,
	    TextureFormat::V8U8,     TextureFormat::P8,       TextureFormat::DXT1,
	    TextureFormat::DXT2,     TextureFormat::DXT3,     TextureFormat::DXT4,
	    TextureFormat::DXT5,
	};
	uint32_t mask = 0;
	for (TextureFormat format : kAll) {
		const FormatPlan plan = Plan_For(format, view_swizzle);
		VkFormatProperties props{};
		vkGetPhysicalDeviceFormatProperties(physical, plan.vk, &props);
		if ((props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0) {
			mask |= 1u << static_cast<int>(format);
		}
	}
	return mask;
}

void Fill_Adapter_Info(VkPhysicalDevice physical, bool view_swizzle, AdapterInfo& out) {
	VkPhysicalDeviceProperties props{};
	vkGetPhysicalDeviceProperties(physical, &props);
	VkPhysicalDeviceFeatures features{};
	vkGetPhysicalDeviceFeatures(physical, &features);
	VkPhysicalDeviceMemoryProperties mem{};
	vkGetPhysicalDeviceMemoryProperties(physical, &mem);

	out.name = props.deviceName;
	out.vendor_id = props.vendorID;
	out.device_id = props.deviceID;
	out.driver_version = props.driverVersion;
	out.api_version = props.apiVersion;
	out.discrete = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
	out.max_texture_dimension = props.limits.maxImageDimension2D;
	// The engine's cascade is at most kMaxTextureStages stages, so a device that could
	// bind more still reports what this backend implements.
	out.max_texture_stages =
	    std::min<uint32_t>(kMaxTextureStages, props.limits.maxPerStageDescriptorSampledImages);
	// 16-bit indices are the only ones the backend binds, so the reachable vertex index
	// is bounded by the index type, not only by the device limit.
	out.max_vertex_index = std::min<uint32_t>(props.limits.maxDrawIndexedIndexValue, 0xFFFFu);
	out.max_primitive_count = out.max_vertex_index;
	out.anisotropic_filtering = features.samplerAnisotropy == VK_TRUE;
	out.max_anisotropy = out.anisotropic_filtering ? props.limits.maxSamplerAnisotropy : 1.0f;
	for (uint32_t i = 0; i < mem.memoryHeapCount; ++i) {
		if ((mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) {
			out.device_memory_bytes += mem.memoryHeaps[i].size;
		}
	}
	out.sampled_formats = Sampled_Format_Mask(physical, view_swizzle);

	if (Device_Extension_Available(physical, VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME)) {
		VkPhysicalDeviceDriverPropertiesKHR driver{
		    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES_KHR};
		VkPhysicalDeviceProperties2 props2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
		props2.pNext = &driver;
		vkGetPhysicalDeviceProperties2(physical, &props2);
		out.driver = driver.driverName;
		if (driver.driverInfo[0] != '\0') {
			out.driver += " ";
			out.driver += driver.driverInfo;
		}
	}
}

} // namespace

bool VulkanBackend::Get_Adapter_Info(AdapterInfo& out) const {
	if (physical_ == VK_NULL_HANDLE) return false;
	out = AdapterInfo();
	Fill_Adapter_Info(physical_, view_swizzle_, out);
	return true;
}

bool Enumerate_Adapters(std::vector<AdapterInfo>& out, bool enable_validation) {
	out.clear();

	std::vector<const char*> extensions;
	std::vector<const char*> layers;
	VkInstanceCreateFlags flags = 0;
	if (Instance_Extension_Available(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
		extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
		flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
	}
	if (enable_validation) {
		uint32_t layer_count = 0;
		vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
		std::vector<VkLayerProperties> available(layer_count);
		vkEnumerateInstanceLayerProperties(&layer_count, available.data());
		for (const auto& l : available) {
			if (std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0) {
				layers.push_back("VK_LAYER_KHRONOS_validation");
			}
		}
	}

	VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.pApplicationName = "zh-adapter-enumeration";
	app.apiVersion = VK_API_VERSION_1_1;
	VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ci.pApplicationInfo = &app;
	ci.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
	ci.ppEnabledExtensionNames = extensions.empty() ? nullptr : extensions.data();
	ci.enabledLayerCount = static_cast<uint32_t>(layers.size());
	ci.ppEnabledLayerNames = layers.empty() ? nullptr : layers.data();
	ci.flags = flags;

	VkInstance instance = VK_NULL_HANDLE;
	const VkResult created = vkCreateInstance(&ci, nullptr, &instance);
	if (created != VK_SUCCESS) {
		std::fprintf(stderr, "Enumerate_Adapters: vkCreateInstance failed with VkResult %d\n",
		             static_cast<int>(created));
		return false;
	}

	uint32_t count = 0;
	if (vkEnumeratePhysicalDevices(instance, &count, nullptr) != VK_SUCCESS || count == 0) {
		vkDestroyInstance(instance, nullptr);
		return false;
	}
	std::vector<VkPhysicalDevice> devices(count);
	if (vkEnumeratePhysicalDevices(instance, &count, devices.data()) != VK_SUCCESS) {
		vkDestroyInstance(instance, nullptr);
		return false;
	}

	// The swizzle mode only decides which VkFormat a D3D8 format maps onto, and the
	// engine asks about formats before a device exists, so the enumeration reports what
	// the default (swizzled) mapping supports; the backend re-measures after Init().
	for (VkPhysicalDevice d : devices) {
		AdapterInfo info;
		Fill_Adapter_Info(d, true, info);
		out.push_back(std::move(info));
	}

	vkDestroyInstance(instance, nullptr);
	return true;
}

RenderBackend* Create_Vulkan_Backend(bool enable_validation, bool headless) {
	return new VulkanBackend(enable_validation, headless);
}

} // namespace spike
