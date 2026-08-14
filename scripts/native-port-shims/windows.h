// Declaration-only <windows.h> stand-in for scripts/native-port-probe.py.
// See README.md in this directory. Not a port; a measuring instrument.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>
#include <math.h>
#include <stdio.h>

// The repo already ships POSIX equivalents for a slice of the Win32 surface (timeGetTime,
// GetTickCount, Sleep, GetCurrentThreadId, _alloca, stricmp...). Defer to those rather than
// redeclaring them, so the shims and the real compat layer cannot drift apart.
#include <Utility/compat.h>

// MSVC CRT spellings live in this directory's <stdlib.h> / <stdio.h> stand-ins, not here: a real
// <windows.h> does not declare them either, and PreRTS.h no longer drags <windows.h> everywhere.

// ---------------------------------------------------------------- calling conventions
#define WINAPI
#define APIENTRY
#define CALLBACK
#define WINAPIV
#define PASCAL
#define FAR
#define NEAR
#define CONST const
#ifndef IN
#define IN
#endif
#ifndef OUT
#define OUT
#endif
#ifndef OPTIONAL
#define OPTIONAL
#endif
#define DECLSPEC_IMPORT
#define WINBASEAPI
#define WINUSERAPI
#define WINGDIAPI
#define WINOLEAPI
#define STDMETHODCALLTYPE
#define STDAPICALLTYPE
#define EXTERN_C extern "C"

// ---------------------------------------------------------------------- integral types
typedef int                 BOOL;
typedef unsigned char       BYTE;
typedef unsigned short      WORD;
// LLP64: `long` is 32-bit on Win64, so these are spelled with exact-width types rather
// than `long`, which is 64-bit on the LP64 host running the probe.
typedef uint32_t            DWORD;
typedef unsigned int        UINT;
typedef int                 INT;
typedef int32_t             LONG;
typedef uint32_t            ULONG;
typedef short               SHORT;
typedef unsigned short      USHORT;
typedef float               FLOAT;
typedef double              DOUBLE;
typedef char                CHAR;
typedef unsigned char       UCHAR;
typedef wchar_t             WCHAR;
typedef char*               PCHAR;
typedef wchar_t*            PWCHAR;
typedef void                VOID;
typedef unsigned char       BOOLEAN;

typedef int8_t              INT8;
typedef int16_t             INT16;
typedef int32_t             INT32;
typedef int64_t             INT64;
typedef uint8_t             UINT8;
typedef uint16_t            UINT16;
typedef uint32_t            UINT32;
typedef uint64_t            UINT64;
typedef int32_t             LONG32;
typedef int64_t             LONG64;
typedef uint32_t            ULONG32;
typedef uint64_t            ULONG64;
typedef uint64_t            DWORD64;
typedef uint64_t            DWORDLONG;
typedef int64_t             LONGLONG;
typedef uint64_t            ULONGLONG;

// Pointer-sized integers. LLP64: `long` stays 32-bit on Win64, so these follow the pointer,
// not `long`. Keeping this honest is what lets the probe see Phase 2 truncation bugs.
typedef intptr_t            INT_PTR;
typedef uintptr_t           UINT_PTR;
typedef intptr_t            LONG_PTR;
typedef uintptr_t           ULONG_PTR;
typedef uintptr_t           DWORD_PTR;
typedef intptr_t            SSIZE_T;
typedef size_t              SIZE_T;
typedef uintptr_t           WPARAM;
typedef intptr_t            LPARAM;
typedef intptr_t            LRESULT;

typedef void*               PVOID;
typedef void*               LPVOID;
typedef const void*         LPCVOID;
typedef char*               PSTR;
typedef char*               LPSTR;
typedef const char*         PCSTR;
typedef const char*         LPCSTR;
typedef wchar_t*            PWSTR;
typedef wchar_t*            LPWSTR;
typedef const wchar_t*      PCWSTR;
typedef const wchar_t*      LPCWSTR;
typedef BYTE*               PBYTE;
typedef BYTE*               LPBYTE;
typedef WORD*               PWORD;
typedef WORD*               LPWORD;
typedef DWORD*              PDWORD;
typedef DWORD*              LPDWORD;
typedef BOOL*               PBOOL;
typedef BOOL*               LPBOOL;
typedef LONG*               PLONG;
typedef LONG*               LPLONG;
typedef int*                PINT;
typedef int*                LPINT;
typedef UINT*               PUINT;
typedef ULONG*              PULONG;
typedef float*              PFLOAT;
typedef LONG                HRESULT;
typedef LONG                NTSTATUS;
typedef DWORD               COLORREF;
typedef DWORD*              LPCOLORREF;
typedef DWORD               LCID;
typedef WORD                LANGID;
typedef WORD                ATOM;
typedef DWORD               ACCESS_MASK;
typedef ACCESS_MASK         REGSAM;

#define TRUE  1
#define FALSE 0
#ifndef NULL
#define NULL 0
#endif
#define MAX_PATH 260

