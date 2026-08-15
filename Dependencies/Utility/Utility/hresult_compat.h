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

// TheSuperHackers @port The COM result code the download and patch code returns, under the
// Win32 spelling. See docs/porting/online-absent-menu-seam.md.
//
// HRESULT here is not COM: nothing calls CoCreateInstance, no interface is registered, and
// no value ever crosses a process boundary. The download library uses it as its own error
// enumeration, built out of MAKE_HRESULT and compared against S_OK. That is a plain 32-bit
// signed integer with a sign-bit-means-failure convention, so it ports as itself.
//
// On Windows this header is empty: the callers get HRESULT from <windows.h> exactly as they
// did before, so the Windows preprocessed output is unchanged.
#pragma once

#ifndef _WIN32

#include <Utility/stdint_adapter.h>

// A repeated identical typedef is legal, which is what makes this safe next to a windows.h
// that has already spelled the same type (the native-port harness shim does).
typedef int32_t HRESULT;

#ifndef S_OK
#define S_OK ((HRESULT)0L)
#endif
#ifndef S_FALSE
#define S_FALSE ((HRESULT)1L)
#endif
#ifndef E_FAIL
#define E_FAIL ((HRESULT)0x80004005L)
#endif

#ifndef SUCCEEDED
#define SUCCEEDED(hr) (((HRESULT)(hr)) >= 0)
#endif
#ifndef FAILED
#define FAILED(hr) (((HRESULT)(hr)) < 0)
#endif

#ifndef SEVERITY_SUCCESS
#define SEVERITY_SUCCESS 0
#endif
#ifndef SEVERITY_ERROR
#define SEVERITY_ERROR 1
#endif

// FACILITY_ITF is the "interface specific" facility: the range Win32 reserves for codes whose
// meaning is defined by the interface returning them, which is how the download library uses it.
#ifndef FACILITY_ITF
#define FACILITY_ITF 4
#endif

#ifndef MAKE_HRESULT
#define MAKE_HRESULT(severity, facility, code) \
	((HRESULT)(((uint32_t)(severity) << 31) | ((uint32_t)(facility) << 16) | ((uint32_t)(code))))
#endif

#endif // !_WIN32
