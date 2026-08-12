#!/usr/bin/env bash
#
# macOS window + CAMetalLayer + MoltenVK presentation check.
#
# ##############################################################################
# ##  THE macOS CODE THIS CHECKS WAS WRITTEN BLIND AND HAS NEVER BEEN RUN.    ##
# ##  This script is how a Mac session finds out what is wrong with it.       ##
# ##############################################################################
#
# The window/event-loop/input seam
# (Core/Libraries/Source/WWVegas/WWLib/platform/platform_window.h) has two backends: an SDL2
# one, which has been built and run on Linux against a real X11 window, and a native
# Cocoa/CAMetalLayer one (platform_window_cocoa.mm), which was written on a Linux box with no
# macOS SDK and has therefore never been compiled, let alone executed. Everything this script
# reports about the Cocoa path is new information.
#
# What it does, in order, so that a failure tells you how far the path got:
#
#   1. checks the toolchain and finds MoltenVK/the Vulkan loader;
#   2. runs the scan-code table check (pure Python, no Mac needed, but cheap and it gates the
#      keyboard mapping);
#   3. configures and builds the spike, which is where a blind Objective-C++ file first meets
#      a compiler;
#   4. runs zh-window-spike-cocoa, which creates an NSWindow, puts a CAMetalLayer on it, asks
#      MoltenVK for a VkSurfaceKHR through VK_EXT_metal_surface, presents 240 frames to it,
#      reads the last one back and checks the pixels;
#   5. runs zh-window-spike (the SDL2 backend, if SDL2 is installed) for comparison, so that a
#      failure can be attributed to the Cocoa backend rather than to the Mac, the driver or
#      the renderer.
#
# Requirements: Xcode command line tools, CMake 3.20+, glslangValidator or glslc, and a Vulkan
# loader with MoltenVK (the LunarG macOS SDK, or `brew install molten-vk vulkan-headers
# vulkan-loader glslang`). SDL2 (`brew install sdl2`) is optional and only used for step 5.
#
# Usage, from anywhere in the repo:
#
#   spikes/renderer/tools/macos-window-check.sh                 # build and run everything
#   spikes/renderer/tools/macos-window-check.sh --interactive   # keep the window up until
#                                                               # you close it, so the
#                                                               # keyboard/mouse translation
#                                                               # can be tried by hand
#
# It exits 0 only if every step passed, and prints a single summary line either way. A real
# window has to become visible for a pass to mean anything: this runs on a login session with
# a display, NOT over ssh and NOT on a headless CI runner. See docs/porting/window-event-loop.md.

set -u -o pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
spike_dir="$(cd "${script_dir}/.." && pwd)"
repo_root="$(cd "${spike_dir}/../.." && pwd)"
build_dir="${BUILD_DIR:-${spike_dir}/build-macos-window}"

interactive=0
for arg in "$@"; do
	case "${arg}" in
		--interactive) interactive=1 ;;
		--help)
			sed -n '2,45p' "${BASH_SOURCE[0]}"
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

# The loader is what dlsym(RTLD_DEFAULT, "vkGetInstanceProcAddr") in the Cocoa backend
# resolves against, so its absence is the most likely cause of a step 4 failure.
if [[ -n "${VULKAN_SDK:-}" ]]; then
	pass "VULKAN_SDK=${VULKAN_SDK}"
elif [[ -e /usr/local/lib/libvulkan.dylib || -e /opt/homebrew/lib/libvulkan.dylib ]]; then
	pass "Vulkan loader found under a Homebrew prefix"
else
	skip "no VULKAN_SDK and no libvulkan.dylib in the usual places; the build may still \
find one through CMake"
fi
if [[ -n "${VK_ICD_FILENAMES:-}" ]]; then
	pass "VK_ICD_FILENAMES=${VK_ICD_FILENAMES}"
else
	skip "VK_ICD_FILENAMES unset; the loader must find MoltenVK's ICD by itself"
fi

step "scan-code tables (KeyScanCodes.h agreement)"
if python3 "${repo_root}/scripts/ci/check-window-scancodes.py"; then
	pass "both backends' scan-code tables agree with KeyScanCodes.h"
else
	fail "scan-code tables disagree with KeyScanCodes.h"
fi

step "configure"
if cmake -S "${spike_dir}" -B "${build_dir}" -DCMAKE_BUILD_TYPE=Release; then
	pass "cmake configure"
else
	fail "cmake configure"
	echo
	echo "SUMMARY: ${failures} step(s) failed; nothing about the Cocoa path was learned"
	exit 1
fi

step "build the blind Cocoa backend"
# This is the moment platform_window_cocoa.mm is compiled for the first time ever. Expect
# work here; the file is a proposal, not a tested artefact.
if cmake --build "${build_dir}" --target zh-window-spike-cocoa; then
	pass "platform_window_cocoa.mm compiles and links"
else
	fail "platform_window_cocoa.mm does not build (this is the expected first failure)"
	echo
	echo "SUMMARY: ${failures} step(s) failed; the Cocoa backend does not compile yet"
	exit 1
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
if "${build_dir}/zh-window-spike-cocoa" "${run_args[@]}"; then
	pass "the Cocoa backend presented frames to a CAMetalLayer through MoltenVK"
else
	fail "zh-window-spike-cocoa reported failures (see its PASS/FAIL lines above)"
fi

step "control: the same driver on the SDL2 backend"
if [[ -x "${build_dir}/zh-window-spike" ]] || \
   cmake --build "${build_dir}" --target zh-window-spike >/dev/null 2>&1; then
	if "${build_dir}/zh-window-spike" --frames 240 \
	       --out "${build_dir}/macos-window-check-sdl2.png"; then
		pass "SDL2 backend also presents on this machine"
	else
		fail "SDL2 backend fails too, so the problem is not specific to the Cocoa backend"
	fi
else
	skip "SDL2 not installed (brew install sdl2) - no control run"
fi

step "summary"
if [[ "${failures}" == "0" ]]; then
	echo "SUMMARY: PASS - a real window presented MoltenVK frames through a CAMetalLayer."
	echo "Please record this in docs/porting/window-event-loop.md, which currently states"
	echo "that the macOS path is unverified."
	exit 0
fi
echo "SUMMARY: FAIL - ${failures} step(s) failed. The macOS path remains unverified."
exit 1
