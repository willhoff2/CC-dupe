# The LAN packet at 64 bits

`LANMessage` is the entire LAN broadcast protocol: one `#pragma pack(1)` struct, handed to
`Transport::queueSend()` as `sizeof(LANMessage)` raw bytes and cast back out of the receive buffer at
the other end. Its size and every member offset therefore *are* the wire format, and
`Core/GameEngine/Include/GameNetwork/LANAPI.h` has always asserted that it fits in a packet:

```cpp
static_assert(sizeof(LANMessage) <= MAX_LANAPI_PACKET_SIZE, ...);   // MAX_LANAPI_PACKET_SIZE == 476
```

Under 64-bit clang that assertion failed, and because `LANAPI.h` is included well outside the LAN
code it failed 13 translation units across `Core/GameEngine` and `GeneralsMD/Code/GameEngine`. This
was the single largest compile blocker in the native build.

LAN and GameSpy *online play* remain out of scope for the port; these files still have to compile,
which is the whole reason this is worth doing.

## Measured: what grew

`sizeof(MAX_LANAPI_PACKET_SIZE)`'s value is a constant 476 everywhere. The struct was 471 bytes with
MSVC and 536 bytes at LP64, i.e. 60 bytes over the limit. Read out of clang's own record layout dump
(`-Xclang -fdump-record-layouts`), the *only* members that changed were the Unicode arrays:

| Member | 32-bit MSVC shape | 64-bit clang, pre-port | Delta |
| --- | ---: | ---: | ---: |
| `Type messageType` | 4 @ 0 | 4 @ 0 | — |
| `WideChar name[13]` | **26** @ 4 | **52** @ 4 | **+26** |
| `char userName[2]` | 2 @ 30 | 2 @ 56 | — (offset +26) |
| `char hostName[2]` | 2 @ 32 | 2 @ 58 | — (offset +26) |
| payload union | 437 @ 34 | 476 @ 60 | **+39** |
| `WideChar gameName[17]` (7 arms) | **34** | **68** | **+34** each |
| `WideChar playerName[13]` | **26** | **52** | **+26** |
| `WideChar message[101]` | **202** | **404** | **+202** |
| `char options[401]`, `char serial[23]`, `UnsignedInt`/`Bool`/`Int` fields, both enums | unchanged | unchanged | — |
| **`sizeof(LANMessage)`** | **471** | **536** | **+65** |

So: no pointer, no `size_t`, no `time_t`, no enum and no padding grew. `#pragma pack(1)` means
alignment contributes nothing, and every scalar in the struct is already one of the fixed-width
engine typedefs (`Int`, `UnsignedInt`, `Bool`). The whole 65 bytes are `WideChar`, which is
`typedef wchar_t` — 2 bytes with MSVC, 4 bytes with clang and gcc on macOS and Linux. See
[`widechar-fallout.md`](widechar-fallout.md) for the rest of that iceberg.

One second-order effect worth recording: doubling the arrays also changes *which* union arm is the
largest. At the MSVC width the biggest arm is `GameInfo` (437 bytes), which is what
`m_lanMaxOptionsLength` is derived from; at 4-byte `WideChar` the biggest arm becomes `Chat`
(gameName 68 + chatType 4 + message 404 = 476). A "make the packet bigger" fix would have had to
chase that too.

## The fix: the wire is UTF-16, the engine keeps WideChar

`MAX_LANAPI_PACKET_SIZE` is not raised and the assertion is not weakened. Instead the packet's text
fields have a type of their own, and `WideChar` stops appearing in the wire format at all:

```cpp
typedef UnsignedShort LANWireChar;    // GameNetwork/LANWireString.h
STATIC_ASSERT_ALWAYS(sizeof(LANWireChar) == 2, ...);
```

`LANMessage`'s 11 `WideChar` arrays are `LANWireChar` arrays of the same length, and text crosses the
boundary through two functions:

```cpp
void          lanWireStringSet( LANWireChar *dest, Int destCount, const WideChar *src );
UnicodeString lanWireStringGet( const LANWireChar *src, Int srcCount );
```

