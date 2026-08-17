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
 *  Cocoa implementation of the window/event-loop/input seam: NSWindow + CAMetalLayer, with     *
 *  the Vulkan surface created through VK_EXT_metal_surface so MoltenVK presents straight to    *
 *  the layer.                                                                                  *
 *                                                                                             *
 *  This file is COMPILED AND RUN IN CI on a macos-15 arm64 runner (the `window-seam-macos` job  *
 *  in .github/workflows/native-port-ci.yml): it creates an NSWindow, gets a VkSurfaceKHR from   *
 *  its CAMetalLayer through vkCreateMetalSurfaceEXT, presents 240 frames through MoltenVK and   *
 *  reads them back. The runner has no display and nobody types on it, so what remains          *
 *  unverified is anything visible (window ordering, a Retina contentsScale of 2), the           *
 *  kVK_* table against a real keypress, mouse translation and cursor clipping. The list is in   *
 *  docs/porting/window-event-loop.md section 4.                                                *
 *                                                                                             *
 *  Design notes that are *not* guesses, because they come from Apple's and MoltenVK's          *
 *  documented contracts rather than from having run this:                                       *
 *                                                                                             *
 *   * MoltenVK presents to a CAMetalLayer. VK_EXT_metal_surface's                              *
 *     VkMetalSurfaceCreateInfoEXT takes `const CAMetalLayer *pLayer` directly, which is why    *
 *     the layer - not the NSWindow, not the NSView - is the handle this file has to own.        *
 *     (The older VK_MVK_macos_surface takes the NSView instead and is deprecated.)             *
 *   * AppKit requires the NSApplication to exist and to be on the process's main thread before  *
 *     any window is created, and requires event pumping to happen on that same thread.         *
 *   * The layer's drawableSize is in pixels while the window's frame is in points, so a Retina  *
 *     display needs the contentsScale applied or the swapchain comes out at half resolution.    *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#include "platform_window.h"

#if defined(__APPLE__) && !defined(_WIN32)

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
// CGAssociateMouseAndMouseCursorPosition and CGWarpMouseCursorPosition live in
// CoreGraphics/CGRemoteOperation.h, which AppKit does not promise to pull in.
#import <CoreGraphics/CoreGraphics.h>

#include <cstddef>
#include <deque>
#include <dlfcn.h>
#include <string>

/*
**	The Metal surface plumbing below prefers the real VK_EXT_metal_surface declarations from
**	the Vulkan/MoltenVK headers and falls back to the local mirror when this translation unit
**	is built without them - which is the WWLib case, since WWLib must not require a Vulkan SDK
**	to compile. When both are present the mirror is checked against the real thing at compile
**	time (see the static_asserts further down), so the fallback cannot silently drift.
*/
#if defined(__has_include)
#	if __has_include(<vulkan/vulkan.h>)
#		define WWLIB_COCOA_HAVE_VULKAN_HEADERS 1
#	endif
#endif

/*
**	The kVK_* column of the scan-code table below is written as literals so that Carbon is not
**	dragged into the WWLib build. Define WWLIB_COCOA_VERIFY_KVK (the spike and the CI job do)
**	to have every one of those literals checked against <Carbon/Carbon.h>'s real constant at
**	compile time; scripts/ci/check-window-scancodes.py checks the other column, against
**	KeyScanCodes.h.
*/
#if defined(WWLIB_COCOA_VERIFY_KVK) && defined(__has_include)
#	if __has_include(<Carbon/Carbon.h>)
#		include <Carbon/Carbon.h>
#		define WWLIB_COCOA_KVK_CHECKED 1
#	else
#		error "WWLIB_COCOA_VERIFY_KVK was requested but <Carbon/Carbon.h> is not available"
#	endif
#endif

#ifdef WWLIB_COCOA_HAVE_VULKAN_HEADERS
#	ifndef VK_USE_PLATFORM_METAL_EXT
#		define VK_USE_PLATFORM_METAL_EXT 1
#	endif
#	include <vulkan/vulkan.h>
#endif

