// MoltenVK feature probe: the things the engine needs that the two-draw spike does not cover.
//
// Every case renders to an offscreen target, reads specific pixels back, and compares them
// against a value derived from the case -- so a case can only pass if the GPU actually
// produced the right colour. "Ran without errors" is not a pass here.
//
// Cases, and why each one is here (see docs/porting/renderer-surface.md):
//   depth-test        D3DRS_ZENABLE/ZFUNC/ZWRITEENABLE, 370 render-state sites depend on it
//   alpha-blend       D3DRS_ALPHABLENDENABLE/SRCBLEND/DESTBLEND, the water and shadow passes
//   bc1 / bc3         D3DFMT_DXT1/DXT3/DXT5 -- the shipped asset format
//   bc1-mips          DXT with a mip chain, i.e. what a .dds actually contains
//   two-stage         a second D3DTSS_* stage sampled in one draw (detail/cloud/shroud passes)
//   stencil           D3DRS_STENCILENABLE/FUNC/PASS -- W3DVolumetricShadow's whole method
//   render-target     SetRenderTarget + sample the result (W3DShaderManager, shadows, water)
//   dynamic-buffers   DrawPrimitiveUP's replacement: per-draw suballocation of one host buffer
//
// Deliberately standalone: this must be able to fail without taking the spike down with it.

#define VK_ENABLE_BETA_EXTENSIONS
#include <vulkan/vulkan.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kTargetSize = 256;
constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;

