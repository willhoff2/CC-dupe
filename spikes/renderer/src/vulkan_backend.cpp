// Renderer spike: a Vulkan implementation of the DX8Wrapper-shaped interface.
//
// Deliberately written in the style a real port would need: the engine's
// one-state-at-a-time D3D8 calls are recorded into a shadow state block, and the
// pipeline is materialised lazily at draw time from a hash of that block. Nothing
// here is engine-specific; it is the machinery every strategy in the write-up needs.
//
// What this file does NOT do, and a real backend must: mipmap generation, render
// targets, depth-stencil readback, dynamic vertex buffer rings, DXT decode,
// device-lost handling, multiple streams, or programmable ps.1.1 / vs.1.1 shaders.

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
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace spike {

Matrix4x4 Matrix4x4::Identity() {
	Matrix4x4 r{};
	for (int i = 0; i < 4; ++i) r.m[i][i] = 1.0f;
	return r;
}

namespace {

constexpr uint32_t kMaxDrawsPerFrame = 64;
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

// One mip level of a lockable texture: where in the persistent staging buffer its
// texels live, and the pitch handed to the caller. Tightly packed rows, so the
// pitch is the level width in bytes -- D3D8 does not promise any particular pitch,
// only that the caller uses the one it is given.
struct LockableLevel {
	VkDeviceSize offset = 0;
	uint32_t pitch = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	bool locked = false;
	uint32_t lock_flags = 0;
	LockRect lock_rect{};
};

struct TextureHandle {
	Image image;

	// --- lockable path (see docs/porting/renderer-resource-seam.md) ------------
	bool lockable = false;
	TextureFormat format = TextureFormat::A8R8G8B8;
	VkFormat vk_format = VK_FORMAT_B8G8R8A8_UNORM;
	bool expand_on_unlock = false;
	// Bytes per texel in the format the *caller* writes, which is the D3D8 format,
	// not necessarily the VkFormat the image has.
	uint32_t src_texel_bytes = 4;
	uint32_t dst_texel_bytes = 4;
	// Permanently mapped, permanently owned: a D3D8 lock may hand out a pointer that
	// outlives the Lock call (class C4) or even the Unlock (class C7).
	Buffer staging;
	void* staging_mapped = nullptr;
	// Second staging buffer, only when the device has no view swizzle and the format
	// has to be expanded on the CPU: the caller writes L8/A8/A8L8/X8R8G8B8 into
	// `staging`, Unlock expands into this, and this is what the image is copied from.
	Buffer upload;
	void* upload_mapped = nullptr;
	std::vector<LockableLevel> levels;
	VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

struct VertexBufferHandle {
	Buffer buffer;
	VertexLayout layout;
	uint32_t fvf = 0;

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

	TextureHandle* Create_Lockable_Texture(uint32_t width, uint32_t height,
	                                       TextureFormat format,
	                                       uint32_t mip_count) override;
	bool Lock_Texture(TextureHandle* texture, uint32_t level, const LockRect* rect,
	                  uint32_t flags, LockedRect& out) override;
	bool Unlock_Texture(TextureHandle* texture, uint32_t level) override;
	VertexBufferHandle* Create_Dynamic_Vertex_Buffer(size_t bytes, uint32_t fvf) override;
	bool Lock_Vertex_Buffer(VertexBufferHandle* vb, size_t offset, size_t size,
	                        uint32_t flags, void** out_bits) override;
	bool Unlock_Vertex_Buffer(VertexBufferHandle* vb) override;
	ResourceStats Get_Resource_Stats() const override { return resource_stats_; }

	void Set_Vertex_Buffer(VertexBufferHandle* vb, uint32_t stream) override;
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

	const char* Device_Description() const override { return device_description_.c_str(); }
	uint32_t Pipeline_Count() const override {
		return static_cast<uint32_t>(pipelines_.size());
	}

	// Public so main.cpp can drive the optional presentation path.
	bool Present();

	bool Resize_Presentation(uint32_t width, uint32_t height) override;

	uint32_t Validation_Message_Count() const override { return validation_messages_; }

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
	bool Create_Shaders();
	bool Create_Swapchain(void* window_handle);
	bool Build_Swapchain();
	void Destroy_Swapchain();

	bool Allocate_Buffer(VkDeviceSize size, VkBufferUsageFlags usage,
	                     VkMemoryPropertyFlags props, Buffer& out);
	bool Upload_Buffer(const void* data, VkDeviceSize size, VkBufferUsageFlags usage,
	                   Buffer& out);
	bool Find_Memory_Type(uint32_t type_bits, VkMemoryPropertyFlags props, uint32_t& out);
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
	void Transition(VkCommandBuffer cmd, VkImage image, VkImageLayout from,
	                VkImageLayout to, VkImageAspectFlags aspect,
	                uint32_t mip_levels = 1);

	bool validation_ = false;
	bool headless_ = true;
	uint32_t width_ = 0, height_ = 0;
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
	VkRenderPass render_pass_ = VK_NULL_HANDLE;
	VkFramebuffer framebuffer_ = VK_NULL_HANDLE;

	VkShaderModule vert_module_ = VK_NULL_HANDLE;
	VkShaderModule frag_module_ = VK_NULL_HANDLE;
	VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
	VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
	VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
	std::vector<VkDescriptorSet> descriptor_sets_;
	Buffer draw_uniforms_;   // kMaxDrawsPerFrame * aligned(DrawUniforms)
	VkDeviceSize ubo_stride_ = 0;
	uint32_t draw_index_ = 0;

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
	IndexBufferHandle* bound_ib_ = nullptr;
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
			std::fprintf(stderr, "note: VK_LAYER_KHRONOS_validation not present, continuing without\n");
		} else {
			extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
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

bool VulkanBackend::Create_Render_Targets() {
	auto make_image = [&](VkFormat format, VkImageUsageFlags usage,
	                      VkImageAspectFlags aspect, Image& out) -> bool {
		out.width = width_;
		out.height = height_;
		VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		ici.imageType = VK_IMAGE_TYPE_2D;
		ici.format = format;
		ici.extent = {width_, height_, 1};
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

	if (!make_image(kColorFormat,
	                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
	                VK_IMAGE_ASPECT_COLOR_BIT, color_target_)) {
		return false;
	}
	depth_format_ = Pick_Depth_Stencil_Format(physical_);
	if (!make_image(depth_format_, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
	                VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
	                depth_target_)) {
		return false;
	}

	VkAttachmentDescription attachments[2]{};
	attachments[0].format = kColorFormat;
	attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
	// DONT_CARE, not CLEAR: the engine clears with an explicit DX8Wrapper::Clear()
	// call inside Begin_Scene, and D3D8's Clear() takes flags per call. Mapping it
	// onto a render-pass load op would change semantics, so it becomes
	// vkCmdClearAttachments instead.
	attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachments[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

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
	subpass.pDepthStencilAttachment = &depth_ref;

	VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
	rpci.attachmentCount = 2;
	rpci.pAttachments = attachments;
	rpci.subpassCount = 1;
	rpci.pSubpasses = &subpass;
	VK_CHECK(vkCreateRenderPass(device_, &rpci, nullptr, &render_pass_));

	VkImageView views[2] = {color_target_.view, depth_target_.view};
	VkFramebufferCreateInfo fbci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
	fbci.renderPass = render_pass_;
	fbci.attachmentCount = 2;
	fbci.pAttachments = views;
	fbci.width = width_;
	fbci.height = height_;
	fbci.layers = 1;
	VK_CHECK(vkCreateFramebuffer(device_, &fbci, nullptr, &framebuffer_));
	return true;
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

	VkDescriptorPoolSize sizes[2]{};
	sizes[0] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kMaxDrawsPerFrame};
	sizes[1] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	            kMaxDrawsPerFrame * kMaxTextureStages};
	VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	dpci.maxSets = kMaxDrawsPerFrame;
	dpci.poolSizeCount = 2;
	dpci.pPoolSizes = sizes;
	VK_CHECK(vkCreateDescriptorPool(device_, &dpci, nullptr, &descriptor_pool_));

	std::vector<VkDescriptorSetLayout> layouts(kMaxDrawsPerFrame, set_layout_);
	VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	dsai.descriptorPool = descriptor_pool_;
	dsai.descriptorSetCount = kMaxDrawsPerFrame;
	dsai.pSetLayouts = layouts.data();
	descriptor_sets_.resize(kMaxDrawsPerFrame);
	VK_CHECK(vkAllocateDescriptorSets(device_, &dsai, descriptor_sets_.data()));

	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(physical_, &props);
	const VkDeviceSize align = props.limits.minUniformBufferOffsetAlignment;
	ubo_stride_ = ((sizeof(DrawUniforms) + align - 1) / (align ? align : 1)) * (align ? align : 1);
	if (ubo_stride_ == 0) ubo_stride_ = sizeof(DrawUniforms);

	if (!Allocate_Buffer(ubo_stride_ * kMaxDrawsPerFrame, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
	                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	                     draw_uniforms_)) {
		return false;
	}

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
	if (headless_ || window_handle == nullptr) return true;
	if (!Build_Swapchain()) return false;

	VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	VK_CHECK(vkCreateFence(device_, &fci, nullptr, &acquire_fence_));
	return true;
#else
	(void)window_handle;
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
	if (surface_ == VK_NULL_HANDLE) return true;

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
	if (extent.width == 0xFFFFFFFFu) extent = {width_, height_};
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
	return true;
#endif
}

bool VulkanBackend::Init(void* window_handle, uint32_t width, uint32_t height) {
	width_ = width;
	height_ = height;

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
	if (!Create_Render_Targets()) return false;
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

	for (auto* t : owned_textures_) {
		free_image(t->image);
		// A lockable texture's staging memory stays mapped for its whole life; the
		// unmap only happens here.
		if (t->staging_mapped != nullptr) vkUnmapMemory(device_, t->staging.memory);
		if (t->upload_mapped != nullptr) vkUnmapMemory(device_, t->upload.memory);
		free_buffer(t->staging);
		free_buffer(t->upload);
		delete t;
	}
	owned_textures_.clear();
	for (auto* vb : owned_vbs_) {
		if (vb->mapped != nullptr) vkUnmapMemory(device_, vb->buffer.memory);
		free_buffer(vb->buffer);
		delete vb;
	}
	owned_vbs_.clear();
	for (auto* ib : owned_ibs_) {
		free_buffer(ib->buffer);
		delete ib;
	}
	owned_ibs_.clear();

	free_buffer(draw_uniforms_);
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
	if (framebuffer_) vkDestroyFramebuffer(device_, framebuffer_, nullptr);
	if (render_pass_) vkDestroyRenderPass(device_, render_pass_, nullptr);
	if (descriptor_pool_) vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
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
	ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
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

} // namespace

TextureHandle* VulkanBackend::Create_Lockable_Texture(uint32_t width, uint32_t height,
                                                      TextureFormat format,
                                                      uint32_t mip_count) {
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

	// The staging buffer holds the whole mip chain, because a texture-loader lock
	// (class C4) locks every level at once and keeps all the pointers.
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

	const VkMemoryPropertyFlags host =
	    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	if (!Allocate_Buffer(staging_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
	                                       VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	                     host, handle->staging) ||
	    vkMapMemory(device_, handle->staging.memory, 0, staging_size, 0,
	                &handle->staging_mapped) != VK_SUCCESS) {
		delete handle;
		return nullptr;
	}
	std::memset(handle->staging_mapped, 0, static_cast<size_t>(staging_size));
	++resource_stats_.staging_allocations;
	resource_stats_.staging_bytes += staging_size;

	// No view swizzle (MoltenVK): the caller still writes D3D8's L8/A8/A8L8/X8R8G8B8
	// layout, so Unlock has to expand into a *second* host-visible buffer that the
	// image is actually copied from. That doubles the resident staging memory for
	// those formats and adds a CPU pass over every unlocked rectangle.
	if (handle->expand_on_unlock) {
		if (!Allocate_Buffer(upload_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, host,
		                     handle->upload) ||
		    vkMapMemory(device_, handle->upload.memory, 0, upload_size, 0,
		                &handle->upload_mapped) != VK_SUCCESS) {
			delete handle;
			return nullptr;
		}
		std::memset(handle->upload_mapped, 0, static_cast<size_t>(upload_size));
		++resource_stats_.staging_allocations;
		resource_stats_.staging_bytes += upload_size;
	}

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

	if ((flags & LOCK_READONLY) != 0) {
		if (texture->expand_on_unlock) {
			// The image holds expanded BGRA8, the caller expects the D3D8 format:
			// contracting back is not implemented, and no engine site needs it (no
			// READONLY lock in the classes uses an expanded format).
			std::fprintf(stderr,
			             "Lock_Texture: READONLY on a CPU-expanded format is not "
			             "implemented\n");
			return false;
		}
		// This is the cost D3D8 hides: a read-only lock is a copy back out of the
		// image, a queue submit, and a fence wait before the pointer can be handed
		// over. Nothing else can proceed in between.
		VkCommandBuffer cmd = Begin_One_Shot();
		if (cmd == VK_NULL_HANDLE) return false;
		Transition(cmd, texture->image.image, texture->layout,
		           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
		           texture->image.mip_levels);
		VkBufferImageCopy copy{};
		copy.bufferOffset = l.offset;
		copy.bufferRowLength = l.width;
		copy.bufferImageHeight = l.height;
		copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
		copy.imageExtent = {l.width, l.height, 1};
		vkCmdCopyImageToBuffer(cmd, texture->image.image,
		                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		                       texture->staging.buffer, 1, &copy);
		Transition(cmd, texture->image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
		           texture->image.mip_levels);
		if (!End_One_Shot(cmd)) return false;
		texture->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		++resource_stats_.readback_stalls;
	}

	l.locked = true;
	l.lock_flags = flags;
	l.lock_rect = r;
	// The pointer is into the persistent mapping, offset to the rectangle's first
	// texel, exactly as D3D8 documents pBits for a sub-rect lock.
	out.bits = static_cast<uint8_t*>(texture->staging_mapped) + l.offset +
	           static_cast<size_t>(r.top) * l.pitch +
	           static_cast<size_t>(r.left) * texture->src_texel_bytes;
	out.pitch = l.pitch;
	return true;
}

bool VulkanBackend::Unlock_Texture(TextureHandle* texture, uint32_t level) {
	if (texture == nullptr || !texture->lockable || level >= texture->levels.size()) {
		return false;
	}
	LockableLevel& l = texture->levels[level];
	if (!l.locked) return false;
	l.locked = false;

	// A read-only lock uploads nothing. This is why the write-only/read-only
	// distinction matters: it is the difference between a copy and no copy.
	if ((l.lock_flags & LOCK_READONLY) != 0) return true;

	const LockRect r = l.lock_rect;
	const uint32_t rect_width = r.right - r.left;
	const uint32_t rect_height = r.bottom - r.top;

	VkBuffer source = texture->staging.buffer;
	VkDeviceSize source_offset = l.offset + static_cast<VkDeviceSize>(r.top) * l.pitch +
	                             static_cast<VkDeviceSize>(r.left) * texture->src_texel_bytes;
	uint32_t row_length = l.pitch / texture->src_texel_bytes;

	if (texture->expand_on_unlock) {
		// CPU channel expansion, row by row, into the second staging buffer. Only the
		// locked rectangle is expanded, so a partial-rect unlock does not cost a pass
		// over the whole level.
		VkDeviceSize level_upload_offset = 0;
		for (uint32_t i = 0; i < level; ++i) {
			level_upload_offset += static_cast<VkDeviceSize>(texture->levels[i].width) *
			                       texture->levels[i].height * 4;
		}
		auto* dst = static_cast<uint8_t*>(texture->upload_mapped) + level_upload_offset;
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
		source = texture->upload.buffer;
		source_offset = level_upload_offset +
		                (static_cast<VkDeviceSize>(r.top) * l.width + r.left) * 4;
		row_length = l.width;
	}

	VkCommandBuffer cmd = Begin_One_Shot();
	if (cmd == VK_NULL_HANDLE) return false;
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
	if (!End_One_Shot(cmd)) return false;
	texture->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	++resource_stats_.texture_upload_regions;
	++resource_stats_.upload_submits;
	return true;
}

VertexBufferHandle* VulkanBackend::Create_Dynamic_Vertex_Buffer(size_t bytes,
                                                                uint32_t fvf) {
	if (bytes == 0) return nullptr;
	auto* handle = new VertexBufferHandle();
	handle->fvf = fvf;
	if (!Decode_Fvf(fvf, handle->layout)) {
		std::fprintf(stderr, "Decode_Fvf: unsupported FVF 0x%x\n", fvf);
		delete handle;
		return nullptr;
	}
	handle->dynamic = true;
	handle->capacity = bytes;
	// D3DLOCK_DISCARD is "rename this buffer": the driver hands back memory the GPU
	// is not reading. Reproducing that needs more than one copy behind the handle,
	// one per frame that can be in flight, plus one being written.
	handle->region_count = kDynamicRingRegions;
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
	++resource_stats_.staging_allocations;
	resource_stats_.staging_bytes += total;
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

void VulkanBackend::Set_Vertex_Buffer(VertexBufferHandle* vb, uint32_t stream) {
	if (stream == 0) bound_vb_ = vb;
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

	VkViewport viewport{0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_), 0.0f, 1.0f};
	VkRect2D scissor{{0, 0}, {width_, height_}};
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
	gpci.renderPass = render_pass_;
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

	VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
	rpbi.renderPass = render_pass_;
	rpbi.framebuffer = framebuffer_;
	rpbi.renderArea = {{0, 0}, {width_, height_}};
	vkCmdBeginRenderPass(frame_cmd_, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

	draw_index_ = 0;
	in_scene_ = true;
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
	VkClearRect rect{{{0, 0}, {width_, height_}}, 0, 1};
	vkCmdClearAttachments(frame_cmd_, count, clears, 1, &rect);
}

VkRect2D VulkanBackend::Clamp_Scissor(const VkRect2D& rect) const {
	// D3D8's SetScissors rectangle is in the same space as the viewport but is not
	// required to lie inside the render target; Vulkan requires that it does.
	int32_t x0 = rect.offset.x < 0 ? 0 : rect.offset.x;
	int32_t y0 = rect.offset.y < 0 ? 0 : rect.offset.y;
	int64_t x1 = static_cast<int64_t>(rect.offset.x) + rect.extent.width;
	int64_t y1 = static_cast<int64_t>(rect.offset.y) + rect.extent.height;
	if (x1 > width_) x1 = width_;
	if (y1 > height_) y1 = height_;
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
	out.misc[1] = static_cast<float>(width_);
	out.misc[2] = static_cast<float>(height_);

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
}

bool VulkanBackend::Prepare_Draw(uint32_t primitive_type, const VertexBufferHandle& vb) {
	if (!in_scene_) return false;
	if (draw_index_ >= kMaxDrawsPerFrame) {
		std::fprintf(stderr, "spike limit: more than %u draws per frame\n", kMaxDrawsPerFrame);
		return false;
	}

	// This is DX8Wrapper::Apply_Render_State_Changes' job, moved to draw time
	// because Vulkan has no per-state setters.
	PipelineKey key;
	key.fvf = vb.fvf;
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

	VkPipeline pipeline = Get_Or_Create_Pipeline(key, vb.layout);
	if (pipeline == VK_NULL_HANDLE) return false;

	DrawUniforms uniforms;
	Fill_Draw_Uniforms(primitive_type, vb.layout, uniforms);

	const VkDeviceSize ubo_offset = ubo_stride_ * draw_index_;
	void* mapped = nullptr;
	vkMapMemory(device_, draw_uniforms_.memory, ubo_offset, sizeof(DrawUniforms), 0, &mapped);
	std::memcpy(mapped, &uniforms, sizeof(DrawUniforms));
	vkUnmapMemory(device_, draw_uniforms_.memory);

	VkDescriptorSet set = descriptor_sets_[draw_index_];
	VkDescriptorBufferInfo buffer_info{draw_uniforms_.buffer, ubo_offset, sizeof(DrawUniforms)};
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
	const VkRect2D scissor =
	    scissor_enabled_ ? Clamp_Scissor(scissor_) : VkRect2D{{0, 0}, {width_, height_}};
	vkCmdSetScissor(frame_cmd_, 0, 1, &scissor);
	// D3D8's viewport is y-down from the top-left of the target and so is Vulkan's,
	// so the rectangle carries over unchanged; the y flip lives in the projection
	// matrix, not here.
	const VkViewport vk_viewport{static_cast<float>(viewport_.x),
	                             static_cast<float>(viewport_.y),
	                             static_cast<float>(viewport_.width),
	                             static_cast<float>(viewport_.height),
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

	vkCmdBindIndexBuffer(frame_cmd_, bound_ib_->buffer.buffer, 0, VK_INDEX_TYPE_UINT16);
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
	vkCmdEndRenderPass(frame_cmd_);
	vkEndCommandBuffer(frame_cmd_);

	VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &frame_cmd_;
	vkQueueSubmit(queue_, 1, &si, frame_fence_);

	in_scene_ = false;
	if (flip_frame) Present();
}

bool VulkanBackend::Resize_Presentation(uint32_t width, uint32_t height) {
	if (swapchain_ == VK_NULL_HANDLE) return true;
	(void)width;
	(void)height;
	// The surface's own currentExtent is authoritative; the reported size is only the trigger.
	Destroy_Swapchain();
	return Build_Swapchain();
}

bool VulkanBackend::Present() {
	if (swapchain_ == VK_NULL_HANDLE) return true;

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
	Transition(cmd, swapchain_images_[index], VK_IMAGE_LAYOUT_UNDEFINED,
	           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
	VkImageBlit blit{};
	blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	blit.srcOffsets[1] = {static_cast<int32_t>(width_), static_cast<int32_t>(height_), 1};
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
	const VkDeviceSize bytes = static_cast<VkDeviceSize>(width_) * height_ * 4;
	Buffer staging;
	if (!Allocate_Buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	                     staging)) {
		return false;
	}

	VkCommandBuffer cmd = Begin_One_Shot();
	if (cmd == VK_NULL_HANDLE) return false;
	VkBufferImageCopy copy{};
	copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	copy.imageExtent = {width_, height_, 1};
	vkCmdCopyImageToBuffer(cmd, color_target_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	                       staging.buffer, 1, &copy);
	if (!End_One_Shot(cmd)) return false;

	void* mapped = nullptr;
	VK_CHECK(vkMapMemory(device_, staging.memory, 0, bytes, 0, &mapped));
	out_rgba.assign(static_cast<const char*>(mapped), static_cast<size_t>(bytes));
	vkUnmapMemory(device_, staging.memory);

	vkDestroyBuffer(device_, staging.buffer, nullptr);
	vkFreeMemory(device_, staging.memory, nullptr);

	out_format.width = width_;
	out_format.height = height_;
	return true;
}

RenderBackend* Create_Vulkan_Backend(bool enable_validation, bool headless) {
	return new VulkanBackend(enable_validation, headless);
}

} // namespace spike
