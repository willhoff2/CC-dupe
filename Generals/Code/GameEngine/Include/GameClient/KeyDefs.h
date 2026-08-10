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

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////

// FILE: KeyDefs.h ////////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------
//
//                       Westwood Studios Pacific.
//
//                       Confidential Information
//                Copyright (C) 2001 - All Rights Reserved
//
//-----------------------------------------------------------------------------
//
// Project:    RTS3
//
// File name:  KeyDefs.h
//
// Created:    Mike Morrison, 1995
//						 Colin Day, June 2001
//
// Desc:       Basic keyboard key definitions.
//
/** @todo NOTE: These key definitions are currently tied directly to the
*		Direct Input key codes, therefore making these definitions device
*		dependent even though this code lives on the device INdependent side
*		of the engine.  In the future to be truly device independent we
*		need to define our own key codes, and have a translation between
*		what we read from the device to our own system*/
//
//-----------------------------------------------------------------------------
///////////////////////////////////////////////////////////////////////////////

#pragma once

// SYSTEM INCLUDES ////////////////////////////////////////////////////////////
#include <stdlib.h>
#include <GameClient/KeyScanCodes.h>
#include <Lib/BaseType.h>

// USER INCLUDES //////////////////////////////////////////////////////////////

// FORWARD REFERENCES /////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////
// TYPE DEFINES ///////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

//=============================================================================
/** The key tables */
//=============================================================================

enum KeyDefType CPP_11(: UnsignedByte)
{
	// keypad keys ----------------------------------------------------------------
	KEY_KP0 								= KEYSCAN_NUMPAD0,
	KEY_KP1 								= KEYSCAN_NUMPAD1,
	KEY_KP2 								= KEYSCAN_NUMPAD2,
	KEY_KP3 								= KEYSCAN_NUMPAD3,
	KEY_KP4 								= KEYSCAN_NUMPAD4,
	KEY_KP5 								= KEYSCAN_NUMPAD5,
	KEY_KP6 								= KEYSCAN_NUMPAD6,
	KEY_KP7 								= KEYSCAN_NUMPAD7,
	KEY_KP8 								= KEYSCAN_NUMPAD8,
	KEY_KP9 								= KEYSCAN_NUMPAD9,
	KEY_KPDEL 							= KEYSCAN_NUMPADPERIOD,
	KEY_KPSTAR 							= KEYSCAN_NUMPADSTAR,
	KEY_KPMINUS 						= KEYSCAN_NUMPADMINUS,
	KEY_KPPLUS 							= KEYSCAN_NUMPADPLUS,

	// regular keys ---------------------------------------------------------------
	KEY_ESC 								= KEYSCAN_ESCAPE,
	KEY_BACKSPACE 					= KEYSCAN_BACK,
	KEY_ENTER 							= KEYSCAN_RETURN,
	KEY_SPACE 							= KEYSCAN_SPACE,
	KEY_TAB 								= KEYSCAN_TAB,
	KEY_F1 									= KEYSCAN_F1,
	KEY_F2 									= KEYSCAN_F2,
	KEY_F3 									= KEYSCAN_F3,
	KEY_F4 									= KEYSCAN_F4,
	KEY_F5 									= KEYSCAN_F5,
	KEY_F6 									= KEYSCAN_F6,
	KEY_F7 									= KEYSCAN_F7,
	KEY_F8 									= KEYSCAN_F8,
	KEY_F9 									= KEYSCAN_F9,
	KEY_F10 								= KEYSCAN_F10,
	KEY_F11 								= KEYSCAN_F11,
	KEY_F12 								= KEYSCAN_F12,
	KEY_A 									= KEYSCAN_A,
	KEY_B 									= KEYSCAN_B,
	KEY_C 									= KEYSCAN_C,
	KEY_D 									= KEYSCAN_D,
	KEY_E 									= KEYSCAN_E,
	KEY_F 									= KEYSCAN_F,
	KEY_G 									= KEYSCAN_G,
	KEY_H 									= KEYSCAN_H,
	KEY_I 									= KEYSCAN_I,
	KEY_J 									= KEYSCAN_J,
	KEY_K 									= KEYSCAN_K,
	KEY_L 									= KEYSCAN_L,
	KEY_M 									= KEYSCAN_M,
	KEY_N 									= KEYSCAN_N,
	KEY_O 									= KEYSCAN_O,
	KEY_P 									= KEYSCAN_P,
	KEY_Q 									= KEYSCAN_Q,
	KEY_R 									= KEYSCAN_R,
	KEY_S 									= KEYSCAN_S,
	KEY_T 									= KEYSCAN_T,
	KEY_U 									= KEYSCAN_U,
	KEY_V 									= KEYSCAN_V,
	KEY_W 									= KEYSCAN_W,
	KEY_X 									= KEYSCAN_X,
	KEY_Y 									= KEYSCAN_Y,
	KEY_Z 									= KEYSCAN_Z,
	KEY_1 									= KEYSCAN_1,
	KEY_2 									= KEYSCAN_2,
	KEY_3 									= KEYSCAN_3,
	KEY_4 									= KEYSCAN_4,
	KEY_5 									= KEYSCAN_5,
	KEY_6 									= KEYSCAN_6,
	KEY_7 									= KEYSCAN_7,
	KEY_8 									= KEYSCAN_8,
	KEY_9 									= KEYSCAN_9,
	KEY_0 									= KEYSCAN_0,
	KEY_MINUS 							= KEYSCAN_MINUS,
	KEY_EQUAL 							= KEYSCAN_EQUALS,
	KEY_LBRACKET 						= KEYSCAN_LBRACKET,
	KEY_RBRACKET 						= KEYSCAN_RBRACKET,
	KEY_SEMICOLON 					= KEYSCAN_SEMICOLON,
	KEY_APOSTROPHE 					= KEYSCAN_APOSTROPHE,
	KEY_TICK 								= KEYSCAN_GRAVE,
	KEY_BACKSLASH 					= KEYSCAN_BACKSLASH,
	KEY_COMMA 							= KEYSCAN_COMMA,
	KEY_PERIOD 							= KEYSCAN_PERIOD,
	KEY_SLASH 							= KEYSCAN_SLASH,

