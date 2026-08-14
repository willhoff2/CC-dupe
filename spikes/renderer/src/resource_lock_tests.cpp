// Renderer spike: the D3D8 Lock/Unlock contract, end to end over Vulkan.
//
// docs/porting/renderer-resource-seam.md groups the engine's 95 Lock/Unlock call
// sites into 8 usage classes. This file implements the ones that can be driven
// headlessly and asserts on real pixels and real read-back bytes, so the design in
// that document is backed by something that runs:
//
//   C1  whole-surface write      lock level 0, fill it through the pitch, unlock,
//                                sample it, compare every texel against the
//                                pattern the CPU wrote
//   C2  partial-rect write       lock a sub-rect, write only inside it, and check
//                                the texels outside it survived
//   C3  read-back                Lock(READONLY) after a GPU-side write and compare
//                                the bytes with what was uploaded
//   C4  mip-chain load           lock every level at once, keep all the pointers,
//                                fill them later, unlock them all, then sample a
//                                specific level
//   C5  dynamic ring stream      DISCARD then NOOVERWRITE within one frame, two
//                                draws from the two sub-ranges, both visible
//
// Since the staging pool landed there are three more, all about the pool rather than
// about a new class: C4 filled from a second thread, two threads locking different
// resources at the same time, and a run of transient locks whose host-visible cost
// must not grow with the number of locks.
//
// Not covered here, and stated as such in the document: C6 (static buffer fill,
// which is C1 without the image), C7 (a pointer used after Unlock, which needs the
// shroud's whole render path) and C8 (SurfaceClass handing the pointer to arbitrary
// engine code).
//
// The MoltenVK constraint the spike already works around for creation-time uploads
// applies to locks too: with no image-view swizzle, L8/A8/A8L8/X8R8G8B8 have to be
// expanded on the CPU at *unlock*, from a second staging buffer. That path is
// exercised by re-running the L8 case with ZH_SPIKE_NO_VIEW_SWIZZLE=1, which is how
// CI covers it on Linux. It has not been run on MoltenVK by the author.

#include "render_backend.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace spike;

namespace {

constexpr uint32_t kWidth = 64;
constexpr uint32_t kHeight = 64;
constexpr uint32_t kTexWidth = 16;
constexpr uint32_t kTexHeight = 16;

struct Rgba {
	int r = 0, g = 0, b = 0, a = 0;
};

std::string To_String(const Rgba& c) {
	char buffer[64];
	std::snprintf(buffer, sizeof(buffer), "(%d,%d,%d,%d)", c.r, c.g, c.b, c.a);
	return buffer;
}

bool Near(const Rgba& a, const Rgba& b, int tolerance = 1) {
	return std::abs(a.r - b.r) <= tolerance && std::abs(a.g - b.g) <= tolerance &&
	       std::abs(a.b - b.b) <= tolerance && std::abs(a.a - b.a) <= tolerance;
}

// D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1, the pretransformed quad.
struct ScreenVertex {
	float x, y, z, rhw;
	uint32_t diffuse;
	float u, v;
};

constexpr uint32_t kQuadFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

// The pattern the lock cases write. Deliberately per-texel distinct so a wrong
// pitch, a transposed row or an off-by-one rectangle cannot pass.
uint32_t Pattern_Argb(uint32_t x, uint32_t y) {
	const uint32_t r = 8 + x * 13;
	const uint32_t g = 8 + y * 13;
	const uint32_t b = 40 + ((x * 7 + y * 3) & 0x7f);
	return 0xff000000u | ((r & 0xff) << 16) | ((g & 0xff) << 8) | (b & 0xff);
}

Rgba From_Argb(uint32_t argb) {
	return Rgba{static_cast<int>((argb >> 16) & 0xff), static_cast<int>((argb >> 8) & 0xff),
	            static_cast<int>(argb & 0xff), static_cast<int>((argb >> 24) & 0xff)};
}

struct Outcome {
	enum Status { kPass, kFail, kSkip } status = kPass;
	std::string detail;
};

Outcome Pass(const std::string& detail) { return Outcome{Outcome::kPass, detail}; }
Outcome Fail(const std::string& detail) { return Outcome{Outcome::kFail, detail}; }
Outcome Skip(const std::string& detail) { return Outcome{Outcome::kSkip, detail}; }

// ---------------------------------------------------------------------------
// harness
// ---------------------------------------------------------------------------

class Harness {
public:
	explicit Harness(RenderBackend* backend) : gfx_(backend) {}

	bool Init();
	void Shutdown() { gfx_->Shutdown(); }
	RenderBackend& Gfx() { return *gfx_; }

	// D3D8 device state as the cases need it: one texture stage selecting the
	// texture, no lighting, no depth, point sampling so a sampled texel is exactly
	// the texel that was written.
	void Reset_State();

	// Draws a quad covering [x0,x1) x [y0,y1) of the target, sampling the bound
	// texture over the whole [0,1] range of the given texture rectangle.
	void Draw_Textured_Quad(float x0, float y0, float x1, float y1, float u0 = 0.0f,
	                        float v0 = 0.0f, float u1 = 1.0f, float v1 = 1.0f);
	// Same, but the vertices come from a caller-supplied dynamic buffer range.
	void Draw_From(VertexBufferHandle* vb, uint32_t start_index, uint32_t base_vertex,
	               uint32_t polygon_count);

	void Begin();
	void End();
	bool Read_Back();
	Rgba Pixel(uint32_t x, uint32_t y) const;

