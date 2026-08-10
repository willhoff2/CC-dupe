# Sockets and text encoding

Two rows of the "Platform behaviour left for a later slice" table in
[prerts-win32-surgery.md](prerts-win32-surgery.md): **Winsock** (`udp.cpp`, `Transport.cpp`,
`IPEnumeration.cpp`) and **Text encoding** (`ThreadUtils.cpp`, `GlobalLanguage.cpp`).

Probe, `clang++ -fsyntax-only -std=c++20 -m64`, with the fetched dx8/gamespy/miles/lzhl headers
present and clang 14:

| Mode | Before | After |
| --- | ---: | ---: |
| native (no Windows SDK) | 489 / 737 | 493 / 737 |
| shimmed (`--with-shims`) | 638 / 737 | 641 / 737 |

All of the movement is in `Core/GameEngine` (native 107 → 111, shimmed 170 → 173). The newly
clean translation units are `udp.cpp`, `Transport.cpp`, `ThreadUtils.cpp` and
`GlobalLanguage.cpp`. Both Windows builds are green:
`./scripts/docker-build.sh --clean --game zh` and `--game generals`.

## What was measured

### Winsock

Whole-word count of Winsock-only identifiers (`WSA*`, `SOCKET`, `INVALID_SOCKET`, `SOCKET_ERROR`,
`closesocket`, `ioctlsocket`, `HOSTENT`, `SOCKADDR*`, `FIONBIO`, `FIONREAD`, `MAKEWORD`, `LOBYTE`,
`HIBYTE`, `S_un`) over `*.cpp`/`*.h`:

| Tree | Files | Occurrences | Distinct identifiers | BSD-spelled socket calls in the same files |
| --- | ---: | ---: | ---: | ---: |
| **in scope** | **7** | **156** | **66** | **67** |
| GameSpy (out of scope) | 7 | 97 | 62 | 46 |
| `Core/Tools`, `GeneralsMD/Code/Tools` (out of scope) | 11 | 120 | 22 | 173 |
| `Generals/Code` (out of scope) | 1 | 1 | 1 | 0 |

The seven in-scope files, by occurrence count: `udp.cpp` 83, `WWDownload/FTP.cpp` 32,
`IPEnumeration.cpp` 20, `Transport.cpp` 12, `DownloadManager.cpp` 7, and one incidental match each
in `FirewallHelper.cpp` (`MAX_SPARE_SOCKETS`) and `LanLobbyMenu.cpp` (the string
`"SOCKET ERROR!"`), neither of which is Winsock at all.

66 distinct identifiers sounds like a large API surface and is not: 52 of the 66 are Winsock
error constants in one `switch` in `udp.cpp`'s `GetWSAErrorString()`, which exists only to print
a name in a debug log. The Winsock-only names actually used are eight — the functions
`WSAStartup`, `WSACleanup`, `WSAGetLastError`, `closesocket`, `ioctlsocket` and the types
`SOCKET`, `WSADATA`, `HOSTENT` —
and everything else in these files (`socket`, `bind`, `sendto`, `recvfrom`, `select`,
`setsockopt`, `getsockname`, `gethostname`, `gethostbyname`, `inet_addr`, `htonl`/`ntohl`, …) is
already spelled the BSD way, because Winsock 1 copied the BSD API. That is the whole reason a
compatibility header works here.

### Text encoding

Calls (not mentions) of the two conversion functions, `Core/` and `GeneralsMD/`, excluding
`Tools`:

| File | `MultiByteToWideChar` | `WideCharToMultiByte` | Code page |
| --- | ---: | ---: | --- |
| `GameSpy/Thread/ThreadUtils.cpp` | 1 | 2 | `CP_UTF8` |
| `GameClient/GUI/IMEManager.cpp` | 5 | — | `CP_ACP` |
| `WWLib/widestring.cpp` | 2 | — | `CP_ACP` |
| `WWLib/wwstring.cpp` | — | 2 | `CP_ACP` |

Twelve call sites in four files. `ThreadUtils.cpp` is the interesting one: its two functions,
`MultiByteToWideCharSingleLine()` and `WideCharStringToMultiByte()`, are what the rest of the
engine actually uses, at 27 call sites in 9 files — so the whole engine's UTF-8 conversion is
three Win32 calls wide.

Font registration is two calls, both in `GlobalLanguage.cpp`: `AddFontResource` in `init()` and
`RemoveFontResource` in `reset()`.

## What changed

### `Dependencies/Utility/Utility/socket_compat.h` (new)

