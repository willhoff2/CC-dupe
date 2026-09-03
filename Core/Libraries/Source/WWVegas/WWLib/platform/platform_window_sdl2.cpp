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
 *  SDL2 implementation of the window/event-loop/input seam.                                    *
 *                                                                                             *
 *  This is the portable path: X11 and Wayland on Linux, Cocoa on macOS, and (unused by the     *
 *  engine, which keeps its WndProc there) Win32. It is the backend that has actually been run: *
 *  see docs/porting/window-event-loop.md for what was verified where.                          *
 *                                                                                             *
 *  Why SDL2 rather than raw Xlib/xcb plus a second Wayland backend: the work this file does    *
 *  that is *not* opening a window - keyboard scan-code normalisation across XKB layouts, dead  *
 *  keys and IME text input, mouse confinement, display enumeration, and the same code path on  *
 *  Wayland where there is no XSetInputFocus at all - is the majority of it, and is exactly     *
 *  what SDL exists to have already debugged. SDL2 was already an optional dependency of        *
 *  spikes/renderer.                                                                            *
 *                                                                                             *
 *  Why there is *also* a native Cocoa backend (platform_window_cocoa.mm) rather than shipping  *
 *  macOS on SDL: the shipping artefact is a .app that must own its NSApplication main loop and *
 *  its CAMetalLayer, and the CAMetalLayer is the one thing MoltenVK actually needs. Keeping a   *
 *  backend that talks to Cocoa directly is what proves that requirement can be met without     *
 *  SDL in the bundle. Both are behind the same header, so this is a link-time choice.           *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#include "platform_window.h"

#ifndef _WIN32

#include <SDL.h>
#include <SDL_vulkan.h>

#include <cstring>
#include <deque>
#include <string>

