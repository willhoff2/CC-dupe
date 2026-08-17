// Renderer spike: what a frame's worth of D3D8 locks costs in host-visible memory.
//
// docs/porting/renderer-resource-seam.md classifies the engine's 95 Lock/Unlock call
// sites into 8 usage classes. This driver replays that classification as a repeating
// frame: the per-class operation counts are the measured site counts, and the
// resource sizes are the ones §3 read out of the engine (terrain tiles, the loader's
// mip chains, the shadow/decal/water dynamic buffers). It is deliberately not a best
// case -- the heavy C5 stream, the per-frame read-back and two loader chains held
// across the whole frame all run every frame.
//
// The output is the measurement: peak and steady-state staging bytes, the number of
// host-visible allocations, and how many lock acquires the pool served from a freed
// block. `scripts/ci/check-staging-cost.py` compares it against a committed ceiling,
// so a change that reintroduces per-lock allocation fails CI rather than being
// noticed later.
//
// Run with ZH_SPIKE_STAGING_RETAIN=1 for the pre-pool behaviour (every resource pins
// its staging for its lifetime), which is the "before" column in the doc.

#include "render_backend.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace spike;

namespace {

constexpr uint32_t kTargetWidth = 64;
constexpr uint32_t kTargetHeight = 64;

// Per-frame operation counts. These are the §3 site counts per class, used directly
// as "operations per frame" so the mix is the engine's, not a chosen one. C6 is a
// load-time class, so its 12 run once; C7's 2 and C8's 3 are resources whose pointer
// the engine keeps for their lifetime, so they are set up once and held.
constexpr uint32_t kC1PerFrame = 18;
constexpr uint32_t kC2PerFrame = 4;
constexpr uint32_t kC3PerFrame = 9;
constexpr uint32_t kC4PerFrame = 8;
constexpr uint32_t kC5PerFrame = 39;
constexpr uint32_t kC6Once = 12;
constexpr uint32_t kC7Held = 2;
constexpr uint32_t kC8Held = 3;

struct Surface {
	TextureHandle* texture = nullptr;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t levels = 1;
};

// D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1: the shadow, water and snow streams
// all use a pretransformed vertex, and this is the spike's version of one.
struct ScreenVertex {
	float x, y, z, rhw;
	uint32_t diffuse;
	float u, v;
};

constexpr uint32_t kQuadFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

void Fill_Level(const LockedRect& locked, uint32_t width, uint32_t height,
                uint32_t texel_bytes, uint32_t seed) {
	auto* bytes = static_cast<uint8_t*>(locked.bits);
	for (uint32_t y = 0; y < height; ++y) {
		auto* row = bytes + static_cast<size_t>(y) * locked.pitch;
		if (texel_bytes == 4) {
			auto* argb = reinterpret_cast<uint32_t*>(row);
			for (uint32_t x = 0; x < width; ++x) {
				argb[x] = 0xff000000u | ((seed + x) & 0xffu) << 16 |
				          ((seed + y) & 0xffu) << 8 | ((x ^ y) & 0xffu);
			}
		} else {
			for (uint32_t x = 0; x < width; ++x) {
				row[x * texel_bytes] = static_cast<uint8_t>((seed + x + y) & 0xff);
			}
		}
	}
}

struct Report {
	ResourceStats stats;
	uint32_t frames = 0;
	uint32_t texture_locks = 0;
	uint32_t buffer_locks = 0;
	uint64_t steady_state_bytes = 0; // staging checked out at a frame boundary
	uint32_t validation_messages = 0;
	// Whether the layer really loaded: zero messages from an unvalidated run proves nothing
	// (docs/porting/apple-silicon-verification.md 8.1).
	bool validation_active = false;
	bool pixels_ok = false;
	bool retain_mode = false;
	bool view_swizzle = true;
};

class Workload {
public:
	explicit Workload(RenderBackend* backend) : gfx_(backend) {}

	bool Setup();
	bool Run_Frame(uint32_t frame);
	bool Check_Pixels();

	uint32_t texture_locks = 0;
	uint32_t buffer_locks = 0;

private:
	bool Lock_Fill_Unlock(Surface& s, const LockRect* rect, uint32_t seed);
	bool Read_Back_Lock(Surface& s);
	bool Stream_Buffer(VertexBufferHandle* vb, size_t bytes, bool discard);

