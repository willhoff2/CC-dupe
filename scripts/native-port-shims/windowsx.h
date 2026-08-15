// Declaration-only stand-in for scripts/native-port-probe.py. See README.md.
//
// `framgrab.h` is the only include of this header outside the cut tools, and `FramGrab.cpp` uses
// two macros out of it: `GlobalAllocPtr` and `GlobalFreePtr`. The real `<windowsx.h>` is macros
// only -- it has no functions of its own -- so these are spelled the way the SDK spells them, in
// terms of the `Global*` functions `<windows.h>` declares. That keeps the shim declaration-only
// while still type-checking the call sites.
//
// The guard is the SDK header's own rather than `#pragma once`, so that a translation unit which
// somehow reaches both this and a real `<windowsx.h>` takes only the first. See d3d8types.h for
// what sharing the vendored guard is worth.
#ifndef _INC_WINDOWSX
#define _INC_WINDOWSX

#include <windows.h>

#define GlobalPtrHandle(lp) ((HGLOBAL)GlobalHandle(lp))
#define GlobalLockPtr(lp)   (GlobalLock(GlobalPtrHandle(lp)))
#define GlobalUnlockPtr(lp) (GlobalUnlock(GlobalPtrHandle(lp)))
#define GlobalAllocPtr(flags, cb)      (GlobalLock(GlobalAlloc((flags), (cb))))
#define GlobalReAllocPtr(lp, cbNew, flags) \
	(GlobalUnlockPtr(lp), GlobalLock(GlobalReAlloc(GlobalPtrHandle(lp), (cbNew), (flags))))
#define GlobalFreePtr(lp)   (GlobalUnlockPtr(lp), (BOOL)GlobalFree(GlobalPtrHandle(lp)))

#endif /* _INC_WINDOWSX */