#define PROBE_CHECK(expr)                                                             \
	do {                                                                              \
		VkResult probe_result = (expr);                                               \
		if (probe_result != VK_SUCCESS) {                                             \
			std::fprintf(stderr, "%s:%d: %s -> VkResult %d\n", __FILE__, __LINE__,    \
			             #expr, static_cast<int>(probe_result));                      \
			return false;                                                             \
		}                                                                             \
	} while (0)

struct Vertex {
	float x, y;
	float u, v;
	float r, g, b, a;
};

struct Push {
	float z;
	int32_t mode;
};

enum Mode : int32_t {
	kModeDiffuse = 0,
	kModeTexture0 = 1,
	kModeTwoStageModulate = 2,
	kModeTexture0Lod1 = 3,
};

struct Rgba {
	int r, g, b, a;
};

bool Near(int actual, int expected, int tolerance = 6) {
	return std::abs(actual - expected) <= tolerance;
}

struct Image {
	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;
};

struct Buffer {
	VkBuffer buffer = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	void* mapped = nullptr;
};

// --- BC block builders ------------------------------------------------------
// A BC1 block whose two endpoints are the same colour decodes to that colour for
// every texel, which makes the expected readback exact rather than approximate.

uint16_t Rgb565(int r, int g, int b) {
	return static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

void Append_Bc1_Solid_Block(std::vector<uint8_t>& out, int r, int g, int b) {
	const uint16_t c = Rgb565(r, g, b);
	out.push_back(static_cast<uint8_t>(c & 0xff));
	out.push_back(static_cast<uint8_t>(c >> 8));
	out.push_back(static_cast<uint8_t>(c & 0xff));
	out.push_back(static_cast<uint8_t>(c >> 8));
	for (int i = 0; i < 4; ++i) out.push_back(0); // every texel -> endpoint 0
}

void Append_Bc3_Solid_Block(std::vector<uint8_t>& out, int r, int g, int b, uint8_t alpha) {
	out.push_back(alpha); // alpha0
	out.push_back(alpha); // alpha1 == alpha0 -> constant alpha
	for (int i = 0; i < 6; ++i) out.push_back(0);
	Append_Bc1_Solid_Block(out, r, g, b);
}

std::vector<uint8_t> Bc1_Image(uint32_t width, uint32_t height, int r, int g, int b) {
	std::vector<uint8_t> data;
	const uint32_t blocks = ((width + 3) / 4) * ((height + 3) / 4);
	for (uint32_t i = 0; i < blocks; ++i) Append_Bc1_Solid_Block(data, r, g, b);
	return data;
}

std::vector<uint8_t> Bc3_Image(uint32_t width, uint32_t height, int r, int g, int b,
                               uint8_t alpha) {
	std::vector<uint8_t> data;
	const uint32_t blocks = ((width + 3) / 4) * ((height + 3) / 4);
	for (uint32_t i = 0; i < blocks; ++i) Append_Bc3_Solid_Block(data, r, g, b, alpha);
	return data;
}

VKAPI_ATTR VkBool32 VKAPI_CALL Debug_Callback(VkDebugUtilsMessageSeverityFlagBitsEXT,
                                              VkDebugUtilsMessageTypeFlagsEXT,
                                              const VkDebugUtilsMessengerCallbackDataEXT* data,
                                              void* user) {
	auto* count = static_cast<uint32_t*>(user);
	++(*count);
	std::fprintf(stderr, "  [validation] %s\n", data->pMessage);
	return VK_FALSE;
}

class Probe {
public:
	bool Init(bool validation);
	void Shutdown();
	int Run_Cases();
	uint32_t Validation_Messages() const { return validation_messages_; }
	// Whether the layer really loaded, as opposed to having been asked for: zero messages from
	// an unvalidated instance proves nothing (docs/porting/apple-silicon-verification.md 8.1).
	bool Validation_Active() const {
		return validation_layer_loaded_ && messenger_ != VK_NULL_HANDLE;
	}
	const char* Device_Name() const { return device_name_.c_str(); }
	const char* Depth_Format_Name() const { return depth_format_name_; }

private:
	// setup
	bool Create_Instance(bool validation);
	bool Create_Device();
	bool Create_Targets();
	bool Create_Pipelines();
	bool Create_Static_Resources();

	// helpers
	bool Find_Memory_Type(uint32_t bits, VkMemoryPropertyFlags props, uint32_t& out) const;
	bool Create_Buffer(VkDeviceSize size, VkBufferUsageFlags usage,
	                   VkMemoryPropertyFlags props, Buffer& out) const;
	bool Create_Image(uint32_t width, uint32_t height, uint32_t mip_levels, VkFormat format,
	                  VkImageUsageFlags usage, VkImageAspectFlags aspect, Image& out) const;
	bool Upload_Texture(uint32_t width, uint32_t height, VkFormat format,
	                    const std::vector<std::vector<uint8_t>>& mips, Image& out);
	VkShaderModule Load_Shader(const char* name) const;
	enum class StencilMode { kNone, kWrite, kTestEqual };
	VkPipeline Create_Pipeline(bool depth_test, bool depth_write, bool blend,
	                           StencilMode stencil = StencilMode::kNone) const;
	VkDescriptorSet Make_Descriptor_Set(VkImageView tex0, VkImageView tex1);

	VkCommandBuffer Begin_Commands() const;
	bool End_Commands(VkCommandBuffer cmd) const;
	void Begin_Pass(VkCommandBuffer cmd, VkFramebuffer fb, float clear_r, float clear_g,
	                float clear_b) const;
	void Draw(VkCommandBuffer cmd, VkPipeline pipeline, VkDescriptorSet set, Push push,
	          VkBuffer vb, VkDeviceSize vb_offset, VkBuffer ib, VkDeviceSize ib_offset,
	          uint32_t index_count) const;
	bool Read_Back(std::vector<uint8_t>& out);
	Rgba Pixel(const std::vector<uint8_t>& rgba, uint32_t x, uint32_t y) const;

	// cases
	bool Case_Depth_Test(std::string& detail);
	bool Case_Alpha_Blend(std::string& detail);
	bool Case_Compressed(const char* label, VkFormat format,
	                     const std::vector<std::vector<uint8_t>>& mips, Rgba expected,
	                     int32_t mode, std::string& detail);
	bool Case_Two_Stage(std::string& detail);
	bool Case_Stencil(std::string& detail);
	bool Case_Render_Target(std::string& detail);
	bool Case_Dynamic_Buffers(std::string& detail);

	VkInstance instance_ = VK_NULL_HANDLE;
	VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
	VkPhysicalDevice physical_ = VK_NULL_HANDLE;
	VkDevice device_ = VK_NULL_HANDLE;
	VkQueue queue_ = VK_NULL_HANDLE;
	uint32_t queue_family_ = 0;
	VkCommandPool command_pool_ = VK_NULL_HANDLE;
	VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
	VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
	VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
	VkRenderPass render_pass_ = VK_NULL_HANDLE;
	VkSampler sampler_ = VK_NULL_HANDLE;

	Image color_target_{};
	Image offscreen_target_{};
	Image depth_target_{};
	VkFramebuffer framebuffer_ = VK_NULL_HANDLE;
	VkFramebuffer offscreen_framebuffer_ = VK_NULL_HANDLE;
	VkFormat depth_format_ = VK_FORMAT_UNDEFINED;
	const char* depth_format_name_ = "?";

	VkPipeline pipeline_opaque_ = VK_NULL_HANDLE;
	VkPipeline pipeline_depth_ = VK_NULL_HANDLE;
	VkPipeline pipeline_blend_ = VK_NULL_HANDLE;
	VkPipeline pipeline_stencil_write_ = VK_NULL_HANDLE;
	VkPipeline pipeline_stencil_test_ = VK_NULL_HANDLE;

	Image white_{};
	Image grey_{};
	Buffer quad_vb_{};
	Buffer quad_ib_{};
	Buffer dynamic_{}; // host-visible, suballocated per draw
	Buffer readback_{};

	std::vector<Image> textures_;
	std::string device_name_;
	uint32_t validation_messages_ = 0;
	bool validation_layer_loaded_ = false;
};

// --- setup ------------------------------------------------------------------

bool Probe::Create_Instance(bool validation) {
	uint32_t ext_count = 0;
	vkEnumerateInstanceExtensionProperties(nullptr, &ext_count, nullptr);
	std::vector<VkExtensionProperties> available(ext_count);
	vkEnumerateInstanceExtensionProperties(nullptr, &ext_count, available.data());
	bool portability = false;
	for (const auto& e : available) {
		if (std::strcmp(e.extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0) {
			portability = true;
		}
	}

	std::vector<const char*> extensions;
	VkInstanceCreateFlags flags = 0;
	if (portability) {
		extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
		flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
	}

	std::vector<const char*> layers;
	if (validation) {
		uint32_t layer_count = 0;
		vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
		std::vector<VkLayerProperties> layer_props(layer_count);
		vkEnumerateInstanceLayerProperties(&layer_count, layer_props.data());
		for (const auto& l : layer_props) {
			if (std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0) {
				layers.push_back("VK_LAYER_KHRONOS_validation");
			}
		}
		if (layers.empty()) {
			std::printf("validation layer: absent\n");
			std::fprintf(stderr, "note: validation layer not present, continuing without\n");
		} else {
			std::printf("validation layer: loaded\n");
			validation_layer_loaded_ = true;
			extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
		}
	}

	VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.pApplicationName = "zh-feature-probe";
	app.apiVersion = VK_API_VERSION_1_1;

	VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ci.pApplicationInfo = &app;
	ci.flags = flags;
	ci.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
	ci.ppEnabledExtensionNames = extensions.empty() ? nullptr : extensions.data();
	ci.enabledLayerCount = static_cast<uint32_t>(layers.size());
	ci.ppEnabledLayerNames = layers.empty() ? nullptr : layers.data();
	PROBE_CHECK(vkCreateInstance(&ci, nullptr, &instance_));

	if (!layers.empty()) {
		auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
		    vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
		if (create != nullptr) {
			VkDebugUtilsMessengerCreateInfoEXT dci{
			    VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
			dci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
			                      VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
			dci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
			                  VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
			dci.pfnUserCallback = Debug_Callback;
			dci.pUserData = &validation_messages_;
			create(instance_, &dci, nullptr, &messenger_);
		}
	}
	return true;
}

bool Probe::Create_Device() {
	uint32_t count = 0;
	PROBE_CHECK(vkEnumeratePhysicalDevices(instance_, &count, nullptr));
	if (count == 0) {
		std::fprintf(stderr, "no physical devices\n");
		return false;
	}
	std::vector<VkPhysicalDevice> devices(count);
	PROBE_CHECK(vkEnumeratePhysicalDevices(instance_, &count, devices.data()));
	physical_ = devices[0];

	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(physical_, &props);
	device_name_ = props.deviceName;

	// D3DFMT_D24S8 is the engine's preferred depth-stencil; fall back the way a real
	// backend would have to.
	for (VkFormat candidate : {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT,
	                           VK_FORMAT_D32_SFLOAT}) {
		VkFormatProperties fp{};
		vkGetPhysicalDeviceFormatProperties(physical_, candidate, &fp);
		if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
			depth_format_ = candidate;
			depth_format_name_ = candidate == VK_FORMAT_D24_UNORM_S8_UINT ? "D24_UNORM_S8_UINT"
			                     : candidate == VK_FORMAT_D32_SFLOAT_S8_UINT
			                         ? "D32_SFLOAT_S8_UINT"
			                         : "D32_SFLOAT";
			break;
		}
	}
	if (depth_format_ == VK_FORMAT_UNDEFINED) {
		std::fprintf(stderr, "no usable depth format\n");
		return false;
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
	if (!found) return false;

	uint32_t dev_ext_count = 0;
	vkEnumerateDeviceExtensionProperties(physical_, nullptr, &dev_ext_count, nullptr);
	std::vector<VkExtensionProperties> dev_exts(dev_ext_count);
	vkEnumerateDeviceExtensionProperties(physical_, nullptr, &dev_ext_count, dev_exts.data());
	std::vector<const char*> device_extensions;
	for (const auto& e : dev_exts) {
		if (std::strcmp(e.extensionName, "VK_KHR_portability_subset") == 0) {
			device_extensions.push_back("VK_KHR_portability_subset");
		}
	}

	VkPhysicalDeviceFeatures features{};
	VkPhysicalDeviceFeatures available{};
	vkGetPhysicalDeviceFeatures(physical_, &available);
	features.textureCompressionBC = available.textureCompressionBC;
	features.samplerAnisotropy = available.samplerAnisotropy;

	float priority = 1.0f;
	VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	qci.queueFamilyIndex = queue_family_;
	qci.queueCount = 1;
	qci.pQueuePriorities = &priority;

	VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	dci.pEnabledFeatures = &features;
	dci.enabledExtensionCount = static_cast<uint32_t>(device_extensions.size());
	dci.ppEnabledExtensionNames = device_extensions.empty() ? nullptr : device_extensions.data();
	PROBE_CHECK(vkCreateDevice(physical_, &dci, nullptr, &device_));
	vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

	VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pci.queueFamilyIndex = queue_family_;
	PROBE_CHECK(vkCreateCommandPool(device_, &pci, nullptr, &command_pool_));
	return true;
}

bool Probe::Find_Memory_Type(uint32_t bits, VkMemoryPropertyFlags props, uint32_t& out) const {
	VkPhysicalDeviceMemoryProperties mem;
	vkGetPhysicalDeviceMemoryProperties(physical_, &mem);
	for (uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
		if ((bits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & props) == props) {
			out = i;
			return true;
		}
	}
	return false;
}

bool Probe::Create_Buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                          VkMemoryPropertyFlags props, Buffer& out) const {
	VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bci.size = size;
	bci.usage = usage;
	bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	PROBE_CHECK(vkCreateBuffer(device_, &bci, nullptr, &out.buffer));

	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(device_, out.buffer, &req);
	uint32_t type = 0;
	if (!Find_Memory_Type(req.memoryTypeBits, props, type)) return false;
	VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	mai.allocationSize = req.size;
	mai.memoryTypeIndex = type;
	PROBE_CHECK(vkAllocateMemory(device_, &mai, nullptr, &out.memory));
	PROBE_CHECK(vkBindBufferMemory(device_, out.buffer, out.memory, 0));
	if (props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
		PROBE_CHECK(vkMapMemory(device_, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped));
	}
	return true;
}

bool Probe::Create_Image(uint32_t width, uint32_t height, uint32_t mip_levels, VkFormat format,
                         VkImageUsageFlags usage, VkImageAspectFlags aspect, Image& out) const {
	VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = format;
	ici.extent = {width, height, 1};
	ici.mipLevels = mip_levels;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = usage;
	ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	PROBE_CHECK(vkCreateImage(device_, &ici, nullptr, &out.image));

	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(device_, out.image, &req);
	uint32_t type = 0;
	if (!Find_Memory_Type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, type)) {
		return false;
	}
	VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	mai.allocationSize = req.size;
	mai.memoryTypeIndex = type;
	PROBE_CHECK(vkAllocateMemory(device_, &mai, nullptr, &out.memory));
	PROBE_CHECK(vkBindImageMemory(device_, out.image, out.memory, 0));

	VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	vci.image = out.image;
	vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vci.format = format;
	vci.subresourceRange = {aspect, 0, mip_levels, 0, 1};
	PROBE_CHECK(vkCreateImageView(device_, &vci, nullptr, &out.view));
	return true;
}

VkCommandBuffer Probe::Begin_Commands() const {
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

bool Probe::End_Commands(VkCommandBuffer cmd) const {
	PROBE_CHECK(vkEndCommandBuffer(cmd));
	VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cmd;
	PROBE_CHECK(vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE));
	PROBE_CHECK(vkQueueWaitIdle(queue_));
	vkFreeCommandBuffers(device_, command_pool_, 1, &cmd);
	return true;
}

bool Probe::Upload_Texture(uint32_t width, uint32_t height, VkFormat format,
                           const std::vector<std::vector<uint8_t>>& mips, Image& out) {
	const uint32_t mip_levels = static_cast<uint32_t>(mips.size());
	if (!Create_Image(width, height, mip_levels, format,
	                  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
	                  VK_IMAGE_ASPECT_COLOR_BIT, out)) {
		return false;
	}

	VkDeviceSize total = 0;
	for (const auto& m : mips) total += m.size();
	Buffer staging{};
	if (!Create_Buffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
	                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	                   staging)) {
		return false;
	}
	VkDeviceSize offset = 0;
	std::vector<VkBufferImageCopy> copies;
	for (uint32_t level = 0; level < mip_levels; ++level) {
		std::memcpy(static_cast<uint8_t*>(staging.mapped) + offset, mips[level].data(),
		            mips[level].size());
		VkBufferImageCopy copy{};
		copy.bufferOffset = offset;
		copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
		copy.imageExtent = {width >> level ? width >> level : 1u,
		                    height >> level ? height >> level : 1u, 1};
		copies.push_back(copy);
		offset += mips[level].size();
	}

	VkCommandBuffer cmd = Begin_Commands();
	if (cmd == VK_NULL_HANDLE) return false;
	VkImageMemoryBarrier to_dst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	to_dst.image = out.image;
	to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mip_levels, 0, 1};
	to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                     0, 0, nullptr, 0, nullptr, 1, &to_dst);
	vkCmdCopyBufferToImage(cmd, staging.buffer, out.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                       static_cast<uint32_t>(copies.size()), copies.data());
	VkImageMemoryBarrier to_read = to_dst;
	to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	to_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
	                     &to_read);
	if (!End_Commands(cmd)) return false;

	vkDestroyBuffer(device_, staging.buffer, nullptr);
	vkFreeMemory(device_, staging.memory, nullptr);
	return true;
}

