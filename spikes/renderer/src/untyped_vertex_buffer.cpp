// Renderer spike: vertex buffers created with FVF 0, drawn with the layout that is bound
// at draw time.
//
// D3D8 lets CreateVertexBuffer take an FVF of 0: the bytes have no layout of their own and
// are read with whatever SetVertexShader has bound when they are drawn. Zero Hour's two
// shadow managers create their dynamic buffers exactly this way and bind the concrete FVF
// (D3DFVF_XYZ for shadow volumes, XYZ|DIFFUSE|TEX1 for projected shadow decals) just
// before drawing. A backend that resolves the layout at buffer creation cannot create them
// at all, which is where `Decode_Fvf: unsupported FVF 0x0` came from.
//
// The other way an FVF-0 buffer gets a layout is a *vertex declaration*: SetVertexShader
// with a handle CreateVertexShader returned for a D3DVSD_* token stream, which is how the
// engine's programmable paths (W3DShaderManager, W3DWater, W3DTreeBuffer) describe their
// streams. Zero Hour's declarations are three shapes; two are exercised here as written
// and a third is one no FVF can express, so a backend that quietly read the buffer with a
// stale FVF instead of the declaration would miscolour the row.
//
// The proof is a readback, per draw, in the shape of draw_capacity.cpp: every draw owns a
// 4x4 tile of the target and its id has to arrive through the attribute the layout under
// test supplies. Each case is one of the layouts the engine binds over such a buffer, plus
// one that binds an FVF at a larger explicit stride (SetStreamSource's), plus one draw with
// nothing bound at all, which must be *refused and counted*, not read with a stale layout:
//   case  layout bound at draw               id in rgb from        id in alpha from  stride
//   0     FVF XYZ                            D3DRS_TEXTUREFACTOR   TEXTUREFACTOR     12
//   1     FVF XYZ|DIFFUSE                    vertex diffuse        vertex diffuse    16
//   2     FVF XYZ|DIFFUSE|TEX1               texture               vertex diffuse    24
//   3     FVF XYZ|DIFFUSE|TEX1               texture               vertex diffuse    32 (padded)
//   4     decl FLOAT3 D3DCOLOR FLOAT2        texture (via oT0)     vertex diffuse    24
//   5     decl FLOAT3 FLOAT3 D3DCOLOR FLOAT2 texture (via oT0)     vertex diffuse    36
//   6     decl FLOAT3 FLOAT2 D3DCOLOR        texture (via oT0)     vertex diffuse    24
//   7     none                               -- must be dropped, and DrawStats must say why --
// Tile positions are written into the untyped buffer itself in clip space, so a layout
// resolved at the wrong stride or offset misplaces or miscolours tiles instead of merely
// validating.
//
// Exit status is 0 only when every case's tiles carry their own id, case 7's tile is
// untouched and counted under untyped_draws_dropped, and the validation layer was loaded
// and silent. Two negative controls, both required by scripts/ci/check-untyped-vertex-buffer.py:
// with ZH_RENDER_NO_UNTYPED_VB set the backend refuses the FVF-0 buffer as it did before it
// had this path and the workload fails at creation; with ZH_RENDER_NO_VERTEX_DECLARATION
// set the backend ignores the bound program's declaration as it did before it had that
// path, and cases 4-6 come back dropped.

#include "render_backend.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace spike;