	IndexBufferHandle* Quad_Ib() const { return quad_ib_; }
	// Six indices, so two quads can be drawn out of one buffer without the index
	// buffer being the thing under test.
	IndexBufferHandle* Two_Quad_Ib() const { return two_quad_ib_; }

private:
	RenderBackend* gfx_ = nullptr;
	std::string pixels_;
	SurfaceFormat format_{};
	IndexBufferHandle* quad_ib_ = nullptr;
	IndexBufferHandle* two_quad_ib_ = nullptr;
};

bool Harness::Init() {
	if (!gfx_->Init(nullptr, kWidth, kHeight)) return false;
	const uint16_t indices[6] = {0, 1, 2, 0, 2, 3};
	quad_ib_ = gfx_->Create_Index_Buffer(indices, 6);
	const uint16_t two[12] = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};
	two_quad_ib_ = gfx_->Create_Index_Buffer(two, 12);
	return quad_ib_ != nullptr && two_quad_ib_ != nullptr;
}

void Harness::Reset_State() {
	RenderBackend& g = *gfx_;
	g.Set_DX8_Render_State(D3DRS_ZENABLE, 0);
	g.Set_DX8_Render_State(D3DRS_ZWRITEENABLE, 0);
	g.Set_DX8_Render_State(D3DRS_CULLMODE, D3DCULL_NONE);
	g.Set_DX8_Render_State(D3DRS_ALPHABLENDENABLE, 0);
	g.Set_DX8_Render_State(D3DRS_ALPHATESTENABLE, 0);
	g.Set_DX8_Render_State(D3DRS_LIGHTING, 0);
	g.Set_DX8_Render_State(D3DRS_COLORWRITEENABLE, 0xf);
	g.Set_DX8_Render_State(D3DRS_STENCILENABLE, 0);
	g.Set_Transform(D3DTS_WORLD, Matrix4x4::Identity());
	g.Set_Transform(D3DTS_VIEW, Matrix4x4::Identity());
	g.Set_Transform(D3DTS_PROJECTION, Matrix4x4::Identity());
	for (uint32_t stage = 0; stage < 8; ++stage) {
		g.Set_DX8_Texture_Stage_State(stage, D3DTSS_COLOROP, D3DTOP_DISABLE);
		g.Set_DX8_Texture_Stage_State(stage, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
		g.Set_DX8_Texture_Stage_State(stage, D3DTSS_TEXCOORDINDEX, stage);
		g.Set_DX8_Texture_Stage_State(stage, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
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
	g.Set_Material(MaterialState{});
	g.Set_Scissor(false, 0, 0, 0, 0);
	for (uint32_t i = 0; i < 4; ++i) g.Set_Light(i, nullptr);
}

void Harness::Begin() {
	gfx_->Begin_Scene();
	gfx_->Clear(true, true, 0.0f, 0.0f, 0.0f, 1.0f);
}

void Harness::End() { gfx_->End_Scene(false); }

bool Harness::Read_Back() { return gfx_->Read_Back_Color_Target(pixels_, format_); }

Rgba Harness::Pixel(uint32_t x, uint32_t y) const {
	const size_t offset = (static_cast<size_t>(y) * format_.width + x) * 4;
	if (offset + 3 >= pixels_.size()) return Rgba{};
	const auto* p = reinterpret_cast<const unsigned char*>(pixels_.data()) + offset;
	return Rgba{p[0], p[1], p[2], p[3]};
}

void Harness::Draw_Textured_Quad(float x0, float y0, float x1, float y1, float u0,
                                 float v0, float u1, float v1) {
	const ScreenVertex vertices[4] = {
	    {x0, y0, 0.5f, 1.0f, 0xffffffffu, u0, v0},
	    {x1, y0, 0.5f, 1.0f, 0xffffffffu, u1, v0},
	    {x1, y1, 0.5f, 1.0f, 0xffffffffu, u1, v1},
	    {x0, y1, 0.5f, 1.0f, 0xffffffffu, u0, v1},
	};
	VertexBufferHandle* vb =
	    gfx_->Create_Vertex_Buffer(vertices, sizeof(vertices), kQuadFvf);
	if (vb == nullptr) return;
	gfx_->Set_Vertex_Buffer(vb, 0);
	gfx_->Set_Index_Buffer(quad_ib_, 0);
	gfx_->Draw_Triangles(0, 2, 0, 4);
}

void Harness::Draw_From(VertexBufferHandle* vb, uint32_t start_index,
                        uint32_t base_vertex, uint32_t polygon_count) {
	gfx_->Set_Vertex_Buffer(vb, 0);
	gfx_->Set_Index_Buffer(two_quad_ib_, base_vertex);
	gfx_->Draw_Triangles(start_index, polygon_count, 0, polygon_count * 3);
}

// ---------------------------------------------------------------------------
// C1: whole-surface write. TerrainTextureClass::update, SurfaceClass::Clear,
//     DDSFileClass::Copy_Level_To_Surface, MissingTexture::_Init.
// ---------------------------------------------------------------------------

// Fills the locked rectangle with Pattern_Argb, writing through the pitch the lock
// returned rather than assuming rows are width*4 apart.
void Fill_Argb(const LockedRect& locked, uint32_t left, uint32_t top, uint32_t width,
               uint32_t height) {
	auto* bytes = static_cast<uint8_t*>(locked.bits);
	for (uint32_t y = 0; y < height; ++y) {
		auto* row = reinterpret_cast<uint32_t*>(bytes + static_cast<size_t>(y) * locked.pitch);
		for (uint32_t x = 0; x < width; ++x) row[x] = Pattern_Argb(left + x, top + y);
	}
}

Outcome Case_Whole_Surface_Write(Harness& h) {
	RenderBackend& g = h.Gfx();
	TextureHandle* tex =
	    g.Create_Lockable_Texture(kTexWidth, kTexHeight, TextureFormat::A8R8G8B8, 1);
	if (tex == nullptr) return Fail("Create_Lockable_Texture failed");

	LockedRect locked;
	if (!g.Lock_Texture(tex, 0, nullptr, LOCK_NONE, locked)) return Fail("Lock_Texture failed");
	if (locked.pitch < kTexWidth * 4) return Fail("pitch smaller than a row");
	Fill_Argb(locked, 0, 0, kTexWidth, kTexHeight);
	if (!g.Unlock_Texture(tex, 0)) return Fail("Unlock_Texture failed");

	// One quad per texel, so every texel is compared, not just a sample of them.
	h.Reset_State();
	g.Set_Texture(0, tex);
	h.Begin();
	const float sx = static_cast<float>(kWidth) / kTexWidth;
	const float sy = static_cast<float>(kHeight) / kTexHeight;
	h.Draw_Textured_Quad(0.0f, 0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight));
	h.End();
	if (!h.Read_Back()) return Fail("read back failed");

	for (uint32_t ty = 0; ty < kTexHeight; ++ty) {
		for (uint32_t tx = 0; tx < kTexWidth; ++tx) {
			// Centre of the screen area this texel covers.
			const uint32_t px = static_cast<uint32_t>((tx + 0.5f) * sx);
			const uint32_t py = static_cast<uint32_t>((ty + 0.5f) * sy);
			const Rgba expected = From_Argb(Pattern_Argb(tx, ty));
			const Rgba actual = h.Pixel(px, py);
			if (!Near(actual, expected)) {
				char detail[160];
				std::snprintf(detail, sizeof(detail),
				              "texel (%u,%u) got=%s expected=%s", tx, ty,
				              To_String(actual).c_str(), To_String(expected).c_str());
				return Fail(detail);
			}
		}
	}
	return Pass("all 256 locked texels sampled back exactly");
}

// ---------------------------------------------------------------------------
// C2: partial-rect write. SurfaceClass::Copy, W3DShroud's per-frame region.
// ---------------------------------------------------------------------------

Outcome Case_Partial_Rect_Write(Harness& h) {
	RenderBackend& g = h.Gfx();
	TextureHandle* tex =
	    g.Create_Lockable_Texture(kTexWidth, kTexHeight, TextureFormat::A8R8G8B8, 1);
	if (tex == nullptr) return Fail("Create_Lockable_Texture failed");

	// Whole level first, so there is something the partial lock must not disturb.
	LockedRect locked;
	if (!g.Lock_Texture(tex, 0, nullptr, LOCK_NONE, locked)) return Fail("whole lock failed");
	Fill_Argb(locked, 0, 0, kTexWidth, kTexHeight);
	if (!g.Unlock_Texture(tex, 0)) return Fail("whole unlock failed");

	// Then a sub-rect, filled with a constant so the boundary is unambiguous.
	const LockRect rect{4, 2, 12, 10};
	if (!g.Lock_Texture(tex, 0, &rect, LOCK_NONE, locked)) return Fail("rect lock failed");
	const uint32_t fill = 0xff20c040u;
	auto* bytes = static_cast<uint8_t*>(locked.bits);
	for (uint32_t y = 0; y < rect.bottom - rect.top; ++y) {
		auto* row = reinterpret_cast<uint32_t*>(bytes + static_cast<size_t>(y) * locked.pitch);
		for (uint32_t x = 0; x < rect.right - rect.left; ++x) row[x] = fill;
	}
	if (!g.Unlock_Texture(tex, 0)) return Fail("rect unlock failed");

	h.Reset_State();
	g.Set_Texture(0, tex);
	h.Begin();
	h.Draw_Textured_Quad(0.0f, 0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight));
	h.End();
	if (!h.Read_Back()) return Fail("read back failed");

	const float sx = static_cast<float>(kWidth) / kTexWidth;
	const float sy = static_cast<float>(kHeight) / kTexHeight;
	for (uint32_t ty = 0; ty < kTexHeight; ++ty) {
		for (uint32_t tx = 0; tx < kTexWidth; ++tx) {
			const bool inside = tx >= rect.left && tx < rect.right && ty >= rect.top &&
			                    ty < rect.bottom;
			const Rgba expected = From_Argb(inside ? fill : Pattern_Argb(tx, ty));
			const uint32_t px = static_cast<uint32_t>((tx + 0.5f) * sx);
			const uint32_t py = static_cast<uint32_t>((ty + 0.5f) * sy);
			const Rgba actual = h.Pixel(px, py);
			if (!Near(actual, expected)) {
				char detail[192];
				std::snprintf(detail, sizeof(detail), "texel (%u,%u) %s got=%s expected=%s",
				              tx, ty, inside ? "inside" : "outside",
				              To_String(actual).c_str(), To_String(expected).c_str());
				return Fail(detail);
			}
		}
	}
	return Pass("rect (4,2)-(12,10) replaced, the other 192 texels untouched");
}

// ---------------------------------------------------------------------------
// C3: read-back. W3DSmudge::copyRect, SurfaceClass::CreateCopy/FindBB/
//     Is_Transparent_Column.
// ---------------------------------------------------------------------------

Outcome Case_Read_Back_Lock(Harness& h) {
	RenderBackend& g = h.Gfx();
	TextureHandle* tex =
	    g.Create_Lockable_Texture(kTexWidth, kTexHeight, TextureFormat::A8R8G8B8, 1);
	if (tex == nullptr) return Fail("Create_Lockable_Texture failed");

	LockedRect locked;
	if (!g.Lock_Texture(tex, 0, nullptr, LOCK_NONE, locked)) return Fail("write lock failed");
	Fill_Argb(locked, 0, 0, kTexWidth, kTexHeight);
	if (!g.Unlock_Texture(tex, 0)) return Fail("write unlock failed");

	// Scribble over the staging memory, so a read-only lock that forgot to copy the
	// image back cannot pass by finding what the write lock left behind.
	if (!g.Lock_Texture(tex, 0, nullptr, LOCK_NONE, locked)) return Fail("scribble lock failed");
	std::memset(locked.bits, 0x5a, static_cast<size_t>(locked.pitch) * kTexHeight);
	// Deliberately *not* unlocked-with-upload: unlock the scribble too, so the image
	// holds the scribble, then write the pattern again and check the read-back sees
	// the pattern rather than the scribble.
	if (!g.Unlock_Texture(tex, 0)) return Fail("scribble unlock failed");
	if (!g.Lock_Texture(tex, 0, nullptr, LOCK_NONE, locked)) return Fail("rewrite lock failed");
	Fill_Argb(locked, 0, 0, kTexWidth, kTexHeight);
	if (!g.Unlock_Texture(tex, 0)) return Fail("rewrite unlock failed");
	std::memset(locked.bits, 0xa5, static_cast<size_t>(locked.pitch) * kTexHeight);

	if (!g.Lock_Texture(tex, 0, nullptr, LOCK_READONLY, locked)) {
		return Fail("Lock_Texture(READONLY) failed");
	}
	for (uint32_t y = 0; y < kTexHeight; ++y) {
		const auto* row = reinterpret_cast<const uint32_t*>(
		    static_cast<const uint8_t*>(locked.bits) + static_cast<size_t>(y) * locked.pitch);
		for (uint32_t x = 0; x < kTexWidth; ++x) {
			if (row[x] != Pattern_Argb(x, y)) {
				char detail[160];
				std::snprintf(detail, sizeof(detail),
				              "texel (%u,%u) read back 0x%08x, wrote 0x%08x", x, y, row[x],
				              Pattern_Argb(x, y));
				g.Unlock_Texture(tex, 0);
				return Fail(detail);
			}
		}
	}
	if (!g.Unlock_Texture(tex, 0)) return Fail("read-only unlock failed");
	return Pass("READONLY lock returned the image contents, not the stale staging copy");
}

// ---------------------------------------------------------------------------
// C4: the whole mip chain locked at once, filled later, unlocked later.
//     TextureLoadTaskClass::Lock_Surfaces / Unlock_Surfaces.
// ---------------------------------------------------------------------------

Outcome Case_Mip_Chain_Lock(Harness& h) {
	RenderBackend& g = h.Gfx();
	constexpr uint32_t kLevels = 5; // 16, 8, 4, 2, 1
	TextureHandle* tex =
	    g.Create_Lockable_Texture(kTexWidth, kTexHeight, TextureFormat::A8R8G8B8, kLevels);
	if (tex == nullptr) return Fail("Create_Lockable_Texture failed");

	// Exactly what the loader does: lock every level up front and keep the pointers
	// and pitches in an array. In the engine the filling happens on another thread,
	// some frames later; the ordering here is the same, the thread is not.
	LockedRect locked[kLevels];
	for (uint32_t level = 0; level < kLevels; ++level) {
		if (!g.Lock_Texture(tex, level, nullptr, LOCK_NONE, locked[level])) {
			return Fail("Lock_Texture failed for a mip level");
		}
	}
	// One flat colour per level, so sampling a level identifies which level it is.
	const uint32_t level_color[kLevels] = {0xffff0000u, 0xff00ff00u, 0xff0000ffu,
	                                       0xffffff00u, 0xff00ffffu};
	for (uint32_t level = 0; level < kLevels; ++level) {
		const uint32_t size = kTexWidth >> level;
		for (uint32_t y = 0; y < size; ++y) {
			auto* row = reinterpret_cast<uint32_t*>(
			    static_cast<uint8_t*>(locked[level].bits) +
			    static_cast<size_t>(y) * locked[level].pitch);
			for (uint32_t x = 0; x < size; ++x) row[x] = level_color[level];
		}
	}
	for (uint32_t level = 0; level < kLevels; ++level) {
		if (!g.Unlock_Texture(tex, level)) return Fail("Unlock_Texture failed for a level");
	}

	// Sampling a chosen level: D3DTSS_MAXMIPLEVEL is not in the backend's state set,
	// so the level is selected the way the engine's terrain does it, by drawing at a
	// size that makes the sampler pick that level, with MIPFILTER_POINT.
	h.Reset_State();
	g.Set_Texture(0, tex);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_MIPFILTER, D3DTEXF_POINT);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_MINFILTER, D3DTEXF_POINT);
	h.Begin();
	// A 16x16 texture drawn into 4x4 pixels: one screen pixel per 4 texels, which is
	// mip level 2.
	h.Draw_Textured_Quad(0.0f, 0.0f, 4.0f, 4.0f);
	h.End();
	if (!h.Read_Back()) return Fail("read back failed");

	const Rgba actual = h.Pixel(2, 2);
	const Rgba expected = From_Argb(level_color[2]);
	if (!Near(actual, expected)) {
		// A device is free to pick a neighbouring level, so report which level the
		// pixel came from instead of only failing.
		for (uint32_t level = 0; level < kLevels; ++level) {
			if (Near(actual, From_Argb(level_color[level]))) {
				char detail[160];
				std::snprintf(detail, sizeof(detail),
				              "sampled level %u, expected level 2 (both were filled "
				              "through one multi-level lock)",
				              level);
				return Fail(detail);
			}
		}
		return Fail("sampled colour matches no level: got=" + To_String(actual));
	}
	return Pass("5 levels locked at once, filled after the fact, level 2 sampled back");
}

