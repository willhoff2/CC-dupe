// Renderer spike: the text path's per-frame resources, created and released every frame, with
// the backend's live-resource counts asserted to return to where they started.
//
// Render2DSentenceClass::Build_Textures composes each string it draws into a system-memory
// A4R4G4B4 surface, creates a one-level A4R4G4B4 texture, asks that texture for its level-0
// surface, CopyRects the glyphs across, draws with the texture, and then releases all three:
// the level surface (REF_PTR_RELEASE(texture_surface)), the system-memory surface
// (REF_PTR_RELEASE(curr_surface)) and finally the texture (REF_PTR_RELEASE(new_texture)).
// The retail shell and the in-game HUD do this for every string every frame it changes, so
// whatever those last Release() calls do NOT free accumulates at tens of objects per second
// for the rest of the session (docs/porting/renderer-resource-lifetime.md).
//
// This workload is that sequence against the backend directly, K strings per frame for N
// frames, in exactly the order and with exactly the entry points the seam uses:
//   Create_Image_Surface -> Surface_Bits (fill) -> Create_Lockable_Texture ->
//   Get_Surface_Level -> Copy_Rects -> Set_Texture + draw -> readback ->
//   Destroy_Surface(image) ; Destroy_Texture(texture)
// Every frame's textures carry a frame-specific texel that is read back from the target, so a
// frame that drew a stale or missing texture is a failure, not a pass with fewer objects. The
// destroy calls come *after* the draw, which is the engine's order and the one that needs the
// backend to defer the free until that frame's GPU work is done.
//
// Exit status is 0 only when every texel was verified, the backend's live texture and surface
// counts are back to what they were before the first frame, created == destroyed for both,
// nothing is left pending after one more frame, and the validation layer (when requested) was
// loaded and silent. The negative control, required by scripts/ci/check-resource-lifetime.py:
// with ZH_RENDER_NO_RESOURCE_DESTROY set the backend keeps every resource until Shutdown, as it
// did before it had a destroy path, and the live counts end N*K (textures) and 2*N*K
// (surfaces) above their bound.
//
// The cost of Get_Surface_Level is timed separately, first window against last window, because
// its linear scan over the live surface list is a second, distinct defect: a scan that grows
// with a leak is a symptom of the leak, and a scan fixed on its own would hide one.

#include "render_backend.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace spike;

namespace {

constexpr uint32_t kWidth = 256;
constexpr uint32_t kHeight = 256;
// The text path's smallest texture: Allocate_New_Surface picks 64, 128 or 256 square.
constexpr uint32_t kTextSize = 64;
constexpr uint32_t kStringsPerFrame = 8;
constexpr uint32_t kWindowFrames = 10;

struct ScreenVertex {
	float x, y, z, rhw;
	uint32_t diffuse;
	float u, v;
};
constexpr uint32_t kQuadFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

struct Rgba {
	int r = 0, g = 0, b = 0, a = 0;
};

// One A4R4G4B4 texel per (frame, string), distinct from the previous frame's in every
// channel so a stale texture cannot pass.
uint16_t Texel_For(uint32_t frame, uint32_t k) {
	const uint32_t r = (frame * 3u + k) & 0xFu;
	const uint32_t g = (frame * 5u + 2u * k + 7u) & 0xFu;
	const uint32_t b = (frame * 7u + 3u * k + 11u) & 0xFu;
	return static_cast<uint16_t>((0xFu << 12) | (r << 8) | (g << 4) | b);
}

Rgba Expand_4444(uint16_t v) {
	return Rgba{static_cast<int>((v >> 8) & 0xF) * 17, static_cast<int>((v >> 4) & 0xF) * 17,
	            static_cast<int>(v & 0xF) * 17, static_cast<int>((v >> 12) & 0xF) * 17};
}

// The strings' quads tile a row across the target; each is kTextSize wide so the readback
// samples a texel well inside its own quad.
void Quad_Rect(uint32_t k, float& x0, float& y0, float& x1, float& y1) {
	const uint32_t cols = kWidth / kTextSize;
	const uint32_t col = k % cols;
	const uint32_t row = k / cols;
	x0 = static_cast<float>(col * kTextSize);
	y0 = static_cast<float>(row * kTextSize);
	x1 = x0 + static_cast<float>(kTextSize);
	y1 = y0 + static_cast<float>(kTextSize);
}

struct FrameResources {
	SurfaceHandle* image = nullptr;
	TextureHandle* texture = nullptr;
	SurfaceHandle* level = nullptr;
};

class Workload {
public:
	explicit Workload(RenderBackend& gfx) : gfx_(gfx) {}
	bool Init();
	// Builds this frame's K textures the way Build_Textures does, draws them, reads the
	// target back and verifies each string's texel, then releases the frame's resources.
	bool Render_Frame(uint32_t frame);
	double Surface_Level_Ns_Per_Call() const {
		return surface_level_calls_ == 0 ? 0.0
		                                 : static_cast<double>(surface_level_ns_) /
		                                       static_cast<double>(surface_level_calls_);
	}
	void Reset_Surface_Level_Timer() {
		surface_level_ns_ = 0;
		surface_level_calls_ = 0;
	}
	uint32_t Texels_Verified() const { return texels_verified_; }

private:
	Rgba Pixel(uint32_t x, uint32_t y) const;

