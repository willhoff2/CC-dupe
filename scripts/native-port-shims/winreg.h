// Declaration-only stand-in for scripts/native-port-probe.py. See README.md.
#pragma once

#include <windows.h>

#define HKEY_CLASSES_ROOT   ((HKEY)(ULONG_PTR)0x80000000)
#define HKEY_CURRENT_USER   ((HKEY)(ULONG_PTR)0x80000001)
#define HKEY_LOCAL_MACHINE  ((HKEY)(ULONG_PTR)0x80000002)
#define HKEY_USERS          ((HKEY)(ULONG_PTR)0x80000003)

#define REG_NONE       0
#define REG_SZ         1
#define REG_EXPAND_SZ  2
#define REG_BINARY     3
#define REG_DWORD      4
#define REG_MULTI_SZ   7
#define REG_OPTION_NON_VOLATILE 0
#define KEY_QUERY_VALUE 0x0001
#define KEY_SET_VALUE   0x0002
#define KEY_READ        0x20019
#define KEY_WRITE       0x20006
#define KEY_ALL_ACCESS  0xF003F

typedef LONG LSTATUS;

extern "C" {
LSTATUS RegOpenKeyExA(HKEY, LPCSTR, DWORD, REGSAM, PHKEY);
LSTATUS RegCreateKeyExA(HKEY, LPCSTR, DWORD, LPSTR, DWORD, REGSAM,
                        LPSECURITY_ATTRIBUTES, PHKEY, LPDWORD);
LSTATUS RegQueryValueExA(HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
LSTATUS RegSetValueExA(HKEY, LPCSTR, DWORD, DWORD, const BYTE*, DWORD);
LSTATUS RegDeleteValueA(HKEY, LPCSTR);
LSTATUS RegDeleteKeyA(HKEY, LPCSTR);
LSTATUS RegEnumKeyExA(HKEY, DWORD, LPSTR, LPDWORD, LPDWORD, LPSTR, LPDWORD, LPFILETIME);
LSTATUS RegEnumValueA(HKEY, DWORD, LPSTR, LPDWORD, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
LSTATUS RegCloseKey(HKEY);
}
#define RegOpenKeyEx    RegOpenKeyExA
#define RegCreateKeyEx  RegCreateKeyExA
#define RegQueryValueEx RegQueryValueExA
#define RegSetValueEx   RegSetValueExA
#define RegDeleteValue  RegDeleteValueA
#define RegDeleteKey    RegDeleteKeyA
#define RegEnumKeyEx    RegEnumKeyExA
#define RegEnumValue    RegEnumValueA