VkShaderModule Probe::Load_Shader(const char* name) const {
	std::string path = std::string(SPIKE_SHADER_DIR) + "/" + name;
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f) {
		std::fprintf(stderr, "cannot open %s\n", path.c_str());
		return VK_NULL_HANDLE;
	}
	std::fseek(f, 0, SEEK_END);
	const long size = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	std::vector<char> code(static_cast<size_t>(size));
	const size_t read = std::fread(code.data(), 1, code.size(), f);
	std::fclose(f);
	if (read != code.size()) return VK_NULL_HANDLE;

	VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	ci.codeSize = code.size();
	ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
	VkShaderModule module = VK_NULL_HANDLE;
	if (vkCreateShaderModule(device_, &ci, nullptr, &module) != VK_SUCCESS) {
		return VK_NULL_HANDLE;
	}
	return module;
}

bool Probe::Create_Targets() {
	if (!Create_Image(kTargetSize, kTargetSize, 1, kColorFormat,
	                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
	                  VK_IMAGE_ASPECT_COLOR_BIT, color_target_)) {
		return false;
	}
	if (!Create_Image(kTargetSize, kTargetSize, 1, kColorFormat,
	                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
	                  VK_IMAGE_ASPECT_COLOR_BIT, offscreen_target_)) {
		return false;
	}
	VkImageAspectFlags depth_aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
	if (depth_format_ != VK_FORMAT_D32_SFLOAT) depth_aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
	if (!Create_Image(kTargetSize, kTargetSize, 1, depth_format_,
	                  VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, depth_aspect, depth_target_)) {
		return false;
	}

	VkAttachmentDescription attachments[2]{};
	attachments[0].format = kColorFormat;
	attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
	attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachments[0].finalLayout = VK_IMAGE_LAYOUT_GENERAL;
	attachments[1] = attachments[0];
	attachments[1].format = depth_format_;
	attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
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
	PROBE_CHECK(vkCreateRenderPass(device_, &rpci, nullptr, &render_pass_));

	auto make_fb = [&](VkImageView color, VkFramebuffer& out) -> bool {
		VkImageView views[2] = {color, depth_target_.view};
		VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
		fci.renderPass = render_pass_;
		fci.attachmentCount = 2;
		fci.pAttachments = views;
		fci.width = kTargetSize;
		fci.height = kTargetSize;
		fci.layers = 1;
		PROBE_CHECK(vkCreateFramebuffer(device_, &fci, nullptr, &out));
		return true;
	};
	if (!make_fb(color_target_.view, framebuffer_)) return false;
	if (!make_fb(offscreen_target_.view, offscreen_framebuffer_)) return false;
	return true;
}

