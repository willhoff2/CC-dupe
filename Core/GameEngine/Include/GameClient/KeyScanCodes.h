/*
**	Command & Conquer Generals(tm)
**	Copyright 2025 Electronic Arts Inc.
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

// TheSuperHackers @port Keyboard scan codes, so that the device-independent key tables in
// KeyDefs.h no longer have to include <dinput.h>. The values are the PC/AT set-1 scan codes
// that DirectInput reports and that the engine has always stored in KeyDefType, in saved
// key bindings and in the INI files, so they are part of the data format and must not change.
// On Windows dinput.h is still pulled in below and every constant is asserted against its
// DIK_ counterpart, so a divergence is a build error rather than a silently remapped key.

#pragma once

#include <Utility/CppMacros.h>

enum KeyScanCodeType
{
	KEYSCAN_ESCAPE               = 0x01,
	KEYSCAN_1                    = 0x02,
	KEYSCAN_2                    = 0x03,
	KEYSCAN_3                    = 0x04,
	KEYSCAN_4                    = 0x05,
	KEYSCAN_5                    = 0x06,
	KEYSCAN_6                    = 0x07,
	KEYSCAN_7                    = 0x08,
	KEYSCAN_8                    = 0x09,
	KEYSCAN_9                    = 0x0A,
	KEYSCAN_0                    = 0x0B,
	KEYSCAN_MINUS                = 0x0C,		// - on main keyboard
	KEYSCAN_EQUALS               = 0x0D,
	KEYSCAN_BACK                 = 0x0E,		// backspace
	KEYSCAN_TAB                  = 0x0F,
	KEYSCAN_Q                    = 0x10,
	KEYSCAN_W                    = 0x11,
	KEYSCAN_E                    = 0x12,
	KEYSCAN_R                    = 0x13,
	KEYSCAN_T                    = 0x14,
	KEYSCAN_Y                    = 0x15,
	KEYSCAN_U                    = 0x16,
	KEYSCAN_I                    = 0x17,
	KEYSCAN_O                    = 0x18,
	KEYSCAN_P                    = 0x19,
	KEYSCAN_LBRACKET             = 0x1A,
	KEYSCAN_RBRACKET             = 0x1B,
	KEYSCAN_RETURN               = 0x1C,		// Enter on main keyboard
	KEYSCAN_LCONTROL             = 0x1D,
	KEYSCAN_A                    = 0x1E,
	KEYSCAN_S                    = 0x1F,
	KEYSCAN_D                    = 0x20,
	KEYSCAN_F                    = 0x21,
	KEYSCAN_G                    = 0x22,
	KEYSCAN_H                    = 0x23,
	KEYSCAN_J                    = 0x24,
	KEYSCAN_K                    = 0x25,
	KEYSCAN_L                    = 0x26,
	KEYSCAN_SEMICOLON            = 0x27,
	KEYSCAN_APOSTROPHE           = 0x28,
	KEYSCAN_GRAVE                = 0x29,		// accent grave
	KEYSCAN_LSHIFT               = 0x2A,
	KEYSCAN_BACKSLASH            = 0x2B,
	KEYSCAN_Z                    = 0x2C,
	KEYSCAN_X                    = 0x2D,
	KEYSCAN_C                    = 0x2E,
	KEYSCAN_V                    = 0x2F,
	KEYSCAN_B                    = 0x30,
	KEYSCAN_N                    = 0x31,
	KEYSCAN_M                    = 0x32,
	KEYSCAN_COMMA                = 0x33,
	KEYSCAN_PERIOD               = 0x34,		// . on main keyboard
	KEYSCAN_SLASH                = 0x35,		// / on main keyboard
	KEYSCAN_RSHIFT               = 0x36,
	KEYSCAN_NUMPADSTAR           = 0x37,		// * on numeric keypad
	KEYSCAN_LALT                 = 0x38,		// left Alt
	KEYSCAN_SPACE                = 0x39,
	KEYSCAN_CAPSLOCK             = 0x3A,		// CapsLock
	KEYSCAN_F1                   = 0x3B,
	KEYSCAN_F2                   = 0x3C,
	KEYSCAN_F3                   = 0x3D,
	KEYSCAN_F4                   = 0x3E,
	KEYSCAN_F5                   = 0x3F,
	KEYSCAN_F6                   = 0x40,
	KEYSCAN_F7                   = 0x41,
	KEYSCAN_F8                   = 0x42,
	KEYSCAN_F9                   = 0x43,
	KEYSCAN_F10                  = 0x44,
	KEYSCAN_NUMLOCK              = 0x45,
	KEYSCAN_SCROLL               = 0x46,		// Scroll Lock
	KEYSCAN_NUMPAD7              = 0x47,
	KEYSCAN_NUMPAD8              = 0x48,
	KEYSCAN_NUMPAD9              = 0x49,
	KEYSCAN_NUMPADMINUS          = 0x4A,		// - on numeric keypad
	KEYSCAN_NUMPAD4              = 0x4B,
	KEYSCAN_NUMPAD5              = 0x4C,
	KEYSCAN_NUMPAD6              = 0x4D,
	KEYSCAN_NUMPADPLUS           = 0x4E,		// + on numeric keypad
	KEYSCAN_NUMPAD1              = 0x4F,
	KEYSCAN_NUMPAD2              = 0x50,
	KEYSCAN_NUMPAD3              = 0x51,
	KEYSCAN_NUMPAD0              = 0x52,
	KEYSCAN_NUMPADPERIOD         = 0x53,		// . on numeric keypad
	KEYSCAN_OEM_102              = 0x56,		// <> or \| on RT 102-key keyboard (Non-U.S.)
	KEYSCAN_F11                  = 0x57,
	KEYSCAN_F12                  = 0x58,
	KEYSCAN_KANA                 = 0x70,		// (Japanese keyboard)
	KEYSCAN_CONVERT              = 0x79,		// (Japanese keyboard)
	KEYSCAN_NOCONVERT            = 0x7B,		// (Japanese keyboard)
	KEYSCAN_YEN                  = 0x7D,		// (Japanese keyboard)
	KEYSCAN_CIRCUMFLEX           = 0x90,		// Japanese keyboard
	KEYSCAN_KANJI                = 0x94,		// (Japanese keyboard)
	KEYSCAN_NUMPADENTER          = 0x9C,		// Enter on numeric keypad
	KEYSCAN_RCONTROL             = 0x9D,
	KEYSCAN_NUMPADSLASH          = 0xB5,		// / on numeric keypad
	KEYSCAN_SYSRQ                = 0xB7,
	KEYSCAN_RALT                 = 0xB8,		// right Alt
	KEYSCAN_HOME                 = 0xC7,		// Home on arrow keypad
	KEYSCAN_UPARROW              = 0xC8,		// UpArrow on arrow keypad
	KEYSCAN_PGUP                 = 0xC9,		// PgUp on arrow keypad
	KEYSCAN_LEFTARROW            = 0xCB,		// LeftArrow on arrow keypad
	KEYSCAN_RIGHTARROW           = 0xCD,		// RightArrow on arrow keypad
	KEYSCAN_END                  = 0xCF,		// End on arrow keypad
	KEYSCAN_DOWNARROW            = 0xD0,		// DownArrow on arrow keypad
	KEYSCAN_PGDN                 = 0xD1,		// PgDn on arrow keypad
	KEYSCAN_INSERT               = 0xD2,		// Insert on arrow keypad
	KEYSCAN_DELETE               = 0xD3,		// Delete on arrow keypad
};

#ifdef _WIN32

#ifndef DIRECTINPUT_VERSION
#	define DIRECTINPUT_VERSION	0x800
#endif

#include <dinput.h>

// The Windows build keeps talking to DirectInput, so the table above has to agree with it
// exactly. Checked on every toolchain, including VC6, where static_assert is compiled away.
STATIC_ASSERT_ALWAYS(KEYSCAN_ESCAPE == DIK_ESCAPE, "KEYSCAN_ESCAPE must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_1 == DIK_1, "KEYSCAN_1 must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_2 == DIK_2, "KEYSCAN_2 must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_3 == DIK_3, "KEYSCAN_3 must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_4 == DIK_4, "KEYSCAN_4 must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_5 == DIK_5, "KEYSCAN_5 must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_6 == DIK_6, "KEYSCAN_6 must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_7 == DIK_7, "KEYSCAN_7 must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_8 == DIK_8, "KEYSCAN_8 must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_9 == DIK_9, "KEYSCAN_9 must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_0 == DIK_0, "KEYSCAN_0 must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_MINUS == DIK_MINUS, "KEYSCAN_MINUS must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_EQUALS == DIK_EQUALS, "KEYSCAN_EQUALS must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_BACK == DIK_BACK, "KEYSCAN_BACK must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_TAB == DIK_TAB, "KEYSCAN_TAB must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_Q == DIK_Q, "KEYSCAN_Q must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_W == DIK_W, "KEYSCAN_W must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_E == DIK_E, "KEYSCAN_E must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_R == DIK_R, "KEYSCAN_R must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_T == DIK_T, "KEYSCAN_T must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_Y == DIK_Y, "KEYSCAN_Y must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_U == DIK_U, "KEYSCAN_U must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_I == DIK_I, "KEYSCAN_I must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_O == DIK_O, "KEYSCAN_O must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_P == DIK_P, "KEYSCAN_P must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_LBRACKET == DIK_LBRACKET, "KEYSCAN_LBRACKET must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_RBRACKET == DIK_RBRACKET, "KEYSCAN_RBRACKET must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_RETURN == DIK_RETURN, "KEYSCAN_RETURN must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_LCONTROL == DIK_LCONTROL, "KEYSCAN_LCONTROL must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_A == DIK_A, "KEYSCAN_A must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_S == DIK_S, "KEYSCAN_S must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_D == DIK_D, "KEYSCAN_D must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_F == DIK_F, "KEYSCAN_F must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_G == DIK_G, "KEYSCAN_G must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_H == DIK_H, "KEYSCAN_H must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_J == DIK_J, "KEYSCAN_J must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_K == DIK_K, "KEYSCAN_K must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_L == DIK_L, "KEYSCAN_L must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_SEMICOLON == DIK_SEMICOLON, "KEYSCAN_SEMICOLON must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_APOSTROPHE == DIK_APOSTROPHE, "KEYSCAN_APOSTROPHE must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_GRAVE == DIK_GRAVE, "KEYSCAN_GRAVE must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_LSHIFT == DIK_LSHIFT, "KEYSCAN_LSHIFT must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_BACKSLASH == DIK_BACKSLASH, "KEYSCAN_BACKSLASH must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_Z == DIK_Z, "KEYSCAN_Z must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_X == DIK_X, "KEYSCAN_X must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_C == DIK_C, "KEYSCAN_C must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_V == DIK_V, "KEYSCAN_V must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_B == DIK_B, "KEYSCAN_B must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_N == DIK_N, "KEYSCAN_N must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_M == DIK_M, "KEYSCAN_M must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_COMMA == DIK_COMMA, "KEYSCAN_COMMA must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_PERIOD == DIK_PERIOD, "KEYSCAN_PERIOD must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_SLASH == DIK_SLASH, "KEYSCAN_SLASH must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_RSHIFT == DIK_RSHIFT, "KEYSCAN_RSHIFT must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_NUMPADSTAR == DIK_NUMPADSTAR, "KEYSCAN_NUMPADSTAR must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_LALT == DIK_LALT, "KEYSCAN_LALT must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_SPACE == DIK_SPACE, "KEYSCAN_SPACE must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_CAPSLOCK == DIK_CAPSLOCK, "KEYSCAN_CAPSLOCK must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_F1 == DIK_F1, "KEYSCAN_F1 must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_F2 == DIK_F2, "KEYSCAN_F2 must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_F3 == DIK_F3, "KEYSCAN_F3 must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_F4 == DIK_F4, "KEYSCAN_F4 must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_F5 == DIK_F5, "KEYSCAN_F5 must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_F6 == DIK_F6, "KEYSCAN_F6 must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_F7 == DIK_F7, "KEYSCAN_F7 must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_F8 == DIK_F8, "KEYSCAN_F8 must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_F9 == DIK_F9, "KEYSCAN_F9 must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_F10 == DIK_F10, "KEYSCAN_F10 must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_NUMLOCK == DIK_NUMLOCK, "KEYSCAN_NUMLOCK must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_SCROLL == DIK_SCROLL, "KEYSCAN_SCROLL must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_NUMPAD7 == DIK_NUMPAD7, "KEYSCAN_NUMPAD7 must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_NUMPAD8 == DIK_NUMPAD8, "KEYSCAN_NUMPAD8 must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_NUMPAD9 == DIK_NUMPAD9, "KEYSCAN_NUMPAD9 must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_NUMPADMINUS == DIK_NUMPADMINUS, "KEYSCAN_NUMPADMINUS must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_NUMPAD4 == DIK_NUMPAD4, "KEYSCAN_NUMPAD4 must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_NUMPAD5 == DIK_NUMPAD5, "KEYSCAN_NUMPAD5 must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_NUMPAD6 == DIK_NUMPAD6, "KEYSCAN_NUMPAD6 must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_NUMPADPLUS == DIK_NUMPADPLUS, "KEYSCAN_NUMPADPLUS must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_NUMPAD1 == DIK_NUMPAD1, "KEYSCAN_NUMPAD1 must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_NUMPAD2 == DIK_NUMPAD2, "KEYSCAN_NUMPAD2 must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_NUMPAD3 == DIK_NUMPAD3, "KEYSCAN_NUMPAD3 must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_NUMPAD0 == DIK_NUMPAD0, "KEYSCAN_NUMPAD0 must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_NUMPADPERIOD == DIK_NUMPADPERIOD, "KEYSCAN_NUMPADPERIOD must match DirectInput");
#ifdef DIK_OEM_102
STATIC_ASSERT_ALWAYS(KEYSCAN_OEM_102 == DIK_OEM_102, "KEYSCAN_OEM_102 must match DirectInput");
#endif
STATIC_ASSERT_ALWAYS(KEYSCAN_F11 == DIK_F11, "KEYSCAN_F11 must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_F12 == DIK_F12, "KEYSCAN_F12 must match DirectInput");
#ifdef DIK_KANA
STATIC_ASSERT_ALWAYS(KEYSCAN_KANA == DIK_KANA, "KEYSCAN_KANA must match DirectInput");
#endif
#ifdef DIK_CONVERT
STATIC_ASSERT_ALWAYS(KEYSCAN_CONVERT == DIK_CONVERT, "KEYSCAN_CONVERT must match DirectInput");
#endif
#ifdef DIK_NOCONVERT
STATIC_ASSERT_ALWAYS(KEYSCAN_NOCONVERT == DIK_NOCONVERT, "KEYSCAN_NOCONVERT must match DirectInput");
#endif
#ifdef DIK_YEN
STATIC_ASSERT_ALWAYS(KEYSCAN_YEN == DIK_YEN, "KEYSCAN_YEN must match DirectInput");
#endif
#ifdef DIK_CIRCUMFLEX
STATIC_ASSERT_ALWAYS(KEYSCAN_CIRCUMFLEX == DIK_CIRCUMFLEX, "KEYSCAN_CIRCUMFLEX must match DirectInput");
#endif
#ifdef DIK_KANJI
STATIC_ASSERT_ALWAYS(KEYSCAN_KANJI == DIK_KANJI, "KEYSCAN_KANJI must match DirectInput");
#endif
STATIC_ASSERT_ALWAYS(KEYSCAN_NUMPADENTER == DIK_NUMPADENTER, "KEYSCAN_NUMPADENTER must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_RCONTROL == DIK_RCONTROL, "KEYSCAN_RCONTROL must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_NUMPADSLASH == DIK_NUMPADSLASH, "KEYSCAN_NUMPADSLASH must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_SYSRQ == DIK_SYSRQ, "KEYSCAN_SYSRQ must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_RALT == DIK_RALT, "KEYSCAN_RALT must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_HOME == DIK_HOME, "KEYSCAN_HOME must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_UPARROW == DIK_UPARROW, "KEYSCAN_UPARROW must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_PGUP == DIK_PGUP, "KEYSCAN_PGUP must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_LEFTARROW == DIK_LEFTARROW, "KEYSCAN_LEFTARROW must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_RIGHTARROW == DIK_RIGHTARROW, "KEYSCAN_RIGHTARROW must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_END == DIK_END, "KEYSCAN_END must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_DOWNARROW == DIK_DOWNARROW, "KEYSCAN_DOWNARROW must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_PGDN == DIK_PGDN, "KEYSCAN_PGDN must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_INSERT == DIK_INSERT, "KEYSCAN_INSERT must match DirectInput");
STATIC_ASSERT_ALWAYS(KEYSCAN_DELETE == DIK_DELETE, "KEYSCAN_DELETE must match DirectInput");

#endif // _WIN32
