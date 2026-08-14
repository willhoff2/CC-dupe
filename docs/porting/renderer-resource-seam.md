# The renderer resource seam: D3D8 Lock/Unlock over Vulkan

`docs/porting/renderer-seam.md` §5 deliberately left the resource interfaces alone: the backend
seam operates on raw `IDirect3DTexture8*`, `IDirect3DSurface8*`, `IDirect3DVertexBuffer8*` and
`IDirect3DIndexBuffer8*`, and it recorded 213 call sites on those interfaces as the estimate for
a later slice. This document is that slice's answer: what the sites actually are, what the
callers do with the pointer and the pitch, what emulating that contract over Vulkan costs, and
which parts of the cost are now measured by a running test rather than asserted.

**Recommendation: keep the resource interfaces D3D8-shaped.** Not because abstracting them is
hard, but because the contract the engine relies on — a CPU pointer with a pitch, sometimes held
across threads and frames, sometimes still used *after* `Unlock`, sometimes handed to arbitrary
engine code that may read as well as write — is reproducible over Vulkan at a cost that is
bounded, measurable and mostly paid in host-visible memory. Section 4 quantifies it; section 7
lists what that cost does *not* cover.

Everything below Linux/lavapipe is measured on this machine. Everything about MoltenVK is
either quoted from `docs/porting/moltenvk-findings.md` (measured previously in CI) or written
blind and marked as such: **the author has no Mac, and the only evidence for the macOS path is
the `Renderer spike (macOS arm64, MoltenVK)` CI job.**

## 1. Counts, re-derived

Measured by `spikes/renderer/tools/d3d8-lock-scan.py` (added in this slice) over `Core/` +
`GeneralsMD/`, excluding `Generals/` and `*/Tools/*`, i.e. the same scope as the two existing
scanners:

| | Count |
|---|---:|
| D3D8 resource-interface call sites | **226** |
| ...files containing them | **19** |
| reference counting (`AddRef` / `Release`) | **72** |
| real resource operations | **154** |
| ...of which `Lock*` / `Unlock*` | **95** (53 lock, 42 unlock) |

Per interface:

| Interface | Sites | Methods |
|---|---:|---|
| `IDirect3DSurface8` | 88 | `Release` 36, `LockRect` 17, `UnlockRect` 17, `GetDesc` 14, `AddRef` 4 |
| `IDirect3DTexture8` | 47 | `GetSurfaceLevel` 19, `GetLevelCount` 7, `Release` 7, `SetLOD` 4, `GetLevelDesc` 3, `LockRect` 3, `UnlockRect` 3, `AddRef` 1 |
| `IDirect3DVertexBuffer8` | 32 | `Lock` 17, `Unlock` 11, `Release` 4 |
| `IDirect3DIndexBuffer8` | 26 | `Lock` 14, `Unlock` 9, `Release` 3 |
| `IDirect3DBaseTexture8` | 24 | `Release` 10, `GetLevelCount` 7, `AddRef` 5, `GetPriority` 1, `SetPriority` 1 |
| `IDirect3DVolumeTexture8` | 5 | `Release` 2, `GetLevelDesc` 1, `LockBox` 1, `UnlockBox` 1 |
| `IDirect3DCubeTexture8` | 3 | `GetLevelDesc` 1, `LockRect` 1, `UnlockRect` 1 |
| `IDirect3DSwapChain8` | 1 | `GetBackBuffer` 1 |

Per operation group:

| Group | Sites |
|---|---:|
| reference counting | 72 |
| lock | 53 |
| unlock | 42 |
| describe (`GetDesc`, `GetLevelDesc`, `GetLevelCount`) | 33 |
| sub-resource (`GetSurfaceLevel`, `GetBackBuffer`) | 20 |
| residency (`SetLOD`, `SetPriority`, `GetPriority`) | 6 |

Note what is *absent*: no `GetContainer`, no `AddDirtyRect`/`AddDirtyBox`, and no cross-resource
copy. `CopyRects` and `UpdateTexture` do exist in the engine — `textureloader.cpp:508` is
`DX8CALL(UpdateTexture(sysmem_texture, d3d_texture))` — but they are *device* methods, already
wrapped by `D3D8RenderBackendClass::UpdateTexture`/`::CopyRects` in
`WW3D2/d3d8renderbackend.cpp`, so they belong to the device surface `renderer-seam.md` replaced,
not to this seam. That matters for the design: **every one of the 154 operations touches exactly
one resource.**

Per file:

| File | Sites |
|---|---:|
| `WW3D2/texture.cpp` | 37 |
| `WW3D2/dx8wrapper.cpp` | 25 |
| `WW3D2/textureloader.cpp` | 25 |
| `W3DDevice/GameClient/TerrainTex.cpp` | 23 |
| `WW3D2/surfaceclass.cpp` | 20 |
| `W3DDevice/GameClient/Shadow/W3DProjectedShadow.cpp` | 20 |
| `W3DDevice/GameClient/Shadow/W3DVolumetricShadow.cpp` | 14 |
| `WW3D2/missingtexture.cpp` | 11 |
| `W3DDevice/GameClient/W3DTreeBuffer.cpp` | 7 |
| `W3DDevice/GameClient/Water/W3DWater.cpp` | 7 |
| `WW3D2/dx8indexbuffer.cpp` | 7 |
| `WW3D2/dx8vertexbuffer.cpp` | 7 |
| `W3DDevice/GameClient/W3DProfilerFrameCapture.cpp` | 6 |
| `W3DDevice/GameClient/W3DSmudge.cpp` | 4 |
| `W3DDevice/GameClient/W3DShroud.cpp` | 4 |
| `W3DDevice/GameClient/W3DSnow.cpp` | 3 |
| `WW3D2/ddsfile.cpp` | 3 |
| `W3DDevice/GameClient/W3DShaderManager.cpp` | 2 |
| `WW3D2/assetmgr.cpp` | 1 |