namespace {

constexpr uint32_t kTilePixels = 4;
constexpr uint32_t kGridCols = 64;
constexpr uint32_t kGridRows = 64;
constexpr uint32_t kWidth = kGridCols * kTilePixels;
constexpr uint32_t kHeight = kGridRows * kTilePixels;

// Tiles per case: one row of the grid each, so a case's tiles are contiguous and a stride
// fault in one case cannot land in another's row by accident.
constexpr uint32_t kTilesPerCase = kGridCols;
constexpr uint32_t kDrawnCases = 7;
constexpr uint32_t kRefusedCase = 7;
constexpr uint32_t kCases = kDrawnCases + 1;
constexpr uint32_t kTiles = kCases * kTilesPerCase;

// What a vertex is made of, in stream order. The FVF cases are the engine's own shapes
// over FVF-0 buffers (W3DVolumetricShadow.cpp, W3DProjectedShadow.cpp) in D3D8's FVF
// element order; the declaration cases follow their D3DVSD_REG order.
enum Element : uint8_t { E_END = 0, E_POS, E_NORMAL, E_COLOR, E_UV, E_PAD8 };
constexpr uint32_t kMaxElements = 5;

uint32_t Element_Bytes(Element e) {
	switch (e) {
	case E_POS: return 12;
	case E_NORMAL: return 12;
	case E_COLOR: return 4;
	case E_UV: return 8;
	case E_PAD8: return 8;
	default: return 0;
	}
}

// --- D3D8 token assembly, as fixedfunc_tests.cpp encodes it -----------------------------
constexpr uint32_t kVsVersion11 = 0xfffe0101u;
constexpr uint32_t kEndToken = 0x0000ffffu;
constexpr uint32_t kNoSwizzle = 0xe4u << 16;
enum : uint32_t { kTypeInput = 1, kTypeRastOut = 4, kTypeAttrOut = 5, kTypeTexCrdOut = 6 };
constexpr uint32_t Dst(uint32_t type, uint32_t reg) {
	return 0x80000000u | (type << 28) | (0xfu << 16) | reg;
}
constexpr uint32_t Src(uint32_t type, uint32_t reg) {
	return 0x80000000u | (type << 28) | kNoSwizzle | reg;
}
constexpr uint32_t kOpMov = 0x00000001u;
// D3DVSD_STREAM(0), D3DVSD_REG(reg, type), D3DVSD_END().
constexpr uint32_t kDeclStream0 = 0x20000000u;
constexpr uint32_t Decl_Reg(uint32_t reg, uint32_t type) {
	return 0x40000000u | (type << 16) | reg;
}
constexpr uint32_t kDeclEnd = 0xffffffffu;
constexpr uint32_t kFloat2 = 1, kFloat3 = 2, kD3dColor = 4;

// W3DWater.cpp's declaration: position, colour, one texture coordinate.
constexpr uint32_t kWaterDeclaration[] = {kDeclStream0, Decl_Reg(0, kFloat3),
                                          Decl_Reg(1, kD3dColor), Decl_Reg(2, kFloat2),
                                          kDeclEnd};
constexpr uint32_t kWaterProgram[] = {
    kVsVersion11,
    kOpMov, Dst(kTypeRastOut, 0), Src(kTypeInput, 0),    // mov oPos, v0
    kOpMov, Dst(kTypeAttrOut, 0), Src(kTypeInput, 1),    // mov oD0, v1
    kOpMov, Dst(kTypeTexCrdOut, 0), Src(kTypeInput, 2),  // mov oT0, v2
    kEndToken};
// W3DTreeBuffer.cpp's declaration: position, normal, colour, texture coordinate in v7.
constexpr uint32_t kTreeDeclaration[] = {kDeclStream0, Decl_Reg(0, kFloat3),
                                         Decl_Reg(1, kFloat3), Decl_Reg(2, kD3dColor),
                                         Decl_Reg(7, kFloat2), kDeclEnd};
constexpr uint32_t kTreeProgram[] = {
    kVsVersion11,
    kOpMov, Dst(kTypeRastOut, 0), Src(kTypeInput, 0),    // mov oPos, v0
    kOpMov, Dst(kTypeAttrOut, 0), Src(kTypeInput, 2),    // mov oD0, v2
    kOpMov, Dst(kTypeTexCrdOut, 0), Src(kTypeInput, 7),  // mov oT0, v7
    kEndToken};
// Not an engine shape: texture coordinate before colour, an order no D3DFVF_* code can
// express. Reading it with any FVF puts the colour bytes where the u,v should be.
constexpr uint32_t kPermutedDeclaration[] = {kDeclStream0, Decl_Reg(0, kFloat3),
                                             Decl_Reg(1, kFloat2), Decl_Reg(2, kD3dColor),
                                             kDeclEnd};
constexpr uint32_t kPermutedProgram[] = {
    kVsVersion11,
    kOpMov, Dst(kTypeRastOut, 0), Src(kTypeInput, 0),    // mov oPos, v0
    kOpMov, Dst(kTypeAttrOut, 0), Src(kTypeInput, 2),    // mov oD0, v2
    kOpMov, Dst(kTypeTexCrdOut, 0), Src(kTypeInput, 1),  // mov oT0, v1
    kEndToken};

struct Case {
	const char* name;
	uint32_t fvf;      // bound through Set_Fixed_Function_Fvf before the draw; 0 = none
	uint32_t stride;   // passed to Set_Vertex_Buffer; 0 = the layout's own
	uint32_t vertex_bytes;
	Element elements[kMaxElements];
	const uint32_t* declaration; // with `program`, bound through Set_Vertex_Shader; null = FVF
	const uint32_t* program;
};

constexpr Case kCaseTable[kCases] = {
    {"XYZ (shadow volume)", D3DFVF_XYZ, 0, 12, {E_POS}, nullptr, nullptr},
    {"XYZ|DIFFUSE (SV_DEBUG volume)", D3DFVF_XYZ | D3DFVF_DIFFUSE, 0, 16, {E_POS, E_COLOR},
     nullptr, nullptr},
    {"XYZ|DIFFUSE|TEX1 (shadow decal)", D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1, 0, 24,
     {E_POS, E_COLOR, E_UV}, nullptr, nullptr},
    {"XYZ|DIFFUSE|TEX1 at stride 32", D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1, 32, 32,
     {E_POS, E_COLOR, E_UV, E_PAD8}, nullptr, nullptr},
    {"declaration FLOAT3 D3DCOLOR FLOAT2 (water)", 0, 0, 24, {E_POS, E_COLOR, E_UV},
     kWaterDeclaration, kWaterProgram},
    {"declaration FLOAT3 FLOAT3 D3DCOLOR FLOAT2 (trees)", 0, 0, 36,
     {E_POS, E_NORMAL, E_COLOR, E_UV}, kTreeDeclaration, kTreeProgram},
    {"declaration FLOAT3 FLOAT2 D3DCOLOR (no FVF order)", 0, 0, 24, {E_POS, E_UV, E_COLOR},
     kPermutedDeclaration, kPermutedProgram},
    {"nothing bound (must be refused)", 0, 0, 12, {E_POS}, nullptr, nullptr},
};

bool Case_Has(const Case& cs, Element e) {
	for (uint32_t i = 0; i < kMaxElements && cs.elements[i] != E_END; ++i)
		if (cs.elements[i] == e) return true;
	return false;
}

struct Rgba {
	unsigned char r = 0, g = 0, b = 0, a = 0;
};

// Tile id in two colour bytes plus a marker that separates a drawn tile from the clear.
constexpr unsigned char kDrawnMarker = 0x40;

// The alpha copy of the id, offset so a tile whose alpha came from elsewhere cannot agree
// with its rgb by construction.
unsigned char Alpha_For_Tile(uint32_t t) {
	return static_cast<unsigned char>(((t & 0xFFu) ^ 0xA5u) | 0x01u);
}

uint32_t Argb_For_Tile(uint32_t t) {
	const uint32_t r = (t >> 8) & 0xFFu;
	const uint32_t g = t & 0xFFu;
	return (static_cast<uint32_t>(Alpha_For_Tile(t)) << 24) | (r << 16) | (g << 8) |
	       kDrawnMarker;
}

int64_t Decode_Tile_Id(const Rgba& p) {
	if (p.b != kDrawnMarker) return -1;
	return (static_cast<int64_t>(p.r) << 8) | p.g;
}

// Every case's vertices start at a byte offset that is a whole number of its own vertices
// from the start of the buffer, so the index buffer can address them as vertex numbers at
// that case's stride. 288 is the lcm of the strides in kCaseTable (12, 16, 24, 32, 36).
constexpr size_t kCaseAlignment = 288;

size_t Case_Region_Bytes(uint32_t c) {
	const size_t bytes = static_cast<size_t>(kCaseTable[c].vertex_bytes) * 4 * kTilesPerCase;
	return (bytes + kCaseAlignment - 1) / kCaseAlignment * kCaseAlignment;
}

size_t Case_Offset(uint32_t c) {
	size_t offset = 0;
	for (uint32_t i = 0; i < c; ++i) offset += Case_Region_Bytes(i);
	return offset;
}

uint32_t Case_Base_Vertex(uint32_t c) {
	return static_cast<uint32_t>(Case_Offset(c) / kCaseTable[c].vertex_bytes);
}

uint32_t Tile_Col(uint32_t t) { return t % kGridCols; }
uint32_t Tile_Row(uint32_t t) { return t / kGridCols; }

// The four corners of tile t in clip space. `y_down` is measured, not assumed, as in
// draw_capacity.cpp.
void Tile_Corners(uint32_t t, bool y_down, float out[4][3]) {
	const float sx = 2.0f / static_cast<float>(kGridCols);
	const float sy = 2.0f / static_cast<float>(kGridRows);
	const float x0 = -1.0f + sx * static_cast<float>(Tile_Col(t));
	const float row = static_cast<float>(Tile_Row(t));
	const float y0 = y_down ? (-1.0f + sy * row) : (1.0f - sy - sy * row);
	const float x1 = x0 + sx;
	const float y1 = y0 + sy;
	const float c[4][3] = {{x0, y0, 0.5f}, {x1, y0, 0.5f}, {x1, y1, 0.5f}, {x0, y1, 0.5f}};
	std::memcpy(out, c, sizeof(c));
}

class Workload {
public:
	explicit Workload(RenderBackend& gfx) : gfx_(gfx) {}
	bool Init();
	bool Render_Frame(bool y_down);
	Rgba Tile_Pixel(uint32_t col, uint32_t row) const;

private:
	// Writes case c's tiles into `bytes` at that case's stride.
	void Fill_Case(uint32_t c, bool y_down, uint8_t* bytes) const;