VkPipeline Probe::Create_Pipeline(bool depth_test, bool depth_write, bool blend,
                                  StencilMode stencil) const {
	VkShaderModule vert = Load_Shader("probe.vert.spv");
	VkShaderModule frag = Load_Shader("probe.frag.spv");
	if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE) return VK_NULL_HANDLE;

	VkPipelineShaderStageCreateInfo stages[2]{};
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vert;
	stages[0].pName = "main";
	stages[1] = stages[0];
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = frag;

	VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
	VkVertexInputAttributeDescription attributes[3] = {
	    {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, x)},
	    {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, u)},
	    {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, r)},
	};
	VkPipelineVertexInputStateCreateInfo vertex_input{
	    VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	vertex_input.vertexBindingDescriptionCount = 1;
	vertex_input.pVertexBindingDescriptions = &binding;
	vertex_input.vertexAttributeDescriptionCount = 3;
	vertex_input.pVertexAttributeDescriptions = attributes;

	VkPipelineInputAssemblyStateCreateInfo input_assembly{
	    VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
	input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	VkViewport viewport{0.0f, 0.0f, static_cast<float>(kTargetSize),
	                    static_cast<float>(kTargetSize), 0.0f, 1.0f};
	VkRect2D scissor{{0, 0}, {kTargetSize, kTargetSize}};
	VkPipelineViewportStateCreateInfo viewport_state{
	    VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
	viewport_state.viewportCount = 1;
	viewport_state.pViewports = &viewport;
	viewport_state.scissorCount = 1;
	viewport_state.pScissors = &scissor;

	VkPipelineRasterizationStateCreateInfo raster{
	    VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
	raster.polygonMode = VK_POLYGON_MODE_FILL;
	raster.cullMode = VK_CULL_MODE_NONE;
	raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	raster.lineWidth = 1.0f;

	VkPipelineMultisampleStateCreateInfo multisample{
	    VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineDepthStencilStateCreateInfo depth{
	    VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
	depth.depthTestEnable = depth_test ? VK_TRUE : VK_FALSE;
	depth.depthWriteEnable = depth_write ? VK_TRUE : VK_FALSE;
	depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL; // D3DCMP_LESSEQUAL
	if (stencil != StencilMode::kNone) {
		depth.stencilTestEnable = VK_TRUE;
		VkStencilOpState op{};
		op.compareMask = 0xff;
		op.writeMask = 0xff;
		op.reference = 1;
		if (stencil == StencilMode::kWrite) {
			op.compareOp = VK_COMPARE_OP_ALWAYS; // D3DCMP_ALWAYS
			op.passOp = VK_STENCIL_OP_REPLACE;   // D3DSTENCILOP_REPLACE
		} else {
			op.compareOp = VK_COMPARE_OP_EQUAL; // D3DCMP_EQUAL
			op.passOp = VK_STENCIL_OP_KEEP;     // D3DSTENCILOP_KEEP
			op.writeMask = 0;
		}
		op.failOp = VK_STENCIL_OP_KEEP;
		op.depthFailOp = VK_STENCIL_OP_KEEP;
		depth.front = op;
		depth.back = op;
	}

	VkPipelineColorBlendAttachmentState blend_state{};
	blend_state.blendEnable = blend ? VK_TRUE : VK_FALSE;
	blend_state.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;   // D3DBLEND_SRCALPHA
	blend_state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	blend_state.colorBlendOp = VK_BLEND_OP_ADD;
	blend_state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	blend_state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
	blend_state.alphaBlendOp = VK_BLEND_OP_ADD;
	blend_state.colorWriteMask = stencil == StencilMode::kWrite
	                                 ? 0 // the shadow-volume pass writes stencil only
	                                 : VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
	                                       VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	VkPipelineColorBlendStateCreateInfo blend_ci{
	    VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
	blend_ci.attachmentCount = 1;
	blend_ci.pAttachments = &blend_state;

	VkGraphicsPipelineCreateInfo gpci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
	gpci.stageCount = 2;
	gpci.pStages = stages;
	gpci.pVertexInputState = &vertex_input;
	gpci.pInputAssemblyState = &input_assembly;
	gpci.pViewportState = &viewport_state;
	gpci.pRasterizationState = &raster;
	gpci.pMultisampleState = &multisample;
	gpci.pDepthStencilState = &depth;
	gpci.pColorBlendState = &blend_ci;
	gpci.layout = pipeline_layout_;
	gpci.renderPass = render_pass_;

	VkPipeline pipeline = VK_NULL_HANDLE;
	const VkResult r =
	    vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipeline);
	vkDestroyShaderModule(device_, vert, nullptr);
	vkDestroyShaderModule(device_, frag, nullptr);
	if (r != VK_SUCCESS) {
		std::fprintf(stderr, "vkCreateGraphicsPipelines -> %d\n", static_cast<int>(r));
		return VK_NULL_HANDLE;
	}
	return pipeline;
}

bool Probe::Create_Pipelines() {
	VkDescriptorSetLayoutBinding bindings[2]{};
	for (uint32_t i = 0; i < 2; ++i) {
		bindings[i].binding = i;
		bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		bindings[i].descriptorCount = 1;
		bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	}
	VkDescriptorSetLayoutCreateInfo dslci{
	    VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	dslci.bindingCount = 2;
	dslci.pBindings = bindings;
	PROBE_CHECK(vkCreateDescriptorSetLayout(device_, &dslci, nullptr, &set_layout_));

	VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
	                         sizeof(Push)};
	VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	plci.setLayoutCount = 1;
	plci.pSetLayouts = &set_layout_;
	plci.pushConstantRangeCount = 1;
	plci.pPushConstantRanges = &push;
	PROBE_CHECK(vkCreatePipelineLayout(device_, &plci, nullptr, &pipeline_layout_));

	VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64};
	VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	dpci.maxSets = 32;
	dpci.poolSizeCount = 1;
	dpci.pPoolSizes = &pool_size;
	PROBE_CHECK(vkCreateDescriptorPool(device_, &dpci, nullptr, &descriptor_pool_));

	pipeline_opaque_ = Create_Pipeline(false, false, false);
	pipeline_depth_ = Create_Pipeline(true, true, false);
	pipeline_blend_ = Create_Pipeline(false, false, true);
	pipeline_stencil_write_ = Create_Pipeline(false, false, false, StencilMode::kWrite);
	pipeline_stencil_test_ = Create_Pipeline(false, false, false, StencilMode::kTestEqual);
	return pipeline_opaque_ != VK_NULL_HANDLE && pipeline_depth_ != VK_NULL_HANDLE &&
	       pipeline_blend_ != VK_NULL_HANDLE && pipeline_stencil_write_ != VK_NULL_HANDLE &&
	       pipeline_stencil_test_ != VK_NULL_HANDLE;
}

bool Probe::Create_Static_Resources() {
	VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	sci.magFilter = VK_FILTER_LINEAR;
	sci.minFilter = VK_FILTER_LINEAR;
	sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sci.maxLod = VK_LOD_CLAMP_NONE;
	PROBE_CHECK(vkCreateSampler(device_, &sci, nullptr, &sampler_));

	std::vector<uint8_t> white(4 * 4 * 4, 255);
	if (!Upload_Texture(4, 4, VK_FORMAT_R8G8B8A8_UNORM, {white}, white_)) return false;
	std::vector<uint8_t> grey(4 * 4 * 4, 128);
	for (size_t i = 3; i < grey.size(); i += 4) grey[i] = 255;
	if (!Upload_Texture(4, 4, VK_FORMAT_R8G8B8A8_UNORM, {grey}, grey_)) return false;

	// Full-target quad, and a second quad in the bottom-right, both from one static buffer.
	const Vertex quad[4] = {
	    {-1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f},
	    {1.0f, -1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f},
	    {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
	    {-1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
	};
	const uint16_t indices[6] = {0, 1, 2, 0, 2, 3};
	if (!Create_Buffer(sizeof(quad), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
	                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	                   quad_vb_)) {
		return false;
	}
	std::memcpy(quad_vb_.mapped, quad, sizeof(quad));
	if (!Create_Buffer(sizeof(indices), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
	                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	                   quad_ib_)) {
		return false;
	}
	std::memcpy(quad_ib_.mapped, indices, sizeof(indices));

	// The DrawPrimitiveUP replacement: one host-visible ring, suballocated per draw.
	if (!Create_Buffer(64 * 1024,
	                   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
	                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	                   dynamic_)) {
		return false;
	}

	if (!Create_Buffer(static_cast<VkDeviceSize>(kTargetSize) * kTargetSize * 4,
	                   VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	                   readback_)) {
		return false;
	}
	return true;
}

bool Probe::Init(bool validation) {
	return Create_Instance(validation) && Create_Device() && Create_Targets() &&
	       Create_Pipelines() && Create_Static_Resources();
}

VkDescriptorSet Probe::Make_Descriptor_Set(VkImageView tex0, VkImageView tex1) {
	VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	ai.descriptorPool = descriptor_pool_;
	ai.descriptorSetCount = 1;
	ai.pSetLayouts = &set_layout_;
	VkDescriptorSet set = VK_NULL_HANDLE;
	if (vkAllocateDescriptorSets(device_, &ai, &set) != VK_SUCCESS) return VK_NULL_HANDLE;

	VkDescriptorImageInfo images[2]{};
	images[0] = {sampler_, tex0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
	images[1] = {sampler_, tex1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
	VkWriteDescriptorSet writes[2]{};
	for (uint32_t i = 0; i < 2; ++i) {
		writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i].dstSet = set;
		writes[i].dstBinding = i;
		writes[i].descriptorCount = 1;
		writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writes[i].pImageInfo = &images[i];
	}
	vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);
	return set;
}

void Probe::Begin_Pass(VkCommandBuffer cmd, VkFramebuffer fb, float clear_r, float clear_g,
                       float clear_b) const {
	VkClearValue clears[2]{};
	clears[0].color = {{clear_r, clear_g, clear_b, 1.0f}};
	clears[1].depthStencil = {1.0f, 0};
	VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
	bi.renderPass = render_pass_;
	bi.framebuffer = fb;
	bi.renderArea = {{0, 0}, {kTargetSize, kTargetSize}};
	bi.clearValueCount = 2;
	bi.pClearValues = clears;
	vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
}

void Probe::Draw(VkCommandBuffer cmd, VkPipeline pipeline, VkDescriptorSet set, Push push,
                 VkBuffer vb, VkDeviceSize vb_offset, VkBuffer ib, VkDeviceSize ib_offset,
                 uint32_t index_count) const {
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &set,
	                        0, nullptr);
	vkCmdPushConstants(cmd, pipeline_layout_,
	                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
	                   sizeof(Push), &push);
	vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &vb_offset);
	vkCmdBindIndexBuffer(cmd, ib, ib_offset, VK_INDEX_TYPE_UINT16);
	vkCmdDrawIndexed(cmd, index_count, 1, 0, 0, 0);
}

bool Probe::Read_Back(std::vector<uint8_t>& out) {
	VkCommandBuffer cmd = Begin_Commands();
	if (cmd == VK_NULL_HANDLE) return false;
	VkBufferImageCopy copy{};
	copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	copy.imageExtent = {kTargetSize, kTargetSize, 1};
	vkCmdCopyImageToBuffer(cmd, color_target_.image, VK_IMAGE_LAYOUT_GENERAL, readback_.buffer,
	                       1, &copy);
	if (!End_Commands(cmd)) return false;
	out.resize(static_cast<size_t>(kTargetSize) * kTargetSize * 4);
	std::memcpy(out.data(), readback_.mapped, out.size());
	return true;
}

Rgba Probe::Pixel(const std::vector<uint8_t>& rgba, uint32_t x, uint32_t y) const {
	const size_t i = (static_cast<size_t>(y) * kTargetSize + x) * 4;
	return {rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]};
}

// --- cases ------------------------------------------------------------------

bool Probe::Case_Depth_Test(std::string& detail) {
	// Near blue quad over the left half, then a far red quad over everything. With
	// D3DRS_ZFUNC=LESSEQUAL and depth writes on, the red must lose where blue is.
	Vertex* v = static_cast<Vertex*>(dynamic_.mapped);
	auto put_quad = [&](int base, float x0, float x1, float r, float g, float b) {
		v[base + 0] = {x0, -1.0f, 0.0f, 0.0f, r, g, b, 1.0f};
		v[base + 1] = {x1, -1.0f, 1.0f, 0.0f, r, g, b, 1.0f};
		v[base + 2] = {x1, 1.0f, 1.0f, 1.0f, r, g, b, 1.0f};
		v[base + 3] = {x0, 1.0f, 0.0f, 1.0f, r, g, b, 1.0f};
	};
	put_quad(0, -1.0f, 0.0f, 0.0f, 0.0f, 1.0f); // near, left half, blue
	put_quad(4, -1.0f, 1.0f, 1.0f, 0.0f, 0.0f); // far, whole target, red
	uint16_t* idx = reinterpret_cast<uint16_t*>(static_cast<uint8_t*>(dynamic_.mapped) + 4096);
	const uint16_t pattern[12] = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};
	std::memcpy(idx, pattern, sizeof(pattern));

	VkDescriptorSet set = Make_Descriptor_Set(white_.view, white_.view);
	VkCommandBuffer cmd = Begin_Commands();
	if (cmd == VK_NULL_HANDLE || set == VK_NULL_HANDLE) return false;
	Begin_Pass(cmd, framebuffer_, 0.0f, 0.0f, 0.0f);
	Draw(cmd, pipeline_depth_, set, {0.3f, kModeDiffuse}, dynamic_.buffer, 0, dynamic_.buffer,
	     4096, 6);
	Draw(cmd, pipeline_depth_, set, {0.7f, kModeDiffuse}, dynamic_.buffer, 0, dynamic_.buffer,
	     4096 + 6 * sizeof(uint16_t), 6);
	vkCmdEndRenderPass(cmd);
	if (!End_Commands(cmd)) return false;

	std::vector<uint8_t> pixels;
	if (!Read_Back(pixels)) return false;
	const Rgba left = Pixel(pixels, 64, 128);
	const Rgba right = Pixel(pixels, 192, 128);
	char buf[192];
	std::snprintf(buf, sizeof(buf), "near-left=(%d,%d,%d) far-right=(%d,%d,%d)", left.r, left.g,
	              left.b, right.r, right.g, right.b);
	detail = buf;
	return Near(left.b, 255) && Near(left.r, 0) && Near(right.r, 255) && Near(right.b, 0);
}

bool Probe::Case_Alpha_Blend(std::string& detail) {
	// Opaque red, then white at alpha 0.5 with SRCALPHA/INVSRCALPHA -> (255,128,128).
	Vertex* v = static_cast<Vertex*>(dynamic_.mapped);
	for (int i = 0; i < 4; ++i) {
		const float xs[4] = {-1.0f, 1.0f, 1.0f, -1.0f};
		const float ys[4] = {-1.0f, -1.0f, 1.0f, 1.0f};
		v[i] = {xs[i], ys[i], 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f};
		v[4 + i] = {xs[i], ys[i], 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 0.5f};
	}
	uint16_t* idx = reinterpret_cast<uint16_t*>(static_cast<uint8_t*>(dynamic_.mapped) + 4096);
	const uint16_t pattern[12] = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};
	std::memcpy(idx, pattern, sizeof(pattern));

	VkDescriptorSet set = Make_Descriptor_Set(white_.view, white_.view);
	VkCommandBuffer cmd = Begin_Commands();
	if (cmd == VK_NULL_HANDLE || set == VK_NULL_HANDLE) return false;
	Begin_Pass(cmd, framebuffer_, 0.0f, 0.0f, 0.0f);
	Draw(cmd, pipeline_opaque_, set, {0.5f, kModeDiffuse}, dynamic_.buffer, 0, dynamic_.buffer,
	     4096, 6);
	Draw(cmd, pipeline_blend_, set, {0.5f, kModeDiffuse}, dynamic_.buffer, 0, dynamic_.buffer,
	     4096 + 6 * sizeof(uint16_t), 6);
	vkCmdEndRenderPass(cmd);
	if (!End_Commands(cmd)) return false;

	std::vector<uint8_t> pixels;
	if (!Read_Back(pixels)) return false;
	const Rgba p = Pixel(pixels, 128, 128);
	char buf[128];
	std::snprintf(buf, sizeof(buf), "blended=(%d,%d,%d) expected=(255,128,128)", p.r, p.g, p.b);
	detail = buf;
	return Near(p.r, 255) && Near(p.g, 128) && Near(p.b, 128);
}

bool Probe::Case_Compressed(const char* label, VkFormat format,
                            const std::vector<std::vector<uint8_t>>& mips, Rgba expected,
                            int32_t mode, std::string& detail) {
	VkFormatProperties fp{};
	vkGetPhysicalDeviceFormatProperties(physical_, format, &fp);
	if ((fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0) {
		detail = "format not sampleable on this device";
		return false;
	}

	Image texture{};
	if (!Upload_Texture(64, 64, format, mips, texture)) {
		detail = "upload failed";
		return false;
	}
	textures_.push_back(texture);

	VkDescriptorSet set = Make_Descriptor_Set(texture.view, white_.view);
	VkCommandBuffer cmd = Begin_Commands();
	if (cmd == VK_NULL_HANDLE || set == VK_NULL_HANDLE) return false;
	Begin_Pass(cmd, framebuffer_, 0.0f, 0.0f, 0.0f);
	Draw(cmd, pipeline_opaque_, set, {0.5f, mode}, quad_vb_.buffer, 0, quad_ib_.buffer, 0, 6);
	vkCmdEndRenderPass(cmd);
	if (!End_Commands(cmd)) return false;

	std::vector<uint8_t> pixels;
	if (!Read_Back(pixels)) return false;
	const Rgba p = Pixel(pixels, 128, 128);
	char buf[192];
	std::snprintf(buf, sizeof(buf), "%s sampled=(%d,%d,%d,%d) expected=(%d,%d,%d,%d)", label,
	              p.r, p.g, p.b, p.a, expected.r, expected.g, expected.b, expected.a);
	detail = buf;
	return Near(p.r, expected.r) && Near(p.g, expected.g) && Near(p.b, expected.b) &&
	       Near(p.a, expected.a);
}

bool Probe::Case_Two_Stage(std::string& detail) {
	// stage0 = BC1 red, stage1 = 50% grey, modulated: D3DTOP_MODULATE across two stages.
	Image red{};
	if (!Upload_Texture(64, 64, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, {Bc1_Image(64, 64, 255, 0, 0)},
	                    red)) {
		detail = "BC1 upload failed";
		return false;
	}
	textures_.push_back(red);

	VkDescriptorSet set = Make_Descriptor_Set(red.view, grey_.view);
	VkCommandBuffer cmd = Begin_Commands();
	if (cmd == VK_NULL_HANDLE || set == VK_NULL_HANDLE) return false;
	Begin_Pass(cmd, framebuffer_, 0.0f, 0.0f, 0.0f);
	Draw(cmd, pipeline_opaque_, set, {0.5f, kModeTwoStageModulate}, quad_vb_.buffer, 0,
	     quad_ib_.buffer, 0, 6);
	vkCmdEndRenderPass(cmd);
	if (!End_Commands(cmd)) return false;

	std::vector<uint8_t> pixels;
	if (!Read_Back(pixels)) return false;
	const Rgba p = Pixel(pixels, 128, 128);
	char buf[128];
	std::snprintf(buf, sizeof(buf), "modulated=(%d,%d,%d) expected=(128,0,0)", p.r, p.g, p.b);
	detail = buf;
	return Near(p.r, 128) && Near(p.g, 0) && Near(p.b, 0);
}

bool Probe::Case_Stencil(std::string& detail) {
	// The shadow-volume pattern: a colour-masked pass stamps stencil=1 over the left half,
	// then a full-target pass is masked to where stencil==1.
	Vertex* v = static_cast<Vertex*>(dynamic_.mapped);
	auto put_quad = [&](int base, float x0, float x1, float r, float g, float b) {
		v[base + 0] = {x0, -1.0f, 0.0f, 0.0f, r, g, b, 1.0f};
		v[base + 1] = {x1, -1.0f, 1.0f, 0.0f, r, g, b, 1.0f};
		v[base + 2] = {x1, 1.0f, 1.0f, 1.0f, r, g, b, 1.0f};
		v[base + 3] = {x0, 1.0f, 0.0f, 1.0f, r, g, b, 1.0f};
	};
	put_quad(0, -1.0f, 0.0f, 1.0f, 1.0f, 1.0f);
	put_quad(4, -1.0f, 1.0f, 0.0f, 1.0f, 1.0f);
	uint16_t* idx = reinterpret_cast<uint16_t*>(static_cast<uint8_t*>(dynamic_.mapped) + 4096);
	const uint16_t pattern[12] = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};
	std::memcpy(idx, pattern, sizeof(pattern));

	VkDescriptorSet set = Make_Descriptor_Set(white_.view, white_.view);
	VkCommandBuffer cmd = Begin_Commands();
	if (cmd == VK_NULL_HANDLE || set == VK_NULL_HANDLE) return false;
	Begin_Pass(cmd, framebuffer_, 0.0f, 0.0f, 0.0f);
	Draw(cmd, pipeline_stencil_write_, set, {0.5f, kModeDiffuse}, dynamic_.buffer, 0,
	     dynamic_.buffer, 4096, 6);
	Draw(cmd, pipeline_stencil_test_, set, {0.5f, kModeDiffuse}, dynamic_.buffer, 0,
	     dynamic_.buffer, 4096 + 6 * sizeof(uint16_t), 6);
	vkCmdEndRenderPass(cmd);
	if (!End_Commands(cmd)) return false;

	std::vector<uint8_t> pixels;
	if (!Read_Back(pixels)) return false;
	const Rgba inside = Pixel(pixels, 64, 128);
	const Rgba outside = Pixel(pixels, 192, 128);
	char buf[192];
	std::snprintf(buf, sizeof(buf), "stencil==1=(%d,%d,%d) stencil==0=(%d,%d,%d)", inside.r,
	              inside.g, inside.b, outside.r, outside.g, outside.b);
	detail = buf;
	return Near(inside.g, 255) && Near(inside.b, 255) && Near(inside.r, 0) &&
	       Near(outside.g, 0) && Near(outside.b, 0);
}

bool Probe::Case_Render_Target(std::string& detail) {
	// Pass 1 draws magenta into an offscreen colour target; pass 2 samples it into the
	// main target. This is SetRenderTarget + render-to-texture, which the spike lacks.
	VkDescriptorSet white_set = Make_Descriptor_Set(white_.view, white_.view);
	VkDescriptorSet rt_set = Make_Descriptor_Set(offscreen_target_.view, white_.view);
	if (white_set == VK_NULL_HANDLE || rt_set == VK_NULL_HANDLE) return false;

	Vertex* v = static_cast<Vertex*>(dynamic_.mapped);
	const float xs[4] = {-1.0f, 1.0f, 1.0f, -1.0f};
	const float ys[4] = {-1.0f, -1.0f, 1.0f, 1.0f};
	const float us[4] = {0.0f, 1.0f, 1.0f, 0.0f};
	const float vs[4] = {0.0f, 0.0f, 1.0f, 1.0f};
	for (int i = 0; i < 4; ++i) v[i] = {xs[i], ys[i], us[i], vs[i], 1.0f, 0.0f, 1.0f, 1.0f};
	uint16_t* idx = reinterpret_cast<uint16_t*>(static_cast<uint8_t*>(dynamic_.mapped) + 4096);
	const uint16_t pattern[6] = {0, 1, 2, 0, 2, 3};
	std::memcpy(idx, pattern, sizeof(pattern));

	VkCommandBuffer cmd = Begin_Commands();
	if (cmd == VK_NULL_HANDLE) return false;
	Begin_Pass(cmd, offscreen_framebuffer_, 0.0f, 0.0f, 0.0f);
	Draw(cmd, pipeline_opaque_, white_set, {0.5f, kModeDiffuse}, dynamic_.buffer, 0,
	     dynamic_.buffer, 4096, 6);
	vkCmdEndRenderPass(cmd);

	VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = offscreen_target_.image;
	barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
	                     &barrier);

	Begin_Pass(cmd, framebuffer_, 0.0f, 0.0f, 0.0f);
	Draw(cmd, pipeline_opaque_, rt_set, {0.5f, kModeTexture0}, quad_vb_.buffer, 0,
	     quad_ib_.buffer, 0, 6);
	vkCmdEndRenderPass(cmd);
	if (!End_Commands(cmd)) return false;

	std::vector<uint8_t> pixels;
	if (!Read_Back(pixels)) return false;
	const Rgba p = Pixel(pixels, 128, 128);
	char buf[160];
	std::snprintf(buf, sizeof(buf), "sampled-from-RT=(%d,%d,%d) expected=(255,0,255)", p.r, p.g,
	              p.b);
	detail = buf;
	return Near(p.r, 255) && Near(p.g, 0) && Near(p.b, 255);
}

