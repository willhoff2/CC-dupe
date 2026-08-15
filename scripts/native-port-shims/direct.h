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
// _mkdir is deliberately not declared here: Utility/path_compat.h defines it inline, and a second
// declaration with C linkage would conflict with that definition.
extern "C" int _getdrive();
extern "C" int _chdrive(int drive);
