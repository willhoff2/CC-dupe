// Renderer spike: the points/pixels rule, asserted on read-back pixels.
//
// The defect these tests exist for is docs/porting/apple-silicon-verification.md 8.4: on a
// Retina display the colour target was created at the client area's *points* (800x600) while
// the swapchain was in *pixels* (1600x1200), so the game rasterised a quarter of the panel and
// the presentation blit upscaled it. Nothing on Linux noticed, because every Linux display CI
// has runs at a backing scale of exactly 1, where the point size and the pixel size are the
// same number and the blit is an identity copy.
//
// So the scale is injectable: RenderBackend::Set_Render_Scale() is what a Retina display would
// have reported, and these tests run at 2.00 and at 1.25 on a machine that has no such display.
// Every assertion below fails against the pre-fix backend, which returned a target and a
// read-back sized in points whatever the scale was.
//
// The rule being asserted (docs/porting/hidpi-scale.md):
//   - the colour target, the viewport, the scissor and the read-back are in pixels;
//   - everything the engine passes in - back-buffer size, vertex coordinates, viewport and
//     scissor rectangles - is in points, exactly as D3D8 had it;
//   - a render-to-texture target is in its own pixels and the window's scale does not touch it.

#include "render_backend.h"

#ifdef SPIKE_WITH_PLATFORM_WINDOW
#include "platform/platform_window.h"
#endif

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace spike;

namespace {

// Small enough to keep the read-backs cheap at scale 2, and not square, so a width/height
// transposition cannot pass.
constexpr uint32_t kWidth = 100;
constexpr uint32_t kHeight = 80;

constexpr uint32_t kRed = 0xffff0000u;

struct Rgba {
	int r = 0, g = 0, b = 0, a = 0;
};

struct ScreenVertex {
	float x, y, z, rhw;
	uint32_t diffuse;
};

struct TexturedVertex {
	float x, y, z, rhw;
	uint32_t diffuse;
	float u, v;
};

int failures = 0;
int checks = 0;

void Check(bool condition, const char* what, const std::string& detail = std::string()) {
	++checks;
	if (condition) {
		std::printf("  PASS %s%s%s\n", what, detail.empty() ? "" : " ", detail.c_str());
		return;
	}
	++failures;
	std::printf("  FAIL %s%s%s\n", what, detail.empty() ? "" : " ", detail.c_str());
}

std::string Size_Text(uint32_t width, uint32_t height) {
	char buffer[64];
	std::snprintf(buffer, sizeof(buffer), "%ux%u", width, height);
	return buffer;
}

// The harness: D3D8 device state as DX8Wrapper leaves it, plus one pretransformed draw.
class Harness {
public:
	explicit Harness(RenderBackend* backend) : gfx_(backend) {}

	RenderBackend& Gfx() { return *gfx_; }

