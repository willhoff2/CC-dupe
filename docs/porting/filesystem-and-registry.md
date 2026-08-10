# Filesystem/paths and the registry

Two rows of the "Platform behaviour left for a later slice" table in
`docs/porting/prerts-win32-surgery.md`: **Filesystem / paths** and **Registry**.

The result is one new module, `Core/Libraries/Source/WWVegas/WWLib/platform/platform_path.{h,cpp}`,
and a non-Windows back end for the engine's `registry.cpp` written in terms of the
`platform_settings` module that already existed. No public engine signature changed.

## 1. What was measured, before

Win32 path/enumeration entry points reached from `Core/GameEngine`, `GeneralsMD/Code/GameEngine`,
`Generals/Code/GameEngine` and `Core/Libraries/Source/WWVegas` (`.cpp`/`.h`, whole-word match):

**143 references, in 37 files, reaching 18 distinct entry points.**

| Entry point | Refs | | Entry point | Refs |
|---|---:|---|---|---:|
| `GetModuleFileName` | 15 | | `FormatMessage` | 10 |
| `CopyFile` | 14 | | `FindNextFile` | 11 |
| `WIN32_FIND_DATA` | 13 | | `DeleteFile` | 12 |
| `SetCurrentDirectory` | 12 | | `CreateDirectory` | 10 |
| `FindFirstFile` | 9 | | `FindClose` | 7 |
| `GetCurrentDirectory` | 7 | | `_access` | 6 |
| `SHGetKnownFolderPath` | 6 | | `CSIDL_*` | 4 |
| `SHGetSpecialFolderPath` | 2 | | `SHGetSpecialFolderLocation` | 2 |
| `SHGetPathFromIDList` | 2 | | `getcwd` | 1 |

Registry: **45 references across 24 non-tool C++ files**, all of them behind five engine-level
functions in `registry.cpp` (`GetStringFromRegistry`, `GetStringFromGeneralsRegistry`,
`GetUnsignedIntFromRegistry`, `SetStringInRegistry`, `SetUnsignedIntInRegistry`) plus
`WWLib/registry.cpp`'s `RegistryClass`, which already had a non-Windows implementation.

Probe (`scripts/native-port-probe.py`, clang 14, with the fetched dx8/gamespy/miles/lzhl headers,
i.e. the same configuration the checked-in baselines were captured in):

| Mode | Before | After |
|---|---|---|
| native | 489 / 737 | **492 / 738** |
| shimmed | 638 / 737 | **641 / 738** |

The totals go up by one because `platform_path.cpp` is a new translation unit; it is clean in both
modes. `docs/porting/ci-baselines/*.json` are regenerated in this PR.

## 2. What changed

`platform_path.h` declares 12 functions and one small `EntryClass`:

```
Exists  Create_Directory  Delete_File  Copy_File
Get_Current_Directory  Set_Current_Directory  Get_Executable_Path
Enumerate  Has_Match
Get_User_Data_Root  Get_Desktop_Directory  Get_Last_Error_Text
Path::SEPARATOR                       ('\\' on Windows, '/' elsewhere)
```

Unlike the other `platform/` modules, this one is compiled **everywhere**. On Windows every entry
point is the call its call sites used to make (`_access`, `CreateDirectory`, `DeleteFile`,
`CopyFile`, `GetCurrentDirectory`, `SetCurrentDirectory`, `GetModuleFileName`,
`FindFirstFile`/`FindNextFile`/`FindClose`, `SHGetKnownFolderPath` with the same runtime probe and
the same `SHGetSpecialFolderPath(CSIDL_PERSONAL)` fallback, `SHGetSpecialFolderLocation` +
`SHGetPathFromIDList`, `FormatMessage`) so that moving a call site behind the header cannot change
Windows behaviour. Off Windows it is `std::filesystem` + `fnmatch(FNM_CASEFOLD)` + `getcwd`/`chdir`
+ `/proc/self/exe` or `_NSGetExecutablePath`, and `strerror`.

