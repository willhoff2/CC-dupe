# Block-compressed textures: which #137 candidate it was, what the seam now serves, what it refuses

Slice 5 of wave 9. `docs/porting/mission-frame-corruption.md` §5 item 1 left two candidates for
why a mission frame created 3,851 `B8G8R8A8` textures and zero block-compressed ones while every
model read back as noise: (a) a lock-pitch defect on the compressed path, or (b) `CheckDeviceFormat`
refusing the BC formats so `DX8Caps::SupportDXTC` was false. This document answers that with the
measurement #137 asked for, makes the compressed path real through the seam, and separates the
synthetic evidence (Linux/lavapipe, macOS/MoltenVK in CI) from the real-game evidence (macOS arm64,
M1 Pro, MoltenVK).

Every result carries one of **PORT DEFECT**, **UNIMPLEMENTED PATH**, **MISSING DATA**,
**SYNTHETIC-ONLY**. The enforced numbers are produced by `scripts/ci/check-bc-textures.py` on every
CI run; `docs/porting/ci-baselines/*.json` remain the source of truth for everything else quoted
here, and nothing in this file is.

## 0. Summary

- **It was neither candidate as stated (§1).** The caps were truthful all along: on `main`
  (`6ece888d8`) the adapter's `sampled_formats` mask already carried DXT1-DXT5 and
  `CheckDeviceFormat(DXT1/3/5)` answered `D3D_OK`. And there was no compressed lock whose pitch
  could be wrong, because no compressed texture was ever created: `Create_Lockable_Texture` on
  `main` had no host layout for a block format (`Source_Texel_Bytes` returned 0 and the call
  returned null with "block-compressed formats are not served by this path"). `CreateTexture(DXTn)`
  therefore failed, the off-Windows D3DX `Create_Fitted` walked its candidate list, created
  `A8R8G8B8` instead, printed one stderr line, returned `D3D_OK`, and the engine's `DDSFileClass`
  handled the result as the DXT texture it had asked for. Classification: an **UNIMPLEMENTED PATH**
  (compressed lockable textures) hidden behind a substitution that no ledger counted — the
  silent-success class this project exists to stop. The fix is the block-layout lock path #137
  priced under (a); the "pitch confusion" mechanism itself never ran.
- **The noise has a mechanism, reproduced on Linux (§1.4):** with caps saying DXTC yes and creation
  quietly returning 8888, `TextureLoadTaskClass::Load_Compressed_Mipmap` passes its own `Format`
  (DXTn) as `dest_format`, `DDSFileClass::Copy_Level_To_Surface` takes its `dest_format == Format`
  branch and `memcpy`s the *compressed* level (2,048 bytes for 64x64 DXT1) into an 8888 lock with
  pitch 256. The top 1/8 of a DXT1 level (1/4 for DXT3/5) is raw block bytes read as BGRA — each
  8-byte block becomes two pixels (endpoints, then indices), a 2-px-period vertical stripe — and
  the remaining rows stay at the backend's zero fill: black. That is the row-banded,
  mostly-black model surface a human sees on the Mac frame (§5), with terrain and 2D art (TGA and
  uncompressed paths) untouched, because only the compressed path was substituted. **PORT DEFECT**
  (caps and creation disagreed) on top of the unimplemented path; the D3DX contract itself is the
  oracle's.
- **The seam now serves BC1/BC2/BC3 (§2):** DXT1 -> `VK_FORMAT_BC1_RGBA_UNORM_BLOCK`, DXT2/3 ->
  `BC2_UNORM_BLOCK`, DXT4/5 -> `BC3_UNORM_BLOCK`; real compressed `VkImage`s, block-row host
  storage and staging, `LockRect.Pitch` = bytes per row of 4x4 blocks (`ceil(w/4) * 8` for BC1,
  `* 16` for BC2/3) on every level, sub-rect locks block-aligned and refused otherwise. The pitch
  is tight — no padding — because `DDSFileClass::Copy_Level_To_Surface` `memcpy`s a whole
  compressed level into the lock and ignores `dest_pitch`; any padded pitch would corrupt every
  level the same way #63 did.