// -------------------------------------------------------------------------- handle types
#define DECLARE_HANDLE(name) struct name##__ { int unused; }; typedef struct name##__* name
typedef void* HANDLE;
typedef HANDLE* PHANDLE;
typedef HANDLE* LPHANDLE;
DECLARE_HANDLE(HWND);
DECLARE_HANDLE(HINSTANCE);
DECLARE_HANDLE(HDC);
DECLARE_HANDLE(HBITMAP);
DECLARE_HANDLE(HBRUSH);
DECLARE_HANDLE(HPEN);
DECLARE_HANDLE(HFONT);
DECLARE_HANDLE(HPALETTE);
DECLARE_HANDLE(HRGN);
DECLARE_HANDLE(HICON);
DECLARE_HANDLE(HMENU);
DECLARE_HANDLE(HCURSOR);
DECLARE_HANDLE(HKEY);
DECLARE_HANDLE(HKL);
DECLARE_HANDLE(HHOOK);
DECLARE_HANDLE(HRSRC);
// Not DECLARE_HANDLE: winnt.h really does typedef these to HANDLE, i.e. to void*, and code that
// passes a T* straight to GlobalFree() -- SystemAllocator.h's deallocate(), profile.cpp -- relies
// on the implicit conversion that a distinct handle struct would refuse.
typedef HANDLE HGLOBAL;
typedef HANDLE HLOCAL;
DECLARE_HANDLE(HGDIOBJ);
#ifndef HMONITOR_DECLARED
#define HMONITOR_DECLARED
DECLARE_HANDLE(HMONITOR);
#endif
DECLARE_HANDLE(HDROP);
DECLARE_HANDLE(HIMAGELIST);
DECLARE_HANDLE(HDESK);
DECLARE_HANDLE(HWINSTA);
DECLARE_HANDLE(HENHMETAFILE);
DECLARE_HANDLE(HINTERNET);
DECLARE_HANDLE(HGLRC);
typedef HINSTANCE HMODULE;
typedef HKEY* PHKEY;
typedef HANDLE HTREEITEM;
#define INVALID_HANDLE_VALUE ((HANDLE)(LONG_PTR)-1)

// ---------------------------------------------------------------------------- structures
// LARGE_INTEGER, QueryPerformanceCounter and QueryPerformanceFrequency come from
// Utility/time_compat.h, included above: the compat layer implements them for real now.

typedef union _ULARGE_INTEGER {
	struct { DWORD LowPart; DWORD HighPart; };
	struct { DWORD LowPart; DWORD HighPart; } u;
	ULONGLONG QuadPart;
} ULARGE_INTEGER, *PULARGE_INTEGER;

typedef struct _POINT { LONG x; LONG y; } POINT, *PPOINT, *LPPOINT;
typedef struct _POINTS { SHORT x; SHORT y; } POINTS;
typedef struct _SIZE { LONG cx; LONG cy; } SIZE, *PSIZE, *LPSIZE;
typedef struct _RECT { LONG left; LONG top; LONG right; LONG bottom; } RECT, *PRECT, *LPRECT;
typedef const RECT* LPCRECT;

typedef struct _FILETIME { DWORD dwLowDateTime; DWORD dwHighDateTime; } FILETIME, *PFILETIME, *LPFILETIME;
typedef struct _SYSTEMTIME {
	WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds;
} SYSTEMTIME, *PSYSTEMTIME, *LPSYSTEMTIME;

typedef struct _GUID { DWORD Data1; WORD Data2; WORD Data3; BYTE Data4[8]; } GUID;
typedef GUID IID, CLSID, UUID;
typedef short VARIANT_BOOL;
typedef GUID* LPGUID;
typedef const GUID& REFGUID;
typedef const IID& REFIID;
typedef const CLSID& REFCLSID;
// <guiddef.h> defines these as inline memcmp comparisons; WebBrowser.cpp's QueryInterface uses
// them on interface IIDs.
inline bool operator==(const GUID& a, const GUID& b)
{
	return a.Data1 == b.Data1 && a.Data2 == b.Data2 && a.Data3 == b.Data3 &&
	       __builtin_memcmp(a.Data4, b.Data4, sizeof(a.Data4)) == 0;
}
inline bool operator!=(const GUID& a, const GUID& b) { return !(a == b); }

typedef struct _SECURITY_ATTRIBUTES {
	DWORD nLength; LPVOID lpSecurityDescriptor; BOOL bInheritHandle;
} SECURITY_ATTRIBUTES, *LPSECURITY_ATTRIBUTES;

typedef struct _OVERLAPPED {
	ULONG_PTR Internal; ULONG_PTR InternalHigh; DWORD Offset; DWORD OffsetHigh; HANDLE hEvent;
} OVERLAPPED, *LPOVERLAPPED;

typedef struct _WIN32_FIND_DATAA {
	DWORD dwFileAttributes;
	FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
	DWORD nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1;
	CHAR cFileName[MAX_PATH]; CHAR cAlternateFileName[14];
} WIN32_FIND_DATAA, *LPWIN32_FIND_DATAA;
typedef WIN32_FIND_DATAA WIN32_FIND_DATA, *LPWIN32_FIND_DATA;

