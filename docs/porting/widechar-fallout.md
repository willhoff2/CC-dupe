# WideChar → char16_t: measured fallout

`Core/Libraries/Include/Lib/BaseType.h` declares:

```cpp
typedef wchar_t WideChar;
```

`wchar_t` is 2 bytes with MSVC and 4 bytes on macOS/Linux, while the `.csf`/`.str` string data and
every wide literal in the codebase are 16-bit units. The review's recommendation — `char16_t` — is
correct. It was **not** applied in this change, because the fallout is far past the "hundreds of
files, stop and measure" threshold the task set. The numbers below are why.

## Measurements

Taken over `Core/`, `Generals/` and `GeneralsMD/`, `*.cpp` and `*.h` only, at the commit this
document was added.

| What | Files | Occurrences |
| --- | ---: | ---: |
| `WideChar` | 72 | 277 |
| `wchar_t` | 34 | 152 |
| `L"..."` literals | 252 | 1302 |
| wide libc calls (`wcslen`, `swprintf`, `wcscpy`, `iswspace`, …) | 49 | 219 |
| Win32 wide types (`LPWSTR`, `LPCWSTR`, `WCHAR`) | 23 | 213 |
| **Union of the above** | **337** | — |

Counts are of the whole word, e.g. `\bWideChar\b`, over `*.cpp` and `*.h` in `Core/`,
`Generals/` and `GeneralsMD/`.

> Corrected 2026-08 while measuring the sockets/text-encoding slice. Three rows were wrong. The
> `WideChar` file count was 92, which is the count for the *substring* `WideChar` and so also
> counts the 20 files that only mention `MultiByteToWideChar`/`WideCharToMultiByte`; the
> occurrence figure in the same row, 277, was already the whole-word count, so the row mixed two
> measurements. The wide-libc file count was 66 and is 49 for the regex the document itself
> gives (its occurrence figure, 219, reproduces exactly). The `L"` row was 250/1301 when the
> document was written and is 252/1302 now; that one is drift, not an error. The union is
> recomputed from the corrected sets. None of the corrections change the conclusion — the
> fallout is still ~340 files.

Reproduce with:

```bash
grep -rowIE '\bWideChar\b' --include='*.cpp' --include='*.h' Core Generals GeneralsMD | wc -l
grep -rlIE '\bWideChar\b' --include='*.cpp' --include='*.h' Core Generals GeneralsMD | wc -l
grep -rlI  'L"'           --include='*.cpp' --include='*.h' Core Generals GeneralsMD | wc -l
grep -rlIE '\b(wcslen|wcscpy|wcsncpy|wcscmp|wcsicmp|swprintf|vswprintf|wcstombs|mbstowcs|iswspace|iswalpha|iswdigit|wcschr|wcsstr|towupper|towlower|fgetws|fputws)\b' \
           --include='*.cpp' --include='*.h' Core Generals GeneralsMD | wc -l
```

## What the width reaches: disk and wire

The table above counts source mentions. The reason the type cannot be quietly widened is that
`sizeof(WideChar)` is a *format* constant in this codebase: it appears in 35 places in 15 files
(`Core/GameEngine`, `Core/Libraries`, `GeneralsMD/Code/GameEngine`), and these are the ones that
leave the process:

| Where | Sites | What doubles |
| --- | ---: | --- |
| `Xfer.cpp`, `XferSave.cpp`, `XferLoad.cpp` | 3 | every `UnicodeString` in a save game |
| `XferCRC.cpp` | 1 | the game-state CRC, i.e. the lock-step desync check and replay validation |
| `GameText.cpp` | 1 | `.csf` string table reads, `len*sizeof(WideChar)` straight into the buffer |
| `DataChunk.cpp` (both games) | 3 | wide strings in `.map` chunks, read and written |
| `LanguageFilter.cpp` | 2 | the bad-word list file, read one `WideChar` at a time |
| `LocalFile.cpp` | 4 | `writeWideChar`/`printfWideChar` output |
| `NetPacketStructs.h/.cpp` | 6 | wide strings in network packets |
| `LANAPI.h` | 11 `WideChar` array members | `LANMessage`, which is `#pragma pack(1)` and sent as raw bytes |

`LANMessage` is the sharpest example, and it is measurable rather than arguable:
`sizeof(LANMessage)` is **471** bytes with a 2-byte `WideChar` and **536** with a 4-byte one,
against a `MAX_LANAPI_PACKET_SIZE` of 476. The `static_assert` at `LANAPI.h:270` already catches
it — compile `LANAPI.cpp` with the probe's shims and it is the only diagnostic the file produces:

```
static_assert failed due to requirement 'sizeof(LANMessage) <= MAX_LANAPI_PACKET_SIZE'
```

So the wire format does not silently change under LP64; it fails to build, which is the right
failure. The file formats have no such guard, and `.csf`, `.map`, save games and the CRC would all
silently mis-parse or mis-hash.

## Why it is not a typedef change

1. **Literals.** `char16_t` is a distinct type; `L"foo"` is `const wchar_t*` and will not convert.
   All 1302 wide literals become `u"foo"`, and every one of them lives in a file that must be
   re-checked for the two points below.
2. **No libc.** There is no `char16_t` equivalent of `wcslen`/`swprintf`/`iswspace`. Each of the
   219 call sites needs either a hand-written helper or a conversion at the boundary. `swprintf`
   in particular is used with format strings, so it cannot be swapped mechanically.
3. **Win32 interop.** The W-suffixed Win32 API takes `wchar_t*`. On Windows the two types have the
   same width and representation, but they are still distinct types, so every call that passes a
   `WideChar*` into the API needs a reinterpret_cast at the boundary. That is safe on Windows and
   *not* safe on LP64 — which is exactly the reason a half-conversion is worse than none: it would
   compile on Windows while silently truncating natively.

## Recommendation

Do it as its own change, in this order, and not before the native build can actually link:

1. Introduce `WideChar` helpers (`wcslen`/`swprintf`/… equivalents over 16-bit units) and move the
   219 libc call sites onto them while `WideChar` is still `wchar_t`. This step is a no-op on
   Windows and can be verified by the Windows build alone.
2. Add explicit casts at the Win32 boundary, still a no-op on Windows.
3. Only then flip the typedef and mechanically rewrite `L"` → `u"` in the affected files.

Steps 1 and 2 are individually verifiable against the Windows build; step 3 is the only one that
cannot be, and by then it is a mechanical change with nothing else riding on it.
