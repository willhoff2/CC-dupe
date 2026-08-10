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
| `WideChar` | 92 | 277 |
| `wchar_t` | 34 | 152 |
| `L"..."` literals | 250 | 1301 |
| wide libc calls (`wcslen`, `swprintf`, `wcscpy`, `iswspace`, …) | 66 | 219 |
| Win32 wide types (`LPWSTR`, `LPCWSTR`, `WCHAR`) | 23 | — |
| **Union of the above** | **345** | — |

Reproduce with:

```bash
grep -rlI  "WideChar" --include='*.cpp' --include='*.h' Core Generals GeneralsMD | wc -l
grep -rlI  'L"'       --include='*.cpp' --include='*.h' Core Generals GeneralsMD | wc -l
grep -rlIE '\b(wcslen|wcscpy|wcsncpy|wcscmp|wcsicmp|swprintf|vswprintf|wcstombs|mbstowcs|iswspace|iswalpha|iswdigit|wcschr|wcsstr|towupper|towlower|fgetws|fputws)\b' \
           --include='*.cpp' --include='*.h' Core Generals GeneralsMD | wc -l
```

## Why it is not a typedef change

1. **Literals.** `char16_t` is a distinct type; `L"foo"` is `const wchar_t*` and will not convert.
   All 1301 wide literals become `u"foo"`, and every one of them lives in a file that must be
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
