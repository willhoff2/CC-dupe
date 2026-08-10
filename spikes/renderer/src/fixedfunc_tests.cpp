// Renderer spike: pixel assertions for the fixed-function feature set.
//
// `feature_probe.cpp` drives Vulkan directly to answer "can the device do X?".
// This drives the *backend*, through the DX8Wrapper-shaped RenderBackend interface,
// to answer "does the backend produce what D3D8 would have produced?". Every case
// issues D3D8 calls, renders, reads the colour target back and asserts on actual
// pixels.
//
// Expected values are computed from the D3D8 specification (the "Texture Blending"
// and "Mathematics of Lighting" tables), written out here independently of the
// shader that has to reproduce them, and compared with a 1/255 tolerance for the
// UNORM8 round trip. Where a case has no expected value that can be established
// from the specification, it is reported PENDING and asserts nothing -- see the
// bump-environment cases.
//
// The feature set is the measured one: tools/texture-stage-scan.py and
// tools/engine-usage-scan.py, tabulated in
// docs/porting/fixed-function-measurements.md.

#include "render_backend.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace spike;

namespace {

constexpr uint32_t kWidth = 64;
constexpr uint32_t kHeight = 64;

constexpr uint32_t Argb(uint32_t a, uint32_t r, uint32_t g, uint32_t b) {
	return (a << 24) | (r << 16) | (g << 8) | b;
}

struct Rgba {
	int r = 0, g = 0, b = 0, a = 0;
};

std::string To_String(const Rgba& c) {
	char buffer[64];
	std::snprintf(buffer, sizeof(buffer), "(%d,%d,%d,%d)", c.r, c.g, c.b, c.a);
	return buffer;
}

// D3D8 quantises to 8 bits at every stage boundary on the hardware of the era, but
// the reference rasteriser and every modern implementation keep more precision, so
// only the final UNORM8 write is quantised. One LSB of slack covers the rounding.
constexpr int kTolerance = 1;

bool Near(const Rgba& actual, const Rgba& expected, int tolerance = kTolerance) {
	return std::abs(actual.r - expected.r) <= tolerance &&
	       std::abs(actual.g - expected.g) <= tolerance &&
	       std::abs(actual.b - expected.b) <= tolerance &&
	       std::abs(actual.a - expected.a) <= tolerance;
}

int Quantise(float value) {
	if (value < 0.0f) value = 0.0f;
	if (value > 1.0f) value = 1.0f;
	return static_cast<int>(value * 255.0f + 0.5f);
}

Rgba Quantise(float r, float g, float b, float a) {
	return Rgba{Quantise(r), Quantise(g), Quantise(b), Quantise(a)};
}

// --- geometry ---------------------------------------------------------------

// D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1: the pretransformed quad most cases
// draw, so that nothing depends on the transform pipeline except the cases that are
// about the transform pipeline.
struct ScreenVertex {
	float x, y, z, rhw;
	uint32_t diffuse;
	float u, v;
};

// D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1
struct WorldVertex {
	float x, y, z;
	float nx, ny, nz;
	uint32_t diffuse;
	float u, v;
};

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

// ---------------------------------------------------------------------------
// the harness
// ---------------------------------------------------------------------------

class Harness {
public:
	explicit Harness(RenderBackend* backend) : gfx_(backend) {}

	bool Init();
	void Shutdown();

	RenderBackend& Gfx() { return *gfx_; }

	// D3D8's device defaults for everything a case might have changed, so cases do
	// not leak state into each other the way the engine's own state cache would.
	void Reset_State();

	// Draws the full-target pretransformed quad with the given per-vertex colour
	// and texture coordinates, and returns the centre pixel.
	Rgba Draw_Screen_Quad(uint32_t diffuse, float u = 0.5f, float v = 0.5f);
	// Same, but the caller supplies the vertices (for the transform/lighting cases).
	Rgba Draw(VertexBufferHandle* vb, IndexBufferHandle* ib, uint32_t polygon_count);

	Rgba Pixel(uint32_t x, uint32_t y) const;
	bool Read_Back();

	void Begin();
	void End();