// ---------------------------------------------------------------------------
// C4 across threads: the part §7.6 said was asserted by nothing. The engine locks
// every level from Begin_Load on the DX8 thread, fills them from Load() on the
// loader thread, and unlocks from End_Load back on the DX8 thread.
// ---------------------------------------------------------------------------

const uint32_t kLevelColor[5] = {0xffff0000u, 0xff00ff00u, 0xff0000ffu, 0xffffff00u,
                                 0xff00ffffu};

// Fills every level of a locked chain with its flat colour, through the pitches the
// lock returned. Runs on whichever thread calls it.
void Fill_Chain(const LockedRect* locked, uint32_t levels, uint32_t base_size) {
	for (uint32_t level = 0; level < levels; ++level) {
		const uint32_t size = base_size >> level ? base_size >> level : 1u;
		for (uint32_t y = 0; y < size; ++y) {
			auto* row = reinterpret_cast<uint32_t*>(
			    static_cast<uint8_t*>(locked[level].bits) +
			    static_cast<size_t>(y) * locked[level].pitch);
			for (uint32_t x = 0; x < size; ++x) row[x] = kLevelColor[level];
		}
	}
}

Outcome Case_Mip_Chain_Cross_Thread(Harness& h) {
	RenderBackend& g = h.Gfx();
	constexpr uint32_t kLevels = 5;
	TextureHandle* tex =
	    g.Create_Lockable_Texture(kTexWidth, kTexHeight, TextureFormat::A8R8G8B8, kLevels);
	if (tex == nullptr) return Fail("Create_Lockable_Texture failed");

	LockedRect locked[kLevels];
	for (uint32_t level = 0; level < kLevels; ++level) {
		if (!g.Lock_Texture(tex, level, nullptr, LOCK_NONE, locked[level])) {
			return Fail("Lock_Texture failed for a mip level");
		}
	}
	// The loader thread, which never calls the backend at all: it only writes through
	// pointers another thread locked. Host-coherent memory plus the join is the whole
	// synchronisation, which is what the engine's task hand-off already provides.
	std::thread loader([&]() { Fill_Chain(locked, kLevels, kTexWidth); });
	loader.join();
	for (uint32_t level = 0; level < kLevels; ++level) {
		if (!g.Unlock_Texture(tex, level)) return Fail("Unlock_Texture failed for a level");
	}

	h.Reset_State();
	g.Set_Texture(0, tex);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_MIPFILTER, D3DTEXF_POINT);
	h.Begin();
	h.Draw_Textured_Quad(0.0f, 0.0f, 4.0f, 4.0f); // 16x16 into 4x4 px: level 2
	h.End();
	if (!h.Read_Back()) return Fail("read back failed");
	const Rgba actual = h.Pixel(2, 2);
	if (!Near(actual, From_Argb(kLevelColor[2]))) {
		return Fail("level 2 filled on the loader thread sampled back as " + To_String(actual));
	}
	return Pass("5 levels locked on this thread, filled on another, unlocked here, "
	            "level 2 correct");
}

