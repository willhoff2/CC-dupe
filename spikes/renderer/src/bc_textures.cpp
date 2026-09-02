// Renderer spike: block-compressed textures (D3DFMT_DXT1/3/5 -> BC1/BC2/BC3) created through
// the lockable path, filled through Lock/Unlock with BLOCK pitch semantics, drawn, read back.
//
// Zero Hour's texture loader hands every .dds mip level to the texture through
// IDirect3DTexture8::LockRect and a memcpy of the compressed level bytes, so the contract under
// test is D3D8's for a compressed surface: CreateTexture(D3DFMT_DXTn) yields a real compressed
// image; LockRect reports `Pitch` in bytes per row of 4x4 BLOCKS (not per row of texels); a
// sub-rect lock has to be block-aligned and is refused otherwise; a level narrower than a block
// is still one block wide. Until this path existed the backend returned null for every DXTn
// CreateTexture, D3DXCreateTexture substituted A8R8G8B8, and the engine software-decoded every
// texture in the game (docs/porting/block-compressed-textures.md).
//
// Per format, one 16x16 texture with a full mip chain (16, 8, 4, 2, 1). Every block of every
// level is hand-encoded to a solid colour that names the block and the level, so a pitch or
// offset fault lands the wrong colour in a readable place instead of merely validating.
// Cases, each a 64x64 quad drawn with point sampling and read back per block (16 blocks):
//   case 0/3/6  level 0 as written: every block carries its own colour (and alpha for BC2/BC3)
//   case 1/4/7  after a block-aligned sub-rect lock of the middle 2x2 blocks without DISCARD:
//               those 4 blocks carry their new colour, the other 12 are preserved -- which
//               needs the compressed level read back from the image, not zeroed
//   case 2/5/8  the 8x8 level 1, sampled by drawing the texture into 8x8 pixels (two texels
//               per pixel) with a point mip filter -- how resource_lock_tests.cpp selects a
//               level, since D3DTSS_MAXMIPLEVEL is not in the backend's state set: its 4
//               blocks carry level 1's colours, so a level offset computed in texels instead
//               of blocks shows here
// plus, per format and not drawn: the lock pitch of every level against the block pitch, a
// misaligned sub-rect lock that must be refused, and a read-only lock of the 4x4 level 2 whose
// bytes must be exactly the block written into it.
//
// Exit status is 0 only when every case's blocks are correct, every pitch is the block pitch,
// the refusals happen, the backend counts three block-compressed textures, and the validation
// layer was loaded and silent. Negative control (scripts/ci/check-bc-textures.py): with
// ZH_RENDER_NO_BLOCK_COMPRESSED set the backend refuses the compressed formats as it did before
// this path existed and the workload *must* fail at creation.

#include "png_write.h"
#include "render_backend.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace spike;