On Windows it is `#include <winsock.h>` and one `typedef int socklen_t`, so the Windows
preprocessed output is what it was. Off Windows it pulls in the BSD headers and supplies the
Winsock-only names: `SOCKET`, `INVALID_SOCKET`, `SOCKET_ERROR`, `HOSTENT`, `SOCKADDR*`,
`MAKEWORD`/`LOBYTE`/`HIBYTE`, `WSADATA`, and inline `WSAStartup` (fills in the version so the
callers' `LOBYTE(wsadata.wVersion) != 2` check behaves), `WSACleanup`, `WSAGetLastError`
(`errno`), `WSASetLastError`, `closesocket` (`close`) and `ioctlsocket` (`ioctl`).

It is deliberately **not** included from `Utility/compat.h`. `compat.h` reaches every translation
unit through `BaseTypeCore.h`, and putting `<sys/socket.h>` there would make 737 translation units
pay for six files' worth of sockets. The five files that talk to sockets include it directly.

`ioctlsocket` is not a rename: Winsock's argument is `unsigned long*` and BSD's is `int*`, so the
value is narrowed and widened rather than reinterpreted — reinterpreting would read the wrong half
of the word on a big-endian LP64 host. `socklen_t` is the same story for
`getsockname`/`recvfrom`/`getsockopt`: Winsock 1 says `int*`, BSD says `socklen_t*`, and on LP64
they are not interchangeable. The four affected locals in `udp.cpp` are now `socklen_t`, which is
`int` on Windows.

### `udp.cpp` / `udp.h`

The `#ifdef _WIN32` blocks around the error handling in `Bind`, `Write` and `Read` are gone: the
code inside them is unchanged and now compiles on both, because `SOCKET_ERROR` is -1 and
`WSAGetLastError()` is `errno`. This is a fix off Windows, where `m_lastError` was previously
never assigned and `GetStatus()` therefore always reported the previous error. `GetWSAErrorString`
keeps its 52-constant `switch` on Windows and returns `strerror()` off it — the log line reads
`Connection refused` instead of `WSAECONNREFUSED`.

One latent bug surfaced: in the non-Windows half of `GetStatus()`, `case EAGAIN:` and
`case EWOULDBLOCK:` are the same value on Linux and macOS, so the switch does not compile. The
`EWOULDBLOCK` label is now `#if EWOULDBLOCK != EAGAIN`.

### `Transport.cpp`

`windows.h` is now Windows-only and `winsock.h` became the compat header. Two per-site fixes:
`from.sin_addr.S_un.S_addr` → `from.sin_addr.s_addr` (`S_un` is a Microsoft-specific union inside
`in_addr`; `s_addr` is the standard spelling and is the same member on Windows, where `S_addr` is
itself a `#define` for it). The packet encryption's `htonl` calls are untouched, so the wire bytes
are the same bytes.

### `IPEnumeration.cpp`

Now two functions under `#ifdef _WIN32`. The Windows one is untouched. The other uses
`getifaddrs()`, and the semantics genuinely differ, which is documented at the function:

* Windows asks the *resolver* for the addresses registered against the host name;
  `getifaddrs()` asks the *kernel* for the addresses configured on the interfaces. These are
  different questions and a misconfigured host answers them differently.
* `getifaddrs()` also reports loopback and down interfaces. Both are filtered out, because the
  Windows list contains neither and because the multi-instance feature invents its own `127.x`
  address that must stay the only loopback entry.
* `getifaddrs()` reports IPv6 and link-layer entries; only `AF_INET` is taken, because
  `EnumeratedIP` holds a 32-bit IPv4 address and the LAN protocol puts one on the wire.

This translation unit is still not clean natively, but no longer for a socket reason: it includes
`GameClient/ClientInstance.h`, which needs `HANDLE` and `CreateMutex` for its single-instance
mutex. That belongs to whoever ports `ClientInstance` onto the existing `platform_mutex`.

### `Dependencies/Utility/Utility/unicode_compat.h` (new)

`wchar_compat.h` previously carried

```cpp
#define MultiByteToWideChar(cp, flags, mbstr, cb, wcstr, cch) mbstowcs(wcstr, mbstr, cch)
#define WideCharToMultiByte(cp, flags, wcstr, cch, mbstr, cb, defchar, used) wcstombs(mbstr, wcstr, cb)
```

which is not what those functions do: `mbstowcs` ignores the code page, converts in the current C
locale (`"C"`, i.e. ASCII-only, unless something calls `setlocale`), returns `(size_t)-1` rather
than 0 on failure, and has no "measure the output" form. All four in-scope callers pass a code
page and two of them rely on the `-1`/`0` length conventions. The macros are replaced by real
inline functions implementing the Win32 contract: `-1` means null-terminated and counts the
terminator, a zero destination size means "return the required length", 0 means failure.

One thing found while matching the contract and deliberately left alone:
`MultiByteToWideCharSingleLine()` allocates `len+1` wide characters for a `len`-byte string and
then passes `cchWideChar = len`, one short of what a `cbMultiByte = -1` call needs for the
terminator it is asked to write. On Windows that call fails with `ERROR_INSUFFICIENT_BUFFER` and
writes nothing, and the function then reads the uninitialised buffer. The implementation here
fails the same way rather than being quietly more forgiving, because the alternative is a
platform-dependent bug. Fixing it changes Windows behaviour in the GameSpy chat path and belongs
in its own change.

Two differences from Windows that cannot be removed at this layer, both consequences of the wide
type:

* the wide form is UTF-32, not UTF-16, so a non-BMP character is one `wchar_t` here and a
  surrogate pair on Windows. Everything the game ships in `.csf`/`.str` is BMP.
* `CP_ACP` is the system ANSI code page on Windows (1252 for a Western install). There is no such
  thing off Windows; the honest approximation is UTF-8, which agrees for ASCII and disagrees
  above it. The five `IMEManager.cpp` call sites are the ones this affects, and the IME is a
  Win32 subsystem that has to be reimplemented anyway.

### `GlobalLanguage.cpp`

`AddFontResource`/`RemoveFontResource` install a font file into the GDI font namespace for the
lifetime of the process, so that the GDI rasteriser behind the W3D font engine can find the
language pack's fonts by family name. There is no process-scoped equivalent off Windows —
fontconfig and CoreText both want the file loaded into the text system that will rasterise it —
so the two loops are Windows-only and the non-Windows path does nothing at all rather than
pretending to succeed. The parsed file names stay in `m_localFonts` either way, so whoever ports
the font rasteriser has the list. Faking success here would have meant `DEBUG_CRASH` on every
language pack font, or a silent lie.

### `WWDownload/ftp.h`, `DownloadManager.cpp`

`ftp.h` was the last direct `#include <winsock.h>` outside GameSpy and the tools; it now includes
the compat header, which is also how `DownloadManager.cpp` gets `WSAStartup`. Neither is in the
default probe scope (`FTP.cpp` is a renderer-scope target and still fails on `process.h`), but
leaving one file including `<winsock.h>` for no reason would have been worse.

## Deliberately not done

**`WideChar` is still `wchar_t`.** The conversion functions were easy; the underlying type is a
separate and much larger slice. This is not a guess — see
[widechar-fallout.md](widechar-fallout.md), whose numbers this slice re-measured and corrected:
337 files mention wide characters, and `sizeof(WideChar)` is a *format* constant in 35 places, 8
of which are file or network formats. Concretely, `sizeof(LANMessage)` is 471 bytes with a 2-byte
`WideChar` and 536 with a 4-byte one, against a `MAX_LANAPI_PACKET_SIZE` of 476: the
`static_assert` in `LANAPI.h` fails under LP64 today, which is why `LANAPI.cpp` is one of the
translation units this slice did *not* make clean. Save games, the game-state CRC, `.csf` string
tables, `.map` chunks and the bad-word list have no such guard and would silently mis-parse.

**GameSpy.** Seven files, 97 Winsock identifier occurrences, 46 BSD-spelled socket calls. They
compile as they did; the SDK is not vendored and online play is cut, so pointing them at the
compat header would be churn with nothing to verify it.

**`Core/Tools` and `GeneralsMD/Code/Tools`.** 11 files, 120 occurrences — mangler and matchbot
carry their own `wnet/udp.h` and `wnet/tcp.h`, which are a second, older copy of the same
abstraction. Out of scope, untouched, still compiling.

**Blocking sockets, `select()` and the receive loop.** Unchanged. The port has no runtime yet, so
there is nothing to observe and no reason to touch the shape of the I/O.

## What the next slice here has to solve

1. `ClientInstance` (`HANDLE`/`CreateMutex`/`GetLastError`) — the last Win32 dependency between
   `IPEnumeration.cpp` and a clean compile, and `platform_mutex` already exists.
2. `WWDownload`: `process.h`/`_beginthread` in `FTP.cpp`, which is the thread slice, not this one.
3. The wide type. Steps 1 and 2 of the plan at the end of
   [widechar-fallout.md](widechar-fallout.md) are verifiable against the Windows build alone and
   should be done before anything tries to read a `.csf` natively.
4. `getifaddrs()` is in glibc and on macOS, but the `ifa_flags` bits are only mostly the same;
   when there is a runtime, the enumerated list should be checked against `ifconfig` on both.