### 1.1 Why 226/72/154/95 and not 213/71/142/88

The difference is entirely in the scanners, not in the engine: no engine code changed in this
slice. `d3d8-resource-scan.py` still prints 213 and is left alone, because
`.github/workflows/native-port-ci.yml` and `renderer-seam.md` are calibrated against it. The
new scanner reports both numbers on every run so the gap stays visible.

Site by site, 213 → 226 is:

**−5 sites the old scanner counts that are not calls.** `dx8vertexbuffer.cpp` lines 170, 200,
235, 267 and 403 are inside `WWDEBUG_SAY(("VertexBuffer->Lock(start_index: %d, ...)"))` and
`WWDEBUG_SAY(("VertexBuffer->Release()"))` under `#ifdef VERTEX_BUFFER_LOG`. They are string
literals that happen to spell a call. `d3d8-resource-scan.py` strips comments but not literals;
the new scanner blanks literal contents before matching. The real calls in that file (177, 203,
243, 269, 407, 852, 882) are counted by both.

**+5 calls split across lines.** `W3DWater.cpp:678` and `:735`, `textureloader.cpp:1640`,
`:2013` and `:2329` are written as

```cpp
	if (FAILED(hr=m_vertexBufferD3D->Lock
	(
		0,
		m_numVertices*sizeof(SEA_PATCH_VERTEX),
		(BYTE**)&pVertices,
		0//D3DLOCK_DISCARD
	)))
```

or, in the texture loader, with the arguments one per line. A line-at-a-time regex cannot see
them. Three of the five are the texture loader's `Lock_Surfaces` overrides — the most important
lock class in the engine (C4 below), invisible to the old count.

**+6 calls the old scanner's comment stripping deletes.** `W3DWater.cpp:2186` is

```cpp
//	DX8Wrapper::Set_Shader(ShaderClass::/*_PresetAdditiveShader*//*_PresetOpaqueShader*/_PresetAlphaShader);
```

A `/\*.*?\*/` pass that runs before line-comment removal starts a "block comment" at the `/*`
inside that `//` comment and swallows real code up to the next `*/`, which in this file reaches
past line 2376. `W3DTreeBuffer.cpp` has the same shape. That hides `W3DWater.cpp`'s dynamic
water-mesh locks (2303, 2308, 2376) and `W3DTreeBuffer.cpp`'s `UnlockRect`/`Release`/`SetLOD`
(190, 191, 194). The new scanner matches literals and both comment forms in one alternation, in
source order, so neither can hide inside the other.

**+7 calls through locally declared names.** `missingtexture.cpp` calls `tex->LockRect` etc. on
a local `IDirect3DTexture8 *tex`, and `dx8wrapper.cpp:2514` calls `surface->GetDesc` on a
parameter named `surface`. The old scanner keeps one global name→interface table and drops
names like `tex`, `surface`, `texture` as too generic to attribute. The new scanner resolves
declarations per file first and only falls back to the global table, which recovers 6 sites in
`missingtexture.cpp` and 1 in `dx8wrapper.cpp`.

`213 − 5 + 5 + 6 + 7 = 226`.

Two heuristics remain, and are printed by the scanner rather than hidden:

- 10 sites in `texture.cpp` are attributed to `IDirect3DBaseTexture8` by the new scanner and to
  `IDirect3DTexture8` by the old one. Both are defensible — the variable is declared
  `IDirect3DBaseTexture8*` and the calls are `GetLevelCount`/`AddRef`, which
  `IDirect3DTexture8` inherits. This shifts the per-interface table but not the total.
- **11 names are left unattributed** because one file declares the same identifier both as a
  D3D8 pointer and as something else: `surface` and `tex` in `texture.cpp`, `texture` and
  `depth_buffer` in `dx8wrapper.cpp`, `Textures`/`index_buffer`/`texture` in `dx8wrapper.h`,
  `VertexBuffer` in `dx8vertexbuffer.h`, `index_buffer` in `dx8indexbuffer.h`, `surface` in
  `W3DSmudge.cpp`, `tex` in `textureloader.cpp`. Attributing those would be a guess, so the
  226 is a lower bound. It is not a large hole — the affected files' calls are reached through
  other, unambiguous names — but it is a hole.

## 2. What the lock sites actually do

`d3d8-lock-scan.py` records, per lock site, the flags argument, the rect/box argument, the mip
level, whether anything reads the pitch nearby, whether the returned pointer is stored rather
than consumed in place, and the enclosing function. Over the 53 lock sites:

| Flags argument as written | Sites |
|---|---:|
| `0` | 21 |
| `D3DLOCK_DISCARD` | 11 |
| `D3DLOCK_NOOVERWRITE` | 11 |
| `D3DLOCK_READONLY` | 4 |
| `flags` (a parameter: `VertexBufferClass::WriteLockClass`, `IndexBufferClass::WriteLockClass`) | 2 |
| `!DynamicVBAccess->VertexBufferOffset ? D3DLOCK_DISCARD : D3DLOCK_NOOVERWRITE` (+`NOSYSLOCK`) | 1 |
| `!DynamicIBAccess->IndexBufferOffset ? D3DLOCK_DISCARD : D3DLOCK_NOOVERWRITE` | 1 |
| `m_dwBase ? D3DLOCK_NOOVERWRITE : D3DLOCK_DISCARD` | 1 |
| `D3DLOCK_NO_DIRTY_UPDATE` | 1 |

| Extent | Sites |
|---|---:|
| byte range (`Lock(offset, size, ...)` on a vertex/index buffer) | 31 |
| whole surface / level (`pRect == nullptr`) | 17 |
| explicit sub-rect | 5 |

16 of the 53 read `Pitch`/`RowPitch`/`SlicePitch`; 8 store the returned pointer somewhere that
outlives the statement. **`D3DLOCK_WRITEONLY` does not exist in D3D8** (it is a D3D9 addition),
so the 21 `0`-flag locks are, as far as the API is concerned, read-write locks: the distinction
between "the caller only writes" and "the caller may read" is not in the flags and had to be
established by reading each call site. That is the single most consequential fact in this
document, because a read-write mapping of a `VK_IMAGE_TILING_OPTIMAL` image is not a thing that
exists.

## 3. The eight usage classes

`spikes/renderer/tools/d3d8-lock-classes.json` assigns every one of the 95 sites to a class,
keyed by file and enclosing function; `d3d8-lock-scan.py --check` fails if a site is
unclassified or if the totals drift, and runs in CI. Counts below are lock *and* unlock sites.

| Class | Sites | What the caller does |
|---|---:|---|
| **C1** whole-surface write | 18 | lock a whole surface or level with no flags, fill every texel, unlock. Nothing is read. |
| **C2** partial-rect write | 4 | as C1 but a sub-rect; texels outside it must survive. |
| **C3** read-back | 9 | `D3DLOCK_READONLY`, read texels on the CPU, unlock. Must observe what the GPU last wrote. |
| **C4** mip chain locked across frames *and* threads | 8 | lock every level/face/slice at once, keep the pointers and pitches, fill them later on another thread, unlock them all later still. |
| **C5** dynamic ring stream | 39 | per frame, many times per frame: `DISCARD` at offset 0 or `NOOVERWRITE` to append, write, unlock, draw. |
| **C6** static buffer fill | 12 | lock a static vertex/index buffer once at load time, write geometry, unlock, draw from it all level. |
| **C7** pointer kept after unlock | 2 | lock, unlock immediately, keep using the returned `pBits`/`Pitch` for the rest of the process. |
| **C8** opaque hand-out | 3 | `SurfaceClass::Lock` returns the pointer and pitch to arbitrary engine code, which may read, write, or both. |

**C1 — whole-surface write (18).** `TerrainTextureClass::update` (`TerrainTex.cpp:110`, `:230`),
`TerrainTextureClass::updateFlat` (`:391`), `AlphaEdgeTextureClass::update` (`:778`),
`W3DTreeBuffer::W3DTreeTextureClass::update` (`W3DTreeBuffer.cpp:141`), `MissingTexture::_Init`
(`missingtexture.cpp:79`, which passes a rect covering the whole level),
`SurfaceClass::Clear` (`surfaceclass.cpp:270`),
`TextureLoader::Load_Surface_Immediate` (`textureloader.cpp:605`),
`DDSFileClass::Copy_Level_To_Surface` (`ddsfile.cpp:346`). Cheapest class: the upload can happen
entirely at unlock and the staging memory can be returned to a pool immediately afterwards.

**C2 — partial-rect write (4).** The two `SurfaceClass::Copy` overloads and their unlocks:
`surfaceclass.cpp:309` locks the whole surface and `:353` locks `(min,max)`, and both walk rows
by `lock_rect.Pitch` while reading a tightly packed source array — so this class is where the
pitch contract is load-bearing. Vulkan's `vkCmdCopyBufferToImage` takes an
`imageOffset`/`imageExtent`, so a partial rect is directly expressible, provided the staging
allocation is laid out as a full level and the copy is issued for the sub-rect only.

**C3 — read-back (9).** `W3DSmudge.cpp:174` (`copyRect`), `SurfaceClass::CreateCopy`
(`surfaceclass.cpp:399`), `SurfaceClass::FindBB` (`:566`), `SurfaceClass::Is_Transparent_Column`
(`:644`). `FindBB` and `Is_Transparent_Column` are asset-load-time queries on system-memory
surfaces; `W3DSmudge::copyRect` is per-frame and reads the *back buffer*. That one is the
expensive one, and it is expensive in D3D8 too.

**C4 — mip chain locked across frames and threads (8).** `TextureLoadTaskClass::Lock_Surfaces`
(`textureloader.cpp:1640`), `CubeTextureLoadTaskClass::Lock_Surfaces` (`:2013`),
`VolumeTextureLoadTaskClass::Lock_Surfaces` (`:2329`), `TextureLoader::Load_Thumbnail`
(`:460`), and the matching `Unlock_Surfaces`. The lifetime is the point:

```cpp
// textureloader.cpp:1631 — every level locked at once, pointers kept
void TextureLoadTaskClass::Lock_Surfaces() {
    MipLevelCount = D3DTexture->GetLevelCount();
    for (unsigned i = 0; i < MipLevelCount; ++i) {
        D3DLOCKED_RECT locked_rect;
        Peek_D3D_Texture()->LockRect(i, &locked_rect, nullptr, 0);
        LockedSurfacePtr[i]   = (unsigned char *)locked_rect.pBits;
        LockedSurfacePitch[i] = locked_rect.Pitch;
    }
}
```

`Lock_Surfaces` runs from `Begin_Load`, the pixels are filled by `Load()` on the background
loader thread, and `Unlock_Surfaces` runs from `End_Load` on the DX8 thread. So the mapping must
survive an arbitrary number of frames, be writable from a non-render thread, and the whole mip
chain must be mapped simultaneously. Any design that maps at `Lock` and unmaps at `Unlock`
using a per-frame or ring allocator is wrong for this class, which is 8 sites and every
streamed texture in the game.

**C5 — dynamic ring stream (39).** `DynamicVBAccessClass::WriteLockClass` /
`DynamicIBAccessClass::WriteLockClass` (`dx8vertexbuffer.cpp:852`, `dx8indexbuffer.cpp:434`),
plus the shadow, water and snow systems streaming through their own D3D8 buffers:
`W3DProjectedShadow.cpp` (`:391`/`:397`, `:424`/`:430`, `:996`/`:1005`, `:1069`/`:1078`,
`:1166`/`:1175`, `:1228`/`:1237`), `W3DVolumetricShadow.cpp` (`:1412`/`:1418`, `:1442`/`:1448`,
`:1559`/`:1565`, `:1586`/`:1592`), `W3DWater.cpp:2303`/`:2308`,
`W3DSnowManager::renderSubBox` (`W3DSnow.cpp:267`). Every one is the same D3D8 idiom, a
`DISCARD`-or-`NOOVERWRITE` pair chosen by whether the buffer has room left:

```cpp
// W3DVolumetricShadow.cpp:1410
if (nShadowVertsInBuf > (SHADOW_VERTEX_SIZE-numVerts)) { // no room: rename
    shadowVertexBufferD3D->Lock(0, numVerts*sizeof(V), &pvVertices, D3DLOCK_DISCARD);
    nShadowVertsInBuf = 0;
} else {                                                 // room: append
    shadowVertexBufferD3D->Lock(nShadowVertsInBuf*sizeof(V), numVerts*sizeof(V),
                                &pvVertices, D3DLOCK_NOOVERWRITE);
}
```

`W3DSnow.cpp:267` writes it as one call with the flag chosen inline
(`m_dwBase ? D3DLOCK_NOOVERWRITE : D3DLOCK_DISCARD`), which is the same thing.

The buffers, and therefore the amount of memory the flags govern: `SHADOW_VERTEX_SIZE` 4096
verts × 16 B = 64 KiB plus `SHADOW_INDEX_SIZE` 8192 × 2 B = 16 KiB;
`SHADOW_DECAL_VERTEX_SIZE` 32768 × 24 B = 768 KiB plus `SHADOW_DECAL_INDEX_SIZE` 65536 × 2 B =
128 KiB (`W3DVolumetricShadow.cpp:116`–`117`, `W3DProjectedShadow.cpp:108`–`109`);
`DEFAULT_VB_SIZE`/`DEFAULT_IB_SIZE` 5000 elements each for WW3D2's shared dynamic buffers
(`dx8vertexbuffer.cpp:50`, `dx8indexbuffer.cpp:48`), which grow on demand. ≈ 1 MiB of dynamic
buffer, relocked several times per frame.

**C6 — static buffer fill (12).** `WaterRenderObjClass::generateVertexBuffer`
(`W3DWater.cpp:678`, one of the multiline calls the old scanner could not see) and
`generateIndexBuffer` (`:735`),
`VertexBufferClass::WriteLockClass`/`AppendLockClass` (`dx8vertexbuffer.cpp:177`, `:243`),
`IndexBufferClass::WriteLockClass`/`AppendLockClass` (`dx8indexbuffer.cpp:194`, `:245`) and
their unlocks. C1 without the image: one write at load, then read-only for the level. Note the
two `WriteLockClass` sites pass `flags` through from the caller, so their *effective* flags are
whatever the ~200 `WriteLockClass` users ask for — a hole the scanner cannot close and neither
can this document.

**C7 — pointer kept after unlock (2).** `W3DShroud::init` (`W3DShroud.cpp:170`):

```cpp
HRESULT res = m_pSrcTexture->LockRect(&rect, nullptr, D3DLOCK_NO_DIRTY_UPDATE);
m_pSrcTexture->UnlockRect();
m_srcTextureData  = rect.pBits;     // kept for the lifetime of the shroud
m_srcTexturePitch = rect.Pitch;
memset(m_srcTextureData, 0, m_srcTexturePitch*srcHeight);
```

Using `pBits` after `UnlockRect` is undefined in D3D8. It works because the surface comes from
`DX8Wrapper::_Create_DX8_Surface` → `CreateImageSurface` (`dx8wrapper.cpp:2838`), i.e. a
system-memory image surface whose bits the runtime does not move, and the shroud then reads and
writes `m_srcTextureData` every frame (`W3DShroud.cpp:269`, `:309`, `:331`, `:394`, `:452`,
`:557`, `:573`) and pushes the result with `DX8Wrapper::_Copy_DX8_Rects` (`:483`, `:494`, `:714`),
which is the device-level `CopyRects`.
Reproducing this needs the mapping to be permanent, which — see §4 — the recommended design
gives it anyway, so **C7 keeps working without touching `W3DShroud`**. That is the strongest
single argument for the persistent-mapping design over a transient one.