	void Reset_State(uint32_t target_width, uint32_t target_height) {
		RenderBackend& g = *gfx_;
		g.Set_DX8_Render_State(D3DRS_ZENABLE, 0);
		g.Set_DX8_Render_State(D3DRS_CULLMODE, D3DCULL_NONE);
		g.Set_DX8_Render_State(D3DRS_LIGHTING, 0);
		g.Set_DX8_Render_State(D3DRS_COLORVERTEX, 1);
		g.Set_DX8_Render_State(D3DRS_ALPHABLENDENABLE, 0);
		g.Set_DX8_Render_State(D3DRS_ALPHATESTENABLE, 0);
		g.Set_DX8_Render_State(D3DRS_COLORWRITEENABLE, 0xf);
		for (uint32_t stage = 0; stage < 8; ++stage) {
			g.Set_DX8_Texture_Stage_State(stage, D3DTSS_COLOROP, D3DTOP_DISABLE);
			g.Set_DX8_Texture_Stage_State(stage, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
			g.Set_Texture(stage, nullptr);
		}
		g.Set_Scissor(false, 0, 0, 0, 0);
		// In points, like D3D8's own SetViewport: the whole of the target.
		g.Set_Viewport(ViewportRect{0, 0, target_width, target_height, 0.0f, 1.0f});
	}

	// A pretransformed quad over the left half of the target, in points. Half rather than all
	// of it so that the read-back can locate the edge, which is what says whether the draw was
	// rasterised over the target's pixels or over its points.
	void Draw_Left_Half(uint32_t target_width, uint32_t target_height) {
		const float w = static_cast<float>(target_width) / 2.0f;
		const float h = static_cast<float>(target_height);
		const ScreenVertex strip[4] = {{0.0f, 0.0f, 0.5f, 1.0f, kRed},
		                               {w, 0.0f, 0.5f, 1.0f, kRed},
		                               {0.0f, h, 0.5f, 1.0f, kRed},
		                               {w, h, 0.5f, 1.0f, kRed}};
		gfx_->Draw_Primitive_UP(D3DPT_TRIANGLESTRIP, 2, strip, sizeof(ScreenVertex),
		                        D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
	}

	// The whole of the target, in points, textured with `texture` at point sampling: how the
	// pixels of a render-to-texture target are observed without locking it. Called inside the
	// scene the texture was rendered in, which is the shape the engine's render-to-texture
	// passes have.
	void Sample_Over_Target(TextureHandle* texture, uint32_t target_width,
	                        uint32_t target_height) {
		Reset_State(target_width, target_height);
		gfx_->Set_DX8_Texture_Stage_State(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
		gfx_->Set_DX8_Texture_Stage_State(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
		gfx_->Set_DX8_Texture_Stage_State(0, D3DTSS_MINFILTER, D3DTEXF_POINT);
		gfx_->Set_DX8_Texture_Stage_State(0, D3DTSS_MAGFILTER, D3DTEXF_POINT);
		gfx_->Set_Texture(0, texture);
		const float w = static_cast<float>(target_width);
		const float h = static_cast<float>(target_height);
		const TexturedVertex strip[4] = {{0.0f, 0.0f, 0.5f, 1.0f, kRed, 0.0f, 0.0f},
		                                 {w, 0.0f, 0.5f, 1.0f, kRed, 1.0f, 0.0f},
		                                 {0.0f, h, 0.5f, 1.0f, kRed, 0.0f, 1.0f},
		                                 {w, h, 0.5f, 1.0f, kRed, 1.0f, 1.0f}};
		gfx_->Clear(true, true, 0.0f, 0.0f, 0.0f, 1.0f);
		gfx_->Draw_Primitive_UP(D3DPT_TRIANGLESTRIP, 2, strip, sizeof(TexturedVertex),
		                        D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
		gfx_->End_Scene(false);
		gfx_->Set_Texture(0, nullptr);
	}

	bool Read_Back() { return gfx_->Read_Back_Color_Target(pixels_, format_); }

	const SurfaceFormat& Format() const { return format_; }

	Rgba Pixel(uint32_t x, uint32_t y) const {
		const size_t offset = (static_cast<size_t>(y) * format_.width + x) * 4;
		if (offset + 3 >= pixels_.size()) return Rgba{};
		const auto* p = reinterpret_cast<const unsigned char*>(pixels_.data()) + offset;
		return Rgba{p[0], p[1], p[2], p[3]};
	}

	// The x of the first background pixel on the given row, or the row's width if the row is
	// covered to its right edge.
	uint32_t Coverage_Edge(uint32_t y) const {
		for (uint32_t x = 0; x < format_.width; ++x) {
			if (Pixel(x, y).r < 128) return x;
		}
		return format_.width;
	}

private:
	RenderBackend* gfx_ = nullptr;
	std::string pixels_;
	SurfaceFormat format_{};
};

// --- the cases --------------------------------------------------------------

// Scale enters at the target, not at the blit: the colour target and the read-back are in
// pixels while the back buffer the engine was given stays in points.
void Case_Target_Is_In_Pixels(Harness& h, float scale) {
	RenderBackend& g = h.Gfx();
	const uint32_t expect_w = static_cast<uint32_t>(std::ceil(kWidth * scale));
	const uint32_t expect_h = static_cast<uint32_t>(std::ceil(kHeight * scale));

	uint32_t pixel_w = 0, pixel_h = 0;
	g.Device_Pixel_Size(pixel_w, pixel_h);
	Check(pixel_w == expect_w && pixel_h == expect_h, "colour target in pixels",
	      Size_Text(pixel_w, pixel_h) + ", expected " + Size_Text(expect_w, expect_h));
	Check(g.Render_Scale() == scale, "scale reported back");

	h.Reset_State(kWidth, kHeight);
	g.Begin_Scene();
	g.Clear(true, true, 0.0f, 0.0f, 0.0f, 1.0f);
	h.Draw_Left_Half(kWidth, kHeight);
	g.End_Scene(false);
	if (!h.Read_Back()) {
		Check(false, "read-back succeeded");
		return;
	}
	Check(h.Format().width == expect_w && h.Format().height == expect_h,
	      "read-back reports pixels",
	      Size_Text(h.Format().width, h.Format().height) + ", expected " +
	          Size_Text(expect_w, expect_h));

	// The pixels themselves: a draw that covers half the target in points has to cover half
	// its pixels. Rendering at point size and letting the presentation blit upscale would put
	// the edge at kWidth/2 in an image expect_w wide -- a quarter of the panel, which is
	// exactly what 8.4 measured.
	const uint32_t edge = h.Coverage_Edge(expect_h / 2);
	const uint32_t expect_edge = expect_w / 2;
	// One pixel of slack for the rasteriser's fill rule at a fractional scale.
	Check(edge + 1 >= expect_edge && edge <= expect_edge + 1, "half the target's pixels covered",
	      "edge at x=" + std::to_string(edge) + ", expected " + std::to_string(expect_edge));
	Check(h.Pixel(expect_edge / 2, expect_h / 2).r > 200, "covered half is drawn");
	Check(h.Pixel(expect_w - 2, expect_h / 2).r < 50, "uncovered half is clear");
}

// A scissor rectangle is in points too, and has to clip the same part of the panel at any
// scale: at scale 2 a 25-point rectangle clips at pixel 50, not pixel 25.
void Case_Scissor_Is_In_Points(Harness& h, float scale) {
	RenderBackend& g = h.Gfx();
	const uint32_t expect_w = static_cast<uint32_t>(std::ceil(kWidth * scale));
	const uint32_t expect_h = static_cast<uint32_t>(std::ceil(kHeight * scale));

	h.Reset_State(kWidth, kHeight);
	g.Set_Scissor(true, 0, 0, static_cast<int32_t>(kWidth) / 4,
	              static_cast<int32_t>(kHeight));
	g.Begin_Scene();
	g.Clear(true, true, 0.0f, 0.0f, 0.0f, 1.0f);
	h.Draw_Left_Half(kWidth, kHeight);
	g.End_Scene(false);
	g.Set_Scissor(false, 0, 0, 0, 0);
	if (!h.Read_Back()) {
		Check(false, "read-back succeeded (scissor)");
		return;
	}
	const uint32_t edge = h.Coverage_Edge(expect_h / 2);
	const uint32_t expect_edge = expect_w / 4;
	Check(edge + 1 >= expect_edge && edge <= expect_edge + 1, "scissor clips in points",
	      "edge at x=" + std::to_string(edge) + ", expected " + std::to_string(expect_edge));
}

// A window dragged from a Retina display to a 1x one changes backingScaleFactor while its
// client area in points does not move, so the scale has to be changeable after Init().
void Case_Scale_Change(Harness& h) {
	RenderBackend& g = h.Gfx();
	Check(!g.Set_Render_Scale(0.0f), "a non-positive scale is refused");

	Check(g.Set_Render_Scale(1.0f), "scale 2 -> 1 accepted");
	Case_Target_Is_In_Pixels(h, 1.0f);
	Check(g.Set_Render_Scale(2.0f), "scale 1 -> 2 accepted");
	Case_Target_Is_In_Pixels(h, 2.0f);
}

// A render-to-texture target is in its own pixels: the window's backing scale says nothing
// about a 32x32 texture, and multiplying it in would corrupt every cascade the engine renders
// to a texture.
void Case_Render_Target_Texture_Unscaled(Harness& h) {
	RenderBackend& g = h.Gfx();
	constexpr uint32_t kSize = 32;
	TextureHandle* texture = g.Create_Render_Target_Texture(kSize, kSize);
	if (texture == nullptr) {
		Check(false, "render-target texture created");
		return;
	}
	SurfaceHandle* surface = g.Get_Surface_Level(texture, 0);
	if (surface == nullptr) {
		Check(false, "render-target surface created");
		return;
	}

	// The engine's save-render-restore triple (W3DShaderManager::startRenderToTexture), with
	// the device's own depth buffer kept as D3D8 keeps it -- which at scale 2 is a 200x160
	// pixel depth attachment under a 32x32 colour one.
	SurfaceHandle* saved_depth = g.Get_Depth_Stencil_Target();
	if (!g.Set_Render_Target(surface, saved_depth)) {
		Check(false, "render-to-texture target bound at scale 2");
		return;
	}
	h.Reset_State(kSize, kSize);
	g.Begin_Scene();
	g.Clear(true, true, 0.0f, 0.0f, 0.0f, 1.0f);
	h.Draw_Left_Half(kSize, kSize);

	Check(g.Set_Render_Target(nullptr, saved_depth), "default target restored");
	// The texture's own pixels, seen by sampling it over the whole back buffer: its left half
	// is the quad and its right half is the clear. Had the window's scale reached the texture,
	// the 16-point quad would have been rasterised over 32 pixels and covered all of it.
	h.Sample_Over_Target(texture, kWidth, kHeight);
	if (h.Read_Back()) {
		const uint32_t mid_y = h.Format().height / 2;
		const int left = h.Pixel(h.Format().width / 4, mid_y).r;
		const int right = h.Pixel(h.Format().width - h.Format().width / 4, mid_y).r;
		Check(left > 200 && right < 50, "texture rasterised at its own 32x32",
		      "left=" + std::to_string(left) + " right=" + std::to_string(right));
	} else {
		Check(false, "read-back succeeded (render-to-texture)");
	}
	uint32_t pixel_w = 0, pixel_h = 0;
	g.Device_Pixel_Size(pixel_w, pixel_h);
	Check(pixel_w == kWidth * 2 && pixel_h == kHeight * 2, "default target still in pixels",
	      Size_Text(pixel_w, pixel_h));
}

// The back buffer at a scale other than 1 advertises points and holds pixels, so the paths
// that work in a surface's advertised units refuse it rather than handing back its top-left
// corner. Read_Back_Color_Target(), which reports the size it read, is the path for it.
void Case_Back_Buffer_Lock_Refused(Harness& h) {
	RenderBackend& g = h.Gfx();
	SurfaceHandle* back_buffer = g.Get_Render_Target();
	if (back_buffer == nullptr) {
		Check(false, "back buffer surface available");
		return;
	}
	LockedRect locked{};
	std::printf("  (the two refusals below are the assertion; their stderr lines are expected)\n");
	Check(!g.Surface_Bits(back_buffer, locked), "locking the scaled back buffer is refused");
	SurfaceHandle* staging = g.Create_Image_Surface(kWidth, kHeight, TextureFormat::A8R8G8B8);
	Check(staging != nullptr && !g.Copy_Rects(back_buffer, nullptr, 0, staging, nullptr),
	      "CopyRects off the scaled back buffer is refused");
}

#ifdef SPIKE_WITH_PLATFORM_WINDOW
// The same rule against a real window on a real display, with nothing injected: the scale comes
// from the window seam (NSWindow.backingScaleFactor on macOS), so this is the only form of the
// check that can answer "is the game rendering at the panel's resolution on this Mac?". Linux CI
// cannot run it -- every display it has is scale 1, and at scale 1 the pre-fix code passes -- which
// is why the headless injected-scale run above exists as well and neither replaces the other.
int Window_Mode(bool validation, float minimum_scale) {
	using namespace WWPlatform;

	std::printf("window backend: %s\n", Window_Backend_Name());
	WindowConfig config;
	config.Title = "Zero Hour HiDPI check: is the render full resolution?";
	config.Width = static_cast<int>(kWidth);
	config.Height = static_cast<int>(kHeight);
	void* window = Window_Create(config);
	if (window == nullptr) {
		std::fprintf(stderr, "Window_Create failed: %s\n", Window_Last_Error());
		return 1;
	}
	Window_Show(window, true);
	Window_Pump(window);

	int client_w = 0, client_h = 0;
	Window_Client_Size(window, client_w, client_h);
	const float scale = Window_Backing_Scale(window);
	std::printf("client area:   %dx%d points\n", client_w, client_h);
	std::printf("backing scale: %.2f  <- NSWindow.backingScaleFactor on macOS\n", scale);

	// Not headless: the point of this mode is that the frames go to the window's real swapchain,
	// so Init() refuses a window it cannot present to instead of measuring a headless render.
	RenderBackend* backend = Create_Vulkan_Backend(validation, /*headless=*/false);
	if (!backend->Init(window, static_cast<uint32_t>(client_w), static_cast<uint32_t>(client_h))) {
		std::fprintf(stderr, "backend Init failed\n");
		Window_Destroy(window);
		return 1;
	}
	std::printf("device: %s\n\n", backend->Device_Description());

	Harness harness(backend);
	const uint32_t points_w = static_cast<uint32_t>(client_w);
	const uint32_t points_h = static_cast<uint32_t>(client_h);
	uint32_t pixel_w = 0, pixel_h = 0;
	backend->Device_Pixel_Size(pixel_w, pixel_h);
	const uint32_t expect_w = static_cast<uint32_t>(std::ceil(points_w * scale));
	const uint32_t expect_h = static_cast<uint32_t>(std::ceil(points_h * scale));

	// Init() must have taken the scale from the window: this is the claim that fails on the pre-fix
	// code on a Retina display and cannot be reproduced anywhere else.
	Check(backend->Render_Scale() == scale, "the backend took the window's backing scale",
	      "backend " + std::to_string(backend->Render_Scale()) + ", window " +
	          std::to_string(scale));
	Check(pixel_w == expect_w && pixel_h == expect_h, "colour target in the panel's pixels",
	      Size_Text(pixel_w, pixel_h) + ", expected " + Size_Text(expect_w, expect_h));

	// A run on a scale-1 display measures nothing about 8.4, so it is a failure here rather than a
	// pass: the point of this binary is the Retina answer.
	char scale_detail[176];
	if (scale >= minimum_scale) {
		std::snprintf(scale_detail, sizeof(scale_detail), "%.2f >= %.2f", scale, minimum_scale);
	} else {
		std::snprintf(scale_detail, sizeof(scale_detail),
		              "%.2f < %.2f: this display cannot answer the question -- run it on the "
		              "Retina panel", scale, minimum_scale);
	}
	Check(scale >= minimum_scale, "the display's backing scale is >= the required minimum",
	      scale_detail);

	// The pixels: draw and present a frame the way the game does, then read the default target
	// back and locate the edge. The read-back is the rasterised image, before the presentation
	// blit, so a half-resolution render cannot be hidden by the upscale.
	harness.Reset_State(points_w, points_h);
	backend->Begin_Scene();
	backend->Clear(true, true, 0.0f, 0.0f, 0.0f, 1.0f);
	harness.Draw_Left_Half(points_w, points_h);
	backend->End_Scene(true);
	Window_Pump(window);
	if (harness.Read_Back()) {
		Check(harness.Format().width == expect_w && harness.Format().height == expect_h,
		      "read-back is the panel's pixels",
		      Size_Text(harness.Format().width, harness.Format().height));
		const uint32_t edge = harness.Coverage_Edge(expect_h / 2);
		const uint32_t expect_edge = expect_w / 2;
		Check(edge + 1 >= expect_edge && edge <= expect_edge + 1,
		      "half the panel's pixels covered by a half-width point-space quad",
		      "edge at x=" + std::to_string(edge) + ", expected " + std::to_string(expect_edge));
		// The sharpness claim, which is what "full resolution" means: the covered/uncovered
		// transition is one pixel wide. A 1x render upscaled by a VK_FILTER_LINEAR blit spreads it
		// over two or more, and cannot produce this even though its edge lands in the right place.
		const uint32_t mid_y = expect_h / 2;
		const int inside = edge == 0 ? 0 : harness.Pixel(edge - 1, mid_y).r;
		const int outside = harness.Pixel(edge, mid_y).r;
		Check(inside > 200 && outside < 50, "the coverage edge is one pixel wide",
		      "x=" + std::to_string(edge - 1) + " r=" + std::to_string(inside) + ", x=" +
		          std::to_string(edge) + " r=" + std::to_string(outside));
	} else {
		Check(false, "read-back succeeded");
	}

	// The scissor case works in the constants above, so it only applies if the window manager gave
	// the client area that was asked for.
	if (points_w == kWidth && points_h == kHeight) {
		Case_Scissor_Is_In_Points(harness, scale);
	}

	if (validation) {
		Check(backend->Validation_Active(), "validation layer loaded and messenger installed");
	}
	Check(backend->Validation_Message_Count() == 0, "validation messages",
	      std::to_string(backend->Validation_Message_Count()));

	backend->Shutdown();
	Window_Destroy(window);
	std::printf("\n%d checks, %d failed\n", checks, failures);
	if (failures == 0) {
		std::printf("OK: at backing scale %.2f the render is %ux%u pixels for a %ux%u point "
		            "client area, and the coverage edge is sharp\n", scale, pixel_w, pixel_h,
		            points_w, points_h);
	}
	return failures == 0 ? 0 : 1;
}
#endif  // SPIKE_WITH_PLATFORM_WINDOW

}  // namespace

int main(int argc, char** argv) {
	bool validation = true;
	bool window_mode = false;
	float minimum_scale = 2.0f;
	for (int i = 1; i < argc; ++i) {
		if (std::strcmp(argv[i], "--no-validation") == 0) {
			validation = false;
		} else if (std::strcmp(argv[i], "--window") == 0) {
			window_mode = true;
		} else if (std::strcmp(argv[i], "--min-scale") == 0 && i + 1 < argc) {
			minimum_scale = static_cast<float>(std::atof(argv[++i]));
		} else if (std::strcmp(argv[i], "--help") == 0) {
			std::printf("usage: %s [--no-validation] [--window] [--min-scale 2.0]\n", argv[0]);
			return 0;
		}
	}

	if (window_mode) {
#ifdef SPIKE_WITH_PLATFORM_WINDOW
		return Window_Mode(validation, minimum_scale);
#else
		std::fprintf(stderr, "--window (--min-scale %.2f) needs a build with a window backend: "
		                     "this binary is the headless one, which has no surface or swapchain "
		                     "code compiled in at all\n", minimum_scale);
		return 1;
#endif
	}

	RenderBackend* backend = Create_Vulkan_Backend(validation, true);
	// Before Init(), which is how a headless run chooses the scale a Retina window would have
	// reported. A real window does not need this: Init() asks the window seam.
	if (!backend->Set_Render_Scale(2.0f)) {
		std::fprintf(stderr, "Set_Render_Scale(2.0) refused before Init\n");
		return 1;
	}
	if (!backend->Init(nullptr, kWidth, kHeight)) {
		std::fprintf(stderr, "backend Init failed\n");
		return 1;
	}
	std::printf("device: %s\n", backend->Device_Description());
	std::printf("back buffer: %ux%u points\n\n", kWidth, kHeight);

	Harness harness(backend);

	std::printf("== scale 2.00 (a Retina panel's factor) ==\n");
	Case_Target_Is_In_Pixels(harness, 2.0f);
	Case_Scissor_Is_In_Points(harness, 2.0f);

	std::printf("\n== render-to-texture is unaffected ==\n");
	Case_Render_Target_Texture_Unscaled(harness);

	std::printf("\n== the scaled back buffer's point-space paths refuse ==\n");
	Case_Back_Buffer_Lock_Refused(harness);

	std::printf("\n== scale changes at run time (Retina -> 1x -> Retina) ==\n");
	Case_Scale_Change(harness);

	std::printf("\n== a fractional scale (1.25, as a scaled non-Retina display reports) ==\n");
	if (backend->Set_Render_Scale(1.25f)) {
		Case_Target_Is_In_Pixels(harness, 1.25f);
	} else {
		Check(false, "scale 1.25 accepted");
	}

	std::printf("\n== validation ==\n");
	if (validation) {
		// The layer's absence is the failure, not its silence: zero messages from a layer that
		// never loaded proves nothing (apple-silicon-verification.md 8.1).
		Check(backend->Validation_Active(), "validation layer loaded and messenger installed");
	} else {
		std::printf("  SKIP validation not requested\n");
	}
	Check(backend->Validation_Message_Count() == 0, "validation messages",
	      std::to_string(backend->Validation_Message_Count()));

	backend->Shutdown();
	std::printf("\n%d checks, %d failed\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