	TextureHandle* Solid_Texture(uint32_t argb);

private:
	RenderBackend* gfx_ = nullptr;
	std::string pixels_;
	SurfaceFormat format_{};
	VertexBufferHandle* screen_quad_vb_ = nullptr;
	IndexBufferHandle* quad_ib_ = nullptr;
	std::vector<ScreenVertex> screen_quad_;
};

bool Harness::Init() {
	if (!gfx_->Init(nullptr, kWidth, kHeight)) return false;
	const uint16_t indices[6] = {0, 1, 2, 0, 2, 3};
	quad_ib_ = gfx_->Create_Index_Buffer(indices, 6);
	screen_quad_.resize(4);
	return quad_ib_ != nullptr;
}

void Harness::Shutdown() { gfx_->Shutdown(); }

void Harness::Reset_State() {
	RenderBackend& g = *gfx_;
	g.Set_DX8_Render_State(D3DRS_ZENABLE, 0);
	g.Set_DX8_Render_State(D3DRS_ZWRITEENABLE, 1);
	g.Set_DX8_Render_State(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
	g.Set_DX8_Render_State(D3DRS_ZBIAS, 0);
	g.Set_DX8_Render_State(D3DRS_CULLMODE, D3DCULL_NONE);
	g.Set_DX8_Render_State(D3DRS_ALPHABLENDENABLE, 0);
	g.Set_DX8_Render_State(D3DRS_ALPHATESTENABLE, 0);
	g.Set_DX8_Render_State(D3DRS_ALPHAFUNC, D3DCMP_ALWAYS);
	g.Set_DX8_Render_State(D3DRS_ALPHAREF, 0);
	g.Set_DX8_Render_State(D3DRS_STENCILENABLE, 0);
	g.Set_DX8_Render_State(D3DRS_STENCILFUNC, D3DCMP_ALWAYS);
	g.Set_DX8_Render_State(D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP);
	g.Set_DX8_Render_State(D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
	g.Set_DX8_Render_State(D3DRS_STENCILPASS, D3DSTENCILOP_KEEP);
	g.Set_DX8_Render_State(D3DRS_STENCILREF, 0);
	g.Set_DX8_Render_State(D3DRS_STENCILMASK, 0xffffffff);
	g.Set_DX8_Render_State(D3DRS_STENCILWRITEMASK, 0xffffffff);
	g.Set_DX8_Render_State(D3DRS_LIGHTING, 0);
	g.Set_DX8_Render_State(D3DRS_SPECULARENABLE, 0);
	g.Set_DX8_Render_State(D3DRS_COLORVERTEX, 1);
	g.Set_DX8_Render_State(D3DRS_NORMALIZENORMALS, 0);
	g.Set_DX8_Render_State(D3DRS_LOCALVIEWER, 1);
	g.Set_DX8_Render_State(D3DRS_AMBIENT, 0);
	g.Set_DX8_Render_State(D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_COLOR1);
	g.Set_DX8_Render_State(D3DRS_SPECULARMATERIALSOURCE, D3DMCS_COLOR2);
	g.Set_DX8_Render_State(D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_MATERIAL);
	g.Set_DX8_Render_State(D3DRS_EMISSIVEMATERIALSOURCE, D3DMCS_MATERIAL);
	g.Set_DX8_Render_State(D3DRS_FOGENABLE, 0);
	g.Set_DX8_Render_State(D3DRS_FOGVERTEXMODE, D3DFOG_NONE);
	g.Set_DX8_Render_State(D3DRS_FOGTABLEMODE, D3DFOG_NONE);
	g.Set_DX8_Render_State(D3DRS_RANGEFOGENABLE, 0);
	g.Set_DX8_Render_State(D3DRS_TEXTUREFACTOR, 0xffffffff);
	g.Set_DX8_Render_State(D3DRS_COLORWRITEENABLE, 0xf);
	g.Set_Transform(D3DTS_WORLD, Matrix4x4::Identity());
	g.Set_Transform(D3DTS_VIEW, Matrix4x4::Identity());
	g.Set_Transform(D3DTS_PROJECTION, Matrix4x4::Identity());
	for (uint32_t stage = 0; stage < 4; ++stage)
		g.Set_Transform(static_cast<D3DTRANSFORMSTATETYPE>(D3DTS_TEXTURE0 + stage),
		                Matrix4x4::Identity());
	for (uint32_t stage = 0; stage < 8; ++stage) {
		g.Set_DX8_Texture_Stage_State(stage, D3DTSS_COLOROP, D3DTOP_DISABLE);
		g.Set_DX8_Texture_Stage_State(stage, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
		g.Set_DX8_Texture_Stage_State(stage, D3DTSS_COLORARG1, D3DTA_TEXTURE);
		g.Set_DX8_Texture_Stage_State(stage, D3DTSS_COLORARG2, D3DTA_CURRENT);
		g.Set_DX8_Texture_Stage_State(stage, D3DTSS_COLORARG0, D3DTA_CURRENT);
		g.Set_DX8_Texture_Stage_State(stage, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
		g.Set_DX8_Texture_Stage_State(stage, D3DTSS_ALPHAARG2, D3DTA_CURRENT);
		g.Set_DX8_Texture_Stage_State(stage, D3DTSS_ALPHAARG0, D3DTA_CURRENT);
		g.Set_DX8_Texture_Stage_State(stage, D3DTSS_TEXCOORDINDEX, stage);
		g.Set_DX8_Texture_Stage_State(stage, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
		g.Set_DX8_Texture_Stage_State(stage, D3DTSS_RESULTARG, D3DTA_CURRENT);
		g.Set_DX8_Texture_Stage_State(stage, D3DTSS_MINFILTER, D3DTEXF_POINT);
		g.Set_DX8_Texture_Stage_State(stage, D3DTSS_MAGFILTER, D3DTEXF_POINT);
		g.Set_DX8_Texture_Stage_State(stage, D3DTSS_MIPFILTER, D3DTEXF_NONE);
		g.Set_DX8_Texture_Stage_State(stage, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
		g.Set_DX8_Texture_Stage_State(stage, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
		g.Set_Texture(stage, nullptr);
	}
	g.Set_Light(0, nullptr);
	g.Set_Light(1, nullptr);
	g.Set_Light(2, nullptr);
	g.Set_Light(3, nullptr);
	g.Set_Material(MaterialState{});
	g.Set_Scissor(false, 0, 0, 0, 0);
}

void Harness::Begin() {
	gfx_->Begin_Scene();
	gfx_->Clear(true, true, 0.0f, 0.0f, 0.0f, 0.0f);
}

void Harness::End() { gfx_->End_Scene(false); }

bool Harness::Read_Back() { return gfx_->Read_Back_Color_Target(pixels_, format_); }

Rgba Harness::Pixel(uint32_t x, uint32_t y) const {
	const size_t offset = (static_cast<size_t>(y) * format_.width + x) * 4;
	if (offset + 3 >= pixels_.size()) return Rgba{};
	const auto* p = reinterpret_cast<const unsigned char*>(pixels_.data()) + offset;
	return Rgba{p[0], p[1], p[2], p[3]};
}

Rgba Harness::Draw_Screen_Quad(uint32_t diffuse, float u, float v) {
	const float w = static_cast<float>(kWidth);
	const float h = static_cast<float>(kHeight);
	screen_quad_[0] = {0.0f, 0.0f, 0.5f, 1.0f, diffuse, u, v};
	screen_quad_[1] = {w, 0.0f, 0.5f, 1.0f, diffuse, u, v};
	screen_quad_[2] = {w, h, 0.5f, 1.0f, diffuse, u, v};
	screen_quad_[3] = {0.0f, h, 0.5f, 1.0f, diffuse, u, v};
	screen_quad_vb_ = gfx_->Create_Vertex_Buffer(
	    screen_quad_.data(), screen_quad_.size() * sizeof(ScreenVertex),
	    D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
	if (screen_quad_vb_ == nullptr) return Rgba{};
	return Draw(screen_quad_vb_, quad_ib_, 2);
}

Rgba Harness::Draw(VertexBufferHandle* vb, IndexBufferHandle* ib, uint32_t polygon_count) {
	gfx_->Set_Vertex_Buffer(vb, 0);
	gfx_->Set_Index_Buffer(ib, 0);
	gfx_->Draw_Triangles(0, polygon_count, 0, polygon_count * 3);
	return Rgba{};
}

TextureHandle* Harness::Solid_Texture(uint32_t argb) {
	const uint32_t texels[4] = {argb, argb, argb, argb};
	return gfx_->Create_Texture(2, 2, reinterpret_cast<const uint8_t*>(texels));
}

// ---------------------------------------------------------------------------
// D3D8 reference maths, from the specification, for the expected values
// ---------------------------------------------------------------------------

struct Color {
	float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
};

Color From_Argb(uint32_t argb) {
	return Color{((argb >> 16) & 0xff) / 255.0f, ((argb >> 8) & 0xff) / 255.0f,
	             (argb & 0xff) / 255.0f, ((argb >> 24) & 0xff) / 255.0f};
}

float Saturate(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// D3D8's argument modifiers, applied before the operation.
Color Apply_Modifiers(Color c, uint32_t arg) {
	if (arg & D3DTA_ALPHAREPLICATE) c = Color{c.a, c.a, c.a, c.a};
	if (arg & D3DTA_COMPLEMENT) c = Color{1.0f - c.r, 1.0f - c.g, 1.0f - c.b, 1.0f - c.a};
	return c;
}

// The colour half of D3D8's texture blending table. Reproduced from the D3D8
// documentation, not from the shader, so that agreeing with the shader means
// something.
Color Blend(uint32_t op, Color a1, Color a2, Color a0, float texture_alpha,
            float current_alpha, float diffuse_alpha, float factor_alpha) {
	Color out;
	switch (op) {
	case D3DTOP_SELECTARG1: out = a1; break;
	case D3DTOP_SELECTARG2: out = a2; break;
	case D3DTOP_MODULATE:
		out = Color{a1.r * a2.r, a1.g * a2.g, a1.b * a2.b, a1.a * a2.a};
		break;
	case D3DTOP_MODULATE2X:
		out = Color{Saturate(a1.r * a2.r * 2.0f), Saturate(a1.g * a2.g * 2.0f),
		            Saturate(a1.b * a2.b * 2.0f), Saturate(a1.a * a2.a * 2.0f)};
		break;
	case D3DTOP_MODULATE4X:
		out = Color{Saturate(a1.r * a2.r * 4.0f), Saturate(a1.g * a2.g * 4.0f),
		            Saturate(a1.b * a2.b * 4.0f), Saturate(a1.a * a2.a * 4.0f)};
		break;
	case D3DTOP_ADD:
		out = Color{Saturate(a1.r + a2.r), Saturate(a1.g + a2.g), Saturate(a1.b + a2.b),
		            Saturate(a1.a + a2.a)};
		break;
	case D3DTOP_ADDSIGNED:
		out = Color{Saturate(a1.r + a2.r - 0.5f), Saturate(a1.g + a2.g - 0.5f),
		            Saturate(a1.b + a2.b - 0.5f), Saturate(a1.a + a2.a - 0.5f)};
		break;
	case D3DTOP_ADDSIGNED2X:
		out = Color{Saturate((a1.r + a2.r - 0.5f) * 2.0f),
		            Saturate((a1.g + a2.g - 0.5f) * 2.0f),
		            Saturate((a1.b + a2.b - 0.5f) * 2.0f),
		            Saturate((a1.a + a2.a - 0.5f) * 2.0f)};
		break;
	case D3DTOP_SUBTRACT:
		out = Color{Saturate(a1.r - a2.r), Saturate(a1.g - a2.g), Saturate(a1.b - a2.b),
		            Saturate(a1.a - a2.a)};
		break;
	case D3DTOP_ADDSMOOTH:
		out = Color{Saturate(a1.r + a2.r - a1.r * a2.r), Saturate(a1.g + a2.g - a1.g * a2.g),
		            Saturate(a1.b + a2.b - a1.b * a2.b), Saturate(a1.a + a2.a - a1.a * a2.a)};
		break;
	case D3DTOP_BLENDDIFFUSEALPHA:
	case D3DTOP_BLENDTEXTUREALPHA:
	case D3DTOP_BLENDFACTORALPHA:
	case D3DTOP_BLENDCURRENTALPHA: {
		const float t = op == D3DTOP_BLENDDIFFUSEALPHA   ? diffuse_alpha
		                : op == D3DTOP_BLENDTEXTUREALPHA ? texture_alpha
		                : op == D3DTOP_BLENDFACTORALPHA  ? factor_alpha
		                                                 : current_alpha;
		out = Color{a1.r * t + a2.r * (1.0f - t), a1.g * t + a2.g * (1.0f - t),
		            a1.b * t + a2.b * (1.0f - t), a1.a * t + a2.a * (1.0f - t)};
		break;
	}
	case D3DTOP_MODULATEALPHA_ADDCOLOR:
		out = Color{Saturate(a1.r + a1.a * a2.r), Saturate(a1.g + a1.a * a2.g),
		            Saturate(a1.b + a1.a * a2.b), a1.a};
		break;
	case D3DTOP_MODULATECOLOR_ADDALPHA:
		out = Color{Saturate(a1.r * a2.r + a1.a), Saturate(a1.g * a2.g + a1.a),
		            Saturate(a1.b * a2.b + a1.a), a1.a};
		break;
	case D3DTOP_DOTPRODUCT3: {
		const float d = Saturate((a1.r * 2.0f - 1.0f) * (a2.r * 2.0f - 1.0f) +
		                         (a1.g * 2.0f - 1.0f) * (a2.g * 2.0f - 1.0f) +
		                         (a1.b * 2.0f - 1.0f) * (a2.b * 2.0f - 1.0f));
		out = Color{d, d, d, d};
		break;
	}
	case D3DTOP_MULTIPLYADD:
		out = Color{Saturate(a0.r + a1.r * a2.r), Saturate(a0.g + a1.g * a2.g),
		            Saturate(a0.b + a1.b * a2.b), Saturate(a0.a + a1.a * a2.a)};
		break;
	case D3DTOP_LERP:
		out = Color{a0.r * a1.r + (1.0f - a0.r) * a2.r, a0.g * a1.g + (1.0f - a0.g) * a2.g,
		            a0.b * a1.b + (1.0f - a0.b) * a2.b, a0.a * a1.a + (1.0f - a0.a) * a2.a};
		break;
	default: out = a1; break;
	}
	return out;
}

// ---------------------------------------------------------------------------
// cases
// ---------------------------------------------------------------------------

struct Outcome {
	enum Status { kPass, kFail, kPending } status = kPass;
	std::string detail;
};

Outcome Pass(const std::string& detail) { return Outcome{Outcome::kPass, detail}; }
Outcome Fail(const std::string& detail) { return Outcome{Outcome::kFail, detail}; }
Outcome Pending(const std::string& detail) { return Outcome{Outcome::kPending, detail}; }

Outcome Check(const Rgba& actual, const Rgba& expected, const std::string& what) {
	const std::string detail = what + " got=" + To_String(actual) +
	                           " expected=" + To_String(expected);
	return Near(actual, expected) ? Pass(detail) : Fail(detail);
}

// One measured (op, arg1, arg2, arg0) tuple, evaluated in a single stage against a
// texture, a vertex colour and the texture factor.
struct CascadeCase {
	const char* name;
	uint32_t color_op;
	uint32_t color_arg1;
	uint32_t color_arg2;
	uint32_t color_arg0;
	// Most cases share the inputs below; DOTPRODUCT3 needs its own, because the
	// shared ones happen to dot to a negative number and clamp to black, which
	// would pass against almost any implementation.
	uint32_t tex_argb = 0;
	uint32_t factor_argb = 0;
};

// The inputs every cascade case uses unless it overrides them. Deliberately
// asymmetric per channel so that a swapped argument or channel shows up.
constexpr uint32_t kTexArgb = Argb(0x40, 0xc0, 0x80, 0x20);
constexpr uint32_t kDiffuseArgb = Argb(0xc0, 0x40, 0x60, 0xe0);
constexpr uint32_t kFactorArgb = Argb(0x80, 0x20, 0xa0, 0x60);

// The ops the two scans found the engine can request, one case each. The tuples are
// the measured ones where the measurement pins them down; where the same op is used
// with several argument tuples, the arguments here are chosen to make every input
// distinguishable in the result rather than to replay one specific call site.
const CascadeCase kCascadeCases[] = {
    {"SELECTARG1(TEXTURE)", D3DTOP_SELECTARG1, D3DTA_TEXTURE, D3DTA_DIFFUSE, D3DTA_TFACTOR},
    {"SELECTARG2(DIFFUSE)", D3DTOP_SELECTARG2, D3DTA_TEXTURE, D3DTA_DIFFUSE, D3DTA_TFACTOR},
    {"MODULATE(TEXTURE,DIFFUSE)", D3DTOP_MODULATE, D3DTA_TEXTURE, D3DTA_DIFFUSE, D3DTA_TFACTOR},
    {"MODULATE2X(TEXTURE,DIFFUSE)", D3DTOP_MODULATE2X, D3DTA_TEXTURE, D3DTA_DIFFUSE, D3DTA_TFACTOR},
    {"ADD(TEXTURE,DIFFUSE)", D3DTOP_ADD, D3DTA_TEXTURE, D3DTA_DIFFUSE, D3DTA_TFACTOR},
    {"ADDSIGNED(TEXTURE,DIFFUSE)", D3DTOP_ADDSIGNED, D3DTA_TEXTURE, D3DTA_DIFFUSE, D3DTA_TFACTOR},
    {"ADDSIGNED2X(TEXTURE,DIFFUSE)", D3DTOP_ADDSIGNED2X, D3DTA_TEXTURE, D3DTA_DIFFUSE, D3DTA_TFACTOR},
    {"SUBTRACT(TEXTURE,DIFFUSE)", D3DTOP_SUBTRACT, D3DTA_TEXTURE, D3DTA_DIFFUSE, D3DTA_TFACTOR},
    {"ADDSMOOTH(TEXTURE,DIFFUSE)", D3DTOP_ADDSMOOTH, D3DTA_TEXTURE, D3DTA_DIFFUSE, D3DTA_TFACTOR},
    {"BLENDTEXTUREALPHA(TEXTURE,DIFFUSE)", D3DTOP_BLENDTEXTUREALPHA, D3DTA_TEXTURE,
     D3DTA_DIFFUSE, D3DTA_TFACTOR},
    {"BLENDCURRENTALPHA(TEXTURE,DIFFUSE)", D3DTOP_BLENDCURRENTALPHA, D3DTA_TEXTURE,
     D3DTA_DIFFUSE, D3DTA_TFACTOR},
    {"MODULATEALPHA_ADDCOLOR(TEXTURE,DIFFUSE)", D3DTOP_MODULATEALPHA_ADDCOLOR, D3DTA_TEXTURE,
     D3DTA_DIFFUSE, D3DTA_TFACTOR},
    {"DOTPRODUCT3(TEXTURE,TFACTOR)", D3DTOP_DOTPRODUCT3, D3DTA_TEXTURE, D3DTA_TFACTOR,
     D3DTA_DIFFUSE, Argb(0xff, 0xbf, 0x80, 0x80), Argb(0xff, 0xff, 0x80, 0x80)},
    {"MULTIPLYADD(TFACTOR|ALPHAREPLICATE,TEXTURE,TFACTOR|ALPHAREPLICATE)", D3DTOP_MULTIPLYADD,
     D3DTA_TFACTOR | D3DTA_ALPHAREPLICATE, D3DTA_TEXTURE,
     D3DTA_TFACTOR | D3DTA_ALPHAREPLICATE},
    {"ADD(DIFFUSE|COMPLEMENT|ALPHAREPLICATE,DIFFUSE)", D3DTOP_ADD,
     D3DTA_DIFFUSE | D3DTA_COMPLEMENT | D3DTA_ALPHAREPLICATE, D3DTA_DIFFUSE, D3DTA_TFACTOR},
    {"LERP(TEXTURE,DIFFUSE,TFACTOR)", D3DTOP_LERP, D3DTA_TEXTURE, D3DTA_DIFFUSE, D3DTA_TFACTOR},
};

Outcome Case_Cascade_Op(Harness& h, const CascadeCase& c) {
	RenderBackend& g = h.Gfx();
	h.Reset_State();
	const uint32_t tex_argb = c.tex_argb != 0 ? c.tex_argb : kTexArgb;
	const uint32_t factor_argb = c.factor_argb != 0 ? c.factor_argb : kFactorArgb;
	TextureHandle* texture = h.Solid_Texture(tex_argb);
	if (texture == nullptr) return Fail("Create_Texture failed");

	g.Set_DX8_Render_State(D3DRS_TEXTUREFACTOR, factor_argb);
	g.Set_Texture(0, texture);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_COLOROP, c.color_op);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_COLORARG1, c.color_arg1);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_COLORARG2, c.color_arg2);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_COLORARG0, c.color_arg0);
	// Alpha is SELECTARG1(DIFFUSE) throughout so the colour result is what is
	// being compared, not a combination of two independent results.
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);

	h.Begin();
	h.Draw_Screen_Quad(kDiffuseArgb);
	h.End();
	if (!h.Read_Back()) return Fail("readback failed");

	const Color tex = From_Argb(tex_argb);
	const Color diffuse = From_Argb(kDiffuseArgb);
	const Color factor = From_Argb(factor_argb);
	auto source = [&](uint32_t arg) {
		switch (arg & 0x0f) {
		case D3DTA_TEXTURE: return Apply_Modifiers(tex, arg);
		case D3DTA_TFACTOR: return Apply_Modifiers(factor, arg);
		case D3DTA_CURRENT: return Apply_Modifiers(diffuse, arg); // stage 0: CURRENT == diffuse
		default: return Apply_Modifiers(diffuse, arg);
		}
	};
	const Color result = Blend(c.color_op, source(c.color_arg1), source(c.color_arg2),
	                           source(c.color_arg0), tex.a, diffuse.a, diffuse.a, factor.a);
	// DOTPRODUCT3 replicates its scalar result into alpha as well, overriding the
	// alpha pipeline; every other op leaves the alpha stage in charge.
	const float alpha = c.color_op == D3DTOP_DOTPRODUCT3 ? result.r : diffuse.a;
	return Check(h.Pixel(kWidth / 2, kHeight / 2),
	             Quantise(result.r, result.g, result.b, alpha), c.name);
}

// The engine writes literal stage state up to stage 7, so the cascade has to be
// eight stages deep. Eight MODULATE stages against known texel values is a result
// no shorter cascade can produce.
Outcome Case_Eight_Stages(Harness& h) {
	RenderBackend& g = h.Gfx();
	h.Reset_State();
	// 8 textures whose product is exactly representable: 0.5^7 * 1.0 in the red
	// channel is 2^-7, and the readback is UNORM8, so the expected value is exact.
	TextureHandle* textures[8];
	for (uint32_t i = 0; i < 8; ++i) {
		const uint32_t value = i == 0 ? 0xff : 0x80;
		textures[i] = h.Solid_Texture(Argb(0xff, value, value, value));
		if (textures[i] == nullptr) return Fail("Create_Texture failed");
		g.Set_Texture(i, textures[i]);
		g.Set_DX8_Texture_Stage_State(i, D3DTSS_COLOROP,
		                              i == 0 ? D3DTOP_SELECTARG1 : D3DTOP_MODULATE);
		g.Set_DX8_Texture_Stage_State(i, D3DTSS_COLORARG1, D3DTA_TEXTURE);
		g.Set_DX8_Texture_Stage_State(i, D3DTSS_COLORARG2, D3DTA_CURRENT);
		g.Set_DX8_Texture_Stage_State(i, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
		g.Set_DX8_Texture_Stage_State(i, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
		g.Set_DX8_Texture_Stage_State(i, D3DTSS_TEXCOORDINDEX, 0);
	}

	h.Begin();
	h.Draw_Screen_Quad(Argb(0xff, 0xff, 0xff, 0xff));
	h.End();
	if (!h.Read_Back()) return Fail("readback failed");

	// 1.0 * (128/255)^7 == 0.008... -> 2 in UNORM8. A cascade that stopped at two
	// stages would read 128; at four, 16.
	float value = 1.0f;
	for (int i = 1; i < 8; ++i) value *= 128.0f / 255.0f;
	return Check(h.Pixel(kWidth / 2, kHeight / 2),
	             Quantise(value, value, value, 1.0f), "8-stage MODULATE chain");
}

// D3DTSS_RESULTARG=D3DTA_TEMP: a stage writes to the temporary register instead of
// CURRENT, and a later stage reads it back. The engine's only call site is commented
// out, so this proves the cascade is faithful rather than that the game needs it.
Outcome Case_Result_Temp(Harness& h) {
	RenderBackend& g = h.Gfx();
	h.Reset_State();
	TextureHandle* red = h.Solid_Texture(Argb(0xff, 0xff, 0x00, 0x00));
	TextureHandle* green = h.Solid_Texture(Argb(0xff, 0x00, 0xff, 0x00));
	if (red == nullptr || green == nullptr) return Fail("Create_Texture failed");

	g.Set_Texture(0, red);
	g.Set_Texture(1, green);
	// stage 0: TEMP = texture (red); CURRENT keeps the vertex colour (blue)
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_RESULTARG, D3DTA_TEMP);
	// stage 1: CURRENT = TEMP + texture = red + green = yellow
	g.Set_DX8_Texture_Stage_State(1, D3DTSS_COLOROP, D3DTOP_ADD);
	g.Set_DX8_Texture_Stage_State(1, D3DTSS_COLORARG1, D3DTA_TEMP);
	g.Set_DX8_Texture_Stage_State(1, D3DTSS_COLORARG2, D3DTA_TEXTURE);
	g.Set_DX8_Texture_Stage_State(1, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
	g.Set_DX8_Texture_Stage_State(1, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
	g.Set_DX8_Texture_Stage_State(1, D3DTSS_TEXCOORDINDEX, 0);

	h.Begin();
	h.Draw_Screen_Quad(Argb(0xff, 0x00, 0x00, 0xff));
	h.End();
	if (!h.Read_Back()) return Fail("readback failed");
	return Check(h.Pixel(kWidth / 2, kHeight / 2), Rgba{255, 255, 0, 255},
	             "RESULTARG=TEMP then read TEMP");
}

// D3DTOP_BUMPENVMAP perturbs the *next* stage's texture coordinates by the signed
// du,dv of this stage's texture through the 2x2 BUMPENVMAT. That much is
// unambiguous in the D3D8 documentation and is what is asserted: with a bump matrix
// that shifts a full texel, the next stage samples a different, known texel.
Outcome Case_Bump_Env_Map(Harness& h) {
	RenderBackend& g = h.Gfx();
	h.Reset_State();

	// V8U8: du = +0.5 (0x40 as SNORM8 == 64/127), dv = 0.
	const uint8_t bump_texels[2 * 2 * 2] = {0x40, 0x00, 0x40, 0x00, 0x40, 0x00, 0x40, 0x00};
	const TextureMip bump_mip{bump_texels, sizeof(bump_texels), 2, 2};
	TextureDesc bump_desc;
	bump_desc.format = TextureFormat::V8U8;
	bump_desc.mip_count = 1;
	bump_desc.mips = &bump_mip;
	if (!g.Supports_Texture_Format(TextureFormat::V8U8))
		return Pending("device cannot sample D3DFMT_V8U8 (R8G8_SNORM)");
	TextureHandle* bump = g.Create_Texture(bump_desc);

	// The environment map: left half red, right half green, so a positive du shifts
	// the sample from one to the other.
	const uint32_t env_texels[4] = {Argb(0xff, 0xff, 0, 0), Argb(0xff, 0, 0xff, 0),
	                                Argb(0xff, 0xff, 0, 0), Argb(0xff, 0, 0xff, 0)};
	TextureHandle* env = g.Create_Texture(2, 2, reinterpret_cast<const uint8_t*>(env_texels));
	if (bump == nullptr || env == nullptr) return Fail("Create_Texture failed");

	g.Set_Texture(0, bump);
	g.Set_Texture(1, env);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_COLOROP, D3DTOP_BUMPENVMAP);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_COLORARG2, D3DTA_CURRENT);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAARG2, D3DTA_CURRENT);
	// A du of 64/127 scaled by 0.75 moves the sample from u=0.25 to u=0.63, i.e.
	// from the left texel to the right one.
	const float matrix00 = 0.75f;
	uint32_t bits = 0;
	std::memcpy(&bits, &matrix00, sizeof(bits));
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_BUMPENVMAT00, bits);
	const float zero = 0.0f;
	uint32_t zero_bits = 0;
	std::memcpy(&zero_bits, &zero, sizeof(zero_bits));
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_BUMPENVMAT01, zero_bits);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_BUMPENVMAT10, zero_bits);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_BUMPENVMAT11, zero_bits);
	g.Set_DX8_Texture_Stage_State(1, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
	g.Set_DX8_Texture_Stage_State(1, D3DTSS_COLORARG1, D3DTA_TEXTURE);
	g.Set_DX8_Texture_Stage_State(1, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
	g.Set_DX8_Texture_Stage_State(1, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
	g.Set_DX8_Texture_Stage_State(1, D3DTSS_TEXCOORDINDEX, 0);

	h.Begin();
	h.Draw_Screen_Quad(Argb(0xff, 0xff, 0xff, 0xff), 0.25f, 0.25f);
	h.End();
	if (!h.Read_Back()) return Fail("readback failed");
	// Unperturbed, u=0.25 samples the red texel; perturbed by +0.375 it samples the
	// green one. Only the perturbation is asserted; what a BUMPENVMAP stage writes
	// to CURRENT is left to the next stage, which overwrites it here.
	return Check(h.Pixel(kWidth / 2, kHeight / 2), Rgba{0, 255, 0, 255},
	             "BUMPENVMAP shifts the next stage's sample");
}

// --- vertex pipeline --------------------------------------------------------

// A world/view/projection transform that is not the identity: the quad is a unit
// square at the origin, scaled and translated so its edges land on known pixels.
Outcome Case_Transform(Harness& h) {
	RenderBackend& g = h.Gfx();
	h.Reset_State();
	const WorldVertex quad[4] = {
	    {-1.0f, -1.0f, 0.5f, 0, 0, 1, Argb(0xff, 0xff, 0xff, 0xff), 0, 0},
	    {1.0f, -1.0f, 0.5f, 0, 0, 1, Argb(0xff, 0xff, 0xff, 0xff), 1, 0},
	    {1.0f, 1.0f, 0.5f, 0, 0, 1, Argb(0xff, 0xff, 0xff, 0xff), 1, 1},
	    {-1.0f, 1.0f, 0.5f, 0, 0, 1, Argb(0xff, 0xff, 0xff, 0xff), 0, 1},
	};
	const uint16_t indices[6] = {0, 1, 2, 0, 2, 3};
	VertexBufferHandle* vb = g.Create_Vertex_Buffer(
	    quad, sizeof(quad), D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1);
	IndexBufferHandle* ib = g.Create_Index_Buffer(indices, 6);
	if (vb == nullptr || ib == nullptr) return Fail("buffer creation failed");

	// Half size, shifted into the +x/+y quadrant of clip space. In D3D that is the
	// top-right of the screen; the backend's y flip has to put it there in Vulkan
	// too, which is the thing being checked.
	g.Set_Transform(D3DTS_WORLD, Scale(0.5f, 0.5f, 1.0f));
	g.Set_Transform(D3DTS_VIEW, Translation(0.5f, 0.5f, 0.0f));
	g.Set_Transform(D3DTS_PROJECTION, Matrix4x4::Identity());
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);

	h.Begin();
	h.Draw(vb, ib, 2);
	h.End();
	if (!h.Read_Back()) return Fail("readback failed");

	// Clip-space x,y in [0,1] -> the top-right quarter of the target.
	const Rgba inside = h.Pixel(kWidth * 3 / 4, kHeight / 4);
	const Rgba outside = h.Pixel(kWidth / 4, kHeight * 3 / 4);
	if (!Near(inside, Rgba{255, 255, 255, 255}))
		return Fail("top-right should be covered, got " + To_String(inside));
	if (!Near(outside, Rgba{0, 0, 0, 0}))
		return Fail("bottom-left should be clear, got " + To_String(outside));
	return Pass("world*view maps the quad to the top-right quadrant");
}

// D3D8's directional light: colour = ambient*matAmbient + N.L * lightDiffuse *
// matDiffuse, with everything in camera space.
Outcome Case_Directional_Light(Harness& h) {
	RenderBackend& g = h.Gfx();
	h.Reset_State();
	// The normal is tilted 60 degrees from the light direction, so N.L = 0.5 exactly.
	const float angle = 60.0f * 3.14159265358979323846f / 180.0f;
	const WorldVertex quad[4] = {
	    {-1, -1, 0.5f, std::sin(angle), 0.0f, std::cos(angle), Argb(0xff, 0xff, 0xff, 0xff), 0, 0},
	    {1, -1, 0.5f, std::sin(angle), 0.0f, std::cos(angle), Argb(0xff, 0xff, 0xff, 0xff), 1, 0},
	    {1, 1, 0.5f, std::sin(angle), 0.0f, std::cos(angle), Argb(0xff, 0xff, 0xff, 0xff), 1, 1},
	    {-1, 1, 0.5f, std::sin(angle), 0.0f, std::cos(angle), Argb(0xff, 0xff, 0xff, 0xff), 0, 1},
	};
	const uint16_t indices[6] = {0, 1, 2, 0, 2, 3};
	VertexBufferHandle* vb = g.Create_Vertex_Buffer(
	    quad, sizeof(quad), D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1);
	IndexBufferHandle* ib = g.Create_Index_Buffer(indices, 6);
	if (vb == nullptr || ib == nullptr) return Fail("buffer creation failed");

	LightState light;
	light.type = D3DLIGHT_DIRECTIONAL;
	light.diffuse[0] = light.diffuse[1] = light.diffuse[2] = 1.0f;
	light.direction[0] = 0.0f;
	light.direction[1] = 0.0f;
	light.direction[2] = -1.0f; // pointing towards -z, i.e. at the quad's front
	g.Set_Light(0, &light);

	MaterialState material;
	material.diffuse[0] = material.diffuse[1] = material.diffuse[2] = 1.0f;
	material.ambient[0] = material.ambient[1] = material.ambient[2] = 1.0f;
	g.Set_Material(material);

	g.Set_DX8_Render_State(D3DRS_LIGHTING, 1);
	g.Set_DX8_Render_State(D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_MATERIAL);
	g.Set_DX8_Render_State(D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_MATERIAL);
	g.Set_DX8_Render_State(D3DRS_AMBIENT, Argb(0, 0x20, 0x20, 0x20));
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);

	h.Begin();
	h.Draw(vb, ib, 2);
	h.End();
	if (!h.Read_Back()) return Fail("readback failed");

	const float ambient = 32.0f / 255.0f;
	const float lit = ambient + 0.5f;
	return Check(h.Pixel(kWidth / 2, kHeight / 2), Quantise(lit, lit, lit, 1.0f),
	             "directional N.L=0.5 plus ambient 0x20");
}

// A point light with a1 attenuation. D3D8 lighting is per *vertex*, so the value
// that can be predicted exactly is the one at the corners: the light is at (0,0,4)
// and every corner is at distance sqrt(18), which makes the interpolated interior
// constant and equal to the corner value.
//   atten = min(1, 1/(a1*d)) = 1/(0.5*sqrt(18))
//   N.L   = 4/sqrt(18)
//   result = atten * N.L = 4/(0.5*18) = 0.4444
Outcome Case_Point_Light(Harness& h) {
	RenderBackend& g = h.Gfx();
	h.Reset_State();
	// The quad sits at z = 0 facing +z; the light is 4 units away along +z.
	const WorldVertex quad[4] = {
	    {-1, -1, 0.0f, 0, 0, 1, Argb(0xff, 0xff, 0xff, 0xff), 0, 0},
	    {1, -1, 0.0f, 0, 0, 1, Argb(0xff, 0xff, 0xff, 0xff), 1, 0},
	    {1, 1, 0.0f, 0, 0, 1, Argb(0xff, 0xff, 0xff, 0xff), 1, 1},
	    {-1, 1, 0.0f, 0, 0, 1, Argb(0xff, 0xff, 0xff, 0xff), 0, 1},
	};
	const uint16_t indices[6] = {0, 1, 2, 0, 2, 3};
	VertexBufferHandle* vb = g.Create_Vertex_Buffer(
	    quad, sizeof(quad), D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1);
	IndexBufferHandle* ib = g.Create_Index_Buffer(indices, 6);
	if (vb == nullptr || ib == nullptr) return Fail("buffer creation failed");

	LightState light;
	light.type = D3DLIGHT_POINT;
	light.diffuse[0] = light.diffuse[1] = light.diffuse[2] = 1.0f;
	light.position[2] = 4.0f;
	light.range = 100.0f;
	light.attenuation0 = 0.0f;
	light.attenuation1 = 0.5f;
	light.attenuation2 = 0.0f;
	g.Set_Light(0, &light);
	MaterialState material;
	material.diffuse[0] = material.diffuse[1] = material.diffuse[2] = 1.0f;
	g.Set_Material(material);
	g.Set_DX8_Render_State(D3DRS_LIGHTING, 1);
	g.Set_DX8_Render_State(D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_MATERIAL);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);

	h.Begin();
	h.Draw(vb, ib, 2);
	h.End();
	if (!h.Read_Back()) return Fail("readback failed");

	const float expected = 4.0f / (0.5f * 18.0f);
	return Check(h.Pixel(kWidth / 2, kHeight / 2),
	             Quantise(expected, expected, expected, 1.0f),
	             "point light, 1/(a1*d) attenuation at d=sqrt(18)");
}

// A spot light. Lighting is per vertex, so the cone has to be judged at the
// vertices, not at the pixel on the axis: the quad's four corners all sit at 54.7
// degrees off the axis of a light at (0,0,1) pointing down -z. Two draws, one with
// a cone wide enough to contain them (spot factor 1) and one with a cone that
// excludes them (spot factor 0), so both assertions are exact and neither lands in
// the falloff band between theta and phi.
//   d = sqrt(3), atten = 1/a0 = 1, N.L = 1/sqrt(3) = 0.5774
Outcome Case_Spot_Light(Harness& h) {
	RenderBackend& g = h.Gfx();
	const WorldVertex quad[4] = {
	    {-1, -1, 0.0f, 0, 0, 1, Argb(0xff, 0xff, 0xff, 0xff), 0, 0},
	    {1, -1, 0.0f, 0, 0, 1, Argb(0xff, 0xff, 0xff, 0xff), 1, 0},
	    {1, 1, 0.0f, 0, 0, 1, Argb(0xff, 0xff, 0xff, 0xff), 1, 1},
	    {-1, 1, 0.0f, 0, 0, 1, Argb(0xff, 0xff, 0xff, 0xff), 0, 1},
	};
	const uint16_t indices[6] = {0, 1, 2, 0, 2, 3};
	const float kPi = 3.14159265358979323846f;

	auto draw_with_cone = [&](float theta_degrees, float phi_degrees, Rgba& out) {
		h.Reset_State();
		VertexBufferHandle* vb = g.Create_Vertex_Buffer(
		    quad, sizeof(quad), D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1);
		IndexBufferHandle* ib = g.Create_Index_Buffer(indices, 6);
		if (vb == nullptr || ib == nullptr) return false;

		LightState light;
		light.type = D3DLIGHT_SPOT;
		light.diffuse[0] = light.diffuse[1] = light.diffuse[2] = 1.0f;
		light.position[2] = 1.0f;
		light.direction[2] = -1.0f;
		light.range = 100.0f;
		light.attenuation0 = 1.0f;
		light.attenuation1 = 0.0f;
		light.attenuation2 = 0.0f;
		light.falloff = 1.0f;
		light.theta = theta_degrees * kPi / 180.0f;
		light.phi = phi_degrees * kPi / 180.0f;
		g.Set_Light(0, &light);
		MaterialState material;
		material.diffuse[0] = material.diffuse[1] = material.diffuse[2] = 1.0f;
		g.Set_Material(material);
		g.Set_DX8_Render_State(D3DRS_LIGHTING, 1);
		g.Set_DX8_Render_State(D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_MATERIAL);
		g.Set_DX8_Texture_Stage_State(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
		g.Set_DX8_Texture_Stage_State(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
		g.Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
		g.Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);

		h.Begin();
		h.Draw(vb, ib, 2);
		h.End();
		if (!h.Read_Back()) return false;
		out = h.Pixel(kWidth / 2, kHeight / 2);
		return true;
	};

	Rgba inside{}, outside{};
	// theta/2 = 70 deg > 54.7 deg: every vertex is inside the inner cone.
	if (!draw_with_cone(140.0f, 160.0f, inside)) return Fail("draw failed");
	// phi/2 = 10 deg < 54.7 deg: every vertex is outside the outer cone.
	if (!draw_with_cone(10.0f, 20.0f, outside)) return Fail("draw failed");

	const float lit = 1.0f / std::sqrt(3.0f);
	const Rgba expected_inside = Quantise(lit, lit, lit, 1.0f);
	if (!Near(inside, expected_inside))
		return Fail("inside theta got=" + To_String(inside) +
		            " expected=" + To_String(expected_inside));
	const Rgba expected_outside = Quantise(0.0f, 0.0f, 0.0f, 1.0f);
	if (!Near(outside, expected_outside))
		return Fail("outside phi got=" + To_String(outside) +
		            " expected=" + To_String(expected_outside));
	return Pass("spot cone: inside theta=" + To_String(inside) + ", outside phi=" +
	            To_String(outside));
}

// D3DRS_FOGVERTEXMODE = D3DFOG_LINEAR: f = (end - d)/(end - start), and the pixel is
// mix(fogColor, colour, f). The quad is drawn with a world transform that puts it at
// a known camera-space depth.
Outcome Case_Vertex_Fog(Harness& h) {
	RenderBackend& g = h.Gfx();
	h.Reset_State();
	const WorldVertex quad[4] = {
	    {-1, -1, 0.5f, 0, 0, 1, Argb(0xff, 0xff, 0xff, 0xff), 0, 0},
	    {1, -1, 0.5f, 0, 0, 1, Argb(0xff, 0xff, 0xff, 0xff), 1, 0},
	    {1, 1, 0.5f, 0, 0, 1, Argb(0xff, 0xff, 0xff, 0xff), 1, 1},
	    {-1, 1, 0.5f, 0, 0, 1, Argb(0xff, 0xff, 0xff, 0xff), 0, 1},
	};
	const uint16_t indices[6] = {0, 1, 2, 0, 2, 3};
	VertexBufferHandle* vb = g.Create_Vertex_Buffer(
	    quad, sizeof(quad), D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1);
	IndexBufferHandle* ib = g.Create_Index_Buffer(indices, 6);
	if (vb == nullptr || ib == nullptr) return Fail("buffer creation failed");

	// Camera-space depth 0.5: start 0, end 1 -> f = 0.5.
	const float start = 0.0f, end = 1.0f;
	uint32_t start_bits = 0, end_bits = 0;
	std::memcpy(&start_bits, &start, sizeof(start_bits));
	std::memcpy(&end_bits, &end, sizeof(end_bits));
	g.Set_DX8_Render_State(D3DRS_FOGENABLE, 1);
	g.Set_DX8_Render_State(D3DRS_FOGVERTEXMODE, D3DFOG_LINEAR);
	g.Set_DX8_Render_State(D3DRS_FOGTABLEMODE, D3DFOG_NONE);
	g.Set_DX8_Render_State(D3DRS_FOGSTART, start_bits);
	g.Set_DX8_Render_State(D3DRS_FOGEND, end_bits);
	g.Set_DX8_Render_State(D3DRS_FOGCOLOR, Argb(0, 0, 0, 0xff)); // blue fog
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);

	h.Begin();
	h.Draw(vb, ib, 2);
	h.End();
	if (!h.Read_Back()) return Fail("readback failed");

	// f = (1 - 0.5)/(1 - 0) = 0.5: half white, half blue fog.
	return Check(h.Pixel(kWidth / 2, kHeight / 2), Quantise(0.5f, 0.5f, 1.0f, 1.0f),
	             "vertex LINEAR fog, f=0.5");
}

// D3DRS_FOGTABLEMODE = D3DFOG_EXP2: f = exp(-(d*density)^2), computed per pixel.
Outcome Case_Table_Fog(Harness& h) {
	RenderBackend& g = h.Gfx();
	h.Reset_State();
	const WorldVertex quad[4] = {
	    {-1, -1, 0.5f, 0, 0, 1, Argb(0xff, 0xff, 0xff, 0xff), 0, 0},
	    {1, -1, 0.5f, 0, 0, 1, Argb(0xff, 0xff, 0xff, 0xff), 1, 0},
	    {1, 1, 0.5f, 0, 0, 1, Argb(0xff, 0xff, 0xff, 0xff), 1, 1},
	    {-1, 1, 0.5f, 0, 0, 1, Argb(0xff, 0xff, 0xff, 0xff), 0, 1},
	};
	const uint16_t indices[6] = {0, 1, 2, 0, 2, 3};
	VertexBufferHandle* vb = g.Create_Vertex_Buffer(
	    quad, sizeof(quad), D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1);
	IndexBufferHandle* ib = g.Create_Index_Buffer(indices, 6);
	if (vb == nullptr || ib == nullptr) return Fail("buffer creation failed");

	const float density = 2.0f;
	uint32_t density_bits = 0;
	std::memcpy(&density_bits, &density, sizeof(density_bits));
	g.Set_DX8_Render_State(D3DRS_FOGENABLE, 1);
	g.Set_DX8_Render_State(D3DRS_FOGVERTEXMODE, D3DFOG_NONE);
	g.Set_DX8_Render_State(D3DRS_FOGTABLEMODE, D3DFOG_EXP2);
	g.Set_DX8_Render_State(D3DRS_FOGDENSITY, density_bits);
	g.Set_DX8_Render_State(D3DRS_FOGCOLOR, Argb(0, 0, 0, 0));
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);