	RenderBackend& gfx_;
	std::vector<TextureHandle*> textures_;
	VertexBufferHandle* vb_ = nullptr;
	IndexBufferHandle* ib_[kCases]{}; // one per case: vertex numbers differ per stride
	ShaderHandle shader_[kCases]{};   // the declaration cases' programs; kNullShader otherwise
	size_t vb_bytes_ = 0;
	std::string pixels_;
	SurfaceFormat format_{};
};

bool Workload::Init() {
	vb_bytes_ = Case_Offset(kCases);
	// The buffer under test: FVF 0, dynamic, exactly as both shadow managers create theirs.
	vb_ = gfx_.Create_Lockable_Vertex_Buffer(vb_bytes_, 0, true);
	if (vb_ == nullptr) {
		std::fprintf(stderr, "untyped-vb: the backend refused a vertex buffer with FVF 0\n");
		return false;
	}
	std::printf("untyped vertex buffer: %zu bytes created with FVF 0\n", vb_bytes_);

	for (uint32_t c = 0; c < kCases; ++c) {
		std::vector<uint16_t> indices(kTilesPerCase * 6);
		for (uint32_t i = 0; i < kTilesPerCase; ++i) {
			const uint16_t base = static_cast<uint16_t>(Case_Base_Vertex(c) + i * 4);
			const uint16_t quad[6] = {base, static_cast<uint16_t>(base + 1),
			                          static_cast<uint16_t>(base + 2), base,
			                          static_cast<uint16_t>(base + 2),
			                          static_cast<uint16_t>(base + 3)};
			std::memcpy(&indices[i * 6], quad, sizeof(quad));
		}
		ib_[c] = gfx_.Create_Index_Buffer(indices.data(), indices.size());
		if (ib_[c] == nullptr) {
			std::fprintf(stderr, "untyped-vb: index buffer creation failed\n");
			return false;
		}
		shader_[c] = kNullShader;
		if (kCaseTable[c].declaration != nullptr) {
			shader_[c] = gfx_.Create_Vertex_Shader(kCaseTable[c].declaration,
			                                       kCaseTable[c].program, 0);
			if (shader_[c] == kNullShader) {
				std::fprintf(stderr, "untyped-vb: the backend refused case %u's declaration\n", c);
				return false;
			}
		}
	}

	// One 1x1 texture per tile of the textured cases, carrying that tile's id.
	for (uint32_t t = 0; t < kTiles; ++t) {
		const uint32_t argb = Argb_For_Tile(t) | 0xFF000000u;
		const uint8_t pixel[4] = {static_cast<uint8_t>(argb & 0xFFu),
		                          static_cast<uint8_t>((argb >> 8) & 0xFFu),
		                          static_cast<uint8_t>((argb >> 16) & 0xFFu),
		                          static_cast<uint8_t>((argb >> 24) & 0xFFu)};
		TextureHandle* texture = gfx_.Create_Texture(1, 1, pixel);
		if (texture == nullptr) {
			std::fprintf(stderr, "untyped-vb: texture %u creation failed\n", t);
			return false;
		}
		textures_.push_back(texture);
	}

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
	gfx_.Set_Transform(D3DTS_WORLD, Matrix4x4::Identity());
	gfx_.Set_Transform(D3DTS_VIEW, Matrix4x4::Identity());
	gfx_.Set_Transform(D3DTS_PROJECTION, Matrix4x4::Identity());
	return true;
}

void Workload::Fill_Case(uint32_t c, bool y_down, uint8_t* bytes) const {
	const Case& cs = kCaseTable[c];
	for (uint32_t i = 0; i < kTilesPerCase; ++i) {
		const uint32_t t = c * kTilesPerCase + i;
		float corners[4][3];
		Tile_Corners(t, y_down, corners);
		const uint32_t diffuse = Argb_For_Tile(t);
		const float uv[2] = {0.5f, 0.5f};
		const float normal[3] = {0.0f, 0.0f, 1.0f};
		for (uint32_t v = 0; v < 4; ++v) {
			uint8_t* dst = bytes + (static_cast<size_t>(i) * 4 + v) * cs.vertex_bytes;
			uint32_t offset = 0;
			for (uint32_t e = 0; e < kMaxElements && cs.elements[e] != E_END; ++e) {
				switch (cs.elements[e]) {
				case E_POS: std::memcpy(dst + offset, corners[v], sizeof(float) * 3); break;
				case E_NORMAL: std::memcpy(dst + offset, normal, sizeof(normal)); break;
				case E_COLOR: std::memcpy(dst + offset, &diffuse, sizeof(diffuse)); break;
				case E_UV: std::memcpy(dst + offset, uv, sizeof(uv)); break;
				// Bytes SetStreamSource's stride skips over: a layout that read them as
				// anything would miscolour the tile.
				case E_PAD8: std::memset(dst + offset, 0xEE, 8); break;
				default: break;
				}
				offset += Element_Bytes(cs.elements[e]);
			}
		}
	}
}

bool Workload::Render_Frame(bool y_down) {
	// One DISCARD lock per frame, then draws: the shadow decal path's shape.
	void* bits = nullptr;
	if (!gfx_.Lock_Vertex_Buffer(vb_, 0, vb_bytes_, LOCK_DISCARD, &bits) || bits == nullptr) {
		std::fprintf(stderr, "untyped-vb: lock failed\n");
		return false;
	}
	for (uint32_t c = 0; c < kCases; ++c) {
		Fill_Case(c, y_down, static_cast<uint8_t*>(bits) + Case_Offset(c));
	}
	gfx_.Unlock_Vertex_Buffer(vb_);

	gfx_.Begin_Scene();
	gfx_.Clear(true, true, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0);
	for (uint32_t c = 0; c < kCases; ++c) {
		const Case& cs = kCaseTable[c];
		// Where the id is read from, per case: the one attribute this layout supplies that
		// the previous one did not. Under a program, oD0 and oT0 feed the same cascade.
		const bool textured = Case_Has(cs, E_UV);
		const bool coloured = Case_Has(cs, E_COLOR);
		const uint32_t rgb_arg = textured ? D3DTA_TEXTURE : (coloured ? D3DTA_DIFFUSE
		                                                              : D3DTA_TFACTOR);
		const uint32_t alpha_arg = coloured ? D3DTA_DIFFUSE : D3DTA_TFACTOR;
		gfx_.Set_DX8_Texture_Stage_State(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
		gfx_.Set_DX8_Texture_Stage_State(0, D3DTSS_COLORARG1, rgb_arg);
		gfx_.Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
		gfx_.Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAARG1, alpha_arg);

		// The engine's order: SetVertexShader(fvf or handle), SetStreamSource(vb, stride),
		// draw. The bridge's SetVertexShader clears the fixed-function FVF when it binds a
		// program and vice versa; both are done here so a case cannot lean on the other's.
		gfx_.Set_Fixed_Function_Fvf(cs.fvf);
		gfx_.Set_Vertex_Shader(shader_[c]);
		gfx_.Set_Vertex_Buffer(vb_, 0, cs.stride);
		gfx_.Set_Index_Buffer(ib_[c], 0);
		// The refused case draws one tile: one dropped draw is the assertion, and a row
		// of them would only repeat it.
		const uint32_t draws = c == kRefusedCase ? 1 : kTilesPerCase;
		for (uint32_t i = 0; i < draws; ++i) {
			const uint32_t t = c * kTilesPerCase + i;
			gfx_.Set_Texture(0, textures_[t]);
			gfx_.Set_DX8_Render_State(D3DRS_TEXTUREFACTOR, Argb_For_Tile(t));
			gfx_.Draw_Triangles(i * 6, 2, Case_Base_Vertex(c) + i * 4, 4);
		}
	}
	gfx_.End_Scene(false);
	return gfx_.Read_Back_Color_Target(pixels_, format_);
}

Rgba Workload::Tile_Pixel(uint32_t col, uint32_t row) const {
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
	uint32_t dropped = 0;
	uint32_t aliased = 0;
	uint32_t alpha_wrong = 0;
	uint32_t mismatched = 0;
};

Verdict Classify_Case(const Workload& w, uint32_t c) {
	Verdict v;
	for (uint32_t i = 0; i < kTilesPerCase; ++i) {
		const uint32_t t = c * kTilesPerCase + i;
		const Rgba p = w.Tile_Pixel(Tile_Col(t), Tile_Row(t));
		const int64_t id = Decode_Tile_Id(p);
		if (id < 0) {
			if (p.r == 0 && p.g == 0 && p.b == 0) ++v.dropped;
			else ++v.mismatched;
			continue;
		}
		if (static_cast<uint32_t>(id) != t) {
			++v.aliased;
			continue;
		}
		if (p.a != Alpha_For_Tile(t)) {
			++v.alpha_wrong;
			continue;
		}
		++v.correct;
	}
	return v;
}

} // namespace

