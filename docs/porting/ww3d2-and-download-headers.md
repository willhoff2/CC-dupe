# The renderer's remaining Win32 headers, and the patch downloader

Ten translation units of the level-4 native build failed for reasons that were neither the renderer
backend, the D3DX math family, nor the deliberately-cut GameSpy SDK. They split three ways, and only
the third needed a decision:

* **Two headers the harness never had** — `windowsx.h` (`FramGrab.cpp`, `ww3d.cpp`) and `ddraw.h`
  (`ddsfile.cpp`). Both are declaration-only additions to `scripts/native-port-shims/`.
* **Four per-file gaps** — a `size_t` that used to arrive through the precompiled header
  (`w3d_dep.cpp`), an `LPCSTR` in an interface header (`ftp.h`), and two diagnostics the *harness*
  invented by probing `WWDownload` with the renderer's configuration instead of its own
  (`Common/Debug.h` in `Download.cpp`, `sku` in `urlBuilder.cpp`).
* **`textdraw.cpp`, which no compiler has ever compiled.** It is commented out of both WW3D2
  `CMakeLists.txt` files. See §5: it is removed from the measured denominator rather than "fixed".
* **`WWDownload`, the FTP patch downloader**, whose four units all failed, so level 4 produced no
  archive for it at all. It is ported, not excluded. The argument is §4.

`dx8caps.cpp`'s missing `DriverVersion` is the fifth item and it was already answered:
[`win32-runtime-and-crt-gaps.md`](win32-runtime-and-crt-gaps.md) resolved it for
`W3DShaderManager.cpp` *without adding a field* to the vendored header — the DX8 SDK spells the field
twice, `LARGE_INTEGER DriverVersion` under its own `#ifdef _WIN32` and a
`DriverVersionHighPart`/`LowPart` pair without it, and `Utility/d3d8_compat.h`'s
`D3D8AdapterDriverVersion()` reads whichever spelling the header gave. This slice applies that same
answer rather than inventing a second one; `dx8caps.cpp` loses a `LARGE_INTEGER` type pun it did not
need and keeps the same `HIWORD`/`LOWORD` arithmetic on both platforms.

## 1. Measured

Linux/x86-64, `clang++-14 -std=c++20 -m64`, `./scripts/ci/fetch-probe-deps.sh` first, then
`scripts/native-build.py --with-shims`. The "before" column is a fresh run of `main` at `a6ef778ed`
in this session, which reproduced the committed baseline exactly.

| Levels 1-4 (renderer, audio, downloader included) | before (`main`) | after |
|---|---:|---:|
| Object files | 952 / 971 | **962 / 971** |
| Translation units producing no object | 19 | **9** |
| Undefined symbols at link | 412 | **391** |
| …of which "defined in a translation unit that failed to compile" | 178 | **157** |
| Archives linked | 13 | **14** |
| Libraries producing no archive | `WWDownload` | **none** |
| Unresolved Win32 API symbols | 3 | **3** |

| Levels 1-3 | before | after |
|---|---:|---:|
| Object files | 834 / 839 | **835 / 840** |
| Translation units producing no object | 5 | **5** |

The level 1-3 denominator gains one translation unit and loses none: the new
`WWLib/platform/platform_win32_vfw.cpp` (§3). The level 1-4 denominator gains that same unit and
loses `textdraw.cpp` (§5), so the total is unchanged at 971 while ten more units compile. The probe's
own counts move by the same single file: native 669/757 → 670/758, shimmed 712/757 → 713/758.

**The nine remaining failures all belong to other slices** and are reported, not fixed:
`dx8webbrowser.cpp`, `dx8wrapper.cpp`, `W3DDisplay.cpp` on `LPDISPATCH` (browser excision);
`pointgr.cpp`, `sortingrenderer.cpp`, `W3DWater.cpp` on `D3DX_PI`/`D3DXMATRIX`/`D3DXMatrixInverse`
(D3DX math family); `MainMenuUtils.cpp`, `PingThread.cpp`, `StagingRoomGameInfo.cpp` on
`HANDLE`/`HOSTENT`/`AsnObjectIdentifier` (GameSpy, cut scope). The 157 symbols still attributed to
uncompiled units are theirs, plus the Miles and FFmpeg layers that are not linked here.

### Re-measured after the browser and D3DX slices landed

