// Renderer spike driver.
//
// Issues the same sequence of calls the engine issues, in the same order, through
// the DX8Wrapper-shaped interface -- and nothing else. If you diff this against
// DX8Wrapper::Draw_Triangles' caller in dx8renderer.cpp the shape should be
// recognisable: set states, set stage states, set texture, set buffers, draw.

#include "png_write.h"
#include "render_backend.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef SPIKE_WITH_SDL
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#endif

using namespace spike;

namespace {

constexpr uint32_t kWidth = 800;
constexpr uint32_t kHeight = 600;

// D3DCOLOR order: 0xAARRGGBB.
constexpr uint32_t Argb(uint32_t a, uint32_t r, uint32_t g, uint32_t b) {
	return (a << 24) | (r << 16) | (g << 8) | b;
}

// D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1, the engine's most common vertex.
struct VertexXyzDiffuseTex1 {
	float x, y, z;
	uint32_t diffuse;
	float u, v;
};

// D3DFVF_XYZRHW | D3DFVF_DIFFUSE, what the engine uses for its 2D/screen-space
// passes (W3DShaderManager's post-process quads, the shadow passes).
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

} // namespace

int main(int argc, char** argv) {
	bool headless = true;
	bool validation = true;
	std::string out_path = "spike-triangle.png";
	for (int i = 1; i < argc; ++i) {
		if (std::strcmp(argv[i], "--window") == 0) headless = false;
		else if (std::strcmp(argv[i], "--no-validation") == 0) validation = false;
		else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) out_path = argv[++i];
		else if (std::strcmp(argv[i], "--help") == 0) {
			std::printf("usage: %s [--window] [--no-validation] [--out file.png]\n", argv[0]);
			return 0;
		}
	}

	void* window = nullptr;
#ifdef SPIKE_WITH_SDL
	if (!headless) {
		if (SDL_Init(SDL_INIT_VIDEO) != 0) {
			std::fprintf(stderr, "SDL_Init failed: %s -- falling back to headless\n", SDL_GetError());
			headless = true;
		} else {
			window = SDL_CreateWindow("Zero Hour renderer spike: DX8Wrapper -> Vulkan",
			                          SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
			                          static_cast<int>(kWidth), static_cast<int>(kHeight),
			                          SDL_WINDOW_VULKAN | SDL_WINDOW_SHOWN);
			if (window == nullptr) {
				std::fprintf(stderr, "SDL_CreateWindow failed: %s -- falling back to headless\n",
				             SDL_GetError());
				headless = true;
			}
		}
	}
#else
	if (!headless) {
		std::fprintf(stderr, "built without SDL2; running headless\n");
		headless = true;
	}
#endif

	RenderBackend* gfx = Create_Vulkan_Backend(validation, headless);
	if (!gfx->Init(window, kWidth, kHeight)) {
		std::fprintf(stderr, "backend Init failed\n");
		return 1;
	}
	std::printf("device: %s\n", gfx->Device_Description());

	// --- resources ----------------------------------------------------------
	const uint32_t kTextureSize = 64;
	std::vector<uint32_t> checker = Make_Checkerboard(kTextureSize);
	TextureHandle* texture = gfx->Create_Texture(
	    kTextureSize, kTextureSize, reinterpret_cast<const uint8_t*>(checker.data()));
	if (texture == nullptr) {
		std::fprintf(stderr, "Create_Texture failed\n");
		return 1;
	}

	const VertexXyzDiffuseTex1 triangle[3] = {
	    {0.0f, 0.75f, 0.5f, Argb(255, 255, 255, 255), 0.5f, 0.0f},
	    {0.85f, -0.6f, 0.5f, Argb(255, 255, 160, 160), 1.0f, 1.0f},
	    {-0.85f, -0.6f, 0.5f, Argb(255, 160, 255, 160), 0.0f, 1.0f},
	};
	const uint16_t triangle_indices[3] = {0, 1, 2};
	VertexBufferHandle* triangle_vb = gfx->Create_Vertex_Buffer(
	    triangle, sizeof(triangle), D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1);
	IndexBufferHandle* triangle_ib = gfx->Create_Index_Buffer(triangle_indices, 3);

	// Screen-space quad in the corner: exercises the second FVF variant, a second
	// pipeline (alpha blending on), and D3DTOP_SELECTARG1 with no texture.
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

	if (triangle_vb == nullptr || triangle_ib == nullptr || quad_vb == nullptr ||
	    quad_ib == nullptr) {
		std::fprintf(stderr, "buffer creation failed\n");
		return 1;
	}

	// --- the frame, in the engine's call order ------------------------------
	int frames = headless ? 1 : 240;
	for (int frame = 0; frame < frames; ++frame) {
#ifdef SPIKE_WITH_SDL
		if (!headless) {
			SDL_Event event;
			while (SDL_PollEvent(&event)) {
				if (event.type == SDL_QUIT ||
				    (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE)) {
					frames = 0;
				}
			}
		}
#endif
		gfx->Begin_Scene();
		gfx->Clear(true, true, 0.06f, 0.07f, 0.10f, 1.0f);

		// pass 1: textured triangle, texture MODULATE diffuse
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

		// pass 2: pretransformed, alpha-blended, diffuse-only quad
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

		// No flip: this spike is headless, so there is no swapchain and nothing to present to.
		// Its proof is the framebuffer readback below, and Present() now says so rather than
		// reporting success for a presentation that did not happen.
		gfx->End_Scene(false);
	}

	// --- proof --------------------------------------------------------------
	std::string rgba;
	SurfaceFormat format;
	if (!gfx->Read_Back_Color_Target(rgba, format)) {
		std::fprintf(stderr, "Read_Back_Color_Target failed\n");
		return 1;
	}
	if (!Write_Png(out_path, rgba, format.width, format.height)) {
		std::fprintf(stderr, "failed to write %s\n", out_path.c_str());
		return 1;
	}
	std::printf("wrote %s (%ux%u), %u VkPipeline(s) created for 2 draws\n", out_path.c_str(),
	            format.width, format.height, gfx->Pipeline_Count());

	// Sanity check the readback rather than trusting the eye: the centre pixel must
	// be part of the textured triangle, not the clear colour.
	const size_t centre = (static_cast<size_t>(format.height / 2) * format.width +
	                       format.width / 2) * 4;
	const unsigned char* px = reinterpret_cast<const unsigned char*>(rgba.data()) + centre;
	std::printf("centre pixel rgba = %u,%u,%u,%u\n", px[0], px[1], px[2], px[3]);
	const bool centre_is_clear_colour = px[0] < 30 && px[1] < 30 && px[2] < 40;
	if (centre_is_clear_colour) {
		std::fprintf(stderr, "FAIL: nothing was rasterised at the centre of the target\n");
		return 1;
	}

	const uint32_t validation_messages = gfx->Validation_Message_Count();
	std::printf("validation messages: %u\n", validation_messages);
	if (validation && validation_messages != 0) {
		std::fprintf(stderr, "FAIL: %u validation warning(s)/error(s)\n", validation_messages);
		return 1;
	}

	gfx->Shutdown();
	delete gfx;
#ifdef SPIKE_WITH_SDL
	if (window != nullptr) {
		SDL_DestroyWindow(static_cast<SDL_Window*>(window));
		SDL_Quit();
	}
#endif
	std::printf("OK\n");
	return 0;
}
