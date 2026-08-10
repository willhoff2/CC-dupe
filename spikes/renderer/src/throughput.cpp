// Renderer spike: a crude throughput baseline, so the Mac has a Linux number to be
// compared against later.
//
// Three numbers, in the order they matter for the port:
//   1. draw calls/sec  -- one pipeline, one descriptor set write per draw, which is
//      what the backend does for every DX8Wrapper::Draw_Triangles.
//   2. state changes/sec -- the same draws with a render state changed in between,
//      so each draw re-derives its pipeline key and hits the pipeline cache.
//   3. pipeline creation time -- the one that decides whether MoltenVK's shader
//      compile hitch is a problem. docs/porting/fixed-function-measurements.md
//      estimates how many pipelines a real frame needs; this says what each costs.
//
// These are CPU-side submission rates on a 64x64 target with trivial geometry: the
// point is the per-call overhead of the translation layer, not fill rate.

#include "render_backend.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace spike;

namespace {

constexpr uint32_t kWidth = 64;
constexpr uint32_t kHeight = 64;
// The backend keeps one descriptor set per draw per frame; see kMaxDrawsPerFrame.
constexpr uint32_t kDrawsPerFrame = 64;

struct Vertex {
	float x, y, z, rhw;
	uint32_t diffuse;
};

double Seconds_Since(const std::chrono::steady_clock::time_point& start) {
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

void Set_Minimal_Cascade(RenderBackend& g) {
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
	for (uint32_t stage = 1; stage < 8; ++stage) {
		g.Set_DX8_Texture_Stage_State(stage, D3DTSS_COLOROP, D3DTOP_DISABLE);
		g.Set_DX8_Texture_Stage_State(stage, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
	}
	g.Set_DX8_Render_State(D3DRS_CULLMODE, D3DCULL_NONE);
	g.Set_DX8_Render_State(D3DRS_ZENABLE, 0);
	g.Set_DX8_Render_State(D3DRS_LIGHTING, 0);
}

} // namespace

int main(int argc, char** argv) {
	uint32_t frames = 200;
	uint32_t pipeline_samples = 64;
	for (int i = 1; i < argc; ++i) {
		if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
			frames = static_cast<uint32_t>(std::atoi(argv[++i]));
		else if (std::strcmp(argv[i], "--pipelines") == 0 && i + 1 < argc)
			pipeline_samples = static_cast<uint32_t>(std::atoi(argv[++i]));
		else if (std::strcmp(argv[i], "--help") == 0) {
			std::printf("usage: %s [--frames N] [--pipelines N]\n", argv[0]);
			return 0;
		}
	}

	// Validation off: it is a debug tool and it dominates the submission cost.
	RenderBackend* gfx = Create_Vulkan_Backend(false, true);
	if (!gfx->Init(nullptr, kWidth, kHeight)) {
		std::fprintf(stderr, "backend Init failed\n");
		return 1;
	}
	std::printf("device: %s\n", gfx->Device_Description());
	std::printf("%u frames x %u draws, 64x64 target, validation off\n\n", frames,
	            kDrawsPerFrame);

	const float w = static_cast<float>(kWidth), h = static_cast<float>(kHeight);
	const Vertex quad[4] = {{0, 0, 0.5f, 1, 0xffffffffu},
	                        {w, 0, 0.5f, 1, 0xffffffffu},
	                        {w, h, 0.5f, 1, 0xffffffffu},
	                        {0, h, 0.5f, 1, 0xffffffffu}};
	const uint16_t indices[6] = {0, 1, 2, 0, 2, 3};
	VertexBufferHandle* vb =
	    gfx->Create_Vertex_Buffer(quad, sizeof(quad), D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
	IndexBufferHandle* ib = gfx->Create_Index_Buffer(indices, 6);
	if (vb == nullptr || ib == nullptr) {
		std::fprintf(stderr, "buffer creation failed\n");
		return 1;
	}
	Set_Minimal_Cascade(*gfx);
	gfx->Set_Vertex_Buffer(vb, 0);
	gfx->Set_Index_Buffer(ib, 0);

	// --- 1. draw calls/sec ---------------------------------------------------
	{
		// One warm frame so pipeline creation is not counted.
		gfx->Begin_Scene();
		gfx->Draw_Triangles(0, 2, 0, 4);
		gfx->End_Scene(false);

		const auto start = std::chrono::steady_clock::now();
		for (uint32_t frame = 0; frame < frames; ++frame) {
			gfx->Begin_Scene();
			for (uint32_t draw = 0; draw < kDrawsPerFrame; ++draw)
				gfx->Draw_Triangles(0, 2, 0, 4);
			gfx->End_Scene(false);
		}
		const double elapsed = Seconds_Since(start);
		const double draws = static_cast<double>(frames) * kDrawsPerFrame;
		std::printf("draw calls          %10.0f /sec  (%.2f us each)\n", draws / elapsed,
		            elapsed / draws * 1e6);
	}

	// --- 2. state changes/sec ------------------------------------------------
	{
		// Alternate between two blend states, so every draw re-derives its pipeline
		// key and looks the pipeline up. Two pipelines, not two thousand: this is the
		// cost of the *lookup* and the re-bind, not of compilation.
		const auto start = std::chrono::steady_clock::now();
		for (uint32_t frame = 0; frame < frames; ++frame) {
			gfx->Begin_Scene();
			for (uint32_t draw = 0; draw < kDrawsPerFrame; ++draw) {
				gfx->Set_DX8_Render_State(D3DRS_ALPHABLENDENABLE, draw & 1u);
				gfx->Set_DX8_Render_State(D3DRS_SRCBLEND,
				                          (draw & 1u) ? D3DBLEND_SRCALPHA : D3DBLEND_ONE);
				gfx->Set_DX8_Render_State(D3DRS_DESTBLEND,
				                          (draw & 1u) ? D3DBLEND_INVSRCALPHA : D3DBLEND_ZERO);
				gfx->Draw_Triangles(0, 2, 0, 4);
			}
			gfx->End_Scene(false);
		}
		const double elapsed = Seconds_Since(start);
		const double changes = static_cast<double>(frames) * kDrawsPerFrame * 3.0;
		std::printf("state changes       %10.0f /sec  (3 per draw, pipeline cache hit)\n",
		            changes / elapsed);
	}

	// --- 3. pipeline creation ------------------------------------------------
	{
		// Force `pipeline_samples` distinct pipeline keys by walking the stencil
		// reference value, which is baked into the pipeline (D3D8 has no notion of a
		// pipeline, so every one of these is a state the engine can set for free).
		const uint32_t before = gfx->Pipeline_Count();
		gfx->Set_DX8_Render_State(D3DRS_ALPHABLENDENABLE, 0);
		gfx->Set_DX8_Render_State(D3DRS_STENCILENABLE, 1);
		const auto start = std::chrono::steady_clock::now();
		uint32_t created = 0;
		for (uint32_t i = 0; i < pipeline_samples; i += kDrawsPerFrame) {
			gfx->Begin_Scene();
			for (uint32_t draw = 0; draw < kDrawsPerFrame && i + draw < pipeline_samples;
			     ++draw) {
				gfx->Set_DX8_Render_State(D3DRS_STENCILREF, i + draw + 1);
				gfx->Draw_Triangles(0, 2, 0, 4);
				++created;
			}
			gfx->End_Scene(false);
		}
		const double elapsed = Seconds_Since(start);
		const uint32_t actually_created = gfx->Pipeline_Count() - before;
		std::printf("pipeline creation   %10.2f ms each (%u created of %u draws)\n",
		            elapsed / (actually_created ? actually_created : 1) * 1e3,
		            actually_created, created);
		std::printf("                    %10.2f s   to warm the estimated 32,256-pipeline"
		            " upper bound\n",
		            elapsed / (actually_created ? actually_created : 1) * 32256.0);
	}

	gfx->Shutdown();
	delete gfx;
	return 0;
}
