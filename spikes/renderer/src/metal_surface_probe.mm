// VK_EXT_metal_surface probe: what MoltenVK on this machine actually does with a CAMetalLayer.
//
// PR #32 wrote the macOS half of the window seam
// (Core/Libraries/Source/WWVegas/WWLib/platform/platform_window_cocoa.mm) on a Linux box with no
// macOS SDK. Two of its guesses could not be checked there and are not checked by the window
// spike either, because the window spike needs an NSWindow and therefore a windowing session:
//
//   * that VK_EXT_metal_surface is advertised at all by the loader/driver on the machine, and
//     that vkCreateMetalSurfaceEXT is resolvable through vkGetInstanceProcAddr - which is what
//     the seam resolves it with, after finding vkGetInstanceProcAddr through
//     dlsym(RTLD_DEFAULT, ...);
//   * that a VkSurfaceKHR can be created from a CAMetalLayer, and that a swapchain can be made
//     and presented to.
//
// This probe checks those without a window: a CAMetalLayer needs no NSWindow to exist, so
// everything from the layer down can be exercised on a headless CI runner. Each stage prints
// PASS/FAIL and a fact; the process exits non-zero if a required stage failed. Stages named with
// --optional are reported but do not fail the run, which is how the paravirtualised GPU on a
// GitHub macOS runner is accommodated without pretending the stage passed.
//
//   ./zh-metal-surface-probe
//   ./zh-metal-surface-probe --optional present,swapchain
//
// See docs/porting/window-event-loop.md.

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <dlfcn.h>

// Without this vulkan.h declares no VkMetalSurfaceCreateInfoEXT and no
// PFN_vkCreateMetalSurfaceEXT, and clang helpfully suggests VkHeadlessSurfaceCreateInfoEXT
// instead. Found by the macos-15 job, which is the point of the job.
#define VK_USE_PLATFORM_METAL_EXT 1
#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::vector<std::string> Optional_Stages;
int Failures = 0;

bool Is_Optional(const char* stage) {
	for (const std::string& name : Optional_Stages) {
		if (name == stage) return true;
	}
	return false;
}

// Every claim goes through here, so the log lists the stage names --optional accepts.
bool Check(const char* stage, bool ok, const std::string& detail) {
	const bool optional = Is_Optional(stage);
	const char* verdict = ok ? "PASS" : (optional ? "SKIP" : "FAIL");
	std::printf("%-6s %-16s %s\n", verdict, stage, detail.c_str());
	if (!ok && !optional) ++Failures;
	return ok;
}

std::string Result_Name(VkResult result) {
	switch (result) {
		case VK_SUCCESS: return "VK_SUCCESS";
		case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
		case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
		case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
		case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
		case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
		case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
		case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
		case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
		case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
		default: return "VkResult " + std::to_string(static_cast<int>(result));
	}
}

std::string Format_Name(VkFormat format) {
	switch (format) {
		case VK_FORMAT_B8G8R8A8_UNORM: return "VK_FORMAT_B8G8R8A8_UNORM";
		case VK_FORMAT_B8G8R8A8_SRGB: return "VK_FORMAT_B8G8R8A8_SRGB";
		case VK_FORMAT_R8G8B8A8_UNORM: return "VK_FORMAT_R8G8B8A8_UNORM";
		case VK_FORMAT_R8G8B8A8_SRGB: return "VK_FORMAT_R8G8B8A8_SRGB";
		case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return "VK_FORMAT_A2B10G10R10_UNORM_PACK32";
		case VK_FORMAT_R16G16B16A16_SFLOAT: return "VK_FORMAT_R16G16B16A16_SFLOAT";
		default: return "VkFormat " + std::to_string(static_cast<int>(format));
	}
}

} // namespace