typedef struct _SYSTEM_INFO {
	union { DWORD dwOemId; struct { WORD wProcessorArchitecture; WORD wReserved; }; };
	DWORD dwPageSize; LPVOID lpMinimumApplicationAddress; LPVOID lpMaximumApplicationAddress;
	DWORD_PTR dwActiveProcessorMask; DWORD dwNumberOfProcessors; DWORD dwProcessorType;
	DWORD dwAllocationGranularity; WORD wProcessorLevel; WORD wProcessorRevision;
} SYSTEM_INFO, *LPSYSTEM_INFO;

typedef struct _OSVERSIONINFOA {
	DWORD dwOSVersionInfoSize, dwMajorVersion, dwMinorVersion, dwBuildNumber, dwPlatformId;
	CHAR szCSDVersion[128];
} OSVERSIONINFOA, *LPOSVERSIONINFOA;
typedef OSVERSIONINFOA OSVERSIONINFO, *LPOSVERSIONINFO;

// The fixed part of a Win32 VERSIONINFO resource. verchk.cpp memcpy()s one out of a PE image and
// compares dwFileVersionMS/LS, so the field order and the 32-bit widths are load-bearing.
typedef struct tagVS_FIXEDFILEINFO {
	DWORD dwSignature, dwStrucVersion;
	DWORD dwFileVersionMS, dwFileVersionLS, dwProductVersionMS, dwProductVersionLS;
	DWORD dwFileFlagsMask, dwFileFlags, dwFileOS, dwFileType, dwFileSubtype;
	DWORD dwFileDateMS, dwFileDateLS;
} VS_FIXEDFILEINFO;

typedef struct _MEMORYSTATUS {
	DWORD dwLength, dwMemoryLoad;
	SIZE_T dwTotalPhys, dwAvailPhys, dwTotalPageFile, dwAvailPageFile, dwTotalVirtual, dwAvailVirtual;
} MEMORYSTATUS, *LPMEMORYSTATUS;

typedef struct _MEMORYSTATUSEX {
	DWORD dwLength, dwMemoryLoad;
	DWORDLONG ullTotalPhys, ullAvailPhys, ullTotalPageFile, ullAvailPageFile,
	          ullTotalVirtual, ullAvailVirtual, ullAvailExtendedVirtual;
} MEMORYSTATUSEX, *LPMEMORYSTATUSEX;

typedef struct _MSG {
	HWND hwnd; UINT message; WPARAM wParam; LPARAM lParam; DWORD time; POINT pt;
} MSG, *PMSG, *LPMSG;

typedef struct _PAINTSTRUCT {
	HDC hdc; BOOL fErase; RECT rcPaint; BOOL fRestore, fIncUpdate; BYTE rgbReserved[32];
} PAINTSTRUCT, *LPPAINTSTRUCT;

typedef struct _RGBQUAD { BYTE rgbBlue, rgbGreen, rgbRed, rgbReserved; } RGBQUAD;
typedef struct _PALETTEENTRY { BYTE peRed, peGreen, peBlue, peFlags; } PALETTEENTRY, *LPPALETTEENTRY;

typedef struct _BITMAPINFOHEADER {
	DWORD biSize; LONG biWidth, biHeight; WORD biPlanes, biBitCount;
	DWORD biCompression, biSizeImage; LONG biXPelsPerMeter, biYPelsPerMeter;
	DWORD biClrUsed, biClrImportant;
} BITMAPINFOHEADER, *LPBITMAPINFOHEADER;
typedef struct _BITMAPINFO { BITMAPINFOHEADER bmiHeader; RGBQUAD bmiColors[1]; } BITMAPINFO, *LPBITMAPINFO;
typedef struct _BITMAPFILEHEADER {
	WORD bfType; DWORD bfSize; WORD bfReserved1, bfReserved2; DWORD bfOffBits;
} BITMAPFILEHEADER, *LPBITMAPFILEHEADER;

typedef struct _CRITICAL_SECTION {
	void* DebugInfo; LONG LockCount, RecursionCount; HANDLE OwningThread, LockSemaphore;
	ULONG_PTR SpinCount;
} CRITICAL_SECTION, *LPCRITICAL_SECTION;
typedef CRITICAL_SECTION RTL_CRITICAL_SECTION;

typedef struct _EXCEPTION_RECORD {
	DWORD ExceptionCode, ExceptionFlags; struct _EXCEPTION_RECORD* ExceptionRecord;
	PVOID ExceptionAddress; DWORD NumberParameters; ULONG_PTR ExceptionInformation[15];
} EXCEPTION_RECORD, *PEXCEPTION_RECORD;
typedef struct _CONTEXT { DWORD ContextFlags; ULONG_PTR Rip, Rsp, Rbp; } CONTEXT, *PCONTEXT;
typedef struct _EXCEPTION_POINTERS {
	PEXCEPTION_RECORD ExceptionRecord; PCONTEXT ContextRecord;
} EXCEPTION_POINTERS, *PEXCEPTION_POINTERS, *LPEXCEPTION_POINTERS;

