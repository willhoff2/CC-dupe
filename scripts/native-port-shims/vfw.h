// Declaration-only stand-in for scripts/native-port-probe.py. See README.md.
//
// Video for Windows, for `FramGrab.cpp`'s AVI recorder. VfW is a Windows multimedia API, the
// recorder is a developer capture facility rather than something the game needs to run, and the
// replacement on macOS would be a different API entirely -- so the port answers it the way it
// answers the DbgHelp crash reporter: the entry points declared here are defined, as loud stubs
// that refuse and print, in WWLib/platform/platform_win32_vfw.cpp, which is what keeps the native
// link clean. See docs/porting/ww3d2-and-download-headers.md.
//
// `AVISTREAMINFO` is spelled out in full rather than approximated, because `FramGrab.cpp` assigns
// to fifteen of its members and the point of compiling the file is to type-check those.
#pragma once

#include <windows.h>
#include <mmsystem.h>

typedef struct {
	DWORD fccType;
	DWORD fccHandler;
	DWORD dwFlags;
	DWORD dwCaps;
	WORD  wPriority;
	WORD  wLanguage;
	DWORD dwScale;
	DWORD dwRate;
	DWORD dwStart;
	DWORD dwLength;
	DWORD dwInitialFrames;
	DWORD dwSuggestedBufferSize;
	DWORD dwQuality;
	DWORD dwSampleSize;
	RECT  rcFrame;
	DWORD dwEditCount;
	DWORD dwFormatChangeCount;
	char  szName[64];
} AVISTREAMINFOA, *LPAVISTREAMINFOA;
typedef AVISTREAMINFOA AVISTREAMINFO;
typedef LPAVISTREAMINFOA LPAVISTREAMINFO;

typedef struct IAVIFile* PAVIFILE;
typedef struct IAVIStream* PAVISTREAM;

/* Stream types, as four-character codes. */
#define streamtypeVIDEO mmioFOURCC('v', 'i', 'd', 's')
#define streamtypeAUDIO mmioFOURCC('a', 'u', 'd', 's')

/* The one AVI error code the stub implementation returns. */
#define AVIERR_UNSUPPORTED ((HRESULT)0x80044065L)

/* Flags of the AVI index entry a written sample produces. */
#define AVIIF_LIST      0x00000001L
#define AVIIF_KEYFRAME  0x00000010L
#define AVIIF_NOTIME    0x00000100L

extern "C" {
void    AVIFileInit();
void    AVIFileExit();
HRESULT AVIFileOpenA(PAVIFILE*, LPCSTR, UINT, const void*);
HRESULT AVIFileCreateStreamA(PAVIFILE, PAVISTREAM*, AVISTREAMINFOA*);
ULONG   AVIFileRelease(PAVIFILE);
HRESULT AVIStreamSetFormat(PAVISTREAM, LONG, void*, LONG);
HRESULT AVIStreamWrite(PAVISTREAM, LONG, LONG, void*, LONG, DWORD, LONG*, LONG*);
ULONG   AVIStreamRelease(PAVISTREAM);
}
#define AVIFileOpen         AVIFileOpenA
#define AVIFileCreateStream AVIFileCreateStreamA
