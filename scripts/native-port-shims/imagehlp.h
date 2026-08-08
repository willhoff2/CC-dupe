// Declaration-only stand-in for scripts/native-port-probe.py. See README.md.
#pragma once

#include <windows.h>

#define MAX_SYM_NAME 2000
#define SYMOPT_DEFERRED_LOADS 0x00000004
#define SYMOPT_UNDNAME        0x00000002
#define SYMOPT_LOAD_LINES     0x00000010

typedef struct _IMAGEHLP_SYMBOL {
	DWORD SizeOfStruct; DWORD_PTR Address; DWORD Size, Flags, MaxNameLength; CHAR Name[1];
} IMAGEHLP_SYMBOL, *PIMAGEHLP_SYMBOL;

typedef struct _IMAGEHLP_LINE {
	DWORD SizeOfStruct; PVOID Key; DWORD LineNumber; PCHAR FileName; DWORD_PTR Address;
} IMAGEHLP_LINE, *PIMAGEHLP_LINE;

typedef struct _IMAGEHLP_MODULE {
	DWORD SizeOfStruct; DWORD_PTR BaseOfImage; DWORD ImageSize, TimeDateStamp, CheckSum,
	NumSyms; DWORD SymType; CHAR ModuleName[32], ImageName[256], LoadedImageName[256];
} IMAGEHLP_MODULE, *PIMAGEHLP_MODULE;

typedef struct _ADDRESS { DWORD_PTR Offset; WORD Segment; DWORD Mode; } ADDRESS, *LPADDRESS;
typedef struct _STACKFRAME {
	ADDRESS AddrPC, AddrReturn, AddrFrame, AddrStack;
	PVOID FuncTableEntry; DWORD_PTR Params[4];
	BOOL Far, Virtual; DWORD_PTR Reserved[3];
} STACKFRAME, *LPSTACKFRAME;

extern "C" {
BOOL  SymInitialize(HANDLE, PSTR, BOOL);
BOOL  SymCleanup(HANDLE);
DWORD SymSetOptions(DWORD);
DWORD SymGetOptions();
BOOL  SymGetSymFromAddr(HANDLE, DWORD_PTR, DWORD_PTR*, PIMAGEHLP_SYMBOL);
BOOL  SymGetLineFromAddr(HANDLE, DWORD_PTR, PDWORD, PIMAGEHLP_LINE);
BOOL  SymGetModuleInfo(HANDLE, DWORD_PTR, PIMAGEHLP_MODULE);
DWORD UnDecorateSymbolName(PCSTR, PSTR, DWORD, DWORD);
BOOL  StackWalk(DWORD, HANDLE, HANDLE, LPSTACKFRAME, PVOID, PVOID, PVOID, PVOID, PVOID);
PVOID SymFunctionTableAccess(HANDLE, DWORD_PTR);
DWORD_PTR SymGetModuleBase(HANDLE, DWORD_PTR);
}