bool Probe::Case_Dynamic_Buffers(std::string& detail) {
	// Two draws in one pass from one host-visible buffer at different offsets, which is
	// what the engine's 11 DrawPrimitiveUP sites have to become.
	auto* base = static_cast<uint8_t*>(dynamic_.mapped);
	auto write_quad = [&](size_t byte_offset, float x0, float x1, float y0, float y1, float r,
	                      float g, float b) {
		Vertex quad[4] = {
		    {x0, y0, 0.0f, 0.0f, r, g, b, 1.0f},
		    {x1, y0, 1.0f, 0.0f, r, g, b, 1.0f},
		    {x1, y1, 1.0f, 1.0f, r, g, b, 1.0f},
		    {x0, y1, 0.0f, 1.0f, r, g, b, 1.0f},
		};
		std::memcpy(base + byte_offset, quad, sizeof(quad));
	};
	write_quad(0, -0.9f, -0.1f, -0.9f, -0.1f, 0.0f, 1.0f, 0.0f);
	write_quad(1024, 0.1f, 0.9f, 0.1f, 0.9f, 1.0f, 1.0f, 0.0f);
	const uint16_t pattern[6] = {0, 1, 2, 0, 2, 3};
	std::memcpy(base + 4096, pattern, sizeof(pattern));

	VkDescriptorSet set = Make_Descriptor_Set(white_.view, white_.view);
	VkCommandBuffer cmd = Begin_Commands();
	if (cmd == VK_NULL_HANDLE || set == VK_NULL_HANDLE) return false;
	Begin_Pass(cmd, framebuffer_, 0.0f, 0.0f, 0.0f);
	Draw(cmd, pipeline_opaque_, set, {0.5f, kModeDiffuse}, dynamic_.buffer, 0, dynamic_.buffer,
	     4096, 6);
	Draw(cmd, pipeline_opaque_, set, {0.5f, kModeDiffuse}, dynamic_.buffer, 1024,
	     dynamic_.buffer, 4096, 6);
	vkCmdEndRenderPass(cmd);
	if (!End_Commands(cmd)) return false;

	std::vector<uint8_t> pixels;
	if (!Read_Back(pixels)) return false;
	// y is flipped relative to NDC: the first quad lands top-left in the readback.
	const Rgba first = Pixel(pixels, 64, 64);
	const Rgba second = Pixel(pixels, 192, 192);
	char buf[192];
	std::snprintf(buf, sizeof(buf), "suballoc0=(%d,%d,%d) suballoc1=(%d,%d,%d)", first.r,
	              first.g, first.b, second.r, second.g, second.b);
	detail = buf;
	return Near(first.g, 255) && Near(first.r, 0) && Near(second.r, 255) &&
	       Near(second.g, 255);
}

