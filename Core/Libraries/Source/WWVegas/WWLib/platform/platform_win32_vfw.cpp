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
 *                 Project Name : Westwood Library                                             *
 *                                                                                             *
 *  Video for Windows, for the one file that calls it: WW3D2/FramGrab.cpp, the developer AVI     *
 *  frame recorder behind WW3D::Start_Movie_Capture(). Every entry point here is a loud stub.    *
 *                                                                                             *
 *  This is a stub rather than an implementation on purpose. VfW is a Windows multimedia API     *
 *  with no portable equivalent; recording an AVI on macOS is AVFoundation, a different API      *
 *  with a different frame-submission model, and nothing about running a skirmish or a campaign  *
 *  mission depends on it. The precedent is the DbgHelp crash reporter                           *
 *  (docs/porting/debug-and-profile-libs.md): where a Windows-only facility is not on the path   *
 *  to running the game, the port keeps the call sites compiling, refuses at run time, and says  *
 *  so on stderr the first time it is reached.                                                   *
 *                                                                                              *
 *  What that means for a caller: AVIFileOpen() fails, so FramGrab.cpp's Open() returns its      *
 *  failure path and the recorder never starts. The stream calls below are therefore             *
 *  unreachable in practice and exist so that the recorder links; each still refuses rather      *
 *  than pretending a frame was written. See docs/porting/ww3d2-and-download-headers.md.          *
 *                                                                                              *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#include "WWLib/platform/platform_win32_compat.h"

#ifdef WWPLATFORM_WIN32_COMPAT

#if defined(__has_include)
#if __has_include(<vfw.h>)
#define WWPLATFORM_VFW_COMPAT 1
#endif
#endif

#ifdef WWPLATFORM_VFW_COMPAT

#include <vfw.h>

extern "C" {

void AVIFileInit()
{
	WWPlatform::Win32::Report_Stub("AVIFileInit",
		"there is no Video for Windows here; AVI frame capture is not implemented");
}


void AVIFileExit()
{
}


HRESULT AVIFileOpenA(PAVIFILE * file, LPCSTR name, UINT, const void *)
{
	if (file != nullptr) {
		*file = nullptr;
	}
	WWPlatform::Win32::Report_Stub("AVIFileOpenA", name);
	return AVIERR_UNSUPPORTED;
}


HRESULT AVIFileCreateStreamA(PAVIFILE, PAVISTREAM * stream, AVISTREAMINFOA *)
{
	if (stream != nullptr) {
		*stream = nullptr;
	}
	WWPlatform::Win32::Report_Stub("AVIFileCreateStreamA",
		"there is no AVI file to add a stream to");
	return AVIERR_UNSUPPORTED;
}


ULONG AVIFileRelease(PAVIFILE)
{
	/*
	**	Win32 returns the remaining reference count. Nothing was ever opened, so there is none.
	*/
	return 0;
}


HRESULT AVIStreamSetFormat(PAVISTREAM, LONG, void *, LONG)
{
	WWPlatform::Win32::Report_Stub("AVIStreamSetFormat", "there is no AVI stream to format");
	return AVIERR_UNSUPPORTED;
}


HRESULT AVIStreamWrite(PAVISTREAM, LONG, LONG, void *, LONG, DWORD, LONG * written,
	LONG * written_bytes)
{
	if (written != nullptr) {
		*written = 0;
	}
	if (written_bytes != nullptr) {
		*written_bytes = 0;
	}
	WWPlatform::Win32::Report_Stub("AVIStreamWrite", "there is no AVI stream to write to");
	return AVIERR_UNSUPPORTED;
}


ULONG AVIStreamRelease(PAVISTREAM)
{
	return 0;
}

}	// extern "C"

#endif // WWPLATFORM_VFW_COMPAT
#endif // WWPLATFORM_WIN32_COMPAT