int main(int argc, char** argv) {
	uint32_t width = 256;
	uint32_t height = 256;
	for (int i = 1; i < argc; ++i) {
		if (std::strcmp(argv[i], "--optional") == 0 && i + 1 < argc) {
			std::string list = argv[++i];
			size_t start = 0;
			while (start <= list.size()) {
				const size_t comma = list.find(',', start);
				const std::string name = list.substr(
				    start, comma == std::string::npos ? std::string::npos : comma - start);
				if (!name.empty()) Optional_Stages.push_back(name);
				if (comma == std::string::npos) break;
				start = comma + 1;
			}
		} else if (std::strcmp(argv[i], "--help") == 0) {
			std::printf("usage: %s [--optional stage,stage]\n", argv[0]);
			std::printf("stages: extensions instance dlsym entrypoint metal-device layer "
			            "surface physical-device surface-support swapchain present\n");
			return 0;
		}
	}

	// The struct the blind backend mirrored, as this SDK declares it. Printed rather than only
	// static_asserted so the numbers appear in the CI log.
	std::printf("VK_EXT_metal_surface as this SDK declares it:\n");
	std::printf("       VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT = %d\n",
	            static_cast<int>(VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT));
	std::printf("       sizeof(VkMetalSurfaceCreateInfoEXT) = %zu, sizeof(VkSurfaceKHR) = %zu\n",
	            sizeof(VkMetalSurfaceCreateInfoEXT), sizeof(VkSurfaceKHR));
	std::printf("       VK_EXT_METAL_SURFACE_EXTENSION_NAME = %s (spec version %d)\n",
	            VK_EXT_METAL_SURFACE_EXTENSION_NAME, VK_EXT_METAL_SURFACE_SPEC_VERSION);
	std::printf("       header VK_HEADER_VERSION = %d\n", VK_HEADER_VERSION);

	// --- the loader/driver's own answer about the extension -----------------------------------
	uint32_t extension_count = 0;
	vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, nullptr);
	std::vector<VkExtensionProperties> extensions(extension_count);
	if (extension_count > 0) {
		vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, extensions.data());
	}
	bool has_surface = false;
	bool has_metal_surface = false;
	bool has_portability = false;
	std::printf("instance extensions advertised (%u):\n", extension_count);
	for (const VkExtensionProperties& extension : extensions) {
		std::printf("       %s (spec %u)\n", extension.extensionName, extension.specVersion);
		if (std::strcmp(extension.extensionName, VK_KHR_SURFACE_EXTENSION_NAME) == 0) {
			has_surface = true;
		}
		if (std::strcmp(extension.extensionName, VK_EXT_METAL_SURFACE_EXTENSION_NAME) == 0) {
			has_metal_surface = true;
		}
		if (std::strcmp(extension.extensionName, "VK_KHR_portability_enumeration") == 0) {
			has_portability = true;
		}
	}
	Check("extensions", has_surface && has_metal_surface,
	      std::string("VK_KHR_surface ") + (has_surface ? "yes" : "NO") +
	          ", VK_EXT_metal_surface " + (has_metal_surface ? "yes" : "NO") +
	          ", VK_KHR_portability_enumeration " + (has_portability ? "yes" : "no"));

	// --- instance -----------------------------------------------------------------------------
	std::vector<const char*> enabled;
	if (has_surface) enabled.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
	if (has_metal_surface) enabled.push_back(VK_EXT_METAL_SURFACE_EXTENSION_NAME);
	if (has_portability) enabled.push_back("VK_KHR_portability_enumeration");

	VkApplicationInfo app{};
	app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	app.pApplicationName = "zh-metal-surface-probe";
	app.apiVersion = VK_API_VERSION_1_1;

	VkInstanceCreateInfo ici{};
	ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	ici.pApplicationInfo = &app;
	ici.enabledExtensionCount = static_cast<uint32_t>(enabled.size());
	ici.ppEnabledExtensionNames = enabled.data();
	// Without this a current Khronos loader refuses MoltenVK outright: see the
	// renderer-spike-macos job's comment in .github/workflows/native-port-ci.yml.
	if (has_portability) ici.flags |= 0x00000001;	// VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR

	VkInstance instance = VK_NULL_HANDLE;
	VkResult result = vkCreateInstance(&ici, nullptr, &instance);
	if (!Check("instance", result == VK_SUCCESS, "vkCreateInstance: " + Result_Name(result))) {
		std::printf("\nFAIL: %d stage(s) failed\n", Failures);
		return 1;
	}

	// --- exactly how the seam resolves the entry point ----------------------------------------
	typedef PFN_vkVoidFunction (*Get_Proc_Type)(VkInstance, const char*);
	Get_Proc_Type get_proc =
	    reinterpret_cast<Get_Proc_Type>(dlsym(RTLD_DEFAULT, "vkGetInstanceProcAddr"));
	Check("dlsym", get_proc != nullptr,
	      get_proc != nullptr
	          ? "dlsym(RTLD_DEFAULT, \"vkGetInstanceProcAddr\") resolves, which is what "
	            "platform_window_cocoa.mm does"
	          : "dlsym(RTLD_DEFAULT, \"vkGetInstanceProcAddr\") returned null: the seam's "
	            "resolution strategy does not work here");

	PFN_vkCreateMetalSurfaceEXT create_metal_surface = nullptr;
	if (get_proc != nullptr) {
		create_metal_surface = reinterpret_cast<PFN_vkCreateMetalSurfaceEXT>(
		    get_proc(instance, "vkCreateMetalSurfaceEXT"));
	}
	Check("entrypoint", create_metal_surface != nullptr,
	      create_metal_surface != nullptr
	          ? "vkCreateMetalSurfaceEXT resolved through vkGetInstanceProcAddr"
	          : "vkCreateMetalSurfaceEXT did not resolve");

	// --- Metal device and layer ---------------------------------------------------------------
	id<MTLDevice> metal_device = MTLCreateSystemDefaultDevice();
	Check("metal-device", metal_device != nil,
	      metal_device != nil ? std::string("MTLCreateSystemDefaultDevice(): ") +
	                                [[metal_device name] UTF8String]
	                          : "MTLCreateSystemDefaultDevice() returned nil");

	CAMetalLayer* layer = [CAMetalLayer layer];
	if (layer != nil && metal_device != nil) {
		layer.device = metal_device;
		layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
		layer.framebufferOnly = NO;
		// The bounds first: a layer with a zero frame logs "CAMetalLayer ignoring invalid
		// setDrawableSize width=0.000000" and keeps a zero drawable.
		layer.bounds = CGRectMake(0.0, 0.0, width, height);
		layer.drawableSize = CGSizeMake(width, height);
	}
	Check("layer", layer != nil && metal_device != nil,
	      layer != nil ? "CAMetalLayer created, not attached to any NSWindow (which is the "
	                     "point: no windowing session is needed below this line)"
	                   : "[CAMetalLayer layer] returned nil");

	VkSurfaceKHR surface = VK_NULL_HANDLE;
	if (create_metal_surface != nullptr && layer != nil) {
		VkMetalSurfaceCreateInfoEXT info{};
		info.sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT;
		info.pLayer = layer;
		result = create_metal_surface(instance, &info, nullptr, &surface);
	} else {
		result = VK_ERROR_INITIALIZATION_FAILED;
	}
	Check("surface", result == VK_SUCCESS && surface != VK_NULL_HANDLE,
	      "vkCreateMetalSurfaceEXT: " + Result_Name(result));

	// --- physical device, and what it says about that surface ---------------------------------
	uint32_t device_count = 0;
	vkEnumeratePhysicalDevices(instance, &device_count, nullptr);
	std::vector<VkPhysicalDevice> devices(device_count);
	if (device_count > 0) vkEnumeratePhysicalDevices(instance, &device_count, devices.data());
	VkPhysicalDevice physical = device_count > 0 ? devices[0] : VK_NULL_HANDLE;
	std::string device_name = "none";
	if (physical != VK_NULL_HANDLE) {
		VkPhysicalDeviceProperties properties{};
		vkGetPhysicalDeviceProperties(physical, &properties);
		device_name = properties.deviceName;
	}
	Check("physical-device", physical != VK_NULL_HANDLE,
	      std::to_string(device_count) + " device(s); first is " + device_name);

	uint32_t queue_family = 0;
	bool supported = false;
	if (physical != VK_NULL_HANDLE && surface != VK_NULL_HANDLE) {
		uint32_t family_count = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, nullptr);
		for (uint32_t i = 0; i < family_count; ++i) {
			VkBool32 present_support = VK_FALSE;
			vkGetPhysicalDeviceSurfaceSupportKHR(physical, i, surface, &present_support);
			if (present_support == VK_TRUE) {
				queue_family = i;
				supported = true;
				break;
			}
		}
	}

	VkSurfaceCapabilitiesKHR caps{};
	std::vector<VkSurfaceFormatKHR> formats;
	std::vector<VkPresentModeKHR> present_modes;
	if (supported) {
		vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps);
		uint32_t format_count = 0;
		vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &format_count, nullptr);
		formats.resize(format_count);
		if (format_count > 0) {
			vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &format_count,
			                                     formats.data());
		}
		uint32_t mode_count = 0;
		vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &mode_count, nullptr);
		present_modes.resize(mode_count);
		if (mode_count > 0) {
			vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &mode_count,
			                                          present_modes.data());
		}
	}
	Check("surface-support", supported && !formats.empty(),
	      supported ? "queue family " + std::to_string(queue_family) + " presents; " +
	                      std::to_string(formats.size()) + " surface format(s), " +
	                      std::to_string(present_modes.size()) + " present mode(s), " +
	                      "currentExtent " + std::to_string(caps.currentExtent.width) + "x" +
	                      std::to_string(caps.currentExtent.height) + ", images " +
	                      std::to_string(caps.minImageCount) + ".." +
	                      std::to_string(caps.maxImageCount)
	                : "no queue family can present to this surface");
	// MoltenVK advertises the same handful of formats once per colour space (60 pairs on the
	// CI runner), so only the sRGB-nonlinear ones are listed; the count above is the total.
	for (const VkSurfaceFormatKHR& format : formats) {
		if (format.colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) continue;
		std::printf("       surface format: %s (VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)\n",
		            Format_Name(format.format).c_str());
	}
	if (supported) {
		std::printf("       supportedUsageFlags = 0x%X (TRANSFER_DST %s, which the renderer's "
		            "swapchain asks for)\n",
		            caps.supportedUsageFlags,
		            (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) ? "yes" : "NO");
	}

	// --- a device, a swapchain and one present -------------------------------------------------
	VkDevice device = VK_NULL_HANDLE;
	VkSwapchainKHR swapchain = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	if (supported && !formats.empty()) {
		const float priority = 1.0f;
		VkDeviceQueueCreateInfo qci{};
		qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		qci.queueFamilyIndex = queue_family;
		qci.queueCount = 1;
		qci.pQueuePriorities = &priority;

		std::vector<const char*> device_extensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
		uint32_t device_extension_count = 0;
		vkEnumerateDeviceExtensionProperties(physical, nullptr, &device_extension_count, nullptr);
		std::vector<VkExtensionProperties> available(device_extension_count);
		if (device_extension_count > 0) {
			vkEnumerateDeviceExtensionProperties(physical, nullptr, &device_extension_count,
			                                     available.data());
		}
		for (const VkExtensionProperties& extension : available) {
			if (std::strcmp(extension.extensionName, "VK_KHR_portability_subset") == 0) {
				device_extensions.push_back("VK_KHR_portability_subset");
			}
		}

		VkDeviceCreateInfo dci{};
		dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
		dci.queueCreateInfoCount = 1;
		dci.pQueueCreateInfos = &qci;
		dci.enabledExtensionCount = static_cast<uint32_t>(device_extensions.size());
		dci.ppEnabledExtensionNames = device_extensions.data();
		result = vkCreateDevice(physical, &dci, nullptr, &device);
		if (result == VK_SUCCESS) vkGetDeviceQueue(device, queue_family, 0, &queue);

		if (result == VK_SUCCESS) {
			VkSwapchainCreateInfoKHR sci{};
			sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
			sci.surface = surface;
			sci.minImageCount = caps.minImageCount;
			sci.imageFormat = formats[0].format;
			sci.imageColorSpace = formats[0].colorSpace;
			sci.imageExtent = caps.currentExtent.width == 0xFFFFFFFFu
			                      ? VkExtent2D{width, height}
			                      : caps.currentExtent;
			sci.imageArrayLayers = 1;
			sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
			if (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) {
				sci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
			}
			sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
			sci.preTransform = caps.currentTransform;
			sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
			sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
			sci.clipped = VK_TRUE;
			result = vkCreateSwapchainKHR(device, &sci, nullptr, &swapchain);
		}
	} else {
		result = VK_ERROR_INITIALIZATION_FAILED;
	}
	Check("swapchain", swapchain != VK_NULL_HANDLE,
	      "vkCreateSwapchainKHR: " + Result_Name(result));

	if (swapchain != VK_NULL_HANDLE) {
		// One acquire/present round trip. A drawable from a layer that is in no window is
		// what a headless runner can offer; whether pixels reach a screen is not what this
		// asserts, and cannot be.
		VkFenceCreateInfo fci{};
		fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		VkFence fence = VK_NULL_HANDLE;
		vkCreateFence(device, &fci, nullptr, &fence);
		uint32_t image_index = 0;
		result = vkAcquireNextImageKHR(device, swapchain, 2000000000ull, VK_NULL_HANDLE, fence,
		                               &image_index);
		if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
			vkWaitForFences(device, 1, &fence, VK_TRUE, 2000000000ull);
			VkPresentInfoKHR pi{};
			pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
			pi.swapchainCount = 1;
			pi.pSwapchains = &swapchain;
			pi.pImageIndices = &image_index;
			result = vkQueuePresentKHR(queue, &pi);
			Check("present", result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR,
			      "acquired image " + std::to_string(image_index) + " and vkQueuePresentKHR: " +
			          Result_Name(result));
		} else {
			Check("present", false, "vkAcquireNextImageKHR: " + Result_Name(result));
		}
		vkDestroyFence(device, fence, nullptr);
		vkDeviceWaitIdle(device);
	} else {
		Check("present", false, "not attempted: no swapchain");
	}

	if (swapchain != VK_NULL_HANDLE) vkDestroySwapchainKHR(device, swapchain, nullptr);
	if (device != VK_NULL_HANDLE) vkDestroyDevice(device, nullptr);
	if (surface != VK_NULL_HANDLE) vkDestroySurfaceKHR(instance, surface, nullptr);
	vkDestroyInstance(instance, nullptr);

	if (Failures != 0) {
		std::printf("\nFAIL: %d stage(s) failed\n", Failures);
		return 1;
	}
	std::printf("\nOK: every required stage passed\n");
	return 0;
}
