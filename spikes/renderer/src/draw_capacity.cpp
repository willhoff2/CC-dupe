// Renderer spike: how many draws a single frame can carry, and what happens to the ones
// past that point.
//
// A real Zero Hour mission frame issues thousands of DrawIndexedPrimitive calls. The
// backend keeps one descriptor set and one uniform-buffer slice per draw per frame, so
// "draws per frame" is a resource limit rather than a performance figure, and exceeding it
// has to be *measured* per draw: a frame that lost a draw and a frame that drew geometry
// with another draw's texture look equally plausible on screen.
//
// The measurement is a grid. Draw i owns one tile of the colour target and writes:
//   * rgb  <- its own 1x1 texture, whose colour encodes i          (image descriptor)
//   * a    <- D3DRS_TEXTUREFACTOR, whose alpha encodes i           (uniform buffer)
//   * xy   <- its own world transform, which places the tile       (uniform buffer)
// so every per-draw resource is visible in the readback and every failure mode is a
// different picture:
//   * tile still the clear colour            -> the draw was dropped
//   * tile carries another draw's id         -> a live descriptor set was reused (aliased)
//   * tile's alpha disagrees with its rgb    -> the uniform slice aliased separately
// Anything else is reported as a mismatch rather than silently averaged away.
//
// Exit status is 0 only when every requested draw landed in its own tile with its own id.

#include "render_backend.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace spike;

namespace {

// 4x4 pixel tiles on a 512x512 target: 128x128 = 16384 distinguishable draws, which is
// above the thousands a mission frame issues, and small enough to read back per frame.
constexpr uint32_t kTilePixels = 4;
constexpr uint32_t kGridCols = 128;
constexpr uint32_t kGridRows = 128;
constexpr uint32_t kWidth = kGridCols * kTilePixels;
constexpr uint32_t kHeight = kGridRows * kTilePixels;
constexpr uint32_t kMaxTiles = kGridCols * kGridRows;

struct Vertex {
	float x, y, z;
	float u, v;
};

struct Rgba {
	unsigned char r = 0, g = 0, b = 0, a = 0;
};

// The id lives in two bytes of colour plus a marker, so a tile that carries the wrong id
// names the draw it came from instead of merely being "wrong". 0x40 in blue separates a
// drawn tile from the black clear.
constexpr unsigned char kDrawnMarker = 0x40;

uint32_t Texture_Argb_For_Draw(uint32_t i) {
	const uint32_t r = (i >> 8) & 0xFFu;
	const uint32_t g = i & 0xFFu;
	return 0xFF000000u | (r << 16) | (g << 8) | kDrawnMarker;
}

// The uniform's own copy of the id: the low byte, offset so that a tile whose alpha came
// from a different draw's uniform slice cannot coincide with its own rgb by construction.
unsigned char Uniform_Alpha_For_Draw(uint32_t i) {
	return static_cast<unsigned char>(((i & 0xFFu) ^ 0xA5u) | 0x01u);
}

uint32_t Texture_Factor_For_Draw(uint32_t i) {
	return (static_cast<uint32_t>(Uniform_Alpha_For_Draw(i)) << 24) | 0x00FFFFFFu;
}

// Decodes the draw id a tile's rgb claims to come from, or -1 when the tile was never
// drawn or carries something that is not an id at all.
int64_t Decode_Draw_Id(const Rgba& p) {
	if (p.b != kDrawnMarker) return -1;
	return (static_cast<int64_t>(p.r) << 8) | p.g;
}

Matrix4x4 Translation(float x, float y, float z) {
	Matrix4x4 m = Matrix4x4::Identity();
	m.m[3][0] = x;
	m.m[3][1] = y;
	m.m[3][2] = z;
	return m;
}

Matrix4x4 Scale(float x, float y, float z) {
	Matrix4x4 m = Matrix4x4::Identity();
	m.m[0][0] = x;
	m.m[1][1] = y;
	m.m[2][2] = z;
	return m;
}

Matrix4x4 Multiply(const Matrix4x4& a, const Matrix4x4& b) {
	Matrix4x4 out{};
	for (int row = 0; row < 4; ++row)
		for (int col = 0; col < 4; ++col) {
			float sum = 0.0f;
			for (int k = 0; k < 4; ++k) sum += a.m[row][k] * b.m[k][col];
			out.m[row][col] = sum;
		}
	return out;
}

// Where draw i's tile goes, in the grid.
uint32_t Tile_Col(uint32_t i) { return i % kGridCols; }
uint32_t Tile_Row(uint32_t i) { return i / kGridCols; }

// The unit quad placed over one tile, in clip space. `y_down` is measured rather than
// assumed: the backend puts D3D8's y flip in the projection matrix, so which end of clip
// space the first row of pixels is at is a property of the backend, not of this file.
Matrix4x4 Tile_Transform(uint32_t col, uint32_t row, bool y_down) {
	const float sx = 2.0f / static_cast<float>(kGridCols);
	const float sy = 2.0f / static_cast<float>(kGridRows);
	const float x = -1.0f + sx * static_cast<float>(col);
	const float y_top = -1.0f + sy * static_cast<float>(row);
	const float y = y_down ? y_top : (1.0f - sy - sy * static_cast<float>(row));
	return Multiply(Scale(sx, sy, 1.0f), Translation(x, y, 0.0f));
}

class Grid {
public:
	explicit Grid(RenderBackend& gfx) : gfx_(gfx) {}