// Two threads locking, filling and unlocking *different* resources at the same time.
// D3D8's runtime serialised this internally; the backend does the same with one
// mutex over the lock path, and this case is what proves the pool cannot hand the
// same block to both threads.
Outcome Case_Concurrent_Locks(Harness& h) {
	RenderBackend& g = h.Gfx();
	constexpr uint32_t kThreads = 4;
	constexpr uint32_t kRounds = 8;
	TextureHandle* tex[kThreads] = {};
	for (uint32_t t = 0; t < kThreads; ++t) {
		tex[t] = g.Create_Lockable_Texture(kTexWidth, kTexHeight, TextureFormat::A8R8G8B8, 1);
		if (tex[t] == nullptr) return Fail("Create_Lockable_Texture failed");
	}

	std::atomic<uint32_t> failures{0};
	std::vector<std::thread> threads;
	for (uint32_t t = 0; t < kThreads; ++t) {
		threads.emplace_back([&, t]() {
			for (uint32_t round = 0; round < kRounds; ++round) {
				LockedRect locked;
				if (!g.Lock_Texture(tex[t], 0, nullptr, LOCK_NONE, locked)) {
					++failures;
					return;
				}
				// The last round writes the colour the pixel check looks for; the
				// earlier ones exist to churn the pool.
				const uint32_t colour =
				    round + 1 == kRounds ? kLevelColor[t] : 0xff101010u + round;
				for (uint32_t y = 0; y < kTexHeight; ++y) {
					auto* row = reinterpret_cast<uint32_t*>(
					    static_cast<uint8_t*>(locked.bits) +
					    static_cast<size_t>(y) * locked.pitch);
					for (uint32_t x = 0; x < kTexWidth; ++x) row[x] = colour;
				}
				if (!g.Unlock_Texture(tex[t], 0)) {
					++failures;
					return;
				}
			}
		});
	}
	for (std::thread& thread : threads) thread.join();
	if (failures.load() != 0) return Fail("a threaded lock/unlock returned failure");

	// Each texture must hold its own thread's last colour: a block handed to two
	// threads at once, or stats racing, shows up as the wrong colour here.
	for (uint32_t t = 0; t < kThreads; ++t) {
		h.Reset_State();
		g.Set_Texture(0, tex[t]);
		h.Begin();
		h.Draw_Textured_Quad(0.0f, 0.0f, static_cast<float>(kWidth),
		                     static_cast<float>(kHeight));
		h.End();
		if (!h.Read_Back()) return Fail("read back failed");
		const Rgba actual = h.Pixel(kWidth / 2, kHeight / 2);
		if (!Near(actual, From_Argb(kLevelColor[t]))) {
			char detail[176];
			std::snprintf(detail, sizeof(detail),
			              "texture %u written by thread %u sampled back as %s", t, t,
			              To_String(actual).c_str());
			return Fail(detail);
		}
	}
	return Pass("4 threads x 8 lock/fill/unlock rounds on 4 textures, every texture "
	            "holds its own thread's pixels");
}