Call sites converted (the five files this slice owns):

| File | Was |
|---|---|
| `Core/GameEngine/.../System/Image.cpp` | `WIN32_FIND_DATA` + `FindFirstFile` probe for `*.ini` → `Has_Match` |
| `Core/GameEngine/.../INI/INIWebpageURL.cpp` | `<direct.h>`, `getcwd`, hardcoded `\` → `Get_Current_Directory`, `Path::SEPARATOR` |
| `GeneralsMD/.../Common/GlobalData.cpp` | `SHGetKnownFolderPath`/`CSIDL_PERSONAL`, `CreateDirectory`, `GetModuleFileName` |
| `GeneralsMD/.../SaveGame/GameStateMap.cpp` | `Get/SetCurrentDirectory`, `FindFirstFile` loop, `DeleteFile` |
| `GeneralsMD/.../Menus/ReplayMenu.cpp` | `DeleteFile`, `CopyFile`, `CSIDL_DESKTOPDIRECTORY`, `FormatMessage` |

Registry: `registry.cpp` in both games keeps its five public functions and its
HKCU-then-HKLM lookup order. The `HKEY` helpers are now `#ifdef _WIN32`; off Windows the same key
paths are section names in the `platform_settings` store. `WWLib/registry.cpp` was **not** changed:
it already routes every operation through `platform_settings` under `#ifndef _WIN32`, so the
engine's front end and `RegistryClass` now share one store rather than two.

After the change the same scan finds **121 references in 35 files** outside `platform_path.*`, of
which 4 are mentions in the new explanatory comments. What is left is listed in §6.

## 3. Case sensitivity — measured, and fixed in the API

In-scope engine sources (`Core/GameEngine` + `GeneralsMD/Code/GameEngine`, `.cpp/.h/.hpp/.inl`):

- **266 string literals containing a backslash**, in **65 files**
- **72 `'\\'` character literals**, in **37 files**

That is the count of path *spellings*, and it excludes the retail data itself — the INI, map and
`.big` contents that name assets in whatever case the artist typed. Fixing either the code literals
or the data is out of the question at this size and neither would be sufficient without the other.