int main(int argc, char** argv) {
	bool validation = false;
	uint32_t frames = 1;
	for (int i = 1; i < argc; ++i) {
		if (std::strcmp(argv[i], "--validation") == 0) validation = true;
		else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
			frames = static_cast<uint32_t>(std::atoi(argv[++i]));
		else if (std::strcmp(argv[i], "--help") == 0) {
			std::printf("usage: %s [--frames N] [--validation]\n", argv[0]);
			return 0;
		}
	}
	if (frames == 0) frames = 1;

	RenderBackend* gfx = Create_Vulkan_Backend(validation, true);
	if (!gfx->Init(nullptr, kWidth, kHeight)) {
		std::fprintf(stderr, "untyped-vb: backend Init failed\n");
		return 1;
	}
	std::printf("device: %s\n", gfx->Device_Description());

	Workload w(*gfx);
	if (!w.Init()) {
		std::printf("untyped-vb: FAIL\n");
		return 1;
	}

	// Orientation, measured: case 0's first tile has to land in tile (0,0).
	bool y_down = true;
	bool oriented = false;
	for (int attempt = 0; attempt < 2 && !oriented; ++attempt) {
		y_down = attempt == 0;
		if (!w.Render_Frame(y_down)) {
			std::fprintf(stderr, "untyped-vb: readback failed\n");
			return 1;
		}
		oriented = Decode_Tile_Id(w.Tile_Pixel(0, 0)) == 0;
	}
	if (!oriented) {
		std::fprintf(stderr, "untyped-vb: orientation probe failed - case 0's first draw did "
		                     "not land in tile (0,0) either way up\n");
		std::printf("untyped-vb: FAIL\n");
		return 1;
	}
	std::printf("orientation: clip y %s, tile 0 verified\n", y_down ? "down" : "up");

	int status = 0;
	for (uint32_t frame = 0; frame < frames; ++frame) {
		if (!w.Render_Frame(y_down)) {
			std::fprintf(stderr, "untyped-vb: readback failed\n");
			return 1;
		}
		std::printf("frame %u\n", frame);
		for (uint32_t c = 0; c < kCases; ++c) {
			const Verdict v = Classify_Case(w, c);
			std::printf("case %u %s\n", c, kCaseTable[c].name);
			std::printf("  tiles correct           %u\n", v.correct);
			std::printf("  tiles dropped           %u\n", v.dropped);
			std::printf("  tiles aliased           %u\n", v.aliased);
			std::printf("  tiles alpha-wrong       %u\n", v.alpha_wrong);
			std::printf("  tiles mismatched        %u\n", v.mismatched);
			if (c == kRefusedCase) {
				// Nothing may have been drawn: the whole row is the clear colour.
				if (v.dropped != kTilesPerCase) status = 1;
			} else if (v.correct != kTilesPerCase) {
				status = 1;
			}
		}
		DrawStats stats{};
		gfx->Get_Draw_Stats(stats);
		const uint32_t expected_issued = kDrawnCases * kTilesPerCase;
		std::printf("backend draws requested   %u\n", stats.draws_requested);
		std::printf("backend draws issued      %u\n", stats.draws_issued);
		std::printf("backend draws dropped     %u\n", stats.draws_dropped);
		std::printf("backend untyped issued    %u\n", stats.untyped_draws_issued);
		std::printf("backend untyped dropped   %u\n", stats.untyped_draws_dropped);
		if (stats.draws_requested != expected_issued + 1) status = 1;
		if (stats.draws_issued != expected_issued) status = 1;
		if (stats.untyped_draws_issued != expected_issued) status = 1;
		// The refused draw has to be the one that was dropped, and it has to be named.
		if (stats.draws_dropped != 1 || stats.untyped_draws_dropped != 1) status = 1;
	}

	std::printf("validation layer: %s\n", gfx->Validation_Active() ? "loaded" : "not requested");
	std::printf("validation messages: %u\n", gfx->Validation_Message_Count());
	if (validation && !gfx->Validation_Active()) status = 1;
	if (gfx->Validation_Message_Count() != 0) status = 1;

	gfx->Shutdown();
	delete gfx;
	std::printf("%s\n", status == 0 ? "untyped-vb: PASS" : "untyped-vb: FAIL");
	return status;
}