Both columns above were measured before `main` gained the browser excision (#65) and the D3DX math
family (#66), so they no longer describe the tree. Re-measured on `8bb8aff56`, levels 1-4 stand at
**968 / 972 objects, 4 failures, 339 undefined symbols**; the only failure left outside GameSpy is
`dx8wrapper.cpp`, which needs the user32 window-management surface (`GetWindowLong`, `GWL_STYLE`,
`AdjustWindowRect`, `GetMonitorInfo`) that the window/input slice owns. `dx8wrapper.cpp` had a second
copy of the `DriverVersion` arithmetic below, which now calls `D3D8AdapterDriverVersion()` too, so
there is one answer to that vendored-header split in the tree rather than three. The generated
`docs/porting/ci-baselines/*.json` are the authority for all of these.

## 2. The two new shims

Both follow the existing style: declarations and macros only, no definitions, and the vendored
header's own include guard where there is one to share — the lesson `d3d8types.h` records in its
comment, having cost a session.

`windowsx.h` is macros only, which is what the real header mostly is. `FramGrab.cpp` uses the
`Global*Ptr` family (`GlobalAllocPtr`, `GlobalLockPtr`, `GlobalFreePtr`); `ww3d.cpp` reaches it
transitively. The macros expand to the `GlobalAlloc`/`GlobalLock`/`GlobalHandle` calls the real ones
expand to, so they resolve against the existing global-memory implementation in
`WWLib/platform/platform_win32_module.cpp` — which gained `GlobalHandle()` (the inverse of
`GlobalLock()`, i.e. the identity, since a handle there *is* the pointer) so that the family links
rather than merely parses.

`ddraw.h` carries the `DDSCAPS2_CUBEMAP*` and `DDSCAPS2_VOLUME` bits and nothing else. `ddsfile.cpp`
reads them out of a `.dds` file's header; it never touches a DirectDraw interface. Declaring
`IDirectDraw` to satisfy an include that only wants eight constants would be inventing surface.

`SetRect()` — arithmetic on a `RECT`, no display involved — joins the user32 file for the same
link-not-parse reason.

## 3. AVI capture is a loud stub, and says so

`FramGrab.cpp` is the developer frame grabber: it writes an uncompressed AVI through Video for
Windows (`AVIFileOpen`, `AVIStreamWrite`, …). VfW is a Windows multimedia API, the game does not need
it to run, and the macOS equivalent would be a different API entirely — so it takes the same shape as
the `DbgHelp` crash-reporting precedent. `scripts/native-port-shims/vfw.h` declares the surface,
including all fifteen `AVISTREAMINFO` members the file assigns to, and
`WWLib/platform/platform_win32_vfw.cpp` **defines** those entry points as stubs that return
`AVIERR_UNSUPPORTED`, clear their output parameters, and print through
`WWPlatform::Win32::Report_Stub()` the first time each is reached.

The definitions exist for one reason: a declaration-only VfW header turns a compile failure into
eight undefined symbols, which is not progress. Recording a frame grab natively is open work; asking
for one prints a refusal instead of failing silently or crashing.

## 4. Why `WWDownload` is ported rather than excluded

The honest options were to port it or to exclude it from level 4 with a named reason, the way GameSpy
is excluded. Excluding it loses:

* **The single-player link still needs its symbols.** `MainMenu.cpp` calls
  `CancelPatchCheckCallback()` unconditionally on menu shutdown, and `DownloadManager.cpp` — compiled
  at level 2 already, `NEW CDownload(this)` and all — is pumped every frame. Excluding the library
  would move `Cftp::` and `CDownload::` from the "failed to compile" bucket into a "not built at this
  level" bucket. The number would improve and the binary would be no closer to linking.
* **Two of its four diagnostics were the harness's fault.** `z_wwdownload` links
  `zi_gameengine_include` and `zi_always`, so the real build gives it the GameEngine include tree and
  `RTS_ZEROHOUR=1`. The probe was compiling it with the renderer's configuration, which is why
  `Common/Debug.h` was "not found" and `sku` was "undeclared" — neither is reachable in the build
  CMake describes. `scripts/native-port-probe.py` now models the target's real include set and
  defines, and reads its translation units from `CMakeLists.txt` instead of walking the directory.
* **The registry work already existed.** `registry.cpp` is a second, older copy of the four functions
  `GameEngine/Source/Common/System/registry.cpp` already had ported to the settings store in
  `WWLib/platform/`. `KEY_READ` was a seam gap, not new work.

So the library compiles, links, and produces the fourteenth archive.

### What the portable side does

`registry.cpp` keeps its Win32 half byte-for-byte under `#ifdef _WIN32`. Off Windows the same four
entry points read and write `WWPlatform::Settings`, the per-user settings store described in
[`filesystem-and-registry.md`](filesystem-and-registry.md), under the same key paths — so the
downloader still sees values the game wrote, and vice versa.

Two behaviours are deliberately *not* reproduced, because there is nothing off Windows for them to
read:

* **The `HKEY_LOCAL_MACHINE` fallback** that follows every read here. HKLM held what the retail
  installer wrote (install path, language, SKU, version); a native build has no installer and no
  machine-wide store. Reads of it return false, exactly as the GameEngine copy already decided.
* **`FTP.cpp`'s `Use_Non_Blocking_Mode()`** reads an HKLM flag under *another product's* key — a
  Westwood MMO beta whose installer wrote it — as a workaround for firewalls that broke non-blocking
  sockets in 2002. Off Windows the read cannot succeed, so the function returns the `TRUE` it already
  returns on every machine that lacks the key. The Win32 code path is untouched.

Three further `FTP.cpp` changes are genuine 64-bit or BSD-sockets mismatches rather than seam work,
and each keeps the Win32 spelling: the host-lookup thread id becomes a `DWORD` (`CreateThread()`
writes 32 bits; `unsigned long` is 64 on LP64), `getsockname()`'s length becomes `socklen_t`
(`Utility/socket_compat.h` typedefs it to `int` on Windows), and `sin_addr.S_un.S_addr` becomes
`sin_addr.s_addr`, the spelling both platforms accept. `WSAECONNRESET`/`WSAENOTCONN` join
`socket_compat.h` as the BSD codes under the Winsock names; `_splitpath()`, `_mkdir()` and
`_chmod()`/`_S_IREAD`/`_S_IWRITE` join a new `Utility/path_compat.h`, which is where the MSVC CRT
path spellings belong — `_splitpath` reimplemented to its documented behaviour, since
`basename()`/`dirname()` neither split the extension nor write into caller buffers.

**The patch downloader is not thereby usable.** Nothing about downloading a patch is verified here;
no server was contacted. What is claimed is that the library compiles, links, and stores its settings
somewhere real, so the single-player binary can be linked.

## 5. `textdraw.cpp` leaves the denominator

`Core/Libraries/Source/WWVegas/WW3D2/CMakeLists.txt` reads `#textdraw.cpp # unused`, and the
GeneralsMD copy is commented out too. MSVC has therefore never compiled it, which is why its
`font->Peek_Texture(ch)` call passes an argument to a function that takes none — the font class lost
its per-char texture and the dead file kept the old call. Fixing it would be writing new renderer
code to satisfy a measurement, so instead the harness stops inventing the failure: both
`native-port-probe.py` and `native-build.py` now take the renderer, audio and downloader targets'
translation units from `CMakeLists.txt`, as the GameEngine targets already did, instead of walking
the directory with `rglob("*.cpp")`.

`textdraw.cpp` is the only file in that tree CMake comments out, and
`scripts/ci/check-download-seam.py` fails if another appears, so the denominator cannot shrink
quietly.

## 6. The gate

`scripts/ci/check-download-seam.py`, wired into `native-port-ci.yml`'s level 1-4 job, holds the three
claims this slice makes:

* every `WWDownload` translation unit produces an object and the library produces an archive;
* `registry.cpp` goes through `WWPlatform::Settings` off Windows, keeps its `_WIN32` branch, and has
  no `Reg*` call outside it;
* the downloader's consumers (`MainMenu.cpp`, `DownloadManager.cpp`) keep the Win32 spelling and
  carry no portability `#ifdef`;
* no source other than `textdraw.cpp` is absent from the measured set by being commented out of
  CMake.

`check-win32-undefined.py` continues to pass with its budget of three (`GetCursorPos`,
`ScreenToClient`, `SetCursor`, all the mouse-cursor path, another slice's).

## 7. Still open

* Native AVI capture (§3) — stubbed, loud, and not on the path to running the game.
* The patch downloader's runtime behaviour off Windows is unexercised: no FTP transfer has been run.
* HKLM-backed values (install path, language, SKU) have no native source; whatever eventually
  provisions them will decide what a native install looks like.
* The nine remaining level-4 compile failures belong to the browser, D3DX and GameSpy slices.