	h.Begin();
	h.Draw(vb, ib, 2);
	h.End();
	if (!h.Read_Back()) return Fail("readback failed");

	const float d = 0.5f;
	const float f = std::exp(-(d * density) * (d * density));
	return Check(h.Pixel(kWidth / 2, kHeight / 2), Quantise(f, f, f, 1.0f),
	             "table EXP2 fog at d=0.5, density=2");
}

// D3DRS_ALPHATESTENABLE with GREATEREQUAL and the engine's most common reference
// value, 0x80: alpha 0x80 survives, alpha 0x7f does not.
Outcome Case_Alpha_Test(Harness& h) {
	RenderBackend& g = h.Gfx();
	h.Reset_State();
	g.Set_DX8_Render_State(D3DRS_ALPHATESTENABLE, 1);
	g.Set_DX8_Render_State(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
	g.Set_DX8_Render_State(D3DRS_ALPHAREF, 0x80);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);

	h.Begin();
	h.Draw_Screen_Quad(Argb(0x80, 0xff, 0x00, 0x00)); // passes
	h.End();
	if (!h.Read_Back()) return Fail("readback failed");
	const Rgba passed = h.Pixel(kWidth / 2, kHeight / 2);

	h.Begin();
	h.Draw_Screen_Quad(Argb(0x7f, 0x00, 0xff, 0x00)); // fails
	h.End();
	if (!h.Read_Back()) return Fail("readback failed");
	const Rgba failed = h.Pixel(kWidth / 2, kHeight / 2);

	if (!Near(passed, Rgba{255, 0, 0, 128}))
		return Fail("alpha 0x80 >= ref 0x80 should draw, got " + To_String(passed));
	if (!Near(failed, Rgba{0, 0, 0, 0}))
		return Fail("alpha 0x7f < ref 0x80 should be discarded, got " + To_String(failed));
	return Pass("GREATEREQUAL ref=0x80 keeps 0x80, discards 0x7f");
}