**C8 — opaque hand-out (3).** The two `SurfaceClass::Lock` overloads (`surfaceclass.cpp:219`,
`:234`) and `SurfaceClass::Unlock` (`:242`):

```cpp
SurfaceClass::LockedSurfacePtr SurfaceClass::Lock(int *pitch) {
    D3DLOCKED_RECT lock_rect;
    DX8_ErrorCode(D3DSurface->LockRect(&lock_rect, nullptr, 0));
    *pitch = lock_rect.Pitch;
    return static_cast<LockedSurfacePtr>(lock_rect.pBits);
}
```

The pointer and pitch go to callers all over the engine (`W3DWater.cpp:471`/`:548`/`:981`/
`:1130` among them) with no indication of intent, and the flags argument is `0` — read-write.
C8 therefore requires a mapping that can be read as well as written, unless every caller is
audited: 3 sites in the seam, an unbounded number outside it.

## 4. The design, and what it costs

The interfaces stay as they are. `DX8Wrapper` keeps handing out resource pointers; the Vulkan
backend implements the resource objects behind them. Concretely, per lockable resource:

- **one host-visible, host-coherent, mapped buffer sized for the whole mip chain**, laid out
  level by level, tightly packed, `Pitch = level_width × source_texel_bytes`. It is held for as
  long as any level of the resource is locked — and, for C7 and C8, for the resource's lifetime.
  This is what makes C4 (all levels mapped at once, across threads and frames), C7 (pointer
  valid after unlock) and C8 (read-write) work without changing a line of engine code. It comes
  from the pool of §4.1 rather than from a per-resource allocation; that is invisible to callers.
- **a device-local `VK_IMAGE_TILING_OPTIMAL` image** with `TRANSFER_DST | TRANSFER_SRC |
  SAMPLED` usage. `TRANSFER_SRC` exists only for C3.
- `Lock(level, rect, flags)` returns `mapped + level_offset + top×pitch + left×texel_bytes` and
  the level's pitch — the same arithmetic D3D8 documents for a sub-rect lock.
- `Unlock(level)` issues one `vkCmdCopyBufferToImage` for the locked rectangle only, with the
  layout transitions around it, and nothing else.

So the D3D8 contract costs, per *lock*: **a pooled host-visible block the size of the mip chain,
held until the last level is unlocked** (a second one, transiently, for the CPU-expanded
formats, see §4.3), and **1 buffer→image copy + 2 layout transitions per unlock**. Per
*read-only* lock it costs an image→buffer copy, a queue submit and a fence wait, i.e. a full
CPU/GPU sync point. §4.1 has what the pool costs across a frame's worth of locks.

Measured, by `zh-resource-lock-tests` on lavapipe, for the nine cases in §5 — which now include
two cross-thread cases and a recycling case, so 79 acquires over the run:

```
staging blocks the pool ever allocated:               1 (4096 bytes)
dynamic vertex-buffer memory (not poolable):          1 (672 bytes)
staging peak checked out at once:                    4096 bytes in 1 block(s)
staging still checked out now:                       0 bytes
pool: 1 free block(s), 4096 bytes, 78/79 acquires reused, 0 pinned
buffer-to-image copy regions issued from Unlock:     86
queue submits caused by locks:                       103
read-back stalls (submit + fence wait inside Lock):  1
CPU channel expansions at unlock:                    0
dynamic ring: 2 DISCARD, 1 NOOVERWRITE, 336 bytes, 0 wrap stalls
```

One 4 KiB block serves all 79 lock acquisitions in the suite. With `ZH_SPIKE_NO_VIEW_SWIZZLE=1`,
which is MoltenVK's case: 2 blocks / 8192 bytes and 1 CPU expansion, the second block being the
expansion buffer for the 16×16 L8 texture, taken from the same pool and returned at unlock.

### 4.1 Memory: pooled, and measured

The per-resource rule *was*: **staging bytes = the D3D8 texture's own byte size**, ×2 for a
format that needs CPU expansion where the view cannot swizzle, held from creation to
destruction. For a 256×256 A8R8G8B8 texture with a full mip chain that is 349 KiB per texture,
resident whether or not anything is locked. That is what the pool replaces.

**Pool policy**, in `vulkan_backend.cpp`, entirely behind the unchanged lock/unlock signatures:

- staging is a **pool of mapped host-visible blocks in power-of-two size classes from 4 KiB**
  up. `Lock` takes the smallest free block that fits the whole mip chain of the resource being
  locked and zeroes the requested bytes; `Unlock` returns it to the free list once the last
  locked level of that resource is unlocked. Nothing is unmapped or freed until shutdown, so a
  reused block costs no `vkAllocateMemory` and no `vkMapMemory`.
- **one block per resource, not per level**: a C4 caller locks every level at once and keeps the
  pointers, so the block has to cover the chain and stay checked out until the final unlock.
  That is what makes the pool safe for C4 without the caller noticing.
- **C7/C8 resources keep their block for their lifetime**, because their callers may read after
  unlock. They are the two classes the pool cannot help, and they are the whole steady-state
  cost below.
