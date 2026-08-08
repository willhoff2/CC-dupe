// Declaration-only stand-in for scripts/native-port-probe.py. See README.md.
#pragma once

#include <unistd.h>
#include <sys/stat.h>

#define _MAX_PATH  260
#define _MAX_DRIVE 3
#define _MAX_DIR   256
#define _MAX_FNAME 256
#define _MAX_EXT   256

#define _getcwd getcwd
#define _chdir  chdir
#define _rmdir  rmdir
extern "C" int _mkdir(const char* path);
extern "C" int _getdrive();
extern "C" int _chdrive(int drive);
