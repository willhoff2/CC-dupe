// Declaration-only stand-in for scripts/native-port-probe.py. See README.md.
#pragma once

// Wraps the real <sys/stat.h> and adds the MSVC `_stat` spellings the engine uses.
#include_next <sys/stat.h>

#define _stat  stat
#define _fstat fstat
#define _S_IFMT  S_IFMT
#define _S_IFDIR S_IFDIR
#define _S_IFREG S_IFREG
#define _S_IREAD  S_IRUSR
#define _S_IWRITE S_IWUSR
