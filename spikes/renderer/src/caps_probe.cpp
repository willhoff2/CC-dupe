// MoltenVK capability probe.
//
// Answers the questions the two-draw rendering spike cannot: which of the D3D8
// formats, features and limits the Zero Hour engine actually depends on survive
// the trip through MoltenVK/Metal. Queries only -- no rendering, no window.
//
// Every D3DFMT_* / feature listed here was taken from docs/porting/renderer-surface.md
// or measured out of the engine with tools/d3d8-surface-scan.py.

#define VK_ENABLE_BETA_EXTENSIONS
#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct FormatRow {
	const char* d3d_name;
	const char* vk_name;
	VkFormat format;
	const char* note;
};

// The D3DFMT_* values the engine names, mapped to their nearest Vulkan format.
const FormatRow kFormats[] = {
    // colour / texture
    {"D3DFMT_A8R8G8B8", "B8G8R8A8_UNORM", VK_FORMAT_B8G8R8A8_UNORM, "primary 32-bit texture+RT format"},
    {"D3DFMT_X8R8G8B8", "B8G8R8A8_UNORM", VK_FORMAT_B8G8R8A8_UNORM, "same memory layout, alpha ignored"},
    {"D3DFMT_R5G6B5", "R5G6B5_UNORM_PACK16", VK_FORMAT_R5G6B5_UNORM_PACK16, "16-bit colour path"},
    {"D3DFMT_A1R5G5B5", "A1R5G5B5_UNORM_PACK16", VK_FORMAT_A1R5G5B5_UNORM_PACK16, "16-bit with 1-bit alpha"},
    {"D3DFMT_A4R4G4B4", "B4G4R4A4_UNORM_PACK16", VK_FORMAT_B4G4R4A4_UNORM_PACK16, "16-bit with 4-bit alpha"},
    {"D3DFMT_L8", "R8_UNORM", VK_FORMAT_R8_UNORM, "luminance; needs a shader swizzle"},
    {"D3DFMT_A8", "R8_UNORM", VK_FORMAT_R8_UNORM, "alpha-only; needs a shader swizzle"},
    {"D3DFMT_A8L8", "R8G8_UNORM", VK_FORMAT_R8G8_UNORM, "needs a shader swizzle"},
    {"D3DFMT_V8U8", "R8G8_SNORM", VK_FORMAT_R8G8_SNORM, "bump map"},
    {"D3DFMT_X8L8V8U8", "R8G8B8A8_SNORM", VK_FORMAT_R8G8B8A8_SNORM, "bump map, mixed signedness in D3D"},
    // compressed -- the asset format
    {"D3DFMT_DXT1", "BC1_RGBA_UNORM_BLOCK", VK_FORMAT_BC1_RGBA_UNORM_BLOCK, "the game's bulk texture format"},
    {"D3DFMT_DXT2", "BC2_UNORM_BLOCK", VK_FORMAT_BC2_UNORM_BLOCK, "premultiplied DXT3"},
    {"D3DFMT_DXT3", "BC2_UNORM_BLOCK", VK_FORMAT_BC2_UNORM_BLOCK, "explicit alpha"},
    {"D3DFMT_DXT4", "BC3_UNORM_BLOCK", VK_FORMAT_BC3_UNORM_BLOCK, "premultiplied DXT5"},
    {"D3DFMT_DXT5", "BC3_UNORM_BLOCK", VK_FORMAT_BC3_UNORM_BLOCK, "interpolated alpha"},
    // depth/stencil
    {"D3DFMT_D16", "D16_UNORM", VK_FORMAT_D16_UNORM, ""},
    {"D3DFMT_D24S8", "D24_UNORM_S8_UINT", VK_FORMAT_D24_UNORM_S8_UINT, "engine's preferred depth-stencil"},
    {"D3DFMT_D24X8", "X8_D24_UNORM_PACK32", VK_FORMAT_X8_D24_UNORM_PACK32, ""},
    {"D3DFMT_D32", "D32_SFLOAT", VK_FORMAT_D32_SFLOAT, ""},
    {"(D24S8 fallback)", "D32_SFLOAT_S8_UINT", VK_FORMAT_D32_SFLOAT_S8_UINT, "fallback if D24S8 absent"},
    // Apple-native compressed formats, for the transcode question
    {"(Apple native)", "ASTC_4x4_UNORM_BLOCK", VK_FORMAT_ASTC_4x4_UNORM_BLOCK, "transcode target candidate"},
    {"(Apple native)", "ETC2_R8G8B8A8_UNORM", VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK, "transcode target candidate"},
};