- **Refusals are loud and counted (§3):** an unsampleable format fails `CheckDeviceFormat` and
  `CreateTexture` and lands in the unimplemented-call ledger; a misaligned compressed sub-rect
  lock fails with `D3DERR_INVALIDCALL` and is counted; `CopyRects` with a compressed endpoint is
  refused and counted; and the D3DX substitution that hid the original defect is now reported
  through `RenderBackendClass::Record_Unserviceable`, so a substituted format appears in the same
  ledger as `D3DXCreateTexture(block-compressed format substituted)`. The Windows D3D8 backend is
  untouched: `Record_Unserviceable` is a no-op there and the D3DX replacement is `!_WIN32` only.
- **Proven with pixels, synthetically (§4, SYNTHETIC-ONLY):** `zh-bc-textures` creates three
  16x16 five-level BC1/BC2/BC3 textures, writes deterministic blocks through the lock at block
  pitch, rewrites a block-aligned sub-rect, is refused a half-block one, reads a level back
  byte-exact, draws nine cases and classifies every block from the framebuffer, with the validation
  layer loaded and silent. `ZH_RENDER_NO_BLOCK_COMPRESSED=1` is the negative control and the gate
  requires it to fail *for that reason*. Wired into both CI jobs (Linux/lavapipe, macOS
  arm64/MoltenVK).
- **The engine's own path agrees (§4.2):** `DX8Wrapper::_Create_DX8_Texture(DXT1/3/5)` through
  the real `DX8Caps` returns the requested format with the block pitch on levels 0 and 5 and refuses
  the half-block lock; `DX8Caps::Support_DXTC()` is 1.
- **Real game on the Mac (§5):** see that section for the BC creation count, missing-texture count,
  the mission-frame classification, and the answer to "are the models still noise" — measured on
  the M1 Pro, not inferred from lavapipe.

## 1. The decision between the candidates

### 1.1 What #137 asked to measure

(a) lock a 64x64 8888 level, write a known pattern through the D3D8 pitch, read it back;
(b) print the `sampled_formats` bits for DXT1/3/5 at enumeration.

### 1.2 What was measured (Linux x86-64, lavapipe `llvmpipe (LLVM 15.0.7)`, Mesa 23.2.1, Vulkan 1.3.255)

**(b) is false.** `Sampled_Format_Mask` on `main` iterates DXT1..DXT5 with every other format and
sets the bit when `vkGetPhysicalDeviceFormatProperties(...).optimalTilingFeatures` has
`SAMPLED_IMAGE_BIT`; lavapipe reports it for BC1/2/3, and `zh-bc-textures` prints
`format DXT1 (BC1): device samples it yes` (same for BC2, BC3). `CheckDeviceFormat` in
`vulkanrenderbackend.cpp` reads that mask, so the engine's `DX8Caps` saw DXTC as supported.
The engine harness confirms it end to end: `DX8Caps::Support_DXTC() = 1`.

(One artefact was found on the way and fixed in the harness, not the engine: `native_render_run`
did not call `Init_D3D_To_WW3_Conversion()`, which `WW3D::Init()` does before `DX8Wrapper::Init()`.
Without it `D3DFormat_To_WW3DFormat(DisplayFormat)` is `WW3D_FORMAT_UNKNOWN` and
`DX8Caps::Check_Texture_Format_Support` marks *every* texture format unsupported, including
`A8R8G8B8`. That read as a caps refusal for a minute and was not one; the game calls the
initialiser.)

**(a) could not have happened.** On `main`, `Create_Lockable_Texture` returned null for every
block-compressed format before allocating anything, so no compressed `LockRect` existed to have a
wrong pitch. The 8888 lock path that *did* run is the one #137's own trace shows
(`pitch0=128 expand=1`), and the engine's software decoder (`DDSFileClass::Get_4x4_Block` into a
locked 8888 level) is the path that produced the noise. Whether that decoder-into-8888 path is
itself correct is a separate question that this slice makes moot for DDS textures — they no
longer take it — but not for the noise question, which §5 measures directly.

### 1.3 What it actually was

`IDirect3DDevice8::CreateTexture(DXTn)` -> backend refuses (no compressed host layout) ->
off-Windows `D3DXCreateTexture` (`d3dx8texcreate.cpp` `Create_Fitted`) tries the next candidate
format, succeeds with `A8R8G8B8`, prints to stderr, returns `D3D_OK` -> `TextureLoader` sees a
successful creation and `DDSFileClass::Copy_Level_To_Surface` decodes blocks to 8888.

