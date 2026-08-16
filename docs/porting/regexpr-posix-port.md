# `WWLib/regexpr.cpp`: the last macOS compile failure, and why it became POSIX regex

976 of 977 translation units compiled on Apple Silicon
([`first-native-run-arm64.md`](first-native-run-arm64.md)). The holdout was
`Core/Libraries/Source/WWVegas/WWLib/regexpr.cpp`:

```text
error: unknown type name 'reg_syntax_t'
```

`reg_syntax_t`, `re_set_syntax`, `re_compile_pattern`, `re_match` and the `RE_*` syntax bits are
**glibc's own extensions** to `<regex.h>`. They are not in POSIX and not in Apple's libc, which has the
POSIX subset (`regcomp`, `regexec`, `regfree`, `regerror`) only. This is a real API absence, not a
header path or a flag.

## The three options, and the evidence for each

Nothing on the running game's path references this class, which is exactly why the choice had to be
recorded rather than made silently.

| | Option | Evidence |
|---|---|---|
| a | Cut it from the measured set, recording that it is dead on the single-player path | `RegularExpressionClass` has **no consumer anywhere in the tree**: `grep -rn "regexpr\|RegularExpressionClass"` over all sources matches only the file and its own header. It is also already **commented out** of its own build: lines 110-111 of `Core/Libraries/Source/WWVegas/WWLib/CMakeLists.txt` are `#regexpr.cpp` / `#regexpr.h`, so the real CMake build — Windows included — has never compiled it. |
| b | Port it to POSIX `<regex.h>`, present on macOS | The nine syntax bits the class selected (`RE_CHAR_CLASSES`, `RE_CONTEXT_INDEP_ANCHORS`, `RE_CONTEXT_INDEP_OPS`, `RE_CONTEXT_INVALID_OPS`, `RE_INTERVALS`, `RE_NO_BK_BRACES`, `RE_NO_BK_PARENS`, `RE_NO_BK_VBAR`, `RE_NO_EMPTY_RANGES`) are nine of the twelve in glibc's own `RE_SYNTAX_POSIX_EXTENDED`, which is what `regcomp(..., REG_EXTENDED)` selects. The class was already asking for POSIX extended regex through a GNU door. |
| c | Genuinely needed, worth a full port of the GNU semantics | Would mean vendoring glibc's regex engine (LGPL, ~5 kLOC) to preserve three syntax bits for zero callers. Nothing needs them. |

**Chosen: (b).** Option (a) is defensible on the usage evidence alone, but "cut it and record why" leaves
the next platform the same landmine in a file that is still in the tree and still enumerated by the
measurement harness, and (b) costs about forty lines. Option (c) buys nothing for nobody.

Deleting the file was not on the table for this slice: it is a WWLib utility with a header, and removing
a public class from the shared library is a scope decision about the vendored engine, not a macOS
build fix. What (b) does buy is that `native-build.py`'s recursive enumeration — which compiles the file
even though CMake does not — now reaches 977/977 on Darwin for the same reason it does on Linux, so
the harness and the real build stop disagreeing about whether this file is portable.

## What changed, and the three differences it accepts

The vendored `scripts/native-port-shims/gnu_regex.h` is **deleted**: it declared the glibc API to get
this one file through the shimmed probe, and with the GNU calls gone it had no other consumer. That is
also why the *native* (unshimmed) probe improves by one, from 671/760 to 672/760 clean: the file no
longer needs a shim to be parsed at all.

`Compile()` calls `regcomp(&expr, pattern, REG_EXTENDED)`, `Match()` calls `regexec()`, and the
destructor calls `regfree()`. Three syntax bits differ, all of them named in the source so that
"equivalent" is never asserted without its exceptions:

| glibc bit, absent from `REG_EXTENDED` | Before | After |
|---|---|---|
| `RE_DOT_NEWLINE` | `.` did not match a newline | `.` matches a newline |
| `RE_DOT_NOT_NULL` | `.` could match a NUL | `.` does not match a NUL |
| `RE_UNMATCHED_RIGHT_PAREN_ORD` | an unmatched `)` was invalid, so `Compile()` returned false | it is an ordinary character, so `Compile()` returns true |

All three need a pattern or subject containing a newline, an embedded NUL, or an unbalanced `)`.
Classes, intervals, alternation, grouping and anchors are the same language.

One difference does not follow from the syntax bits and would have been silent, so it is handled
explicitly: **`re_match` anchors at the start of the subject** and returns how far it matched, while
`regexec` searches anywhere in it. A bare `regexec` would report `"xxabc"` as matching `abc`. `Match()`
therefore requires `regmatch_t.rm_so == 0`, which is what `re_match` meant, and a zero-length match at
offset 0 stays a match — distinct from no match, as before.

## Status of the fix

**Unverified on macOS.** The port was written and measured on Linux x86-64 with clang 14: the file
compiles in both the native and shimmed probes, the Linux level 1-4 build stays at 977/977 objects with
a clean strict link, and no behaviour changed for any caller because there are none. Whether Darwin
reaches 977/977 has not been observed; the Mac is a separate outpost slice. Behaviour under the
Windows CMake build is unchanged **by construction**, since that build does not compile this file.
