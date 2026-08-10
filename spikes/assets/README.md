# `zh-asset-inspect` — native asset parsing spike

A standalone 64-bit tool that reads **retail** Zero Hour data files with the engine's own parsing
code, natively, off Windows. It exists to replace inference with measurement: until this spike,
every portability claim in `docs/porting/` had been made against source alone.

It ships no game data, bundles no assets, and hardcodes no path. Every command takes the location
of your own installed data as an argument.

Results measured on an Apple M1 Pro: [`docs/porting/native-asset-parsing-findings.md`](../../docs/porting/native-asset-parsing-findings.md).

## What is engine code and what is not

| Part | Source |
|---|---|
| `.w3d` chunk walking | **the engine's own `Core/Libraries/Source/WWVegas/WWLib/chunkio.cpp`**, compiled into this tool unmodified |
| `FileClass` the loader reads through | `src/posix_file.cpp`, a ~90-line POSIX implementation of the engine's abstract `FileClass` |
| W3D chunk id names | generated at build time from the engine's `w3d_file.h` by `tools/gen_chunk_names.py`; never hand-copied |
| `.big` table of contents | `src/big_archive.cpp`, a transcription of `StdBIGFileSystem::openArchiveFile` — same fields, widths and byte swaps, but not the class itself, which cannot be linked without `AsciiString`, `GameMemory` and the whole `ArchiveFileSystem` hierarchy |
| `.dds` header | `src/dds_inspect.cpp`, written against the DDS format; the engine's `.dds` reader lives behind D3D8 |

So the `.w3d` result tests the real port; the `.big` and `.dds` results test the format
understanding, not engine code.

## Build

```sh
cmake -S spikes/assets -B build/assets -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/assets
```

It takes its integer widths from the tree it is built in, so it doubles as a check on them. On a
tree where `WWLib/bittype.h` still spells `uint32` as `unsigned long` — 64 bits under LP64 —
`ChunkHeader` compiles to 16 bytes, `sizes` says so, and no `.w3d` parses. Build it against `main`
and against the fixed-width branch to see both.

## Use

```sh
Z=/path/to/your/zero-hour-install
S=$(mktemp -d)

build/assets/zh-asset-inspect sizes                 # sizeof() the on-disk structures; must say 8
build/assets/zh-asset-inspect inventory "$Z"        # every .big archive, validated
build/assets/zh-asset-inspect big "$Z/W3DZH.big" --list --limit 20
build/assets/zh-asset-inspect extract "$Z/W3DZH.big" ABBarracks_AC.W3D "$S/model.w3d"
build/assets/zh-asset-inspect w3d "$S/model.w3d"    # chunk tree
build/assets/zh-asset-inspect sweep "$Z/W3DZH.big" "$S" .w3d   # parse all 4,429 models
build/assets/zh-asset-inspect sweep "$Z/TexturesZH.big" "$S" .dds
```

Every command exits non-zero if any cross-check fails, so it works as a regression test once a
data directory is available.

## What "it parsed" has to mean

A parser that reads plausible-looking garbage is the failure mode worth guarding against, so
nothing is reported as a success until it survives independent arithmetic:

* **`.big`** — the header's entry count must equal the entries actually read; the header's size
  field must equal the file size on disk; no entry may end past EOF or start inside the header;
  no two entries may overlap.
* **`.w3d`** — the sum of `(payload + 8)` over the top-level chunks must equal the file size
  exactly; every container chunk must be filled exactly by its children; every chunk id must be
  in the `W3D_CHUNK_*` enum from the engine's own header.
* **`.dds`** — the payload must be exactly the size that the declared dimensions, mip count and
  block format imply, and `dwPitchOrLinearSize` must match the top mip.
