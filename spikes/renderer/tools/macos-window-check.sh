#!/usr/bin/env bash
#
#>> macOS window + CAMetalLayer + MoltenVK presentation check.
#>>
#>> The window/event-loop/input seam
#>> (Core/Libraries/Source/WWVegas/WWLib/platform/platform_window.h) has two backends: an SDL2
#>> one, built and run on Linux against a real X11 window, and a native Cocoa/CAMetalLayer one
#>> (platform_window_cocoa.mm), which PR #32 wrote on a Linux box with no macOS SDK. The Cocoa
#>> backend now compiles in CI on a macos-15 runner, and the CAMetalLayer -> VkSurfaceKHR ->
#>> swapchain -> present path is asserted there; what CI cannot do is put a window on a screen.
#>> That is what this script is for.
#>>
#>> What it does, in order, so that a failure tells you how far the path got:
#>>
#>>   1. checks the toolchain, finds MoltenVK/the Vulkan loader and exports the loader
#>>      environment (a Homebrew molten-vk is keg-only: without VK_ICD_FILENAMES the loader has
#>>      no driver at all);
#>>   2. runs the scan-code table check (pure Python: the set-1 column against KeyScanCodes.h -
#>>      the kVK_* column is checked by the compiler, see WWLIB_COCOA_VERIFY_KVK);
#>>   3. configures and builds the Cocoa backend and the Metal surface probe;
#>>   4. runs zh-metal-surface-probe: no window, so this works over ssh and in CI. It resolves
#>>      vkCreateMetalSurfaceEXT the way the seam does, makes a VkSurfaceKHR from a CAMetalLayer,
#>>      creates a swapchain and presents;
#>>   5. runs zh-window-spike-cocoa, which additionally creates an NSWindow, pumps AppKit
#>>      events, presents 240 frames and checks the read-back pixels. This is the step that
#>>      needs a login session with a display;
#>>   6. runs zh-hidpi-tests-cocoa --window, which is the full-resolution check: it asserts that
#>>      the colour target, the viewport, the scissor and the read-back followed this display's
#>>      backingScaleFactor, and that the coverage edge is one pixel wide rather than smeared by
#>>      an upscale. It FAILS on a scale-1 display (--min-scale 2.0) because a pass there would
#>>      say nothing about Retina;
#>>   7. runs zh-window-spike (the SDL2 backend, if usable) for comparison, so that a failure
#>>      can be attributed to the Cocoa backend rather than to the Mac, the driver or the
#>>      renderer.
#>>
#>> Requirements: Xcode command line tools, CMake 3.20+, glslangValidator or glslc, and a Vulkan
#>> loader with MoltenVK (the LunarG macOS SDK, or `brew install molten-vk vulkan-headers
#>> vulkan-loader glslang`). SDL2 (`brew install sdl2`) is optional and only used for step 7;
#>> note that Homebrew's SDL2 has shipped x86-only headers on arm64 images, which is what
#>> --no-sdl2 is for.
#>>
#>> Usage, from anywhere in the repo:
#>>
#>>   spikes/renderer/tools/macos-window-check.sh                    # build and run everything
#>>   spikes/renderer/tools/macos-window-check.sh --interactive      # keep the window up until
#>>                                                                  # you close it, so the
#>>                                                                  # keyboard/mouse mapping
#>>                                                                  # can be tried by hand
#>>   spikes/renderer/tools/macos-window-check.sh --allow-no-display  # what CI runs: the
#>>                                                                  # NSWindow step is reported
#>>                                                                  # but does not fail the run
#>>   spikes/renderer/tools/macos-window-check.sh --no-sdl2           # skip the SDL2 control
#>>
#>> It exits 0 only if every required step passed, and prints a single summary line either way.
#>> Without --allow-no-display, a real window has to become visible for a pass to mean anything:
#>> run it on a login session with a display, NOT over ssh. See
#>> docs/porting/window-event-loop.md.

set -u -o pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
spike_dir="$(cd "${script_dir}/.." && pwd)"
repo_root="$(cd "${spike_dir}/../.." && pwd)"
build_dir="${BUILD_DIR:-${spike_dir}/build-macos-window}"

interactive=0
allow_no_display=0
use_sdl2=1
for arg in "$@"; do
	case "${arg}" in
		--interactive) interactive=1 ;;
		--allow-no-display) allow_no_display=1 ;;
		--no-sdl2) use_sdl2=0 ;;
		--help)
			sed -n '/^#>>/ s/^#>>[[:space:]]\{0,1\}//p' "${BASH_SOURCE[0]}"
			exit 0
			;;
		*)
			echo "unknown argument: ${arg}" >&2
			exit 2
			;;
	esac
done

failures=0
step() { printf '\n=== %s\n' "$1"; }
pass() { printf 'PASS   %s\n' "$1"; }
fail() { printf 'FAIL   %s\n' "$1"; failures=$((failures + 1)); }
skip() { printf 'SKIP   %s\n' "$1"; }

