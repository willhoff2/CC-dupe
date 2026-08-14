# The Win32 file / module / locale seam

The native 64-bit link's unresolved-symbol report had one category that was entirely the port's own
problem: **`Win32 API`, 43 symbols**. Every name in it is a function the engine calls and Windows
supplies from `kernel32`/`user32`/`version`/`ole32`, and every one of them was *declared* by the
shim headers in `scripts/native-port-shims/` and defined nowhere. Declaring them is what got the
translation units to compile; this slice defines them, under the same spellings, so the call sites
stay byte-for-byte identical on Windows and the link resolves off it.

This continues the pattern of `Core/Libraries/Source/WWVegas/WWLib/platform/` (threads, mutexes,
the monotonic clock, paths, registry-as-settings, process/single-instance, the window). The
difference is the direction the seam faces: `platform_path.h` is a *new* API that call sites were
moved onto, whereas the files here **define the Win32 entry points themselves** and no call site
changes at all.

## 1. Measured

All figures below were produced on Linux/x86-64 with clang 14 by
`scripts/native-build.py --level 1 --level 2 --level 3 --with-shims` and
`scripts/native-port-probe.py`, in this PR **after rebasing onto the level 3 baseline from #44**,
and are the committed baselines. Nothing here was measured on macOS or arm64.

| | before (`main`) | after |
|---|---:|---:|
| Undefined symbols in the `Win32 API` category | **44** | **0** |
| Undefined symbols, total | 393 | **346** |
| Object files (shimmed, levels 1-2-3) | 748 / 829 | **755 / 834** |
| Probe-clean, shimmed | 687 / 744 | **694 / 749** |
| Probe-clean, native (no shims) | 643 / 744 | **647 / 749** |

The measurement was originally taken at levels 1-2, where the category was 43 and the total 280.
Rebasing onto level 3 raised the category to 44: `GetFullPathNameA` is reached only through
`GameEngineDevice`'s `Win32LocalFileSystem::normalizePath()`, so it was invisible at the level the
slice was first measured at, and its own gate caught it (section 5).

The 44 symbols, as classified by `native-build.py` before this slice (the 43 seen at levels 1-2,
plus `GetFullPathNameA` at level 3):

```
CopyFileA            FindClose               GetDateFormatW      GetTimeFormatA   LockResource
CreateDirectoryA     FindFirstFileA          GetDoubleClickTime  GetTimeFormatW   OleInitialize
CreateStdDispatch    FindNextFileA           GetFileTime         GetVersionExA    OleUninitialize
DeleteFileA          FindResourceA           GetFileVersionInfoA GlobalAlloc      SetCurrentDirectoryA
FormatMessageW       GetCommandLineA         GetFileVersionInfoSizeA              SizeofResource
FreeLibrary          GetCurrentDirectoryA    GetLastError        GlobalFree       SysFreeString
GetDateFormatA       GetLocalTime            GetModuleFileNameA  GlobalLock       VerQueryValueA
GetFullPathNameA     GetModuleFileNameW      GetProcAddress      GetSystemDirectoryA
GlobalMemoryStatus
LoadLibraryA         LoadResource            LoadTypeLib         GlobalUnlock     itoa
```

## 2. Where the code lives

| File | Defines |
|---|---|
| `platform/platform_win32_compat.h` | the shared internals: last-error storage, `errno` ↔ Win32 error mapping, `time_t` → `FILETIME`, the wildcard matcher, `Report_Stub()` |
| `platform/platform_win32_file.cpp` | `GetLastError`, `SetLastError`, `FindFirstFileA`, `FindNextFileA`, `FindClose`, `DeleteFileA`, `CopyFileA`, `CreateDirectoryA`, `GetCurrentDirectoryA`, `SetCurrentDirectoryA`, `GetFullPathNameA`, `GetFileAttributesA`, `GetFileTime` |
| `platform/platform_win32_module.cpp` | `LoadLibraryA`, `FreeLibrary`, `GetProcAddress`, `GetModuleFileNameA/W`, `GetSystemDirectoryA`, `GetCommandLineA`, `GlobalAlloc`/`Lock`/`Unlock`/`Free`, `GetVersionExA`, `GlobalMemoryStatus`, `GetLocalTime`, `GetDoubleClickTime`, `itoa` |
| `platform/platform_win32_locale.cpp` | `GetDateFormatA/W`, `GetTimeFormatA/W`, `FormatMessageW` |
| `platform/platform_win32_stub.cpp` | the deliberate failures: PE resources, VERSIONINFO, OLE (section 6) |