int Probe::Run_Cases() {
	struct Result {
		const char* name;
		bool passed;
		std::string detail;
	};
	std::vector<Result> results;

	auto run = [&](const char* name, bool (Probe::*fn)(std::string&)) {
		std::string detail;
		const bool ok = (this->*fn)(detail);
		results.push_back({name, ok, detail});
	};

	run("depth test (D3DRS_ZENABLE/ZFUNC)", &Probe::Case_Depth_Test);
	run("alpha blend (SRCALPHA/INVSRCALPHA)", &Probe::Case_Alpha_Blend);

	{
		std::string detail;
		const bool ok = Case_Compressed("DXT1", VK_FORMAT_BC1_RGBA_UNORM_BLOCK,
		                                {Bc1_Image(64, 64, 255, 0, 0)}, {255, 0, 0, 255},
		                                kModeTexture0, detail);
		results.push_back({"BC1 / D3DFMT_DXT1 sampled", ok, detail});
	}
	{
		std::string detail;
		const bool ok = Case_Compressed("DXT5", VK_FORMAT_BC3_UNORM_BLOCK,
		                                {Bc3_Image(64, 64, 0, 255, 0, 128)}, {0, 255, 0, 128},
		                                kModeTexture0, detail);
		results.push_back({"BC3 / D3DFMT_DXT5 sampled (incl. alpha)", ok, detail});
	}
	{
		// A DXT mip chain, i.e. what a .dds file actually holds: mip0 red, mip1 blue,
		// sampled with an explicit LOD of 1 so only mip1 can answer.
		std::vector<std::vector<uint8_t>> mips = {Bc1_Image(64, 64, 255, 0, 0),
		                                          Bc1_Image(32, 32, 0, 0, 255),
		                                          Bc1_Image(16, 16, 0, 0, 255)};
		std::string detail;
		const bool ok = Case_Compressed("DXT1 mip1", VK_FORMAT_BC1_RGBA_UNORM_BLOCK, mips,
		                                {0, 0, 255, 255}, kModeTexture0Lod1, detail);
		results.push_back({"BC1 mip chain (explicit LOD)", ok, detail});
	}

	run("two texture stages in one draw", &Probe::Case_Two_Stage);
	run("stencil write + test (shadow volumes)", &Probe::Case_Stencil);
	run("render target + sample the result", &Probe::Case_Render_Target);
	run("dynamic buffer suballocation", &Probe::Case_Dynamic_Buffers);

	std::printf("\n== functional cases ==\n");
	int failures = 0;
	for (const auto& r : results) {
		std::printf("  %-42s %-4s %s\n", r.name, r.passed ? "PASS" : "FAIL", r.detail.c_str());
		if (!r.passed) ++failures;
	}
	return failures;
}