This is deliberately the same shape as the fixed-width blocks in
[`xfer-64bit-audit.md`](xfer-64bit-audit.md) and [`raw-blob-audit.md`](raw-blob-audit.md): a
fixed-width representation for the bytes that leave the process, a conversion at the boundary, and
the layout pinned by assertions in the header rather than by a comment.

Because MSVC's `wchar_t` *is* a UTF-16 code unit, `lanWireStringSet()` on Windows copies units
straight through and the resulting bytes are exactly the ones the 32-bit build has always produced.
The 471-byte layout is unchanged, member for member, from the shipped one.

Where `WideChar` is 4 bytes the conversion is a real UTF-16 encode/decode: astral code points become
surrogate pairs on the way out and are recombined on the way in, and anything unrepresentable (a lone
surrogate, a truncated pair at the end of a field) becomes U+FFFD rather than a malformed field.
Truncation cuts on a code-point boundary, never leaving a lone high surrogate.

Two consumer-visible consequences, both called out because they are behaviour and not just types:

- `wcslcpy(msg.field, str, N)` became `lanWireStringSet(msg.field, N, str)` and
  `UnicodeString(msg->field)` became `lanWireStringGet(msg->field, N)` at every LAN send and receive
  site in `LANAPI.cpp` and `LANAPIhandlers.cpp`. There was no way to keep those call sites literally
  unchanged: the field is no longer a `wchar_t` array, and pretending otherwise via a cast is exactly
  the silent-truncation bug this slice exists to prevent.
- `ContainsInvalidChars()` / `ContainsAnyReadableChars()`, the join-time player-name validation, now
  read the *wire* form one 16-bit unit at a time instead of the decoded string.
  `IsInvalidCharForPlayerName()` rejects surrogates, and a decoded name would present an astral
  character as one code point where `WideChar` is 4 bytes and as a surrogate pair where it is 2 — so
  judging the decoded form would have accepted, natively, names that Windows rejects. Judging the
  wire form keeps every target's answer identical to the retail one.

Both enums that appear in the struct (`LANMessage::Type`, `LANAPIInterface::ChatType`,
`LANAPIInterface::ReturnType`) are now `CPP_11(: Int)`, so their width is stated rather than left to
the compiler. This changes nothing on any current toolchain — all three are already 4 bytes — but
these are wire fields and an implementation-defined width in a wire field is a latent bug.

## The gate

Three layers, all committed:

1. **In the header.** `LANMESSAGE_WIRE_SIZE` and 20 `STATIC_ASSERT_ALWAYS`s pinning
   `sizeof(LANMessage)`, the offset of every named field, and the width of each enum.
   `STATIC_ASSERT_ALWAYS` rather than `static_assert` because the pre-C++11 build defines
   `static_assert` away to nothing, and an assertion that does not fire on the oracle toolchain is
   not much of an assertion. The original `<= MAX_LANAPI_PACKET_SIZE` assertion is untouched.
2. **`scripts/ci/check-lanmessage-layout.py`.** Compiles the header in four configurations — 32 and
   64 bit, each with a 2-byte and a 4-byte `wchar_t` (`-m32`, `-fshort-wchar`) — and requires that
   all four compile, and that all four agree on `sizeof(LANMessage)` *and* on the offset of all 46
   members, read from clang's record layout dump rather than from the assertions. It then compiles a
   *poisoned* header that spells the fields the pre-port way and requires that to fail, inside the
   layout assertions: 18 of them fire. Current output:

   ```
   [ ok ] 32-bit, 2 byte wchar_t (the MSVC/VC6 layout): sizeof(LANMessage) = 471
   [ ok ] 32-bit, 4 byte wchar_t: sizeof(LANMessage) = 471
   [ ok ] 64-bit, 4 byte wchar_t (native macOS/Linux): sizeof(LANMessage) = 471
   [ ok ] 64-bit, 2 byte wchar_t: sizeof(LANMessage) = 471
   [ ok ] all 4 configurations agree on the layout, 46 members
   [ ok ] negative control fails in 18 layout assertions
   ```

3. **`native-port-ci.yml`.** A `lanmessage-layout` job runs the above on every push and PR. It needs
   `gcc-multilib` for the 32-bit half; it is a separate job from the probe so that installing that
   cannot perturb the probe's measured counts.

## Measured: the drop

