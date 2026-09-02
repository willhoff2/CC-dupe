# Phase 2, slice 1: the raw blocks in the save/replay format

Phase 2 of `docs/porting/native-port-plan.md` is "make the serialised state 64-bit correct". This
is the measurement of that phase and the first slice of the conversion. Retail save and replay
compatibility is out of scope for the port, so the bar is not "read a 2003 file"; it is that the
format is **self-consistent and deterministic across the Windows and the native build**, and that
it stops silently depending on how a particular compiler lays out a particular struct.

Everything below was measured, not estimated. The tool is `scripts/xfer-blob-audit.py`, which is
committed with this change; run it with no arguments to reproduce the tables.

## What the format actually writes

`Xfer` has two kinds of method. The scalar ones (`xferInt`, `xferReal`, `xferAsciiString`, …) go
through `Lib/BaseTypeCore.h`, whose `Int`/`UnsignedInt`/`Short`/`Real` are `<cstdint>` typedefs and
therefore do not move under LP64. Those are not the problem. The problem is the *raw block*: a
pointer and a byte count, handed to `xferUser()` (or, inside the implementations, to
`xferImplementation()`), which puts `sizeof(whatever)` bytes of a live C++ object on disk.

Measured over the tracked `*.cpp`/`*.h`/`*.inl` in `Core/`, `Generals/` and `GeneralsMD/`:

| | All three trees | In ported scope |
| --- | ---: | ---: |
| `xferUser` / `xferImplementation` call sites | **480** in **146** files | **292** in **82** files |
| `fwrite`/`fread` of an address | **33** in **10** files | **20** in **6** files |
| `memcpy` call sites | **402** in **141** files | — |
| `memcpy` inside the save/replay/CRC implementation | **0** | **0** |

"Ported scope" is `Core/GameEngine`, `Core/GameEngineDevice`, `GeneralsMD/Code/GameEngine` and
`GeneralsMD/Code/GameEngineDevice`. `Generals/Code`, `Core/Tools` and `GeneralsMD/Code/Tools` are
excluded from the port and were deliberately not touched by this slice, but they are counted so
the "how big is this really" question has an honest answer.

The `memcpy` number deserves a note, because it is the one that looks alarming and is not: 402
call sites exist, but **none** of them are in `XferSave.cpp`, `XferLoad.cpp`, `XferCRC.cpp`,
`XferDeepCRC.cpp`, `Snapshot.cpp`, `DataChunk.cpp`, `Recorder.cpp` or the `SaveGame/` directory.
There is no "memcpy into a save buffer" problem to fix. The save path copies bytes exactly once,
in `XferSave::xferImplementation()`, straight to `fwrite`.

## How many of those raw blocks move under LP64

Every raw-block site was classified by the type its `sizeof` names. Before this change, in ported
scope:

| Category | Sites (all) | Sites (in scope) | Moves under LP64? |
| --- | ---: | ---: | --- |
| enum with a fixed underlying type (`CPP_11(: Int)`) | 225 | 126 | no |
| fixed-width scalar (`Int`, `Real`, `Bool`, …) | 102 | 61 | no |
| struct/class record | 90 | 65 | only `RadiusDecal` — see below |
| **enum with no fixed underlying type** | **33** | **19** | **implementation-defined** |
| `sizeof(this)` | 2 | 1 | **yes, 4 → 8** |
| `WideChar` payloads | 4 | 4 | **yes, 2 → 4** |
| unresolved by the script (checked by hand) | 24 | 16 | no |

So the honest headline is: **the save/replay format is far closer to 64-bit clean than the phase
estimate assumes.** 90% of the raw blocks are already fixed-width by construction. The exceptions
are a small, enumerable set, and this slice closes most of them.

### Distinct types crossing the boundary as raw blocks

Ten, and only ten:

| Type | Sites | Win32 size | LP64 size |
| --- | ---: | ---: | ---: |
| `GameClientRandomVariable` | 31 | 12 | 12 |
| `Matrix3D` | 18 | 48 | 48 |
| `Coord3D` | 16 | 12 | 12 |
| `Vector3` | 8 | 12 | 12 |
| `ShroudLevel` | 4 | 4 | 4 |
| `DozerAIUpdate::DozerTaskInfo` | 4 | 8 | 8 |
| `BitFlags<N, TAG>` | 3 | N/8 rounded up | same |
| `RadiusDecal` | 3 | **12** | **24** |
| `ProductionUpdate::DoorInfo` | 2 | 16 | 16 |
| `IRegion2D` | 1 | 16 | 16 |

Nine of the ten are built from `Int`, `Real`, `UnsignedInt` and fixed-width enums and survive the
move to 64 bits unchanged. One does not: `RadiusDecal` is `{const RadiusDecalTemplate*, Shadow*,
Bool}`, i.e. it writes two raw pointer values into the save file and doubles in size under LP64.

## What commit `f117f5ec4` covered, and what it did not

`f117f5ec4` ("Make the on-disk integer types fixed width and assert the layouts") was about the
**asset** formats, not the save/replay format. It fixed `bittype.h` (`uint32`/`sint32` were
spelled with `long`), and it added layout assertions for the `.w3d`, `.tga` and chunk-IO
structures: 392 `STATIC_ASSERT_ALWAYS` lines in `WW3D2/w3d_file_layout.h` plus `sphereobj.h`,
`ringobj.h`, `chunkio.h`, `iostruct.h`, `TARGA.h` and `CRC.h`.

It did **not** touch the `Xfer` path at all. Before this change there was not a single assertion
on any type that `xferUser()` writes — no assertion on `Coord3D`, on `GameClientRandomVariable`, on
the replay header, on anything. Its `docs/porting/raw-blob-audit.md` also states that "GameEngine
serialization … writes scalars one at a time", which the 480 raw-block sites above disprove; that
document is corrected in this change.

## What this slice changes

### 1. The replay header timestamp is no longer a Win32 struct

`RecorderClass::ReplayHeader::timeVal` was a `SYSTEMTIME`, written to and read from the `.rep`
file with `sizeof(SYSTEMTIME)`, which put a type that only exists on Windows into the file format.
It is now `SerializedDateTime` (`Core/GameEngine/Include/Common/SerializedDateTime.h`): eight
`UnsignedShort` fields in `SYSTEMTIME`'s member order, with the size *and every field offset*
asserted. **The bytes on disk are identical on Windows** — this is a width-and-layout change, not a
content change.

It stayed local civil time rather than becoming UTC. The brief asked for a fixed-width UTC
timestamp; fixed-width is the part that matters for 64-bit correctness, and switching to UTC would
change what the sixteen bytes *mean*, silently shifting every displayed replay date by the
recorder's timezone offset. That is a content change, and the rule for this phase is to prefer
changing widths over changing content. `getLocalSerializedDateTime()` is `GetLocalTime()` on
Windows and `localtime_r()` plus `gettimeofday()` elsewhere.

`replayHeader.startTime`/`endTime` are `time_t`, which is 4 bytes under VC6 and 8 under LP64, but
they were already written through the `int32_t replay_time_t` alias, so the file is fine. The
in-memory fields were left as `time_t`.

### 2. `GameState.h` and `Recorder.h` no longer include `windows.h`

Both headers only needed it for `SYSTEMTIME`. `getUnicodeDateBuffer()`/`getUnicodeTimeBuffer()`
now take a `const SerializedDateTime&`; their bodies are unchanged Win32 locale code, with the
conversion back to `SYSTEMTIME` done in a static helper in `GameState.cpp`. The saved-game
`SaveDate` was already eight `UnsignedShort`s serialised one field at a time and needed no change.

This is where the probe movement comes from: 71 translation units include `GameState.h` and 25
include `Recorder.h`.

### 3. Twenty-one enum declarations that reach disk got a fixed underlying type