// D3DRS_ZBIAS. D3D8 leaves the magnitude of a ZBIAS unit undefined, so what is
// asserted is the property the engine relies on: with ZFUNC=LESS a coplanar second
// polygon is hidden without bias and visible with it, and never the other way round.
Outcome Case_Depth_Bias(Harness& h) {
	RenderBackend& g = h.Gfx();
	h.Reset_State();
	g.Set_DX8_Render_State(D3DRS_ZENABLE, 1);
	g.Set_DX8_Render_State(D3DRS_ZWRITEENABLE, 1);
	g.Set_DX8_Render_State(D3DRS_ZFUNC, D3DCMP_LESS);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);

	h.Begin();
	h.Draw_Screen_Quad(Argb(0xff, 0xff, 0x00, 0x00));
	g.Set_DX8_Render_State(D3DRS_ZBIAS, 0);
	h.Draw_Screen_Quad(Argb(0xff, 0x00, 0xff, 0x00)); // coplanar: must be rejected
	h.End();
	if (!h.Read_Back()) return Fail("readback failed");
	const Rgba unbiased = h.Pixel(kWidth / 2, kHeight / 2);

	h.Begin();
	g.Set_DX8_Render_State(D3DRS_ZBIAS, 0);
	h.Draw_Screen_Quad(Argb(0xff, 0xff, 0x00, 0x00));
	g.Set_DX8_Render_State(D3DRS_ZBIAS, 7); // the engine's decal/track value
	h.Draw_Screen_Quad(Argb(0xff, 0x00, 0xff, 0x00));
	h.End();
	if (!h.Read_Back()) return Fail("readback failed");
	const Rgba biased = h.Pixel(kWidth / 2, kHeight / 2);

	if (!Near(unbiased, Rgba{255, 0, 0, 255}))
		return Fail("coplanar without ZBIAS must not draw, got " + To_String(unbiased));
	if (!Near(biased, Rgba{0, 255, 0, 255}))
		return Fail("ZBIAS=7 must lift the coplanar polygon, got " + To_String(biased));
	return Pass("ZBIAS 0 hides, ZBIAS 7 shows a coplanar polygon (ZFUNC=LESS)");
}

