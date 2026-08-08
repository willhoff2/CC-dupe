// Declaration-only stand-in for scripts/native-port-probe.py. See README.md.
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef uintptr_t _beginthread_handle_t;
extern "C" {
uintptr_t _beginthread(void (*start)(void*), unsigned stack_size, void* arglist);
uintptr_t _beginthreadex(void* security, unsigned stack_size,
                         unsigned (*start)(void*), void* arglist,
                         unsigned initflag, unsigned* thrdaddr);
void      _endthread();
void      _endthreadex(unsigned retval);
int       _getpid();
intptr_t  _spawnl(int mode, const char* path, const char* arg0, ...);
intptr_t  _spawnv(int mode, const char* path, const char* const* argv);
}
#define _P_WAIT     0
#define _P_NOWAIT   1
#define _P_OVERLAY  2
#define _P_DETACH   4