namespace WWPlatform
{

namespace
{

/*
**	SDL scan code -> PC/AT set-1 scan code, which is the KEYSCAN_* value in
**	Core/GameEngine/Include/GameClient/KeyScanCodes.h and therefore the KeyDefType the engine
**	stores. The KEYSCAN_ name is carried in the table so that
**	scripts/ci/check-window-scancodes.py can check every value against that header rather
**	than the table being trusted.
**
**	SDL scan codes are USB HID usage codes, which are *physical* keys, the same thing set-1
**	codes are - so this is a relabelling, not a layout-dependent guess. Character input does
**	not come through here at all; it arrives as WINDOW_EVENT_TEXT.
*/
struct ScanCodeMapping
{
	SDL_Scancode Sdl;
	int Set1;
	const char * Name;
};

const ScanCodeMapping SCAN_CODE_TABLE[] = {
	{SDL_SCANCODE_ESCAPE, 0x01, "KEYSCAN_ESCAPE"},
	{SDL_SCANCODE_1, 0x02, "KEYSCAN_1"},
	{SDL_SCANCODE_2, 0x03, "KEYSCAN_2"},
	{SDL_SCANCODE_3, 0x04, "KEYSCAN_3"},
	{SDL_SCANCODE_4, 0x05, "KEYSCAN_4"},
	{SDL_SCANCODE_5, 0x06, "KEYSCAN_5"},
	{SDL_SCANCODE_6, 0x07, "KEYSCAN_6"},
	{SDL_SCANCODE_7, 0x08, "KEYSCAN_7"},
	{SDL_SCANCODE_8, 0x09, "KEYSCAN_8"},
	{SDL_SCANCODE_9, 0x0A, "KEYSCAN_9"},
	{SDL_SCANCODE_0, 0x0B, "KEYSCAN_0"},
	{SDL_SCANCODE_MINUS, 0x0C, "KEYSCAN_MINUS"},
	{SDL_SCANCODE_EQUALS, 0x0D, "KEYSCAN_EQUALS"},
	{SDL_SCANCODE_BACKSPACE, 0x0E, "KEYSCAN_BACK"},
	{SDL_SCANCODE_TAB, 0x0F, "KEYSCAN_TAB"},
	{SDL_SCANCODE_Q, 0x10, "KEYSCAN_Q"},
	{SDL_SCANCODE_W, 0x11, "KEYSCAN_W"},
	{SDL_SCANCODE_E, 0x12, "KEYSCAN_E"},
	{SDL_SCANCODE_R, 0x13, "KEYSCAN_R"},
	{SDL_SCANCODE_T, 0x14, "KEYSCAN_T"},
	{SDL_SCANCODE_Y, 0x15, "KEYSCAN_Y"},
	{SDL_SCANCODE_U, 0x16, "KEYSCAN_U"},
	{SDL_SCANCODE_I, 0x17, "KEYSCAN_I"},
	{SDL_SCANCODE_O, 0x18, "KEYSCAN_O"},
	{SDL_SCANCODE_P, 0x19, "KEYSCAN_P"},
	{SDL_SCANCODE_LEFTBRACKET, 0x1A, "KEYSCAN_LBRACKET"},
	{SDL_SCANCODE_RIGHTBRACKET, 0x1B, "KEYSCAN_RBRACKET"},
	{SDL_SCANCODE_RETURN, 0x1C, "KEYSCAN_RETURN"},
	{SDL_SCANCODE_LCTRL, 0x1D, "KEYSCAN_LCONTROL"},
	{SDL_SCANCODE_A, 0x1E, "KEYSCAN_A"},
	{SDL_SCANCODE_S, 0x1F, "KEYSCAN_S"},
	{SDL_SCANCODE_D, 0x20, "KEYSCAN_D"},
	{SDL_SCANCODE_F, 0x21, "KEYSCAN_F"},
	{SDL_SCANCODE_G, 0x22, "KEYSCAN_G"},
	{SDL_SCANCODE_H, 0x23, "KEYSCAN_H"},
	{SDL_SCANCODE_J, 0x24, "KEYSCAN_J"},
	{SDL_SCANCODE_K, 0x25, "KEYSCAN_K"},
	{SDL_SCANCODE_L, 0x26, "KEYSCAN_L"},
	{SDL_SCANCODE_SEMICOLON, 0x27, "KEYSCAN_SEMICOLON"},
	{SDL_SCANCODE_APOSTROPHE, 0x28, "KEYSCAN_APOSTROPHE"},
	{SDL_SCANCODE_GRAVE, 0x29, "KEYSCAN_GRAVE"},
	{SDL_SCANCODE_LSHIFT, 0x2A, "KEYSCAN_LSHIFT"},
	{SDL_SCANCODE_BACKSLASH, 0x2B, "KEYSCAN_BACKSLASH"},
	{SDL_SCANCODE_Z, 0x2C, "KEYSCAN_Z"},
	{SDL_SCANCODE_X, 0x2D, "KEYSCAN_X"},
	{SDL_SCANCODE_C, 0x2E, "KEYSCAN_C"},
	{SDL_SCANCODE_V, 0x2F, "KEYSCAN_V"},
	{SDL_SCANCODE_B, 0x30, "KEYSCAN_B"},
	{SDL_SCANCODE_N, 0x31, "KEYSCAN_N"},
	{SDL_SCANCODE_M, 0x32, "KEYSCAN_M"},
	{SDL_SCANCODE_COMMA, 0x33, "KEYSCAN_COMMA"},
	{SDL_SCANCODE_PERIOD, 0x34, "KEYSCAN_PERIOD"},
	{SDL_SCANCODE_SLASH, 0x35, "KEYSCAN_SLASH"},
	{SDL_SCANCODE_RSHIFT, 0x36, "KEYSCAN_RSHIFT"},
	{SDL_SCANCODE_KP_MULTIPLY, 0x37, "KEYSCAN_NUMPADSTAR"},
	{SDL_SCANCODE_LALT, 0x38, "KEYSCAN_LALT"},
	{SDL_SCANCODE_SPACE, 0x39, "KEYSCAN_SPACE"},
	{SDL_SCANCODE_CAPSLOCK, 0x3A, "KEYSCAN_CAPSLOCK"},
	{SDL_SCANCODE_F1, 0x3B, "KEYSCAN_F1"},
	{SDL_SCANCODE_F2, 0x3C, "KEYSCAN_F2"},
	{SDL_SCANCODE_F3, 0x3D, "KEYSCAN_F3"},
	{SDL_SCANCODE_F4, 0x3E, "KEYSCAN_F4"},
	{SDL_SCANCODE_F5, 0x3F, "KEYSCAN_F5"},
	{SDL_SCANCODE_F6, 0x40, "KEYSCAN_F6"},
	{SDL_SCANCODE_F7, 0x41, "KEYSCAN_F7"},
	{SDL_SCANCODE_F8, 0x42, "KEYSCAN_F8"},
	{SDL_SCANCODE_F9, 0x43, "KEYSCAN_F9"},
	{SDL_SCANCODE_F10, 0x44, "KEYSCAN_F10"},
	{SDL_SCANCODE_NUMLOCKCLEAR, 0x45, "KEYSCAN_NUMLOCK"},
	{SDL_SCANCODE_SCROLLLOCK, 0x46, "KEYSCAN_SCROLL"},
	{SDL_SCANCODE_KP_7, 0x47, "KEYSCAN_NUMPAD7"},
	{SDL_SCANCODE_KP_8, 0x48, "KEYSCAN_NUMPAD8"},
	{SDL_SCANCODE_KP_9, 0x49, "KEYSCAN_NUMPAD9"},
	{SDL_SCANCODE_KP_MINUS, 0x4A, "KEYSCAN_NUMPADMINUS"},
	{SDL_SCANCODE_KP_4, 0x4B, "KEYSCAN_NUMPAD4"},
	{SDL_SCANCODE_KP_5, 0x4C, "KEYSCAN_NUMPAD5"},
	{SDL_SCANCODE_KP_6, 0x4D, "KEYSCAN_NUMPAD6"},
	{SDL_SCANCODE_KP_PLUS, 0x4E, "KEYSCAN_NUMPADPLUS"},
	{SDL_SCANCODE_KP_1, 0x4F, "KEYSCAN_NUMPAD1"},
	{SDL_SCANCODE_KP_2, 0x50, "KEYSCAN_NUMPAD2"},
	{SDL_SCANCODE_KP_3, 0x51, "KEYSCAN_NUMPAD3"},
	{SDL_SCANCODE_KP_0, 0x52, "KEYSCAN_NUMPAD0"},
	{SDL_SCANCODE_KP_PERIOD, 0x53, "KEYSCAN_NUMPADPERIOD"},
	{SDL_SCANCODE_NONUSBACKSLASH, 0x56, "KEYSCAN_OEM_102"},
	{SDL_SCANCODE_F11, 0x57, "KEYSCAN_F11"},
	{SDL_SCANCODE_F12, 0x58, "KEYSCAN_F12"},
	// The Japanese keys. SDL follows the USB HID "International"/"Lang" naming; the pairing
	// with the set-1 codes below is from the HID usage tables, not from having been typed on
	// a JIS keyboard, and is called out as unverified in the doc.
	{SDL_SCANCODE_INTERNATIONAL2, 0x70, "KEYSCAN_KANA"},
	{SDL_SCANCODE_INTERNATIONAL4, 0x79, "KEYSCAN_CONVERT"},
	{SDL_SCANCODE_INTERNATIONAL5, 0x7B, "KEYSCAN_NOCONVERT"},
	{SDL_SCANCODE_INTERNATIONAL3, 0x7D, "KEYSCAN_YEN"},
	{SDL_SCANCODE_INTERNATIONAL6, 0x90, "KEYSCAN_CIRCUMFLEX"},
	{SDL_SCANCODE_LANG1, 0x94, "KEYSCAN_KANJI"},
	{SDL_SCANCODE_KP_ENTER, 0x9C, "KEYSCAN_NUMPADENTER"},
	{SDL_SCANCODE_RCTRL, 0x9D, "KEYSCAN_RCONTROL"},
	{SDL_SCANCODE_KP_DIVIDE, 0xB5, "KEYSCAN_NUMPADSLASH"},
	{SDL_SCANCODE_PRINTSCREEN, 0xB7, "KEYSCAN_SYSRQ"},
	{SDL_SCANCODE_RALT, 0xB8, "KEYSCAN_RALT"},
	{SDL_SCANCODE_HOME, 0xC7, "KEYSCAN_HOME"},
	{SDL_SCANCODE_UP, 0xC8, "KEYSCAN_UPARROW"},
	{SDL_SCANCODE_PAGEUP, 0xC9, "KEYSCAN_PGUP"},
	{SDL_SCANCODE_LEFT, 0xCB, "KEYSCAN_LEFTARROW"},
	{SDL_SCANCODE_RIGHT, 0xCD, "KEYSCAN_RIGHTARROW"},
	{SDL_SCANCODE_END, 0xCF, "KEYSCAN_END"},
	{SDL_SCANCODE_DOWN, 0xD0, "KEYSCAN_DOWNARROW"},
	{SDL_SCANCODE_PAGEDOWN, 0xD1, "KEYSCAN_PGDN"},
	{SDL_SCANCODE_INSERT, 0xD2, "KEYSCAN_INSERT"},
	{SDL_SCANCODE_DELETE, 0xD3, "KEYSCAN_DELETE"},
};

const int SCAN_CODE_COUNT = static_cast<int>(sizeof(SCAN_CODE_TABLE) / sizeof(SCAN_CODE_TABLE[0]));

int Set1_From_Sdl(SDL_Scancode code)
{
	for (int i = 0; i < SCAN_CODE_COUNT; ++i) {
		if (SCAN_CODE_TABLE[i].Sdl == code) return SCAN_CODE_TABLE[i].Set1;
	}
	return 0;
}

SDL_Scancode Sdl_From_Set1(int set1)
{
	for (int i = 0; i < SCAN_CODE_COUNT; ++i) {
		if (SCAN_CODE_TABLE[i].Set1 == set1) return SCAN_CODE_TABLE[i].Sdl;
	}
	return SDL_SCANCODE_UNKNOWN;
}

unsigned int Modifiers_From_Sdl(SDL_Keymod mod)
{
	unsigned int flags = WINDOW_MODIFIER_NONE;
	if ((mod & KMOD_SHIFT) != 0) flags |= WINDOW_MODIFIER_SHIFT;
	if ((mod & KMOD_CTRL) != 0) flags |= WINDOW_MODIFIER_CONTROL;
	if ((mod & KMOD_ALT) != 0) flags |= WINDOW_MODIFIER_ALT;
	if ((mod & KMOD_CAPS) != 0) flags |= WINDOW_MODIFIER_CAPS_LOCK;
	return flags;
}

int Button_From_Sdl(Uint8 button)
{
	switch (button) {
		case SDL_BUTTON_MIDDLE: return MOUSE_BUTTON_MIDDLE;
		case SDL_BUTTON_RIGHT: return MOUSE_BUTTON_RIGHT;
		default: return MOUSE_BUTTON_LEFT;
	}
}

/*
**	One UTF-32 code point out of SDL's UTF-8 SDL_TEXTINPUT payload. Returns the number of
**	bytes consumed, and never reads past the terminator, so malformed input terminates the
**	walk rather than running off the buffer.
*/
int Decode_Utf8(const char * text, unsigned int & out_code_point)
{
	const unsigned char * p = reinterpret_cast<const unsigned char *>(text);
	if (p[0] < 0x80) {
		out_code_point = p[0];
		return p[0] == 0 ? 0 : 1;
	}
	int length = 0;
	unsigned int value = 0;
	if ((p[0] & 0xE0) == 0xC0) { length = 2; value = p[0] & 0x1Fu; }
	else if ((p[0] & 0xF0) == 0xE0) { length = 3; value = p[0] & 0x0Fu; }
	else if ((p[0] & 0xF8) == 0xF0) { length = 4; value = p[0] & 0x07u; }
	else return 0;
	for (int i = 1; i < length; ++i) {
		if ((p[i] & 0xC0) != 0x80) return 0;
		value = (value << 6) | (p[i] & 0x3Fu);
	}
	out_code_point = value;
	return length;
}

struct WindowState
{
	SDL_Window * Sdl_Window;
	std::deque<WindowEvent> Queue;
	bool Active;
	bool Minimised;
	bool Cursor_Inside;
	int Width;
	int Height;

