# Native asset parsing on Apple Silicon — measured against retail data

First time this project has touched real game data. Everything below was produced by running
`spikes/assets/zh-asset-inspect` against a retail Zero Hour install on an Apple M1 Pro. Where a
claim could not be verified, it says so.

## Environment

| | |
|---|---|
| Machine | Apple M1 Pro, macOS 26.6.1 (build 25G76), arm64 |
| Compiler | Apple clang 16.0.0, target `arm64-apple-darwin25.6.0`, `-std=c++20 -m64` |
| Tree | `main`, and the fixed-width-types branch (PR #10) that this branch is stacked on |
| Data | Steam Windows depot of Zero Hour, 20 `.big` archives, ~1.0 GB |

The game data is **not** in the repository and nothing derived from it is committed. The tool
takes the data directory as an argument.

## 1. `sizeof` of the on-disk structures, as actually compiled on arm64

Measured with `zh-asset-inspect sizes`, not read off a header.

| Type | Header | Bytes on `main` | Bytes with PR #10's typedefs | On-disk requirement |
|---|---|---:|---:|---:|
| `ChunkHeader` | `WWLib/chunkio.h` | **16** | **8** | **8** |
| `MicroChunkHeader` | `WWLib/chunkio.h` | 2 | 2 | 2 |
| `IOVector2Struct` | `WWLib/iostruct.h` | 8 | 8 | 8 |
| `IOVector3Struct` | `WWLib/iostruct.h` | 12 | 12 | 12 |
| `IOVector4Struct` | `WWLib/iostruct.h` | 16 | 16 | 16 |
| `IOQuaternionStruct` | `WWLib/iostruct.h` | 16 | 16 | 16 |
| `uint32` | `WWLib/bittype.h` | **8** | 4 | 4 |
| `sint32` | `WWLib/bittype.h` | **8** | 4 | 4 |
| `DWORD` | `WWLib/bittype.h` | **8** | 4 | 4 |
| `Int` | `Lib/BaseTypeCore.h` | 4 | 4 | 4 |
| `UnsignedInt` | `Lib/BaseTypeCore.h` | 4 | 4 | 4 |

Two typedef systems coexist and only one of them is broken. `Lib/BaseTypeCore.h` (the GameEngine
side) already uses `<stdint.h>` and is correct as-is. `WWLib/bittype.h` (the WWVegas side, which
owns every W3D file format structure) uses `unsigned long`, which is 32 bits under Windows' LLP64
and 64 bits under LP64. The vector and quaternion structs survive because they are `float`
members; anything with a 32-bit integer field does not.

This confirms the review document's §1.2 prediction — and it is the first time the consequence has
been observed on a real file rather than argued from the type system.

## 2. The `.big` archives parse, and every cross-check passes

`zh-asset-inspect inventory <data-dir>`, 20 archives, no failures:

| Archive | Bytes | Entries | Σ entry sizes |
|---|---:|---:|---:|
| AudioEnglishZH.big | 58,326,522 | 794 | 58,289,036 |
| AudioZH.big | 23,965,542 | 287 | 23,954,022 |
| EnglishZH.big | 80,472,928 | 75 | 80,468,356 |
| gensecZH.big | 787,464 | 1 | 787,416 |
| INIZH.big | 18,764,687 | 135 | 18,758,893 |
| MapsZH.big | 39,749,312 | 299 | 39,736,080 |
| Music.big | 786,724 | 1 | 786,676 |
| MusicZH.big | 34,741,916 | 8 | 34,741,594 |
| PatchData.big | 127 | 1 | 87 |
| PatchINI.big | 53,717 | 1 | 53,661 |
| PatchWindow.big | 260,789 | 2 | 260,694 |
| PatchZH.big | 118,822 | 3 | 118,722 |
| ShadersZH.big | 996 | 8 | 728 |
| SpeechEnglishZH.big | 254,275,694 | 2,430 | 254,156,818 |
| SpeechZH.big | 6,174,552 | 12 | 6,174,042 |
| TerrainZH.big | 8,660,432 | 149 | 8,655,260 |
| TexturesZH.big | 222,753,036 | 3,546 | 222,616,704 |
| W3DEnglishZH.big | 2,696,872 | 16 | 2,696,186 |
| W3DZH.big | 189,741,064 | 4,432 | 189,589,260 |
| WindowZH.big | 8,493,653 | 80 | 8,490,275 |

12,280 entries in total. For every archive: the big-endian entry count in the header equals the
number of entries actually read; the little-endian size field in the header equals the file size
on disk to the byte; no entry ends past EOF; no two non-empty entries overlap.

The `.big` read path is **not endian- or width-sensitive** the way the W3D path is: it reads into
`Int` from `Lib/BaseTypeCore.h`, which is already `int32_t`, and it byte-swaps explicitly with
`betoh()`. That is why it works on `main` today while `.w3d` does not.

Three things the real data revealed that reading the code would not have:

* **`PatchZH.big` contains zero-size entries at offset 0 whose names are wildcards** (`Data\*`).
  They are deletion instructions for the patcher, not files. A validator that treats
  `offset < header size` as corruption flags them; the engine happily adds them to its file map.
* **Every one of the 12,280 stored paths uses `\` as the separator** (e.g.
  `Art\W3D\ABBarracks_AC.W3D`). A native port cannot pass these to `open()`; the archive layer
  must translate separators, and lookups are case-insensitive on Windows but not on Linux. The
  engine already lowercases the filename portion but leaves the directory portion as stored.
* **The longest stored path is 29 bytes.** `StdBIGFileSystem::openArchiveFile` reads each name
  into `char buffer[_MAX_PATH]` one byte at a time with no bound check, so a name of 260+ bytes is
  a stack overrun. Retail data comes nowhere near it; a mod could. Worth a bound, not a panic.

## 3. `.w3d` parsing with the engine's own `chunkio.cpp` — the before/after

The tool links `Core/Libraries/Source/WWVegas/WWLib/chunkio.cpp` unmodified and drives
`ChunkLoadClass` through a ~90-line POSIX `FileClass`. This is the engine's parser, not a
reimplementation of it.

### On `main` today: it fails on the first header

`Art\W3D\ABBarracks_AC.W3D`, 9,334 bytes, extracted from `W3DZH.big`:

```
sizeof(ChunkHeader)  16 (on-disk layout is 8)
chunks read          2
top-level bytes      8388881 (must equal the file size)
validation: 4 problem(s)
  - sizeof(ChunkHeader) is 16 bytes as compiled; the on-disk header is 8.
  - chunk 0x1098907648 claims 8388608 payload bytes, more than the whole file
  - top-level chunks account for 8388881 bytes; the file is 9334
  - 1 chunk ids are outside the W3D_CHUNK_* enum in w3d_file.h

    offset       size    sub  chunk
         0        257     no  0x00000100 W3D_CHUNK_HIERARCHY
       265    8388608     no  0x41800000 <unknown id>
```

Note the shape of the failure, because it is the dangerous one. `Open_Chunk()` does
`File->Read(&HeaderStack[i], sizeof(ChunkHeader))`, so it consumes **16** bytes and lands
`ChunkType` on bytes 0–7 and `ChunkSize` on bytes 8–15. The first chunk id still reads as
`0x100 W3D_CHUNK_HIERARCHY` — a real, correct-looking id — because the true id occupies the low
half of the widened field and the file happens to have a small size next to it. A reader that
stopped at "the first chunk id is right" would call this a success. It is not: the sub-chunk flag
is lost, the size is garbage, and the walk is off the rails by the second header.

Across all 4,429 `.w3d` files in `W3DZH.big`: **0 validate, 4,429 fail.** Only 11,011 chunks are
reached in total (2.5 per file) before the walk desynchronises.

### With PR #10's fixed-width typedefs: it is exact

Same tool, same file, built on the branch that makes `uint32` a `uint32_t`:

```
sizeof(ChunkHeader)  8 (on-disk layout is 8)
chunks read          79
top-level bytes      9334 (must equal the file size)
unknown chunk ids    0
validation: all cross-checks passed

    offset       size    sub  chunk
         0        492    yes  0x00000100 W3D_CHUNK_HIERARCHY
         8         36     no    0x00000101 W3D_CHUNK_HIERARCHY_HEADER
        52        240     no    0x00000102 W3D_CHUNK_PIVOTS
       300        192     no    0x00000103 W3D_CHUNK_PIVOT_FIXUPS
       500       4480    yes  0x00000200 W3D_CHUNK_ANIMATION
       508         44     no    0x00000201 W3D_CHUNK_ANIMATION_HEADER
       560       1468     no    0x00000202 W3D_CHUNK_ANIMATION_CHANNEL
      ...
      4988       1394    yes  0x00000000 W3D_CHUNK_MESH
      4996        116     no    0x0000001F W3D_CHUNK_MESH_HEADER3
      5120        192     no    0x00000002 W3D_CHUNK_VERTICES
      5320        192     no    0x00000003 W3D_CHUNK_VERTEX_NORMALS
      5520        448     no    0x00000020 W3D_CHUNK_TRIANGLES
      ...
```

Whole-archive sweep, `W3DZH.big`:

| | `main` (16-byte header) | PR #10 (8-byte header) |
|---|---:|---:|
| `.w3d` files parsed | 4,429 | 4,429 |
| all cross-checks passed | **0** | **4,424** |
| failures | 4,429 | 5 |
| chunks walked | 11,011 | **948,036** |

Across every archive containing models — `W3DZH.big` (4,429), `W3DEnglishZH.big` (7),
`PatchZH.big` (2) — 4,433 of 4,438 models parse and validate exactly, with 949,024 chunks walked.

**The typedef change in `WWLib/bittype.h` is what stands between the engine's existing W3D loader
and reading real assets natively.** No other change was needed: `chunkio.cpp` itself compiles and
runs unmodified at 64-bit on arm64. This is the measurement that PR #10 was making on faith.

### The 5 that still fail are a data anomaly, not a portability one

`UISabotr_idel`, `_Jump`, `_Left`, `_Right`, `_Up` — five UI animation files. In
`UISabotr_Jump.w3d`, the chunk chain is exact for the first four chunks and then drifts: the third
`W3D_CHUNK_ANIMATION_CHANNEL` at offset 1,188 declares 556 payload bytes, which is arithmetically
correct for its own header (136 frames × 1 float + 12 bytes), but the next valid chunk header
starts at 1,753 rather than 1,752. The drift recurs throughout the file and the top-level
`W3D_CHUNK_ANIMATION` claims 51,984 bytes in a 52,109-byte file.

This is a property of the files, not of the platform: the tool runs the engine's own reader, and
the engine on Windows walks chunks the same way, so it should behave identically there.
**Untested** — no Windows build was run against these files in this session. Whether the retail
game loads them by a path that does not walk the chunk chain generically is also untested.

## 4. Textures: the retail data is exactly the BC formats MoltenVK reported

`TexturesZH.big` holds 3,546 entries; 3,496 are `.dds` and 50 are `.tga`. Every one of the 3,496
DDS files parsed, and for every one the payload is *exactly* the size implied by its declared
dimensions, mip count and block format — a total-size check, not a header sniff:

| FourCC | Files | Vulkan format |
|---|---:|---|
| DXT1 | 1,975 | `VK_FORMAT_BC1_RGBA_UNORM_BLOCK` |
| DXT3 | 6 | `VK_FORMAT_BC2_UNORM_BLOCK` |
| DXT5 | 1,515 | `VK_FORMAT_BC3_UNORM_BLOCK` |

Zero uncompressed DDS, zero formats outside BC1/BC2/BC3.

This closes the loop `moltenvk-findings.md` left open. That document measured
`textureCompressionBC = VK_TRUE` on this GPU; this measures that **100% of the game's shipped
textures are in that family**, so no runtime transcoding path is needed for them. BC2 (DXT3) is
only 6 files, but it is not zero — the renderer still has to support it.

The 50 `.tga` files are uncompressed and were not analysed here.

## 5. Portability defects found and fixed in passing

Three files in the tree could not compile on macOS at all. All three are in the existing
non-Windows compatibility layer, so they were presumably only ever built on Linux:

| File | Problem |
|---|---|
| `Dependencies/Utility/Utility/time_compat.h` | uses `CLOCK_BOOTTIME`, which is Linux-only |
| `Dependencies/Utility/Utility/thread_compat.h` | returns `pthread_self()` as `int`; `pthread_t` is a pointer on macOS |
| `Dependencies/Utility/Utility/endian_compat.h` | its `__APPLE__` branch uses `UInt16`/`UInt32`/`UInt64`, Carbon types from `<MacTypes.h>`, which it never includes |

The last one is worth noting for what it implies: the `__APPLE__` branch of `endian_compat.h`
has never been compiled by anyone. "Has a macOS branch" and "builds on macOS" are different
claims.

These three headers reach *every* translation unit, via `always.h` → `Utility/compat.h`, so the
effect on the headline probe number is not marginal. Running the repo's own
`scripts/native-port-probe.py --with-shims` on this Mac, before and after:

| | clean / 737 |
|---|---:|
| `main` as it stands, on macOS | **20** (2.7%) |
| with the three compat-header fixes | **627** (85.1%) |
| documented figure, Linux + clang 14 | 637 (86.4%) |

So the "86% of the engine's own C++ is portable" figure is a *Linux* figure. On macOS — the actual
target of this port — the same probe returned 20 before this branch. Three lines of platform glue
account for the entire difference. The remaining 10-unit gap to the Linux number is unexamined;
the top macOS-only failure categories are `debug/` console I/O, `DbgHelp*`, and Winsock, i.e. code
already known to need a platform layer.

The probe should be run on macOS as well as Linux before any portability percentage is quoted
again.

## 6. What this contradicts, confirms, and leaves open

**Confirms.** `review-and-decisions.md` §1.2 argued that `-fsyntax-only` is blind to layout bugs
and that making the integer widths correct changes the probe's answer by one translation unit
while changing `sizeof(ChunkHeader)` from 16 back to 8. Both halves are now measured on real data:
the probe delta is cosmetic, the asset delta is 0% → 99.9%.

**Contradicts.** The 86%-shimmed figure quoted throughout the doc set does not hold on macOS. It
was measured on Linux, and on macOS the same probe against the same tree returns 20/737 until
three Linux-only compat headers are fixed (§5). Nothing was wrong with the count; the platform it
was measured on was never stated, and the port targets the other one.

It also sharpens Phase 2. `native-port-plan.md` scopes Phase 2
("64-bit correctness", 600–1,200 h) around save/load serialisation and pointer casts. The measured
blocker for *asset loading* is narrower than that: it is the scalar typedefs in one header. Fixing
`bittype.h` is hours, not weeks, and it unblocks the entire W3D asset pipeline. The rest of Phase
2 is still needed; it is just not what stands between the engine and reading its own data.

**Open, explicitly untested here.**

* Only the *chunk container* layer was exercised. The structs *inside* the chunks
  (`W3dMeshHeader3Struct`, `W3dVertexMaterialStruct`, the animation channel structs) have their
  own layout requirements and were not checked. Chunk payload sizes matching exactly is strong
  circumstantial evidence, not proof.
* `.big` decompression (refpack/LZH-compressed entries) was not exercised; the entries read here
  are stored raw.
* No INI, map, or audio file was parsed.
* Nothing was rendered. This says the bytes can be read, not that they can be drawn.
* No Windows build was run for comparison, so "the retail game behaves the same on these five
  files" is inference from shared source, not measurement. CI's `win32` presets currently fail on
  `main` and on every open branch with `intrin_compat.h(97): fatal error C1012` — MSVC choking on
  `__has_builtin(_lrotl)` — so no MSVC build of this tree exists to compare against right now.
  That is pre-existing and unrelated to this branch, which touches no file in that path.

## Reproducing

```sh
cmake -S spikes/assets -B build/assets -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/assets
build/assets/zh-asset-inspect sizes
build/assets/zh-asset-inspect inventory /path/to/zero-hour
build/assets/zh-asset-inspect sweep /path/to/zero-hour/W3DZH.big "$(mktemp -d)" .w3d
```

The tool takes its integer widths from the tree it is built in, so building it on `main` instead
reproduces the 16-byte failure. See [`spikes/assets/README.md`](../../spikes/assets/README.md).
