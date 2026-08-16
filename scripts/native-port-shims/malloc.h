// Forwarding stand-in for scripts/native-port-probe.py. See README.md.
//
// <malloc.h> is a glibc header, not a standard one. The engine includes it for malloc/free/alloca,
// all of which the BSD C library declares in <stdlib.h> and <alloca.h>; the allocator introspection
// glibc puts here (mallinfo, malloc_usable_size) is in <malloc/malloc.h> on Apple platforms and is
// not used by any consumer in this tree. Where a real <malloc.h> exists it is used unchanged, so
// this header changes nothing on Linux.
#pragma once

#if defined(__has_include_next)
#if __has_include_next(<malloc.h>)
#include_next <malloc.h>
#define NATIVE_PORT_SHIM_HAVE_MALLOC_H 1
#endif
#endif

#ifndef NATIVE_PORT_SHIM_HAVE_MALLOC_H
#include <stdlib.h>
#if defined(__APPLE__)
#include <alloca.h>
#include <malloc/malloc.h>
#endif
#endif
