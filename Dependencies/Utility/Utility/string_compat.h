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

// This file contains string macros and alias functions to help compiling on non-windows platforms
#pragma once
#include <ctype.h>
#include <string.h>
#include <strings.h>

typedef const char* LPCSTR;
typedef char* LPSTR;

// String functions
//
// TheSuperHackers @port C linkage, not C++: the GameSpy SDK declares `_strlwr` and `_strupr`
// itself for non-Windows targets, inside its own `extern "C"` block. A C++-linkage definition here
// makes every translation unit that reaches both declarations fail with "different language
// linkage", which off Windows is most of GameEngine.
#ifdef __cplusplus
extern "C" {
#endif

inline char *_strlwr(char *str) {
  for (int i = 0; str[i] != '\0'; i++) {
    str[i] = tolower(str[i]);
  }
  return str;
}

inline char *_strupr(char *str) {
  for (int i = 0; str[i] != '\0'; i++) {
    str[i] = toupper(str[i]);
  }
  return str;
}

#ifdef __cplusplus
}
#endif

#define strlwr _strlwr
#define strupr _strupr
#define stricmp strcasecmp
#define strnicmp strncasecmp
#define strcmpi strcasecmp

// TheSuperHackers @port The underscore-prefixed MSVC spellings. The engine uses both; they used
// to be supplied off Windows by the probe's stand-in <windows.h>, which real code cannot rely on.
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#define _strcmpi strcasecmp
#define _strdup strdup

// TheSuperHackers @port The kernel32 string entry points. The W3D renderer calls them qualified,
// as `::lstrcpy` / `::lstrcmpi` / ... (WW3D2/part_ldr.cpp includes WWLib/win.h for exactly this),
// so a macro alias is not enough: they have to exist as names in the global namespace. They are
// spellings of the C library rather than Win32 functionality; `lstrcpyn`'s count includes the
// terminator, which is the one behavioural difference from `strncpy`.
#ifdef __cplusplus
inline char *lstrcpy(char *dest, const char *src) { return strcpy(dest, src); }
inline char *lstrcat(char *dest, const char *src) { return strcat(dest, src); }
inline int lstrcmp(const char *a, const char *b) { return strcmp(a, b); }
inline int lstrcmpi(const char *a, const char *b) { return strcasecmp(a, b); }
inline int lstrlen(const char *str) { return str != 0 ? (int)strlen(str) : 0; }

inline char *lstrcpyn(char *dest, const char *src, int count) {
  if (count <= 0) {
    return dest;
  }
  strncpy(dest, src, (size_t)count - 1);
  dest[count - 1] = '\0';
  return dest;
}
#endif