// ---------------------------------------------------------------------------
// The pool itself: a run of transient locks must not allocate per lock.
// ---------------------------------------------------------------------------

Outcome Case_Pool_Recycles_Staging(Harness& h) {
	RenderBackend& g = h.Gfx();
	constexpr uint32_t kTextures = 6;
	constexpr uint32_t kRounds = 6;
	TextureHandle* tex[kTextures] = {};
	for (uint32_t i = 0; i < kTextures; ++i) {
		tex[i] = g.Create_Lockable_Texture(32, 32, TextureFormat::A8R8G8B8, 1);
		if (tex[i] == nullptr) return Fail("Create_Lockable_Texture failed");
	}
	const ResourceStats before = g.Get_Resource_Stats();

	for (uint32_t round = 0; round < kRounds; ++round) {
		for (uint32_t i = 0; i < kTextures; ++i) {
			LockedRect locked;
			if (!g.Lock_Texture(tex[i], 0, nullptr, LOCK_NONE, locked)) {
				return Fail("Lock_Texture failed");
			}
			for (uint32_t y = 0; y < 32; ++y) {
				auto* row = reinterpret_cast<uint32_t*>(
				    static_cast<uint8_t*>(locked.bits) +
				    static_cast<size_t>(y) * locked.pitch);
				for (uint32_t x = 0; x < 32; ++x) row[x] = kLevelColor[i % 5];
			}
			if (!g.Unlock_Texture(tex[i], 0)) return Fail("Unlock_Texture failed");
		}
	}
	const ResourceStats after = g.Get_Resource_Stats();
	const uint32_t locks = kTextures * kRounds;
	const uint32_t new_allocations = after.staging_allocations - before.staging_allocations;
	// Locks never overlap here, so one block serves all of them. Anything above 1 is
	// the pool failing to recycle; the check is <= 1 rather than == 0 because the
	// first lock may legitimately need a block the pool does not have yet.
	if (new_allocations > 1) {
		char detail[176];
		std::snprintf(detail, sizeof(detail),
		              "%u sequential locks caused %u host-visible allocations", locks,
		              new_allocations);
		return Fail(detail);
	}
	if (after.staging_live_bytes != 0) {
		return Fail("staging bytes are still checked out after every unlock");
	}

	// ...and the pixels still have to be right, because a pool that recycles too
	// eagerly shows up as the wrong texture's texels.
	h.Reset_State();
	g.Set_Texture(0, tex[2]);
	h.Begin();
	h.Draw_Textured_Quad(0.0f, 0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight));
	h.End();
	if (!h.Read_Back()) return Fail("read back failed");
	if (!Near(h.Pixel(kWidth / 2, kHeight / 2), From_Argb(kLevelColor[2]))) {
		return Fail("a recycled block published the wrong texels");
	}

	char detail[192];
	std::snprintf(detail, sizeof(detail),
	              "%u locks over %u textures cost %u new allocation(s); %u of %u "
	              "acquires reused a pooled block",
	              locks, kTextures, new_allocations,
	              after.staging_reuses - before.staging_reuses,
	              after.staging_acquires - before.staging_acquires);
	return Pass(detail);
}