	// special keys ---------------------------------------------------------------
	KEY_SYSREQ 							= KEYSCAN_SYSRQ,

	KEY_CAPS 								= KEYSCAN_CAPSLOCK,
	KEY_NUM 								= KEYSCAN_NUMLOCK,
	KEY_SCROLL 							= KEYSCAN_SCROLL,
	KEY_LCTRL 							= KEYSCAN_LCONTROL,
	KEY_LALT 								= KEYSCAN_LALT,
	KEY_LSHIFT 							= KEYSCAN_LSHIFT,
	KEY_RSHIFT 							= KEYSCAN_RSHIFT,

	KEY_UP 									= KEYSCAN_UPARROW,
	KEY_DOWN 								= KEYSCAN_DOWNARROW,
	KEY_LEFT 								= KEYSCAN_LEFTARROW,
	KEY_RIGHT 							= KEYSCAN_RIGHTARROW,
	KEY_RALT 								= KEYSCAN_RALT,
	KEY_RCTRL 							= KEYSCAN_RCONTROL,
	KEY_HOME 								= KEYSCAN_HOME,
	KEY_END 								= KEYSCAN_END,
	KEY_PGUP 								= KEYSCAN_PGUP,
	KEY_PGDN 								= KEYSCAN_PGDN,
	KEY_INS 								= KEYSCAN_INSERT,
	KEY_DEL 								= KEYSCAN_DELETE,
	KEY_KPENTER 						= KEYSCAN_NUMPADENTER,
	KEY_KPSLASH 						= KEYSCAN_NUMPADSLASH,

	KEY_102 								= KEYSCAN_OEM_102,

	// Japanese keyboard keys -----------------------------------------------------
	KEY_KANA 								= KEYSCAN_KANA,
	KEY_CONVERT 						= KEYSCAN_CONVERT,
	KEY_NOCONVERT 					= KEYSCAN_NOCONVERT,
	KEY_YEN 								= KEYSCAN_YEN,
	KEY_CIRCUMFLEX 					= KEYSCAN_CIRCUMFLEX,
	KEY_KANJI 							= KEYSCAN_KANJI,

	// specials -------------------------------------------------------------------
	KEY_NONE								= 0x00,		///< to report end of key stream
	KEY_LOST								= 0xFF,		///< to report lost keyboard focus

};

enum
{
	KEY_COUNT = 256
};

// state for keyboard IO ------------------------------------------------------
enum
{
	KEY_STATE_NONE								= 0x0000, // No modifier state
	KEY_STATE_UP									= 0x0001,	// Key is up (default state)
	KEY_STATE_DOWN								= 0x0002,	// Key is down
	KEY_STATE_LCONTROL						= 0x0004,	// Left control is pressed
	KEY_STATE_RCONTROL						= 0x0008,	// Right control is pressed
	KEY_STATE_LSHIFT							= 0x0010,	// left shift is pressed
	KEY_STATE_RSHIFT							= 0x0020,	// right shift is pressed
	KEY_STATE_LALT								= 0x0040,	// left alt is pressed
	KEY_STATE_RALT								= 0x0080,	// right alt is pressed
	KEY_STATE_AUTOREPEAT					= 0x0100,	// Key is down due to autorepeat (only seen in conjunction with KEY_STATE_DOWN)
	KEY_STATE_CAPSLOCK						= 0x0200, // Caps Lock key is on.
	KEY_STATE_SHIFT2							= 0x0400, // Alternate shift key is pressed (I think this is for foreign keyboards..)

	// modifier combinations when left/right isn't a factor
	KEY_STATE_CONTROL		= (KEY_STATE_LCONTROL | KEY_STATE_RCONTROL),
	KEY_STATE_SHIFT			= (KEY_STATE_LSHIFT | KEY_STATE_RSHIFT | KEY_STATE_SHIFT2 ),
	KEY_STATE_ALT				= (KEY_STATE_LALT | KEY_STATE_RALT)

};

// INLINING ///////////////////////////////////////////////////////////////////

// EXTERNALS //////////////////////////////////////////////////////////////////
