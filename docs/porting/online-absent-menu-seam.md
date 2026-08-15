# The single-player menu with the online path absent

Eight of the native build's 20 remaining compile failures were online/GameSpy units. Online
matchmaking is cut scope for this port, but five of the eight are on the single-player path or one
Winsock spelling away from portable, and one of them is `MainMenu.cpp` — the main menu of the game
this port exists to run. This slice makes those five compile, and leaves the three that genuinely
need the online path's Win32 surfaces failing under a named reason.

Measured with `./scripts/ci/fetch-probe-deps.sh` then
`CLANGXX=clang++-14 python3 scripts/native-build.py --level 1 --level 2 --level 3 --with-shims`:

| | Before | After |
| --- | ---: | ---: |
| objects | 816 / 836 | **821 / 836** |
| compile failures | 20 | **15** |
| unresolved symbols | 457 | 496 |

Probe, same deps, clang 14:

| Mode | Before | After |
| --- | ---: | ---: |
| native | 664 / 754 | **667 / 754** |
| shimmed (`--with-shims`) | 706 / 754 | **709 / 754** |

The unresolved-symbol count going *up* is the honest consequence of the objects going up, and is
[explained below](#why-unresolved-symbols-went-up-457--496). Windows is unaffected: every change is
either inside a `#ifndef _WIN32` branch or an `#include` that resolves to `<winsock.h>` and
`<windows.h>` on Windows.

## What Windows did, and what the five failures actually were

Two distinct things, neither of them online:

**`HRESULT` in the download library (4 units).** `WWDownload` predates the rest of this code and
uses `HRESULT` as its return convention: `downloaddefs.h` and `ftpdefs.h` build their status codes
with `MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, n)`, and callers test them with
`SUCCEEDED`/`FAILED`. 76 occurrences of `HRESULT` across the download library, the download manager
and the two menus. None of it is COM: nothing is queried for an interface, nothing is
reference-counted, no `IUnknown` appears. It is a 32-bit signed error code whose sign bit means
failure, and on Windows it came from `<windows.h>` transitively. `MainMenu.cpp` and
`DownloadMenu.cpp` failed for exactly this reason — `unknown type name 'HRESULT'` — because they
include `DownloadManager.h`, whose interface is HRESULT-shaped. That is a header dependency, not
online code.

**Two gaps in the existing Winsock seam (2 units).** The repo already puts BSD sockets behind the
Winsock spellings in `Dependencies/Utility/Utility/socket_compat.h` (see
[sockets-and-text-encoding.md](sockets-and-text-encoding.md)); these two files just were not using
it:

- `GameResultsThread.cpp` included `<winsock.h>` directly and used `HOSTENT`, `WSAEINVAL`,
  `WSAEALREADY` and `WSAEISCONN`. The seam already had `HOSTENT` and the socket calls, but of the
  `WSAE*` error constants it had only `WSAEWOULDBLOCK`.
- `PeerThread.cpp` failed on `no matching function for call to 'recvfrom'` — an LP64 signature
  mismatch, not a missing declaration. Winsock's last parameter is `int*`; BSD's is `socklen_t*`,
  which is 32-bit on both but a distinct type, so `int saddrlen` does not bind.

## What the portable side does

`Dependencies/Utility/Utility/hresult_compat.h` (new) defines, under `#ifndef _WIN32` only:
`HRESULT` as `int32_t`, `S_OK`, `S_FALSE`, `E_FAIL`, `SEVERITY_SUCCESS`, `SEVERITY_ERROR`,
`FACILITY_ITF`, `SUCCEEDED`, `FAILED` and `MAKE_HRESULT`, each guarded so a real `<windows.h>` (or
the port's `windows.h` shim, which also defines some of these) wins. The values are the Win32 ones,
so the encoded status codes are bit-identical to what Windows produces and the sign-bit-means-failure
convention holds. On Windows the header expands to nothing. `downloaddefs.h` and `ftpdefs.h` include
it; nothing else changed, and no call site was rewritten.

`socket_compat.h` gains `WSAEINVAL`, `WSAEALREADY` and `WSAEISCONN` in its non-Windows branch,
mapped to the `errno` values a non-blocking `connect()` actually reports there.
`GameResultsThread.cpp` and `PeerThread.cpp` now include the seam instead of `<winsock.h>`, and
`PeerThread.cpp`'s `saddrlen` is `socklen_t`, which *is* `int` on Windows because the seam typedefs
it that way — so the Windows compile sees the same type it saw before.

Net effect: `MainMenu.cpp`, `DownloadMenu.cpp`, `DownloadManager.cpp`, `GameResultsThread.cpp` and
`PeerThread.cpp` compile natively. Zero `#ifdef` in any consumer; two added `#include` lines and one
changed local variable type.

## Why portable spellings and not a build option

The alternative was an `RTS_BUILD_OPTION_ONLINE`-style switch (precedent:
`RTS_BUILD_OPTION_FFMPEG` in `cmake/config-build.cmake`) that drops the online translation units
from a native build. Rejected, for three reasons:

1. **It does not solve the problem that mattered.** `MainMenu.cpp` calls `TheDownloadManager->update()`
   and `TearDownGameSpy()` and `GameSpyUpdateOverlays()`, and includes five GameSpy headers. Excising
   translation units does not make a *consumer* of those units compile — it makes it fail to link
   instead of failing to compile, or forces the `#ifdef`s into the menu that the hard requirements
   rule out. The menu's failure was `HRESULT`; the fix for `HRESULT` is `HRESULT`.
2. **The cost is 40 lines of type definitions, and they are provably right.** `MAKE_HRESULT` is
   arithmetic on a 32-bit integer. A build option is permanent build-matrix surface — a
   configuration Windows never compiles and therefore nothing keeps honest, and the Windows build is
   the behavioural oracle here.
3. **A build option would still be needed later, but for the runtime, not the compiler.** Deciding
   what a single-player-only binary does when the player clicks "Online" is a product decision about
   behaviour, and it will be a smaller and better-posed change once the units compile. Doing it now,
   as a compile-time excision, would have pre-committed to the answer while hiding the measurement.

## Deliberately not done — three units still fail, by name

No gameplay is stubbed and no online behaviour is faked. These stay red, and the seam gate does not
pretend otherwise:

| Unit | Named reason |
| --- | --- |
| `GameSpy/StagingRoomGameInfo.cpp` | The **Windows SNMP SDK** (`AsnObjectIdentifier`, `RFC1157VarBindList`, `AsnInteger32`, `SnmpVarBindList`, `SNMP_PDU_GETNEXT`, and function pointers into `inetmib1.dll`). It walks the local UPnP/router MIB to discover the external address for hosting a game — online-only, and there is no portable SNMP equivalent worth writing for a feature that is cut. Needs the online product decision, not a type. |
| `GameSpy/Thread/PingThread.cpp` | The **Windows ICMP API** (`IcmpCreateFile`/`IcmpSendEcho` from `icmp.dll`, via `HANDLE` and a `WINAPI` function pointer). Raw ICMP off Windows needs a privileged socket or `SOCK_DGRAM`+`IPPROTO_ICMP`, which is a real implementation with a permissions story. Pointing it at the socket seam alone makes it *worse* (measured: the direct `<winsock.h>` include also supplies `HANDLE` and `WINAPI`, so replacing it produced `unknown type name 'HANDLE'` and a `WINAPI` redefinition) — this file wants the Win32 thread/handle surface that `platform/win32-file-api` owns. |
| `GameSpy/MainMenuUtils.cpp` | The **Win32 thread and low-level-IO surface**: `HANDLE`/`CreateThread` for an async `gethostbyname`, plus `_open`/`_close`/`_O_CREAT`/`_S_IREAD` for the patch-directory write test. Both have portable answers (`std::thread`, `getaddrinfo`, `open`) that belong to the thread and file-API slices, not here. Its `HRESULT` failure *is* fixed; it now fails on `unknown type name 'HANDLE'`, one class further along. |

Also not done: `FTP.cpp` and `Download.cpp` (the download library's implementation) are outside the
probe's target set and still fail on `process.h`/`_beginthread` — the thread slice's, and untouched.

## Why unresolved symbols went up (457 → 496)

| Category | Before | After |
| --- | ---: | ---: |
| Defined in a translation unit that failed to compile | 86 | **75** |
| GameSpy SDK (cut scope, not linked) | 33 | **81** |
| Defined in a layer not built here (renderer / audio) | 272 | 274 |
| everything else | 66 | 66 |

Five online translation units that previously produced no object now produce one. Their calls into
the GameSpy SDK — `ghttpStartup`, `SBServerGetStringValueA`, `PersistThink`, `NewGame`, … — were
invisible to the linker while the units failed to compile, and are now visible and unresolved. The
SDK is not vendored as a library and online play is cut, so nothing defines them. 11 symbols moved
the other way, out of "failed to compile".

This is a measurement becoming more truthful, not a regression: the same 48 SDK dependencies existed
before, hidden behind a compile error. But it is exactly the shape of movement that a
total-count ratchet cannot distinguish from a real regression, which is why this slice adds a gate
instead of just widening the total.

## The gate

`scripts/ci/check-online-absent-seam.py`, against
`docs/porting/ci-baselines/online-absent-seam.json`, checks two things the total-unresolved ratchet
in `check-native-build-baseline.py` cannot:

1. The five menu-path translation units compile — **by name**. "Objects went up" is satisfiable by any
   other file in the tree, so it would let the main menu silently regress.
2. The `GameSpy SDK (cut scope, not linked)` category is pinned **per symbol**, not per count. That
   category is expected to be large and non-empty; pinning the count would let a new online
   dependency anywhere in the engine be absorbed into it. Now a new one fails CI naming the symbol
   that caused it, and widening the budget is a reviewable diff:

```sh
python3 scripts/ci/check-online-absent-seam.py --results <native-build.json>          # check
python3 scripts/ci/check-online-absent-seam.py --results <native-build.json> --update # widen
```

Verified in both directions: it passes on this branch, and against the pre-change measurement it
fails with all five units listed as no longer compiling.

## What the next slice here has to solve

1. **The product decision.** A single-player build has 81 unresolved GameSpy SDK symbols. Someone has
   to choose between a loud-failing SDK stub layer (`ghttpStartup` and friends returning failure so
   the menu's online buttons degrade), and the `RTS_BUILD_OPTION_ONLINE` excision — which is now a
   behaviour question with a measurement behind it rather than a way to dodge a compile error.
2. `MainMenuUtils.cpp` unblocks itself once the thread seam (`std::thread` for `CreateThread`) and
   the file-API seam (`_open`/`_close`) land; its DNS lookup wants `getaddrinfo`, which is portable
   and changes Windows behaviour, so it needs the Windows build as oracle.
3. `PingThread.cpp` needs a real ICMP decision (privileged raw socket vs. `SOCK_DGRAM`), or to be
   whatever the online product decision makes it.
4. `StagingRoomGameInfo.cpp`'s SNMP address discovery should not be ported as SNMP. If NAT traversal
   is ever wanted, it is UPnP/NAT-PMP over HTTP, and that is a new implementation.
