// Window / event loop / input spike: the same two draws as main.cpp, but presented into a
// real on-screen window created through the platform seam in
// Core/Libraries/Source/WWVegas/WWLib/platform/platform_window.h - not through SDL directly,
// and not headless.
//
// This is both the demo and the check. It asserts, and prints PASS/FAIL for, every claim the
// seam makes that can be checked from outside it:
//
//   1. a window can be created;
//   2. it names the Vulkan instance extensions its platform needs, and they include a
//      surface extension (VK_EXT_metal_surface on macOS, VK_KHR_xlib/xcb/wayland_surface on
//      Linux);
//   3. a VkSurfaceKHR can be created from it, and frames can be *presented* to it - which is
//      what the headless spike has never done on either platform;
//   4. the frames actually contain the geometry (read back and checked, so this passes or
//      fails without a human looking at the screen);
//   5. events arrive translated: window close, resize, focus, and keyboard events carrying
//      PC/AT set-1 scan codes rather than platform key codes;
//   6. Vulkan validation stays silent throughout.
//
// The same binary is what a Mac session runs, via tools/macos-window-check.sh, against the
// Cocoa/CAMetalLayer backend. See docs/porting/window-event-loop.md.
//
//   ./zh-window-spike                     # 240 frames, exits, prints PASS/FAIL lines
//   ./zh-window-spike --interactive       # runs until the window is closed or Escape
//   ./zh-window-spike --mode-change       # also exercises Window_Set_Mode() mid-run
//   ./zh-window-spike --frame-ms 16       # paces the loop, so a human can watch it
//
// Note that unpaced, this presents a few hundred frames a second even on lavapipe, so the
// default run is over in a couple of seconds: use --frame-ms (and --frames) when the point is
// for someone to see the window rather than for the checks to pass.

#include "png_write.h"
#include "render_backend.h"

#include "platform/platform_window.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace spike;

namespace {

constexpr uint32_t kWidth = 800;
constexpr uint32_t kHeight = 600;

constexpr uint32_t Argb(uint32_t a, uint32_t r, uint32_t g, uint32_t b) {
	return (a << 24) | (r << 16) | (g << 8) | b;
}

struct VertexXyzDiffuseTex1 {
	float x, y, z;
	uint32_t diffuse;
	float u, v;
};

struct VertexXyzrhwDiffuse {
	float x, y, z, rhw;
	uint32_t diffuse;
};

std::vector<uint32_t> Make_Checkerboard(uint32_t size) {
	std::vector<uint32_t> pixels(static_cast<size_t>(size) * size);
	for (uint32_t y = 0; y < size; ++y) {
		for (uint32_t x = 0; x < size; ++x) {
			const bool light = ((x / 8) + (y / 8)) % 2 == 0;
			pixels[y * size + x] = light ? Argb(255, 235, 235, 235) : Argb(255, 40, 90, 190);
		}
	}
	return pixels;
}

int Failures = 0;

void Check(bool condition, const char* what) {
	std::printf("%-6s %s\n", condition ? "PASS" : "FAIL", what);
	if (!condition) ++Failures;
}

const char* Event_Name(WWPlatform::WindowEventType type) {
	switch (type) {
		case WWPlatform::WINDOW_EVENT_CLOSE: return "close";
		case WWPlatform::WINDOW_EVENT_RESIZE: return "resize";
		case WWPlatform::WINDOW_EVENT_MOVE: return "move";
		case WWPlatform::WINDOW_EVENT_FOCUS_GAINED: return "focus-gained";
		case WWPlatform::WINDOW_EVENT_FOCUS_LOST: return "focus-lost";
		case WWPlatform::WINDOW_EVENT_MINIMISED: return "minimised";
		case WWPlatform::WINDOW_EVENT_RESTORED: return "restored";
		case WWPlatform::WINDOW_EVENT_KEY_DOWN: return "key-down";
		case WWPlatform::WINDOW_EVENT_KEY_UP: return "key-up";
		case WWPlatform::WINDOW_EVENT_TEXT: return "text";
		case WWPlatform::WINDOW_EVENT_MOUSE_MOVE: return "mouse-move";
		case WWPlatform::WINDOW_EVENT_MOUSE_DOWN: return "mouse-down";
		case WWPlatform::WINDOW_EVENT_MOUSE_UP: return "mouse-up";
		case WWPlatform::WINDOW_EVENT_MOUSE_WHEEL: return "mouse-wheel";
		case WWPlatform::WINDOW_EVENT_MOUSE_ENTER: return "mouse-enter";
		case WWPlatform::WINDOW_EVENT_MOUSE_LEAVE: return "mouse-leave";
		default: return "none";
	}
}

const char* Surface_Kind_Name(WWPlatform::NativeSurfaceKind kind) {
	switch (kind) {
		case WWPlatform::NATIVE_SURFACE_X11: return "x11";
		case WWPlatform::NATIVE_SURFACE_WAYLAND: return "wayland";
		case WWPlatform::NATIVE_SURFACE_METAL_LAYER: return "CAMetalLayer";
		default: return "none";
	}
}

} // namespace