namespace {

constexpr uint32_t kBlock = 4;
constexpr uint32_t kTexEdge = 16;
constexpr uint32_t kBlocksPerEdge = kTexEdge / kBlock; // 4
constexpr uint32_t kBlocksPerLevel0 = kBlocksPerEdge * kBlocksPerEdge; // 16
constexpr uint32_t kLevels = 5; // 16 8 4 2 1
constexpr uint32_t kQuadPixels = 64; // 4 pixels per texel at level 0
constexpr uint32_t kLevel1Pixels = 8; // 16 texels into 8 pixels: LOD 1, so level 1
constexpr uint32_t kFormats = 3;
constexpr uint32_t kCasesPerFormat = 3;
constexpr uint32_t kCases = kFormats * kCasesPerFormat;
constexpr uint32_t kVertexBytes = 20; // D3DFVF_XYZ | D3DFVF_TEX1
constexpr uint32_t kWidth = kCasesPerFormat * kQuadPixels;
constexpr uint32_t kHeight = kFormats * kQuadPixels;

struct FormatUnderTest {
	const char* name;
	TextureFormat format;
	uint32_t block_bytes;
	bool has_alpha;
};

constexpr FormatUnderTest kFormatTable[kFormats] = {
    {"DXT1 (BC1)", TextureFormat::DXT1, 8, false},
    {"DXT3 (BC2)", TextureFormat::DXT3, 16, true},
    {"DXT5 (BC3)", TextureFormat::DXT5, 16, true},
};

struct Rgba {
	unsigned char r = 0, g = 0, b = 0, a = 0;
};

// The colour a block is encoded with, per level and block index. Kept to what 5:6:5 can
// hold exactly (multiples of 8 in r/b, 4 in g) so the expected readback is the decoder's
// exact expansion, not a rounding guess.
Rgba Block_Colour(uint32_t level, uint32_t block, bool updated) {
	Rgba c;
	c.r = static_cast<unsigned char>(((block & 3) * 64 + 32) & 0xF8);
	c.g = static_cast<unsigned char>((((block >> 2) & 3) * 64 + 32) & 0xFC);
	c.b = static_cast<unsigned char>((level * 48 + 24) & 0xF8);
	if (updated) c.b = static_cast<unsigned char>((0xF8 - c.b) & 0xF8);
	// 4-bit alpha (BC2) and 8-bit alpha (BC3) both have to reproduce this exactly, so it is
	// a multiple of 17.
	c.a = static_cast<unsigned char>(((block + level + (updated ? 7 : 0)) % 16) * 17);
	return c;
}

// 5:6:5 expansion as every decoder does it: replicate the top bits into the low ones.
Rgba Expanded(const Rgba& c, bool has_alpha) {
	Rgba e;
	const uint32_t r5 = c.r >> 3, g6 = c.g >> 2, b5 = c.b >> 3;
	e.r = static_cast<unsigned char>((r5 << 3) | (r5 >> 2));
	e.g = static_cast<unsigned char>((g6 << 2) | (g6 >> 4));
	e.b = static_cast<unsigned char>((b5 << 3) | (b5 >> 2));
	e.a = has_alpha ? c.a : 255;
	return e;
}

uint16_t Pack_565(const Rgba& c) {
	return static_cast<uint16_t>(((c.r >> 3) << 11) | ((c.g >> 2) << 5) | (c.b >> 3));
}

// One solid 4x4 block in the format's on-disk layout. BC1: two 5:6:5 endpoints then 16 2-bit
// indices, all 0 (endpoint 0). BC2: 16 explicit 4-bit alphas then the BC1 colour block. BC3:
// two 8-bit alpha endpoints, 16 3-bit indices (all 0), then the colour block. Endpoint 0 ==
// endpoint 1 on purpose: a decoder that read the block at the wrong offset could not produce
// the colour by accident from a neighbour's index bits.
void Encode_Block(const FormatUnderTest& f, const Rgba& c, uint8_t* out) {
	std::memset(out, 0, f.block_bytes);
	uint8_t* colour = out + (f.block_bytes - 8);
	const uint16_t packed = Pack_565(c);
	colour[0] = static_cast<uint8_t>(packed & 0xFF);
	colour[1] = static_cast<uint8_t>(packed >> 8);
	colour[2] = colour[0];
	colour[3] = colour[1];
	// indices bytes 4..7 stay 0
	if (f.format == TextureFormat::DXT3) {
		const uint8_t nibble = static_cast<uint8_t>(c.a >> 4);
		std::memset(out, (nibble << 4) | nibble, 8);
	} else if (f.format == TextureFormat::DXT5) {
		out[0] = c.a;
		out[1] = c.a;
		// 6 index bytes stay 0: every texel takes alpha0
	}
}

uint32_t Level_Edge(uint32_t level) { return kTexEdge >> level; }
uint32_t Level_Blocks_Per_Edge(uint32_t level) {
	return (Level_Edge(level) + kBlock - 1) / kBlock;
}
uint32_t Block_Pitch(const FormatUnderTest& f, uint32_t level) {
	return Level_Blocks_Per_Edge(level) * f.block_bytes;
}

class Workload {
public:
	explicit Workload(RenderBackend& gfx) : gfx_(gfx) {}
	bool Init();
	// Writes every level of every texture; `updated` false writes the initial colours.
	bool Fill_All_Levels();
	bool Lock_Sub_Rect_And_Update();
	bool Refuse_Misaligned_Lock();
	bool Read_Back_Level_2();
	bool Render_Frame();
	Rgba Pixel(uint32_t x, uint32_t y) const;
	bool Write_Frame(const std::string& path) const {
		return Write_Png(path, pixels_, format_.width, format_.height);
	}

private:
	bool Fill_Level(uint32_t fi, uint32_t level);

