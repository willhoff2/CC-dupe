/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 TheSuperHackers
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/***********************************************************************************************
 *                                                                                             *
 *  Drive the engine's own DX8Wrapper through the sequence W3DDisplay::init() performs, off     *
 *  Windows, and report the exact call it stops at.                                            *
 *                                                                                             *
 *  WHY THIS EXISTS. The renderer seam cannot be checked by building the game: the null         *
 *  RenderBackend that #87 measured on Apple Silicon linked, started, loaded 20 `.big`          *
 *  archives and opened a window before it dereferenced null at dx8wrapper.cpp:307. And the     *
 *  full game cannot be started here at all, because it needs a retail install this box does    *
 *  not have. So this is the engine's renderer path with the data-dependent engine removed:     *
 *  the same DX8Wrapper::Init, the same device enumeration, the same Set_Render_Device, and     *
 *  the same Begin_Scene/Clear/End_Scene/Present, called by a main() instead of by              *
 *  GameClient::init.                                                                          *
 *                                                                                             *
 *  It is NOT a mock. Every call below is the engine's, through the engine's archives, and the  *
 *  only thing this file supplies is the window and the argument values W3DDisplay would have   *
 *  read out of the options file. Nothing here is allowed to report success it did not get:     *
 *  each stage prints the value the engine returned, and the process exit code is the number    *
 *  of stages that failed.                                                                     *
 *                                                                                             *
 *  Run through scripts/native-render-backend-run.py, which links it against the same archives  *
 *  scripts/native-build.py links the game from.                                                *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#include "dx8wrapper.h"
#include "ww3d.h"

#include "Common/CriticalSection.h"
#include "Common/GameMemory.h"

#include "platform/platform_window.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
int main()
{
	std::printf("native_render_run: Windows has TheD3D8RenderBackend and the retail game to "
		"exercise it; this harness is the off-Windows substitute and is not built here.\n");
	return 0;
}
#else

#include "vulkanrenderbackend.h"

namespace
{

// W3DDisplay's defaults, which is what the game asks for before the options file is read.
const int WIDTH_POINTS = 800;
const int HEIGHT_POINTS = 600;
const int BIT_DEPTH = 32;
const int FRAMES = 3;
// The clear colour, kept here because the frame proof below has to compare against the same value
// the engine was asked to clear with rather than against a colour written twice.
const float CLEAR_R = 0.10f;
const float CLEAR_G = 0.20f;
const float CLEAR_B = 0.45f;

int Failures = 0;

// main()'s prologue in GeneralsMD/Code/Main/PlatformMain.cpp, which every allocation in the engine
// -- and, because the engine replaces the global operator new, every allocation the Vulkan driver
// makes on this thread -- depends on having run.
CriticalSection AsciiStringSection;
CriticalSection UnicodeStringSection;
CriticalSection DmaSection;
CriticalSection MemoryPoolSection;
CriticalSection DebugLogSection;

void Engine_Prologue()
{
	TheAsciiStringCriticalSection = &AsciiStringSection;
	TheUnicodeStringCriticalSection = &UnicodeStringSection;
	TheDmaCriticalSection = &DmaSection;
	TheMemoryPoolCriticalSection = &MemoryPoolSection;
	TheDebugLogCriticalSection = &DebugLogSection;
	initMemoryManager();
}

void Stage(const char * name, bool ok, const char * detail = NULL)
{
	if (!ok) Failures++;
	std::printf("%-42s %s%s%s\n", name, ok ? "ok" : "FAILED",
		detail != NULL ? "  " : "", detail != NULL ? detail : "");
	std::fflush(stdout);
}

}	// namespace

int main(int argc, char ** argv)
{
	bool present = true;
	bool stop_after_init = false;
	const char * frame_png = NULL;
	for (int index = 1; index < argc; index++) {
		if (strcmp(argv[index], "--no-present") == 0) present = false;
		if (strcmp(argv[index], "--stop-after-init") == 0) stop_after_init = true;
		// Where to write the frame the engine drew, so the picture can be looked at as well as
		// measured.
		if (strcmp(argv[index], "--frame-png") == 0 && index + 1 < argc) {
			frame_png = argv[++index];
		}
	}

	Engine_Prologue();

	std::printf("== the window (points; the renderer converts at its own boundary)\n");
	WWPlatform::WindowConfig config;
	config.Title = "Zero Hour native render run";
	config.Width = WIDTH_POINTS;
	config.Height = HEIGHT_POINTS;
	void * window = WWPlatform::Window_Create(config);
	Stage("WWPlatform::Window_Create", window != NULL,
		window != NULL ? NULL : WWPlatform::Window_Last_Error());
	if (window == NULL) return 1;

	int client_width = 0;
	int client_height = 0;
	WWPlatform::Window_Client_Size(window, client_width, client_height);
	std::printf("client size: %dx%d points\n", client_width, client_height);
	WWPlatform::Window_Show(window, true);
	WWPlatform::Window_Pump(window);

	std::printf("\n== DX8Wrapper::Init -> RenderBackend->Open() -> Enumerate_Devices()\n");
	// This is the call that dereferenced null in #87. It is the whole point of the slice.
	const bool initted = DX8Wrapper::Init(window, false);
	Stage("DX8Wrapper::Init", initted);
	if (!initted) {
		WWPlatform::Window_Destroy(window);
		return 1;
	}

	// Through WW3D, which is how W3DDisplay::init asks: DX8Wrapper's device enumeration and
	// device selection are protected and WW3D is their friend, so these two forwarders are the
	// engine's own route to them. WW3D::Init() itself is not called, because the rest of what it
	// does -- the dazzle INI, the animated-sound manager -- needs the retail install this box has
	// not got, and DX8Wrapper::Init() above is the part of it this slice is about.
	const int devices = WW3D::Get_Render_Device_Count();
	std::printf("render devices: %d\n", devices);
	for (int index = 0; index < devices; index++) {
		std::printf("  %d: %s\n", index, WW3D::Get_Render_Device_Name(index));
	}
	Stage("WW3D::Get_Render_Device_Count > 0", devices > 0);

	std::printf("\n== WW3D::Set_Render_Device -> CreateDevice + one-time inits\n");
	// Where this stops today, measured: CreateDevice succeeds, and then
	// DX8Wrapper::Do_Onetime_Device_Dependent_Inits() -> MissingTexture::_Init() ->
	// DX8Wrapper::_Create_DX8_Texture() -> D3DXCreateTexture(_Get_D3D_Device8(), ...) hands the
	// D3DX helper the null device that _Get_D3D_Device8() returns off Windows, gets
	// D3DERR_INVALIDCALL and a null texture, and dereferences it. The next seam is a device-shaped
	// facade over this backend for the D3DX helpers; see docs/porting/renderer-integration.md.
	// --stop-after-init stops here so the ledger below is printed instead.
	bool device = false;
	if (stop_after_init) {
		std::printf("(--stop-after-init: not creating a device)\n");
	} else if (devices > 0) {
		device = WW3D::Set_Render_Device(0, WIDTH_POINTS, HEIGHT_POINTS, BIT_DEPTH,
			1 /* windowed */, false /* resize_window */) == WW3D_ERROR_OK;
	}
	if (!stop_after_init) Stage("WW3D::Set_Render_Device", device);

	if (device) {
		std::printf("\n== %d frames of Begin_Scene / Clear / End_Scene%s\n", FRAMES,
			present ? " with a flip" : " without a flip");
		const unsigned long frames_before = DX8Wrapper::Get_FrameCount();
		for (int frame = 0; frame < FRAMES; frame++) {
			WWPlatform::Window_Pump(window);
			DX8Wrapper::Begin_Scene();
			// A colour that is not black, so a window that presents nothing is distinguishable
			// from a window that presented a cleared frame.
			DX8Wrapper::Clear(true, true, Vector3(CLEAR_R, CLEAR_G, CLEAR_B), 0.0f, 1.0f, 0);
			DX8Wrapper::End_Scene(present);
			std::printf("frame %d: submitted\n", frame);
			std::fflush(stdout);
		}
		Stage("frames submitted", true);

		// Not "and flipped", printed because the loop ran: DX8Wrapper::End_Scene advances
		// FrameCount only when Present() SUCCEEDED and sets IsDeviceLost when it did not, so
		// these two are the engine's own answer to "did the flip happen". A backend with no
		// swapchain fails Present, which lands here rather than passing silently.
		char detail[128];
		const unsigned long flipped = DX8Wrapper::Get_FrameCount() - frames_before;
		std::snprintf(detail, sizeof(detail), "(FrameCount advanced %lu, device lost %s)",
			flipped, DX8Wrapper::Is_Device_Lost() ? "yes" : "no");
		if (present) {
			Stage("frames actually presented", flipped == (unsigned long)FRAMES &&
				!DX8Wrapper::Is_Device_Lost(), detail);
		} else {
			// Without a flip nothing should have been presented, and claiming otherwise would be
			// the same defect from the other direction.
			Stage("no frame presented without a flip", flipped == 0, detail);
		}

		// What is IN the frame, read back rather than assumed. A Present that succeeded proves a
		// swapchain, not a picture.
		VulkanRenderBackendClass::FrameProofClass proof;
		const unsigned char expect_r = (unsigned char)(CLEAR_R * 255.0f + 0.5f);
		const unsigned char expect_g = (unsigned char)(CLEAR_G * 255.0f + 0.5f);
		const unsigned char expect_b = (unsigned char)(CLEAR_B * 255.0f + 0.5f);
		std::printf("\n== what was in the frame (read back from the colour target)\n");
		const bool measured = TheVulkanRenderBackend.Measure_Frame(expect_r, expect_g, expect_b,
			2 /* tolerance */, frame_png, proof);
		if (measured) {
			std::printf("%ux%u, %lu/%lu pixels within 2 of the clear colour %u,%u,%u\n",
				proof.Width, proof.Height, proof.Matching, proof.Pixels, expect_r, expect_g,
				expect_b);
			std::printf("centre pixel rgba = %u,%u,%u,%u; channel range r %u..%u g %u..%u "
				"b %u..%u\n", proof.CentreRGBA[0], proof.CentreRGBA[1], proof.CentreRGBA[2],
				proof.CentreRGBA[3], proof.MinRGB[0], proof.MaxRGB[0], proof.MinRGB[1],
				proof.MaxRGB[1], proof.MinRGB[2], proof.MaxRGB[2]);
			if (frame_png != NULL) std::printf("wrote %s\n", frame_png);
		}
		// The engine cleared every pixel, so every pixel must be the clear colour: a partial
		// match would mean something else was in the target, which is a finding either way.
		Stage("frame contents are the engine's clear colour",
			measured && proof.Pixels > 0 && proof.Matching == proof.Pixels);
	}

	std::printf("\n== the unimplemented-call ledger (each entry is a finding, not a fallback)\n");
	const unsigned kinds = VulkanRenderBackendClass::Unimplemented_Call_Kinds();
	if (kinds == 0) {
		std::printf("(empty: every D3D8 entry point the engine reached is implemented)\n");
	}
	for (unsigned index = 0; index < kinds; index++) {
		const VulkanRenderBackendClass::UnimplementedCallClass * call =
			VulkanRenderBackendClass::Unimplemented_Call(index);
		if (call == NULL) continue;
		std::printf("%6u x  %s\n            %s\n", call->Count, call->Name, call->Why);
	}

	// The layer's silence only means something if the layer was there: -1 says no device, and any
	// positive count is a finding about the Vulkan the seam emits.
	const long validation_messages = TheVulkanRenderBackend.Validation_Message_Count();
	std::printf("\nvalidation messages: %ld%s\n", validation_messages,
		validation_messages < 0 ? " (no device: the layer was never asked anything)" : "");
	if (device) Stage("validation layer silent", validation_messages == 0);

	std::printf("\n== shutdown\n");
	DX8Wrapper::Shutdown();
	Stage("DX8Wrapper::Shutdown", true);
	WWPlatform::Window_Destroy(window);

	std::printf("\nstages failed: %d\n", Failures);
	return Failures == 0 ? 0 : 1;
}

#endif	// _WIN32
