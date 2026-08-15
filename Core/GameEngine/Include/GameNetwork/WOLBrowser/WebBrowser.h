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

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////

/******************************************************************************
*
* NAME
*     $Archive:  $
*
* DESCRIPTION
*     Web Browser
*
* PROGRAMMER
*     Bryan Cleveland
*     $Author:  $
*
* VERSION INFO
*     $Revision:  $
*     $Modtime:  $
*
******************************************************************************/

#pragma once

#include "Common/SubsystemInterface.h"
#include <windows.h>
#include <Common/GameMemory.h>
#include <Lib/BaseType.h>

// The embedded browser is EA's in-process Internet Explorer control, reached through the
// BrowserDispatch COM server that only exists in the Windows configuration. RTS_HAS_EMBEDDED_BROWSER
// is defined by Core/Libraries/Source/EABrowserDispatch when that server is built; where it is not,
// the control is absent and WebBrowser is a browser-less subsystem that still carries the Webpages
// INI table. See docs/porting/embedded-browser-seam.md.
#ifdef RTS_HAS_EMBEDDED_BROWSER
// TheSuperHackers @port ATL compatibility used to arrive via PreRTS.h, which no longer includes
// platform headers; see docs/porting/prerts-win32-surgery.md
#if defined __MINGW32__
#include "Utility/atl_compat.h"
#endif
#include <atlbase.h>
#include "EABrowserDispatch/BrowserDispatch.h"
#include "FEBDispatch.h"
#endif // RTS_HAS_EMBEDDED_BROWSER

class GameWindow;

class WebBrowserURL : public MemoryPoolObject
{
	MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE( WebBrowserURL, "WebBrowserURL" )

public:

	WebBrowserURL();
	// virtual destructor prototype defined by memory pool object

	const FieldParse *getFieldParse() const { return m_URLFieldParseTable; }

	AsciiString m_tag;
	AsciiString m_url;

	WebBrowserURL *m_next;

	static const FieldParse m_URLFieldParseTable[];		///< the parse table for INI definition

};



#ifdef RTS_HAS_EMBEDDED_BROWSER

class WebBrowser :
		public FEBDispatch<WebBrowser, IBrowserDispatch, &IID_IBrowserDispatch>,
		public SubsystemInterface
	{
	public:
		virtual void init() override;
		virtual void reset() override;
		virtual void update() override;

		// Create an instance of the embedded browser for Dune Emperor.
		virtual Bool createBrowserWindow(const char *tag, GameWindow *win) = 0;
		virtual void closeBrowserWindow(GameWindow *win) = 0;

		WebBrowserURL *makeNewURL(AsciiString tag);
		WebBrowserURL *findURL(AsciiString tag);

	protected:
		// Protected to prevent direct construction via new, use CreateInstance() instead.
		WebBrowser();
		virtual ~WebBrowser() override;

		// Protected to prevent copy and assignment
		WebBrowser(const WebBrowser&);
		const WebBrowser& operator=(const WebBrowser&);

//		Bool RetrievePageURL(const char* page, char* url, int size);
//		Bool RetrieveHTMLPath(char* path, int size);

	protected:
		ULONG mRefCount;
		WebBrowserURL *m_urlList;

	//---------------------------------------------------------------------------
	// IUnknown methods
	//---------------------------------------------------------------------------
	protected:
		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) IUNKNOWN_NOEXCEPT override;
		ULONG STDMETHODCALLTYPE AddRef() IUNKNOWN_NOEXCEPT override;
		ULONG STDMETHODCALLTYPE Release() IUNKNOWN_NOEXCEPT override;

	//---------------------------------------------------------------------------
	// IBrowserDispatch methods
	//---------------------------------------------------------------------------
	public:
		STDMETHOD(TestMethod)(Int num1);
	};

extern CComObject<WebBrowser> *TheWebBrowser;

#else // !RTS_HAS_EMBEDDED_BROWSER

/**
	* The browser-less WebBrowser: same subsystem, same Webpages INI table, no browser. Every
	* consumer already guards on TheWebBrowser being null (retail leaves it null -- GameEngine.cpp's
	* initSubsystem call is commented out), and the factory that would have created one returns
	* nothing in this configuration, so createBrowserWindow() is reached only by a caller that
	* constructed one itself. It complains and opens nothing rather than pretending to browse.
	*/
class WebBrowser : public SubsystemInterface
	{
	public:
		virtual void init() override;
		virtual void reset() override;
		virtual void update() override;

		virtual Bool createBrowserWindow(const char *tag, GameWindow *win);
		virtual void closeBrowserWindow(GameWindow *win);

		WebBrowserURL *makeNewURL(AsciiString tag);
		WebBrowserURL *findURL(AsciiString tag);

	protected:
		WebBrowser();
		virtual ~WebBrowser() override;

		// Protected to prevent copy and assignment
		WebBrowser(const WebBrowser&);
		const WebBrowser& operator=(const WebBrowser&);

	protected:
		WebBrowserURL *m_urlList;
	};

extern WebBrowser *TheWebBrowser;

#endif // RTS_HAS_EMBEDDED_BROWSER