// Scissor. No engine call site sets one (engine-usage-scan.py reports 0), so this
// is backend capability, not a reproduction of engine behaviour.
Outcome Case_Scissor(Harness& h) {
	RenderBackend& g = h.Gfx();
	h.Reset_State();
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
	g.Set_Scissor(true, 0, 0, static_cast<int32_t>(kWidth / 2),
	              static_cast<int32_t>(kHeight / 2));

	h.Begin();
	h.Draw_Screen_Quad(Argb(0xff, 0xff, 0xff, 0x00));
	h.End();
	if (!h.Read_Back()) return Fail("readback failed");

	const Rgba inside = h.Pixel(kWidth / 4, kHeight / 4);
	const Rgba outside = h.Pixel(kWidth * 3 / 4, kHeight * 3 / 4);
	if (!Near(inside, Rgba{255, 255, 0, 255}))
		return Fail("inside the scissor should draw, got " + To_String(inside));
	if (!Near(outside, Rgba{0, 0, 0, 0}))
		return Fail("outside the scissor should not, got " + To_String(outside));
	return Pass("scissor clips to the top-left quadrant");
}

// Stencil, through the backend rather than raw Vulkan: write 1 where a small quad
// covers, then draw a full-target quad that only passes where the stencil is 1.
Outcome Case_Stencil(Harness& h) {
	RenderBackend& g = h.Gfx();
	h.Reset_State();
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);

	h.Begin();
	// pass 1: stencil = 1 in the scissored region, no colour
	g.Set_DX8_Render_State(D3DRS_STENCILENABLE, 1);
	g.Set_DX8_Render_State(D3DRS_STENCILFUNC, D3DCMP_ALWAYS);
	g.Set_DX8_Render_State(D3DRS_STENCILPASS, D3DSTENCILOP_REPLACE);
	g.Set_DX8_Render_State(D3DRS_STENCILREF, 1);
	g.Set_DX8_Render_State(D3DRS_COLORWRITEENABLE, 0);
	g.Set_Scissor(true, 0, 0, static_cast<int32_t>(kWidth / 2),
	              static_cast<int32_t>(kHeight / 2));
	h.Draw_Screen_Quad(Argb(0xff, 0xff, 0xff, 0xff));

	// pass 2: colour only where stencil == 1
	g.Set_Scissor(false, 0, 0, 0, 0);
	g.Set_DX8_Render_State(D3DRS_COLORWRITEENABLE, 0xf);
	g.Set_DX8_Render_State(D3DRS_STENCILFUNC, D3DCMP_EQUAL);
	g.Set_DX8_Render_State(D3DRS_STENCILPASS, D3DSTENCILOP_KEEP);
	h.Draw_Screen_Quad(Argb(0xff, 0x00, 0xff, 0xff));
	h.End();
	if (!h.Read_Back()) return Fail("readback failed");

	const Rgba stenciled = h.Pixel(kWidth / 4, kHeight / 4);
	const Rgba clear = h.Pixel(kWidth * 3 / 4, kHeight * 3 / 4);
	if (!Near(stenciled, Rgba{0, 255, 255, 255}))
		return Fail("stencil==1 should draw, got " + To_String(stenciled));
	if (!Near(clear, Rgba{0, 0, 0, 0}))
		return Fail("stencil==0 should not, got " + To_String(clear));
	return Pass("REPLACE then EQUAL through the backend");
}

