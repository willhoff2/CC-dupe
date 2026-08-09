// Declaration-only stand-in for scripts/native-port-probe.py. See README.md.
#pragma once

// glibc deprecates <sys/timeb.h> and macOS does not ship it at all.
#include <time.h>

struct _timeb {
	time_t         time;
	unsigned short millitm;
	short          timezone;
	short          dstflag;
};
extern "C" void _ftime(struct _timeb* tb);
#define timeb _timeb
#define ftime _ftime
