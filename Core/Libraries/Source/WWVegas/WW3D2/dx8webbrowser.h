/*
**	Command & Conquer Generals Zero Hour(tm)
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

//******************************************************************************************
//
// Earth And Beyond
// Copyright (c) 2002 Electronic Arts , Inc.  -  Westwood Studios
//
// File Name		: dx8webbrowser.h
// Description		: Implementation of D3D Embedded Browser Wrapper
// Author			: Darren Schueller
// Date of Creation	: 6/4/2002
//
//******************************************************************************************
// $Header: $
//******************************************************************************************

#pragma once

// ***********************************
// Set this to 0 to remove all embedded browser code.
//
// The control is Internet Explorer hosted in a COM server (BrowserEngine) that exists only where
// RTS_HAS_EMBEDDED_BROWSER is defined, i.e. where EABrowserDispatch builds it. Where it is absent
// the class keeps its name and every entry point, so DX8Wrapper and W3DDisplay compile and link
// unchanged; see docs/porting/embedded-browser-seam.md.
//
#ifdef RTS_HAS_EMBEDDED_BROWSER
#define ENABLE_EMBEDDED_BROWSER		1
#else
#define ENABLE_EMBEDDED_BROWSER		0
#endif
//
// ***********************************

#if ENABLE_EMBEDDED_BROWSER
#include <windows.h>
#include "d3d8.h"
#endif

// These options must match the browser option bits defined in the BrowserEngine code.
// Look in febrowserengine.h
#define BROWSEROPTION_SCROLLBARS		0x0001
#define BROWSEROPTION_3DBORDER		0x0002

#if ENABLE_EMBEDDED_BROWSER
struct IDirect3DDevice8;

// The browser's option bits and the game-side scripting object it is handed. Spelled through the
// class' own names so the declaration below is one declaration on both platforms: the OLE
// IDispatch the control really wants, or the opaque pointer it is where there is no OLE.
typedef LONG			DX8WebBrowserOptions;
typedef LPDISPATCH	DX8WebBrowserDispatch;
#else
typedef long			DX8WebBrowserOptions;
typedef void *			DX8WebBrowserDispatch;
#endif

/**
** DX8WebBrowser
**
** DX8 interface wrapper class.  This encapsulates the BrowserEngine interface.
*/
class DX8WebBrowser
{
public:

	static bool			Initialize(	const char* badpageurl = 0,
											const char* loadingpageurl = 0,
											const char* mousefilename = 0,
											const char* mousebusyfilename = 0);			//Initialize the Embedded Browser

	static void			Shutdown();			// Shutdown the embedded browser.  Will close any open browsers.

	static void			Update();				// Copies all browser contexts to D3D Image surfaces.
	static void			Render(int backbufferindex);	//Draws all browsers to the backbuffer.

	// Creates a browser with the specified name
	static void			CreateBrowser(const char* browsername, const char* url, int x, int y, int w, int h, int updateticks = 0, DX8WebBrowserOptions options = BROWSEROPTION_SCROLLBARS | BROWSEROPTION_3DBORDER, DX8WebBrowserDispatch gamedispatch = 0);

	// Destroys the browser with the specified name
	static void			DestroyBrowser(const char* browsername);

	// Returns true if a browser with the specified name is open.
	static bool			Is_Browser_Open(const char* browsername);

	// Navigates the specified browser to the specified page.
	static void			Navigate(const char* browsername, const char* url);

#if ENABLE_EMBEDDED_BROWSER
private:
	// The window handle of the application.  This is initialized by Initialize().
	static				HWND						hWnd;
#endif
};
