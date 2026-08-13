# Native port probe shims

Declaration-only stand-ins for the Win32 headers that `PreRTS.h` pulls into **every**
GameEngine translation unit. They exist so `scripts/native-port-probe.py --with-shims` can
answer a question the plain probe cannot:

> Once the Win32 *headers* are satisfied, how much of the engine's own C++ is actually
> portable to 64-bit clang?

These headers are **not** a port. Nothing here has an implementation, the types are only
approximately right, and linking against them is impossible by design. They are a measuring
instrument. The real platform layer is Phase 3 of
[`native-port-plan.md`](../../docs/porting/native-port-plan.md), and it should not start from
this directory — it should start from the call sites the probe report identifies.

Deliberate properties:

- **64-bit-correct handle types.** `HWND`, `HANDLE` and friends are pointer-sized, and
  `WPARAM`/`LPARAM`/`LRESULT` are 64-bit, exactly as they are in the real LLP64 Win32 headers.
  Getting this wrong would hide the Phase 2 pointer-truncation errors that are the whole
  reason to compile at 64 bits.
- **No `-fms-extensions` dependence beyond what the probe already passes.**
- **Only MS-specific headers are shadowed, with two named exceptions.** `math.h`, `time.h`,
  `sys/stat.h`, `assert.h` and other headers that exist on POSIX are left to libc. `sys/timeb.h`
  is stubbed because glibc's copy is deprecated and absent on macOS. `stdlib.h` and `stdio.h`
  `#include_next` the real header and add only the MSVC CRT spellings the engine still calls
  (`itoa`, `_itoa`, `_ultoa`, `_snprintf`, `_vsnprintf`). Those used to arrive through
  `PreRTS.h`'s Windows headers; nothing in the repository provides them off Windows yet, so they
  are counted here as open CRT-compatibility debt rather than looking solved. Aliases that *are*
  solved live in `Dependencies/Utility/Utility/` instead — `_stricmp`, `__min`, `__max`, `_isnan`
  and `_finite` moved there and were deleted from this directory.

- **COM and the embedded browser are stubbed to fail loudly.** `oaidl.h`, `atlcom.h`, `comutil.h`
  and `EABrowserDispatch/BrowserDispatch.h` declare just enough for `WWCOMUtil.cpp` and the WOL
  browser screens to parse. `QueryInterface` always fails, `IID_IBrowserDispatch` is deliberately
  not EA's IID, and nothing has a definition — see
  [`crt-and-widechar-compat.md`](../../docs/porting/crt-and-widechar-compat.md) for why that scope
  is cut rather than ported.

See [`prerts-win32-surgery.md`](../../docs/porting/prerts-win32-surgery.md) for what the removal of
`PreRTS.h`'s Win32 block changed, and why these shims are still needed to measure the engine's own
C++ separately from the Win32 headers a file includes on purpose.
