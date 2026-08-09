// Declaration-only stand-in for scripts/native-port-probe.py. See README.md.
#pragma once

#include <windows.h>

typedef struct _ITEMIDLIST { BYTE mkid[1]; } ITEMIDLIST, *LPITEMIDLIST;
typedef const ITEMIDLIST* LPCITEMIDLIST;

#define CSIDL_PERSONAL      0x0005
#define CSIDL_APPDATA       0x001a
#define CSIDL_MYDOCUMENTS   0x000c
#define CSIDL_MYPICTURES    0x0027
#define CSIDL_COMMON_APPDATA 0x0023
#define CSIDL_DESKTOPDIRECTORY 0x0010
#define CSIDL_FLAG_CREATE   0x8000

extern "C" {
BOOL    SHGetSpecialFolderPathA(HWND, LPSTR, int, BOOL);
HRESULT SHGetFolderPathA(HWND, int, HANDLE, DWORD, LPSTR);
HRESULT SHGetSpecialFolderLocation(HWND, int, LPITEMIDLIST*);
BOOL    SHGetPathFromIDListA(LPCITEMIDLIST, LPSTR);
}
#define SHGetSpecialFolderPath SHGetSpecialFolderPathA
#define SHGetFolderPath       SHGetFolderPathA
#define SHGetPathFromIDList   SHGetPathFromIDListA
