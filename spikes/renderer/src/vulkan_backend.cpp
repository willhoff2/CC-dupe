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
constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

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
};

// Sampler state in D3D8 is texture *stage* state, not part of the texture object.
// In Vulkan it is a VkSampler bound alongside the image, so it needs its own cache.
struct SamplerKey {
	uint32_t min_filter, mag_filter, mip_filter, address_u, address_v;
	bool operator==(const SamplerKey& o) const {
		return min_filter == o.min_filter && mag_filter == o.mag_filter &&
		       mip_filter == o.mip_filter && address_u == o.address_u &&
		       address_v == o.address_v;
	}
};

struct SamplerKeyHash {
	size_t operator()(const SamplerKey& k) const {
		size_t h = 1469598103934665603ull;
		for (uint32_t v : {k.min_filter, k.mag_filter, k.mip_filter, k.address_u, k.address_v}) {
			h = (h ^ v) * 1099511628211ull;
		}
		return h;
	}
};

struct PushConstants {
	float wvp[16];
	int32_t flags[4];
	float viewport[4];
};
static_assert(sizeof(PushConstants) == 96, "must fit the 128-byte guaranteed minimum");

struct PerStage {
	uint32_t color_op = D3DTOP_DISABLE;
	uint32_t color_arg1 = D3DTA_TEXTURE;
	uint32_t color_arg2 = D3DTA_CURRENT;
	uint32_t alpha_op = D3DTOP_DISABLE;
	uint32_t alpha_arg1 = D3DTA_TEXTURE;
	uint32_t alpha_arg2 = D3DTA_CURRENT;
	uint32_t texcoord_index = 0;
	uint32_t min_filter = D3DTEXF_LINEAR;
	uint32_t mag_filter = D3DTEXF_LINEAR;
	uint32_t mip_filter = D3DTEXF_NONE;
	uint32_t address_u = D3DTADDRESS_WRAP;
	uint32_t address_v = D3DTADDRESS_WRAP;
};

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

struct TextureHandle {
	Image image;
};

struct VertexBufferHandle {
	Buffer buffer;
	VertexLayout layout;
	uint32_t fvf = 0;
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

	TextureHandle* Create_Texture(uint32_t width, uint32_t height,
	                              const uint8_t* argb_pixels) override;
	VertexBufferHandle* Create_Vertex_Buffer(const void* data, size_t bytes,
	                                         uint32_t fvf) override;
	IndexBufferHandle* Create_Index_Buffer(const uint16_t* data, size_t count) override;

	void Set_Vertex_Buffer(VertexBufferHandle* vb, uint32_t stream) override;
	void Set_Index_Buffer(IndexBufferHandle* ib, uint32_t index_base_offset) override;

	void Draw_Triangles(uint32_t start_index, uint32_t polygon_count,
	                    uint32_t min_vertex_index, uint32_t vertex_count) override;

	bool Read_Back_Color_Target(std::string& out_rgba, SurfaceFormat& out_format) override;

	const char* Device_Description() const override { return device_description_.c_str(); }
	uint32_t Pipeline_Count() const override {
		return static_cast<uint32_t>(pipelines_.size());
	}

	// Public so main.cpp can drive the optional presentation path.
	bool Present();

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

	bool Allocate_Buffer(VkDeviceSize size, VkBufferUsageFlags usage,
	                     VkMemoryPropertyFlags props, Buffer& out);
	bool Upload_Buffer(const void* data, VkDeviceSize size, VkBufferUsageFlags usage,
	                   Buffer& out);
	bool Find_Memory_Type(uint32_t type_bits, VkMemoryPropertyFlags props, uint32_t& out);
	VkCommandBuffer Begin_One_Shot();
	bool End_One_Shot(VkCommandBuffer cmd);

	VkPipeline Get_Or_Create_Pipeline(const PipelineKey& key, const VertexLayout& layout);
	VkSampler Get_Or_Create_Sampler(const SamplerKey& key);
	void Transition(VkCommandBuffer cmd, VkImage image, VkImageLayout from,
	                VkImageLayout to, VkImageAspectFlags aspect);

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
	VkRenderPass render_pass_ = VK_NULL_HANDLE;
	VkFramebuffer framebuffer_ = VK_NULL_HANDLE;