	RenderBackend* gfx_ = nullptr;
	// C1: terrain tiles and the tree/alpha-edge textures, at the sizes §3 records.
	std::vector<Surface> c1_;
	// C2: the surfaces SurfaceClass::Copy writes sub-rects of.
	std::vector<Surface> c2_;
	// C3: W3DSmudge's per-frame read-back.
	Surface c3_;
	// C4: loader tasks in flight, each holding a whole mip chain locked.
	std::vector<Surface> c4_;
	// C5: the shadow, decal and water dynamic buffers, plus WW3D2's shared one.
	std::vector<VertexBufferHandle*> c5_;
	std::vector<size_t> c5_capacity_;
	// C6: static geometry, filled once at load.
	std::vector<VertexBufferHandle*> c6_;
	// C7/C8: pointers the engine keeps for the resource's lifetime. At this seam that
	// is a lock that is never released, which is the only way a pooled block can stay
	// valid after Unlock without the seam being told which surfaces those are.
	std::vector<Surface> held_;
	Surface probe_;
	uint32_t probe_seed_ = 0;
};

bool Workload::Setup() {
	auto make = [&](uint32_t w, uint32_t h, TextureFormat f, uint32_t levels) {
		Surface s;
		s.texture = gfx_->Create_Lockable_Texture(w, h, f, levels);
		s.width = w;
		s.height = h;
		s.levels = levels;
		return s;
	};

	// TerrainTex.cpp's tiles are 256x256; the tree texture is 128x128; the alpha-edge
	// texture is smaller again. Six resources serve the 18 C1 locks per frame.
	for (uint32_t i = 0; i < 3; ++i) {
		c1_.push_back(make(256, 256, TextureFormat::A8R8G8B8, 1));
	}
	c1_.push_back(make(128, 128, TextureFormat::A8R8G8B8, 1));
	c1_.push_back(make(128, 128, TextureFormat::L8, 1)); // shroud-shaped, 1 byte/texel
	c1_.push_back(make(64, 64, TextureFormat::A8R8G8B8, 1));
	for (uint32_t i = 0; i < 2; ++i) {
		c2_.push_back(make(256, 256, TextureFormat::A8R8G8B8, 1));
	}
	c3_ = make(128, 128, TextureFormat::A8R8G8B8, 1);
	// Two loader tasks in flight, each a full 256x256 chain: the class the doc says
	// bounds its cost by the loader's in-flight task count.
	for (uint32_t i = 0; i < 2; ++i) {
		c4_.push_back(make(256, 256, TextureFormat::A8R8G8B8, 9));
	}
	for (uint32_t i = 0; i < kC7Held + kC8Held; ++i) {
		held_.push_back(make(128, 128, TextureFormat::A8R8G8B8, 1));
	}
	probe_ = make(16, 16, TextureFormat::A8R8G8B8, 1);

	for (const Surface& s : c1_) {
		if (s.texture == nullptr) return false;
	}
	for (const Surface& s : c2_) {
		if (s.texture == nullptr) return false;
	}
	for (const Surface& s : c4_) {
		if (s.texture == nullptr) return false;
	}
	for (const Surface& s : held_) {
		if (s.texture == nullptr) return false;
	}
	if (c3_.texture == nullptr || probe_.texture == nullptr) return false;

	// The dynamic buffers of §3: SHADOW_VERTEX_SIZE 4096x16 B, the decal buffer
	// 32768x24 B, the water mesh, and WW3D2's shared DEFAULT_VB_SIZE buffer.
	const size_t capacities[] = {4096 * 16, 32768 * 24, 5000 * sizeof(ScreenVertex),
	                             5000 * sizeof(ScreenVertex)};
	for (size_t bytes : capacities) {
		VertexBufferHandle* vb = gfx_->Create_Dynamic_Vertex_Buffer(bytes, kQuadFvf);
		if (vb == nullptr) return false;
		c5_.push_back(vb);
		c5_capacity_.push_back(bytes);
	}

	// C6: static geometry written once. The seam has no separate static-lock entry
	// point, so these are dynamic buffers locked once with no flags, which is exactly
	// what VertexBufferClass::WriteLockClass does.
	for (uint32_t i = 0; i < kC6Once; ++i) {
		VertexBufferHandle* vb =
		    gfx_->Create_Dynamic_Vertex_Buffer(512 * sizeof(ScreenVertex), kQuadFvf);
		if (vb == nullptr) return false;
		c6_.push_back(vb);
		void* bits = nullptr;
		if (!gfx_->Lock_Vertex_Buffer(vb, 0, 512 * sizeof(ScreenVertex), LOCK_NONE,
		                              &bits)) {
			return false;
		}
		std::memset(bits, 0, 512 * sizeof(ScreenVertex));
		if (!gfx_->Unlock_Vertex_Buffer(vb)) return false;
		++buffer_locks;
	}

	// C7 and C8: locked once and never unlocked, so the pointer the engine keeps
	// stays valid for the resource's lifetime.
	for (Surface& s : held_) {
		LockedRect locked;
		if (!gfx_->Lock_Texture(s.texture, 0, nullptr, LOCK_NONE, locked)) return false;
		Fill_Level(locked, s.width, s.height, 4, 7);
		++texture_locks;
	}
	return true;
}

bool Workload::Lock_Fill_Unlock(Surface& s, const LockRect* rect, uint32_t seed) {
	LockedRect locked;
	if (!gfx_->Lock_Texture(s.texture, 0, rect, LOCK_NONE, locked)) return false;
	++texture_locks;
	const uint32_t w = rect != nullptr ? rect->right - rect->left : s.width;
	const uint32_t h = rect != nullptr ? rect->bottom - rect->top : s.height;
	Fill_Level(locked, w, h, locked.pitch / s.width >= 4 ? 4 : 1, seed);
	return gfx_->Unlock_Texture(s.texture, 0);
}

bool Workload::Read_Back_Lock(Surface& s) {
	LockedRect locked;
	if (!gfx_->Lock_Texture(s.texture, 0, nullptr, LOCK_READONLY, locked)) return false;
	++texture_locks;
	// Touch the bytes: the read is the point of the class, and it also keeps the
	// compiler from eliding the lock.
	volatile uint32_t sink = 0;
	const auto* row = static_cast<const uint8_t*>(locked.bits);
	for (uint32_t y = 0; y < s.height; y += 8) {
		sink += *reinterpret_cast<const uint32_t*>(row + static_cast<size_t>(y) * locked.pitch);
	}
	(void)sink;
	return gfx_->Unlock_Texture(s.texture, 0);
}

bool Workload::Stream_Buffer(VertexBufferHandle* vb, size_t bytes, bool discard) {
	void* bits = nullptr;
	if (!gfx_->Lock_Vertex_Buffer(vb, 0, bytes, discard ? LOCK_DISCARD : LOCK_NOOVERWRITE,
	                              &bits)) {
		return false;
	}
	++buffer_locks;
	std::memset(bits, 0x40, bytes);
	return gfx_->Unlock_Vertex_Buffer(vb);
}

bool Workload::Run_Frame(uint32_t frame) {
	gfx_->Begin_Scene();
	gfx_->Clear(true, true, 0.0f, 0.0f, 0.0f, 1.0f);

	// C4 first: the loader locks a whole chain and holds it while the rest of the
	// frame runs, which is what makes the peak a peak.
	std::vector<std::vector<LockedRect>> chains(c4_.size());
	for (size_t i = 0; i < c4_.size(); ++i) {
		chains[i].resize(c4_[i].levels);
		for (uint32_t level = 0; level < c4_[i].levels; ++level) {
			if (!gfx_->Lock_Texture(c4_[i].texture, level, nullptr, LOCK_NONE,
			                        chains[i][level])) {
				return false;
			}
			++texture_locks;
		}
	}

	for (uint32_t i = 0; i < kC1PerFrame; ++i) {
		Surface& s = c1_[i % c1_.size()];
		if (!Lock_Fill_Unlock(s, nullptr, frame + i)) return false;
	}
	for (uint32_t i = 0; i < kC2PerFrame; ++i) {
		Surface& s = c2_[i % c2_.size()];
		const LockRect rect{8, 8, 8 + 64, 8 + 64};
		if (!Lock_Fill_Unlock(s, &rect, frame + i)) return false;
	}
	for (uint32_t i = 0; i < kC3PerFrame; ++i) {
		if (!Read_Back_Lock(c3_)) return false;
	}
	for (uint32_t i = 0; i < kC5PerFrame; ++i) {
		VertexBufferHandle* vb = c5_[i % c5_.size()];
		const size_t capacity = c5_capacity_[i % c5_.size()];
		// The engine's own condition: rename on the frame's first lock of a buffer,
		// append afterwards.
		if (!Stream_Buffer(vb, capacity / 4, i < c5_.size())) return false;
	}

	// The probe texture is what the pixel assertion samples, so the workload proves
	// the pool is correct and not merely cheap.
	probe_seed_ = frame + 1;
	if (!Lock_Fill_Unlock(probe_, nullptr, probe_seed_)) return false;

	// Fill the loader chains from the kept pointers and release them, as End_Load
	// does.
	for (size_t i = 0; i < c4_.size(); ++i) {
		for (uint32_t level = 0; level < c4_[i].levels; ++level) {
			const uint32_t w = c4_[i].width >> level ? c4_[i].width >> level : 1u;
			const uint32_t h = c4_[i].height >> level ? c4_[i].height >> level : 1u;
			Fill_Level(chains[i][level], w, h, 4, frame + level);
		}
		for (uint32_t level = 0; level < c4_[i].levels; ++level) {
			if (!gfx_->Unlock_Texture(c4_[i].texture, level)) return false;
		}
	}
	// The remaining C4 sites per frame beyond the two chains: Load_Thumbnail, which
	// locks one level of a chain on its own.
	const uint32_t extra_c4 = kC4PerFrame - 2 * static_cast<uint32_t>(c4_.size());
	for (uint32_t i = 0; i < extra_c4; ++i) {
		LockedRect locked;
		Surface& s = c4_[i % c4_.size()];
		if (!gfx_->Lock_Texture(s.texture, 1, nullptr, LOCK_NONE, locked)) return false;
		++texture_locks;
		Fill_Level(locked, s.width / 2, s.height / 2, 4, frame);
		if (!gfx_->Unlock_Texture(s.texture, 1)) return false;
	}

	gfx_->End_Scene(false);
	return true;
}

bool Workload::Check_Pixels() {
	// One textured quad from the probe texture the last frame wrote through a pooled
	// block, read back and compared texel by texel.
	RenderBackend& g = *gfx_;
	g.Set_DX8_Render_State(D3DRS_ZENABLE, 0);
	g.Set_DX8_Render_State(D3DRS_ZWRITEENABLE, 0);
	g.Set_DX8_Render_State(D3DRS_CULLMODE, D3DCULL_NONE);
	g.Set_DX8_Render_State(D3DRS_LIGHTING, 0);
	g.Set_DX8_Render_State(D3DRS_COLORWRITEENABLE, 0xf);
	g.Set_Transform(D3DTS_WORLD, Matrix4x4::Identity());
	g.Set_Transform(D3DTS_VIEW, Matrix4x4::Identity());
	g.Set_Transform(D3DTS_PROJECTION, Matrix4x4::Identity());
	for (uint32_t stage = 0; stage < 8; ++stage) {
		g.Set_DX8_Texture_Stage_State(stage, D3DTSS_COLOROP, D3DTOP_DISABLE);
		g.Set_DX8_Texture_Stage_State(stage, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
		g.Set_DX8_Texture_Stage_State(stage, D3DTSS_MINFILTER, D3DTEXF_POINT);
		g.Set_DX8_Texture_Stage_State(stage, D3DTSS_MAGFILTER, D3DTEXF_POINT);
		g.Set_DX8_Texture_Stage_State(stage, D3DTSS_MIPFILTER, D3DTEXF_NONE);
		g.Set_DX8_Texture_Stage_State(stage, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
		g.Set_DX8_Texture_Stage_State(stage, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
		g.Set_Texture(stage, nullptr);
	}
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
	g.Set_Texture(0, probe_.texture);

	const uint16_t indices[6] = {0, 1, 2, 0, 2, 3};
	IndexBufferHandle* ib = g.Create_Index_Buffer(indices, 6);
	const float w = static_cast<float>(kTargetWidth);
	const float h = static_cast<float>(kTargetHeight);
	const ScreenVertex vertices[4] = {
	    {0.0f, 0.0f, 0.5f, 1.0f, 0xffffffffu, 0.0f, 0.0f},
	    {w, 0.0f, 0.5f, 1.0f, 0xffffffffu, 1.0f, 0.0f},
	    {w, h, 0.5f, 1.0f, 0xffffffffu, 1.0f, 1.0f},
	    {0.0f, h, 0.5f, 1.0f, 0xffffffffu, 0.0f, 1.0f},
	};
	VertexBufferHandle* vb = g.Create_Vertex_Buffer(vertices, sizeof(vertices), kQuadFvf);
	if (ib == nullptr || vb == nullptr) return false;

	g.Begin_Scene();
	g.Clear(true, true, 0.0f, 0.0f, 0.0f, 1.0f);
	g.Set_Vertex_Buffer(vb, 0);
	g.Set_Index_Buffer(ib, 0);
	g.Draw_Triangles(0, 2, 0, 4);
	g.End_Scene(false);

	std::string pixels;
	SurfaceFormat format{};
	if (!g.Read_Back_Color_Target(pixels, format)) return false;
	const auto* bytes = reinterpret_cast<const unsigned char*>(pixels.data());
	const uint32_t scale = kTargetWidth / 16;
	for (uint32_t ty = 0; ty < 16; ++ty) {
		for (uint32_t tx = 0; tx < 16; ++tx) {
			const uint32_t px = static_cast<uint32_t>((tx + 0.5f) * scale);
			const uint32_t py = static_cast<uint32_t>((ty + 0.5f) * scale);
			const size_t offset = (static_cast<size_t>(py) * format.width + px) * 4;
			if (offset + 3 >= pixels.size()) return false;
			const int r = static_cast<int>((probe_seed_ + tx) & 0xff);
			const int g8 = static_cast<int>((probe_seed_ + ty) & 0xff);
			const int b = static_cast<int>((tx ^ ty) & 0xff);
			if (std::abs(bytes[offset] - r) > 1 || std::abs(bytes[offset + 1] - g8) > 1 ||
			    std::abs(bytes[offset + 2] - b) > 1) {
				std::fprintf(stderr,
				             "probe texel (%u,%u): got (%d,%d,%d) expected (%d,%d,%d)\n",
				             tx, ty, bytes[offset], bytes[offset + 1], bytes[offset + 2], r,
				             g8, b);
				return false;
			}
		}
	}
	return true;
}

void Print_Json(const Report& r) {
	const double reuse_rate =
	    r.stats.staging_acquires != 0
	        ? static_cast<double>(r.stats.staging_reuses) / r.stats.staging_acquires
	        : 0.0;
	std::printf("{\n");
	std::printf("  \"mode\": \"%s\",\n", r.retain_mode ? "retain" : "pool");
	std::printf("  \"view_swizzle\": %s,\n", r.view_swizzle ? "true" : "false");
	std::printf("  \"frames\": %u,\n", r.frames);
	std::printf("  \"texture_locks\": %u,\n", r.texture_locks);
	std::printf("  \"buffer_locks\": %u,\n", r.buffer_locks);
	std::printf("  \"staging_allocations\": %u,\n", r.stats.staging_allocations);
	std::printf("  \"staging_resident_bytes\": %llu,\n",
	            static_cast<unsigned long long>(r.stats.staging_bytes));
	std::printf("  \"staging_peak_bytes\": %llu,\n",
	            static_cast<unsigned long long>(r.stats.staging_live_peak_bytes));
	std::printf("  \"staging_peak_blocks\": %u,\n", r.stats.staging_live_blocks_peak);
	std::printf("  \"staging_steady_state_bytes\": %llu,\n",
	            static_cast<unsigned long long>(r.steady_state_bytes));
	std::printf("  \"dynamic_buffer_allocations\": %u,\n",
	            r.stats.dynamic_buffer_allocations);
	std::printf("  \"dynamic_buffer_bytes\": %llu,\n",
	            static_cast<unsigned long long>(r.stats.dynamic_buffer_bytes));
	std::printf("  \"staging_retained_blocks\": %u,\n", r.stats.staging_retained_blocks);
	std::printf("  \"staging_acquires\": %u,\n", r.stats.staging_acquires);
	std::printf("  \"staging_reuses\": %u,\n", r.stats.staging_reuses);
	std::printf("  \"staging_reuse_rate\": %.4f,\n", reuse_rate);
	std::printf("  \"pool_free_blocks\": %u,\n", r.stats.staging_pool_blocks);
	std::printf("  \"pool_free_bytes\": %llu,\n",
	            static_cast<unsigned long long>(r.stats.staging_pool_bytes));
	std::printf("  \"texture_upload_regions\": %u,\n", r.stats.texture_upload_regions);
	std::printf("  \"upload_submits\": %u,\n", r.stats.upload_submits);
	std::printf("  \"readback_stalls\": %u,\n", r.stats.readback_stalls);
	// What the D3D8 preserve-on-lock contract costs the pool: the levels a lock had
	// to bring back because the block no longer held them, against the locks that
	// proved they did not need to.
	std::printf("  \"staging_preserve_readbacks\": %u,\n",
	            r.stats.staging_preserve_readbacks);
	std::printf("  \"staging_preserve_bytes\": %llu,\n",
	            static_cast<unsigned long long>(r.stats.staging_preserve_bytes));
	std::printf("  \"staging_preserve_skips\": %u,\n", r.stats.staging_preserve_skips);
	std::printf("  \"gpu_write_marks\": %u,\n", r.stats.gpu_write_marks);
	std::printf("  \"dirty_reads\": %u,\n", r.stats.dirty_reads);
	std::printf("  \"clean_reads\": %u,\n", r.stats.clean_reads);
	std::printf("  \"cpu_expansions\": %u,\n", r.stats.cpu_expansions);
	std::printf("  \"ring_discards\": %u,\n", r.stats.ring_discards);
	std::printf("  \"ring_appends\": %u,\n", r.stats.ring_appends);
	std::printf("  \"ring_bytes\": %llu,\n",
	            static_cast<unsigned long long>(r.stats.ring_bytes));
	std::printf("  \"ring_wrap_waits\": %u,\n", r.stats.ring_wrap_waits);
	std::printf("  \"validation_messages\": %u,\n", r.validation_messages);
	std::printf("  \"validation_active\": %s,\n", r.validation_active ? "true" : "false");
	std::printf("  \"pixels_ok\": %s\n", r.pixels_ok ? "true" : "false");
	std::printf("}\n");
}

} // namespace

int main(int argc, char** argv) {
	bool validation = true;
	uint32_t frames = 12;
	for (int i = 1; i < argc; ++i) {
		if (std::strcmp(argv[i], "--no-validation") == 0) {
			validation = false;
		} else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
			frames = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
		}
	}
	if (frames == 0) frames = 1;

	RenderBackend* backend = Create_Vulkan_Backend(validation, true);
	if (!backend->Init(nullptr, kTargetWidth, kTargetHeight)) {
		std::fprintf(stderr, "backend Init failed\n");
		return 1;
	}
	std::fprintf(stderr, "device: %s\n", backend->Device_Description());

	Workload workload(backend);
	if (!workload.Setup()) {
		std::fprintf(stderr, "workload setup failed\n");
		return 1;
	}

	Report report;
	report.frames = frames;
	report.retain_mode = std::getenv("ZH_SPIKE_STAGING_RETAIN") != nullptr;
	report.view_swizzle = std::getenv("ZH_SPIKE_NO_VIEW_SWIZZLE") == nullptr;
	for (uint32_t frame = 0; frame < frames; ++frame) {
		if (!workload.Run_Frame(frame)) {
			std::fprintf(stderr, "frame %u failed\n", frame);
			return 1;
		}
		// Steady state is what is still checked out at the frame boundary: the held
		// C7/C8 surfaces, and nothing else if the pool is doing its job.
		report.steady_state_bytes = backend->Get_Resource_Stats().staging_live_bytes;
	}

	report.pixels_ok = workload.Check_Pixels();
	report.stats = backend->Get_Resource_Stats();
	report.texture_locks = workload.texture_locks;
	report.buffer_locks = workload.buffer_locks;
	report.validation_messages = backend->Validation_Message_Count();
	report.validation_active = backend->Validation_Active();
	Print_Json(report);

	backend->Shutdown();
	delete backend;
	if (!report.pixels_ok) return 1;
	if (validation && report.validation_messages != 0) return 1;
	if (validation && !report.validation_active) {
		std::fprintf(stderr, "FAIL: validation was requested but no layer was loaded\n");
		return 1;
	}
	return 0;
}