Three things were wrong with that at once, and they are fixed in this order of importance:

1. The compressed lockable path was **UNIMPLEMENTED** — now implemented (§2).
2. The substitution was **silent** at the ledger level — now recorded as a chosen fallback (§3).
3. The `native_render_run` harness could not have told the difference because of the conversion
   table artefact — now it calls the initialiser and asserts `Support_DXTC()`.

### 1.4 Reproducing the failure signature on Linux (SYNTHETIC-ONLY, lavapipe)

The negative control forces the `main` shape of the world — caps refuse nothing the engine asks
before creation, creation refuses the block format — through the engine's own `_Create_DX8_Texture`:

```
$ ZH_RENDER_NO_BLOCK_COMPRESSED=1 python3 scripts/native-render-backend-run.py --validation
D3DX texture creation: this device rejected format 827611204, so the texture was created as format 22 instead.
DXT1: texture created                      ok
DXT1: created as requested                 FAILED  (asked D3DFMT 827611204, got 22, 7 levels)
DXT1: lock pitch is the block pitch        FAILED  (level 0, 64 wide: pitch 256, block pitch 128)
DXT5: created as requested                 FAILED  (asked D3DFMT 894720068, got 21, 7 levels)
== the unimplemented-call ledger
     3 x  IDirect3DDevice8::CreateTexture(unsupported format)
     3 x  D3DXCreateTexture(block-compressed format substituted)
```

`_Create_DX8_Texture` returned `S_OK` with an `X8R8G8B8`/`A8R8G8B8` texture at pitch 256 where the
engine believes it holds DXT1 at pitch 128 (and it never re-reads `GetLevelDesc`). The only
difference from `main` is that the two ledger lines now exist. On `main` the game never saw either:
`DX8Caps::Support_DXTC()` was 1 (the sampled mask was truthful), so `Get_Valid_Texture_Format` kept
DXTn, `Load_Compressed_Mipmap` ran, and the whole-level `memcpy` wrote compressed bytes into an
uncompressed lock. (In this forced run the caps *also* say no, so a real load would take the
engine's own software decode instead; the substitution and the pitch mismatch are what the run
isolates.)

## 2. What is implemented (the seam)

`spikes/renderer/src/vulkan_backend.cpp`:

- `Plan_For(DXTn)` already mapped to BC1/BC2/BC3 with `block_size = 4` and `block_bytes` 8/16;
  the lockable path now uses it. `LockableLevel` gained `rows` (block rows for compressed
  levels), `TextureHandle` gained `block_size`.
- Host storage and the staging copy are sized `rows * pitch` where `pitch = ceil(w/4) *
  block_bytes` and `rows = ceil(h/4)`; the `vkCmdCopyBufferToImage` region uses the full level
  extent with `bufferRowLength = 0`, so Vulkan reads the same tightly packed block rows the caller
  wrote. Tail levels smaller than a block are one block.
- `Lock_Texture` returns `pitch` = block pitch and `bits` offset by
  `(top / 4) * pitch + (left / 4) * block_bytes`; a sub-rect that does not start on a block
  boundary, or end on one other than the level's own edge, is refused (`Block_Aligned`).
- Compressed textures cannot be initialised with `vkCmdClearColorImage` (invalid for BC formats
  in Vulkan), so the first upload is a staging copy of the zero-filled block rows.
- `resource_stats_.block_compressed_textures` counts each compressed creation; the trace line
  `create texture=... lockable WxH mips=N fmt=F vk=V pitch0=P` carries the VkFormat (133/135/137 for BC1/BC2/BC3).
- `Supports_Texture_Format` and `Sampled_Format_Mask` consult `vkGetPhysicalDeviceFormatProperties`
  per format; `ZH_RENDER_NO_BLOCK_COMPRESSED` removes the BC formats from both and makes
  `Create_Lockable_Texture` refuse them, which is the negative control.

`Core/Libraries/Source/WWVegas/WW3D2/vulkanrenderbackend.cpp`:

- `CheckDeviceFormat` answers from the measured mask; `CreateTexture` refuses an unsampleable
  format (`CreateTexture(unsupported format)` in the ledger) rather than widening it.
- `VulkanD3DSurfaceClass::GetDesc` on a compressed level reports the DXTn `D3DFORMAT` and the
  block-sized `Size`.
- `VulkanD3DTextureClass::LockRect` checks block alignment before calling the backend and records
  a misaligned lock as `IDirect3DTexture8::LockRect(compressed sub-rect)`.

`renderbackend.h` gained `Record_Unserviceable(name, why)` (default no-op, so the D3D8 backend and
the mock backends are unchanged); the Vulkan backend forwards it to its ledger.

## 3. What is refused, and why

| Call | Answer | Ledger entry | Class |
|---|---|---|---|
| `CheckDeviceFormat` for a format the device cannot sample | `D3DERR_NOTAVAILABLE` | — (a truthful "no", as D3D8 gives) | truthful caps |
| `CreateTexture` in a format the device cannot sample | `D3DERR_INVALIDCALL` | `IDirect3DDevice8::CreateTexture(unsupported format)` | refusal |
| `CreateTexture` with a `D3DFORMAT` the seam has no `TextureFormat` for | `D3DERR_INVALIDCALL` | `IDirect3DDevice8::CreateTexture(format)` | UNIMPLEMENTED PATH |
| `LockRect` on a compressed level with a non-block-aligned rect | `D3DERR_INVALIDCALL` | `IDirect3DTexture8::LockRect(compressed sub-rect)` | oracle-correct refusal (D3D8 fails it too) |
| `CopyRects` with a block-compressed source or destination | `D3DERR_INVALIDCALL` | `IDirect3DDevice8::CopyRects(block-compressed surface)` | UNIMPLEMENTED PATH; the engine's compressed uploads go through `LockRect` |
| D3DX substituting a format the device refused | `D3D_OK` (D3DX's contract) | `D3DXCreateTexture(block-compressed format substituted)` / `(format substituted)` | **chosen fallback, recorded**: D3DX on Windows walks the same candidate list, so the behaviour is the oracle's; what was missing was the count |
| `Create_Lockable_Texture(P8)` | null | (through `CreateTexture`) | UNIMPLEMENTED PATH, unchanged from before |

### 3.1 The failure mode, named: silent format substitution

The defect class here is more valuable than the fix and is easy to lose, so it is written down
separately from the mechanism in §1.3.

**A creation call that fails at the requested format and reports success at a different one.**
`CreateTexture(DXT1)` failed; `D3DXCreateTexture` walked its candidate list, created `A8R8G8B8`,
and returned `D3D_OK`. Every consumer downstream trusted the HRESULT and none re-read
`GetLevelDesc`, so the engine held a texture whose *format, pitch and level sizes* were all
different from what it believed, and wrote raw DXT bytes into it. Nothing failed, nothing was
logged to the ledger, validation was silent (the writes were in-bounds), and the only symptom was
pixels — the stripes-and-black in §5.1. It is #63's class (surface layout diverging from the image)
arrived at through a *successful* call rather than a broken one.

**Why `CheckDeviceFormat` was not the culprit, and both #137 candidates were wrong.**
`DX8Caps::Support_DXTC()` measured 1 on lavapipe (§1.2): the sampled-feature mask was truthful and
the engine kept the DXTn request. And no compressed `LockRect` ever executed, because creation
had already failed and the lock that did run was an 8888 lock reporting an honest 8888 pitch. The
pitch *mismatch* was real but it was between the engine's belief and the object, not inside the
compressed lock path — that path did not exist. Candidate A (caps refusal → software decode) and
candidate B (compressed pitch defect) both presuppose a texture created in the requested format.

**How it was detectable, and how it is now.** Three observations, any one of which would have
named it:

1. `D3DX texture creation: this device rejected format 827611204, so the texture was created as
   format 22 instead.` on stderr — one line per texture, present on `main`, drowned in the load
   log and counted nowhere.
2. `GetLevelDesc().Format != requested` after a successful `D3DXCreateTexture` — the check the
   `native_render_run` probe now makes (`created as requested`, §4.2), which no engine site makes.
3. The exit ledger: on `main` it had no entry for this, which is why it read as "0 BC textures
   created" with nothing refused — the silence *was* the finding.

**The regression barrier.** The ledger now counts substitutions. `Create_Fitted` in
`d3dx8texcreate.cpp` calls `RenderBackendClass::Record_Unserviceable` whenever the format it
succeeded with is not the format asked for, under
`D3DXCreateTexture(block-compressed format substituted)` for DXTn and
`D3DXCreateTexture(format substituted)` for everything else; both appear in the exit ledger and in
the negative-control output (§1.4, §4.2). A run of the real game whose ledger shows a non-zero
count on either line has taken a chosen fallback and says so. A run that creates zero BC textures
with *neither* line present is now a different kind of failure (the engine never asked), not this
one.

**Can any other creation path in the seam substitute without being counted?** Audited on this
branch (`vulkanrenderbackend.cpp`, `d3dx8texcreate.cpp`, `spikes/renderer/src/vulkan_backend.cpp`):

| Path | Can it change the format? | Counted? |
|---|---|---|
| `IDirect3DDevice8::CreateTexture` / `CreateImageSurface` | No: untranslatable or unsampleable formats fail with `D3DERR_INVALIDCALL` | Yes, `Record_Unimplemented` (§3 table) |
| `CreateCubeTexture` / `CreateVolumeTexture` | No: refused outright | Yes, `IDirect3DDevice8::CreateCubeTexture` / `CreateVolumeTexture` |
| `D3DXCreateTexture` / `D3DXCreateCubeTexture` / `D3DXCreateVolumeTexture` (all three go through `Create_Fitted`) | Yes, by contract — the same candidate walk Windows D3DX performs | Yes, `Record_Substitution` on every non-first success. It records through `target.Backend`; off-Windows `target.Backend` is always the live seam (`Has_Device()`), and on Windows this file is not compiled |
| `CreateRenderTarget` / `CreateDepthStencilSurface` | The backend owns one depth format (`Pick_Depth_Stencil_Format`: `D24S8` if the device has it, else `D32S8`) | Not a substitution as seen by the engine: `CheckDeviceFormat`/`CheckDepthStencilMatch` answer `D3D_OK` only for `D24S8`, the format the engine is told it has, and no engine site locks the depth buffer. If a site ever does, that is a new UNIMPLEMENTED PATH, not a silent one: `LockRect` on the depth surface is refused |
| Backend host expansion (`FormatPlan::expand_to_bgra8` for `R8G8B8`, `P8`, `A4R4G4B4`, and `X8R8G8B8`/`A1R5G5B5`/`R5G6B5` when the device lacks view swizzle) | The *Vulkan image* is `B8G8R8A8`, but `GetLevelDesc`, `LockRect` pitch and level sizes all report the **requested** D3D8 format and the texels are converted on unlock (`expand_on_unlock`) | Not a substitution: the engine's view and the object agree, which is exactly the invariant the DXT case broke. It is not in the ledger and does not need to be; the write-through-the-pitch locks in `zh-resource-lock-tests` cover it |
| `Create_Lockable_Texture(P8)` | No: returns null → `CreateTexture` fails | Yes, through `CreateTexture(unsupported format)` |

So on this branch there is no path through the seam that returns success at a format other than
the one asked for without a ledger line. That is the barrier; the synthetic checkers
(`check-bc-textures.py` negative control, `native-render-backend-run.py` "created as requested")
fail when it is crossed for DXTn, and the ledger line is the tell for every other format.

No format is silently widened or silently decoded by the backend. When the engine's software
decode *does* run — `TextureLoader` with `compression_allowed = 0`, thumbnails, the
`Get_Valid_Texture_Format(..., false)` sites — that is the engine's own choice, taken on Windows
too, and it is not a substitution.

## 4. Synthetic evidence — SYNTHETIC-ONLY

### 4.1 `zh-bc-textures` (Linux x86-64, lavapipe, `VK_LAYER_KHRONOS_validation` loaded)

`python3 scripts/ci/check-bc-textures.py --binary build/spike/zh-bc-textures`:

```
format DXT1 (BC1): device samples it yes
DXT1 (BC1): 16x16, 5 levels created through the lockable path
format DXT3 (BC2): device samples it yes
DXT3 (BC2): 16x16, 5 levels created through the lockable path
format DXT5 (BC3): device samples it yes
DXT5 (BC3): 16x16, 5 levels created through the lockable path
DXT1 (BC1) level 0 (16 texels): lock pitch 32, block pitch 32 ok
DXT1 (BC1) level 1 (8 texels): lock pitch 16, block pitch 16 ok
DXT1 (BC1) level 2 (4 texels): lock pitch 8, block pitch 8 ok
DXT1 (BC1) level 3 (2 texels): lock pitch 8, block pitch 8 ok
DXT1 (BC1) level 4 (1 texels): lock pitch 8, block pitch 8 ok
DXT3 (BC2) level 0 (16 texels): lock pitch 64, block pitch 64 ok   (... 32, 16, 16, 16)
DXT5 (BC3) level 0 (16 texels): lock pitch 64, block pitch 64 ok   (... 32, 16, 16, 16)
DXT1/DXT3/DXT5 misaligned sub-rect (2,0)-(6,4): refused
orientation: pixel (8,8) = 33 32 24, DXT1 block 0 wants 33 32 24: top-left verified
DXT1/DXT3/DXT5 level 2 read back: 8/16/16 block bytes match
case 0..8: level 0 as written, level 0 after sub-rect update, level 1 via mip filter, x3 formats
validation layer: loaded
validation messages: 0
bc-textures: PASS
OK: 3 block-compressed formats x 5 levels created, written at block pitch, a block-aligned
sub-rect rewritten, a half-block sub-rect refused, a level read back byte-exact, 9 cases drawn
and every block verified in the readback, the validation layer active and silent
```

Each drawn case is classified block by block: the decoded colour of every 4x4 block is compared
against the colour the workload encoded into that block, so a wrong pitch, a wrong block offset, a
wrong orientation or a substituted format each fail a specific block, not the run.

Negative control, `ZH_RENDER_NO_BLOCK_COMPRESSED=1`:

```
format DXT1 (BC1): device samples it no
Create_Lockable_Texture: block-compressed formats refused (ZH_RENDER_NO_BLOCK_COMPRESSED)
bc-textures: the backend refused a block-compressed texture (DXT1 (BC1))
bc-textures: FAIL          (exit 1)
```

The gate requires the control to exit non-zero *and* to print that refusal; a control that fails
for any other reason fails the gate ("the failure is not the one this gate measures").

The other renderer gates on this tree: `check-spike-render.py` 0/480000 differ, validation silent;
`check-draw-capacity.py --self-check` 4096 draws, 0 dropped, 0 aliased; `check-untyped-vertex-buffer.py`
7 layouts x 64 tiles verified; `check-d3d8-surface.py` and `check-backend-coverage.py` exact.

### 4.2 The engine's own creation path (Linux x86-64, lavapipe, validation loaded)

`CLANGXX=clang++-14 python3 scripts/native-render-backend-run.py --validation`, which links the
harness against the strict build's own archives (980/980 objects, 0 unresolved) and goes through
`DX8Wrapper::Init`, `Set_Render_Device`, `DX8Caps` and `DX8Wrapper::_Create_DX8_Texture`:

```
DX8Caps::Support_DXTC() = 1
caps: DXTC supported                       ok
DXT1: created as requested                 ok  (asked D3DFMT 827611204, got 827611204, 7 levels)
DXT1: lock pitch is the block pitch        ok  (level 0, 64 wide: pitch 128, block pitch 128)
DXT1: lock pitch is the block pitch        ok  (level 5, 2 wide: pitch 8, block pitch 8)
DXT1: unaligned sub-rect lock refused      ok
DXT3: ... pitch 256 / 16 ... ok            DXT5: ... pitch 256 / 16 ... ok
== the unimplemented-call ledger
     3 x  IDirect3DTexture8::LockRect(compressed sub-rect)
            a block-compressed sub-rect lock must be 4x4-block aligned
validation messages: 0
stages failed: 0
```

The three ledger entries are the harness's own deliberate half-block locks: the refusal is
counted, which is the point.

### 4.3 macOS arm64 / MoltenVK, in CI

The same `check-bc-textures.py` step runs in the macOS job of `native-port-ci.yml` (step
"Block-compressed textures through MoltenVK"). Its output on the PR's CI run is the MoltenVK
synthetic evidence; nothing about MoltenVK is inferred from lavapipe here.

Measured on PR #141's run (GitHub `macos-15-arm64` runner, macOS 15.7.7, MoltenVK reporting
`device: Apple Paravirtual device`, validation layer loaded) — SYNTHETIC-ONLY, not a real GPU
name and not the retail game:

```text
format DXT1 (BC1): device samples it yes    16x16, 5 levels created through the lockable path
format DXT3 (BC2): device samples it yes    16x16, 5 levels created through the lockable path
format DXT5 (BC3): device samples it yes    16x16, 5 levels created through the lockable path
misaligned sub-rect locks: refused
orientation: pixel (8,8) = 33 32 24, DXT1 block 0 wants 33 32 24: top-left verified
DXT1 (BC1) level 2 read back: 8 block bytes match
DXT3 (BC2) level 2 read back: 16 block bytes match
DXT5 (BC3) level 2 read back: 16 block bytes match
validation messages: 0
bc-textures: PASS
OK: 3 block-compressed formats x 5 levels created, written at block pitch, a block-aligned
sub-rect rewritten, a half-block sub-rect refused, a level read back byte-exact, 9 cases drawn
and every block verified in the readback, the validation layer active and silent
```

So MoltenVK samples BC1/2/3 and the block-pitch lock contract holds on it; the M1 Pro
real-game measurement in §5 remains owed.

## 5. Real-game evidence — macOS arm64, Apple M1 Pro, MoltenVK

**Status at the time of this PR: NOT MEASURED ON THE BRANCH — MISSING DATA.** Two `will-mac` child
sessions were started for exactly this measurement (`0b5ed184…`, `24bbe3a8…`); neither got past
environment start-up (the repository pull hung for ~30 minutes twice and the first session was
suspended for inactivity having run nothing). The blocker is reported. Everything below that is
not labelled otherwise is what a human saw on the Mac running the *pre-slice* game, plus what the
Linux reproduction predicts for the branch; none of it is a claim about the branch on MoltenVK.