- the CPU-expansion path (no image-view swizzle, MoltenVK's case) takes a *second* block from
  the same pool for the duration of one unlock and returns it immediately, instead of holding a
  second per-resource allocation.
- `ZH_SPIKE_STAGING_RETAIN=1` restores the old per-resource-lifetime behaviour. It exists so the
  before/after below is one binary and one workload, and so CI can assert that the pre-pool cost
  actually breaks the committed ceiling (§4.1.2).

#### 4.1.1 What a frame's worth of locks costs

`spikes/renderer/src/staging_workload.cpp` (`zh-staging-workload`) drives the pool with the
class mix §3 measured, not with a best case: per frame 18 C1 whole-level writes, 4 C2 sub-rect
writes, 9 C3 read-back locks, 8 C4 levels across a held mip chain and 39 C5 ring locks, plus 12
C6 static fills once at load and 5 retained C7/C8-like surfaces held for the run. 12 frames,
**653 texture locks and 480 buffer locks**, ending in a rendered read-back that must match the
pattern the pool handed out (`pixels_ok`), with the validation layer loaded and silent.

Measured on Linux/lavapipe (`llvmpipe`, LLVM 20.1.2), same binary, same workload:

| | before (per-resource lifetime) | after (pooled) |
|---|---:|---:|
| host-visible staging allocations | 17 | **8** |
| resident staging bytes (allocated, never returned before shutdown) | 2,854,912 | **1,638,400** |
| peak bytes checked out at once | 2,854,912 | **1,638,400** |
| steady state: bytes still checked out at a frame boundary | 2,854,912 | **327,680** |
| block reuse rate | 0.0% (17 acquires, 0 reused) | **98.3%** (461 acquires, 453 reused) |
| allocations per texture lock | 0.026 | **0.012** |

and with `ZH_SPIKE_NO_VIEW_SWIZZLE=1`, i.e. MoltenVK's expansion path forced on: 18 → **9**
allocations, 2,969,600 → **1,703,936** resident bytes, 2,904,064 → **327,680** steady state,
reuse 98.2% (497 acquires, 488 reused), 36 CPU expansions. Both modes: 0 validation messages,
`pixels_ok`.

Read those numbers precisely:

- **steady state** is what is still *checked out* when a frame ends: in pooled mode the five
  retained C7/C8-like surfaces and nothing else, 327,680 bytes, i.e. **8.7× less pinned memory
  than the per-resource design**, and it does not grow with the number of transient resources.
  This is the figure §7.3 called unknown.
- **resident** is what the process still owns: checked-out blocks plus the free list, because a
  returned block stays allocated and mapped for reuse. 1,638,400 bytes here — 8 blocks, the
  largest being the 256×256 mip chain a C4 loader holds — against 2,854,912. The pool trades
  fragmentation for allocation calls deliberately: fewer, bigger, recycled blocks.
- **allocations stop**: 8 allocations serve 461 acquires. A regression that reintroduces
  per-lock allocation shows up as the allocation count tracking the lock count, which is what
  the gate keys on.
- **dynamic (C5) buffers are accounted separately** (16 allocations, 3,912,000 bytes here) and
  are deliberately *not* pooled: they are the ring of §4.2, host-visible by design.
- this is 12 frames of a *representative mix at spike scale*, not the game's resident set. The
  game still does not run on this path; §7.4 stands.

#### 4.1.2 The gate

`scripts/ci/check-staging-cost.py` runs the workload in both swizzle modes and fails if
allocations, resident bytes, peak bytes or steady-state bytes exceed
`docs/porting/ci-baselines/staging-cost-ceiling.json` (the measured figures + 25% on bytes, +2
on allocation counts), if allocations per texture lock exceed 0.05, if the reuse rate drops
below 0.90, if `pixels_ok` is false, or if the validation layer says anything. `--self-check`,
which is what CI runs, additionally runs the *pre-pool* mode and requires that it **violates**
that ceiling — so the gate is known to catch the regression it exists for rather than merely to
pass. It does, on all four byte/allocation limits.

Which classes give memory back, restated against the implementation: C1, C2, C3 and C6 return
their block at unlock (the 98% reuse is mostly them); C4 holds one block per in-flight load
task, bounded by the loader's task count rather than the texture set; C7 (1 surface, the shroud)
and C8 (every `SurfaceClass` the engine keeps) still hold for the resource's lifetime, and C8 is
the unbounded one, because `SurfaceClass::Lock` cannot say whether its callers read (§7.1).

The dynamic (C5) buffers are different: they are already host-visible in D3D8, and the extra
cost is only the renaming copies. With 3 regions per buffer (§4.2), the ≈1 MiB of dynamic
buffers in §3 becomes ≈3 MiB, i.e. **+2 MiB of host-visible memory for the whole engine**, no
copies and no submits.

### 4.2 The lock flags, one by one

| D3D8 | Vulkan realisation |
|---|---|
| `D3DLOCK_DISCARD` | rename: advance to the next region of a ring of `kDynamicRingRegions = 3` copies of the buffer (frames-in-flight + 1) and hand out its base. The draw path applies the region's byte offset when binding, so the engine's vertex indices do not change. If the ring wraps onto a region a submitted frame may still read, the promise "DISCARD never blocks" cannot be kept and the honest answer is to grow the ring; the spike counts the event (`ring_wrap_waits`) and, having only one frame in flight, waits on that frame's fence. |
| `D3DLOCK_NOOVERWRITE` | append within the current region. Nothing to do beyond not renaming — this is exactly what a persistently mapped host-visible buffer already provides, and it is the one class that is free. |
| `0` (no flags) on a buffer | treated as C6: write into the region, no rename. Correct only because no C6 site relocks a buffer the GPU is still reading; a `WriteLockClass` user that did would need a fence wait. |
| `D3DLOCK_READONLY` | `vkCmdCopyImageToBuffer` for the locked level, submit, `vkWaitForFences`, then hand out the pointer. Unavoidably a stall. |
| `D3DLOCK_NO_DIRTY_UPDATE` | ignored. It is a hint about `UpdateTexture`'s dirty-region tracking; the seam has no `UpdateTexture` (§1) and always copies the region the caller locked. |
| `D3DLOCK_NOSYSLOCK` | ignored. It is about the Win32 critical section D3D8 held during a lock. |
| no flags at all on a texture/surface | must be assumed read-write (C8), because D3D8 has no `WRITEONLY`. A retained staging copy is what makes that assumption cheap: reads hit the staging bytes, which hold whatever was last written through them, so a C8 resource keeps its pooled block for its lifetime (§4.1). **This is a real semantic gap: a C8 caller that reads a surface the GPU wrote (a render target) would see the staging copy, not the GPU's result.** No C8 caller in the 19 files does that; the callers outside them are now counted — 21 sites, 18 of which read, 5 of which read a surface the GPU can have written (§7.1). |

### 4.3 MoltenVK

Two constraints from `docs/porting/moltenvk-findings.md`, both already handled by the spike's
creation-time upload path and now by the lock path as well:

- **No image-view swizzle.** `imageViewFormatSwizzle` is `VK_FALSE` in MoltenVK's portability
  subset, because Metal has no equivalent. D3D8's `L8` → (L,L,L,1), `A8` → (0,0,0,A), `A8L8` →
  (L,L,L,A) and `X8R8G8B8` → alpha-forced-to-1 expansions therefore have to happen on the CPU.
  For a *lock*, that means `Unlock` expands the locked rectangle, row by row, from the staging
  buffer the caller wrote into a second host-visible buffer that the image is copied from. The
  caller still writes D3D8's layout and pitch and does not know the difference. Cost: a second
  allocation of `width × height × 4` per level, plus one CPU pass over the unlocked rectangle
  per unlock. Exercised on Linux by `ZH_SPIKE_NO_VIEW_SWIZZLE=1`, which forces the same path.
- **No `D3DFMT_D24S8`.** Irrelevant here; no lock site touches a depth surface.

**Everything in this section about MoltenVK behaviour is inference from the previously measured
capability flags plus the Vulkan spec, not observation.** The lock path has never run on a Mac
at the time of writing; the `Renderer spike (macOS arm64, MoltenVK)` job in
`.github/workflows/native-port-ci.yml` now runs `zh-resource-lock-tests` there, and that job's
output is the only evidence that will exist.

## 5. What the spike proves

`spikes/renderer/src/resource_lock_tests.cpp` (`zh-resource-lock-tests`) implements the design
against the real backend and asserts on read-back pixels and read-back bytes, not on log output.
`render_backend.h` grew `Create_Lockable_Texture`, `Lock_Texture`, `Unlock_Texture`,
`Create_Dynamic_Vertex_Buffer`, `Lock_Vertex_Buffer`, `Unlock_Vertex_Buffer` and
`Get_Resource_Stats`; `vulkan_backend.cpp` implements them.

| Case | Class | What is asserted |
|---|---|---|
| whole-surface write | C1 | 16×16 A8R8G8B8 filled through the returned pitch with a per-texel distinct pattern; all 256 texels sampled back and compared individually |
| partial-rect write | C2 | level filled, then rect (4,2)–(12,10) overwritten; all 256 texels checked — 64 replaced, 192 unchanged |
| read-back | C3 | pattern uploaded, staging deliberately scribbled over, pattern re-uploaded, staging scribbled again, then `LOCK_READONLY` must return the *image's* bytes — so a read-only lock that skipped the image→buffer copy fails |
| mip-chain lock | C4 | all 5 levels of a 16×16 texture locked simultaneously, filled afterwards from the kept pointers, unlocked, then level 2 sampled and identified by colour |
| dynamic ring stream | C5 | `DISCARD` then `NOOVERWRITE` in one frame, two draws from the two sub-ranges, both correct (so the append did not disturb the first range); then `DISCARD` in the next frame renames to a different region with no wrap stall |
| L8 lock | MoltenVK | a byte-per-texel L8 ramp written as L8 and sampled as (L,L,L,1), on both the swizzled and the CPU-expanded path |
| **cross-thread fill** | C4 | all 5 levels locked on the main thread, filled on a second thread through the kept `LockedRect` pointers, joined, unlocked on the original thread, level 2 sampled back — the loader's actual thread hand-off |
| **concurrent locks** | C4 | 4 threads × 8 lock/fill/unlock rounds on 4 textures against one shared pool; every texture must sample back its own thread's colour, so a block handed to two threads at once fails |
| **pool recycling** | C1/C2 | 6 textures × 6 rounds = 36 sequential locks must cost **0** new allocations after the first, end with 0 bytes checked out, and still sample back correctly |

All nine pass on lavapipe with zero validation messages, in both swizzle modes. Both modes run
in CI on Linux (`Renderer spike (Linux, lavapipe)`); the default mode runs on `macos-15`.
`zh-staging-workload` (§4.1.1) is the second pixel assertion: its final read-back must match the
pattern written through pooled blocks, so a pool that hands the same block to two live locks
fails on pixels rather than on a counter.

Not proven: C6 (static buffer fill — mechanically C1 without the image), C7 (needs the shroud's
`CopyRects` path) and C8 (needs a caller audit, not a test; §7.1 has the audit). Cross-thread C4
is now asserted, on lavapipe, with the mapping host-coherent and never remapped and the hand-off
ordered by a `std::thread` join — which is the loader's ordering, not weaker.

## 6. Effect on the Windows build

None. No engine file is modified by this slice: the changes are `docs/porting/`,
`spikes/renderer/` (a standalone CMake project that is not part of the game build),
`scripts/ci/check-staging-cost.py` and `.github/workflows/native-port-ci.yml`. The
`SurfaceClass::Lock` audit (§7.1) reads engine sources and changes none of them.
`d3d8-resource-scan.py`, which the existing CI ratchet uses, is untouched and still prints 213.

## 7. What is not resolved

1. **The C8 read hazard — audited, and it needs a decision this slice did not take.**
   `spikes/renderer/tools/surface-lock-audit.py` (`--check` in CI, counts committed in
   `surface-lock-audit.json`) counts every `SurfaceClass::Lock` caller: **26 sites, 5 inside the
   19 files that hold a direct D3D8 lock and 21 outside them**. Of the 21 outside, **18 read
   through the returned pointer** (mechanical upper bound: a null check or passing the pointer to
   a callee counts as a read), and **5 read a surface whose contents can have come from the
   GPU**:
   `W3DScreenshot.cpp:200` (`W3D_TakeCompressedScreenshot`), and `WW3D::Make_Screen_Shot` /
   `WW3D::Update_Movie_Capture` in both `Generals` and `GeneralsMD` `ww3d.cpp`. All 5 have the
   same shape: create a plain surface, `DX8Wrapper::_Copy_DX8_Rects` the back buffer into it,
   `Lock` with no flags, read every pixel. The other 13 read only bytes the CPU itself wrote
   (radar/shroud read-modify-write, `Blit_Char`, palette remap), which the staging copy serves
   correctly.
   **The decision, not taken here:** those 5 need an unflagged C8 lock on a blit destination to
   perform the C3 image→buffer read-back, and the seam cannot tell that case from an ordinary
   write-only C8 lock without either (a) making every C8 lock read back — a stall on locks that
   do not need one, (b) tracking "the GPU wrote this resource since the last lock" as a
   dirty bit on the resource, or (c) splitting `SurfaceClass::Lock` into read and write variants,
   an engine change. It also depends on `CopyRects`/blit, which is an unimplemented backend
   method owned by another slice, so the pool cannot close it alone. Raised, not chosen.
2. **`WriteLockClass`'s pass-through flags.** 2 lock sites take their flags from ~200 callers.
   Their effective flag distribution is unmeasured.
3. ~~**Staging pooling.**~~ Written and measured: §4.1. Pooled, recycled, 98% block reuse, and a
   CI ceiling that the pre-pool behaviour provably breaks. What remains open is the *policy* at
   game scale: the size classes are powers of two from 4 KiB with no trimming, so the resident
   figure is a high-water mark that never shrinks. Whether a running game wants trimming is
   unmeasurable until the game runs on this path.
4. **Still no number for the game's resident lockable-texture bytes.** §4.1.1 measures a
   representative *mix* at spike scale (653 texture locks over 12 frames), not the game's texture
   set: the game does not run on this path, so the per-frame counts come from the 95 measured
   lock sites and the resource sizes are the spike's own.
5. **The dynamic-ring wrap case.** `kDynamicRingRegions = 3` is derived from
   frames-in-flight + 1, not measured against the engine's worst-case per-frame dynamic usage.
   The spike counts wraps but never provoked one, so the "grow the ring" branch is untested.
6. ~~**C4 across threads.**~~ Now shown, on lavapipe: `C4 cross-thread fill` (lock on one
   thread, fill on another, unlock back on the first) and `C4 concurrent locks` (4 threads
   locking, filling and unlocking their own textures against one shared pool) both assert on
   sampled pixels, with the validation layer silent. The backend serialises lock/unlock and the
   pool's free list under one mutex; the mapping stays host-coherent and is never remapped, so
   the fill itself needs no synchronisation beyond the hand-off the loader already has. Two
   caveats: lavapipe is a software device and its coherency is trivially satisfied, and
   `Unlock` still submits from the calling thread on a single queue — an engine that unlocked
   from two threads at once would be serialised, not parallel, which matches D3D8's own
   single-threaded device but is worth knowing.
7. **`P8`.** `Create_Lockable_Texture` refuses paletted textures: expansion needs the palette at
   unlock time and a lockable P8 texture could have its palette replaced between locks. No lock
   site uses P8, so no policy was invented.
8. **`D3DLOCK_READONLY` on an expanded format** returns failure rather than contracting BGRA8
   back to L8/A8/A8L8. No engine site needs it; a Mac-only future one would.
9. **Everything about MoltenVK is unverified by the author.** The CPU-expansion-at-unlock path,
   the portability-subset assumption, and the whole macOS run are inference plus CI. There is no
   Mac on this end. Specifically for the pool: the `macos-15` job *prints* the workload's staging
   figures and does not gate on them, because the committed ceiling was measured against
   lavapipe's allocation behaviour and MoltenVK's will differ. The macOS runner is also a VM with
   a paravirtualised GPU, so nothing there is Apple Silicon hardware verification.