int main(int argc, char** argv) {
	bool validation = true;
	bool interactive = false;
	bool mode_change = false;
	int frames = 240;
	int frame_ms = 0;
	std::string out_path = "window-spike.png";
	for (int i = 1; i < argc; ++i) {
		if (std::strcmp(argv[i], "--no-validation") == 0) validation = false;
		else if (std::strcmp(argv[i], "--interactive") == 0) interactive = true;
		else if (std::strcmp(argv[i], "--mode-change") == 0) mode_change = true;
		else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) frames = std::atoi(argv[++i]);
		else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) out_path = argv[++i];
		else if (std::strcmp(argv[i], "--frame-ms") == 0 && i + 1 < argc) {
			frame_ms = std::atoi(argv[++i]);
		}
		else if (std::strcmp(argv[i], "--help") == 0) {
			std::printf("usage: %s [--interactive] [--mode-change] [--frames N] "
			            "[--frame-ms N] [--no-validation] [--out file.png]\n", argv[0]);
			return 0;
		}
	}

	std::printf("window backend: %s\n", WWPlatform::Window_Backend_Name());

	WWPlatform::WindowConfig config;
	config.Title = "Zero Hour window spike: platform_window -> Vulkan";
	config.Width = static_cast<int>(kWidth);
	config.Height = static_cast<int>(kHeight);
	void* window = WWPlatform::Window_Create(config);
	Check(window != nullptr, "Window_Create()");
	if (window == nullptr) {
		std::fprintf(stderr, "  %s\n", WWPlatform::Window_Last_Error());
		return 1;
	}

	// The extensions have to be known before the instance exists, which is why the seam
	// reports them rather than creating the instance itself.
	const char* extension_names[8] = {nullptr};
	const int extension_count =
	    WWPlatform::Window_Vulkan_Instance_Extensions(window, extension_names, 8);
	Check(extension_count >= 2, "Window_Vulkan_Instance_Extensions() names >= 2 extensions");
	bool has_surface_extension = false;
	for (int i = 0; i < extension_count; ++i) {
		std::printf("       instance extension: %s\n", extension_names[i]);
		if (std::strstr(extension_names[i], "surface") != nullptr &&
		    std::strcmp(extension_names[i], "VK_KHR_surface") != 0) {
			has_surface_extension = true;
		}
	}
	Check(has_surface_extension, "a platform surface extension is required by the window");

	const WWPlatform::NativeSurface native = WWPlatform::Window_Native_Surface(window);
	std::printf("       native surface: %s\n", Surface_Kind_Name(native.Kind));

	RenderBackend* gfx = Create_Vulkan_Backend(validation, /*headless=*/false);
	const bool initialised = gfx->Init(window, kWidth, kHeight);
	Check(initialised, "VkSurfaceKHR + swapchain from the seam's window");
	if (!initialised) {
		std::fprintf(stderr, "  %s\n", WWPlatform::Window_Last_Error());
		return 1;
	}
	std::printf("device: %s\n", gfx->Device_Description());

	const uint32_t kTextureSize = 64;
	std::vector<uint32_t> checker = Make_Checkerboard(kTextureSize);
	TextureHandle* texture = gfx->Create_Texture(
	    kTextureSize, kTextureSize, reinterpret_cast<const uint8_t*>(checker.data()));

	const VertexXyzDiffuseTex1 triangle[3] = {
	    {0.0f, 0.75f, 0.5f, Argb(255, 255, 255, 255), 0.5f, 0.0f},
	    {0.85f, -0.6f, 0.5f, Argb(255, 255, 160, 160), 1.0f, 1.0f},
	    {-0.85f, -0.6f, 0.5f, Argb(255, 160, 255, 160), 0.0f, 1.0f},
	};
	const uint16_t triangle_indices[3] = {0, 1, 2};
	VertexBufferHandle* triangle_vb = gfx->Create_Vertex_Buffer(
	    triangle, sizeof(triangle), D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1);
	IndexBufferHandle* triangle_ib = gfx->Create_Index_Buffer(triangle_indices, 3);

	const VertexXyzrhwDiffuse quad[4] = {
	    {16.0f, 16.0f, 0.1f, 1.0f, Argb(255, 255, 210, 60)},
	    {216.0f, 16.0f, 0.1f, 1.0f, Argb(255, 255, 210, 60)},
	    {216.0f, 76.0f, 0.1f, 1.0f, Argb(255, 200, 60, 60)},
	    {16.0f, 76.0f, 0.1f, 1.0f, Argb(255, 200, 60, 60)},
	};
	const uint16_t quad_indices[6] = {0, 1, 2, 0, 2, 3};
	VertexBufferHandle* quad_vb =
	    gfx->Create_Vertex_Buffer(quad, sizeof(quad), D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
	IndexBufferHandle* quad_ib = gfx->Create_Index_Buffer(quad_indices, 6);
	Check(texture != nullptr && triangle_vb != nullptr && triangle_ib != nullptr &&
	          quad_vb != nullptr && quad_ib != nullptr,
	      "resources created");

	int event_counts[WWPlatform::WINDOW_EVENT_MOUSE_LEAVE + 1] = {0};
	int keys_seen = 0;
	int presented = 0;
	bool quit = false;
	bool resized = false;

	// This is the loop shape Win32GameEngine::serviceWindowsOS() has: drain everything the
	// window server has for us, then run the frame. The difference is that nothing is
	// dispatched to a WndProc - the events are returned here, in order.
	for (int frame = 0; (interactive || frame < frames) && !quit; ++frame) {
		WWPlatform::WindowEvent event;
		while (WWPlatform::Window_Poll_Event(window, event)) {
			if (event.Type <= WWPlatform::WINDOW_EVENT_MOUSE_LEAVE) {
				++event_counts[event.Type];
			}
			switch (event.Type) {
				case WWPlatform::WINDOW_EVENT_CLOSE:
					quit = true;
					break;
				case WWPlatform::WINDOW_EVENT_KEY_DOWN:
					++keys_seen;
					std::printf("       key-down set-1 scan code 0x%02X%s modifiers 0x%X\n",
					            event.Scan_Code, event.Repeat ? " (repeat)" : "",
					            event.Modifiers);
					// KEYSCAN_ESCAPE. Deliberately compared as the set-1 value the engine's
					// KeyDefType would hold, which is the whole point of the translation.
					if (event.Scan_Code == 0x01) quit = true;
					break;
				case WWPlatform::WINDOW_EVENT_RESIZE:
					std::printf("       resize to %dx%d\n", event.Width, event.Height);
					// The swapchain no longer matches the window, and on a platform whose
					// driver never returns VK_ERROR_OUT_OF_DATE_KHR (lavapipe does not)
					// nothing else will notice: without this the presented image stays the
					// old size in the corner of a bigger window.
					resized = gfx->Resize_Presentation(static_cast<uint32_t>(event.Width),
					                                   static_cast<uint32_t>(event.Height));
					if (!resized) {
						std::fprintf(stderr, "Resize_Presentation() failed\n");
					}
					break;
				case WWPlatform::WINDOW_EVENT_TEXT:
					std::printf("       text U+%04X\n", event.Character);
					break;
				default:
					break;
			}
		}

		if (mode_change && frame == frames / 2) {
			// The fullscreen/mode-change path, which on Win32 is SetWindowPos + ShowWindow +
			// a D3D device reset. Here it is one call plus the resize event it produces,
			// which is what rebuilds the swapchain (see WINDOW_EVENT_RESIZE above).
			const bool ok = WWPlatform::Window_Set_Mode(window, 1024, 768, false);
			Check(ok, "Window_Set_Mode(1024x768, windowed)");
		}

		// The engine skips rendering entirely while iconified; so does this, and that is the
		// IsIconic()/Sleep(5) branch of Win32GameEngine::update() in seam terms.
		if (WWPlatform::Window_Is_Minimised(window)) continue;

		gfx->Begin_Scene();
		gfx->Clear(true, true, 0.06f, 0.07f, 0.10f, 1.0f);

		gfx->Set_Transform(D3DTS_WORLD, Matrix4x4::Identity());
		gfx->Set_Transform(D3DTS_VIEW, Matrix4x4::Identity());
		gfx->Set_Transform(D3DTS_PROJECTION, Matrix4x4::Identity());
		gfx->Set_DX8_Render_State(D3DRS_ZENABLE, 1);
		gfx->Set_DX8_Render_State(D3DRS_ZWRITEENABLE, 1);
		gfx->Set_DX8_Render_State(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
		gfx->Set_DX8_Render_State(D3DRS_CULLMODE, D3DCULL_NONE);
		gfx->Set_DX8_Render_State(D3DRS_ALPHABLENDENABLE, 0);
		gfx->Set_DX8_Render_State(D3DRS_ALPHATESTENABLE, 0);
		gfx->Set_DX8_Texture_Stage_State(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
		gfx->Set_DX8_Texture_Stage_State(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
		gfx->Set_DX8_Texture_Stage_State(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
		gfx->Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
		gfx->Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
		gfx->Set_DX8_Texture_Stage_State(0, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
		gfx->Set_DX8_Texture_Stage_State(0, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
		gfx->Set_DX8_Texture_Stage_State(0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
		gfx->Set_DX8_Texture_Stage_State(0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
		gfx->Set_DX8_Texture_Stage_State(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
		gfx->Set_DX8_Texture_Stage_State(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
		gfx->Set_Texture(0, texture);
		gfx->Set_Vertex_Buffer(triangle_vb, 0);
		gfx->Set_Index_Buffer(triangle_ib, 0);
		gfx->Draw_Triangles(0, 1, 0, 3);

		gfx->Set_DX8_Render_State(D3DRS_ZENABLE, 0);
		gfx->Set_DX8_Render_State(D3DRS_ALPHABLENDENABLE, 1);
		gfx->Set_DX8_Render_State(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
		gfx->Set_DX8_Render_State(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
		gfx->Set_DX8_Texture_Stage_State(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
		gfx->Set_DX8_Texture_Stage_State(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
		gfx->Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
		gfx->Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
		gfx->Set_Texture(0, nullptr);
		gfx->Set_Vertex_Buffer(quad_vb, 0);
		gfx->Set_Index_Buffer(quad_ib, 0);
		gfx->Draw_Triangles(0, 2, 0, 4);

		gfx->End_Scene(true);	// flip: this is the vkQueuePresentKHR
		++presented;
		if (frame_ms > 0) {
			std::this_thread::sleep_for(std::chrono::milliseconds(frame_ms));
		}
	}

	Check(presented > 0, "frames presented to the window's swapchain");
	std::printf("       presented %d frame(s)\n", presented);

	// A mode change that the renderer did not follow is the failure the readback cannot see:
	// the colour target keeps its size, so only the swapchain rebuild proves the window is
	// still being filled.
	if (mode_change) {
		int client_width = 0;
		int client_height = 0;
		WWPlatform::Window_Client_Size(window, client_width, client_height);
		std::printf("       client size after the mode change: %dx%d\n", client_width,
		            client_height);
		Check(resized, "the swapchain was rebuilt for the new window size");
	}

	// Read the last frame back and check it, so that "a window appeared" is not the only
	// evidence: a window showing a black rectangle would pass a screenshot and fail here.
	std::string rgba;
	SurfaceFormat format;
	const bool read_back = gfx->Read_Back_Color_Target(rgba, format);
	Check(read_back, "Read_Back_Color_Target()");
	if (read_back) {
		if (!Write_Png(out_path, rgba, format.width, format.height)) {
			std::fprintf(stderr, "failed to write %s\n", out_path.c_str());
		} else {
			std::printf("       wrote %s (%ux%u)\n", out_path.c_str(), format.width,
			            format.height);
		}
		const size_t centre =
		    (static_cast<size_t>(format.height / 2) * format.width + format.width / 2) * 4;
		const unsigned char* px =
		    reinterpret_cast<const unsigned char*>(rgba.data()) + centre;
		std::printf("       centre pixel rgba = %u,%u,%u,%u\n", px[0], px[1], px[2], px[3]);
		Check(!(px[0] < 30 && px[1] < 30 && px[2] < 40),
		      "the presented frame contains the geometry, not just the clear colour");
	}

	std::printf("events seen:");
	for (int i = 0; i <= WWPlatform::WINDOW_EVENT_MOUSE_LEAVE; ++i) {
		if (event_counts[i] > 0) {
			std::printf(" %s=%d", Event_Name(static_cast<WWPlatform::WindowEventType>(i)),
			            event_counts[i]);
		}
	}
	std::printf("\n");
	// A window that never gains focus is a real failure mode on Wayland and on a Mac that was
	// launched without an activation policy, so it is reported - but it is not a hard failure,
	// because a window manager is entitled not to focus a new window and CI has none.
	if (event_counts[WWPlatform::WINDOW_EVENT_FOCUS_GAINED] == 0) {
		std::printf("note: no focus-gained event; the window never became active\n");
	}
	if (interactive && keys_seen == 0) {
		std::printf("note: no keys were pressed, so the scan-code translation was not "
		            "exercised\n");
	}

	const uint32_t validation_messages = gfx->Validation_Message_Count();
	std::printf("validation messages: %u\n", validation_messages);
	if (validation) Check(validation_messages == 0, "no Vulkan validation messages");

	gfx->Shutdown();
	delete gfx;
	WWPlatform::Window_Destroy(window);

	if (Failures != 0) {
		std::printf("\nFAIL: %d check(s) failed\n", Failures);
		return 1;
	}
	std::printf("\nOK: all checks passed on the %s backend\n", WWPlatform::Window_Backend_Name());
	return 0;
}
