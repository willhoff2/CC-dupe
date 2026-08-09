// Declaration-only stand-in for scripts/native-port-probe.py. See README.md.
#pragma once

#include <windows.h>
#include <mmsystem.h>

typedef struct { DWORD dwFlags; LONG lStart, lLength; } AVISTREAMINFO;
typedef struct IAVIFile* PAVIFILE;
typedef struct IAVIStream* PAVISTREAM;

extern "C" {
void  AVIFileInit();
void  AVIFileExit();
HRESULT AVIFileOpenA(PAVIFILE*, LPCSTR, UINT, const void*);
ULONG AVIFileRelease(PAVIFILE);
}