void Probe::Shutdown() {
	if (device_ == VK_NULL_HANDLE) return;
	vkDeviceWaitIdle(device_);
	for (auto& t : textures_) {
		vkDestroyImageView(device_, t.view, nullptr);
		vkDestroyImage(device_, t.image, nullptr);
		vkFreeMemory(device_, t.memory, nullptr);
	}
	for (Image* image : {&white_, &grey_, &color_target_, &offscreen_target_, &depth_target_}) {
		vkDestroyImageView(device_, image->view, nullptr);
		vkDestroyImage(device_, image->image, nullptr);
		vkFreeMemory(device_, image->memory, nullptr);
	}
	for (Buffer* buffer : {&quad_vb_, &quad_ib_, &dynamic_, &readback_}) {
		vkDestroyBuffer(device_, buffer->buffer, nullptr);
		vkFreeMemory(device_, buffer->memory, nullptr);
	}
	for (VkPipeline p : {pipeline_opaque_, pipeline_depth_, pipeline_blend_,
	                     pipeline_stencil_write_, pipeline_stencil_test_}) {
		vkDestroyPipeline(device_, p, nullptr);
	}
	vkDestroySampler(device_, sampler_, nullptr);
	vkDestroyFramebuffer(device_, framebuffer_, nullptr);
	vkDestroyFramebuffer(device_, offscreen_framebuffer_, nullptr);
	vkDestroyRenderPass(device_, render_pass_, nullptr);
	vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
	vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
	vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
	vkDestroyCommandPool(device_, command_pool_, nullptr);
	vkDestroyDevice(device_, nullptr);
	if (messenger_ != VK_NULL_HANDLE) {
		auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
		    vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
		if (destroy != nullptr) destroy(instance_, messenger_, nullptr);
	}
	vkDestroyInstance(instance_, nullptr);
	device_ = VK_NULL_HANDLE;
}

} // namespace

int main(int argc, char** argv) {
	bool validation = true;
	for (int i = 1; i < argc; ++i) {
		if (std::strcmp(argv[i], "--no-validation") == 0) validation = false;
	}

	Probe probe;
	if (!probe.Init(validation)) {
		std::fprintf(stderr, "probe init failed\n");
		return 1;
	}
	std::printf("device: %s\n", probe.Device_Name());
	std::printf("depth-stencil format chosen: %s\n", probe.Depth_Format_Name());

	int failures = probe.Run_Cases();
	if (validation && !probe.Validation_Active()) {
		std::fprintf(stderr, "\nFAIL: validation was requested but no layer was loaded\n");
		++failures;
	}
	std::printf("\nvalidation messages: %u\n", probe.Validation_Messages());
	std::printf("%d case(s) failed\n", failures);
	probe.Shutdown();
	return failures == 0 ? 0 : 1;
}