All four are compiled **only off Windows** — the CMake `WWLIB_SRC` append is inside the
`if(NOT WIN32)` branch — so the Windows build is untouched by construction. Each file is
additionally wrapped in `#ifndef _WIN32` and in a `__has_include(<windows.h>)` test, so it
contributes nothing if the shim headers are not on the include path, rather than growing a second,
divergent set of Win32 types.

### The linkage trap

The shim headers declare these inside `extern "C"`, matching the real SDK. A definition with C++
linkage would mangle differently, leave the original symbol undefined, and be invisible in the
compile log — the `_strlwr` incident cost 33 translation units this way. Every definition in these
files is inside an `extern "C" { ... }` block, and the check in section 7 is what proves it: the
category is 0 only if the linker actually matched the names.

## 3. `FindFirstFile`: the subtle one

The engine's asset scan (`Win32LocalFileSystem::getFileListInDirectory`) is built on this API, so
its semantics are load-bearing. What is implemented:

* **Snapshot, not a live cursor.** `FindFirstFileA` reads the whole matching directory with
  `opendir`/`readdir` and stores the names; `FindNextFileA` walks the list. Win32 keeps the state
  in the kernel and is also effectively a snapshot for the callers here (the save-game list deletes
  files while enumerating), so this matches what the engine relies on.
* **`.` and `..` come first**, in that order, when the pattern matches them — Windows returns them
  before anything else and code that skips "the first two entries" exists in the wild. The rest are
  sorted case-insensitively; NTFS order is not defined by the API, and a deterministic order is
  more useful than `readdir` order.
* **The DOS pattern language, not `fnmatch`.** `*` and `?` behave as `fnmatch` would (`?` matches
  exactly one character), with two special cases that a POSIX matcher gets wrong:
  * `*.*` means *everything*, including names with no dot at all. ntdll rewrites it to `*`. A
    literal reading silently drops half of a listing.
  * a trailing `.` (`*.`) means *the extension must be empty*, i.e. the name contains no dot. This
    is the pattern the recursive directory scan uses to find subdirectories, so getting it wrong
    stops asset discovery rather than degrading it.
  * `DOS_QM` (a trailing `?` also matching zero characters) and the NT-only `<` `>` `"` wildcards
    are not implemented; nothing in the engine can produce them.
* **`FILE_ATTRIBUTE_DIRECTORY`** is set from `stat`, `FILE_ATTRIBUTE_READONLY` from the write bits,
  `FILE_ATTRIBUTE_NORMAL` otherwise. Sizes fill both `nFileSizeLow`/`High`. Timestamps are
  `st_mtime`/`st_atime` and `st_birthtime` on macOS / `st_ctime` elsewhere, converted to
  `FILETIME`; the Linux creation time is therefore an inode-change time, not a birth time.
* **Errors** are Win32's: `ERROR_FILE_NOT_FOUND` when nothing matches, `ERROR_PATH_NOT_FOUND` when
  the directory does not exist, `ERROR_NO_MORE_FILES` at the end of the walk,
  `INVALID_HANDLE_VALUE` from `FindFirstFileA`, and `GetLastError()` reports them.