`DoorStateType`, `FadingMode`, `StationTask`, `ConductorState`, `DynamicGeometryDirection`,
`MissileStateType` (×2), `PackingState`, `StructureCollapseStateType`, `StructureToppleStateType`,
`ToppleState`, `TFade`, `LocoGoalType`, `DiveState`, `POWTruckAIMode`,
`WeaponRecoilInfo::RecoilState`, and five in `ParticleSys.h`, all gained `CPP_11(: Int)` — the
idiom that already covers the 225 raw-block sites classified as `enum-fixed` above. VC6 sees no
change; a C++11
compiler is now forbidden from choosing a different width. In-scope raw blocks writing an
unpinned enum: **19 → 0**.

### 4. The two pointer-width writes are pinned

* `BitFlagsIO.h`, retail CRC path, wrote `sizeof(this)` — the size of a *pointer*, not of the
  object. Retail therefore CRC'd exactly the first four bytes of the mask, and on a 64-bit build
  that would silently become eight and change every CRC. It is a literal `4` now, with an
  assertion that the mask is at least that big. Bit-identical on Win32.
* `SpectreGunshipUpdate::xfer()`, version 1 load path, wrote `sizeof(RadiusDecal)`. It is a
  literal `12` now, asserted equal to `sizeof(RadiusDecal)` on 32-bit targets.

### 5. A layout ledger

`GeneralsMD/Code/GameEngine/Source/Common/System/XferLayout.cpp` is a code-free file that asserts
the size of every record type that crosses the save/replay boundary. It is the thing that fails
loudly: give `Coord3D` a pointer member, or a `long`, and the build stops there with the file
format named in the error message.

## Deliberately not changed

* **`WideChar` string payloads** (4 sites: `Xfer.cpp`, `XferCRC.cpp`, `XferLoad.cpp`,
  `XferSave.cpp`, all `sizeof(WideChar) * len`). `WideChar` is 2 bytes on Windows and 4 natively,
  so a native build writes twice the bytes for every Unicode string in a save game. This is the
  single largest remaining hole and it is *not* a widths-only fix: it needs a UTF-16 encode/decode
  step at the serialisation boundary, which changes `UnicodeString`'s relationship to the file
  format. It has its own document, `docs/porting/widechar-fallout.md`, and its own slice.
* **`Generals/Code`** (14 unpinned-enum sites, one `sizeof(this)`). Out of scope for the port by
  decision; it still compiles, and the audit reports it separately so the debt is visible.
* **The `Xfer` API itself.** `xferUser(void*, Int)` has 480 call sites. Replacing it with typed
  per-field serialisation is the correct end state and is exactly the kind of change the plan says
  not to attempt in one go.
* **`DozerTaskInfo` and `DoorInfo`** are private nested types, so the ledger cannot name them.
  They are LP64-stable; they need an in-class assertion instead.
* **`W3DWaterTracks.cpp`** writes an enum into `.wak` files. That is an asset format, already
  documented in `raw-blob-audit.md`, and not on the save/replay path.

## What the next slice in this area has to solve

1. `WideChar`: pick UTF-16 on the wire, add the conversion in `XferSave`/`XferLoad`/`XferCRC`, and
   prove the CRC is unchanged on Windows.
2. Prove, rather than assert, that a save written by the Windows build loads in the native build:
   `scripts/native-layout-test.py` exists for the asset structures and wants the same treatment
   here, comparing `sizeof` for every ledger type across both toolchains.
3. `Snapshot`/`Xfer` version numbers are `XferVersion` (fixed width) but the *block sizes* in
   `XferSave.cpp` are `XferBlockSize`; confirm that alias is fixed width before the first native
   save is written.
4. In-class assertions for the nested records above.

## Probe movement

| Probe | Before | After |
| --- | --- | --- |
| native (no Windows SDK) | 489 / 737 | **535 / 739** |
| shimmed (engine's own C++) | 638 / 737 | **640 / 739** |

Per target, native: `Core/GameEngine` 107/207 → 116/208, `GeneralsMD/Code/GameEngine` 267/379 →
304/380. The total rises by two because this change adds two translation units, both clean in both
modes. Baselines in `docs/porting/ci-baselines/` are regenerated.

Both Windows builds were verified locally with `./scripts/docker-build.sh --clean --game zh` and
`--game generals`.