### 5.1 What a human saw (macOS arm64, M1 Pro, MoltenVK; build SHA not stated, pre-slice behaviour; two screenshots)

The user attached two real mission frames (China base on a grass map, 30 FPS, HUD and radar
intact) to the session — retail art, so not committed. In both:

- Terrain, terrain blends, tree sprites, the command bar, unit portraits, radar and health bars are
  correct.
- Every building, vehicle and infantry model, *and* the flat road/decal quads on the terrain, are
  either solid black or covered in a fine (1-2 px period) multi-coloured vertical stripe pattern
  that repeats across whole faces. Geometry, silhouettes, depth, transforms and the health bar's
  placement are right.

That is exactly the signature §1.4 reproduces: DXT-payload DDS surfaces — models and decals —
carry raw block bytes in their top rows (the 2-px stripe is one 8-byte DXT1 block read as two BGRA
pixels) and zero fill below; TGA-sourced and uncompressed-DDS art (terrain atlas, UI, tree sprites
with alpha) never took the substituted path. The two appearances are one mechanism, and the
black is "no texel data written", not "misread data": the `memcpy` covers only `compressed_size`
bytes of a surface eight (DXT1) or four (DXT3/5) times larger.

This answers the question the slice was set — **the models were never noise in their vertex,
index, stride, transform or sampler state**: every geometric property in the frame is correct and
the defect follows texture *format*, not mesh. It does so from a human reading two screenshots,
not from a per-draw trace on the branch; the per-draw isolation the prompt asked for is therefore
not needed for a *second* cause, but the measurement that the branch removes the first one is
still owed.