	bool Init();
	// Issues `draws` draws in one frame, then reads the target back.
	bool Render_Frame(uint32_t draws, bool y_down);
	Rgba Tile_Pixel(uint32_t col, uint32_t row) const;
	// Ensures a texture exists for every draw id up to `draws`.
	bool Ensure_Textures(uint32_t draws);

private:
	RenderBackend& gfx_;
	std::vector<TextureHandle*> textures_;
	VertexBufferHandle* vb_ = nullptr;
	IndexBufferHandle* ib_ = nullptr;
	std::string pixels_;
	SurfaceFormat format_{};
};

bool Grid::Init() {
	const Vertex quad[4] = {{0.0f, 0.0f, 0.5f, 0.5f, 0.5f},
	                        {1.0f, 0.0f, 0.5f, 0.5f, 0.5f},
	                        {1.0f, 1.0f, 0.5f, 0.5f, 0.5f},
	                        {0.0f, 1.0f, 0.5f, 0.5f, 0.5f}};
	const uint16_t indices[6] = {0, 1, 2, 0, 2, 3};
	vb_ = gfx_.Create_Vertex_Buffer(quad, sizeof(quad), D3DFVF_XYZ | D3DFVF_TEX1);
	ib_ = gfx_.Create_Index_Buffer(indices, 6);
	if (vb_ == nullptr || ib_ == nullptr) {
		std::fprintf(stderr, "draw-capacity: buffer creation failed\n");
		return false;
	}

	// One texture selected straight into rgb, one texture factor selected straight into
	// alpha: no modulation, so both ids survive the cascade byte-exact.
	gfx_.Set_DX8_Texture_Stage_State(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
	gfx_.Set_DX8_Texture_Stage_State(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
	gfx_.Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
	gfx_.Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
	gfx_.Set_DX8_Texture_Stage_State(0, D3DTSS_MINFILTER, D3DTEXF_POINT);
	gfx_.Set_DX8_Texture_Stage_State(0, D3DTSS_MAGFILTER, D3DTEXF_POINT);
	gfx_.Set_DX8_Texture_Stage_State(0, D3DTSS_MIPFILTER, D3DTEXF_NONE);
	for (uint32_t stage = 1; stage < 8; ++stage) {
		gfx_.Set_DX8_Texture_Stage_State(stage, D3DTSS_COLOROP, D3DTOP_DISABLE);
		gfx_.Set_DX8_Texture_Stage_State(stage, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
	}
	gfx_.Set_DX8_Render_State(D3DRS_CULLMODE, D3DCULL_NONE);
	gfx_.Set_DX8_Render_State(D3DRS_ZENABLE, 0);
	gfx_.Set_DX8_Render_State(D3DRS_ZWRITEENABLE, 0);
	gfx_.Set_DX8_Render_State(D3DRS_LIGHTING, 0);
	gfx_.Set_DX8_Render_State(D3DRS_ALPHABLENDENABLE, 0);
	gfx_.Set_DX8_Render_State(D3DRS_COLORWRITEENABLE, 0xF);
	gfx_.Set_Transform(D3DTS_VIEW, Matrix4x4::Identity());
	gfx_.Set_Transform(D3DTS_PROJECTION, Matrix4x4::Identity());
	gfx_.Set_Vertex_Buffer(vb_, 0);
	gfx_.Set_Index_Buffer(ib_, 0);
	return true;
}

bool Grid::Ensure_Textures(uint32_t draws) {
	textures_.reserve(draws);
	while (textures_.size() < draws) {
		const uint32_t argb = Texture_Argb_For_Draw(static_cast<uint32_t>(textures_.size()));
		// A8R8G8B8 is little-endian in memory: b, g, r, a.
		const uint8_t pixel[4] = {static_cast<uint8_t>(argb & 0xFFu),
		                          static_cast<uint8_t>((argb >> 8) & 0xFFu),
		                          static_cast<uint8_t>((argb >> 16) & 0xFFu),
		                          static_cast<uint8_t>((argb >> 24) & 0xFFu)};
		TextureHandle* texture = gfx_.Create_Texture(1, 1, pixel);
		if (texture == nullptr) {
			std::fprintf(stderr, "draw-capacity: texture %zu creation failed\n",
			             textures_.size());
			return false;
		}
		textures_.push_back(texture);
	}
	return true;
}

bool Grid::Render_Frame(uint32_t draws, bool y_down) {
	if (!Ensure_Textures(draws)) return false;
	gfx_.Begin_Scene();
	gfx_.Clear(true, true, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0);
	for (uint32_t i = 0; i < draws; ++i) {
		gfx_.Set_Texture(0, textures_[i]);
		gfx_.Set_DX8_Render_State(D3DRS_TEXTUREFACTOR, Texture_Factor_For_Draw(i));
		gfx_.Set_Transform(D3DTS_WORLD, Tile_Transform(Tile_Col(i), Tile_Row(i), y_down));
		gfx_.Draw_Triangles(0, 2, 0, 4);
	}
	gfx_.End_Scene(false);
	return gfx_.Read_Back_Color_Target(pixels_, format_);
}

Rgba Grid::Tile_Pixel(uint32_t col, uint32_t row) const {
	// The centre of the tile, in pixels, at whatever scale the target was made at.
	const float scale = static_cast<float>(format_.width) / static_cast<float>(kWidth);
	const uint32_t x = static_cast<uint32_t>((static_cast<float>(col) + 0.5f) *
	                                         static_cast<float>(kTilePixels) * scale);
	const uint32_t y = static_cast<uint32_t>((static_cast<float>(row) + 0.5f) *
	                                         static_cast<float>(kTilePixels) * scale);
	const size_t offset = (static_cast<size_t>(y) * format_.width + x) * 4;
	if (offset + 3 >= pixels_.size()) return Rgba{};
	const auto* p = reinterpret_cast<const unsigned char*>(pixels_.data()) + offset;
	return Rgba{p[0], p[1], p[2], p[3]};
}

struct Verdict {
	uint32_t correct = 0;
	uint32_t dropped = 0;   // tile never written
	uint32_t aliased = 0;   // tile carries a different draw's texture id
	uint32_t uniform_aliased = 0; // tile's own id, another draw's uniform alpha
	uint32_t mismatched = 0;      // written, but not with any draw's id
	int64_t first_dropped = -1;
	int64_t first_aliased_tile = -1;
	int64_t first_aliased_source = -1;
};

Verdict Classify(const Grid& grid, uint32_t draws) {
	Verdict v;
	for (uint32_t i = 0; i < draws; ++i) {
		const Rgba p = grid.Tile_Pixel(Tile_Col(i), Tile_Row(i));
		const int64_t id = Decode_Draw_Id(p);
		if (id < 0) {
			if (p.r == 0 && p.g == 0 && p.b == 0) {
				++v.dropped;
				if (v.first_dropped < 0) v.first_dropped = i;
			} else {
				++v.mismatched;
			}
			continue;
		}
		if (static_cast<uint32_t>(id) != i) {
			++v.aliased;
			if (v.first_aliased_tile < 0) {
				v.first_aliased_tile = i;
				v.first_aliased_source = id;
			}
			continue;
		}
		if (p.a != Uniform_Alpha_For_Draw(i)) {
			++v.uniform_aliased;
			continue;
		}
		++v.correct;
	}
	return v;
}

void Print_Verdict(const char* label, uint32_t draws, const Verdict& v) {
	std::printf("%s: %u draws requested\n", label, draws);
	std::printf("  tiles correct           %u\n", v.correct);
	std::printf("  tiles dropped           %u\n", v.dropped);
	std::printf("  tiles aliased           %u\n", v.aliased);
	std::printf("  tiles uniform-aliased   %u\n", v.uniform_aliased);
	std::printf("  tiles mismatched        %u\n", v.mismatched);
	if (v.first_dropped >= 0)
		std::printf("  first dropped draw      %lld\n",
		            static_cast<long long>(v.first_dropped));
	if (v.first_aliased_tile >= 0)
		std::printf("  first aliased draw      %lld shows draw %lld's texture\n",
		            static_cast<long long>(v.first_aliased_tile),
		            static_cast<long long>(v.first_aliased_source));
}

} // namespace

int main(int argc, char** argv) {
	uint32_t draws = 4096;
	uint32_t frames = 1;
	bool validation = false;
	for (int i = 1; i < argc; ++i) {
		if (std::strcmp(argv[i], "--draws") == 0 && i + 1 < argc)
			draws = static_cast<uint32_t>(std::atoi(argv[++i]));
		else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
			frames = static_cast<uint32_t>(std::atoi(argv[++i]));
		else if (std::strcmp(argv[i], "--validation") == 0)
			validation = true;
		else if (std::strcmp(argv[i], "--help") == 0) {
			std::printf("usage: %s [--draws N] [--frames N] [--validation]\n", argv[0]);
			return 0;
		}
	}
	if (draws == 0 || draws > kMaxTiles) {
		std::fprintf(stderr, "draw-capacity: --draws must be 1..%u\n", kMaxTiles);
		return 2;
	}

	RenderBackend* gfx = Create_Vulkan_Backend(validation, true);
	if (!gfx->Init(nullptr, kWidth, kHeight)) {
		std::fprintf(stderr, "draw-capacity: backend Init failed\n");
		return 1;
	}
	std::printf("device: %s\n", gfx->Device_Description());

	Grid grid(*gfx);
	if (!grid.Init()) return 1;

	// Which end of clip space the first row of pixels is at, measured with one draw
	// rather than assumed. Both orientations are tried and the one that puts draw 0 in
	// tile (0,0) wins; if neither does, the harness cannot tell a dropped draw from a
	// misplaced one and says so instead of reporting a number.
	bool y_down = true;
	bool oriented = false;
	for (int attempt = 0; attempt < 2 && !oriented; ++attempt) {
		y_down = attempt == 0;
		if (!grid.Render_Frame(1, y_down)) {
			std::fprintf(stderr, "draw-capacity: readback failed\n");
			return 1;
		}
		oriented = Decode_Draw_Id(grid.Tile_Pixel(0, 0)) == 0;
	}
	if (!oriented) {
		std::fprintf(stderr, "draw-capacity: orientation probe failed - a single draw did "
		                     "not land in tile (0,0) either way up\n");
		return 1;
	}
	std::printf("orientation: clip y %s, tile 0 verified\n", y_down ? "down" : "up");

	int status = 0;
	for (uint32_t frame = 0; frame < frames; ++frame) {
		if (!grid.Render_Frame(draws, y_down)) {
			std::fprintf(stderr, "draw-capacity: readback failed\n");
			return 1;
		}
		const Verdict v = Classify(grid, draws);
		if (frames > 1) std::printf("frame %u\n", frame);
		Print_Verdict("draws-per-frame", draws, v);
		if (v.correct != draws) status = 1;
	}

	DrawStats stats{};
	gfx->Get_Draw_Stats(stats);
	std::printf("backend draws requested %u\n", stats.draws_requested);
	std::printf("backend draws issued    %u\n", stats.draws_issued);
	std::printf("backend draws dropped   %u\n", stats.draws_dropped);
	std::printf("backend peak draws      %u\n", stats.peak_draws_per_frame);
	std::printf("descriptor sets         %u in %u block(s)\n", stats.descriptor_capacity,
	            stats.descriptor_blocks);
	if (stats.draws_dropped != 0) status = 1;
	if (stats.draws_requested != 0 && stats.draws_requested != draws) status = 1;

	std::printf("validation layer: %s\n", gfx->Validation_Active() ? "loaded" : "not requested");
	std::printf("validation messages: %u\n", gfx->Validation_Message_Count());
	if (validation && !gfx->Validation_Active()) status = 1;
	if (gfx->Validation_Message_Count() != 0) status = 1;

	gfx->Shutdown();
	delete gfx;
	std::printf("%s\n", status == 0 ? "draw-capacity: PASS" : "draw-capacity: FAIL");
	return status;
}