// D3DTSS_TEXCOORDINDEX: stage 1 reads coordinate set 0 instead of its own, which is
// what the engine's terrain and shadow passes do (33 PASSTHRU sites plus 21 literal
// index writes).
Outcome Case_Texcoord_Index(Harness& h) {
	RenderBackend& g = h.Gfx();
	h.Reset_State();
	// Two coordinate sets: set 0 addresses the left texel, set 1 the right one.
	struct Vertex2 {
		float x, y, z, rhw;
		uint32_t diffuse;
		float u0, v0;
		float u1, v1;
	};
	const uint32_t texels[4] = {Argb(0xff, 0xff, 0, 0), Argb(0xff, 0, 0xff, 0),
	                            Argb(0xff, 0xff, 0, 0), Argb(0xff, 0, 0xff, 0)};
	TextureHandle* texture =
	    g.Create_Texture(2, 2, reinterpret_cast<const uint8_t*>(texels));
	if (texture == nullptr) return Fail("Create_Texture failed");

	const float w = static_cast<float>(kWidth), hh = static_cast<float>(kHeight);
	const Vertex2 quad[4] = {
	    {0, 0, 0.5f, 1, 0xffffffff, 0.25f, 0.5f, 0.75f, 0.5f},
	    {w, 0, 0.5f, 1, 0xffffffff, 0.25f, 0.5f, 0.75f, 0.5f},
	    {w, hh, 0.5f, 1, 0xffffffff, 0.25f, 0.5f, 0.75f, 0.5f},
	    {0, hh, 0.5f, 1, 0xffffffff, 0.25f, 0.5f, 0.75f, 0.5f},
	};
	const uint16_t indices[6] = {0, 1, 2, 0, 2, 3};
	VertexBufferHandle* vb = g.Create_Vertex_Buffer(
	    quad, sizeof(quad), D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX2);
	IndexBufferHandle* ib = g.Create_Index_Buffer(indices, 6);
	if (vb == nullptr || ib == nullptr) return Fail("buffer creation failed");

	g.Set_Texture(0, texture);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_TEXCOORDINDEX, 1); // read set 1

	h.Begin();
	h.Draw(vb, ib, 2);
	h.End();
	if (!h.Read_Back()) return Fail("readback failed");
	return Check(h.Pixel(kWidth / 2, kHeight / 2), Rgba{0, 255, 0, 255},
	             "TEXCOORDINDEX=1 samples the second coordinate set");
}

