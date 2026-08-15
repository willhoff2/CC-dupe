// Declaration-only stand-in for scripts/native-port-probe.py. See README.md.
//
// Only `ddsfile.cpp` includes this outside the cut tools, and it wants exactly two macros from it:
// `DDSCAPS2_CUBEMAP` and `DDSCAPS2_VOLUME`. They are not DirectDraw API -- they are two bits of the
// `dwCaps2` field of the DDS *file format*, which is why `ddsfile.h` carries its own
// `LegacyDDSCAPS2` / `LegacyDDSURFACEDESC2` copies of the on-disk layout and marks them DO NOT
// MODIFY rather than reading the SDK structures. The values are therefore file-format constants,
// fixed by every DDS ever written, and are reproduced here rather than approximated.
//
// Nothing here declares a DirectDraw interface: no `IDirectDraw*` survives in the engine, the
// renderer is Direct3D 8 throughout, and the replacement backend is Vulkan. If a future call site
// needs DirectDraw proper, that is a scope question and not a missing declaration.
//
// The guard is the SDK header's own rather than `#pragma once`; see d3d8types.h for why sharing
// the vendored guard matters.
#ifndef __DDRAW_INCLUDED__
#define __DDRAW_INCLUDED__

#include <windows.h>

/* DDSCAPS2 flags -- `dwCaps2` of the DDS header. */
#define DDSCAPS2_CUBEMAP            0x00000200l
#define DDSCAPS2_CUBEMAP_POSITIVEX  0x00000400l
#define DDSCAPS2_CUBEMAP_NEGATIVEX  0x00000800l
#define DDSCAPS2_CUBEMAP_POSITIVEY  0x00001000l
#define DDSCAPS2_CUBEMAP_NEGATIVEY  0x00002000l
#define DDSCAPS2_CUBEMAP_POSITIVEZ  0x00004000l
#define DDSCAPS2_CUBEMAP_NEGATIVEZ  0x00008000l
#define DDSCAPS2_VOLUME             0x00200000l

#endif /* __DDRAW_INCLUDED__ */
