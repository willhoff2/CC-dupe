// Measurement instrument, not a port. See README.md in this directory.
// Adds the MSVC CRT spellings of stdio.h functions on top of the real system header, so that the
// probe's diagnostics are about the Win32/platform surface rather than about CRT naming. Every
// alias here is an OPEN CRT-compatibility item: nothing in the repo provides it yet.
#pragma once
#include_next <stdio.h>

#define _snprintf snprintf
#define _vsnprintf vsnprintf