Same tree, same container, `clang++-14`, before and after this change (`scripts/native-port-probe.py`
and `scripts/native-build.py`, JSON in `docs/porting/ci-baselines/`):

| | before | after |
| --- | ---: | ---: |
| probe clean TUs, native | 621 / 742 | **634 / 743** |
| probe clean TUs, shimmed | 651 / 742 | **664 / 743** |
| native build objects | 676 / 717 | **679 / 718** |
| unresolved symbols after link | 378 | **376** |

The totals go up by one because `LANWireString.cpp` is a new translation unit; it is clean, so the
real gain is **+12 previously-failing translation units** on both probe modes, and +2 in the
(narrower) levels 1-2 native build. The 13 that were failing on this assertion:

| Translation unit | now |
| --- | --- |
| `Core/GameEngine/Source/GameNetwork/LANAPI.cpp` | clean |
| `Core/GameEngine/Source/GameNetwork/LANAPICallbacks.cpp` | clean |
| `Core/GameEngine/Source/GameNetwork/LANAPIhandlers.cpp` | clean |
| `Core/GameEngine/Source/GameNetwork/LANGameInfo.cpp` | clean |
| `Core/GameEngine/Source/GameNetwork/GameInfo.cpp` | clean |
| `Core/GameEngine/Source/GameNetwork/ConnectionManager.cpp` | clean |
| `GeneralsMD/.../Common/StatsCollector.cpp` | clean |
| `GeneralsMD/.../GameNetwork/GUIUtil.cpp` | clean |
| `GeneralsMD/.../Menus/GameInfoWindow.cpp` | clean |
| `GeneralsMD/.../Menus/LanGameOptionsMenu.cpp` | clean |
| `GeneralsMD/.../Menus/LanLobbyMenu.cpp` | clean |
| `GeneralsMD/.../Menus/LanMapSelectMenu.cpp` | clean |
| `GeneralsMD/.../Menus/SkirmishMapSelectMenu.cpp` | clean |

`GameLogic.cpp` and `ScoreScreen.cpp` were named in the slice brief as casualties of this assertion;
they are not — they fail on `'gscommon.h' file not found` (GameSpy) and still do. `Recorder.cpp` was
one of its casualties, but it was failing on GameSpy headers as well, so it is not in the list above:
this change removed one of its two diagnostics, not both.

## Compatibility, plainly

- **Retail 1.04 LAN wire compatibility: unchanged for the 32-bit Windows build, and it was already
  impossible for a 64-bit build.** On the MSVC width the bytes are identical to the shipped ones, so
  a Windows build of this tree still talks to any other build of this tree, and the layout is the
  retail one. Where `wchar_t` is 4 bytes there was no previous behaviour to break — the file did not
  compile.
- **What did change is text handling at the boundary, not layout.** A player name or chat message
  containing characters outside the BMP is carried as surrogate pairs, so it survives a round trip
  between a Windows peer and a native peer; the same text used to be untransportable because the
  native side did not build. Unrepresentable input degrades to U+FFFD instead of being copied
  verbatim.
- **Replays and save games are untouched.** `LANMessage` is not part of either.
- Retail replay/save compatibility remains out of scope for the port generally; this slice did not
  need to spend any of it.

## Still open

- The other rows of [`widechar-fallout.md`](widechar-fallout.md) — `.csf`, `.map`, `Xfer`, the
  game-state CRC, `NetPacketStructs` — are untouched, and unlike `LANMessage` they have no assertion
  guarding them. `NetPacketStructs.h` is the closest relative of this work and the obvious next
  slice: it also puts wide strings on the wire, but through explicit serialisation rather than a
  blob, so it mis-parses rather than failing to build.
- The LAN code compiles; it is not exercised. Nothing here was run over a socket on any platform, and
  LAN play is not on the single-player port's path. The encode/decode functions are covered by the
  layout gate only in the sense that the layout they write into is pinned — their *text* behaviour
  has no test, because the repo has no unit-test harness for `Core/GameEngine`.
- **Written blind:** nothing in this slice is macOS- or arm64-specific, and every configuration in
  the gate was executed on Linux with clang 14. The macOS claim in this document is the LP64 4-byte
  `wchar_t` layout, which is what `-m64` without `-fshort-wchar` measures; the same numbers on Apple
  clang on arm64 were not observed.