	RenderBackend& gfx_;
	TextureHandle* textures_[kFormats]{};
	VertexBufferHandle* vb_ = nullptr;
	IndexBufferHandle* ib_ = nullptr;
	std::string pixels_;
	SurfaceFormat format_{};
	int pitch_failures_ = 0;
};

bool Workload::Init() {
	for (uint32_t fi = 0; fi < kFormats; ++fi) {
		const FormatUnderTest& f = kFormatTable[fi];
		std::printf("format %s: device samples it %s\n", f.name,
		            gfx_.Supports_Texture_Format(f.format) ? "yes" : "no");
		textures_[fi] = gfx_.Create_Lockable_Texture(kTexEdge, kTexEdge, f.format, kLevels);
		if (textures_[fi] == nullptr) {
			std::fprintf(stderr, "bc-textures: the backend refused a block-compressed texture "
			                     "(%s)\n", f.name);
			return false;
		}
		std::printf("%s: %ux%u, %u levels created through the lockable path\n", f.name,
		            kTexEdge, kTexEdge, kLevels);
	}

	// Indices: one quad per case, addressed by start index (D3D8's min_vertex_index is a
	// hint, not a base).
	uint16_t indices[kCases * 6];
	for (uint32_t c = 0; c < kCases; ++c) {
		const uint16_t base = static_cast<uint16_t>(c * 4);
		const uint16_t quad[6] = {base, static_cast<uint16_t>(base + 1),
		                          static_cast<uint16_t>(base + 2), base,
		                          static_cast<uint16_t>(base + 2), static_cast<uint16_t>(base + 3)};
		std::memcpy(&indices[c * 6], quad, sizeof(quad));
	}
	ib_ = gfx_.Create_Index_Buffer(indices, kCases * 6);
	// One quad per case, all written in one lock per frame.
	vb_ = gfx_.Create_Lockable_Vertex_Buffer(kCases * 4 * kVertexBytes, D3DFVF_XYZ | D3DFVF_TEX1,
	                                         false);
	if (ib_ == nullptr || vb_ == nullptr) {
		std::fprintf(stderr, "bc-textures: geometry creation failed\n");
		return false;
	}

	gfx_.Set_DX8_Texture_Stage_State(0, D3DTSS_MINFILTER, D3DTEXF_POINT);
	gfx_.Set_DX8_Texture_Stage_State(0, D3DTSS_MAGFILTER, D3DTEXF_POINT);
	gfx_.Set_DX8_Texture_Stage_State(0, D3DTSS_MIPFILTER, D3DTEXF_POINT);
	gfx_.Set_DX8_Texture_Stage_State(0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
	gfx_.Set_DX8_Texture_Stage_State(0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
	gfx_.Set_DX8_Texture_Stage_State(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
	gfx_.Set_DX8_Texture_Stage_State(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
	gfx_.Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
	gfx_.Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
	for (uint32_t stage = 1; stage < 8; ++stage) {
		gfx_.Set_DX8_Texture_Stage_State(stage, D3DTSS_COLOROP, D3DTOP_DISABLE);
		gfx_.Set_DX8_Texture_Stage_State(stage, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
	}
	gfx_.Set_DX8_Render_State(D3DRS_CULLMODE, D3DCULL_NONE);
	gfx_.Set_DX8_Render_State(D3DRS_ZENABLE, 0);
	gfx_.Set_DX8_Render_State(D3DRS_ZWRITEENABLE, 0);
	gfx_.Set_DX8_Render_State(D3DRS_LIGHTING, 0);
	gfx_.Set_DX8_Render_State(D3DRS_ALPHABLENDENABLE, 0);
	gfx_.Set_DX8_Render_State(D3DRS_ALPHATESTENABLE, 0);
	gfx_.Set_DX8_Render_State(D3DRS_COLORWRITEENABLE, 0xF);
	gfx_.Set_Transform(D3DTS_WORLD, Matrix4x4::Identity());
	gfx_.Set_Transform(D3DTS_VIEW, Matrix4x4::Identity());
	gfx_.Set_Transform(D3DTS_PROJECTION, Matrix4x4::Identity());
	gfx_.Set_Fixed_Function_Fvf(D3DFVF_XYZ | D3DFVF_TEX1);
	gfx_.Set_Vertex_Shader(kNullShader);
	gfx_.Set_Pixel_Shader(kNullShader);
	return true;
}

bool Workload::Fill_Level(uint32_t fi, uint32_t level) {
	const FormatUnderTest& f = kFormatTable[fi];
	LockedRect locked;
	// The loader's own flags: a whole-level lock with no DISCARD (D3DLOCK_DISCARD is not
	// legal on a managed texture and the engine never passes it).
	if (!gfx_.Lock_Texture(textures_[fi], level, nullptr, 0, locked) || locked.bits == nullptr) {
		std::fprintf(stderr, "bc-textures: %s level %u lock failed\n", f.name, level);
		return false;
	}
	const uint32_t expected_pitch = Block_Pitch(f, level);
	const bool pitch_ok = locked.pitch == expected_pitch;
	if (!pitch_ok) ++pitch_failures_;
	std::printf("%s level %u (%u texels): lock pitch %u, block pitch %u %s\n", f.name, level,
	            Level_Edge(level), locked.pitch, expected_pitch, pitch_ok ? "ok" : "WRONG");
	const uint32_t blocks = Level_Blocks_Per_Edge(level);
	for (uint32_t by = 0; by < blocks; ++by) {
		for (uint32_t bx = 0; bx < blocks; ++bx) {
			// Written at the pitch the backend reported: if that is a texel pitch the blocks
			// land in the wrong rows and the draw shows it.
			uint8_t* dst = static_cast<uint8_t*>(locked.bits) +
			               static_cast<size_t>(by) * locked.pitch + bx * f.block_bytes;
			Encode_Block(f, Block_Colour(level, by * blocks + bx, false), dst);
		}
	}
	return gfx_.Unlock_Texture(textures_[fi], level);
}

bool Workload::Fill_All_Levels() {
	for (uint32_t fi = 0; fi < kFormats; ++fi)
		for (uint32_t level = 0; level < kLevels; ++level)
			if (!Fill_Level(fi, level)) return false;
	return pitch_failures_ == 0;
}

bool Workload::Lock_Sub_Rect_And_Update() {
	for (uint32_t fi = 0; fi < kFormats; ++fi) {
		const FormatUnderTest& f = kFormatTable[fi];
		// Blocks (1,1)..(2,2) of level 0: texels 4..12 both ways, block-aligned.
		LockRect rect;
		rect.left = 4; rect.top = 4; rect.right = 12; rect.bottom = 12;
		LockedRect locked;
		if (!gfx_.Lock_Texture(textures_[fi], 0, &rect, 0, locked) || locked.bits == nullptr) {
			std::fprintf(stderr, "bc-textures: %s aligned sub-rect lock failed\n", f.name);
			return false;
		}
		std::printf("%s sub-rect (4,4)-(12,12): lock pitch %u\n", f.name, locked.pitch);
		if (locked.pitch != Block_Pitch(f, 0)) ++pitch_failures_;
		for (uint32_t by = 0; by < 2; ++by) {
			for (uint32_t bx = 0; bx < 2; ++bx) {
				uint8_t* dst = static_cast<uint8_t*>(locked.bits) +
				               static_cast<size_t>(by) * locked.pitch + bx * f.block_bytes;
				const uint32_t block = (by + 1) * kBlocksPerEdge + (bx + 1);
				Encode_Block(f, Block_Colour(0, block, true), dst);
			}
		}
		if (!gfx_.Unlock_Texture(textures_[fi], 0)) return false;
	}
	return pitch_failures_ == 0;
}

bool Workload::Refuse_Misaligned_Lock() {
	bool all_refused = true;
	for (uint32_t fi = 0; fi < kFormats; ++fi) {
		const FormatUnderTest& f = kFormatTable[fi];
		LockRect rect;
		rect.left = 2; rect.top = 0; rect.right = 6; rect.bottom = 4;
		LockedRect locked;
		const bool locked_ok = gfx_.Lock_Texture(textures_[fi], 0, &rect, 0, locked);
		if (locked_ok) gfx_.Unlock_Texture(textures_[fi], 0);
		std::printf("%s misaligned sub-rect (2,0)-(6,4): %s\n", f.name,
		            locked_ok ? "ACCEPTED" : "refused");
		if (locked_ok) all_refused = false;
	}
	return all_refused;
}

bool Workload::Read_Back_Level_2() {
	bool all_match = true;
	for (uint32_t fi = 0; fi < kFormats; ++fi) {
		const FormatUnderTest& f = kFormatTable[fi];
		LockedRect locked;
		if (!gfx_.Lock_Texture(textures_[fi], 2, nullptr, LOCK_READONLY, locked) ||
		    locked.bits == nullptr) {
			std::fprintf(stderr, "bc-textures: %s level 2 read-only lock failed\n", f.name);
			return false;
		}
		uint8_t expected[16];
		Encode_Block(f, Block_Colour(2, 0, false), expected);
		const bool match = std::memcmp(locked.bits, expected, f.block_bytes) == 0;
		std::printf("%s level 2 read back: %u block bytes %s\n", f.name, f.block_bytes,
		            match ? "match" : "DIFFER");
		gfx_.Unlock_Texture(textures_[fi], 2);
		if (!match) all_match = false;
	}
	return all_match;
}

bool Workload::Render_Frame() {
	void* bits = nullptr;
	if (!gfx_.Lock_Vertex_Buffer(vb_, 0, kCases * 4 * kVertexBytes, 0, &bits) ||
	    bits == nullptr) {
		std::fprintf(stderr, "bc-textures: vertex lock failed\n");
		return false;
	}
	for (uint32_t c = 0; c < kCases; ++c) {
		const uint32_t fi = c / kCasesPerFormat;
		const uint32_t kind = c % kCasesPerFormat;
		// Case columns: 0 = level 0 (drawn full size), 1 = level 0 after the sub-rect
		// update -- the same texture, drawn again, since the update is in place -- and
		// 2 = level 1, drawn into 8x8 pixels so the point mip filter selects it.
		const float sx = 2.0f / static_cast<float>(kCasesPerFormat);
		const float sy = 2.0f / static_cast<float>(kFormats);
		// D3D clip space, +y up: the cell's top edge is y_top, and texel row 0 (v = 0) is
		// put on it so the image reads top-down like the target.
		const float x0 = -1.0f + sx * static_cast<float>(kind);
		const float y_top = 1.0f - sy * static_cast<float>(fi);
		const float scale = kind == 2 ? static_cast<float>(kLevel1Pixels) /
		                                    static_cast<float>(kQuadPixels)
		                              : 1.0f;
		const float x1 = x0 + sx * scale;
		const float y_bottom = y_top - sy * scale;
		const float v[4][5] = {{x0, y_top, 0.5f, 0.0f, 0.0f},
		                       {x1, y_top, 0.5f, 1.0f, 0.0f},
		                       {x1, y_bottom, 0.5f, 1.0f, 1.0f},
		                       {x0, y_bottom, 0.5f, 0.0f, 1.0f}};
		std::memcpy(static_cast<uint8_t*>(bits) + static_cast<size_t>(c) * 4 * kVertexBytes, v,
		            sizeof(v));
	}
	gfx_.Unlock_Vertex_Buffer(vb_);

	gfx_.Begin_Scene();
	gfx_.Clear(true, true, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0);
	gfx_.Set_Vertex_Buffer(vb_, 0, kVertexBytes);
	gfx_.Set_Index_Buffer(ib_, 0);
	for (uint32_t c = 0; c < kCases; ++c) {
		gfx_.Set_Texture(0, textures_[c / kCasesPerFormat]);
		gfx_.Draw_Triangles(c * 6, 2, c * 4, 4);
	}
	gfx_.End_Scene(false);
	return gfx_.Read_Back_Color_Target(pixels_, format_);
}

Rgba Workload::Pixel(uint32_t x, uint32_t y) const {
	const float scale = static_cast<float>(format_.width) / static_cast<float>(kWidth);
	const uint32_t px = static_cast<uint32_t>((static_cast<float>(x) + 0.5f) * scale);
	const uint32_t py = static_cast<uint32_t>((static_cast<float>(y) + 0.5f) * scale);
	const size_t offset = (static_cast<size_t>(py) * format_.width + px) * 4;
	if (offset + 3 >= pixels_.size()) return Rgba{};
	const auto* p = reinterpret_cast<const unsigned char*>(pixels_.data()) + offset;
	return Rgba{p[0], p[1], p[2], p[3]};
}

struct Verdict {
	uint32_t correct = 0;
	uint32_t wrong_colour = 0;
	uint32_t wrong_alpha = 0;
	uint32_t missing = 0;
};

// Decoders are allowed a little latitude on the 5:6:5 expansion; a wrong block is off by
// tens, not by one.
bool Near(unsigned char a, unsigned char b) { return (a > b ? a - b : b - a) <= 3; }

Verdict Classify_Case(const Workload& w, uint32_t c) {
	const uint32_t fi = c / kCasesPerFormat;
	const uint32_t kind = c % kCasesPerFormat;
	const FormatUnderTest& f = kFormatTable[fi];
	const uint32_t level = kind == 2 ? 1 : 0;
	const uint32_t blocks = Level_Blocks_Per_Edge(level);
	// Pixels per block on the target: the quad is 64 px for 16 texels at level 0 (16 px per
	// block) and 8 px for 8 level-1 texels (2 px per block).
	const uint32_t block_px = kind == 2 ? kBlock * kLevel1Pixels / Level_Edge(1)
	                                    : kBlock * (kQuadPixels / kTexEdge);
	const uint32_t origin_x = kind * kQuadPixels;
	const uint32_t origin_y = fi * kQuadPixels;
	Verdict v;
	for (uint32_t by = 0; by < blocks; ++by) {
		for (uint32_t bx = 0; bx < blocks; ++bx) {
			const uint32_t block = by * blocks + bx;
			const bool updated = kind == 1 && bx >= 1 && bx <= 2 && by >= 1 && by <= 2;
			const Rgba want = Expanded(Block_Colour(level, block, updated), f.has_alpha);
			const uint32_t x = origin_x + bx * block_px + block_px / 2;
			const uint32_t y = origin_y + by * block_px + block_px / 2;
			const Rgba got = w.Pixel(x, y);
			if (got.r == 0 && got.g == 0 && got.b == 0 && got.a == 0) { ++v.missing; continue; }
			if (!Near(got.r, want.r) || !Near(got.g, want.g) || !Near(got.b, want.b)) {
				++v.wrong_colour;
				continue;
			}
			if (!Near(got.a, want.a)) { ++v.wrong_alpha; continue; }
			++v.correct;
		}
	}
	return v;
}

} // namespace

int main(int argc, char** argv) {
	bool validation = false;
	std::string png;
	for (int i = 1; i < argc; ++i) {
		if (std::strcmp(argv[i], "--validation") == 0) validation = true;
		else if (std::strcmp(argv[i], "--png") == 0 && i + 1 < argc) png = argv[++i];
		else if (std::strcmp(argv[i], "--help") == 0) {
			std::printf("usage: %s [--validation] [--png final-frame.png]\n", argv[0]);
			return 0;
		}
	}

	RenderBackend* gfx = Create_Vulkan_Backend(validation, true);
	if (!gfx->Init(nullptr, kWidth, kHeight)) {
		std::fprintf(stderr, "bc-textures: backend Init failed\n");
		return 1;
	}
	std::printf("device: %s\n", gfx->Device_Description());

	Workload w(*gfx);
	if (!w.Init()) {
		std::printf("bc-textures: FAIL\n");
		return 1;
	}
	int status = 0;
	if (!w.Fill_All_Levels()) status = 1;
	std::printf("lock pitch: %s\n", status == 0 ? "block pitch on every level" : "WRONG");
	const bool refused = w.Refuse_Misaligned_Lock();
	std::printf("misaligned sub-rect locks: %s\n", refused ? "refused" : "ACCEPTED");
	if (!refused) status = 1;

	// Frame 0: as written. Then the sub-rect update and a level-2 read-back, then frame 1.
	if (!w.Render_Frame()) {
		std::fprintf(stderr, "bc-textures: readback failed\n");
		return 1;
	}
	// Orientation, measured before anything is classified: DXT1's block 0 (texel row 0,
	// column 0) has to be the top-left block of case 0. A transposed or flipped upload
	// would still be "16 blocks of the right colours" under a permissive classifier.
	{
		const Rgba want = Expanded(Block_Colour(0, 0, false), false);
		const Rgba got = w.Pixel(8, 8);
		const bool oriented = Near(got.r, want.r) && Near(got.g, want.g) && Near(got.b, want.b);
		std::printf("orientation: pixel (8,8) = %u %u %u, DXT1 block 0 wants %u %u %u: %s\n",
		            got.r, got.g, got.b, want.r, want.g, want.b,
		            oriented ? "top-left verified" : "NOT top-left");
		if (!oriented) status = 1;
	}
	Verdict frame0[kCases];
	for (uint32_t c = 0; c < kCases; ++c) frame0[c] = Classify_Case(w, c);
	if (!w.Lock_Sub_Rect_And_Update()) status = 1;
	if (!w.Read_Back_Level_2()) status = 1;
	if (!w.Render_Frame()) {
		std::fprintf(stderr, "bc-textures: readback failed\n");
		return 1;
	}
	for (uint32_t c = 0; c < kCases; ++c) {
		const uint32_t fi = c / kCasesPerFormat;
		const uint32_t kind = c % kCasesPerFormat;
		// Column 1 is read after the update, columns 0 and 2 from the first frame (column 0
		// is the same texture as column 1, so after the update it would carry the new
		// blocks too; the first frame is what proves the initial upload).
		const Verdict v = kind == 1 ? Classify_Case(w, c) : frame0[c];
		const uint32_t expected = kind == 2 ? 4 : kBlocksPerLevel0;
		static const char* const kKind[kCasesPerFormat] = {"level 0 as written",
		                                                    "level 0 after sub-rect update",
		                                                    "level 1 via mip filter"};
		std::printf("case %u %s %s\n", c, kFormatTable[fi].name, kKind[kind]);
		std::printf("  blocks correct          %u\n", v.correct);
		std::printf("  blocks wrong-colour     %u\n", v.wrong_colour);
		std::printf("  blocks wrong-alpha      %u\n", v.wrong_alpha);
		std::printf("  blocks missing          %u\n", v.missing);
		if (v.correct != expected) status = 1;
	}

	if (!png.empty()) {
		std::printf("frame written: %s (%s)\n", png.c_str(),
		            w.Write_Frame(png) ? "ok" : "FAILED");
	}

	const ResourceStats stats = gfx->Get_Resource_Stats();
	std::printf("backend block-compressed textures %u\n", stats.block_compressed_textures);
	if (stats.block_compressed_textures != kFormats) status = 1;

	std::printf("validation layer: %s\n", gfx->Validation_Active() ? "loaded" : "not requested");
	std::printf("validation messages: %u\n", gfx->Validation_Message_Count());
	if (validation && !gfx->Validation_Active()) status = 1;
	if (gfx->Validation_Message_Count() != 0) status = 1;

	gfx->Shutdown();
	delete gfx;
	std::printf("%s\n", status == 0 ? "bc-textures: PASS" : "bc-textures: FAIL");
	return status;
}
