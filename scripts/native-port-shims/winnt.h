// Declaration-only stand-in for scripts/native-port-probe.py. See README.md.
//
// The real <winnt.h> is the NT type and structure header that <windows.h> includes; a few sources
// include it directly. Most of what this codebase wants from it -- VS_FIXEDFILEINFO and the
// handle and integer types -- is in the windows.h shim already. The PE image headers below are
// here rather than there because they are the one part that is genuinely about the executable file
// format rather than about the API.
//
// verchk.cpp reads these out of a file, and out of its own loaded image, to compare build stamps
// between the running EXE and one on disk. The struct layouts are the on-disk PE layout, which is
// fixed by the file format and identical everywhere, so these are exact rather than approximate --
// but nothing off Windows has a PE image to read, which is why the version-check functions
// themselves are declared and never defined.
#pragma once

#include <windows.h>

#define IMAGE_DOS_SIGNATURE 0x5A4D		// MZ
#define IMAGE_NT_SIGNATURE  0x00004550	// PE00

typedef struct _IMAGE_DOS_HEADER {
	WORD e_magic, e_cblp, e_cp, e_crlc, e_cparhdr, e_minalloc, e_maxalloc, e_ss, e_sp, e_csum;
	WORD e_ip, e_cs, e_lfarlc, e_ovno, e_res[4], e_oemid, e_oeminfo, e_res2[10];
	LONG e_lfanew;			// file offset of the PE signature
} IMAGE_DOS_HEADER, *PIMAGE_DOS_HEADER;

typedef struct _IMAGE_FILE_HEADER {
	WORD  Machine, NumberOfSections;
	DWORD TimeDateStamp, PointerToSymbolTable, NumberOfSymbols;
	WORD  SizeOfOptionalHeader, Characteristics;
} IMAGE_FILE_HEADER, *PIMAGE_FILE_HEADER;