typedef struct _STARTUPINFOA {
	DWORD cb; LPSTR lpReserved, lpDesktop, lpTitle;
	DWORD dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
	WORD wShowWindow, cbReserved2; LPBYTE lpReserved2; HANDLE hStdInput, hStdOutput, hStdError;
} STARTUPINFOA, *LPSTARTUPINFOA;
typedef STARTUPINFOA STARTUPINFO, *LPSTARTUPINFO;
typedef struct _PROCESS_INFORMATION {
	HANDLE hProcess, hThread; DWORD dwProcessId, dwThreadId;
} PROCESS_INFORMATION, *LPPROCESS_INFORMATION;

typedef struct tagWNDCLASSA {
	UINT style; LRESULT (*lpfnWndProc)(HWND, UINT, WPARAM, LPARAM); int cbClsExtra, cbWndExtra;
	HINSTANCE hInstance; HICON hIcon; HCURSOR hCursor; HBRUSH hbrBackground;
	LPCSTR lpszMenuName, lpszClassName;
} WNDCLASSA, *LPWNDCLASSA;
typedef WNDCLASSA WNDCLASS, *LPWNDCLASS;
typedef struct tagWNDCLASSEXA {
	UINT cbSize, style; LRESULT (*lpfnWndProc)(HWND, UINT, WPARAM, LPARAM);
	int cbClsExtra, cbWndExtra; HINSTANCE hInstance; HICON hIcon; HCURSOR hCursor;
	HBRUSH hbrBackground; LPCSTR lpszMenuName, lpszClassName; HICON hIconSm;
} WNDCLASSEXA, *LPWNDCLASSEXA;
typedef WNDCLASSEXA WNDCLASSEX, *LPWNDCLASSEX;

typedef INT_PTR (*FARPROC)();
typedef INT_PTR (*NEARPROC)();
typedef INT_PTR (*PROC)();
typedef LRESULT (*WNDPROC)(HWND, UINT, WPARAM, LPARAM);
typedef INT_PTR (*DLGPROC)(HWND, UINT, WPARAM, LPARAM);
typedef LRESULT (*HOOKPROC)(int, WPARAM, LPARAM);
typedef DWORD (*LPTHREAD_START_ROUTINE)(LPVOID);
typedef void (*TIMERPROC)(HWND, UINT, UINT_PTR, DWORD);
typedef BOOL (*WNDENUMPROC)(HWND, LPARAM);
typedef LONG (*PTOP_LEVEL_EXCEPTION_FILTER)(PEXCEPTION_POINTERS);
typedef PTOP_LEVEL_EXCEPTION_FILTER LPTOP_LEVEL_EXCEPTION_FILTER;

// ---------------------------------------------------------------------------- COM basics
#define S_OK          ((HRESULT)0)
#define S_FALSE       ((HRESULT)1)
#define E_FAIL        ((HRESULT)0x80004005L)
#define E_INVALIDARG  ((HRESULT)0x80070057L)
#define E_OUTOFMEMORY ((HRESULT)0x8007000EL)
#define E_NOINTERFACE ((HRESULT)0x80004002L)
#define E_NOTIMPL     ((HRESULT)0x80004001L)
#define SUCCEEDED(hr) (((HRESULT)(hr)) >= 0)
#define FAILED(hr)    (((HRESULT)(hr)) < 0)
#define MAKE_HRESULT(sev, fac, code) ((HRESULT)(((unsigned long)(sev) << 31) | ((unsigned long)(fac) << 16) | ((unsigned long)(code))))

#define interface struct
#define STDMETHOD(m)          virtual HRESULT STDMETHODCALLTYPE m
#define STDMETHOD_(t, m)      virtual t STDMETHODCALLTYPE m
#define STDMETHODIMP          HRESULT STDMETHODCALLTYPE
#define STDMETHODIMP_(t)      t STDMETHODCALLTYPE
#define PURE                  = 0
#define THIS_
#define THIS                  void
#define DECLARE_INTERFACE(i)  struct i
#define DECLARE_INTERFACE_(i, b) struct i : public b

#define STDAPI                extern "C" HRESULT
#define STDAPI_(t)            extern "C" t
#define DEFINE_GUID(name, ...) extern "C" const GUID name
#define DEFINE_OLEGUID(name, ...) extern "C" const GUID name

struct IUnknown {
	virtual HRESULT QueryInterface(REFIID, void**) = 0;
	virtual ULONG AddRef() = 0;
	virtual ULONG Release() = 0;
};
typedef IUnknown* LPUNKNOWN;
extern "C" const IID IID_IUnknown;
extern "C" const IID IID_IDispatch;
extern "C" const IID IID_IClassFactory;