### 5.2 What the branch must show when the outpost is available (owed, not claimed)

From `scripts/mission-frame-trace.py` on the branch, in the same mission:

| Figure | `main` (#137, measured) | branch, predicted | branch, measured |
|---|---|---|---|
| `create texture` lines with `vk` in {133,135,137} | 0 | hundreds (every DXT DDS) | **not measured** |
| `create texture` lines with `vk=44` | 3,851 | far fewer (TGA and 8888 DDS only) | **not measured** |
| `pitch0` for BC creations vs width | — | `ceil(w/4)*8` (BC1) / `*16` (BC2/3) | **not measured** |
| `missing-texture` lines in the mission | 1 | 1 (unchanged; unrelated to format) | **not measured** |
| ledger: `D3DXCreateTexture(block-compressed format substituted)` | (did not exist) | 0 | **not measured** |
| model/decal surfaces: black or 2-px striped | yes (human) | no (human) | **not looked at** |
| negative control `ZH_RENDER_NO_BLOCK_COMPRESSED=1` | — | caps say no DXTC; engine software-decodes; substitution count 0, BC count 0 | **not measured** |

When those numbers exist they replace this table; until then the branch's real-game claim is:
none.

## 6. Ranked residuals

Ranked by what a player notices; classified.

1. **Branch not yet measured in the real game on MoltenVK — MISSING DATA.** The synthetic
   evidence is complete on lavapipe and on the macOS arm64 CI runner (§4.3); the real-game table
   in §5.2 is empty because the `will-mac` outpost disconnected three times during this slice
   (two child sessions, no command run). This PR was landed on that evidence deliberately, with
   no claim about the branch's mission frame. **Named follow-up, to run the moment the outpost
   stays connected:** on the M1 Pro, with the merged SHA, `scripts/mission-frame-trace.py` into
   the USA campaign — (a) BC creation count (`vk=133/135/137` traces, must be non-zero), (b)
   `missing-texture` count from `game.log`, (c) the exit ledger (both substitution lines must be
   0), (d) the mission-frame PNG classified per pixel and per draw, (e) a human looking at it.
   If (d)/(e) still show striped or black skins with (a) non-zero and (c) zero, the noise has a
   second cause and is isolated with `ZH_RENDER_TRACE_PERDRAW=1`. Cost: one working `will-mac`
   session (~1 h).
2. **`CopyRects` with a compressed endpoint is refused — UNIMPLEMENTED PATH.** No mission path
   is known to take it (the engine uploads compressed levels through `LockRect`); if the ledger on
   the Mac shows the entry, that is the next item. Cost: a block-granular `vkCmdCopyImage`, under a
   session.
3. **Palettised (`P8`) textures still refused — UNIMPLEMENTED PATH**, unchanged; not used by the
   retail data as far as any trace has shown.
4. **The engine's whole-level `memcpy` relies on a tight compressed pitch.** The backend's is
   tight and the workload asserts it on every level; a future padded pitch would reintroduce #63.
   Guarded by `check-bc-textures.py`; not a current defect.
5. **The `dest_format == Format` `memcpy` in `DDSFileClass` cannot detect a format substitution
   because the engine never re-reads `GetLevelDesc`.** That is upstream engine behaviour and also
   the oracle's; the port now makes the substitution visible in the ledger rather than changing
   the engine. Not a defect to fix here; recorded so it is not rediscovered.

Everything else in `docs/porting/playability-probe.md` §9 (sound, save/load, input wedge, texture
leak, quit crash, logic speed) is unaffected by this slice and is not re-ranked here.

## 7. Reproducing this

```sh
# synthetic, either platform (validation layer: eval "$(python3 scripts/ci/vulkan_manifests.py --require-layer --print-env)" on macOS)
cmake --build build/spike --target zh-bc-textures
python3 scripts/ci/check-bc-textures.py --binary build/spike/zh-bc-textures
ZH_RENDER_NO_BLOCK_COMPRESSED=1 ./build/spike/zh-bc-textures --validation   # must FAIL

# the engine's creation path
CLANGXX=clang++-14 python3 scripts/native-build.py --level 1 --level 2 --level 3 --level 4 --with-shims --strict-link
CLANGXX=clang++-14 python3 scripts/native-render-backend-run.py --validation

# the real game (macOS, retail data; docs/porting/mission-frame-corruption.md §1)
python3 scripts/mission-frame-trace.py run --run-dir <run-dir> --out <out>
python3 scripts/mission-frame-trace.py summarize <out>/trace.log
grep -c 'create texture=.* vk=13[357] ' <out>/trace.log      # BC1/BC2/BC3 creations
grep -c '^missing-texture' <out>/game.log
```