std::string Feature_Flags(VkFormatFeatureFlags f) {
	std::string s;
	auto add = [&](VkFormatFeatureFlags bit, const char* name) {
		if (f & bit) {
			if (!s.empty()) s += "+";
			s += name;
		}
	};
	add(VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT, "sampled");
	add(VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT, "linear");
	add(VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT, "colour-rt");
	add(VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT, "blend");
	add(VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT, "depth-rt");
	add(VK_FORMAT_FEATURE_BLIT_SRC_BIT, "blit-src");
	add(VK_FORMAT_FEATURE_BLIT_DST_BIT, "blit-dst");
	if (s.empty()) s = "-";
	return s;
}

bool Has_Extension(const std::vector<VkExtensionProperties>& list, const char* name) {
	for (const auto& e : list) {
		if (std::strcmp(e.extensionName, name) == 0) return true;
	}
	return false;
}

void Print_Bool(const char* name, VkBool32 value) {
	std::printf("  %-42s %s\n", name, value ? "yes" : "NO");
}

} // namespace

int main() {
	uint32_t instance_version = 0;
	vkEnumerateInstanceVersion(&instance_version);
	std::printf("loader instance version: %u.%u.%u\n", VK_VERSION_MAJOR(instance_version),
	            VK_VERSION_MINOR(instance_version), VK_VERSION_PATCH(instance_version));

	uint32_t ext_count = 0;
	vkEnumerateInstanceExtensionProperties(nullptr, &ext_count, nullptr);
	std::vector<VkExtensionProperties> instance_exts(ext_count);
	vkEnumerateInstanceExtensionProperties(nullptr, &ext_count, instance_exts.data());
	const bool portability =
	    Has_Extension(instance_exts, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
	std::printf("VK_KHR_portability_enumeration advertised: %s\n", portability ? "yes" : "no");

	std::vector<const char*> extensions{VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME};
	VkInstanceCreateFlags flags = 0;
	if (portability) {
		extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
		flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
	}

	VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.pApplicationName = "zh-caps-probe";
	app.apiVersion = VK_API_VERSION_1_1;

	VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ci.pApplicationInfo = &app;
	ci.flags = flags;
	ci.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
	ci.ppEnabledExtensionNames = extensions.data();

	VkInstance instance = VK_NULL_HANDLE;
	VkResult r = vkCreateInstance(&ci, nullptr, &instance);
	if (r != VK_SUCCESS) {
		std::fprintf(stderr, "vkCreateInstance failed with VkResult %d\n", static_cast<int>(r));
		return 1;
	}

	uint32_t device_count = 0;
	vkEnumeratePhysicalDevices(instance, &device_count, nullptr);
	if (device_count == 0) {
		std::fprintf(stderr, "no physical devices\n");
		return 1;
	}
	std::vector<VkPhysicalDevice> devices(device_count);
	vkEnumeratePhysicalDevices(instance, &device_count, devices.data());
	VkPhysicalDevice gpu = devices[0];

	VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
	VkPhysicalDeviceProperties2 props2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
	props2.pNext = &driver;
	vkGetPhysicalDeviceProperties2(gpu, &props2);
	const VkPhysicalDeviceProperties& props = props2.properties;

	std::printf("\n== device ==\n");
	std::printf("  name          %s\n", props.deviceName);
	std::printf("  api           %u.%u.%u\n", VK_VERSION_MAJOR(props.apiVersion),
	            VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion));
	std::printf("  driver        %s %s\n", driver.driverName, driver.driverInfo);
	std::printf("  type          %s\n",
	            props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? "integrated"
	            : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? "discrete"
	                                                                       : "other/cpu");

	uint32_t dev_ext_count = 0;
	vkEnumerateDeviceExtensionProperties(gpu, nullptr, &dev_ext_count, nullptr);
	std::vector<VkExtensionProperties> device_exts(dev_ext_count);
	vkEnumerateDeviceExtensionProperties(gpu, nullptr, &dev_ext_count, device_exts.data());
	const bool has_subset = Has_Extension(device_exts, "VK_KHR_portability_subset");
	std::printf("  VK_KHR_portability_subset advertised: %s\n", has_subset ? "yes" : "no");

	// --- core features the engine's D3D8 usage needs --------------------------
	VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
	VkPhysicalDevicePortabilitySubsetFeaturesKHR subset{
	    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PORTABILITY_SUBSET_FEATURES_KHR};
	if (has_subset) features2.pNext = &subset;
	vkGetPhysicalDeviceFeatures2(gpu, &features2);
	const VkPhysicalDeviceFeatures& f = features2.features;

	std::printf("\n== core features the D3D8 surface depends on ==\n");
	Print_Bool("textureCompressionBC (DXT1/3/5)", f.textureCompressionBC);
	Print_Bool("textureCompressionETC2", f.textureCompressionETC2);
	Print_Bool("textureCompressionASTC_LDR", f.textureCompressionASTC_LDR);
	Print_Bool("independentBlend (per-RT blend)", f.independentBlend);
	Print_Bool("dualSrcBlend (D3DBLEND_BOTHSRCALPHA)", f.dualSrcBlend);
	Print_Bool("fillModeNonSolid (D3DRS_FILLMODE wire)", f.fillModeNonSolid);
	Print_Bool("depthClamp", f.depthClamp);
	Print_Bool("depthBiasClamp (D3DRS_ZBIAS)", f.depthBiasClamp);
	Print_Bool("depthBounds", f.depthBounds);
	Print_Bool("samplerAnisotropy (D3DTSS_MAXANISOTROPY)", f.samplerAnisotropy);
	Print_Bool("wideLines", f.wideLines);
	Print_Bool("largePoints (D3DRS_POINTSIZE)", f.largePoints);
	Print_Bool("shaderClipDistance (D3DRS_CLIPPLANEENABLE)", f.shaderClipDistance);
	Print_Bool("occlusionQueryPrecise", f.occlusionQueryPrecise);
	Print_Bool("multiViewport", f.multiViewport);
	Print_Bool("fragmentStoresAndAtomics", f.fragmentStoresAndAtomics);

	if (has_subset) {
		std::printf("\n== VK_KHR_portability_subset: what Metal does NOT give us for free ==\n");
		Print_Bool("triangleFans (D3DPT_TRIANGLEFAN)", subset.triangleFans);
		Print_Bool("constantAlphaColorBlendFactors", subset.constantAlphaColorBlendFactors);
		Print_Bool("events", subset.events);
		Print_Bool("imageViewFormatReinterpretation", subset.imageViewFormatReinterpretation);
		Print_Bool("imageViewFormatSwizzle (L8/A8 fixups)", subset.imageViewFormatSwizzle);
		Print_Bool("imageView2DOn3DImage", subset.imageView2DOn3DImage);
		Print_Bool("multisampleArrayImage", subset.multisampleArrayImage);
		Print_Bool("mutableComparisonSamplers", subset.mutableComparisonSamplers);
		Print_Bool("pointPolygons (D3DFILL_POINT)", subset.pointPolygons);
		Print_Bool("samplerMipLodBias (D3DTSS_MIPMAPLODBIAS)", subset.samplerMipLodBias);
		Print_Bool("separateStencilMaskRef", subset.separateStencilMaskRef);
		Print_Bool("shaderSampleRateInterpolationFunctions", subset.shaderSampleRateInterpolationFunctions);
		Print_Bool("tessellationIsolines", subset.tessellationIsolines);
		Print_Bool("tessellationPointMode", subset.tessellationPointMode);
		Print_Bool("vertexAttributeAccessBeyondStride", subset.vertexAttributeAccessBeyondStride);
	}

	std::printf("\n== limits ==\n");
	std::printf("  %-42s %u\n", "maxImageDimension2D", props.limits.maxImageDimension2D);
	std::printf("  %-42s %u\n", "maxBoundDescriptorSets", props.limits.maxBoundDescriptorSets);
	std::printf("  %-42s %u\n", "maxPerStageDescriptorSampledImages",
	            props.limits.maxPerStageDescriptorSampledImages);
	std::printf("  %-42s %u\n", "maxPerStageDescriptorSamplers",
	            props.limits.maxPerStageDescriptorSamplers);
	std::printf("  %-42s %u\n", "maxVertexInputAttributes", props.limits.maxVertexInputAttributes);
	std::printf("  %-42s %u\n", "maxVertexInputBindings", props.limits.maxVertexInputBindings);
	std::printf("  %-42s %u\n", "maxColorAttachments", props.limits.maxColorAttachments);
	std::printf("  %-42s %u\n", "maxPushConstantsSize", props.limits.maxPushConstantsSize);
	std::printf("  %-42s %.1f\n", "maxSamplerAnisotropy", props.limits.maxSamplerAnisotropy);

	std::printf("\n== formats (D3DFMT the engine names -> Vulkan) ==\n");
	std::printf("  %-18s %-24s %s\n", "D3D8", "Vulkan", "optimal-tiling features");
	for (const auto& row : kFormats) {
		VkFormatProperties fp{};
		vkGetPhysicalDeviceFormatProperties(gpu, row.format, &fp);
		std::printf("  %-18s %-24s %s\n", row.d3d_name, row.vk_name,
		            Feature_Flags(fp.optimalTilingFeatures).c_str());
	}

	std::printf("\n== notes ==\n");
	for (const auto& row : kFormats) {
		if (row.note[0] == '\0') continue;
		std::printf("  %-18s %s\n", row.d3d_name, row.note);
	}

	vkDestroyInstance(instance, nullptr);
	return 0;
}