// Minimal COM/GDI declarations the fetched DirectX 8 SDK headers expect from the platform SDK.
struct IStream;
struct IMalloc;
struct IDispatch;
typedef IStream* LPSTREAM;
typedef struct _RGNDATAHEADER { DWORD dwSize, iType, nCount, nRgnSize; RECT rcBound; } RGNDATAHEADER;
typedef struct _RGNDATA { RGNDATAHEADER rdh; char Buffer[1]; } RGNDATA, *LPRGNDATA;
typedef struct _LOGFONTA {
	LONG lfHeight, lfWidth, lfEscapement, lfOrientation, lfWeight;
	BYTE lfItalic, lfUnderline, lfStrikeOut, lfCharSet, lfOutPrecision,
	     lfClipPrecision, lfQuality, lfPitchAndFamily;
	CHAR lfFaceName[32];
} LOGFONTA, *LPLOGFONTA;
typedef LOGFONTA LOGFONT, *LPLOGFONT;
typedef struct _POINTFLOAT { FLOAT x, y; } POINTFLOAT;
typedef struct _GLYPHMETRICSFLOAT {
	FLOAT gmfBlackBoxX, gmfBlackBoxY;
	POINTFLOAT gmfptGlyphOrigin;
	FLOAT gmfCellIncX, gmfCellIncY;
} GLYPHMETRICSFLOAT, *LPGLYPHMETRICSFLOAT;

// ------------------------------------------------------------------------ common constants
#define ERROR_SUCCESS            0L
#define ERROR_FILE_NOT_FOUND     2L
#define ERROR_PATH_NOT_FOUND     3L
#define ERROR_ACCESS_DENIED      5L
#define ERROR_NO_MORE_FILES      18L
#define ERROR_ALREADY_EXISTS     183L
#define ERROR_MORE_DATA          234L
#define WAIT_OBJECT_0            0L
#define WAIT_TIMEOUT             258L
#define WAIT_FAILED              0xFFFFFFFF
#define INFINITE                 0xFFFFFFFF
#define FILE_ATTRIBUTE_READONLY  0x00000001
#define FILE_ATTRIBUTE_HIDDEN    0x00000002
#define FILE_ATTRIBUTE_DIRECTORY 0x00000010
#define FILE_ATTRIBUTE_NORMAL    0x00000080
#define INVALID_FILE_ATTRIBUTES  ((DWORD)0xFFFFFFFF)
#define GENERIC_READ             0x80000000
#define GENERIC_WRITE            0x40000000
#define FILE_SHARE_READ          0x00000001
#define FILE_SHARE_WRITE         0x00000002
#define CREATE_ALWAYS            2
#define CREATE_NEW               1
#define OPEN_EXISTING            3
#define OPEN_ALWAYS              4
#define TRUNCATE_EXISTING        5
#define FILE_BEGIN               0
#define FILE_CURRENT             1
#define FILE_END                 2
#define MB_OK                    0x00000000
#define MB_OKCANCEL              0x00000001
#define MB_ABORTRETRYIGNORE      0x00000002
#define MB_YESNO                 0x00000004
#define MB_ICONERROR             0x00000010
#define MB_ICONHAND              0x00000010
#define MB_ICONQUESTION          0x00000020
#define MB_ICONEXCLAMATION       0x00000030
#define MB_ICONINFORMATION       0x00000040
#define MB_SYSTEMMODAL           0x00001000
#define MB_TASKMODAL             0x00002000
#define MB_TOPMOST               0x00040000
#define MB_SETFOREGROUND         0x00010000
#define IDOK                     1
#define IDCANCEL                 2
#define IDABORT                  3
#define IDRETRY                  4
#define IDIGNORE                 5
#define IDYES                    6
#define IDNO                     7
#define SW_HIDE                  0
#define SW_SHOW                  5
#define SW_SHOWNORMAL            1
#define SW_MINIMIZE              6
#define SW_RESTORE               9
#define HWND_TOP                 ((HWND)0)
#define HWND_TOPMOST             ((HWND)-1)
#define HWND_NOTOPMOST           ((HWND)-2)
#define TIME_ZONE_ID_INVALID     0xFFFFFFFF
#define GMEM_FIXED               0x0000
#define GMEM_MOVEABLE            0x0002
#define GMEM_NOCOMPACT           0x0010
#define GMEM_NODISCARD           0x0020
#define GMEM_ZEROINIT            0x0040
#define GMEM_MODIFY              0x0080
#define GMEM_DISCARDABLE         0x0100
#define GMEM_SHARE               0x2000
#define GMEM_DDESHARE            0x2000
#define GPTR                     (GMEM_FIXED | GMEM_ZEROINIT)
#define GHND                     (GMEM_MOVEABLE | GMEM_ZEROINIT)
#define VER_PLATFORM_WIN32s          0
#define VER_PLATFORM_WIN32_WINDOWS   1
#define VER_PLATFORM_WIN32_NT        2
#define LANG_NEUTRAL             0x00
#define SUBLANG_DEFAULT          0x01
#define LOCALE_USER_DEFAULT      0x0400
#define LOCALE_SYSTEM_DEFAULT    0x0800
#define FORMAT_MESSAGE_ALLOCATE_BUFFER 0x00000100
#define FORMAT_MESSAGE_IGNORE_INSERTS  0x00000200
#define FORMAT_MESSAGE_FROM_STRING     0x00000400
#define FORMAT_MESSAGE_FROM_HMODULE    0x00000800
#define FORMAT_MESSAGE_FROM_SYSTEM     0x00001000
#define DATE_SHORTDATE           0x00000001
#define DATE_LONGDATE            0x00000002
#define TIME_NOMINUTESORSECONDS  0x00000001
#define TIME_NOSECONDS           0x00000002
#define TIME_NOTIMEMARKER        0x00000004
#define TIME_FORCE24HOURFORMAT   0x00000008
#define VK_BACK      0x08
#define VK_TAB       0x09
#define VK_RETURN    0x0D
#define VK_SHIFT     0x10
#define VK_CONTROL   0x11
#define VK_MENU      0x12
#define VK_ESCAPE    0x1B
#define VK_SPACE     0x20
#define VK_LEFT      0x25
#define VK_UP        0x26
#define VK_RIGHT     0x27
#define VK_DOWN      0x28
#define VK_DELETE    0x2E
#define SM_CXSCREEN  0
#define SM_CYSCREEN  1
#define STATUS_ACCESS_VIOLATION  ((DWORD)0xC0000005L)
#define EXCEPTION_EXECUTE_HANDLER    1
#define EXCEPTION_CONTINUE_SEARCH    0
#define EXCEPTION_CONTINUE_EXECUTION (-1)
#define MAKEWORD(a, b)   ((WORD)(((BYTE)(a)) | ((WORD)((BYTE)(b))) << 8))
#define MAKELONG(a, b)   ((LONG)(((WORD)(a)) | ((DWORD)((WORD)(b))) << 16))
#define LOWORD(l)        ((WORD)((DWORD_PTR)(l) & 0xFFFF))
#define HIWORD(l)        ((WORD)(((DWORD_PTR)(l) >> 16) & 0xFFFF))
#define LOBYTE(w)        ((BYTE)((DWORD_PTR)(w) & 0xFF))
#define HIBYTE(w)        ((BYTE)(((DWORD_PTR)(w) >> 8) & 0xFF))
#define RGB(r, g, b)     ((COLORREF)(((BYTE)(r)) | ((WORD)((BYTE)(g)) << 8) | ((DWORD)((BYTE)(b)) << 16)))
#define GetRValue(c)     ((BYTE)(c))
#define GetGValue(c)     ((BYTE)(((WORD)(c)) >> 8))
#define GetBValue(c)     ((BYTE)((c) >> 16))
#define ZeroMemory(d, l)      memset((d), 0, (l))
#define CopyMemory(d, s, l)   memcpy((d), (s), (l))
#define MoveMemory(d, s, l)   memmove((d), (s), (l))
#define FillMemory(d, l, f)   memset((d), (f), (l))