**How it differs from Windows:** it lists the directory in one pass, so a file created *after* the
first call is not reported (Win32's behaviour is unspecified here, not opposite); 8.3 short names
are absent (`cAlternateFileName` is empty, as it is on a modern volume with short names disabled);
and `FILE_ATTRIBUTE_ARCHIVE`/`HIDDEN`/`SYSTEM` are not synthesised from POSIX metadata — a
dot-prefixed file is a normal file here, not hidden.

## 4. Case sensitivity, and what it means for assets

The path *components* are resolved through `WWPlatform::Path::Resolve()` (the filesystem seam),
which already walks a path case-insensitively off Windows, and the *pattern* match here is
case-insensitive on every platform. So:

* **macOS** (case-insensitive, case-preserving by default): behaves like Windows, and the
  resolution step is usually a no-op.
* **Linux** (case-sensitive): the seam supplies the case-folding the filesystem does not, so
  `Data\INI\MappedImages\` finds `data/ini/mappedimages/`. The cost is one directory scan per
  component that does not match exactly, which is why `Path::Resolve()` tries the literal path
  first.
* The name reported in `WIN32_FIND_DATA.cFileName` is always the name **as it is on disk**, not as
  the pattern spelled it, on both platforms. Code that round-trips a found name back into an
  `fopen` therefore works; code that compares a found name to a hard-coded literal must compare
  case-insensitively — which the engine already does, because it has always had to on Windows.

Case-insensitive matching on Linux means two files differing only in case (legal there, impossible
on a default macOS or Windows volume) both match one pattern. That is the intended trade: the
shipped data has inconsistent case, and matching Windows matters more than honouring a filesystem
distinction the game cannot express.

## 5. The rest of the surface

* **`GetFullPathNameA`** is what `Win32LocalFileSystem::normalizePath()` is built on, and it appeared
  in the `Win32 API` category only once the build reached level 3 (`GameEngineDevice`), which is why
  it arrived after the rest. It is **lexical**, as Win32's is: a relative path is prefixed with the
  current directory, `.` and `..` are folded away by string manipulation, and the disk is never
  touched — so a path that does not exist still normalizes, which is exactly what the caller wants
  when it is about to create the file. `Path::Resolve()` is deliberately *not* used here: resolving
  looks the name up to recover the retail data's on-disk case, which would turn a normalization of
  an absent path into a failure. Both halves of Win32's two-call buffer protocol are honoured — a
  null or too-small buffer returns the required size *including* the terminator, a sufficient one
  returns the length *excluding* it — and `lpFilePart` points at the last component or is null for a
  directory. Separators come out in the platform spelling, matching what
  `FileSystem::isPathInDirectory()` compares against off Windows.
* **`GetFileAttributesA`** reports only the three attributes the port's `<windows.h>` defines
  (`DIRECTORY` from `stat`, `READONLY` from the write bit, `NORMAL` otherwise) and
  `INVALID_FILE_ATTRIBUTES` when the name does not exist, which is the existence test `agg_def.cpp`
  performs. Archive/system/hidden are not invented from `st_mode`.
* **Modules** — `LoadLibraryA`/`FreeLibrary`/`GetProcAddress` are `dlopen`/`dlclose`/`dlsym`, with
  a Windows DLL name (`foo.dll`) also tried as `libfoo.so`/`libfoo.dylib` and as the bare name.
  `GetModuleFileNameA(nullptr, ...)` is `Path::Get_Executable_Path()`; a non-null `HMODULE` is not
  supported (there is no PE image to name) and reports a stub. `GetModuleFileNameW` widens the
  bytes into `wchar_t` **as the type stands today** — see the note in section 8.
* **`GetCommandLineA`** rebuilds a single command line from `_NSGetArgv` on macOS and
  `/proc/self/cmdline` on Linux, quoting arguments that contain spaces. It is a reconstruction, not
  the original string: the shell's quoting is gone by the time a process can ask.
* **`GlobalAlloc`/`GlobalLock`/`GlobalUnlock`/`GlobalFree`** are `malloc`/`calloc`/`free` with the
  pointer used as its own handle, which is what `GMEM_FIXED` does on Win32. `GlobalUnlock` returns
  `FALSE` with `GetLastError() == NO_ERROR`, exactly as Win32 does for a fixed block. Moveable
  memory (`GMEM_MOVEABLE`) is not implemented; the one caller (`GameMemory.cpp`) allocates fixed.
* **`GetVersionExA`** reports the host OS through `uname` as a plausible `OSVERSIONINFO` (platform
  id `VER_PLATFORM_WIN32_NT`, version from `uname`), because the callers use it to gate "is this at
  least NT" behaviour and to print a diagnostic line.
* **`GlobalMemoryStatus`** is `sysctl(HW_MEMSIZE)` on macOS and `sysinfo` on Linux. macOS has no
  meaningful "free physical memory" figure (the page cache owns the rest), so the available figure
  is the total there; the callers either print it or difference two samples.
* **`GetLocalTime`** is `localtime_r`; **`GetDoubleClickTime`** returns 500 ms, the Windows default,
  because neither Cocoa nor X11 exposes it in a way worth threading through here.
* **`itoa`** is the MSVC CRT extension: a digit loop over radix 2-36, signed only in base 10, which
  is what MSVC does.
* **`GetDateFormatA/W`, `GetTimeFormatA/W`** interpret the Win32 picture strings the engine passes
  (`d`/`dd`/`ddd`/`dddd`, `M`/`MM`/`MMM`/`MMMM`, `y`/`yy`/`yyyy`; `h`/`hh`/`H`/`HH`, `m`/`mm`,
  `s`/`ss`, `t`/`tt`, with quoted literals) over `struct tm`, and fall back to the C locale's
  `%x`/`%X` when no picture is given. `DATE_LONGDATE`, `TIME_NOSECONDS`, `TIME_NOMINUTESORSECONDS`,
  `TIME_FORCE24HOURFORMAT` and `TIME_NOTIMEMARKER` are honoured. The `LCID` is **ignored**: the
  process locale is whatever `setlocale` has been given, so a caller asking for a specific locale
  gets the current one. Every caller in the engine passes `LOCALE_USER_DEFAULT`.
* **`FormatMessageW`** supports `FORMAT_MESSAGE_FROM_SYSTEM`, mapping the compatibility layer's
  error codes back to `errno` and formatting `strerror`. `FROM_STRING`, `FROM_HMODULE` and
  `ALLOCATE_BUFFER` report a stub and fail; the only caller is error-message display.

## 6. Deliberately stubbed, and they fail loudly

Each of these logs through `Report_Stub()` (once per entry point) and returns the Win32 failure
value, so a caller that starts depending on one gets a diagnostic naming the API, not silence:

* **PE resources** — `FindResourceA`, `LoadResource`, `LockResource`, `SizeofResource`. There is no
  PE image off Windows; the data these read is embedded by the Windows resource compiler. The
  callers are the cursor/version plumbing, not the game loop.
* **VERSIONINFO** — `GetFileVersionInfoSizeA`, `GetFileVersionInfoA`, `VerQueryValueA`. Same
  reason. The version string for a native build has to come from the build system instead (there is
  already generated `gitinfo`).
* **OLE / COM** — `OleInitialize`, `OleUninitialize` (a no-op), `LoadTypeLib`, `CreateStdDispatch`,
  `SysFreeString` (a no-op, since nothing here allocates a `BSTR`). This is the embedded-browser
  and `IDispatch` glue, which is Windows-only by nature and not on the path to single-player.
* **`GetSystemDirectoryA`** returns an empty string: there is no `C:\Windows\System32`. Its callers
  build a path to a system DLL, which cannot work off Windows anyway.

## 7. The gate

`scripts/ci/check-win32-undefined.py` reads `native-build.py`'s JSON and compares the `Win32 API`
category against `docs/porting/ci-baselines/win32-undefined-budget.json`, which now lists **no**
allowed symbols. `check-native-build-baseline.py` already ratchets the *total*, which lets a new
Win32 dependency hide behind an unrelated drop; this pins the category by symbol name, so a new
`FindFirstFileW` call site fails CI with the symbol that caused it and a pointer to this document.
It runs in `native-port-ci.yml`'s `native-build` job, right after the baseline gate.

## 8. Also in this slice

* **`GeneralsMD/.../Common/GameEngine.cpp`** set the window title with `SetWindowText`/`SetWindowTextW`
  on `ApplicationHWnd`. Off Windows it goes through the window seam from PR #40 —
  `WWPlatform::Window_Set_Title()` — and the second, wide call is dropped: the seam's entry point
  takes narrow text, and the reason there were two was Win 9x's missing `SetWindowTextW`. The
  Windows path is unchanged. The file's local `extern HWND ApplicationHWnd` is now `#ifdef _WIN32`,
  because `PlatformWindowHost.h` declares it as `void *` off Windows. With that and the ATL shim
  fix below, the translation unit compiles natively for the first time.
* **`scripts/native-port-shims/atlbase.h`** declared `CComModule::Init` with two parameters; the
  real one takes three (object map, instance, optional library id) and `GameEngine.cpp` passes
  three. Declaration-only shim change, no behaviour.
* **`Core/GameEngine/.../GUI/IMEManager.cpp`** is a client of the Windows Input Method Manager from
  top to bottom: `HIMC` contexts, `WM_IME_*` messages, `ImmGetCompositionStringW`, candidate lists.
  There is no portable equivalent, and an input method is not needed for single-player, so the
  Windows implementation is now `#ifdef _WIN32` and a `NullIMEManager` takes its place off Windows:
  never enabled, never composing, no candidates, `serviceIMEMessage()` reports "not serviced", and
  `init()` logs once that IME is unavailable. Every caller already tests `isEnabled()`/
  `isComposing()` before asking for anything, so this switches IME off rather than breaking them.
  **This is a loud stub, not a port** — typing Japanese, Chinese or Korean into the chat box will
  not work off Windows until someone writes the Cocoa (`NSTextInputClient`) implementation.

## 9. What has actually run

The first version of this slice could only claim "it links". `scripts/native-win32-file-test.py`
compiles the file half of the seam together with
`platform/tests/win32_file_api_test.cpp` and runs it against a real directory tree, so the
enumeration semantics in section 3 are now observed rather than argued. **45 assertions, 0
failures** on Linux/x86-64 with clang 14; it runs in `native-port-ci.yml`'s `native-build` job.

What it pins down:

| Claim | Assertion |
|---|---|
| `*.big` is the asset scan's pattern | matches exactly the three archives, and reports each name with its **on-disk** case, not the caller's |
| `*.*` is everything | includes the extensionless name, the subdirectory, and `.`/`..` |
| `*.` means "no extension" | matches the extensionless name, matches nothing with a dot |
| a literal name | enumerates only itself |
| nothing matches | `INVALID_HANDLE_VALUE` **and** `ERROR_FILE_NOT_FOUND`, which is what the callers test |
| case-insensitive lookup | `ini.BIG` finds `INI.big` on a case-**sensitive** filesystem |
| `FILE_ATTRIBUTE_DIRECTORY` | set on the subdirectory and on `.`/`..`, clear on files |
| find data | carries the size and a non-zero last-write `FILETIME` |
| `GetFullPathName` | folds `..`, folds `.`, normalizes a path whose components do not exist, makes a relative path absolute, and both halves of the buffer protocol agree (`required == length + 1`) |
| `lpFilePart` | points at the last component |
| `CopyFile` | copies; refuses to overwrite with `fail_if_exists`; overwrites without it |
| `DeleteFile`/`CreateDirectory` | succeed, then fail on the second call with `ERROR_ALREADY_EXISTS` for the directory |
| `GetCurrentDirectory` | same buffer protocol as `GetFullPathName` |

The fixture is built with `std::filesystem`, not with the seam: a test that created its own tree
through the API under test would pass by agreeing with itself. A deliberately wrong expectation was
introduced once to confirm the test fails when the behaviour differs.

What it does **not** cover: the module and locale halves, the `FILETIME` epoch (the engine only
compares these against each other), 8.3 alternate names, and anything on macOS or arm64 — the test
is written to build on Windows too, where the real API is the implementation and it becomes a
conformance check, but that has not been run.

## 10. Still open

* **The wide-character decision is untouched, by instruction.** `GetDateFormatW`,
  `GetTimeFormatW`, `FormatMessageW` and `GetModuleFileNameW` are implemented over `wchar_t` as the
  tree defines it today, by widening/narrowing bytes. On Linux that is 4 bytes and on Windows 2, so
  these functions write different unit sizes on the two platforms — correct for ASCII output, which
  is all they produce, and wrong the moment a non-ASCII locale string appears. When
  `widechar-fallout.md`'s `WideChar`/`char16_t` question is answered, the four `Copy_Out_Wide`/
  `Narrow` helpers in `platform_win32_locale.cpp` are the only places that need to change.
* **The `FILETIME` conversions and the module/locale halves have still not run.** Section 10 covers
  what the behaviour test now executes; the rest of the surface — `GetFileVersionInfo`,
  `GetDateFormat`, `LoadLibrary`/`GetProcAddress`, the `FILETIME` values' epoch — is still argued
  from the Win32 documentation and the call sites rather than observed, and the native binary as a
  whole still does not run. **Everything in this slice was measured on Linux/x86-64 only; the macOS
  branches (`st_birthtime`, `_NSGetArgv`, `sysctl`, `.dylib` naming) are written blind and
  unverified** — the `macos-15` CI job compiles the Cocoa window path but does not build WWLib's
  compatibility layer yet.
* **8 translation units still fail to compile** (down from 10), all GameSpy-adjacent:
  `DownloadManager.cpp`, `MainMenuUtils.cpp`, `StagingRoomGameInfo.cpp`, the three GameSpy threads,
  and `DownloadMenu.cpp`/`MainMenu.cpp` which include them. GameSpy is out of scope.
* **`Directory.{h,cpp}`** still calls Win32 enumeration directly rather than going through
  `platform_path.h`; it now links, because these definitions exist, but the case-folding index that
  `filesystem-and-registry.md` asks for is still the better answer.
* **The remaining 234 unresolved symbols** are the renderer, audio, device and entry-point layers
  that levels 1-2 do not build (104 of them are `Dict` keys instantiated in `GameEngineDevice`),
  GameSpy, D3D8, and the 8 failed units. None of them are Win32 API.
