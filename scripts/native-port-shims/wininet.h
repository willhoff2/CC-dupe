// Declaration-only stand-in for scripts/native-port-probe.py. See README.md.
#pragma once

#include <windows.h>

#define INTERNET_OPEN_TYPE_PRECONFIG 0
#define INTERNET_OPEN_TYPE_DIRECT    1
#define INTERNET_SERVICE_HTTP        3
#define INTERNET_FLAG_RELOAD         0x80000000

extern "C" {
HINTERNET InternetOpenA(LPCSTR, DWORD, LPCSTR, LPCSTR, DWORD);
HINTERNET InternetOpenUrlA(HINTERNET, LPCSTR, LPCSTR, DWORD, DWORD, DWORD_PTR);
BOOL      InternetReadFile(HINTERNET, LPVOID, DWORD, LPDWORD);
BOOL      InternetCloseHandle(HINTERNET);
BOOL      InternetGetConnectedState(LPDWORD, DWORD);
}
#define InternetOpen    InternetOpenA
#define InternetOpenUrl InternetOpenUrlA