	WindowState()
		: Sdl_Window(nullptr), Active(false), Minimised(false), Cursor_Inside(false),
		  Width(0), Height(0)
	{
	}
};

WindowState * TheWindow = nullptr;
std::string TheLastError;

WindowState * State(void * window)
{
	// The seam supports exactly one window, so the handle is the state, and a stale handle
	// from a destroyed window is rejected rather than dereferenced.
	if (window == nullptr || window != TheWindow) return nullptr;
	return TheWindow;
}

void Push(WindowState * state, const WindowEvent & event)
{
	state->Queue.push_back(event);
}

void Translate(WindowState * state, const SDL_Event & in)
{
	WindowEvent out;
	switch (in.type) {
		case SDL_QUIT:
			out.Type = WINDOW_EVENT_CLOSE;
			out.Time_Ms = in.quit.timestamp;
			Push(state, out);
			return;

		case SDL_WINDOWEVENT:
			out.Time_Ms = in.window.timestamp;
			switch (in.window.event) {
				case SDL_WINDOWEVENT_CLOSE:
					out.Type = WINDOW_EVENT_CLOSE;
					break;
				case SDL_WINDOWEVENT_SIZE_CHANGED:
					state->Width = in.window.data1;
					state->Height = in.window.data2;
					out.Type = WINDOW_EVENT_RESIZE;
					out.Width = in.window.data1;
					out.Height = in.window.data2;
					break;
				case SDL_WINDOWEVENT_MOVED:
					out.Type = WINDOW_EVENT_MOVE;
					break;
				case SDL_WINDOWEVENT_FOCUS_GAINED:
					state->Active = true;
					out.Type = WINDOW_EVENT_FOCUS_GAINED;
					break;
				case SDL_WINDOWEVENT_FOCUS_LOST:
					state->Active = false;
					out.Type = WINDOW_EVENT_FOCUS_LOST;
					break;
				case SDL_WINDOWEVENT_MINIMIZED:
					state->Minimised = true;
					out.Type = WINDOW_EVENT_MINIMISED;
					break;
				case SDL_WINDOWEVENT_RESTORED:
				case SDL_WINDOWEVENT_MAXIMIZED:
					state->Minimised = false;
					out.Type = WINDOW_EVENT_RESTORED;
					break;
				case SDL_WINDOWEVENT_ENTER:
					state->Cursor_Inside = true;
					out.Type = WINDOW_EVENT_MOUSE_ENTER;
					break;
				case SDL_WINDOWEVENT_LEAVE:
					state->Cursor_Inside = false;
					out.Type = WINDOW_EVENT_MOUSE_LEAVE;
					break;
				default:
					return;
			}
			Push(state, out);
			return;

		case SDL_KEYDOWN:
		case SDL_KEYUP:
			out.Type = in.type == SDL_KEYDOWN ? WINDOW_EVENT_KEY_DOWN : WINDOW_EVENT_KEY_UP;
			out.Scan_Code = Set1_From_Sdl(in.key.keysym.scancode);
			out.Repeat = in.key.repeat != 0;
			out.Modifiers = Modifiers_From_Sdl(static_cast<SDL_Keymod>(in.key.keysym.mod));
			out.Time_Ms = in.key.timestamp;
			// A key with no set-1 code (the GUI/"super" keys, the media keys) is dropped
			// rather than delivered as key 0, which KeyDefType would read as KEY_NONE.
			if (out.Scan_Code != 0) Push(state, out);
			return;

		case SDL_TEXTINPUT: {
			const char * text = in.text.text;
			unsigned int code_point = 0;
			int consumed = Decode_Utf8(text, code_point);
			while (consumed > 0) {
				out = WindowEvent();
				out.Type = WINDOW_EVENT_TEXT;
				out.Character = code_point;
				out.Modifiers = Modifiers_From_Sdl(SDL_GetModState());
				out.Time_Ms = in.text.timestamp;
				Push(state, out);
				text += consumed;
				consumed = Decode_Utf8(text, code_point);
			}
			return;
		}

		case SDL_MOUSEMOTION:
			out.Type = WINDOW_EVENT_MOUSE_MOVE;
			out.Mouse_X = in.motion.x;
			out.Mouse_Y = in.motion.y;
			out.Modifiers = Modifiers_From_Sdl(SDL_GetModState());
			out.Time_Ms = in.motion.timestamp;
			Push(state, out);
			return;

		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEBUTTONUP:
			out.Type = in.type == SDL_MOUSEBUTTONDOWN ? WINDOW_EVENT_MOUSE_DOWN
			                                          : WINDOW_EVENT_MOUSE_UP;
			out.Mouse_X = in.button.x;
			out.Mouse_Y = in.button.y;
			out.Mouse_Button = Button_From_Sdl(in.button.button);
			out.Click_Count = in.button.clicks;
			out.Modifiers = Modifiers_From_Sdl(SDL_GetModState());
			out.Time_Ms = in.button.timestamp;
			Push(state, out);
			return;

		case SDL_MOUSEWHEEL: {
			int x = 0;
			int y = 0;
			SDL_GetMouseState(&x, &y);
			out.Type = WINDOW_EVENT_MOUSE_WHEEL;
			out.Mouse_X = x;
			out.Mouse_Y = y;
			// WM_MOUSEWHEEL reports multiples of WHEEL_DELTA (120) and Win32Mouse.cpp
			// divides by it, so the notch count is scaled back up here.
			out.Wheel_Delta = in.wheel.y * 120;
			out.Modifiers = Modifiers_From_Sdl(SDL_GetModState());
			out.Time_Ms = in.wheel.timestamp;
			Push(state, out);
			return;
		}

		default:
			return;
	}
}

} // namespace

void * Window_Create(const WindowConfig & config)
{
	if (TheWindow != nullptr) {
		TheLastError = "a window already exists; the seam supports one window";
		return nullptr;
	}
	if (SDL_WasInit(SDL_INIT_VIDEO) == 0 && SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
		TheLastError = std::string("SDL_InitSubSystem(SDL_INIT_VIDEO) failed: ") + SDL_GetError();
		return nullptr;
	}

	Uint32 flags = SDL_WINDOW_VULKAN | SDL_WINDOW_SHOWN;
	if (config.Resizable) flags |= SDL_WINDOW_RESIZABLE;
	// Borderless-desktop rather than a real mode switch: see Window_Set_Mode.
	if (config.Fullscreen) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

	SDL_Window * sdl_window = SDL_CreateWindow(
		config.Title != nullptr ? config.Title : "", SDL_WINDOWPOS_CENTERED,
		SDL_WINDOWPOS_CENTERED, config.Width, config.Height, flags);
	if (sdl_window == nullptr) {
		TheLastError = std::string("SDL_CreateWindow failed: ") + SDL_GetError();
		return nullptr;
	}

	TheWindow = new WindowState();
	TheWindow->Sdl_Window = sdl_window;
	TheWindow->Width = config.Width;
	TheWindow->Height = config.Height;
	SDL_GetWindowSize(sdl_window, &TheWindow->Width, &TheWindow->Height);
	if (config.Hide_System_Cursor) SDL_ShowCursor(SDL_DISABLE);
	// The engine has text entry fields (chat, save-game names) and they need composed
	// characters, not scan codes; this is what turns SDL_TEXTINPUT on.
	SDL_StartTextInput();
	TheLastError.clear();
	return TheWindow;
}

void Window_Destroy(void * window)
{
	WindowState * state = State(window);
	if (state == nullptr) return;
	SDL_StopTextInput();
	SDL_DestroyWindow(state->Sdl_Window);
	delete state;
	TheWindow = nullptr;
	// Deliberately not SDL_Quit(): the audio backend may hold other subsystems.
	SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

void Window_Pump(void * window)
{
	WindowState * state = State(window);
	if (state == nullptr) return;
	SDL_Event event;
	while (SDL_PollEvent(&event) != 0) {
		Translate(state, event);
	}
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
	SDL_GetWindowSize(state->Sdl_Window, &state->Width, &state->Height);
	width = state->Width;
	height = state->Height;
}

float Window_Backing_Scale(void * window)
{
	WindowState * state = State(window);
	if (state == nullptr) return 1.0f;

	/*
	**	SDL has no backingScaleFactor call: the factor is the ratio between the drawable, which
	**	is in pixels, and the window, which is in points. On X11 and on a non-Retina display the
	**	two are equal and this is exactly 1.
	*/
	int window_width = 0;
	int window_height = 0;
	SDL_GetWindowSize(state->Sdl_Window, &window_width, &window_height);
	int drawable_width = 0;
	int drawable_height = 0;
	SDL_Vulkan_GetDrawableSize(state->Sdl_Window, &drawable_width, &drawable_height);
	if (window_width <= 0 || window_height <= 0 || drawable_width <= 0 || drawable_height <= 0) {
		return 1.0f;
	}
	return static_cast<float>(drawable_width) / static_cast<float>(window_width);
}

bool Window_Set_Mode(void * window, int width, int height, bool fullscreen)
{
	WindowState * state = State(window);
	if (state == nullptr) return false;
	// FULLSCREEN_DESKTOP, not FULLSCREEN: a borderless window at the current desktop
	// resolution. The Win32 path's ChangeDisplaySettings()/D3DPRESENT_PARAMETERS mode switch
	// has no counterpart on either macOS (which composites instead) or Wayland, so a
	// non-native game resolution is the renderer's problem - it scales at present time -
	// rather than the display server's.
	if (SDL_SetWindowFullscreen(state->Sdl_Window,
	                            fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0) != 0) {
		TheLastError = std::string("SDL_SetWindowFullscreen failed: ") + SDL_GetError();
		return false;
	}
	if (!fullscreen) SDL_SetWindowSize(state->Sdl_Window, width, height);
	SDL_GetWindowSize(state->Sdl_Window, &state->Width, &state->Height);
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

	// A borderless-fullscreen window has no frame at all, and asking the window manager for the
	// borders of one is how a stale title-bar height gets added to a fullscreen resolution.
	if (Window_Is_Fullscreen(window)) return true;

#if SDL_VERSION_ATLEAST(2, 0, 5)
	// SDL_GetWindowBordersSize() reports the window manager's decorations in points, and fails
	// where the window manager does not tell it (Wayland with client-side decorations, and X11
	// before the window is mapped). A failure is reported rather than papered over with zeros,
	// because a caller sizing a frame needs to know it got no answer.
	if (SDL_GetWindowBordersSize(state->Sdl_Window, &top, &left, &bottom, &right) != 0) {
		TheLastError = std::string("SDL_GetWindowBordersSize failed: ") + SDL_GetError();
		left = 0;
		top = 0;
		right = 0;
		bottom = 0;
		return false;
	}
	return true;
#else
	return false;
#endif
}

bool Window_Is_Fullscreen(void * window)
{
	WindowState * state = State(window);
	if (state == nullptr) return false;
	const Uint32 flags = SDL_GetWindowFlags(state->Sdl_Window);
	return (flags & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP)) != 0;
}

bool Window_Set_Position(void * window, int x, int y)
{
	WindowState * state = State(window);
	if (state == nullptr) return false;
	// SDL positions by the client area's top-left, which is the corner the caller means.
	SDL_SetWindowPosition(state->Sdl_Window, x, y);
	return true;
}

bool Window_Set_Client_Size(void * window, int width, int height)
{
	WindowState * state = State(window);
	if (state == nullptr || width <= 0 || height <= 0) return false;
	SDL_SetWindowSize(state->Sdl_Window, width, height);
	SDL_GetWindowSize(state->Sdl_Window, &state->Width, &state->Height);
	return true;
}

void Window_Set_Always_On_Top(void * window, bool on_top)
{
	WindowState * state = State(window);
	if (state == nullptr) return;
#if SDL_VERSION_ATLEAST(2, 0, 16)
	SDL_SetWindowAlwaysOnTop(state->Sdl_Window, on_top ? SDL_TRUE : SDL_FALSE);
#else
	// Before 2.0.16 the flag can only be set at creation time. Raising the window is the part of
	// HWND_TOPMOST that matters to the caller - being in front now - without the part that keeps
	// it there.
	if (on_top) SDL_RaiseWindow(state->Sdl_Window);
#endif
}

int Window_Display_Count()
{
	const int count = SDL_GetNumVideoDisplays();
	return (count > 0) ? count : 0;
}

int Window_Display_For_Window(void * window)
{
	WindowState * state = State(window);
	if (state == nullptr) return 0;
	const int display = SDL_GetWindowDisplayIndex(state->Sdl_Window);
	// SDL returns a negative number when it cannot tell; the primary display is the same answer
	// Win32's MONITOR_DEFAULTTOPRIMARY gives, which is what the call site asks for.
	return (display >= 0) ? display : 0;
}

bool Window_Display_Bounds(int display, int & x, int & y, int & width, int & height)
{
	SDL_Rect bounds = { 0, 0, 0, 0 };
	if (display < 0 || SDL_GetDisplayBounds(display, &bounds) != 0) {
		TheLastError = std::string("SDL_GetDisplayBounds failed: ") + SDL_GetError();
		return false;
	}
	x = bounds.x;
	y = bounds.y;
	width = bounds.w;
	height = bounds.h;
	return true;
}

bool Window_Display_Work_Area(int display, int & x, int & y, int & width, int & height)
{
#if SDL_VERSION_ATLEAST(2, 0, 5)
	SDL_Rect usable = { 0, 0, 0, 0 };
	if (display >= 0 && SDL_GetDisplayUsableBounds(display, &usable) == 0) {
		x = usable.x;
		y = usable.y;
		width = usable.w;
		height = usable.h;
		return true;
	}
#endif
	// Without a usable-bounds query the whole display is the best answer, and it is the one Win32
	// gives on a display with no taskbar on it. A window centred in it can end up under a panel,
	// which is a placement annoyance rather than a wrong-geometry defect.
	return Window_Display_Bounds(display, x, y, width, height);
}

void Window_Show(void * window, bool show)
{
	WindowState * state = State(window);
	if (state == nullptr) return;
	if (show) {
		SDL_ShowWindow(state->Sdl_Window);
		SDL_RaiseWindow(state->Sdl_Window);
	} else {
		SDL_HideWindow(state->Sdl_Window);
	}
}

void Window_Set_Title(void * window, const char * title)
{
	WindowState * state = State(window);
	if (state == nullptr || title == nullptr) return;
	SDL_SetWindowTitle(state->Sdl_Window, title);
}

void Window_Set_Cursor_Clip(void * window, bool clip)
{
	WindowState * state = State(window);
	if (state == nullptr) return;
#if SDL_VERSION_ATLEAST(2, 0, 16)
	SDL_SetWindowMouseGrab(state->Sdl_Window, clip ? SDL_TRUE : SDL_FALSE);
#else
	// Before 2.0.16 there is only the combined grab, which also takes the keyboard on X11.
	SDL_SetWindowGrab(state->Sdl_Window, clip ? SDL_TRUE : SDL_FALSE);
#endif
}

void Window_Warp_Cursor(void * window, int x, int y)
{
	WindowState * state = State(window);
	if (state == nullptr) return;
	SDL_WarpMouseInWindow(state->Sdl_Window, x, y);
}

void Window_Show_System_Cursor(void * window, bool show)
{
	if (State(window) == nullptr) return;
	SDL_ShowCursor(show ? SDL_ENABLE : SDL_DISABLE);
}

/*
**	SDL_PIXELFORMAT_ARGB8888 is the packed 32-bit word A<<24|R<<16|G<<8|B, which on a little
**	endian host is B,G,R,A in memory: the seam's layout, so no conversion. SDL_CreateColorCursor()
**	copies the surface, so the caller's pixels are not referenced afterwards. This is a real
**	cursor on X11 and Wayland; it needs a display server, so it is unmeasured on the headless
**	CI runner (docs/porting/mouse-cursor-seam.md).
*/
void * Window_Create_Cursor(const CursorImage & image)
{
	if (image.Pixels_BGRA == nullptr || image.Width <= 0 || image.Height <= 0) {
		TheLastError = "Window_Create_Cursor: empty image";
		return nullptr;
	}
	SDL_Surface * surface = SDL_CreateRGBSurfaceWithFormatFrom(
		const_cast<unsigned char *>(image.Pixels_BGRA), image.Width, image.Height, 32,
		image.Width * 4, SDL_PIXELFORMAT_ARGB8888);
	if (surface == nullptr) {
		TheLastError = std::string("SDL_CreateRGBSurfaceWithFormatFrom failed: ") + SDL_GetError();
		return nullptr;
	}
	SDL_Cursor * cursor = SDL_CreateColorCursor(surface, image.Hotspot_X, image.Hotspot_Y);
	SDL_FreeSurface(surface);
	if (cursor == nullptr) {
		TheLastError = std::string("SDL_CreateColorCursor failed: ") + SDL_GetError();
		return nullptr;
	}
	return cursor;
}

void Window_Destroy_Cursor(void * cursor)
{
	if (cursor == nullptr) return;
	SDL_Cursor * sdl_cursor = static_cast<SDL_Cursor *>(cursor);
	if (SDL_GetCursor() == sdl_cursor) SDL_SetCursor(SDL_GetDefaultCursor());
	SDL_FreeCursor(sdl_cursor);
}

void Window_Set_Cursor(void * window, void * cursor)
{
	if (State(window) == nullptr) return;
	SDL_SetCursor(cursor != nullptr ? static_cast<SDL_Cursor *>(cursor) : SDL_GetDefaultCursor());
}

bool Window_Cursor_Position(void * window, int & x, int & y)
{
	WindowState * state = State(window);
	if (state == nullptr) return false;

	/*
	**	SDL_GetGlobalMouseState() is the desktop position, which is what GetCursorPos() reports:
	**	it keeps reading while the pointer is outside our window, which SDL_GetMouseState() does
	**	not. Where it is unavailable, the window-relative position plus the window's origin is
	**	the same value for the case that matters (the pointer inside our window) and clamps to
	**	the window's edges outside it.
	*/
#if SDL_VERSION_ATLEAST(2, 0, 4)
	SDL_GetGlobalMouseState(&x, &y);
#else
	int client_x = 0;
	int client_y = 0;
	SDL_GetMouseState(&client_x, &client_y);
	int origin_x = 0;
	int origin_y = 0;
	if (!Window_Client_Origin(window, origin_x, origin_y)) return false;
	x = origin_x + client_x;
	y = origin_y + client_y;
#endif
	return true;
}

bool Window_Client_Origin(void * window, int & x, int & y)
{
	WindowState * state = State(window);
	if (state == nullptr) return false;

	/*
	**	SDL_GetWindowPosition() reports the top-left of the client area, not of the frame, which
	**	is the corner ScreenToClient() measures from. Both values are in points: SDL keeps
	**	window coordinates in points and only SDL_GL_GetDrawableSize()/SDL_Vulkan_GetDrawableSize()
	**	are in pixels.
	*/
	SDL_GetWindowPosition(state->Sdl_Window, &x, &y);
	return true;
}

void * Window_Current()
{
	return TheWindow;
}

bool Window_Key_Is_Down(void * window, int scan_code)
{
	if (State(window) == nullptr) return false;
	SDL_Scancode code = Sdl_From_Set1(scan_code);
	if (code == SDL_SCANCODE_UNKNOWN) return false;
	int count = 0;
	const Uint8 * keys = SDL_GetKeyboardState(&count);
	if (keys == nullptr || static_cast<int>(code) >= count) return false;
	return keys[code] != 0;
}

unsigned int Window_Modifier_State(void * window)
{
	if (State(window) == nullptr) return WINDOW_MODIFIER_NONE;
	return Modifiers_From_Sdl(SDL_GetModState());
}

int Window_Vulkan_Instance_Extensions(void * window, const char ** names, int max_names)
{
	WindowState * state = State(window);
	if (state == nullptr) return -1;
	unsigned int count = 0;
	if (SDL_Vulkan_GetInstanceExtensions(state->Sdl_Window, &count, nullptr) != SDL_TRUE) {
		TheLastError = std::string("SDL_Vulkan_GetInstanceExtensions failed: ") + SDL_GetError();
		return -1;
	}
	if (names == nullptr) return static_cast<int>(count);
	if (max_names < static_cast<int>(count)) return -1;
	if (SDL_Vulkan_GetInstanceExtensions(state->Sdl_Window, &count, names) != SDL_TRUE) {
		TheLastError = std::string("SDL_Vulkan_GetInstanceExtensions failed: ") + SDL_GetError();
		return -1;
	}
	return static_cast<int>(count);
}

bool Window_Create_Vulkan_Surface(void * window, void * vk_instance, void * out_vk_surface)
{
	WindowState * state = State(window);
	if (state == nullptr || vk_instance == nullptr || out_vk_surface == nullptr) return false;
	VkSurfaceKHR * surface = static_cast<VkSurfaceKHR *>(out_vk_surface);
	if (SDL_Vulkan_CreateSurface(state->Sdl_Window, static_cast<VkInstance>(vk_instance),
	                             surface) != SDL_TRUE) {
		TheLastError = std::string("SDL_Vulkan_CreateSurface failed: ") + SDL_GetError();
		return false;
	}
	return true;
}

NativeSurface Window_Native_Surface(void * window)
{
	NativeSurface surface;
	WindowState * state = State(window);
	if (state == nullptr) return surface;
	// Deliberately narrow: only the handles that can be produced without pulling an
	// Objective-C or Wayland-protocol header into this translation unit. Everything the
	// renderer needs it gets from Window_Create_Vulkan_Surface() instead, which is why the
	// Cocoa/CAMetalLayer case is answered by platform_window_cocoa.mm rather than here.
	TheLastError = "Window_Native_Surface is not implemented by the SDL2 backend; "
	               "use Window_Create_Vulkan_Surface";
	return surface;
}

const char * Window_Backend_Name()
{
	return "sdl2";
}

const char * Window_Last_Error()
{
	return TheLastError.c_str();
}

}	// namespace WWPlatform

#endif // !_WIN32