// -------------------------------------------------------------------------------- the API
// Declaration-only. Deliberately unimplemented: the probe never links.
extern "C" {
DWORD  GetLastError();
void   SetLastError(DWORD);
HANDLE GetCurrentProcess();
HANDLE GetCurrentThread();
DWORD  GetCurrentProcessId();
BOOL   CloseHandle(HANDLE);
HANDLE CreateEventA(LPSECURITY_ATTRIBUTES, BOOL, BOOL, LPCSTR);
BOOL   SetEvent(HANDLE);
BOOL   ResetEvent(HANDLE);
DWORD  WaitForSingleObject(HANDLE, DWORD);
DWORD  WaitForMultipleObjects(DWORD, const HANDLE*, BOOL, DWORD);
HANDLE CreateMutexA(LPSECURITY_ATTRIBUTES, BOOL, LPCSTR);
BOOL   ReleaseMutex(HANDLE);
HANDLE CreateSemaphoreA(LPSECURITY_ATTRIBUTES, LONG, LONG, LPCSTR);
BOOL   ReleaseSemaphore(HANDLE, LONG, LPLONG);
void   InitializeCriticalSection(LPCRITICAL_SECTION);
void   DeleteCriticalSection(LPCRITICAL_SECTION);
void   EnterCriticalSection(LPCRITICAL_SECTION);
void   LeaveCriticalSection(LPCRITICAL_SECTION);
BOOL   TryEnterCriticalSection(LPCRITICAL_SECTION);
HANDLE CreateThread(LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD);
BOOL   TerminateThread(HANDLE, DWORD);
BOOL   SetThreadPriority(HANDLE, int);
DWORD  SuspendThread(HANDLE);
DWORD  ResumeThread(HANDLE);
BOOL   SetThreadAffinityMask(HANDLE, DWORD_PTR);
LONG   InterlockedIncrement(LONG volatile*);
LONG   InterlockedDecrement(LONG volatile*);
LONG   InterlockedExchange(LONG volatile*, LONG);
LONG   InterlockedExchangeAdd(LONG volatile*, LONG);
LONG   InterlockedCompareExchange(LONG volatile*, LONG, LONG);
HMODULE GetModuleHandleA(LPCSTR);
UINT   GetSystemDirectoryA(LPSTR, UINT);
UINT   GetWindowsDirectoryA(LPSTR, UINT);
DWORD  GetModuleFileNameA(HMODULE, LPSTR, DWORD);
HMODULE LoadLibraryA(LPCSTR);
BOOL   FreeLibrary(HMODULE);
FARPROC GetProcAddress(HMODULE, LPCSTR);
HRSRC  FindResourceA(HMODULE, LPCSTR, LPCSTR);
HGLOBAL LoadResource(HMODULE, HRSRC);
LPVOID LockResource(HGLOBAL);
DWORD  SizeofResource(HMODULE, HRSRC);
HANDLE CreateFileA(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
BOOL   ReadFile(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
BOOL   WriteFile(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
DWORD  SetFilePointer(HANDLE, LONG, PLONG, DWORD);
BOOL   DeleteFileA(LPCSTR);
BOOL   MoveFileA(LPCSTR, LPCSTR);
BOOL   CopyFileA(LPCSTR, LPCSTR, BOOL);
DWORD  GetFileAttributesA(LPCSTR);
BOOL   SetFileAttributesA(LPCSTR, DWORD);
BOOL   CreateDirectoryA(LPCSTR, LPSECURITY_ATTRIBUTES);
BOOL   RemoveDirectoryA(LPCSTR);
DWORD  GetCurrentDirectoryA(DWORD, LPSTR);
BOOL   SetCurrentDirectoryA(LPCSTR);
DWORD  GetTempPathA(DWORD, LPSTR);
DWORD  GetFullPathNameA(LPCSTR, DWORD, LPSTR, LPSTR*);
HANDLE FindFirstFileA(LPCSTR, LPWIN32_FIND_DATAA);
BOOL   FindNextFileA(HANDLE, LPWIN32_FIND_DATAA);
BOOL   FindClose(HANDLE);
BOOL   GetFileTime(HANDLE, LPFILETIME, LPFILETIME, LPFILETIME);
BOOL   FileTimeToSystemTime(const FILETIME*, LPSYSTEMTIME);
BOOL   SystemTimeToFileTime(const SYSTEMTIME*, LPFILETIME);
BOOL   FileTimeToLocalFileTime(const FILETIME*, LPFILETIME);
LONG   CompareFileTime(const FILETIME*, const FILETIME*);
void   GetLocalTime(LPSYSTEMTIME);
void   GetSystemTime(LPSYSTEMTIME);
void   GetSystemInfo(LPSYSTEM_INFO);
BOOL   GetVersionExA(LPOSVERSIONINFOA);
void   GlobalMemoryStatus(LPMEMORYSTATUS);
BOOL   GlobalMemoryStatusEx(LPMEMORYSTATUSEX);
LPVOID VirtualAlloc(LPVOID, SIZE_T, DWORD, DWORD);
BOOL   VirtualFree(LPVOID, SIZE_T, DWORD);
HGLOBAL GlobalAlloc(UINT, SIZE_T);
HGLOBAL GlobalReAlloc(HGLOBAL, SIZE_T, UINT);
SIZE_T GlobalSize(HGLOBAL);
LPVOID GlobalLock(HGLOBAL);
BOOL   GlobalUnlock(HGLOBAL);
HGLOBAL GlobalFree(HGLOBAL);
// Version resource queries. Windows-only by construction -- they read a VERSIONINFO resource out
// of a PE image -- so they are declared and never defined; see
// docs/porting/crt-and-widechar-compat.md.
DWORD  GetFileVersionInfoSizeA(LPCSTR, LPDWORD);
BOOL   GetFileVersionInfoA(LPCSTR, DWORD, DWORD, LPVOID);
BOOL   VerQueryValueA(LPCVOID, LPCSTR, LPVOID*, PUINT);
void   OutputDebugStringA(LPCSTR);
void   DebugBreak();
BOOL   IsDebuggerPresent();
void   ExitProcess(UINT);
BOOL   TerminateProcess(HANDLE, UINT);
BOOL   CreateProcessA(LPCSTR, LPSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD,
                      LPVOID, LPCSTR, LPSTARTUPINFOA, LPPROCESS_INFORMATION);
DWORD  GetEnvironmentVariableA(LPCSTR, LPSTR, DWORD);
LPSTR  GetCommandLineA();
DWORD  FormatMessageA(DWORD, LPCVOID, DWORD, DWORD, LPSTR, DWORD, va_list*);
// MultiByteToWideChar / WideCharToMultiByte come from Utility/wchar_compat.h as macros over
// mbstowcs / wcstombs. Declaring them here would expand those macros and redeclare the libc
// functions without their exception specifications.
int    lstrlenA(LPCSTR);
int    wsprintfA(LPSTR, LPCSTR, ...);
LPTOP_LEVEL_EXCEPTION_FILTER SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER);
HRESULT CoInitialize(LPVOID);
void    CoUninitialize();
HRESULT CoCreateInstance(REFCLSID, LPUNKNOWN, DWORD, REFIID, void**);
HRESULT OleInitialize(LPVOID);
void    OleUninitialize();
void*   CoTaskMemAlloc(SIZE_T);
void    CoTaskMemFree(void*);

// user32 / gdi32
HWND    CreateWindowExA(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
BOOL    DestroyWindow(HWND);
BOOL    ShowWindow(HWND, int);
BOOL    UpdateWindow(HWND);
BOOL    SetWindowPos(HWND, HWND, int, int, int, int, UINT);
BOOL    GetClientRect(HWND, LPRECT);
BOOL    GetWindowRect(HWND, LPRECT);
LRESULT SendMessageA(HWND, UINT, WPARAM, LPARAM);
BOOL    PostMessageA(HWND, UINT, WPARAM, LPARAM);
LRESULT DefWindowProcA(HWND, UINT, WPARAM, LPARAM);
BOOL    PeekMessageA(LPMSG, HWND, UINT, UINT, UINT);
BOOL    GetMessageA(LPMSG, HWND, UINT, UINT);
BOOL    TranslateMessage(const MSG*);
LRESULT DispatchMessageA(const MSG*);
int     MessageBoxA(HWND, LPCSTR, LPCSTR, UINT);
HWND    GetActiveWindow();
HWND    SetActiveWindow(HWND);
HWND    GetForegroundWindow();
BOOL    SetForegroundWindow(HWND);
HWND    GetFocus();
HWND    SetFocus(HWND);
BOOL    GetCursorPos(LPPOINT);
BOOL    SetCursorPos(int, int);
BOOL    ScreenToClient(HWND, LPPOINT);
BOOL    ClientToScreen(HWND, LPPOINT);
int     ShowCursor(BOOL);
HCURSOR SetCursor(HCURSOR);
HCURSOR LoadCursorA(HINSTANCE, LPCSTR);
HICON   LoadIconA(HINSTANCE, LPCSTR);
short   GetAsyncKeyState(int);
short   GetKeyState(int);
int     GetSystemMetrics(int);
UINT    GetDoubleClickTime();
int     GetDateFormatA(LCID, DWORD, const SYSTEMTIME*, LPCSTR, LPSTR, int);
int     GetTimeFormatA(LCID, DWORD, const SYSTEMTIME*, LPCSTR, LPSTR, int);

// TheSuperHackers @port The wide entry points that call sites ask for *by name*, i.e. where the
// A/W aliasing macros below cannot help because the source already says ...W. LPWSTR is
// wchar_t*, which is 4 bytes here against 2 on MSVC; the call sites pass WideChar buffers, which
// have the same mismatch. That is not fixed by declaring these -- see
// docs/porting/widechar-fallout.md -- it is only made visible at the boundary where it has to be
// fixed.
DWORD   GetModuleFileNameW(HMODULE, LPWSTR, DWORD);
HMODULE GetModuleHandleW(LPCWSTR);
int     GetDateFormatW(LCID, DWORD, const SYSTEMTIME*, LPCWSTR, LPWSTR, int);
int     GetTimeFormatW(LCID, DWORD, const SYSTEMTIME*, LPCWSTR, LPWSTR, int);
DWORD   FormatMessageW(DWORD, LPCVOID, DWORD, DWORD, LPWSTR, DWORD, va_list*);
HDC     GetDC(HWND);
int     ReleaseDC(HWND, HDC);
BOOL    InvalidateRect(HWND, LPCRECT, BOOL);
}

// The engine builds ANSI-only; the real headers alias the A variants behind these names.
#define CreateEvent      CreateEventA
#define CreateMutex      CreateMutexA
#define CreateSemaphore  CreateSemaphoreA
#define GetModuleHandle  GetModuleHandleA
#define GetSystemDirectory GetSystemDirectoryA
#define GetWindowsDirectory GetWindowsDirectoryA
#define GetModuleFileName GetModuleFileNameA
#define LoadLibrary      LoadLibraryA
#define FindResource     FindResourceA
#define CreateFile       CreateFileA
#define DeleteFile       DeleteFileA
#define MoveFile         MoveFileA
#define CopyFile         CopyFileA
#define GetFileAttributes GetFileAttributesA
#define SetFileAttributes SetFileAttributesA
#define CreateDirectory  CreateDirectoryA
#define RemoveDirectory  RemoveDirectoryA
#define GetCurrentDirectory GetCurrentDirectoryA
#define SetCurrentDirectory SetCurrentDirectoryA
#define GetTempPath      GetTempPathA
#define GetFullPathName  GetFullPathNameA
#define FindFirstFile    FindFirstFileA
#define FindNextFile     FindNextFileA
#define GetVersionEx     GetVersionExA
#define GetFileVersionInfoSize GetFileVersionInfoSizeA
#define GetFileVersionInfo GetFileVersionInfoA
#define VerQueryValue    VerQueryValueA
#define CreateProcess    CreateProcessA
#define GetEnvironmentVariable GetEnvironmentVariableA
#define GetCommandLine   GetCommandLineA
#define FormatMessage    FormatMessageA
#define GetDateFormat    GetDateFormatA
#define GetTimeFormat    GetTimeFormatA
#define CreateWindowEx   CreateWindowExA
#define SendMessage      SendMessageA
#define PostMessage      PostMessageA
#define DefWindowProc    DefWindowProcA
#define PeekMessage      PeekMessageA
#define GetMessage       GetMessageA
#define DispatchMessage  DispatchMessageA
#define MessageBox       MessageBoxA
#define LoadCursor       LoadCursorA
#define LoadIcon         LoadIconA
#define wsprintf         wsprintfA