namespace WWPlatform
{

namespace
{

/*
**	macOS virtual key code -> PC/AT set-1 scan code (the KEYSCAN_* values in
**	Core/GameEngine/Include/GameClient/KeyScanCodes.h). The Cocoa side is a *virtual* key code
**	in the HIToolbox kVK_* numbering, which - unlike Windows' VK_* - is positional, so this is
**	still a physical-key relabelling and not layout dependent.
**
**	The kVK_* values are written as literals with their names alongside rather than included
**	from <Carbon/HIToolbox/Events.h>, so that this file does not drag Carbon into the build.
**	scripts/ci/check-window-scancodes.py checks the set-1 column against KeyScanCodes.h; the
**	kVK_ column is checked in the standalone macOS check, since only a Mac has the header.
*/
struct ScanCodeMapping
{
	unsigned short Virtual_Key;
	int Set1;
	const char * Name;
};

const ScanCodeMapping SCAN_CODE_TABLE[] = {
	{0x35, 0x01, "KEYSCAN_ESCAPE"},			// kVK_Escape
	{0x12, 0x02, "KEYSCAN_1"},				// kVK_ANSI_1
	{0x13, 0x03, "KEYSCAN_2"},
	{0x14, 0x04, "KEYSCAN_3"},
	{0x15, 0x05, "KEYSCAN_4"},
	{0x17, 0x06, "KEYSCAN_5"},
	{0x16, 0x07, "KEYSCAN_6"},
	{0x1A, 0x08, "KEYSCAN_7"},
	{0x1C, 0x09, "KEYSCAN_8"},
	{0x19, 0x0A, "KEYSCAN_9"},
	{0x1D, 0x0B, "KEYSCAN_0"},
	{0x1B, 0x0C, "KEYSCAN_MINUS"},			// kVK_ANSI_Minus
	{0x18, 0x0D, "KEYSCAN_EQUALS"},			// kVK_ANSI_Equal
	{0x33, 0x0E, "KEYSCAN_BACK"},			// kVK_Delete, which is backspace
	{0x30, 0x0F, "KEYSCAN_TAB"},
	{0x0C, 0x10, "KEYSCAN_Q"},
	{0x0D, 0x11, "KEYSCAN_W"},
	{0x0E, 0x12, "KEYSCAN_E"},
	{0x0F, 0x13, "KEYSCAN_R"},
	{0x11, 0x14, "KEYSCAN_T"},
	{0x10, 0x15, "KEYSCAN_Y"},
	{0x20, 0x16, "KEYSCAN_U"},
	{0x22, 0x17, "KEYSCAN_I"},
	{0x1F, 0x18, "KEYSCAN_O"},
	{0x23, 0x19, "KEYSCAN_P"},
	{0x21, 0x1A, "KEYSCAN_LBRACKET"},
	{0x1E, 0x1B, "KEYSCAN_RBRACKET"},
	{0x24, 0x1C, "KEYSCAN_RETURN"},
	{0x3B, 0x1D, "KEYSCAN_LCONTROL"},		// kVK_Control
	{0x00, 0x1E, "KEYSCAN_A"},
	{0x01, 0x1F, "KEYSCAN_S"},
	{0x02, 0x20, "KEYSCAN_D"},
	{0x03, 0x21, "KEYSCAN_F"},
	{0x05, 0x22, "KEYSCAN_G"},
	{0x04, 0x23, "KEYSCAN_H"},
	{0x26, 0x24, "KEYSCAN_J"},
	{0x28, 0x25, "KEYSCAN_K"},
	{0x25, 0x26, "KEYSCAN_L"},
	{0x29, 0x27, "KEYSCAN_SEMICOLON"},
	{0x27, 0x28, "KEYSCAN_APOSTROPHE"},		// kVK_ANSI_Quote
	{0x32, 0x29, "KEYSCAN_GRAVE"},
	{0x38, 0x2A, "KEYSCAN_LSHIFT"},			// kVK_Shift
	{0x2A, 0x2B, "KEYSCAN_BACKSLASH"},
	{0x06, 0x2C, "KEYSCAN_Z"},
	{0x07, 0x2D, "KEYSCAN_X"},
	{0x08, 0x2E, "KEYSCAN_C"},
	{0x09, 0x2F, "KEYSCAN_V"},
	{0x0B, 0x30, "KEYSCAN_B"},
	{0x2D, 0x31, "KEYSCAN_N"},
	{0x2E, 0x32, "KEYSCAN_M"},
	{0x2B, 0x33, "KEYSCAN_COMMA"},
	{0x2F, 0x34, "KEYSCAN_PERIOD"},
	{0x2C, 0x35, "KEYSCAN_SLASH"},
	{0x3C, 0x36, "KEYSCAN_RSHIFT"},			// kVK_RightShift
	{0x43, 0x37, "KEYSCAN_NUMPADSTAR"},		// kVK_ANSI_KeypadMultiply
	{0x3A, 0x38, "KEYSCAN_LALT"},			// kVK_Option
	{0x31, 0x39, "KEYSCAN_SPACE"},
	{0x39, 0x3A, "KEYSCAN_CAPSLOCK"},
	{0x7A, 0x3B, "KEYSCAN_F1"},
	{0x78, 0x3C, "KEYSCAN_F2"},
	{0x63, 0x3D, "KEYSCAN_F3"},
	{0x76, 0x3E, "KEYSCAN_F4"},
	{0x60, 0x3F, "KEYSCAN_F5"},
	{0x61, 0x40, "KEYSCAN_F6"},
	{0x62, 0x41, "KEYSCAN_F7"},
	{0x64, 0x42, "KEYSCAN_F8"},
	{0x65, 0x43, "KEYSCAN_F9"},
	{0x6D, 0x44, "KEYSCAN_F10"},
	{0x47, 0x45, "KEYSCAN_NUMLOCK"},		// kVK_ANSI_KeypadClear; macOS has no Num Lock
	{0x6B, 0x46, "KEYSCAN_SCROLL"},			// kVK_F14, which is where Scroll Lock sits
	{0x59, 0x47, "KEYSCAN_NUMPAD7"},
	{0x5B, 0x48, "KEYSCAN_NUMPAD8"},
	{0x5C, 0x49, "KEYSCAN_NUMPAD9"},
	{0x4E, 0x4A, "KEYSCAN_NUMPADMINUS"},
	{0x56, 0x4B, "KEYSCAN_NUMPAD4"},
	{0x57, 0x4C, "KEYSCAN_NUMPAD5"},
	{0x58, 0x4D, "KEYSCAN_NUMPAD6"},
	{0x45, 0x4E, "KEYSCAN_NUMPADPLUS"},
	{0x53, 0x4F, "KEYSCAN_NUMPAD1"},
	{0x54, 0x50, "KEYSCAN_NUMPAD2"},
	{0x55, 0x51, "KEYSCAN_NUMPAD3"},
	{0x52, 0x52, "KEYSCAN_NUMPAD0"},
	{0x41, 0x53, "KEYSCAN_NUMPADPERIOD"},
	{0x0A, 0x56, "KEYSCAN_OEM_102"},		// kVK_ISO_Section
	{0x67, 0x57, "KEYSCAN_F11"},
	{0x6F, 0x58, "KEYSCAN_F12"},
	{0x68, 0x70, "KEYSCAN_KANA"},			// kVK_JIS_Kana
	{0x5D, 0x7D, "KEYSCAN_YEN"},			// kVK_JIS_Yen
	{0x66, 0x94, "KEYSCAN_KANJI"},			// kVK_JIS_Eisu
	{0x4C, 0x9C, "KEYSCAN_NUMPADENTER"},	// kVK_ANSI_KeypadEnter
	{0x3E, 0x9D, "KEYSCAN_RCONTROL"},		// kVK_RightControl
	{0x4B, 0xB5, "KEYSCAN_NUMPADSLASH"},
	{0x69, 0xB7, "KEYSCAN_SYSRQ"},			// kVK_F13, which is where Print Screen sits
	{0x3D, 0xB8, "KEYSCAN_RALT"},			// kVK_RightOption
	{0x73, 0xC7, "KEYSCAN_HOME"},
	{0x7E, 0xC8, "KEYSCAN_UPARROW"},
	{0x74, 0xC9, "KEYSCAN_PGUP"},
	{0x7B, 0xCB, "KEYSCAN_LEFTARROW"},
	{0x7C, 0xCD, "KEYSCAN_RIGHTARROW"},
	{0x77, 0xCF, "KEYSCAN_END"},
	{0x7D, 0xD0, "KEYSCAN_DOWNARROW"},
	{0x79, 0xD1, "KEYSCAN_PGDN"},
	{0x72, 0xD2, "KEYSCAN_INSERT"},			// kVK_Help, the Insert position on a PC keyboard
	{0x75, 0xD3, "KEYSCAN_DELETE"},			// kVK_ForwardDelete
};

const int SCAN_CODE_COUNT =
	static_cast<int>(sizeof(SCAN_CODE_TABLE) / sizeof(SCAN_CODE_TABLE[0]));

// A JIS keyboard's non-ANSI keys beyond Kana/Yen/Eisu, and the PC keys macOS has no key for
// at all (kVK has no Num Lock, no Pause, no Menu), are absent above rather than guessed at.
// KEYSCAN_CONVERT, KEYSCAN_NOCONVERT and KEYSCAN_CIRCUMFLEX are the three the table does not
// produce; the doc lists them as a known gap.

#ifdef WWLIB_COCOA_KVK_CHECKED
/*
**	Every kVK_* literal in the table above, checked against HIToolbox. This is the half of the
**	keyboard mapping a Linux box cannot check: the set-1 column is checked by
**	scripts/ci/check-window-scancodes.py, and these 104 lines are the other column.
*/
#define WWLIB_CHECK_KVK(literal, name) \
	static_assert((literal) == (name), #literal " is not " #name)
WWLIB_CHECK_KVK(0x35, kVK_Escape);		// KEYSCAN_ESCAPE
WWLIB_CHECK_KVK(0x12, kVK_ANSI_1);		// KEYSCAN_1
WWLIB_CHECK_KVK(0x13, kVK_ANSI_2);		// KEYSCAN_2
WWLIB_CHECK_KVK(0x14, kVK_ANSI_3);		// KEYSCAN_3
WWLIB_CHECK_KVK(0x15, kVK_ANSI_4);		// KEYSCAN_4
WWLIB_CHECK_KVK(0x17, kVK_ANSI_5);		// KEYSCAN_5
WWLIB_CHECK_KVK(0x16, kVK_ANSI_6);		// KEYSCAN_6
WWLIB_CHECK_KVK(0x1A, kVK_ANSI_7);		// KEYSCAN_7
WWLIB_CHECK_KVK(0x1C, kVK_ANSI_8);		// KEYSCAN_8
WWLIB_CHECK_KVK(0x19, kVK_ANSI_9);		// KEYSCAN_9
WWLIB_CHECK_KVK(0x1D, kVK_ANSI_0);		// KEYSCAN_0
WWLIB_CHECK_KVK(0x1B, kVK_ANSI_Minus);		// KEYSCAN_MINUS
WWLIB_CHECK_KVK(0x18, kVK_ANSI_Equal);		// KEYSCAN_EQUALS
WWLIB_CHECK_KVK(0x33, kVK_Delete);		// KEYSCAN_BACK
WWLIB_CHECK_KVK(0x30, kVK_Tab);		// KEYSCAN_TAB
WWLIB_CHECK_KVK(0x0C, kVK_ANSI_Q);		// KEYSCAN_Q
WWLIB_CHECK_KVK(0x0D, kVK_ANSI_W);		// KEYSCAN_W
WWLIB_CHECK_KVK(0x0E, kVK_ANSI_E);		// KEYSCAN_E
WWLIB_CHECK_KVK(0x0F, kVK_ANSI_R);		// KEYSCAN_R
WWLIB_CHECK_KVK(0x11, kVK_ANSI_T);		// KEYSCAN_T
WWLIB_CHECK_KVK(0x10, kVK_ANSI_Y);		// KEYSCAN_Y
WWLIB_CHECK_KVK(0x20, kVK_ANSI_U);		// KEYSCAN_U
WWLIB_CHECK_KVK(0x22, kVK_ANSI_I);		// KEYSCAN_I
WWLIB_CHECK_KVK(0x1F, kVK_ANSI_O);		// KEYSCAN_O
WWLIB_CHECK_KVK(0x23, kVK_ANSI_P);		// KEYSCAN_P
WWLIB_CHECK_KVK(0x21, kVK_ANSI_LeftBracket);		// KEYSCAN_LBRACKET
WWLIB_CHECK_KVK(0x1E, kVK_ANSI_RightBracket);		// KEYSCAN_RBRACKET
WWLIB_CHECK_KVK(0x24, kVK_Return);		// KEYSCAN_RETURN
WWLIB_CHECK_KVK(0x3B, kVK_Control);		// KEYSCAN_LCONTROL
WWLIB_CHECK_KVK(0x00, kVK_ANSI_A);		// KEYSCAN_A
WWLIB_CHECK_KVK(0x01, kVK_ANSI_S);		// KEYSCAN_S
WWLIB_CHECK_KVK(0x02, kVK_ANSI_D);		// KEYSCAN_D
WWLIB_CHECK_KVK(0x03, kVK_ANSI_F);		// KEYSCAN_F
WWLIB_CHECK_KVK(0x05, kVK_ANSI_G);		// KEYSCAN_G
WWLIB_CHECK_KVK(0x04, kVK_ANSI_H);		// KEYSCAN_H
WWLIB_CHECK_KVK(0x26, kVK_ANSI_J);		// KEYSCAN_J
WWLIB_CHECK_KVK(0x28, kVK_ANSI_K);		// KEYSCAN_K
WWLIB_CHECK_KVK(0x25, kVK_ANSI_L);		// KEYSCAN_L
WWLIB_CHECK_KVK(0x29, kVK_ANSI_Semicolon);		// KEYSCAN_SEMICOLON
WWLIB_CHECK_KVK(0x27, kVK_ANSI_Quote);		// KEYSCAN_APOSTROPHE
WWLIB_CHECK_KVK(0x32, kVK_ANSI_Grave);		// KEYSCAN_GRAVE
WWLIB_CHECK_KVK(0x38, kVK_Shift);		// KEYSCAN_LSHIFT
WWLIB_CHECK_KVK(0x2A, kVK_ANSI_Backslash);		// KEYSCAN_BACKSLASH
WWLIB_CHECK_KVK(0x06, kVK_ANSI_Z);		// KEYSCAN_Z
WWLIB_CHECK_KVK(0x07, kVK_ANSI_X);		// KEYSCAN_X
WWLIB_CHECK_KVK(0x08, kVK_ANSI_C);		// KEYSCAN_C
WWLIB_CHECK_KVK(0x09, kVK_ANSI_V);		// KEYSCAN_V
WWLIB_CHECK_KVK(0x0B, kVK_ANSI_B);		// KEYSCAN_B
WWLIB_CHECK_KVK(0x2D, kVK_ANSI_N);		// KEYSCAN_N
WWLIB_CHECK_KVK(0x2E, kVK_ANSI_M);		// KEYSCAN_M
WWLIB_CHECK_KVK(0x2B, kVK_ANSI_Comma);		// KEYSCAN_COMMA
WWLIB_CHECK_KVK(0x2F, kVK_ANSI_Period);		// KEYSCAN_PERIOD
WWLIB_CHECK_KVK(0x2C, kVK_ANSI_Slash);		// KEYSCAN_SLASH
WWLIB_CHECK_KVK(0x3C, kVK_RightShift);		// KEYSCAN_RSHIFT
WWLIB_CHECK_KVK(0x43, kVK_ANSI_KeypadMultiply);		// KEYSCAN_NUMPADSTAR
WWLIB_CHECK_KVK(0x3A, kVK_Option);		// KEYSCAN_LALT
WWLIB_CHECK_KVK(0x31, kVK_Space);		// KEYSCAN_SPACE
WWLIB_CHECK_KVK(0x39, kVK_CapsLock);		// KEYSCAN_CAPSLOCK
WWLIB_CHECK_KVK(0x7A, kVK_F1);		// KEYSCAN_F1
WWLIB_CHECK_KVK(0x78, kVK_F2);		// KEYSCAN_F2
WWLIB_CHECK_KVK(0x63, kVK_F3);		// KEYSCAN_F3
WWLIB_CHECK_KVK(0x76, kVK_F4);		// KEYSCAN_F4
WWLIB_CHECK_KVK(0x60, kVK_F5);		// KEYSCAN_F5
WWLIB_CHECK_KVK(0x61, kVK_F6);		// KEYSCAN_F6
WWLIB_CHECK_KVK(0x62, kVK_F7);		// KEYSCAN_F7
WWLIB_CHECK_KVK(0x64, kVK_F8);		// KEYSCAN_F8
WWLIB_CHECK_KVK(0x65, kVK_F9);		// KEYSCAN_F9
WWLIB_CHECK_KVK(0x6D, kVK_F10);		// KEYSCAN_F10
WWLIB_CHECK_KVK(0x47, kVK_ANSI_KeypadClear);		// KEYSCAN_NUMLOCK
WWLIB_CHECK_KVK(0x6B, kVK_F14);		// KEYSCAN_SCROLL
WWLIB_CHECK_KVK(0x59, kVK_ANSI_Keypad7);		// KEYSCAN_NUMPAD7
WWLIB_CHECK_KVK(0x5B, kVK_ANSI_Keypad8);		// KEYSCAN_NUMPAD8
WWLIB_CHECK_KVK(0x5C, kVK_ANSI_Keypad9);		// KEYSCAN_NUMPAD9
WWLIB_CHECK_KVK(0x4E, kVK_ANSI_KeypadMinus);		// KEYSCAN_NUMPADMINUS
WWLIB_CHECK_KVK(0x56, kVK_ANSI_Keypad4);		// KEYSCAN_NUMPAD4
WWLIB_CHECK_KVK(0x57, kVK_ANSI_Keypad5);		// KEYSCAN_NUMPAD5
WWLIB_CHECK_KVK(0x58, kVK_ANSI_Keypad6);		// KEYSCAN_NUMPAD6
WWLIB_CHECK_KVK(0x45, kVK_ANSI_KeypadPlus);		// KEYSCAN_NUMPADPLUS
WWLIB_CHECK_KVK(0x53, kVK_ANSI_Keypad1);		// KEYSCAN_NUMPAD1
WWLIB_CHECK_KVK(0x54, kVK_ANSI_Keypad2);		// KEYSCAN_NUMPAD2
WWLIB_CHECK_KVK(0x55, kVK_ANSI_Keypad3);		// KEYSCAN_NUMPAD3
WWLIB_CHECK_KVK(0x52, kVK_ANSI_Keypad0);		// KEYSCAN_NUMPAD0
WWLIB_CHECK_KVK(0x41, kVK_ANSI_KeypadDecimal);		// KEYSCAN_NUMPADPERIOD
WWLIB_CHECK_KVK(0x0A, kVK_ISO_Section);		// KEYSCAN_OEM_102
WWLIB_CHECK_KVK(0x67, kVK_F11);		// KEYSCAN_F11
WWLIB_CHECK_KVK(0x6F, kVK_F12);		// KEYSCAN_F12
WWLIB_CHECK_KVK(0x68, kVK_JIS_Kana);		// KEYSCAN_KANA
WWLIB_CHECK_KVK(0x5D, kVK_JIS_Yen);		// KEYSCAN_YEN
WWLIB_CHECK_KVK(0x66, kVK_JIS_Eisu);		// KEYSCAN_KANJI
WWLIB_CHECK_KVK(0x4C, kVK_ANSI_KeypadEnter);		// KEYSCAN_NUMPADENTER
WWLIB_CHECK_KVK(0x3E, kVK_RightControl);		// KEYSCAN_RCONTROL
WWLIB_CHECK_KVK(0x4B, kVK_ANSI_KeypadDivide);		// KEYSCAN_NUMPADSLASH
WWLIB_CHECK_KVK(0x69, kVK_F13);		// KEYSCAN_SYSRQ
WWLIB_CHECK_KVK(0x3D, kVK_RightOption);		// KEYSCAN_RALT
WWLIB_CHECK_KVK(0x73, kVK_Home);		// KEYSCAN_HOME
WWLIB_CHECK_KVK(0x7E, kVK_UpArrow);		// KEYSCAN_UPARROW
WWLIB_CHECK_KVK(0x74, kVK_PageUp);		// KEYSCAN_PGUP
WWLIB_CHECK_KVK(0x7B, kVK_LeftArrow);		// KEYSCAN_LEFTARROW
WWLIB_CHECK_KVK(0x7C, kVK_RightArrow);		// KEYSCAN_RIGHTARROW
WWLIB_CHECK_KVK(0x77, kVK_End);		// KEYSCAN_END
WWLIB_CHECK_KVK(0x7D, kVK_DownArrow);		// KEYSCAN_DOWNARROW
WWLIB_CHECK_KVK(0x79, kVK_PageDown);		// KEYSCAN_PGDN
WWLIB_CHECK_KVK(0x72, kVK_Help);		// KEYSCAN_INSERT
WWLIB_CHECK_KVK(0x75, kVK_ForwardDelete);		// KEYSCAN_DELETE
#undef WWLIB_CHECK_KVK
#endif // WWLIB_COCOA_KVK_CHECKED

int Set1_From_Virtual_Key(unsigned short key)
{
	for (int i = 0; i < SCAN_CODE_COUNT; ++i) {
		if (SCAN_CODE_TABLE[i].Virtual_Key == key) return SCAN_CODE_TABLE[i].Set1;
	}
	return 0;
}

unsigned int Modifiers_From_Cocoa(NSEventModifierFlags flags)
{
	unsigned int out = WINDOW_MODIFIER_NONE;
	if ((flags & NSEventModifierFlagShift) != 0) out |= WINDOW_MODIFIER_SHIFT;
	if ((flags & NSEventModifierFlagControl) != 0) out |= WINDOW_MODIFIER_CONTROL;
	// Option is what the game's Alt bindings have to be, since Command belongs to the OS.
	if ((flags & NSEventModifierFlagOption) != 0) out |= WINDOW_MODIFIER_ALT;
	if ((flags & NSEventModifierFlagCapsLock) != 0) out |= WINDOW_MODIFIER_CAPS_LOCK;
	return out;
}

/*
**	VK_EXT_metal_surface. This file cannot *require* the Vulkan headers - WWLib has no Vulkan
**	dependency and must compile without an SDK - so the extension is mirrored locally, and the
**	mirror is checked against the real declarations whenever the headers happen to be
**	available (the spike and the CI job both build with them). The entry point is resolved
**	through vkGetInstanceProcAddr, found with dlsym() in whatever loader the process already
**	has (libvulkan.dylib, or MoltenVK linked directly), so there is still no link-time
**	dependency on the loader either way.
*/
typedef void * VkInstanceOpaque;
typedef unsigned long long VkSurfaceHandle;
typedef void (*PFN_vkVoidFunction_Local)(void);
typedef PFN_vkVoidFunction_Local (*PFN_vkGetInstanceProcAddr_Local)(VkInstanceOpaque,
                                                                    const char *);

struct VkMetalSurfaceCreateInfoEXT_Local
{
	int sType;						// VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT
	const void * pNext;
	unsigned int flags;
	const void * pLayer;			// const CAMetalLayer *
};

const int VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT_Local = 1000217000;

typedef int (*PFN_vkCreateMetalSurfaceEXT_Local)(VkInstanceOpaque,
                                                 const VkMetalSurfaceCreateInfoEXT_Local *,
                                                 const void *, VkSurfaceHandle *);

#ifdef WWLIB_COCOA_HAVE_VULKAN_HEADERS
/*
**	The mirror against the real thing. Every one of these was a guess when this file was
**	written on Linux; each static_assert is one guess turned into a compile-time fact on any
**	machine that has the MoltenVK/Vulkan headers, including the CI runner.
*/
static_assert(VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT_Local ==
                  static_cast<int>(VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT),
              "the mirrored sType is not VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT");
static_assert(sizeof(VkMetalSurfaceCreateInfoEXT_Local) == sizeof(VkMetalSurfaceCreateInfoEXT),
              "VkMetalSurfaceCreateInfoEXT mirror is the wrong size");
static_assert(offsetof(VkMetalSurfaceCreateInfoEXT_Local, sType) ==
                  offsetof(VkMetalSurfaceCreateInfoEXT, sType),
              "VkMetalSurfaceCreateInfoEXT mirror: sType at the wrong offset");
static_assert(offsetof(VkMetalSurfaceCreateInfoEXT_Local, pNext) ==
                  offsetof(VkMetalSurfaceCreateInfoEXT, pNext),
              "VkMetalSurfaceCreateInfoEXT mirror: pNext at the wrong offset");
static_assert(offsetof(VkMetalSurfaceCreateInfoEXT_Local, flags) ==
                  offsetof(VkMetalSurfaceCreateInfoEXT, flags),
              "VkMetalSurfaceCreateInfoEXT mirror: flags at the wrong offset");
static_assert(offsetof(VkMetalSurfaceCreateInfoEXT_Local, pLayer) ==
                  offsetof(VkMetalSurfaceCreateInfoEXT, pLayer),
              "VkMetalSurfaceCreateInfoEXT mirror: pLayer at the wrong offset");
static_assert(sizeof(VkSurfaceHandle) == sizeof(VkSurfaceKHR),
              "VkSurfaceKHR is not 64 bits wide here, so the out parameter is the wrong size");
static_assert(sizeof(VkInstanceOpaque) == sizeof(VkInstance),
              "VkInstance is not pointer-sized here");
static_assert(sizeof(PFN_vkCreateMetalSurfaceEXT_Local) == sizeof(PFN_vkCreateMetalSurfaceEXT),
              "the mirrored entry-point type is not pointer-sized");
#endif

struct WindowState
{
	NSWindow * Window;
	NSView * View;
	CAMetalLayer * Layer;
	std::deque<WindowEvent> Queue;
	NSEventModifierFlags Last_Modifier_Flags;
	bool Key_Down[256];
	bool Active;
	bool Minimised;
	bool Closed;
	bool Cursor_Clipped;
	bool Resizable;
	int Width;
	int Height;

	WindowState()
		: Window(nil), View(nil), Layer(nil), Last_Modifier_Flags(0), Active(false),
		  Minimised(false), Closed(false), Cursor_Clipped(false), Resizable(false), Width(0),
		  Height(0)
	{
		for (int i = 0; i < 256; ++i) Key_Down[i] = false;
	}
};

WindowState * TheWindow = nullptr;
std::string TheLastError;

WindowState * State(void * window)
{
	if (window == nullptr || window != TheWindow) return nullptr;
	return TheWindow;
}

unsigned int Now_Ms()
{
	// Same clock the events are stamped with: NSEvent.timestamp is seconds since boot.
	return static_cast<unsigned int>([NSProcessInfo processInfo].systemUptime * 1000.0);
}

/*
**	Cocoa's origin is bottom-left and the engine's, like Win32's, is top-left, so every mouse
**	coordinate is flipped here rather than at the call sites.
*/
void Mouse_Position_In_Client(WindowState * state, NSEvent * event, int & x, int & y)
{
	NSPoint in_window = [event locationInWindow];
	// An event with no window - which is what AppKit delivers whenever the pointer is outside
	// every window of this process - carries screen coordinates instead, so converting it as a
	// window point offsets the result by the window's origin.
	if ([event window] == nil) {
		const NSRect on_screen = NSMakeRect(in_window.x, in_window.y, 1.0, 1.0);
		in_window = [state->Window convertRectFromScreen:on_screen].origin;
	}
	NSPoint point = [state->View convertPoint:in_window fromView:nil];
	NSRect bounds = [state->View bounds];
	x = static_cast<int>(point.x);
	y = static_cast<int>(bounds.size.height - point.y);
}

/*
**	Cocoa's screen coordinates have their origin at the bottom-left of the PRIMARY screen -- the
**	one with the menu bar, [[NSScreen screens] firstObject] -- and every conversion to the seam's
**	top-left origin flips against its height. +mainScreen is a different screen: it is whichever
**	one has keyboard focus, so on a two-monitor Mac with the focus on the secondary display it
**	gives a different height and every converted y is wrong by the difference. Single-monitor
**	hardware, which is what CI and most testing has, cannot tell the two apart.
*/
CGFloat Primary_Screen_Height()
{
	NSScreen * primary = [[NSScreen screens] firstObject];
	if (primary == nil) return 0.0;
	return [primary frame].size.height;
}

/*
**	The engine's fullscreen is a screen-sized WS_POPUP, which on macOS is a borderless window -
**	but a borderless window is still an ordinary layer-0 window, and the menu bar (level 24), the
**	Control Center items (25) and the Dock (20) all composite above it. Raising the level alone
**	still leaves the Dock's and the menu bar's space reserved, so the presentation options are
**	what actually gets them out of the way.
*/
void Apply_Fullscreen(NSWindow * window)
{
	[NSApp setPresentationOptions:NSApplicationPresentationHideDock |
	                              NSApplicationPresentationHideMenuBar];
	[window setLevel:NSMainMenuWindowLevel + 1];
	[window setFrame:[[NSScreen mainScreen] frame] display:YES];
}

void Push(WindowState * state, const WindowEvent & event)
{
	state->Queue.push_back(event);
}

void Translate_Text(WindowState * state, NSEvent * event, unsigned int time_ms)
{
	NSString * characters = [event characters];
	for (NSUInteger i = 0; i < [characters length]; ++i) {
		unichar unit = [characters characterAtIndex:i];
		// Skip the private-use area AppKit puts the function/arrow keys in: those are
		// delivered as key events, not as text.
		if (unit >= 0xF700 && unit <= 0xF8FF) continue;
		if (unit < 0x20 && unit != '\r' && unit != '\t' && unit != 0x08) continue;
		WindowEvent out;
		out.Type = WINDOW_EVENT_TEXT;
		out.Character = unit;
		out.Modifiers = Modifiers_From_Cocoa([event modifierFlags]);
		out.Time_Ms = time_ms;
		Push(state, out);
	}
}

void Translate_Flags_Changed(WindowState * state, NSEvent * event, unsigned int time_ms)
{
	// Cocoa does not send key down/up for the modifier keys; it sends one flagsChanged whose
	// keyCode names the key that moved. Whether it went down or up has to be recovered from
	// the new flag set.
	const NSEventModifierFlags flags = [event modifierFlags];
	const unsigned short key = [event keyCode];
	const int set1 = Set1_From_Virtual_Key(key);
	if (set1 == 0) return;

	NSEventModifierFlags mask = 0;
	switch (key) {
		case 0x38: case 0x3C: mask = NSEventModifierFlagShift; break;		// Shift
		case 0x3B: case 0x3E: mask = NSEventModifierFlagControl; break;		// Control
		case 0x3A: case 0x3D: mask = NSEventModifierFlagOption; break;		// Option
		case 0x39: mask = NSEventModifierFlagCapsLock; break;
		default: return;
	}
	const bool down = (flags & mask) != 0;
	// With both Shifts held, releasing one leaves the flag set, so the per-key state is
	// tracked rather than derived from the flag alone.
	if (state->Key_Down[set1 & 0xFF] == down && mask != NSEventModifierFlagCapsLock) return;
	state->Key_Down[set1 & 0xFF] = down;
	state->Last_Modifier_Flags = flags;

	WindowEvent out;
	out.Type = down ? WINDOW_EVENT_KEY_DOWN : WINDOW_EVENT_KEY_UP;
	out.Scan_Code = set1;
	out.Modifiers = Modifiers_From_Cocoa(flags);
	out.Time_Ms = time_ms;
	Push(state, out);
}

void Translate(WindowState * state, NSEvent * event)
{
	const unsigned int time_ms = static_cast<unsigned int>([event timestamp] * 1000.0);
	WindowEvent out;
	out.Time_Ms = time_ms;

	switch ([event type]) {
		case NSEventTypeKeyDown: {
			const int set1 = Set1_From_Virtual_Key([event keyCode]);
			if (set1 != 0) {
				state->Key_Down[set1 & 0xFF] = true;
				out.Type = WINDOW_EVENT_KEY_DOWN;
				out.Scan_Code = set1;
				out.Repeat = [event isARepeat] ? true : false;
				out.Modifiers = Modifiers_From_Cocoa([event modifierFlags]);
				Push(state, out);
			}
			Translate_Text(state, event, time_ms);
			return;
		}

		case NSEventTypeKeyUp: {
			const int set1 = Set1_From_Virtual_Key([event keyCode]);
			if (set1 == 0) return;
			state->Key_Down[set1 & 0xFF] = false;
			out.Type = WINDOW_EVENT_KEY_UP;
			out.Scan_Code = set1;
			out.Modifiers = Modifiers_From_Cocoa([event modifierFlags]);
			Push(state, out);
			return;
		}

		case NSEventTypeFlagsChanged:
			Translate_Flags_Changed(state, event, time_ms);
			return;

		case NSEventTypeMouseMoved:
		case NSEventTypeLeftMouseDragged:
		case NSEventTypeRightMouseDragged:
		case NSEventTypeOtherMouseDragged:
			out.Type = WINDOW_EVENT_MOUSE_MOVE;
			Mouse_Position_In_Client(state, event, out.Mouse_X, out.Mouse_Y);
			out.Modifiers = Modifiers_From_Cocoa([event modifierFlags]);
			Push(state, out);
			return;

		case NSEventTypeLeftMouseDown:
		case NSEventTypeRightMouseDown:
		case NSEventTypeOtherMouseDown:
		case NSEventTypeLeftMouseUp:
		case NSEventTypeRightMouseUp:
		case NSEventTypeOtherMouseUp: {
			const NSEventType type = [event type];
			const bool down = type == NSEventTypeLeftMouseDown ||
			                  type == NSEventTypeRightMouseDown ||
			                  type == NSEventTypeOtherMouseDown;
			out.Type = down ? WINDOW_EVENT_MOUSE_DOWN : WINDOW_EVENT_MOUSE_UP;
			if (type == NSEventTypeRightMouseDown || type == NSEventTypeRightMouseUp) {
				out.Mouse_Button = MOUSE_BUTTON_RIGHT;
			} else if (type == NSEventTypeOtherMouseDown || type == NSEventTypeOtherMouseUp) {
				out.Mouse_Button = MOUSE_BUTTON_MIDDLE;
			} else {
				out.Mouse_Button = MOUSE_BUTTON_LEFT;
			}
			out.Click_Count = static_cast<int>([event clickCount]);
			Mouse_Position_In_Client(state, event, out.Mouse_X, out.Mouse_Y);
			out.Modifiers = Modifiers_From_Cocoa([event modifierFlags]);
			Push(state, out);
			return;
		}

		case NSEventTypeScrollWheel:
			out.Type = WINDOW_EVENT_MOUSE_WHEEL;
			Mouse_Position_In_Client(state, event, out.Mouse_X, out.Mouse_Y);
			// scrollingDeltaY is in lines for a wheel and in points for a trackpad; either
			// way one wheel notch is scaled to WM_MOUSEWHEEL's WHEEL_DELTA of 120.
			out.Wheel_Delta = static_cast<int>([event scrollingDeltaY] * 120.0);
			out.Modifiers = Modifiers_From_Cocoa([event modifierFlags]);
			if (out.Wheel_Delta != 0) Push(state, out);
			return;

		default:
			return;
	}
}

void Sync_Window_State(WindowState * state)
{
	const bool active = [state->Window isKeyWindow] ? true : false;
	if (active != state->Active) {
		state->Active = active;
		WindowEvent out;
		out.Type = active ? WINDOW_EVENT_FOCUS_GAINED : WINDOW_EVENT_FOCUS_LOST;
		out.Time_Ms = Now_Ms();
		Push(state, out);
	}
	const bool minimised = [state->Window isMiniaturized] ? true : false;
	if (minimised != state->Minimised) {
		state->Minimised = minimised;
		WindowEvent out;
		out.Type = minimised ? WINDOW_EVENT_MINIMISED : WINDOW_EVENT_RESTORED;
		out.Time_Ms = Now_Ms();
		Push(state, out);
	}
	if (![state->Window isVisible] && !state->Closed) {
		// The close button removes the window rather than sending anything this loop can
		// see, so a vanished window is reported as WINDOW_EVENT_CLOSE exactly once.
		state->Closed = true;
		WindowEvent out;
		out.Type = WINDOW_EVENT_CLOSE;
		out.Time_Ms = Now_Ms();
		Push(state, out);
	}
	NSRect bounds = [state->View bounds];
	const int width = static_cast<int>(bounds.size.width);
	const int height = static_cast<int>(bounds.size.height);
	if (width != state->Width || height != state->Height) {
		state->Width = width;
		state->Height = height;
		const CGFloat scale = [state->Window backingScaleFactor];
		state->Layer.drawableSize = CGSizeMake(width * scale, height * scale);
		WindowEvent out;
		out.Type = WINDOW_EVENT_RESIZE;
		out.Width = width;
		out.Height = height;
		out.Time_Ms = Now_Ms();
		Push(state, out);
	}
}

} // namespace

void * Window_Create(const WindowConfig & config)
{
	if (TheWindow != nullptr) {
		TheLastError = "a window already exists; the seam supports one window";
		return nullptr;
	}
	if (![NSThread isMainThread]) {
		TheLastError = "Window_Create must be called on the main thread (AppKit requirement)";
		return nullptr;
	}

	// A process that was not launched from a .app bundle has no NSApplication and no
	// activation policy, and a window created without both never becomes key.
	[NSApplication sharedApplication];
	[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
	[NSApp finishLaunching];

	const NSRect frame = NSMakeRect(0, 0, config.Width, config.Height);
	NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
	                          NSWindowStyleMaskMiniaturizable;
	if (config.Resizable) style |= NSWindowStyleMaskResizable;
	if (config.Fullscreen) style = NSWindowStyleMaskBorderless;

	NSWindow * window = [[NSWindow alloc] initWithContentRect:frame
	                                               styleMask:style
	                                                 backing:NSBackingStoreBuffered
	                                                   defer:NO];
	if (window == nil) {
		TheLastError = "NSWindow initWithContentRect returned nil";
		return nullptr;
	}
	[window setTitle:[NSString stringWithUTF8String:
	                      config.Title != nullptr ? config.Title : ""]];
	[window setAcceptsMouseMovedEvents:YES];
	[window setRestorable:NO];
	[window center];

	NSView * view = [[NSView alloc] initWithFrame:frame];
	CAMetalLayer * layer = [CAMetalLayer layer];
	if (layer == nil) {
		TheLastError = "CAMetalLayer layer returned nil (no Metal device?)";
		return nullptr;
	}
	id<MTLDevice> metal_device = MTLCreateSystemDefaultDevice();
	if (metal_device == nil) {
		TheLastError = "MTLCreateSystemDefaultDevice() returned nil: no Metal device";
		return nullptr;
	}
	layer.device = metal_device;
	layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
	const CGFloat scale = [window backingScaleFactor];
	layer.contentsScale = scale;
	layer.drawableSize = CGSizeMake(config.Width * scale, config.Height * scale);
	// NO, not YES: the swapchain the renderer asks MoltenVK for includes
	// VK_IMAGE_USAGE_TRANSFER_DST_BIT, and a framebufferOnly layer's drawable textures cannot
	// be the target of a blit or a readback.
	layer.framebufferOnly = NO;
	// A layer-hosting view: setLayer has to come before setWantsLayer, or AppKit takes the
	// view to be layer-*backed* and replaces the layer with one of its own.
	[view setLayer:layer];
	[view setWantsLayer:YES];
	[window setContentView:view];
	[window makeFirstResponder:view];

	if (config.Fullscreen) {
		Apply_Fullscreen(window);
	}

	TheWindow = new WindowState();
	TheWindow->Window = window;
	TheWindow->View = view;
	TheWindow->Layer = layer;
	TheWindow->Width = config.Width;
	TheWindow->Height = config.Height;
	TheWindow->Resizable = config.Resizable;

	[window makeKeyAndOrderFront:nil];
	[NSApp activateIgnoringOtherApps:YES];
	if (config.Hide_System_Cursor) [NSCursor hide];

	TheLastError.clear();
	return TheWindow;
}

void Window_Destroy(void * window)
{
	WindowState * state = State(window);
	if (state == nullptr) return;
	// Otherwise a process that exits from fullscreen leaves the Dock and the menu bar hidden.
	[NSApp setPresentationOptions:NSApplicationPresentationDefault];
	[state->Window orderOut:nil];
	[state->Window close];
	delete state;
	TheWindow = nullptr;
}

void Window_Pump(void * window)
{
	WindowState * state = State(window);
	if (state == nullptr) return;
	// distantPast, so this never blocks: the game loop, not AppKit, owns the frame rate.
	// This is the direct replacement for PeekMessage/GetMessage/DispatchMessage, and unlike
	// [NSApp run] it hands control straight back.
	for (;;) {
		NSEvent * event = [NSApp nextEventMatchingMask:NSEventMaskAny
		                                     untilDate:[NSDate distantPast]
		                                        inMode:NSDefaultRunLoopMode
		                                       dequeue:YES];
		if (event == nil) break;
		Translate(state, event);
		// Still forwarded to AppKit, so the title bar, the menu and Cmd-Q keep working.
		[NSApp sendEvent:event];
	}
	Sync_Window_State(state);
}

bool Window_Poll_Event(void * window, WindowEvent & event)
{
	WindowState * state = State(window);
	if (state == nullptr) return false;
	Window_Pump(window);
	if (state->Queue.empty()) return false;
	event = state->Queue.front();
	state->Queue.pop_front();
	return true;
}

bool Window_Is_Minimised(void * window)
{
	WindowState * state = State(window);
	return state != nullptr && state->Minimised;
}

bool Window_Is_Active(void * window)
{
	WindowState * state = State(window);
	return state != nullptr && state->Active;
}

void Window_Client_Size(void * window, int & width, int & height)
{
	WindowState * state = State(window);
	if (state == nullptr) {
		width = 0;
		height = 0;
		return;
	}
	NSRect bounds = [state->View bounds];
	width = static_cast<int>(bounds.size.width);
	height = static_cast<int>(bounds.size.height);
}

float Window_Backing_Scale(void * window)
{
	WindowState * state = State(window);
	if (state == nullptr) return 1.0f;
	/*
	**	The window's own factor, not the main screen's: a window on a Retina display and a
	**	window on an external 1x display report different numbers, and dragging one between
	**	them changes this while the client size in points stays put.
	*/
	const CGFloat scale = [state->Window backingScaleFactor];
	if (!(scale > 0.0)) return 1.0f;
	return static_cast<float>(scale);
}

bool Window_Set_Mode(void * window, int width, int height, bool fullscreen)
{
	WindowState * state = State(window);
	if (state == nullptr) return false;
	if (fullscreen) {
		[state->Window setStyleMask:NSWindowStyleMaskBorderless];
		Apply_Fullscreen(state->Window);
	} else {
		NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
		                          NSWindowStyleMaskMiniaturizable;
		if (state->Resizable) style |= NSWindowStyleMaskResizable;
		[state->Window setStyleMask:style];
		[NSApp setPresentationOptions:NSApplicationPresentationDefault];
		[state->Window setLevel:NSNormalWindowLevel];
		[state->Window setContentSize:NSMakeSize(width, height)];
		[state->Window center];
	}
	NSRect bounds = [state->View bounds];
	const CGFloat scale = [state->Window backingScaleFactor];
	state->Layer.drawableSize =
		CGSizeMake(bounds.size.width * scale, bounds.size.height * scale);
	return true;
}

bool Window_Frame_Insets(void * window, int & left, int & top, int & right, int & bottom)
{
	WindowState * state = State(window);
	if (state == nullptr) return false;

	left = 0;
	top = 0;
	right = 0;
	bottom = 0;

	// A borderless window has no frame; -frameRectForContentRect: agrees, but asking is pointless
	// and the answer for a borderless style mask is not worth trusting mid-transition.
	if (([state->Window styleMask] & NSWindowStyleMaskTitled) == 0) return true;

	// AppKit's own arithmetic for "how much bigger is the frame than the content", which is where
	// the title bar's height comes from without hard-coding 22 points. Points, because both rects
	// are in the window's coordinate space; -convertRectToBacking: would be the pixel version.
	const NSRect content = NSMakeRect(0.0, 0.0, 200.0, 100.0);
	const NSRect frame = [state->Window frameRectForContentRect:content];
	left = static_cast<int>(content.origin.x - frame.origin.x);
	bottom = static_cast<int>(content.origin.y - frame.origin.y);
	right = static_cast<int>(frame.size.width - content.size.width - (content.origin.x -
		frame.origin.x));
	top = static_cast<int>(frame.size.height - content.size.height - (content.origin.y -
		frame.origin.y));
	return true;
}

bool Window_Is_Fullscreen(void * window)
{
	WindowState * state = State(window);
	if (state == nullptr) return false;
	// Window_Set_Mode() goes borderless-and-covering rather than using AppKit's own fullscreen
	// transition, so borderless is what "fullscreen" looks like here; the native flag is checked
	// too, because the user can take the window fullscreen from the title bar.
	if (([state->Window styleMask] & NSWindowStyleMaskFullScreen) != 0) return true;
	return ([state->Window styleMask] & NSWindowStyleMaskTitled) == 0;
}

bool Window_Set_Position(void * window, int x, int y)
{
	WindowState * state = State(window);
	if (state == nullptr) return false;

	// The caller means the client area's top-left in top-left-origin screen points, and
	// -setFrameTopLeftPoint: takes the *frame's* top-left in bottom-left-origin ones.
	int left = 0;
	int top = 0;
	int right = 0;
	int bottom = 0;
	if (!Window_Frame_Insets(window, left, top, right, bottom)) return false;

	const CGFloat frame_left = static_cast<CGFloat>(x - left);
	const CGFloat frame_top_flipped = static_cast<CGFloat>(y - top);
	[state->Window setFrameTopLeftPoint:
		NSMakePoint(frame_left, Primary_Screen_Height() - frame_top_flipped)];
	return true;
}

bool Window_Set_Client_Size(void * window, int width, int height)
{
	WindowState * state = State(window);
	if (state == nullptr || width <= 0 || height <= 0) return false;
	[state->Window setContentSize:NSMakeSize(width, height)];
	// The drawable is in pixels and the content size is in points, so the backing scale is
	// applied here, at the renderer boundary, exactly as Window_Set_Mode() does it.
	const NSRect bounds = [state->View bounds];
	const CGFloat scale = [state->Window backingScaleFactor];
	state->Layer.drawableSize =
		CGSizeMake(bounds.size.width * scale, bounds.size.height * scale);
	return true;
}

void Window_Set_Always_On_Top(void * window, bool on_top)
{
	WindowState * state = State(window);
	if (state == nullptr) return;
	[state->Window setLevel:(on_top ? NSFloatingWindowLevel : NSNormalWindowLevel)];
}

int Window_Display_Count()
{
	return static_cast<int>([[NSScreen screens] count]);
}

int Window_Display_For_Window(void * window)
{
	WindowState * state = State(window);
	if (state == nullptr) return 0;

	NSScreen * screen = [state->Window screen];
	if (screen == nil) return 0;		// off-screen or minimised: the primary display, as
										// MONITOR_DEFAULTTOPRIMARY asks for.
	NSArray<NSScreen *> * screens = [NSScreen screens];
	const NSUInteger index = [screens indexOfObject:screen];
	return (index == NSNotFound) ? 0 : static_cast<int>(index);
}

namespace
{

bool Screen_Rect(int display, NSRect rect, int & x, int & y, int & width, int & height)
{
	(void)display;
	// Bottom-left origin to top-left, so the bottom edge is what the top becomes.
	x = static_cast<int>(rect.origin.x);
	y = static_cast<int>(Primary_Screen_Height() - (rect.origin.y + rect.size.height));
	width = static_cast<int>(rect.size.width);
	height = static_cast<int>(rect.size.height);
	return true;
}

NSScreen * Screen_At(int display)
{
	NSArray<NSScreen *> * screens = [NSScreen screens];
	if (display < 0 || display >= static_cast<int>([screens count])) return nil;
	return [screens objectAtIndex:static_cast<NSUInteger>(display)];
}

}	// anonymous namespace

bool Window_Display_Bounds(int display, int & x, int & y, int & width, int & height)
{
	NSScreen * screen = Screen_At(display);
	if (screen == nil) return false;
	return Screen_Rect(display, [screen frame], x, y, width, height);
}

bool Window_Display_Work_Area(int display, int & x, int & y, int & width, int & height)
{
	NSScreen * screen = Screen_At(display);
	if (screen == nil) return false;
	// -visibleFrame is the frame minus the menu bar and the Dock, which is what GetMonitorInfo()
	// calls rcWork and what a window is centred in.
	return Screen_Rect(display, [screen visibleFrame], x, y, width, height);
}

void Window_Show(void * window, bool show)
{
	WindowState * state = State(window);
	if (state == nullptr) return;
	if (show) {
		[state->Window makeKeyAndOrderFront:nil];
	} else {
		[state->Window orderOut:nil];
	}
}

void Window_Set_Title(void * window, const char * title)
{
	WindowState * state = State(window);
	if (state == nullptr || title == nullptr) return;
	[state->Window setTitle:[NSString stringWithUTF8String:title]];
}

void Window_Set_Cursor_Clip(void * window, bool clip)
{
	WindowState * state = State(window);
	if (state == nullptr) return;
	// macOS has no ClipCursor. The closest thing is dissociating the pointer from the mouse
	// so it cannot leave, which is what CGAssociateMouseAndMouseCursorPosition(false) does -
	// and it also stops the pointer moving at all, so the game must then draw its own.
	state->Cursor_Clipped = clip;
	CGAssociateMouseAndMouseCursorPosition(clip ? false : true);
}

void Window_Warp_Cursor(void * window, int x, int y)
{
	WindowState * state = State(window);
	if (state == nullptr) return;
	// CGWarpMouseCursorPosition is in global, top-left-origin display coordinates, so the
	// client point is converted through the window's frame rather than passed through.
	NSRect content = [state->Window convertRectToScreen:[state->View frame]];
	const CGFloat global_x = content.origin.x + x;
	const CGFloat global_y =
		Primary_Screen_Height() - (content.origin.y + content.size.height) + y;
	CGWarpMouseCursorPosition(CGPointMake(global_x, global_y));
}

void Window_Show_System_Cursor(void * window, bool show)
{
	if (State(window) == nullptr) return;
	if (show) {
		[NSCursor unhide];
	} else {
		[NSCursor hide];
	}
}

bool Window_Cursor_Position(void * window, int & x, int & y)
{
	if (State(window) == nullptr) return false;

	// [NSEvent mouseLocation] is the pointer in screen coordinates with a bottom-left origin,
	// and keeps reading while the pointer is over another application's window, which is what
	// GetCursorPos() does. The flip to a top-left origin is against the primary screen, the same
	// reference Window_Warp_Cursor() uses for the opposite conversion.
	const NSPoint location = [NSEvent mouseLocation];
	x = static_cast<int>(location.x);
	y = static_cast<int>(Primary_Screen_Height() - location.y);
	return true;
}

bool Window_Client_Origin(void * window, int & x, int & y)
{
	WindowState * state = State(window);
	if (state == nullptr) return false;

	// The view's frame converted to screen coordinates is the client area, so its top-left is
	// the corner ScreenToClient() measures from. These are points: -convertRectToScreen: works
	// in the window's coordinate space, and only -convertRectToBacking: would be in pixels.
	const NSRect content = [state->Window convertRectToScreen:[state->View frame]];
	x = static_cast<int>(content.origin.x);
	y = static_cast<int>(Primary_Screen_Height() - (content.origin.y + content.size.height));
	return true;
}

void * Window_Current()
{
	return TheWindow;
}

bool Window_Key_Is_Down(void * window, int scan_code)
{
	WindowState * state = State(window);
	if (state == nullptr || scan_code <= 0 || scan_code > 0xFF) return false;
	return state->Key_Down[scan_code];
}

unsigned int Window_Modifier_State(void * window)
{
	WindowState * state = State(window);
	if (state == nullptr) return WINDOW_MODIFIER_NONE;
	return Modifiers_From_Cocoa([NSEvent modifierFlags]);
}

int Window_Vulkan_Instance_Extensions(void * window, const char ** names, int max_names)
{
	if (State(window) == nullptr) return -1;
	// VK_EXT_metal_surface, not the deprecated VK_MVK_macos_surface: the former takes the
	// CAMetalLayer this file owns.
	static const char * const REQUIRED[] = {"VK_KHR_surface", "VK_EXT_metal_surface"};
	const int count = static_cast<int>(sizeof(REQUIRED) / sizeof(REQUIRED[0]));
	if (names == nullptr) return count;
	if (max_names < count) return -1;
	for (int i = 0; i < count; ++i) names[i] = REQUIRED[i];
	return count;
}

bool Window_Create_Vulkan_Surface(void * window, void * vk_instance, void * out_vk_surface)
{
	WindowState * state = State(window);
	if (state == nullptr || vk_instance == nullptr || out_vk_surface == nullptr) return false;

	// Real declarations when the translation unit has the Vulkan/MoltenVK headers, the local
	// mirror otherwise. The two are asserted layout-identical above, so this branch is a
	// matter of who declares the names, not of what is passed to the driver.
#ifdef WWLIB_COCOA_HAVE_VULKAN_HEADERS
	typedef PFN_vkGetInstanceProcAddr Get_Proc_Type;
	typedef PFN_vkCreateMetalSurfaceEXT Create_Type;
	typedef VkMetalSurfaceCreateInfoEXT Create_Info_Type;
	typedef VkInstance Instance_Type;
	typedef VkSurfaceKHR Surface_Type;
	const int surface_stype = static_cast<int>(VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT);
#else
	typedef PFN_vkGetInstanceProcAddr_Local Get_Proc_Type;
	typedef PFN_vkCreateMetalSurfaceEXT_Local Create_Type;
	typedef VkMetalSurfaceCreateInfoEXT_Local Create_Info_Type;
	typedef VkInstanceOpaque Instance_Type;
	typedef VkSurfaceHandle Surface_Type;
	const int surface_stype = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT_Local;
#endif

	Get_Proc_Type get_proc =
		reinterpret_cast<Get_Proc_Type>(dlsym(RTLD_DEFAULT, "vkGetInstanceProcAddr"));
	if (get_proc == nullptr) {
		TheLastError = "vkGetInstanceProcAddr not found in this process; is the Vulkan "
		               "loader or MoltenVK linked?";
		return false;
	}
	Instance_Type instance = reinterpret_cast<Instance_Type>(vk_instance);
	Create_Type create =
		reinterpret_cast<Create_Type>(get_proc(instance, "vkCreateMetalSurfaceEXT"));
	if (create == nullptr) {
		TheLastError = "vkCreateMetalSurfaceEXT unavailable; the instance must enable "
		               "VK_EXT_metal_surface";
		return false;
	}

	Create_Info_Type info = Create_Info_Type();
	info.sType = static_cast<decltype(info.sType)>(surface_stype);
	info.pNext = nullptr;
	info.flags = 0;
	info.pLayer = state->Layer;

	Surface_Type * surface = static_cast<Surface_Type *>(out_vk_surface);
	const int result = static_cast<int>(create(instance, &info, nullptr, surface));
	if (result != 0) {
		TheLastError = "vkCreateMetalSurfaceEXT failed (VkResult " + std::to_string(result) + ")";
		return false;
	}
	return true;
}

NativeSurface Window_Native_Surface(void * window)
{
	NativeSurface surface;
	WindowState * state = State(window);
	if (state == nullptr) return surface;
	surface.Kind = NATIVE_SURFACE_METAL_LAYER;
	surface.Handle_A = state->Layer;
	surface.Handle_B = state->Window;
	return surface;
}

const char * Window_Backend_Name()
{
	return "cocoa";
}

const char * Window_Last_Error()
{
	return TheLastError.c_str();
}

}	// namespace WWPlatform

#endif // __APPLE__ && !_WIN32