// ---------------------------------------------------------------------------
// C5: dynamic ring stream. DynamicVBAccessClass::WriteLockClass and the six
//     call sites that stream through it, plus W3DSnowManager::renderSubBox.
// ---------------------------------------------------------------------------

Outcome Case_Dynamic_Ring_Stream(Harness& h) {
	RenderBackend& g = h.Gfx();
	// 8 vertices: the engine's dynamic buffer is much larger and is sub-allocated
	// the same way, an offset at a time.
	const size_t capacity = 8 * sizeof(ScreenVertex);
	VertexBufferHandle* vb = g.Create_Dynamic_Vertex_Buffer(capacity, kQuadFvf);
	if (vb == nullptr) return Fail("Create_Dynamic_Vertex_Buffer failed");

	TextureHandle* tex =
	    g.Create_Lockable_Texture(2, 2, TextureFormat::A8R8G8B8, 1);
	if (tex == nullptr) return Fail("Create_Lockable_Texture failed");
	LockedRect locked;
	if (!g.Lock_Texture(tex, 0, nullptr, LOCK_NONE, locked)) return Fail("texture lock failed");
	auto* texels = static_cast<uint8_t*>(locked.bits);
	for (uint32_t y = 0; y < 2; ++y) {
		auto* row = reinterpret_cast<uint32_t*>(texels + static_cast<size_t>(y) * locked.pitch);
		row[0] = row[1] = 0xffffffffu;
	}
	if (!g.Unlock_Texture(tex, 0)) return Fail("texture unlock failed");

	h.Reset_State();
	g.Set_Texture(0, tex);
	// Modulate so the per-vertex colour is what identifies which sub-range a pixel
	// came from.
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);

	const uint32_t left_color = 0xffff4000u;
	const uint32_t right_color = 0xff0040ffu;
	const float half = static_cast<float>(kWidth) / 2.0f;
	const float bottom = static_cast<float>(kHeight);

	h.Begin();

	// First lock of the frame: offset 0, so DISCARD -- the engine's exact condition
	// (`!DynamicVBAccess->VertexBufferOffset ? D3DLOCK_DISCARD : D3DLOCK_NOOVERWRITE`).
	void* bits = nullptr;
	if (!g.Lock_Vertex_Buffer(vb, 0, 4 * sizeof(ScreenVertex), LOCK_DISCARD, &bits)) {
		return Fail("Lock_Vertex_Buffer(DISCARD) failed");
	}
	{
		auto* v = static_cast<ScreenVertex*>(bits);
		v[0] = {0.0f, 0.0f, 0.5f, 1.0f, left_color, 0.5f, 0.5f};
		v[1] = {half, 0.0f, 0.5f, 1.0f, left_color, 0.5f, 0.5f};
		v[2] = {half, bottom, 0.5f, 1.0f, left_color, 0.5f, 0.5f};
		v[3] = {0.0f, bottom, 0.5f, 1.0f, left_color, 0.5f, 0.5f};
	}
	if (!g.Unlock_Vertex_Buffer(vb)) return Fail("Unlock_Vertex_Buffer failed");
	h.Draw_From(vb, 0, 0, 2);

	// Second lock of the same frame at a non-zero offset: NOOVERWRITE. The first
	// four vertices must survive, because the draw that reads them has been recorded
	// but not submitted.
	if (!g.Lock_Vertex_Buffer(vb, 4 * sizeof(ScreenVertex), 4 * sizeof(ScreenVertex),
	                          LOCK_NOOVERWRITE, &bits)) {
		return Fail("Lock_Vertex_Buffer(NOOVERWRITE) failed");
	}
	{
		auto* v = static_cast<ScreenVertex*>(bits);
		v[0] = {half, 0.0f, 0.5f, 1.0f, right_color, 0.5f, 0.5f};
		v[1] = {static_cast<float>(kWidth), 0.0f, 0.5f, 1.0f, right_color, 0.5f, 0.5f};
		v[2] = {static_cast<float>(kWidth), bottom, 0.5f, 1.0f, right_color, 0.5f, 0.5f};
		v[3] = {half, bottom, 0.5f, 1.0f, right_color, 0.5f, 0.5f};
	}
	if (!g.Unlock_Vertex_Buffer(vb)) return Fail("Unlock_Vertex_Buffer failed");
	h.Draw_From(vb, 6, 0, 2);

	h.End();
	if (!h.Read_Back()) return Fail("read back failed");

	const Rgba left = h.Pixel(kWidth / 4, kHeight / 2);
	const Rgba right = h.Pixel(kWidth * 3 / 4, kHeight / 2);
	if (!Near(left, From_Argb(left_color))) {
		return Fail("the DISCARD sub-range's draw is wrong: got=" + To_String(left) +
		            " expected=" + To_String(From_Argb(left_color)));
	}
	if (!Near(right, From_Argb(right_color))) {
		return Fail("the NOOVERWRITE sub-range's draw is wrong: got=" + To_String(right) +
		            " expected=" + To_String(From_Argb(right_color)));
	}

	// A second frame, DISCARD again: the ring must hand out a different region while
	// the previous frame's region stays intact for as long as it is in flight.
	const ResourceStats before = g.Get_Resource_Stats();
	h.Begin();
	if (!g.Lock_Vertex_Buffer(vb, 0, 4 * sizeof(ScreenVertex), LOCK_DISCARD, &bits)) {
		return Fail("second-frame DISCARD failed");
	}
	{
		auto* v = static_cast<ScreenVertex*>(bits);
		v[0] = {0.0f, 0.0f, 0.5f, 1.0f, right_color, 0.5f, 0.5f};
		v[1] = {static_cast<float>(kWidth), 0.0f, 0.5f, 1.0f, right_color, 0.5f, 0.5f};
		v[2] = {static_cast<float>(kWidth), bottom, 0.5f, 1.0f, right_color, 0.5f, 0.5f};
		v[3] = {0.0f, bottom, 0.5f, 1.0f, right_color, 0.5f, 0.5f};
	}
	g.Unlock_Vertex_Buffer(vb);
	h.Draw_From(vb, 0, 0, 2);
	h.End();
	if (!h.Read_Back()) return Fail("second read back failed");
	if (!Near(h.Pixel(kWidth / 4, kHeight / 2), From_Argb(right_color))) {
		return Fail("the renamed region's draw is wrong");
	}
	const ResourceStats after = g.Get_Resource_Stats();
	if (after.ring_discards != before.ring_discards + 1) {
		return Fail("the second frame's DISCARD was not counted");
	}
	if (after.ring_wrap_waits != before.ring_wrap_waits) {
		return Fail("the ring wrapped when it had a free region");
	}
	return Pass("DISCARD then NOOVERWRITE in one frame, both draws correct, "
	            "renamed region correct next frame");
}