// D3DTS_TEXTURE0 with D3DTTFF_COUNT2, the engine's most common transform setting
// (56 sites): a translation of +0.5 in u moves the sample from the left texel to
// the right one.
Outcome Case_Texture_Transform(Harness& h) {
	RenderBackend& g = h.Gfx();
	h.Reset_State();
	const uint32_t texels[4] = {Argb(0xff, 0xff, 0, 0), Argb(0xff, 0, 0xff, 0),
	                            Argb(0xff, 0xff, 0, 0), Argb(0xff, 0, 0xff, 0)};
	TextureHandle* texture =
	    g.Create_Texture(2, 2, reinterpret_cast<const uint8_t*>(texels));
	if (texture == nullptr) return Fail("Create_Texture failed");
	g.Set_Texture(0, texture);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);
	g.Set_Transform(D3DTS_TEXTURE0, Translation(0.5f, 0.0f, 0.0f));

	h.Begin();
	h.Draw_Screen_Quad(Argb(0xff, 0xff, 0xff, 0xff), 0.25f, 0.5f);
	h.End();
	if (!h.Read_Back()) return Fail("readback failed");
	return Check(h.Pixel(kWidth / 2, kHeight / 2), Rgba{0, 255, 0, 255},
	             "D3DTS_TEXTURE0 translation moves the sample one texel");
}

// Every FVF the engine can present, decoded and drawn. What is asserted is that the
// declaration is accepted, the stride is right (a wrong stride shifts the colours),
// and the components D3D8 substitutes for a missing element are substituted.
struct FvfCase {
	const char* name;
	uint32_t fvf;
	size_t vertex_bytes;
};

Outcome Case_Fvf(Harness& h, const FvfCase& c) {
	RenderBackend& g = h.Gfx();
	h.Reset_State();
	// A quad of `vertex_bytes` per vertex, all zero except the fields the FVF
	// declares, filled so that the resulting pixel is known.
	std::vector<uint8_t> vertices(c.vertex_bytes * 4, 0);
	const float w = static_cast<float>(kWidth), hh = static_cast<float>(kHeight);
	const float xy[4][2] = {{0, 0}, {w, 0}, {w, hh}, {0, hh}};
	for (int i = 0; i < 4; ++i) {
		uint8_t* v = vertices.data() + c.vertex_bytes * i;
		size_t offset = 0;
		// The position bits are a 3-bit field, not flags: XYZB1 (0x006) and XYZB4
		// (0x00c) both have XYZRHW's 0x004 bit set, so this has to mask.
		const uint32_t position_bits = c.fvf & D3DFVF_POSITION_MASK;
		if (position_bits == D3DFVF_XYZRHW) {
			const float position[4] = {xy[i][0], xy[i][1], 0.5f, 1.0f};
			std::memcpy(v, position, sizeof(position));
			offset += sizeof(position);
		} else {
			// Clip-space corners for the non-pretransformed declarations.
			const float position[3] = {xy[i][0] / w * 2.0f - 1.0f,
			                           xy[i][1] / hh * 2.0f - 1.0f, 0.5f};
			std::memcpy(v, position, sizeof(position));
			offset += sizeof(position);
			const uint32_t blend_dwords =
			    position_bits == D3DFVF_XYZ ? 0u : (position_bits - 4u) / 2u;
			offset += blend_dwords * 4; // weights and packed indices stay zero
		}
		if (c.fvf & D3DFVF_NORMAL) {
			const float normal[3] = {0.0f, 0.0f, 1.0f};
			std::memcpy(v + offset, normal, sizeof(normal));
			offset += sizeof(normal);
		}
		if (c.fvf & D3DFVF_DIFFUSE) {
			const uint32_t diffuse = Argb(0xff, 0x00, 0x80, 0xff);
			std::memcpy(v + offset, &diffuse, sizeof(diffuse));
			offset += sizeof(diffuse);
		}
		if (c.fvf & D3DFVF_SPECULAR) offset += 4;
		// Texture coordinates are left at zero; the cascade below does not sample.
	}
	VertexBufferHandle* vb =
	    g.Create_Vertex_Buffer(vertices.data(), vertices.size(), c.fvf);
	const uint16_t indices[6] = {0, 1, 2, 0, 2, 3};
	IndexBufferHandle* ib = g.Create_Index_Buffer(indices, 6);
	if (vb == nullptr || ib == nullptr) return Fail("Decode_Fvf rejected the declaration");

	g.Set_DX8_Texture_Stage_State(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);

	h.Begin();
	h.Draw(vb, ib, 2);
	h.End();
	if (!h.Read_Back()) return Fail("readback failed");
	// With D3DFVF_DIFFUSE the vertex colour is used; without it D3D8 substitutes
	// opaque white, which is what the dummy attribute has to supply.
	const Rgba expected = (c.fvf & D3DFVF_DIFFUSE) ? Rgba{0x00, 0x80, 0xff, 0xff}
	                                               : Rgba{255, 255, 255, 255};
	return Check(h.Pixel(kWidth / 2, kHeight / 2), expected, c.name);
}

// Every TextureFormat the engine's loaders can produce, uploaded as a 4x4 texel of a
// known colour and sampled. Formats the device cannot sample are reported PENDING
// with the reason, not failed: that is a finding about the device, not a bug.
struct FormatCase {
	const char* name;
	TextureFormat format;
};

const FormatCase kFormatCases[] = {
    {"A8R8G8B8", TextureFormat::A8R8G8B8}, {"X8R8G8B8", TextureFormat::X8R8G8B8},
    {"R8G8B8", TextureFormat::R8G8B8},     {"A4R4G4B4", TextureFormat::A4R4G4B4},
    {"A1R5G5B5", TextureFormat::A1R5G5B5}, {"R5G6B5", TextureFormat::R5G6B5},
    {"L8", TextureFormat::L8},             {"A8", TextureFormat::A8},
    {"A8L8", TextureFormat::A8L8},         {"V8U8", TextureFormat::V8U8},
    {"P8", TextureFormat::P8},             {"DXT1", TextureFormat::DXT1},
    {"DXT2", TextureFormat::DXT2},         {"DXT3", TextureFormat::DXT3},
    {"DXT4", TextureFormat::DXT4},         {"DXT5", TextureFormat::DXT5},
};

