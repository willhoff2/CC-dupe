// Declaration-only stand-in for scripts/native-port-probe.py. See README.md.
#pragma once
#include <windows.h>
typedef struct { BYTE* stream; UINT length; BOOL dynamic; } AsnOctetString;