So the fix is in the path API: off Windows every entry point converts `\` to `/` and then resolves
the path one component at a time, matching each component case-insensitively (`strcasecmp`) against
what is actually on disk. When a component matches nothing, the literal spelling is kept for the
remainder — that is what makes "create a file that does not exist yet" still work — and the call
reports failure only if the operation genuinely needed the path to exist.

The cost is a directory scan per unresolved component. That is acceptable for the ~20 call sites
here, all of them cold (startup, menu actions, save-game housekeeping). It would not be acceptable
inside `FileSystem`/`ArchiveFile` asset lookup, which is a different slice and should get a cached
case-folded index rather than reusing this walk.

## 4. Where user data lives off Windows — a decision to overrule if you disagree

`GlobalData::BuildUserDataPathFromRegistry()` asks the shell for **Documents** and appends the
registry's `UserDataLeafName`. There is no Documents folder off Windows, so `Get_User_Data_Root()`
returns:

| Platform | Root | Full example |
|---|---|---|
| Windows | `FOLDERID_Documents` (unchanged) | `C:\Users\x\Documents\Command and Conquer Generals Zero Hour Data\` |
| macOS | `~/Library/Application Support` | `~/Library/Application Support/Command and Conquer Generals Zero Hour Data/` |
| Linux/other | `$XDG_DATA_HOME`, default `~/.local/share` | `~/.local/share/Command and Conquer Generals Zero Hour Data/` |

`$CNC_USER_DATA` overrides it on all non-Windows platforms. The leaf name still comes from the
settings store, so it stays localisable exactly as before.

This deliberately differs from `platform_settings`, which puts the *settings* file under
`~/Library/Application Support/...` on macOS and `$XDG_CONFIG_HOME` on Linux. Saves, replays and
user maps are data, not configuration, hence `XDG_DATA_HOME` for them. A maintainer who would
rather see everything in one directory should say so; it is a one-line change in
`Get_User_Data_Root()`.

Desktop (used only by "copy replay to desktop") is `$XDG_DESKTOP_DIR` if set, else `$HOME/Desktop`.
The correct answer lives in `$XDG_CONFIG_HOME/user-dirs.dirs`, which is shell syntax and needs a
parser; the cost of being wrong is a replay landing in the wrong folder.

## 5. Install-path discovery has no answer off Windows

The retail installer wrote `InstallPath`, `Language`, `SKU`, `Version` and `MapPackVersion` under
`HKEY_LOCAL_MACHINE`, and every engine read is "HKCU first, then HKLM". Off Windows there is one
per-user settings file and no machine-wide store, so:

- HKCU reads and writes go to the settings store, and behave as before.
- **HKLM reads always return false.** There is no installer on a native build, so nothing would
  have written those values anyway.

The practical consequence is that `GetRegistryLanguage()` falls back to `"english"` and
`GetRegistryGameName()`/install path lookups return empty unless the user (or a future launcher)
writes them into the settings store first. Install-path discovery therefore has to be solved
elsewhere: the honest answer for a native build is the executable's own directory
(`Get_Executable_Path()` is now available for exactly this) or an explicit `--data-dir`/env var.
That decision belongs to whoever does the startup/asset-mounting slice; this slice only makes sure
the lookup fails cleanly instead of not compiling.

## 6. Deliberately not done

- **`GameState.{h,cpp}` and `Recorder.h`** — owned by the concurrent `SYSTEMTIME`/save-format
  slice. `GameState.cpp` has 12 of the remaining path references (`FindFirstFile` over the save
  directory, `CreateDirectory`, `DeleteFile`) and converting them would collide. Note that
  `GameStateMap.cpp` and `ReplayMenu.cpp` still fail the native probe *only* because they include
  `GameState.h`, which includes `<windows.h>` for `SYSTEMTIME`: their own path code is now portable
  and they become clean as soon as that slice lands.
- **`GlobalData.cpp` keeps `<windows.h>`** for `GetDoubleClickTime()`. That is input, not paths.
- **`Generals/Code`** (the base game) — out of scope by instruction; its `GlobalData.cpp`,
  `GameStateMap.cpp` and `ReplayMenu.cpp` still call Win32 directly and can be converted
  mechanically with this API when someone wants base-game parity.
- **`Directory.{h,cpp}`** (8 refs per game) — a whole `FileSystem`-level directory-listing type
  with its own `FILE_ATTRIBUTE_*`/timestamp semantics. Wrapping it means deciding what a
  `WIN32_FIND_DATA` timestamp becomes off Windows, which is the file-metadata question the
  save-format slice is already answering for `SYSTEMTIME`. Left alone on purpose.
- **`MiniDumper.cpp`, `StackDump.cpp`, `Except.cpp`** — crash reporting, its own table row.
- **File metadata** is absent from `platform_path.h` (`EntryClass` carries a name and a directory
  flag, nothing else) because nothing this slice touches needs it and inventing a portable
  timestamp type here would prejudge the above.
- `Core/Tools` and `GeneralsMD/Code/Tools` were not touched and still compile: they use the Win32
  APIs directly and are Windows-only targets.

## 7. What the next slice in this area has to solve

1. `Directory.{h,cpp}` + `FileSystem`/`ArchiveFile`: a case-folded index of the mounted data
   directories, built once at startup. The per-call resolve in `platform_path` is correct but too
   slow for asset lookup, and it does not see inside `.big` archives, where the case problem also
   exists.
2. A native definition of "where is the game installed", replacing the HKLM `InstallPath` read.
3. `Generals/Code` parity, once the Zero Hour path is proven.
4. Deciding whether the settings store and the user-data root should be the same directory.
