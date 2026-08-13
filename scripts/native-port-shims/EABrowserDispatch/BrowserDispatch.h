// Declaration-only stand-in for scripts/native-port-probe.py. See README.md.
//
// `EABrowserDispatch/BrowserDispatch.h` is the interface header of EA's embedded Internet Explorer
// control -- a Windows-only in-process COM server that shipped as a separate DLL and is not in
// this repository at all, on any platform. WebBrowser.h derives from IBrowserDispatch through
// FEBDispatch<>, so six translation units cannot be parsed without it: INIWebpageURL.cpp,
// WebBrowser.cpp, GameEngine.cpp and the WOL ladder/login/welcome menus.
//
// CUT SCOPE. The embedded browser exists to render the WOL/GameSpy online screens; single-player
// Zero Hour never creates one (GameEngine.cpp's initSubsystem call for TheWebBrowser is commented
// out in the retail source). This declares the interface so those files compile, and nothing more:
//
//   * IID_IBrowserDispatch is declared, not defined. A native link that reaches it fails at link
//     time rather than silently talking to a browser that is not there -- which is the loud
//     failure this stub wants.
//   * The vtable layout is a guess. It is derived from WebBrowser.h's own override list
//     (TestMethod is the only method the engine implements), not from EA's IDL, so it must never
//     be used to talk to a real BrowserDispatch DLL.
//
// Replacing this means either an embedded web view per platform or deleting the online screens;
// see docs/porting/crt-and-widechar-compat.md.
#pragma once

#include <oaidl.h>

// {00000000-0000-0000-0000-000000000000} -- deliberately not EA's IID. Nothing off Windows can
// hand out this interface, so a matching GUID would only make a broken path look supported.
extern "C" const IID IID_IBrowserDispatch;

interface IBrowserDispatch : public IDispatch
{
	STDMETHOD(TestMethod)(int num1) PURE;
};
