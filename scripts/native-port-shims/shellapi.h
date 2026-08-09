// Declaration-only stand-in for scripts/native-port-probe.py. See README.md.
#pragma once

#include <windows.h>

typedef struct _SHELLEXECUTEINFOA {
	DWORD cbSize; ULONG fMask; HWND hwnd;
	LPCSTR lpVerb, lpFile, lpParameters, lpDirectory;
	int nShow; HINSTANCE hInstApp; void* lpIDList; LPCSTR lpClass;
	HKEY hkeyClass; DWORD dwHotKey; HANDLE hIcon; HANDLE hProcess;
} SHELLEXECUTEINFOA, *LPSHELLEXECUTEINFOA;
typedef SHELLEXECUTEINFOA SHELLEXECUTEINFO, *LPSHELLEXECUTEINFO;

extern "C" {
HINSTANCE ShellExecuteA(HWND, LPCSTR, LPCSTR, LPCSTR, LPCSTR, int);
BOOL      ShellExecuteExA(LPSHELLEXECUTEINFOA);
UINT      DragQueryFileA(HDROP, UINT, LPSTR, UINT);
void      DragFinish(HDROP);
}
#define ShellExecute   ShellExecuteA
#define ShellExecuteEx ShellExecuteExA
#define DragQueryFile  DragQueryFileA