// A 4x4 texel image in `format`, all texels the same colour, plus the colour the
// sampler must produce for it. Returns false for formats this helper cannot encode.
bool Encode_Format(TextureFormat format, std::vector<uint8_t>& bytes, Rgba& expected,
                   std::vector<uint32_t>& palette) {
	const int texels = 16;
	switch (format) {
	case TextureFormat::A8R8G8B8:
	case TextureFormat::X8R8G8B8: {
		bytes.resize(texels * 4);
		for (int i = 0; i < texels; ++i) {
			bytes[i * 4 + 0] = 0x20; // B
			bytes[i * 4 + 1] = 0x40; // G
			bytes[i * 4 + 2] = 0x80; // R
			bytes[i * 4 + 3] = 0xc0; // A
		}
		// X8R8G8B8 has no alpha channel: the sampler must read 1.0.
		expected = format == TextureFormat::A8R8G8B8 ? Rgba{0x80, 0x40, 0x20, 0xc0}
		                                             : Rgba{0x80, 0x40, 0x20, 0xff};
		return true;
	}
	case TextureFormat::R8G8B8: {
		bytes.resize(texels * 3);
		for (int i = 0; i < texels; ++i) {
			bytes[i * 3 + 0] = 0x20;
			bytes[i * 3 + 1] = 0x40;
			bytes[i * 3 + 2] = 0x80;
		}
		expected = Rgba{0x80, 0x40, 0x20, 0xff};
		return true;
	}
	case TextureFormat::A4R4G4B4: {
		// 0xC842: a=0xc, r=0x8, g=0x4, b=0x2; 4-bit n expands to n*17.
		bytes.resize(texels * 2);
		for (int i = 0; i < texels; ++i) {
			const uint16_t value = 0xc842;
			std::memcpy(bytes.data() + i * 2, &value, sizeof(value));
		}
		expected = Rgba{0x88, 0x44, 0x22, 0xcc};
		return true;
	}
	case TextureFormat::A1R5G5B5: {
		// a=1, r=0x10 (of 31), g=0x08, b=0x04; 5-bit n expands to (n*255+15)/31.
		bytes.resize(texels * 2);
		const uint16_t value =
		    static_cast<uint16_t>((1u << 15) | (0x10u << 10) | (0x08u << 5) | 0x04u);
		for (int i = 0; i < texels; ++i)
			std::memcpy(bytes.data() + i * 2, &value, sizeof(value));
		expected = Rgba{(0x10 * 255 + 15) / 31, (0x08 * 255 + 15) / 31,
		                (0x04 * 255 + 15) / 31, 255};
		return true;
	}
	case TextureFormat::R5G6B5: {
		bytes.resize(texels * 2);
		const uint16_t value = static_cast<uint16_t>((0x10u << 11) | (0x20u << 5) | 0x04u);
		for (int i = 0; i < texels; ++i)
			std::memcpy(bytes.data() + i * 2, &value, sizeof(value));
		expected = Rgba{(0x10 * 255 + 15) / 31, (0x20 * 255 + 31) / 63,
		                (0x04 * 255 + 15) / 31, 255};
		return true;
	}
	case TextureFormat::L8: {
		bytes.assign(texels, 0x60);
		expected = Rgba{0x60, 0x60, 0x60, 255};
		return true;
	}
	case TextureFormat::A8: {
		bytes.assign(texels, 0x60);
		// The alpha is defined; the colour is not -- see the A8 note below.
		expected = Rgba{0, 0, 0, 0x60};
		return true;
	}
	case TextureFormat::A8L8: {
		bytes.resize(texels * 2);
		for (int i = 0; i < texels; ++i) {
			bytes[i * 2 + 0] = 0x60; // L
			bytes[i * 2 + 1] = 0xa0; // A
		}
		expected = Rgba{0x60, 0x60, 0x60, 0xa0};
		return true;
	}
	case TextureFormat::P8: {
		bytes.assign(texels, 7);
		palette.assign(256, 0);
		palette[7] = Argb(0xc0, 0x80, 0x40, 0x20);
		expected = Rgba{0x80, 0x40, 0x20, 0xc0};
		return true;
	}
	case TextureFormat::V8U8: {
		// Signed; sampled as SNORM. 0x40 == 64/127.
		bytes.assign(texels * 2, 0x40);
		const int value = static_cast<int>(64.0f / 127.0f * 255.0f + 0.5f);
		expected = Rgba{value, value, 0, 255};
		return true;
	}
	case TextureFormat::DXT1:
	case TextureFormat::DXT2:
	case TextureFormat::DXT3:
	case TextureFormat::DXT4:
	case TextureFormat::DXT5:
		return false; // covered, with real block encodings, by zh-feature-probe
	}
	return false;
}

Outcome Case_Texture_Format(Harness& h, const FormatCase& c) {
	RenderBackend& g = h.Gfx();
	h.Reset_State();
	if (!g.Supports_Texture_Format(c.format))
		return Pending(std::string(c.name) + ": device cannot sample this format");

	std::vector<uint8_t> bytes;
	std::vector<uint32_t> palette;
	Rgba expected;
	if (!Encode_Format(c.format, bytes, expected, palette))
		return Pending(std::string(c.name) + ": block format, asserted in zh-feature-probe");

	const TextureMip mip{bytes.data(), bytes.size(), 4, 4};
	TextureDesc desc;
	desc.format = c.format;
	desc.mip_count = 1;
	desc.mips = &mip;
	desc.palette = palette.empty() ? nullptr : palette.data();
	TextureHandle* texture = g.Create_Texture(desc);
	if (texture == nullptr) return Fail(std::string(c.name) + ": Create_Texture failed");

	g.Set_Texture(0, texture);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
	g.Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);

	h.Begin();
	h.Draw_Screen_Quad(Argb(0xff, 0xff, 0xff, 0xff), 0.5f, 0.5f);
	h.End();
	if (!h.Read_Back()) return Fail("readback failed");
	// One LSB of slack on top of the usual tolerance: the 5- and 6-bit expansions
	// are exactly specified, but the sampler is free to expand by replication or by
	// division, which differ by at most one.
	const Rgba actual = h.Pixel(kWidth / 2, kHeight / 2);
	const std::string detail = std::string(c.name) + " got=" + To_String(actual) +
	                           " expected=" + To_String(expected);
	if (c.format == TextureFormat::A8) {
		// D3D8's documentation defines D3DFMT_A8 as "8-bit alpha only" and does not
		// say what the colour channels sample as, and there is no D3D8 device here
		// to ask. The alpha is asserted; the colour is reported, not asserted. The
		// backend reproduces D3D9/DXGI's (0,0,0,A).
		if (std::abs(actual.a - expected.a) > 2) return Fail(detail + " (alpha)");
		return Pending(std::string(c.name) + " alpha=" + std::to_string(actual.a) +
		               " asserted; D3D8 does not define the colour an alpha-only"
		               " texture samples as, backend gives " + To_String(actual));
	}
	return Near(actual, expected, 2) ? Pass(detail) : Fail(detail);
}

} // namespace

int main(int argc, char** argv) {
	bool validation = true;
	for (int i = 1; i < argc; ++i) {
		if (std::strcmp(argv[i], "--no-validation") == 0) validation = false;
		else if (std::strcmp(argv[i], "--help") == 0) {
			std::printf("usage: %s [--no-validation]\n", argv[0]);
			return 0;
		}
	}

	RenderBackend* backend = Create_Vulkan_Backend(validation, true);
	Harness harness(backend);
	if (!harness.Init()) {
		std::fprintf(stderr, "backend Init failed\n");
		return 1;
	}
	std::printf("device: %s\n\n", backend->Device_Description());

	int failed = 0;
	int pending = 0;
	auto report = [&](const char* group, const Outcome& outcome) {
		const char* label = outcome.status == Outcome::kPass      ? "PASS"
		                    : outcome.status == Outcome::kPending ? "PEND"
		                                                          : "FAIL";
		std::printf("  %-4s %-26s %s\n", label, group, outcome.detail.c_str());
		if (outcome.status == Outcome::kFail) ++failed;
		if (outcome.status == Outcome::kPending) ++pending;
	};

	std::printf("== texture-stage cascade ==\n");
	for (const CascadeCase& c : kCascadeCases) report("cascade op", Case_Cascade_Op(harness, c));
	report("cascade depth", Case_Eight_Stages(harness));
	report("cascade RESULTARG", Case_Result_Temp(harness));
	report("bump environment", Case_Bump_Env_Map(harness));

	std::printf("\n== vertex pipeline ==\n");
	const FvfCase kFvfCases[] = {
	    {"XYZ", D3DFVF_XYZ, 12},
	    {"XYZ|DIFFUSE", D3DFVF_XYZ | D3DFVF_DIFFUSE, 16},
	    {"XYZ|NORMAL|DIFFUSE|TEX1", D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1, 36},
	    {"XYZ|NORMAL|DIFFUSE|TEX2",
	     D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX2, 44},
	    {"XYZRHW|DIFFUSE", D3DFVF_XYZRHW | D3DFVF_DIFFUSE, 20},
	    {"XYZRHW|DIFFUSE|TEX1", D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1, 28},
	    // DX8_FVF_XYZNDUV1TG3: the tangent-space layout, 2+3+3+3 coordinates.
	    {"XYZ|NORMAL|DIFFUSE|TEX4(2,3,3,3)",
	     D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX4 |
	         D3DFVF_TEXCOORDSIZE2(0) | D3DFVF_TEXCOORDSIZE3(1) | D3DFVF_TEXCOORDSIZE3(2) |
	         D3DFVF_TEXCOORDSIZE3(3),
	     28 + 44},
	    // DX8_FVF_XYZNUV2DMAP: 1+4+2 coordinates.
	    {"XYZ|NORMAL|TEX3(1,4,2)",
	     D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX3 | D3DFVF_TEXCOORDSIZE1(0) |
	         D3DFVF_TEXCOORDSIZE4(1) | D3DFVF_TEXCOORDSIZE2(2),
	     24 + 28},
	    // The skinned declaration: 3 weights plus 4 packed bone indices.
	    {"XYZB4|LASTBETA_UBYTE4", D3DFVF_XYZB4 | D3DFVF_LASTBETA_UBYTE4, 28},
	};
	for (const FvfCase& c : kFvfCases) report("fvf", Case_Fvf(harness, c));
	report("transform", Case_Transform(harness));
	report("texcoord index", Case_Texcoord_Index(harness));
	report("texture transform", Case_Texture_Transform(harness));

	std::printf("\n== lighting and fog ==\n");
	report("directional light", Case_Directional_Light(harness));
	report("point light", Case_Point_Light(harness));
	report("spot light", Case_Spot_Light(harness));
	report("vertex fog", Case_Vertex_Fog(harness));
	report("table fog", Case_Table_Fog(harness));

	std::printf("\n== raster state ==\n");
	report("alpha test", Case_Alpha_Test(harness));
	report("depth bias", Case_Depth_Bias(harness));
	report("scissor", Case_Scissor(harness));
	report("stencil", Case_Stencil(harness));

	std::printf("\n== texture formats ==\n");
	for (const FormatCase& c : kFormatCases) report("format", Case_Texture_Format(harness, c));

	const uint32_t validation_messages = backend->Validation_Message_Count();
	std::printf("\nvalidation messages: %u\n", validation_messages);
	std::printf("%d case(s) failed, %d pending\n", failed, pending);
	harness.Shutdown();
	delete backend;
	if (validation && validation_messages != 0) return 1;
	return failed == 0 ? 0 : 1;
}
