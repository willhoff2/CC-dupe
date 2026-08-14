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

/***********************************************************************************************
 *                                                                                             *
 *                 Project Name : Westwood Library                                             *
 *                                                                                             *
 *  The Win32 entry points in this cluster that have no portable meaning at all, defined so the  *
 *  link completes and so that a run which reaches one says so on stderr instead of failing      *
 *  silently. Three groups, all of them off the path to running a single player game:            *
 *                                                                                             *
 *    PE resources (FindResource/LoadResource/LockResource/SizeofResource) -- rcfile.cpp reads   *
 *    RCFILE data out of the running image's resource section. An ELF or Mach-O binary has no    *
 *    such section, and no shipping asset is loaded this way; the data lives in .big archives.   *
 *                                                                                             *
 *    Version resources (GetFileVersionInfoSize/GetFileVersionInfo/VerQueryValue) -- verchk.cpp  *
 *    reads a VERSIONINFO resource out of a PE image to compare build versions. Windows-only by  *
 *    construction, as the port's <windows.h> already notes.                                    *
 *                                                                                             *
 *    OLE/COM (OleInitialize/OleUninitialize/LoadTypeLib/CreateStdDispatch/SysFreeString) -- the *
 *    embedded WOL browser and its IDispatch plumbing. Online play is explicitly out of scope    *
 *    for the native port, so the seam here is an honest refusal rather than a reimplementation. *
 *                                                                                             *
 *  Each of these has a caller that already handles failure, so the stub returns the failure     *
 *  value rather than aborting: crash reporting, version checks and the browser are all things   *
 *  the engine survives losing.                                                                 *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#include "WWLib/platform/platform_win32_compat.h"

#ifdef WWPLATFORM_WIN32_COMPAT

#include <stdlib.h>

/*
**	The OLE entry points are declared by the port's <oaidl.h> rather than by <windows.h>, and
**	BSTR is wchar_t* there.
*/
#include <oaidl.h>

extern "C" {

/***********************************************************************************************
 *                                                                                             *
 *  PE resources.                                                                               *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

HRSRC FindResourceA(HMODULE, LPCSTR, LPCSTR)
{
	WWPlatform::Win32::Report_Stub("FindResourceA",
		"there is no PE resource section here; RCFILE data cannot be found");
	WWPlatform::Win32::Set_Last_Error(ERROR_FILE_NOT_FOUND);
	return nullptr;
}


HGLOBAL LoadResource(HMODULE, HRSRC)
{
	WWPlatform::Win32::Report_Stub("LoadResource", "there is no PE resource section here");
	WWPlatform::Win32::Set_Last_Error(ERROR_FILE_NOT_FOUND);
	return nullptr;
}


LPVOID LockResource(HGLOBAL)
{
	return nullptr;
}


DWORD SizeofResource(HMODULE, HRSRC)
{
	return 0;
}


/***********************************************************************************************
 *                                                                                             *
 *  Version resources.                                                                          *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

DWORD GetFileVersionInfoSizeA(LPCSTR, LPDWORD handle)
{
	WWPlatform::Win32::Report_Stub("GetFileVersionInfoSizeA",
		"a VERSIONINFO resource only exists in a PE image; version checks report no data");
	if (handle != nullptr) {
		*handle = 0;
	}
	return 0;
}


BOOL GetFileVersionInfoA(LPCSTR, DWORD, DWORD, LPVOID)
{
	WWPlatform::Win32::Report_Stub("GetFileVersionInfoA",
		"a VERSIONINFO resource only exists in a PE image");
	return FALSE;
}


BOOL VerQueryValueA(LPCVOID, LPCSTR, LPVOID * value, PUINT length)
{
	if (value != nullptr) {
		*value = nullptr;
	}
	if (length != nullptr) {
		*length = 0;
	}
	return FALSE;
}


/***********************************************************************************************
 *                                                                                             *
 *  OLE/COM.                                                                                    *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

HRESULT OleInitialize(LPVOID)
{
	WWPlatform::Win32::Report_Stub("OleInitialize",
		"there is no COM apartment here; the embedded browser is out of scope");
	return E_NOTIMPL;
}


void OleUninitialize()
{
}


HRESULT LoadTypeLib(LPCOLESTR, ITypeLib ** library)
{
	WWPlatform::Win32::Report_Stub("LoadTypeLib",
		"there are no type libraries here; IDispatch plumbing is out of scope");
	if (library != nullptr) {
		*library = nullptr;
	}
	return E_NOTIMPL;
}


HRESULT CreateStdDispatch(IUnknown *, void *, ITypeInfo *, IUnknown ** dispatch)
{
	WWPlatform::Win32::Report_Stub("CreateStdDispatch",
		"there is no COM dispatch implementation here");
	if (dispatch != nullptr) {
		*dispatch = nullptr;
	}
	return E_NOTIMPL;
}


/*
**	SysFreeString() is reached from _bstr_t's destructor, which is inline in a header and so gets
**	instantiated even where no COM call ever happens. It does nothing rather than free(): the
**	strings come from SysAllocString()/ConvertStringToBSTR(), neither of which has an
**	implementation here, so there is no allocator to hand the pointer back to.
*/
void SysFreeString(BSTR)
{
}

}	// extern "C"

#endif	// WWPLATFORM_WIN32_COMPAT