step "environment"
uname -srm
if [[ "$(uname -s)" != "Darwin" ]]; then
	fail "this check only means anything on macOS; got $(uname -s)"
	echo
	echo "SUMMARY: not macOS, nothing was verified"
	exit 1
fi
pass "running on macOS $(sw_vers -productVersion) $(uname -m)"

if xcrun --find clang++ >/dev/null 2>&1; then
	pass "Objective-C++ toolchain: $(xcrun --find clang++)"
else
	fail "no clang++ from xcrun; install the Xcode command line tools"
fi
if command -v cmake >/dev/null 2>&1; then
	pass "cmake: $(cmake --version | head -1)"
else
	fail "cmake not on PATH"
fi
if command -v glslangValidator >/dev/null 2>&1 || command -v glslc >/dev/null 2>&1; then
	pass "shader compiler present"
else
	fail "need glslangValidator or glslc on PATH"
fi

# The loader is what dlsym(RTLD_DEFAULT, "vkGetInstanceProcAddr") in the Cocoa backend resolves
# against, and MoltenVK's ICD manifest is what tells that loader a driver exists. Homebrew's
# molten-vk is keg-only and has moved the manifest between releases, so it is located rather
# than assumed - without this the run fails with VK_ERROR_INCOMPATIBLE_DRIVER and the Cocoa
# backend gets blamed for it.
brew_prefix=""
if command -v brew >/dev/null 2>&1; then
	brew_prefix="$(brew --prefix)"
fi
if [[ -n "${VULKAN_SDK:-}" ]]; then
	pass "VULKAN_SDK=${VULKAN_SDK}"
elif [[ -n "${brew_prefix}" && -e "${brew_prefix}/lib/libvulkan.dylib" ]]; then
	pass "Vulkan loader: ${brew_prefix}/lib/libvulkan.dylib"
elif [[ -e /usr/local/lib/libvulkan.dylib || -e /opt/homebrew/lib/libvulkan.dylib ]]; then
	pass "Vulkan loader found under a Homebrew prefix"
else
	skip "no VULKAN_SDK and no libvulkan.dylib in the usual places; the build may still \
find one through CMake"
fi

if [[ -z "${VK_ICD_FILENAMES:-}" ]] && command -v brew >/dev/null 2>&1 &&
   brew --prefix molten-vk >/dev/null 2>&1; then
	found_icd="$(find "$(brew --prefix molten-vk)/" -name 'MoltenVK_icd.json' 2>/dev/null | head -1)"
	if [[ -n "${found_icd}" ]]; then
		export VK_ICD_FILENAMES="${found_icd}"
		# Homebrew's manifests name their dylibs relatively, so the loader needs the search
		# path as well as the manifest.
		export DYLD_LIBRARY_PATH="$(brew --prefix molten-vk)/lib:${brew_prefix}/lib${DYLD_LIBRARY_PATH:+:${DYLD_LIBRARY_PATH}}"
	fi
fi
if [[ -n "${VK_ICD_FILENAMES:-}" ]]; then
	pass "VK_ICD_FILENAMES=${VK_ICD_FILENAMES}"
else
	skip "VK_ICD_FILENAMES unset and no Homebrew molten-vk; the loader must find MoltenVK itself"
fi

step "scan-code tables (KeyScanCodes.h agreement)"
if python3 "${repo_root}/scripts/ci/check-window-scancodes.py"; then
	pass "both backends' scan-code tables agree with KeyScanCodes.h"
else
	fail "scan-code tables disagree with KeyScanCodes.h"
fi

step "configure"
configure_args=(-S "${spike_dir}" -B "${build_dir}" -DCMAKE_BUILD_TYPE=Release)
if [[ "${use_sdl2}" == "0" ]]; then
	configure_args+=(-DSPIKE_USE_SDL2=OFF)
fi
if [[ -n "${brew_prefix}" ]]; then
	configure_args+=(-DCMAKE_PREFIX_PATH="${brew_prefix}")
fi
if cmake "${configure_args[@]}"; then
	pass "cmake configure"
else
	fail "cmake configure"
	echo
	echo "SUMMARY: ${failures} step(s) failed; nothing about the Cocoa path was learned"
	exit 1
fi

step "build the Cocoa backend and the Metal surface probe"
# platform_window_cocoa.mm compiles with WWLIB_COCOA_VERIFY_KVK here, so this step also proves
# its 104 kVK_* literals against HIToolbox and its VkMetalSurfaceCreateInfoEXT mirror against
# the MoltenVK headers.
if cmake --build "${build_dir}" --target zh-window-spike-cocoa zh-metal-surface-probe; then
	pass "platform_window_cocoa.mm compiles and links"