	VkShaderModule vert_module_ = VK_NULL_HANDLE;
	VkShaderModule frag_module_ = VK_NULL_HANDLE;
	VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
	VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
	VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
	std::vector<VkDescriptorSet> descriptor_sets_;
	Buffer stage_uniforms_;   // kMaxDrawsPerFrame * aligned(StageUniforms)
	VkDeviceSize ubo_stride_ = 0;
	uint32_t draw_index_ = 0;

	Buffer dummy_vertex_buffer_; // feeds attributes the FVF does not supply
	TextureHandle* white_texture_ = nullptr;

	std::unordered_map<uint64_t, VkPipeline> pipelines_;
	std::unordered_map<SamplerKey, VkSampler, SamplerKeyHash> samplers_;
	std::vector<TextureHandle*> owned_textures_;
	std::vector<VertexBufferHandle*> owned_vbs_;
	std::vector<IndexBufferHandle*> owned_ibs_;

	// --- shadow state, mirroring DX8Wrapper::RenderStates / TextureStageStates ---
	uint32_t render_states_[D3DRS_MAX]{};
	PerStage stages_[2];
	TextureHandle* bound_textures_[2]{nullptr, nullptr};
	Matrix4x4 world_ = Matrix4x4::Identity();
	Matrix4x4 view_ = Matrix4x4::Identity();
	Matrix4x4 projection_ = Matrix4x4::Identity();
	VertexBufferHandle* bound_vb_ = nullptr;
	IndexBufferHandle* bound_ib_ = nullptr;
	uint32_t index_base_offset_ = 0;
	bool in_scene_ = false;

	// --- optional presentation -------------------------------------------------
	VkSurfaceKHR surface_ = VK_NULL_HANDLE;
	VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
	std::vector<VkImage> swapchain_images_;
	VkFormat swapchain_format_ = VK_FORMAT_UNDEFINED;
	VkSemaphore acquire_semaphore_ = VK_NULL_HANDLE;
};

// ---------------------------------------------------------------------------
// initialisation
// ---------------------------------------------------------------------------

