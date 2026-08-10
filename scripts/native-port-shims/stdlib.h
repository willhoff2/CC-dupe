// Measurement instrument, not a port. See README.md in this directory.
// Adds the MSVC CRT spellings of <stdlib.h> functions on top of the real system header, so that the
// probe's diagnostics are about the Win32/platform surface rather than about CRT naming. Every
// name here is an OPEN CRT-compatibility item: nothing in the repository provides it yet, and these
// are declarations only, so linking against them deliberately fails.
#pragma once
#include_next <stdlib.h>

extern "C" char *_itoa(int value, char *buffer, int radix);
extern "C" char *itoa(int value, char *buffer, int radix);
extern "C" char *_ultoa(unsigned long value, char *buffer, int radix);