// ---------------------------------------------------------------------------
// The MoltenVK constraint: no image-view swizzle, so a locked L8 texture has to be
// expanded on the CPU at unlock. The environment variable makes the Linux device
// behave the same way, which is how this is covered without a Mac.
// ---------------------------------------------------------------------------

Outcome Case_L8_Lock(Harness& h) {
	RenderBackend& g = h.Gfx();
	if (!g.Supports_Texture_Format(TextureFormat::L8)) return Skip("device has no L8 path");
	TextureHandle* tex = g.Create_Lockable_Texture(kTexWidth, kTexHeight, TextureFormat::L8, 1);
	if (tex == nullptr) return Fail("Create_Lockable_Texture(L8) failed");

	LockedRect locked;
	if (!g.Lock_Texture(tex, 0, nullptr, LOCK_NONE, locked)) return Fail("lock failed");
	if (locked.pitch < kTexWidth) return Fail("pitch smaller than a row of L8");
	// A ramp, one byte per texel: the caller writes D3D8's L8 layout whether or not
	// the device can swizzle, which is the whole point.
	for (uint32_t y = 0; y < kTexHeight; ++y) {
		auto* row = static_cast<uint8_t*>(locked.bits) + static_cast<size_t>(y) * locked.pitch;
		for (uint32_t x = 0; x < kTexWidth; ++x) {
			row[x] = static_cast<uint8_t>((x * 16 + y) & 0xff);
		}
	}
	if (!g.Unlock_Texture(tex, 0)) return Fail("unlock failed");

	h.Reset_State();
	g.Set_Texture(0, tex);
	h.Begin();
	h.Draw_Textured_Quad(0.0f, 0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight));
	h.End();
	if (!h.Read_Back()) return Fail("read back failed");

	const float sx = static_cast<float>(kWidth) / kTexWidth;
	const float sy = static_cast<float>(kHeight) / kTexHeight;
	for (uint32_t ty = 0; ty < kTexHeight; ++ty) {
		for (uint32_t tx = 0; tx < kTexWidth; ++tx) {
			const int l = static_cast<int>((tx * 16 + ty) & 0xff);
			// D3DFMT_L8 samples as (L,L,L,1).
			const Rgba expected{l, l, l, 255};
			const Rgba actual = h.Pixel(static_cast<uint32_t>((tx + 0.5f) * sx),
			                            static_cast<uint32_t>((ty + 0.5f) * sy));
			if (!Near(actual, expected)) {
				char detail[176];
				std::snprintf(detail, sizeof(detail), "texel (%u,%u) got=%s expected=%s",
				              tx, ty, To_String(actual).c_str(),
				              To_String(expected).c_str());
				return Fail(detail);
			}
		}
	}
	const ResourceStats stats = g.Get_Resource_Stats();
	char detail[176];
	std::snprintf(detail, sizeof(detail),
	              "L8 ramp locked, written as L8 and sampled as (L,L,L,1); "
	              "cpu expansions so far: %u",
	              stats.cpu_expansions);
	return Pass(detail);
}

} // namespace