	RenderBackend& gfx_;
	VertexBufferHandle* vb_ = nullptr;
	IndexBufferHandle* ib_ = nullptr;
	std::string pixels_;
	SurfaceFormat format_{};
	uint64_t surface_level_ns_ = 0;
	uint64_t surface_level_calls_ = 0;
	uint32_t texels_verified_ = 0;
};

bool Workload::Init() {
	// One static quad per string; the textures are what change per frame, as in the engine
	// where the sentence's quads come from a dynamic ring and only the textures are new.
	std::vector<ScreenVertex> vertices(kStringsPerFrame * 4);
	std::vector<uint16_t> indices(kStringsPerFrame * 6);
	for (uint32_t k = 0; k < kStringsPerFrame; ++k) {
		float x0, y0, x1, y1;
		Quad_Rect(k, x0, y0, x1, y1);
		const ScreenVertex quad[4] = {
		    {x0, y0, 0.5f, 1.0f, 0xffffffffu, 0.0f, 0.0f},
		    {x1, y0, 0.5f, 1.0f, 0xffffffffu, 1.0f, 0.0f},
		    {x1, y1, 0.5f, 1.0f, 0xffffffffu, 1.0f, 1.0f},
		    {x0, y1, 0.5f, 1.0f, 0xffffffffu, 0.0f, 1.0f},
		};
		std::memcpy(&vertices[k * 4], quad, sizeof(quad));
		const uint16_t base = static_cast<uint16_t>(k * 4);
		const uint16_t tri[6] = {base, static_cast<uint16_t>(base + 1),
		                         static_cast<uint16_t>(base + 2), base,
		                         static_cast<uint16_t>(base + 2), static_cast<uint16_t>(base + 3)};
		std::memcpy(&indices[k * 6], tri, sizeof(tri));
	}
	vb_ = gfx_.Create_Vertex_Buffer(vertices.data(), vertices.size() * sizeof(ScreenVertex),
	                                kQuadFvf);
	ib_ = gfx_.Create_Index_Buffer(indices.data(), indices.size());
	if (vb_ == nullptr || ib_ == nullptr) {
		std::fprintf(stderr, "resource-lifetime: quad buffer creation failed\n");
		return false;
	}

	gfx_.Set_DX8_Render_State(D3DRS_ZENABLE, 0);
	gfx_.Set_DX8_Render_State(D3DRS_ZWRITEENABLE, 0);
	gfx_.Set_DX8_Render_State(D3DRS_CULLMODE, D3DCULL_NONE);
	gfx_.Set_DX8_Render_State(D3DRS_ALPHABLENDENABLE, 0);
	gfx_.Set_DX8_Render_State(D3DRS_ALPHATESTENABLE, 0);
	gfx_.Set_DX8_Render_State(D3DRS_LIGHTING, 0);
	gfx_.Set_DX8_Render_State(D3DRS_COLORWRITEENABLE, 0xf);
	gfx_.Set_Transform(D3DTS_WORLD, Matrix4x4::Identity());
	gfx_.Set_Transform(D3DTS_VIEW, Matrix4x4::Identity());
	gfx_.Set_Transform(D3DTS_PROJECTION, Matrix4x4::Identity());
	for (uint32_t stage = 0; stage < 8; ++stage) {
		gfx_.Set_DX8_Texture_Stage_State(stage, D3DTSS_COLOROP, D3DTOP_DISABLE);
		gfx_.Set_DX8_Texture_Stage_State(stage, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
		gfx_.Set_DX8_Texture_Stage_State(stage, D3DTSS_MINFILTER, D3DTEXF_POINT);
		gfx_.Set_DX8_Texture_Stage_State(stage, D3DTSS_MAGFILTER, D3DTEXF_POINT);
		gfx_.Set_DX8_Texture_Stage_State(stage, D3DTSS_MIPFILTER, D3DTEXF_NONE);
		gfx_.Set_DX8_Texture_Stage_State(stage, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
		gfx_.Set_DX8_Texture_Stage_State(stage, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
	}
	gfx_.Set_DX8_Texture_Stage_State(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
	gfx_.Set_DX8_Texture_Stage_State(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
	gfx_.Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
	gfx_.Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
	return true;
}

Rgba Workload::Pixel(uint32_t x, uint32_t y) const {
	const size_t offset = (static_cast<size_t>(y) * format_.width + x) * 4;
	if (offset + 3 >= pixels_.size()) return Rgba{};
	const auto* p = reinterpret_cast<const unsigned char*>(pixels_.data()) + offset;
	return Rgba{p[0], p[1], p[2], p[3]};
}

bool Workload::Render_Frame(uint32_t frame) {
	FrameResources res[kStringsPerFrame];
	bool ok = true;

	// Build_Textures, per string.
	for (uint32_t k = 0; k < kStringsPerFrame && ok; ++k) {
		FrameResources& r = res[k];
		r.image = gfx_.Create_Image_Surface(kTextSize, kTextSize, TextureFormat::A4R4G4B4);
		if (r.image == nullptr) {
			std::fprintf(stderr, "resource-lifetime: frame %u string %u: Create_Image_Surface "
			                     "failed\n", frame, k);
			ok = false;
			break;
		}
		LockedRect locked;
		if (!gfx_.Surface_Bits(r.image, locked) || locked.pitch < kTextSize * 2) {
			std::fprintf(stderr, "resource-lifetime: frame %u string %u: Surface_Bits failed\n",
			             frame, k);
			ok = false;
			break;
		}
		const uint16_t texel = Texel_For(frame, k);
		for (uint32_t y = 0; y < kTextSize; ++y) {
			auto* row = static_cast<uint8_t*>(locked.bits) + static_cast<size_t>(y) * locked.pitch;
			for (uint32_t x = 0; x < kTextSize; ++x) std::memcpy(row + x * 2, &texel, 2);
		}
		r.texture = gfx_.Create_Lockable_Texture(kTextSize, kTextSize, TextureFormat::A4R4G4B4, 1);
		if (r.texture == nullptr) {
			std::fprintf(stderr, "resource-lifetime: frame %u string %u: Create_Lockable_Texture "
			                     "failed\n", frame, k);
			ok = false;
			break;
		}
		const auto t0 = std::chrono::steady_clock::now();
		r.level = gfx_.Get_Surface_Level(r.texture, 0);
		const auto t1 = std::chrono::steady_clock::now();
		surface_level_ns_ += static_cast<uint64_t>(
		    std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
		++surface_level_calls_;
		if (r.level == nullptr) {
			std::fprintf(stderr, "resource-lifetime: frame %u string %u: Get_Surface_Level "
			                     "failed\n", frame, k);
			ok = false;
			break;
		}
		if (!gfx_.Copy_Rects(r.image, nullptr, 0, r.level, nullptr)) {
			std::fprintf(stderr, "resource-lifetime: frame %u string %u: Copy_Rects failed\n",
			             frame, k);
			ok = false;
			break;
		}
	}

	if (ok) {
		gfx_.Begin_Scene();
		gfx_.Clear(true, true, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0);
		gfx_.Set_Vertex_Buffer(vb_, 0);
		gfx_.Set_Index_Buffer(ib_, 0);
		for (uint32_t k = 0; k < kStringsPerFrame; ++k) {
			gfx_.Set_Texture(0, res[k].texture);
			gfx_.Draw_Triangles(k * 6, 2, k * 4, 4);
		}
		gfx_.Set_Texture(0, nullptr);
		gfx_.End_Scene(false);
		if (!gfx_.Read_Back_Color_Target(pixels_, format_)) {
			std::fprintf(stderr, "resource-lifetime: frame %u: readback failed\n", frame);
			ok = false;
		}
	}

	if (ok) {
		const float sx = static_cast<float>(format_.width) / static_cast<float>(kWidth);
		const float sy = static_cast<float>(format_.height) / static_cast<float>(kHeight);
		for (uint32_t k = 0; k < kStringsPerFrame; ++k) {
			float x0, y0, x1, y1;
			Quad_Rect(k, x0, y0, x1, y1);
			const Rgba actual = Pixel(static_cast<uint32_t>((x0 + x1) * 0.5f * sx),
			                          static_cast<uint32_t>((y0 + y1) * 0.5f * sy));
			const Rgba expected = Expand_4444(Texel_For(frame, k));
			// 4-bit texels expanded by the device: within one 8-bit step of n*17.
			const int d = std::abs(actual.r - expected.r) + std::abs(actual.g - expected.g) +
			              std::abs(actual.b - expected.b) + std::abs(actual.a - expected.a);
			if (d > 8) {
				std::fprintf(stderr, "resource-lifetime: frame %u string %u: texel mismatch, got "
				                     "%d,%d,%d,%d expected %d,%d,%d,%d\n", frame, k, actual.r,
				             actual.g, actual.b, actual.a, expected.r, expected.g, expected.b,
				             expected.a);
				ok = false;
			} else {
				++texels_verified_;
			}
		}
	}

	// The engine's release order: the level surface is REF_PTR_RELEASEd first (which in D3D8
	// frees nothing -- the texture owns it), then the system-memory surface, then the texture
	// once the sentence has been drawn. Destroy_Surface on a level surface is the no-op D3D8
	// makes it, and is called here so that contract is exercised too.
	for (uint32_t k = 0; k < kStringsPerFrame; ++k) {
		FrameResources& r = res[k];
		if (r.level != nullptr) gfx_.Destroy_Surface(r.level);
		if (r.image != nullptr) gfx_.Destroy_Surface(r.image);
		if (r.texture != nullptr) gfx_.Destroy_Texture(r.texture);
	}
	return ok;
}

void Print_Stats(const char* label, const ResourceStats& s) {
	std::printf("%s textures created %llu destroyed %llu live %u\n", label,
	            static_cast<unsigned long long>(s.textures_created),
	            static_cast<unsigned long long>(s.textures_destroyed), s.live_textures);
	std::printf("%s surfaces created %llu destroyed %llu live %u\n", label,
	            static_cast<unsigned long long>(s.surfaces_created),
	            static_cast<unsigned long long>(s.surfaces_destroyed), s.live_surfaces);
	std::printf("%s retired pending %u\n", label, s.retired_pending);
}

} // namespace

int main(int argc, char** argv) {
	bool validation = false;
	uint32_t frames = 300;
	for (int i = 1; i < argc; ++i) {
		if (std::strcmp(argv[i], "--validation") == 0) validation = true;
		else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
			frames = static_cast<uint32_t>(std::atoi(argv[++i]));
		else if (std::strcmp(argv[i], "--help") == 0) {
			std::printf("usage: %s [--frames N] [--validation]\n", argv[0]);
			return 0;
		}
	}
	if (frames < 2 * kWindowFrames) frames = 2 * kWindowFrames;

	RenderBackend* gfx = Create_Vulkan_Backend(validation, true);
	if (!gfx->Init(nullptr, kWidth, kHeight)) {
		std::fprintf(stderr, "resource-lifetime: backend Init failed\n");
		return 1;
	}
	std::printf("device: %s\n", gfx->Device_Description());
	if (!gfx->Supports_Texture_Format(TextureFormat::A4R4G4B4)) {
		std::fprintf(stderr, "resource-lifetime: device has no A4R4G4B4 path\n");
		return 1;
	}

	Workload w(*gfx);
	if (!w.Init()) {
		std::printf("resource-lifetime: FAIL\n");
		return 1;
	}
	std::printf("workload: %u frames x %u strings of %ux%u A4R4G4B4\n", frames, kStringsPerFrame,
	            kTextSize, kTextSize);

	const ResourceStats before = gfx->Get_Resource_Stats();
	Print_Stats("before", before);

	int status = 0;
	double first_window_ns = 0.0, last_window_ns = 0.0;
	for (uint32_t frame = 0; frame < frames; ++frame) {
		if (frame == 0 || frame == frames - kWindowFrames) w.Reset_Surface_Level_Timer();
		if (!w.Render_Frame(frame)) {
			status = 1;
			break;
		}
		if (frame + 1 == kWindowFrames) first_window_ns = w.Surface_Level_Ns_Per_Call();
		if (frame + 1 == frames) last_window_ns = w.Surface_Level_Ns_Per_Call();
		if ((frame + 1) % 100 == 0 || frame + 1 == frames) {
			const ResourceStats s = gfx->Get_Resource_Stats();
			std::printf("frame %u live textures %u live surfaces %u retired pending %u\n",
			            frame + 1, s.live_textures, s.live_surfaces, s.retired_pending);
		}
	}

	// One more frame with nothing created: whatever the last frame retired is freed by it.
	gfx->Begin_Scene();
	gfx->Clear(true, true, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0);
	gfx->End_Scene(false);

	const ResourceStats after = gfx->Get_Resource_Stats();
	Print_Stats("after", after);
	std::printf("texels verified %u of %u\n", w.Texels_Verified(), frames * kStringsPerFrame);
	std::printf("get_surface_level ns/call first %u frames %.0f last %u frames %.0f\n",
	            kWindowFrames, first_window_ns, kWindowFrames, last_window_ns);

	const uint64_t expected_textures = static_cast<uint64_t>(frames) * kStringsPerFrame;
	const uint64_t expected_surfaces = expected_textures * 2; // image + level 0
	if (w.Texels_Verified() != frames * kStringsPerFrame) status = 1;
	if (after.textures_created - before.textures_created != expected_textures) {
		std::fprintf(stderr, "resource-lifetime: %llu textures created, expected %llu\n",
		             static_cast<unsigned long long>(after.textures_created -
		                                             before.textures_created),
		             static_cast<unsigned long long>(expected_textures));
		status = 1;
	}
	if (after.surfaces_created - before.surfaces_created != expected_surfaces) {
		std::fprintf(stderr, "resource-lifetime: %llu surfaces created, expected %llu\n",
		             static_cast<unsigned long long>(after.surfaces_created -
		                                             before.surfaces_created),
		             static_cast<unsigned long long>(expected_surfaces));
		status = 1;
	}
	if (after.live_textures > before.live_textures) {
		std::fprintf(stderr, "resource-lifetime: live textures %u exceed bound %u\n",
		             after.live_textures, before.live_textures);
		status = 1;
	}
	if (after.live_surfaces > before.live_surfaces) {
		std::fprintf(stderr, "resource-lifetime: live surfaces %u exceed bound %u\n",
		             after.live_surfaces, before.live_surfaces);
		status = 1;
	}
	if (after.textures_destroyed - before.textures_destroyed != expected_textures ||
	    after.surfaces_destroyed - before.surfaces_destroyed != expected_surfaces) {
		std::fprintf(stderr, "resource-lifetime: destroyed %llu textures / %llu surfaces, "
		                     "expected %llu / %llu\n",
		             static_cast<unsigned long long>(after.textures_destroyed -
		                                             before.textures_destroyed),
		             static_cast<unsigned long long>(after.surfaces_destroyed -
		                                             before.surfaces_destroyed),
		             static_cast<unsigned long long>(expected_textures),
		             static_cast<unsigned long long>(expected_surfaces));
		status = 1;
	}
	if (after.retired_pending != 0) {
		std::fprintf(stderr, "resource-lifetime: %u resources still pending after an idle frame\n",
		             after.retired_pending);
		status = 1;
	}

	std::printf("validation layer: %s\n", gfx->Validation_Active() ? "loaded" : "not requested");
	std::printf("validation messages: %u\n", gfx->Validation_Message_Count());
	if (validation && !gfx->Validation_Active()) status = 1;
	if (gfx->Validation_Message_Count() != 0) status = 1;

	gfx->Shutdown();
	delete gfx;
	std::printf("%s\n", status == 0 ? "resource-lifetime: PASS" : "resource-lifetime: FAIL");
	return status;
}