bool VulkanBackend::Create_Instance(void* window_handle) {
	std::vector<const char*> extensions;
	std::vector<const char*> layers;

#ifdef SPIKE_WITH_SDL
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

	VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	dci.enabledExtensionCount = static_cast<uint32_t>(device_extensions.size());
	dci.ppEnabledExtensionNames = device_extensions.empty() ? nullptr : device_extensions.data();
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
                               VkImageLayout to, VkImageAspectFlags aspect) {
	VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	b.oldLayout = from;
	b.newLayout = to;
	b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = image;
	b.subresourceRange = {aspect, 0, 1, 0, 1};
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
	if (!make_image(kDepthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
	                VK_IMAGE_ASPECT_DEPTH_BIT, depth_target_)) {
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

	attachments[1].format = kDepthFormat;
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
	VkDescriptorSetLayoutBinding bindings[3]{};
	bindings[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
	bindings[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
	bindings[2] = {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};

	VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	lci.bindingCount = 3;
	lci.pBindings = bindings;
	VK_CHECK(vkCreateDescriptorSetLayout(device_, &lci, nullptr, &set_layout_));

	VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(PushConstants)};
	VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	plci.setLayoutCount = 1;
	plci.pSetLayouts = &set_layout_;
	plci.pushConstantRangeCount = 1;
	plci.pPushConstantRanges = &range;
	VK_CHECK(vkCreatePipelineLayout(device_, &plci, nullptr, &pipeline_layout_));

	VkDescriptorPoolSize sizes[2]{};
	sizes[0] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kMaxDrawsPerFrame};
	sizes[1] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxDrawsPerFrame * 2};
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
	ubo_stride_ = ((sizeof(StageUniforms) + align - 1) / (align ? align : 1)) * (align ? align : 1);
	if (ubo_stride_ == 0) ubo_stride_ = sizeof(StageUniforms);

	if (!Allocate_Buffer(ubo_stride_ * kMaxDrawsPerFrame, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
	                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	                     stage_uniforms_)) {
		return false;
	}

	// 0xff fill: attributes the FVF omits read white (1,1,1,1), matching D3D8's
	// behaviour of substituting the material's diffuse when the vertex has none.
	if (!Upload_Buffer(nullptr, 64 * 1024, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
	                   dummy_vertex_buffer_)) {
		return false;
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
#ifdef SPIKE_WITH_SDL
	if (headless_ || window_handle == nullptr) return true;

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
	sci.imageExtent = {width_, height_};
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

	VkSemaphoreCreateInfo semci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
	VK_CHECK(vkCreateSemaphore(device_, &semci, nullptr, &acquire_semaphore_));
	return true;
#else
	(void)window_handle;
	return true;
#endif
}

bool VulkanBackend::Init(void* window_handle, uint32_t width, uint32_t height) {
	width_ = width;
	height_ = height;

	if (!Create_Instance(window_handle)) return false;

#ifdef SPIKE_WITH_SDL
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
	render_states_[D3DRS_TEXTUREFACTOR] = 0xffffffff;

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
		delete t;
	}
	owned_textures_.clear();
	for (auto* vb : owned_vbs_) {
		free_buffer(vb->buffer);
		delete vb;
	}
	owned_vbs_.clear();
	for (auto* ib : owned_ibs_) {
		free_buffer(ib->buffer);
		delete ib;
	}
	owned_ibs_.clear();

	free_buffer(stage_uniforms_);
	free_buffer(dummy_vertex_buffer_);
	free_image(color_target_);
	free_image(depth_target_);

	if (acquire_semaphore_) vkDestroySemaphore(device_, acquire_semaphore_, nullptr);
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

TextureHandle* VulkanBackend::Create_Texture(uint32_t width, uint32_t height,
                                             const uint8_t* argb_pixels) {
	auto* handle = new TextureHandle();
	handle->image.width = width;
	handle->image.height = height;

	VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ici.imageType = VK_IMAGE_TYPE_2D;
	// D3DFMT_A8R8G8B8 is B,G,R,A in memory, so B8G8R8A8 is the matching Vulkan
	// format -- not R8G8B8A8. Getting this wrong swaps red and blue everywhere.
	ici.format = VK_FORMAT_B8G8R8A8_UNORM;
	ici.extent = {width, height, 1};
	ici.mipLevels = 1;
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

	Buffer staging;
	const VkDeviceSize bytes = static_cast<VkDeviceSize>(width) * height * 4;
	if (!Upload_Buffer(argb_pixels, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging)) {
		delete handle;
		return nullptr;
	}

	VkCommandBuffer cmd = Begin_One_Shot();
	Transition(cmd, handle->image.image, VK_IMAGE_LAYOUT_UNDEFINED,
	           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
	VkBufferImageCopy copy{};
	copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	copy.imageExtent = {width, height, 1};
	vkCmdCopyBufferToImage(cmd, staging.buffer, handle->image.image,
	                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
	Transition(cmd, handle->image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
	End_One_Shot(cmd);

	vkDestroyBuffer(device_, staging.buffer, nullptr);
	vkFreeMemory(device_, staging.memory, nullptr);

	VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	vci.image = handle->image.image;
	vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vci.format = VK_FORMAT_B8G8R8A8_UNORM;
	vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	if (vkCreateImageView(device_, &vci, nullptr, &handle->image.view) != VK_SUCCESS) {
		delete handle;
		return nullptr;
	}

	owned_textures_.push_back(handle);
	return handle;
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
// state
// ---------------------------------------------------------------------------

void VulkanBackend::Set_DX8_Render_State(D3DRENDERSTATETYPE state, uint32_t value) {
	if (static_cast<uint32_t>(state) < D3DRS_MAX) render_states_[state] = value;
}

void VulkanBackend::Set_DX8_Texture_Stage_State(uint32_t stage,
                                                D3DTEXTURESTAGESTATETYPE state,
                                                uint32_t value) {
	if (stage >= 2) return; // the spike implements 2 of the engine's 8 stages
	PerStage& s = stages_[stage];
	switch (state) {
	case D3DTSS_COLOROP: s.color_op = value; break;
	case D3DTSS_COLORARG1: s.color_arg1 = value; break;
	case D3DTSS_COLORARG2: s.color_arg2 = value; break;
	case D3DTSS_ALPHAOP: s.alpha_op = value; break;
	case D3DTSS_ALPHAARG1: s.alpha_arg1 = value; break;
	case D3DTSS_ALPHAARG2: s.alpha_arg2 = value; break;
	case D3DTSS_TEXCOORDINDEX: s.texcoord_index = value; break;
	case D3DTSS_MINFILTER: s.min_filter = value; break;
	case D3DTSS_MAGFILTER: s.mag_filter = value; break;
	case D3DTSS_MIPFILTER: s.mip_filter = value; break;
	case D3DTSS_ADDRESSU: s.address_u = value; break;
	case D3DTSS_ADDRESSV: s.address_v = value; break;
	default: break; // BUMPENV*, TEXTURETRANSFORMFLAGS, RESULTARG: see the doc
	}
}

void VulkanBackend::Set_Transform(D3DTRANSFORMSTATETYPE transform, const Matrix4x4& m) {
	switch (transform) {
	case D3DTS_WORLD: world_ = m; break;
	case D3DTS_VIEW: view_ = m; break;
	case D3DTS_PROJECTION: projection_ = m; break;
	default: break; // D3DTS_TEXTURE0..3: 46 engine sites, not implemented here
	}
}

void VulkanBackend::Set_Texture(uint32_t stage, TextureHandle* texture) {
	if (stage < 2) bound_textures_[stage] = texture;
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
	sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	sci.maxLod = VK_LOD_CLAMP_NONE;
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
	// backs shader inputs the FVF does not supply.
	VkVertexInputBindingDescription bindings[2]{};
	bindings[0] = {0, layout.stride, VK_VERTEX_INPUT_RATE_VERTEX};
	bindings[1] = {1, 32, VK_VERTEX_INPUT_RATE_VERTEX};

	VkVertexInputAttributeDescription attributes[VA_COUNT]{};
	uint32_t attribute_count = 0;
	static const VkFormat kDummyFormats[VA_COUNT] = {
	    VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT,
	    VK_FORMAT_B8G8R8A8_UNORM,   VK_FORMAT_R32G32_SFLOAT,
	    VK_FORMAT_R32G32_SFLOAT};
	for (uint32_t loc = 0; loc < VA_COUNT; ++loc) {
		if (layout.supplies[loc]) {
			for (uint32_t i = 0; i < layout.attribute_count; ++i) {
				if (layout.attributes[i].location == loc) {
					attributes[attribute_count++] = layout.attributes[i];
					break;
				}
			}
		} else {
			attributes[attribute_count++] = {loc, 1, kDummyFormats[loc], 0};
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
	if (key.z_bias != 0) {
		// D3DRS_ZBIAS is a 0..16 unitless integer that drivers interpreted freely.
		// There is no correct conversion; this is the pragmatic one and it will need
		// per-case tuning against reference screenshots.
		raster.depthBiasEnable = VK_TRUE;
		raster.depthBiasConstantFactor = -static_cast<float>(key.z_bias) * 0.00005f;
	}

	VkPipelineMultisampleStateCreateInfo multisample{
	    VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineDepthStencilStateCreateInfo depth{
	    VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
	depth.depthTestEnable = key.z_enable ? VK_TRUE : VK_FALSE;
	depth.depthWriteEnable = key.z_write_enable ? VK_TRUE : VK_FALSE;
	depth.depthCompareOp = To_Vk_Compare_Op(key.z_func);
	depth.stencilTestEnable = key.stencil_enable ? VK_TRUE : VK_FALSE;

	VkPipelineColorBlendAttachmentState blend{};
	blend.blendEnable = key.alpha_blend_enable ? VK_TRUE : VK_FALSE;
	blend.srcColorBlendFactor = To_Vk_Blend_Factor(key.src_blend);
	blend.dstColorBlendFactor = To_Vk_Blend_Factor(key.dest_blend);
	blend.colorBlendOp = VK_BLEND_OP_ADD;
	blend.srcAlphaBlendFactor = To_Vk_Blend_Factor(key.src_blend);
	blend.dstAlphaBlendFactor = To_Vk_Blend_Factor(key.dest_blend);
	blend.alphaBlendOp = VK_BLEND_OP_ADD;
	blend.colorWriteMask = To_Vk_Color_Write_Mask(key.color_write_enable);

	VkPipelineColorBlendStateCreateInfo blend_state{
	    VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
	blend_state.attachmentCount = 1;
	blend_state.pAttachments = &blend;

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
	vkResetCommandBuffer(frame_cmd_, 0);

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
		clears[count].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
		clears[count].clearValue.depthStencil = {z, stencil};
		++count;
	}
	if (count == 0) return;
	VkClearRect rect{{{0, 0}, {width_, height_}}, 0, 1};
	vkCmdClearAttachments(frame_cmd_, count, clears, 1, &rect);
}

void VulkanBackend::Draw_Triangles(uint32_t start_index, uint32_t polygon_count,
                                   uint32_t min_vertex_index, uint32_t vertex_count) {
	(void)min_vertex_index;
	(void)vertex_count;
	if (!in_scene_ || bound_vb_ == nullptr || bound_ib_ == nullptr) return;
	if (draw_index_ >= kMaxDrawsPerFrame) {
		std::fprintf(stderr, "spike limit: more than %u draws per frame\n", kMaxDrawsPerFrame);
		return;
	}

	// This is DX8Wrapper::Apply_Render_State_Changes' job, moved to draw time
	// because Vulkan has no per-state setters.
	PipelineKey key;
	key.fvf = bound_vb_->fvf;
	key.topology = D3DPT_TRIANGLELIST;
	key.z_enable = render_states_[D3DRS_ZENABLE];
	key.z_write_enable = render_states_[D3DRS_ZWRITEENABLE];
	key.z_func = render_states_[D3DRS_ZFUNC];
	key.cull_mode = render_states_[D3DRS_CULLMODE];
	key.fill_mode = render_states_[D3DRS_FILLMODE];
	key.shade_mode = render_states_[D3DRS_SHADEMODE];
	key.alpha_blend_enable = render_states_[D3DRS_ALPHABLENDENABLE];
	key.src_blend = render_states_[D3DRS_SRCBLEND];
	key.dest_blend = render_states_[D3DRS_DESTBLEND];
	key.color_write_enable = render_states_[D3DRS_COLORWRITEENABLE];
	key.stencil_enable = render_states_[D3DRS_STENCILENABLE];
	key.z_bias = render_states_[D3DRS_ZBIAS];

	VkPipeline pipeline = Get_Or_Create_Pipeline(key, bound_vb_->layout);
	if (pipeline == VK_NULL_HANDLE) return;

	// texture-stage state -> uniform block
	StageUniforms uniforms;
	auto fill = [](const PerStage& s, int32_t* a, int32_t* b) {
		a[0] = static_cast<int32_t>(s.color_op);
		a[1] = static_cast<int32_t>(s.color_arg1);
		a[2] = static_cast<int32_t>(s.color_arg2);
		a[3] = static_cast<int32_t>(s.alpha_op);
		b[0] = static_cast<int32_t>(s.alpha_arg1);
		b[1] = static_cast<int32_t>(s.alpha_arg2);
		b[2] = static_cast<int32_t>(s.texcoord_index);
		b[3] = 0;
	};
	fill(stages_[0], uniforms.stage0, uniforms.stage0b);
	fill(stages_[1], uniforms.stage1, uniforms.stage1b);

	const uint32_t tfactor = render_states_[D3DRS_TEXTUREFACTOR];
	uniforms.tfactor[0] = ((tfactor >> 16) & 0xff) / 255.0f;
	uniforms.tfactor[1] = ((tfactor >> 8) & 0xff) / 255.0f;
	uniforms.tfactor[2] = (tfactor & 0xff) / 255.0f;
	uniforms.tfactor[3] = ((tfactor >> 24) & 0xff) / 255.0f;
	uniforms.flags[0] = static_cast<int32_t>(render_states_[D3DRS_ALPHATESTENABLE]);
	uniforms.flags[1] = static_cast<int32_t>(render_states_[D3DRS_ALPHAFUNC]);
	uniforms.misc[0] = (render_states_[D3DRS_ALPHAREF] & 0xff) / 255.0f;

	const VkDeviceSize ubo_offset = ubo_stride_ * draw_index_;
	void* mapped = nullptr;
	vkMapMemory(device_, stage_uniforms_.memory, ubo_offset, sizeof(StageUniforms), 0, &mapped);
	std::memcpy(mapped, &uniforms, sizeof(StageUniforms));
	vkUnmapMemory(device_, stage_uniforms_.memory);

	VkDescriptorSet set = descriptor_sets_[draw_index_];
	VkDescriptorBufferInfo buffer_info{stage_uniforms_.buffer, ubo_offset, sizeof(StageUniforms)};
	VkDescriptorImageInfo image_info[2]{};
	VkWriteDescriptorSet writes[3]{};
	writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
	writes[0].dstSet = set;
	writes[0].dstBinding = 0;
	writes[0].descriptorCount = 1;
	writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	writes[0].pBufferInfo = &buffer_info;
	for (uint32_t i = 0; i < 2; ++i) {
		TextureHandle* tex = bound_textures_[i] ? bound_textures_[i] : white_texture_;
		const SamplerKey sk{stages_[i].min_filter, stages_[i].mag_filter,
		                    stages_[i].mip_filter, stages_[i].address_u,
		                    stages_[i].address_v};
		image_info[i] = {Get_Or_Create_Sampler(sk), tex->image.view,
		                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
		writes[1 + i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
		writes[1 + i].dstSet = set;
		writes[1 + i].dstBinding = 1 + i;
		writes[1 + i].descriptorCount = 1;
		writes[1 + i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writes[1 + i].pImageInfo = &image_info[i];
	}
	vkUpdateDescriptorSets(device_, 3, writes, 0, nullptr);

	// world * view * projection, then the Vulkan y-flip and the D3D [0,1] ->
	// Vulkan [0,1] depth range (which happens to match, unlike OpenGL's [-1,1]).
	Matrix4x4 wvp = Multiply(Multiply(world_, view_), projection_);
	PushConstants push{};
	for (int i = 0; i < 4; ++i)
		for (int j = 0; j < 4; ++j) {
			// transpose: the engine's matrices are row-vector, GLSL's mat4 is column-major
			push.wvp[j * 4 + i] = wvp.m[i][j] * ((j == 1) ? -1.0f : 1.0f);
		}
	push.flags[0] = bound_vb_->layout.pretransformed ? 1 : 0;
	push.viewport[0] = static_cast<float>(width_);
	push.viewport[1] = static_cast<float>(height_);

	vkCmdBindPipeline(frame_cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	vkCmdPushConstants(frame_cmd_, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT, 0,
	                   sizeof(PushConstants), &push);
	vkCmdBindDescriptorSets(frame_cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_,
	                        0, 1, &set, 0, nullptr);
	VkBuffer vertex_buffers[2] = {bound_vb_->buffer.buffer, dummy_vertex_buffer_.buffer};
	VkDeviceSize offsets[2] = {0, 0};
	vkCmdBindVertexBuffers(frame_cmd_, 0, 2, vertex_buffers, offsets);
	vkCmdBindIndexBuffer(frame_cmd_, bound_ib_->buffer.buffer, 0, VK_INDEX_TYPE_UINT16);
	vkCmdDrawIndexed(frame_cmd_, polygon_count * 3, 1, start_index, index_base_offset_, 0);

	++draw_index_;
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

bool VulkanBackend::Present() {
	if (swapchain_ == VK_NULL_HANDLE) return true;

	uint32_t index = 0;
	VkResult acquired = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
	                                          acquire_semaphore_, VK_NULL_HANDLE, &index);
	if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) return false;

	VkCommandBuffer cmd = Begin_One_Shot();
	if (cmd == VK_NULL_HANDLE) return false;
	Transition(cmd, swapchain_images_[index], VK_IMAGE_LAYOUT_UNDEFINED,
	           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
	VkImageBlit blit{};
	blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	blit.srcOffsets[1] = {static_cast<int32_t>(width_), static_cast<int32_t>(height_), 1};
	blit.dstOffsets[1] = {static_cast<int32_t>(width_), static_cast<int32_t>(height_), 1};
	vkCmdBlitImage(cmd, color_target_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	               swapchain_images_[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
	               VK_FILTER_NEAREST);
	Transition(cmd, swapchain_images_[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	           VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_ASPECT_COLOR_BIT);
	if (!End_One_Shot(cmd)) return false;

	VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
	pi.waitSemaphoreCount = 1;
	pi.pWaitSemaphores = &acquire_semaphore_;
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