int main(int argc, char** argv) {
	bool validation = true;
	for (int i = 1; i < argc; ++i) {
		if (std::strcmp(argv[i], "--no-validation") == 0) validation = false;
	}

	RenderBackend* backend = Create_Vulkan_Backend(validation, true);
	Harness harness(backend);
	if (!harness.Init()) {
		std::fprintf(stderr, "backend Init failed\n");
		return 1;
	}
	std::printf("device: %s\n", backend->Device_Description());
	const char* no_swizzle = std::getenv("ZH_SPIKE_NO_VIEW_SWIZZLE");
	std::printf("view swizzle: %s\n\n",
	            no_swizzle != nullptr && no_swizzle[0] == '1' ? "forced off (MoltenVK's case)"
	                                                         : "as the device reports");

	int failed = 0;
	int skipped = 0;
	auto report = [&](const char* name, const Outcome& outcome) {
		const char* label = outcome.status == Outcome::kPass   ? "PASS"
		                    : outcome.status == Outcome::kSkip ? "SKIP"
		                                                      : "FAIL";
		std::printf("  %-4s %-28s %s\n", label, name, outcome.detail.c_str());
		if (outcome.status == Outcome::kFail) ++failed;
		if (outcome.status == Outcome::kSkip) ++skipped;
	};

	std::printf("== D3D8 lock/unlock usage classes ==\n");
	report("C1 whole-surface write", Case_Whole_Surface_Write(harness));
	report("C2 partial-rect write", Case_Partial_Rect_Write(harness));
	report("C3 read-back", Case_Read_Back_Lock(harness));
	report("C4 mip-chain lock", Case_Mip_Chain_Lock(harness));
	report("C4 cross-thread fill", Case_Mip_Chain_Cross_Thread(harness));
	report("C4 concurrent locks", Case_Concurrent_Locks(harness));
	report("C5 dynamic ring stream", Case_Dynamic_Ring_Stream(harness));
	report("L8 lock (no swizzle)", Case_L8_Lock(harness));
	report("staging pool recycling", Case_Pool_Recycles_Staging(harness));

	// The cost model in docs/porting/renderer-resource-seam.md, measured rather than
	// estimated, for exactly the work the cases above did.
	const ResourceStats stats = backend->Get_Resource_Stats();
	std::printf("\n== what the D3D8 lock contract cost ==\n");
	std::printf("  staging blocks the pool ever allocated:               %u (%llu bytes)\n",
	            stats.staging_allocations,
	            static_cast<unsigned long long>(stats.staging_bytes));
	std::printf("  dynamic vertex-buffer memory (not poolable):          %u (%llu bytes)\n",
	            stats.dynamic_buffer_allocations,
	            static_cast<unsigned long long>(stats.dynamic_buffer_bytes));
	std::printf("  staging peak checked out at once:                    %llu bytes in "
	            "%u block(s)\n",
	            static_cast<unsigned long long>(stats.staging_live_peak_bytes),
	            stats.staging_live_blocks_peak);
	std::printf("  staging still checked out now:                       %llu bytes\n",
	            static_cast<unsigned long long>(stats.staging_live_bytes));
	std::printf("  pool: %u free block(s), %llu bytes, %u/%u acquires reused, %u pinned\n",
	            stats.staging_pool_blocks,
	            static_cast<unsigned long long>(stats.staging_pool_bytes),
	            stats.staging_reuses, stats.staging_acquires,
	            stats.staging_retained_blocks);
	std::printf("  buffer-to-image copy regions issued from Unlock:     %u\n",
	            stats.texture_upload_regions);
	std::printf("  queue submits caused by locks:                       %u\n",
	            stats.upload_submits);
	std::printf("  read-back stalls (submit + fence wait inside Lock):  %u\n",
	            stats.readback_stalls);
	std::printf("  CPU channel expansions at unlock:                    %u\n",
	            stats.cpu_expansions);
	std::printf("  dynamic ring: %u DISCARD, %u NOOVERWRITE, %llu bytes, %u wrap stalls\n",
	            stats.ring_discards, stats.ring_appends,
	            static_cast<unsigned long long>(stats.ring_bytes), stats.ring_wrap_waits);

	const uint32_t validation_messages = backend->Validation_Message_Count();
	std::printf("\nvalidation messages: %u\n", validation_messages);
	std::printf("%d case(s) failed, %d skipped\n", failed, skipped);
	harness.Shutdown();
	delete backend;
	if (validation && validation_messages != 0) return 1;
	return failed == 0 ? 0 : 1;
}
