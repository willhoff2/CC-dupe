# Wide text at the process boundary: 16 bits on disk, 16 bits on the wire

`Core/Libraries/Include/Lib/BaseType.h` says

```cpp
typedef wchar_t WideChar;
```

and that stays. `wchar_t` is 2 bytes with MSVC and 4 bytes on macOS and Linux, so **the in-memory
type follows the platform**: `wcslen`, `swprintf`, `std::wstring` and every OS API that takes a
`wchar_t*` keep working, and none of the ~1300 `L"…"` literals or 219 wide libc call sites move.

What does *not* follow the platform is anything that leaves the process. Every external format the
game has — `.csf` string tables, `.map` chunks, save games, replays, the game-state CRC, network
packets — stores **little-endian UTF-16 code units of a fixed 16 bit width**, because that is what
the Windows build wrote. Code that sized those units with `sizeof(WideChar)` was correct only by
coincidence: on MSVC `sizeof(wchar_t)` is 2 and *is* the unit size.

This document is the crossing register for that boundary and the record of what this slice proved.

## The failure that started it

The first native Apple Silicon run (#87) came up with every localized string missing and no error.
`GameTextManager::init()` read a `.csf` record as

```cpp
file->read( m_tbuffer, len*sizeof(WideChar) );
```

with `len` a count of **code units**. At `sizeof(WideChar) == 4` a 12-unit label consumed 48 bytes
where 24 exist, so the read walked into the next record; the next "record id" the parser saw was
`1919252833`, which is the ASCII bytes `ayer` — the tail of `GUI:SinglePlayer`. The parse then ran on
against garbage. `init()` has no way to report failure, which is why it was silent (that silence is
another slice's problem; the desynchronisation was this one's).

## The seam

`Core/GameEngine/Include/Common/WideCharWire.h`, a header-only conversion:

```cpp
typedef UnsignedShort WideWireChar;              // one external UTF-16 code unit, 2 bytes everywhere

Int wideCharWireBytes( Int units );              // never sizeof(WideChar)
Int wideCharWireUnitCount( const WideChar *src, Int srcCount );
Int wideCharToWire( WideWireChar *dest, Int destUnits, const WideChar *src, Int srcCount );
Int wireToWideChar( WideChar *dest, Int destCount, const WideWireChar *src, Int srcUnits );
```

`srcCount` is a `WideChar` count or `WIDECHAR_WIRE_NUL_TERMINATED`. Neither converter terminates its
destination: the formats disagree about whether text is terminated, so the caller writes the
terminator it needs.

This is the `LANWireChar` seam of [`lanmessage-64bit.md`](lanmessage-64bit.md) generalised rather
than a second pattern. `LANWireChar` **is** `WideWireChar` now, and `lanWireStringSet`/`Get` are thin
LAN-shaped wrappers (fixed-size field, always terminated, `UnicodeString` out) over these four
functions, so there is one conversion in the tree, not two. `LANAPI.h` is untouched and
`check-lanmessage-layout.py` is still green, including its negative control.

### What it does with values that have no representation

| Input | Native (4 byte `wchar_t`) | MSVC (2 byte `wchar_t`) |
| --- | --- | --- |
| BMP code point | one unit | one unit, copied |
| code point > U+FFFF | encoded as a surrogate pair, two units | already a pair in the source, copied |
| lone low surrogate | U+FFFD | U+FFFD |
| lone high surrogate | U+FFFD | ends the text (a pair cannot be completed) |
| value > U+10FFFF | U+FFFD | not representable in a `wchar_t` |
| a pair that does not fit the destination | dropped whole, never half | dropped whole, never half |

Reading back: a pair is recombined into one `WideChar` only where `WideChar` is wide enough to hold
the code point. On Windows it is not, so the pair stays a pair — which is what its wide API wants.
A malformed pair on the way in becomes U+FFFD.

**No conversion truncates silently at a unit boundary, and nothing assumes ASCII.**

### Why the Windows bytes cannot move

With a 2 byte `wchar_t`, `WideChar` already *is* a UTF-16 code unit, so both converters degenerate to
a copy and `wideCharWireBytes(n) == n * sizeof(WideChar)`. That is asserted, not asserted-by-eye:
`scripts/ci/check-widechar-wire.py` compiles the real header twice, once at each width, and requires

* `memcmp(wire, text, bytes) == 0` at 2 bytes, in both directions;
* the `.csf` bytes, the save blob and the wire bytes to be **byte-identical between the two runs**.

## The crossing register

Enumerated three ways, because `grep sizeof(WideChar)` finds only the sites that name the width:

```bash
grep -rnIE 'sizeof\s*\(\s*(WideChar|wchar_t)\s*\)' --include='*.cpp' --include='*.h' Core Generals GeneralsMD
grep -rnIE '(read|write|fread|fwrite|memcpy|xferUser|xferImplementation)\s*\(.*(WideChar|wchar_t|UnicodeString)' \
     --include='*.cpp' --include='*.h' Core/GameEngine Generals/Code/GameEngine GeneralsMD/Code/GameEngine
for f in $(grep -rlI 'pragma pack' --include='*.h' Core Generals GeneralsMD); do \
    grep -qE '\b(WideChar|wchar_t)\b' $f && echo $f; done
python3 scripts/xfer-blob-audit.py; python3 scripts/native-layout-test.py
```

### Fixed in this slice

| Crossing | Format | External representation | What it was |
| --- | --- | --- | --- |
| `GameText.cpp` `.csf`/`.str` reader | file, read | `UnsignedShort` unit count, then that many bit-inverted units | `len*sizeof(WideChar)` bytes, i.e. double. **The observed failure.** Units are now un-inverted *as units*, before decoding, so the obfuscation still applies to what the file holds |
| `Xfer::xferUnicodeString` | save/replay/CRC dispatch | the units of the string | `sizeof(WideChar) * getLength()` raw bytes |
| `XferSave::xferUnicodeString` | save game, write | `UnsignedByte` unit count, then the units | the count was a `WideChar` count and the payload double-width |
| `XferLoad::xferUnicodeString` | save game, read | as above | as above |
| `XferDeepCRC::xferUnicodeString` | CRC input | as above | as above — a 4 byte `WideChar` changed the *hash*, which is the lock-step desync check |
| `DataChunkOutput::writeUnicodeString`, `DataChunkInput::readUnicodeString` (both games) | `.map` chunk | `UnsignedShort` unit count, then the units | `len*sizeof(WideChar)`, and `decrementDataLeft` was wrong by the same factor, so the chunk bookkeeping desynchronised too |
| `LanguageFilter` bad-word list | file, read one unit at a time | units, each XORed with `0x5555`, space separated | read `sizeof(WideChar)` per unit. The XOR is now applied to units *before* decoding — a surrogate pair means nothing while the obfuscation is on |
| `LocalFile::readWideChar` | file, read | one unit; returns that unit | read `sizeof(WideChar)` |
| `LocalFile::writeFormat(const WideChar*, …)` | file, write | the units of the formatted text | formatted natively (correct) then wrote `length*sizeof(WideChar)` bytes of `wchar_t` |
| `LocalFile::writeChar(const WideChar*)` | file, write | one or two units | wrote `sizeof(WideChar)` bytes; also returned the *pointer* cast to `Int`, which is lossy at 64 bits |
| `RecorderClass::writeArgument`/`readArgument`, `ARGUMENTDATATYPE_WIDECHAR` (both games) | replay, read+write | one unit | `sizeof(arg.wChar)`, i.e. 4 bytes natively |
| `RecorderClass::readUnicodeString` (both games) | replay, read | NUL-terminated units | collected `readWideChar()` results as `WideChar` and stopped at a unit boundary |
| `network::readStringWithoutNull`/`writeStringWithoutNull` | network packet | `UnsignedByte` unit count, then the units | `src.size()/sizeof(WideChar)` and `copyLen*sizeof(WideChar)`; the packet's length byte now counts units, and the writer pads to the reserved size so `getSize()` and `copyBytes()` cannot disagree |
| `NetPacketChatCommandData`, `NetPacketDisconnectChatCommandData` | network packet | as above | sized with `sizeof(WideChar)` |
| `NetPacketGameCommandData` wide-char argument | network packet | one unit | `arg->getArgCount() * sizeof(WideChar)` |

### Crossings examined and deliberately left

| Where | Why |
| --- | --- |
| `LANAPI.h` / `LANMessage` | already fixed by `wire/lanmessage-64bit`; its wire type is now the shared `WideWireChar` and its layout test still passes at four target/width combinations |
| `RAMFile::readWideChar`/`writeWideChar`/`printfWideChar` | not implemented — they return `WEOF`/`-1`. Nothing wide crosses a `RAMFile`, so there is no width to get wrong; if they are ever implemented they must go through this seam |
| `Core/Tools/Autorun/GameText.cpp:887` | a *copy* of the `.csf` reader in the Windows-only Autorun tool, which is not built natively and is not on the single-player path. Recorded here so it is not mistaken for a missed site: it has the same bug and will need the same fix if that tool is ever ported |
| `GameSpy/BuddyThread.h` `WideChar text[…]` members | in-process queues between the GameSpy threads, never written to a socket as raw bytes; the online path is cut scope (`online/gamespy-path-excision`) |
| `MiniDumper.h` `WideChar m_executablePath[MAX_PATH]` | argument to a Win32 W API on Windows only; in-memory, so it stays `WideChar` |
| `UnicodeString::getByteCount()` | returns `getLength() * sizeof(WideChar)`. It has **no wide callers** (the two call sites are `AsciiString`), so it is not a crossing — but it is a trap. Any future caller that means "bytes outside the process" wants `wideCharWireBytes(wideCharWireUnitCount(…))` |
| `vswprintf(buf, sizeof(buf)/sizeof(WideChar), …)` in `InGameUI.cpp`, `UnicodeString.cpp`, `LocalFile.cpp` | an *element count* for a native buffer, which is exactly what `sizeof(buf)/sizeof(WideChar)` computes. In-memory; correct as written |
| `UnicodeString.cpp` allocation arithmetic, `WWLib/trim.cpp` `memmove` | in-memory buffers of `WideChar`; correct at any width |
| Keyboard `WideChar stdKey/shifted` | in-memory key tables |

`scripts/xfer-blob-audit.py` reports **no** wide-typed raw-block site left in the save/replay/CRC
implementation, and `scripts/native-layout-test.py` (including its poisoned negative control) is
green.

## Length semantics, stated once

Every external count in the formats above is a **count of 16 bit code units**, never of `WideChar`.
That was already true of the bytes on disk; the code now says so. The consequence is that a save
game or a chat packet holding text with astral code points fits *fewer characters* than before — 255
units, so between 127 and 255 characters — rather than overflowing the field. `XferSave` still
refuses (`XFER_STRING_ERROR`) above 255 units.

The staging buffers the conversions encode through are `WIDECHAR_WIRE_MAX_UNITS` (1024) units, which
is twice the 510 units the 255-unit save/CRC/packet fields can hold. `Xfer::xferUnicodeString` is
the one crossing with no field limit of its own, so it asserts (`DEBUG_ASSERTCRASH`) rather than
truncating quietly if a string ever exceeds that.

## What was verified

Ladder run on this branch, `clang++-14`, Ubuntu 22.04 x86-64:

* `scripts/ci/check-widechar-wire.py` — compiles and **runs** the real header at both `wchar_t`
  widths. All cases pass at both; the external bytes are identical between them; the negative
  control (the old `sizeof(WideChar)` sizing) desynchronises the `.csf` reader by **24 bytes** at 4
  bytes and is correct at 2 — the same 24 bytes the first native run lost.
* `scripts/ci/check-lanmessage-layout.py` — 471 bytes in all four configurations, negative control
  still fails in 18 assertions, after `LANWireChar` became `WideWireChar`.
* `scripts/native-layout-test.py` — LP64 and ILP32 layouts, poisoned control fails.
* `scripts/native-build.py --level 1..4 --with-shims --strict-link` — the link ratchet still
  produces a binary; see the report in the PR.
* `scripts/xfer-blob-audit.py`, `scripts/porting-status.py`,
  `scripts/ci/check-generated-baselines.py`, `scripts/ci/check-probe-baseline.py`.

The two expensive Windows gates were both run on the PR head (#88):

* The Wine/VC6 Zero Hour build (`scripts/docker-build.sh --game zh`) completed all 1361 targets and
  produced `generalszh.exe`, and CI's `Build GeneralsMD`/`Build Generals` matrices are green in all
  of `vc6`, `vc6-debug`, `vc6-profile`, `vc6-releaselog`, `win32`, `win32-debug` and `win32-profile`.
* The retail replay determinism gate passed: `Replay Check GeneralsMD / vc6+t+e` and
  `vc6-releaselog+t+e` both green over the ten `GeneralsReplays/` replays. This is the check that
  would catch an `XferCRC` regression, since the CRC feeds the lock-step desync check.

## What was NOT verified

* **Retail binary compatibility is out of scope and unproven** (and, for save games, was already
  out of scope): a save written by this build is not claimed to load in the retail game. What *is*
  claimed and tested is self-consistency — this build reads what it writes — and cross-width
  agreement, i.e. the Windows and native builds produce the same external bytes for the same text.
* No test exercises a real `.csf`, `.map` or save game *file* end to end — the tests drive the seam
  and reconstruct the record shapes. Real replay files are covered by the gate above; `.csf`, `.map`
  and save-game files are not, beyond whatever the replay run happens to read.
* Astral code points are handled deliberately, not exercised by the game: nothing in the shipped
  data or the input path is known to produce one. The behaviour is defined so that it cannot corrupt
  a stream, not because it is expected.