else
	fail "platform_window_cocoa.mm does not build"
	echo
	echo "SUMMARY: ${failures} step(s) failed; the Cocoa backend does not compile"
	exit 1
fi

step "CAMetalLayer -> vkCreateMetalSurfaceEXT -> swapchain -> present (no window needed)"
if "${build_dir}/zh-metal-surface-probe"; then
	pass "VK_EXT_metal_surface is advertised and a surface, swapchain and present all worked"
else
	fail "the Metal surface path failed (see its PASS/FAIL lines above)"
fi

step "run: NSWindow + CAMetalLayer + vkCreateMetalSurfaceEXT + present"
run_args=(--out "${build_dir}/macos-window-check.png")
if [[ "${interactive}" == "1" ]]; then
	run_args+=(--interactive)
	echo "the window will stay up until you close it or press Escape; try the arrow keys,"
	echo "the mouse buttons and the scroll wheel, and check the scan codes it prints"
else
	run_args+=(--frames 240 --mode-change)
fi
window_ran=0
if "${build_dir}/zh-window-spike-cocoa" "${run_args[@]}"; then
	window_ran=1
	pass "the Cocoa backend presented frames to an NSWindow's CAMetalLayer through MoltenVK"
elif [[ "${allow_no_display}" == "1" ]]; then
	# AppKit needs a windowing session. On a CI runner or over ssh there may be none, and
	# that says nothing about the code, so it is reported and not counted.
	skip "zh-window-spike-cocoa failed; --allow-no-display, so this is not counted. The \
NSWindow path remains unverified"
else
	fail "zh-window-spike-cocoa reported failures (see its PASS/FAIL lines above)"
fi

step "is the render full resolution on this display? (Retina, backing scale 2)"
# The half-resolution defect (docs/porting/apple-silicon-verification.md 8.4,
# docs/porting/hidpi-scale.md): the colour target used to be sized in points while the swapchain
# was in pixels, so the game rendered a quarter of a Retina panel and the presentation blit
# upscaled it. Linux CI runs the same assertions headless at an injected scale, which is honest but
# is not this display: only a real NSWindow reports a real backingScaleFactor. This step needs the
# Retina panel, and it FAILS rather than skips on a scale-1 display, because a pass there would
# mean nothing.
if cmake --build "${build_dir}" --target zh-hidpi-tests-cocoa; then
	hidpi_args=(--window --min-scale 2.0)
	if "${build_dir}/zh-hidpi-tests-cocoa" "${hidpi_args[@]}"; then
		pass "the colour target, the viewport, the scissor and the read-back are the panel's pixels"
	elif [[ "${allow_no_display}" == "1" ]]; then
		skip "zh-hidpi-tests-cocoa needs a windowing session with a Retina display; \
--allow-no-display, so this is not counted and the full-resolution claim is UNVERIFIED"
	else
		fail "the render is not full resolution on this display (see its PASS/FAIL lines above)"
	fi
else
	fail "zh-hidpi-tests-cocoa does not build"
fi

step "control: the same driver on the SDL2 backend"
if [[ "${use_sdl2}" == "0" ]]; then
	skip "--no-sdl2 - no control run"
elif [[ -x "${build_dir}/zh-window-spike" ]] || \
   cmake --build "${build_dir}" --target zh-window-spike >/dev/null 2>&1; then
	if "${build_dir}/zh-window-spike" --frames 240 \
	       --out "${build_dir}/macos-window-check-sdl2.png"; then
		pass "SDL2 backend also presents on this machine"
	elif [[ "${allow_no_display}" == "1" ]]; then
		skip "SDL2 backend also fails without a display, which is expected"
	else
		fail "SDL2 backend fails too, so the problem is not specific to the Cocoa backend"
	fi
else
	skip "SDL2 not installed or not usable (brew install sdl2) - no control run"
fi

step "summary"
if [[ "${failures}" == "0" ]]; then
	if [[ "${window_ran}" == "0" ]]; then
		echo "SUMMARY: PASS - everything that does not need a windowing session passed. The"
		echo "NSWindow path is NOT covered by this run; re-run on a Mac with a login session."
		exit 0
	fi
	if [[ "${allow_no_display}" == "1" ]]; then
		echo "SUMMARY: PASS - including the NSWindow path. Note that --allow-no-display was"
		echo "given, so nothing here proves anything was visible on a screen: window ordering,"
		echo "a Retina contentsScale of 2 and real keyboard/mouse input are still uncovered."
		exit 0
	fi
	echo "SUMMARY: PASS - a real window presented MoltenVK frames through a CAMetalLayer."
	echo "If you watched it happen on a screen, and tried the keyboard with --interactive,"
	echo "record that in docs/porting/window-event-loop.md: those are the last two things in"
	echo "that document that no machine has been able to check."
	exit 0
fi
echo "SUMMARY: FAIL - ${failures} step(s) failed. See the lines above for how far it got."
exit 1
