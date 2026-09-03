# Renderer resource lifetime: the text path's leaked texture and surface, the contract it broke, the gate

Slice 3 of wave 10. `docs/porting/playability-probe.md` §1.1 measured, on the real M1 Pro, the
Vulkan backend's `owned_surfaces_` growing 268,686 → 405,299 and `owned_textures_` 134,523 → 202,830
in ~17 minutes *while the game was paused* (≈ +134 surfaces/s, +67 textures/s), RSS 82 → 227 MB in
30 s of an active mission, and every `SIGSTOP` backtrace inside the linear scan of
`spike::VulkanBackend::Get_Surface_Level` under `Render2DSentenceClass::Build_Textures`. This
document names the D3D8 ownership contract that path relies on, says exactly where the port stopped
honouring it, fixes it in the seam and the backend (not the engine), separates the *growth* defect
from the *scan* defect and measures each, and adds the regression gate with its negative control.

Every result carries one of **PORT DEFECT**, **UNIMPLEMENTED PATH**, **MISSING DATA**,
**SYNTHETIC-ONLY**. The enforced numbers come from `scripts/ci/check-resource-lifetime.py` on every
CI run; the real-game numbers in §5 were produced once, on Linux x86-64 under lavapipe, by
`scripts/native-resource-probe.py`, and are dated. The Apple Silicon rows are **UNMEASURED** in this
slice: the session that produced this document had no Mac and was told not to acquire one; the
M1 Pro confirmation of §5 is owed as a follow-up and the slice is not "done on the Mac" until it
exists. `docs/porting/ci-baselines/*.json` remain the source of truth for everything else quoted in
`docs/porting/`, and nothing in this file is.

## 0. Summary

- **The contract (§1):** every D3D8 resource is a COM object. `CreateTexture`/`CreateImageSurface`
  hand back one reference; `IDirect3DTexture8::GetSurfaceLevel` hands back an *AddRef'd* interface
  to a surface the texture owns; the caller `Release()`s every one of those, and the *device* frees
  the video memory when the last reference goes. The engine honours that contract to the letter in
  the text path (§1.2): `Render2DSentenceClass` releases the level surface, the texture and the
  system-memory image surface it made for each string. It does not leak on Windows, and no engine
  line changes in this slice.
- **What the port did (§2, PORT DEFECT / UNIMPLEMENTED PATH):** the seam's `Release()` reached zero
  correctly and deleted the *wrapper* — and then did nothing, because `spike::RenderBackend` had no
  per-resource destroy entry point at all. Every `TextureHandle`, `SurfaceHandle`,
  `VertexBufferHandle` and `IndexBufferHandle` stayed in the backend's `owned_*` vectors, with its
  `VkImage`/`VkBuffer`/`VkDeviceMemory`, until `Shutdown()`. The old seam even said so in a comment
  ("The spike owns the image until Shutdown … it is a leak of device memory across a long
  session"). So: not an extra backend reference, not a missing `AddRef`, not a `Release` that maps to
  nothing on the interface side — a **`Release`-at-zero that had nowhere to go**, i.e. the last of
  the four mechanisms the slice asked about: the engine relies on D3D8 freeing the resource when its
  count hits zero, and the backend did not reproduce that. Per text string per frame that is one
  texture and two surfaces (the texture's level 0 and the `D3DPOOL_SYSTEMMEM` image surface the
  glyphs are composed into), which is the 2:1 ratio the Mac measured.
- **The fix (§3):** `Destroy_Texture/Surface/Vertex_Buffer/Index_Buffer` on `spike::RenderBackend`,
  implemented in `VulkanBackend` as *deferred* destruction — the handle leaves the live set and its
  bound-texture, current-target and framebuffer-cache references are cleared immediately, the Vulkan
  objects are freed at the next `Begin_Scene` after the previous frame's fence — and the seam's four
  `Release()`s call them at zero. Level surfaces stay owned by their texture exactly as in D3D8
  (§1.1): `Destroy_Surface` on one is a no-op and the texture's destruction retires it.
- **The scan (§4), measured second and separately — and it was not a real cost once the leak was
  fixed.** `Get_Surface_Level` walked `owned_surfaces_` to find the texture's existing level-0
  surface: 265 → 4,905 ns/call over 300 leaking frames on the spike, and O(150 k) entries after
  20 minutes of the real game. With the leak fixed the list is bounded (443 surfaces in the real
  game, §5.1) and the walk measured 131–198 → 119–165 ns/call, flat. It was still replaced by a
  per-texture pointer (`TextureHandle::level0`) in its own commit — O(1), and `Note_Texture_Layout`
  had the same walk — which measures 90–168 → 132–136 ns/call, i.e. no measurable change. The
  commit is separable and is kept as a complexity fix, not claimed as a performance one. Neither
  fix hides the other: the gate asserts the live counts, not the scan cost, and the negative
  control still fails with the pointer in place.
- **Real game, Linux x86-64, retail Zero Hour 1.04 data, 20-minute skirmish, before and after
  (§5):** `owned_surfaces_` +18,832 (+15.7/s) → **+0** (443 at every one of 550 samples),
  `owned_textures_` +9,416 (+7.8/s) → **+0** (375 throughout), RSS +312.8 MB → **+0 bytes**
  (923,312,128 at first and last sample and at the maximum), over 20.0 minutes each; in the after
  run the backend created and destroyed 9,970 textures and 19,940 surfaces, exactly matched.
  Steady state, not a lower slope. Frame time on lavapipe is software-rasteriser time (median
  ~124 ms) and did not change measurably across 20 minutes in either run, so the Mac's 30 → 17 FPS
  decay over hours is **not reproduced or refuted here**; §5.3 says what it would take.
- **A correction to the numbers that found the leak (§5.0):** the first version of the Linux probe
  reported `owned_*` vector sizes in *bytes*, 8× the element count. The `playability-probe.md`
  §1.1 figures (268,686 → 405,299) were read by hand through LLDB with a method that document does
  not record; their 2 : 1 ratio and their slope are right, their magnitude may carry the same 8×
  and should be re-read on the Mac. Every count in this document is in elements.
- **Gate (§6):** `zh-resource-lifetime` + `scripts/ci/check-resource-lifetime.py`, Linux/lavapipe
  and macOS/MoltenVK CI. 300 frames × 8 text strings, every texel of every drawn string read back
  and verified, live counts asserted back at their bound, retired set empty after an idle frame,
  `Get_Surface_Level` cost bounded, validation layer loaded and silent. Negative control
  `ZH_RENDER_NO_RESOURCE_DESTROY=1` runs inside the checker and must fail *for the growth*
  (2,400 textures / 4,800 surfaces above bound) with every texel still verifying.
- **Remaining leaks and costs found but not fixed (§7), ranked with evidence.**

## 1. The contract

### 1.1 What D3D8 promises

The D3D8 SDK documentation (`IDirect3DTexture8::GetSurfaceLevel`, `IDirect3DDevice8::CreateTexture`,
`IDirect3DDevice8::CreateImageSurface`, and the `IUnknown` rules every `IDirect3D*8` interface
inherits) fixes the following, and the engine was written against it:

1. **Creation returns one reference.** `CreateTexture(..., &tex)` and `CreateImageSurface(..., &surf)`
   return an interface with reference count 1 that the caller owns.
2. **`GetSurfaceLevel` returns an AddRef'd interface.** The documentation's words: *"Calling this
   method will increase the internal reference count on the `IDirect3DSurface8` interface. Failure
   to call `IUnknown::Release` when finished using this `IDirect3DSurface8` will result in a memory
   leak."* The caller releases what it was handed. It does not release the texture on the surface's
   behalf.
3. **The level surface is owned by the texture.** A level surface is not a resource of its own: it
   has no `Pool` of its own, cannot be created standalone, and its `GetContainer` (D3D9's spelling;
   D3D8 exposes the same ownership through `GetDevice`/`GetDesc`) is the texture. On the Microsoft
   runtime a level surface's `AddRef`/`Release` are forwarded to the texture's count, so holding a
   level surface keeps its texture alive and releasing the last texture *or* surface reference
   frees the whole thing at once. This last sentence is observable behaviour of the D3D8/D3D9
   runtimes and is how D3D9's documentation describes surface levels ("the texture object owns its
   surfaces"); it is not in the D3D8 header, so it is stated here as the model the seam honours.
4. **The device frees the resource when the last reference goes.** Nothing else does. There is no
   "destroy texture" call in D3D8; `Release()` reaching zero *is* the destroy.

Consequences the engine leans on: it can `Release()` a texture while the device still has it bound
(the device holds its own reference: `SetTexture`'s documentation says it increments the
texture's count), it can `Release()` a surface that
is the current render target for the same reason, and it can create and release resources every
frame with no bookkeeping beyond matching every creation with a `Release`.

### 1.2 What the engine does with it — and it is correct

`Core/Libraries/Source/WWVegas/WW3D2/render2dsentence.cpp`, `Render2DSentenceClass::Build_Textures`
and `Allocate_New_Surface`/`Release_Pending_Surfaces`, unchanged here. Per string, per rebuild:

```cpp
// composition target, D3DPOOL_SYSTEMMEM, via CreateImageSurface
SurfaceClass *new_surface = W3DNEW SurfaceClass (desc.Width, desc.Height, WW3D_FORMAT_A4R4G4B4);
...
// the texture the string is drawn from
TextureClass *new_texture = W3DNEW TextureClass (desc.Width, desc.Width, WW3D_FORMAT_A4R4G4B4, MIP_LEVELS_1);
SurfaceClass *texture_surface = new_texture->Get_Surface_Level ();   // GetSurfaceLevel(0): AddRef'd
DX8Wrapper::_Copy_DX8_Rects (curr_surface->Peek_D3D_Surface (), nullptr, 0,
                             texture_surface->Peek_D3D_Surface (), nullptr);
REF_PTR_RELEASE (texture_surface);                                   // 1.1 rule 2: released
...
renderer->Set_Texture (new_texture);                                 // Render2DClass AddRefs
REF_PTR_RELEASE (new_texture);                                       // 1.1 rule 1: released
...
// Release_Pending_Surfaces(): the composition surfaces
REF_PTR_RELEASE (curr_surface);                                      // 1.1 rule 1: released
```

`SurfaceClass::~SurfaceClass` and `TextureClass::~TextureClass` (`surfaceclass.cpp`,
`texture.cpp`) `Release()` the `IDirect3DSurface8`/`IDirect3DTexture8` they wrap; `Render2DClass`
releases its texture when the sentence is next rebuilt or destroyed. Every creation in the path is
matched by a release. **On Windows this path does not leak**; Windows is the oracle and it did not
need re-measuring because there is no engine line whose behaviour is in question — the whole
mechanism sits below `Peek_D3D_Surface()`.

That the text renderer rebuilds its textures *every frame* for a static text gadget is a
cost (`Render2DSentenceClass::Build_Textures` is called from `W3DDisplayString::draw` whenever the
string is dirty, and the in-game UI dirties several per frame) but it is the engine's Windows
behaviour and out of this slice's scope; §7 ranks it.

## 2. Where the port broke it — PORT DEFECT (backend), UNIMPLEMENTED PATH (destroy)

Before this slice (`main` at `6e182ac34`), `Core/Libraries/Source/WWVegas/WW3D2/vulkanrenderbackend.cpp`:

```cpp
ULONG VulkanD3DTextureClass::Release()
{
	RefCount--;
	if (RefCount > 0) return (ULONG)RefCount;
	// The spike owns the image until Shutdown (it has no per-resource destroy entry point), so
	// this frees the wrapper only.  That is a real difference from D3D8 and it is a leak of
	// device memory across a long session ...
	delete this;
	return 0;
}
ULONG VulkanD3DSurfaceClass::Release()      // standalone (CreateImageSurface) surfaces
{
	RefCount--;
	if (RefCount > 0) return (ULONG)RefCount;
	if (Container == NULL) delete this;      // wrapper only; the SurfaceHandle stays in owned_surfaces_
	return 0;
}
```

and `spikes/renderer/src/render_backend.h` had `Create_Lockable_Texture`, `Create_Image_Surface`,
`Get_Surface_Level`, `Create_Vertex_Buffer`, `Create_Index_Buffer` — and no `Destroy_*` of any kind.
`VulkanBackend` pushed every created handle onto `owned_textures_`/`owned_surfaces_`/`owned_vbs_`/
`owned_ibs_` and freed them in `Shutdown()`.

So, against §1.1: rules 1 and 2 were honoured on the interface side (counts started at 1,
`GetSurfaceLevel` AddRef'd, the caller's `Release`s brought them to zero — the wrappers *were*
deleted, which is why the old code did not crash and why the leak was invisible to anything that
only counted wrappers). Rule 3 was honoured for the level surface's *wrapper* (deleted with the
texture's wrapper). Rule 4 was **not implemented**: reaching zero freed nothing in the backend.
Per text string per frame the backend kept one `TextureHandle` (a `VkImage` + `VkDeviceMemory`,
and for the 4-bit `A4R4G4B4` path its `B8G8R8A8` expansion), its level-0 `SurfaceHandle`, and the
`SYSTEMMEM` image `SurfaceHandle` (a mapped `VkBuffer`) — 1 texture : 2 surfaces, the ratio the
Mac measured (67 : 134 per second).

Classification: **PORT DEFECT** in the seam/backend, of the sub-kind **UNIMPLEMENTED PATH** (the
D3D8 resource-freeing path had no backend counterpart). Not an engine leak; not a Windows
behaviour; nothing in `Generals*/Code` or the D3D8 backend changes.

The separate symptom — the growing cost of `Get_Surface_Level` — is a **PORT DEFECT** of its own
(§4), a linear scan the D3D8 runtime never had, and it is what put the backtraces where the Mac
found them.

## 3. The fix: `Release()` at zero reaches the backend, the backend frees deferred

### 3.1 The backend interface (`spikes/renderer/src/render_backend.h`)

```cpp
virtual void Destroy_Texture(TextureHandle* texture) = 0;
virtual void Destroy_Surface(SurfaceHandle* surface) = 0;     // no-op for a texture's level surface
virtual void Destroy_Vertex_Buffer(VertexBufferHandle* vb) = 0;
virtual void Destroy_Index_Buffer(IndexBufferHandle* ib) = 0;

struct ResourceStats {                      // via Resource_Stats(), read by the gate and the probe
	uint32_t live_textures, live_surfaces, live_vertex_buffers, live_index_buffers;
	uint64_t textures_created, textures_destroyed, surfaces_created, surfaces_destroyed, ...;
	uint32_t retired_pending;               // freed from the live set, not yet freed on the device
};
```

### 3.2 The Vulkan backend (`spikes/renderer/src/vulkan_backend.cpp`)

`Destroy_*` is **two-phase**, because D3D8 lets the engine release a resource the GPU may still be
reading this frame (§1.1 consequences), and Vulkan does not:

- *Immediately, under `resource_mutex_`:* the handle leaves its `owned_*` vector (so `Resource_Stats`
  and the probe see it go at once); any `bound_textures_[stage]`, `bound_vb_`, `bound_ib_` pointing
  at it is cleared; any `framebuffers_` entry whose colour or depth attachment is the surface is
  retired (a `VkFramebuffer` names its attachments' image views and cannot outlive them); if the
  surface is `current_color_`/`current_depth_` the pass ends and the device falls back to its
  default targets, which is where the engine's own `Set_Render_Target` save/restore puts it anyway.
  A texture's `level0` surface is retired with the texture. `Destroy_Surface` on a surface with an
  `owner` returns without doing anything — that *is* rule 3.
- *At the next `Begin_Scene`, after the previous frame's fence has been waited:* `Flush_Retired`
  destroys the `VkFramebuffer`s, then frees each retired texture (`Free_Image` + its staging block
  returned to the pool of `renderer-resource-seam.md` §4.1, decrementing `staging_retained_blocks`
  when the texture had retained one), surface (`Free_Image`/`Free_Buffer`, unmapping first), vertex
  buffer and index buffer. `retired_pending` returns to 0.

`resource_destroy_` is read once from the environment: `ZH_RENDER_NO_RESOURCE_DESTROY=1` turns
every `Destroy_*` into a no-op. It exists only as the negative control of §6 and is not read by the
engine.

### 3.3 The seam (`Core/Libraries/Source/WWVegas/WW3D2/vulkanrenderbackend.cpp`)

```cpp
ULONG VulkanD3DTextureClass::Release()
{
	RefCount--;
	if (RefCount > 0) return (ULONG)RefCount;
	if (Live_Backend(Backend) != NULL) Backend->Destroy_Texture(Handle);
	delete this;                              // deletes the level-surface wrappers too
	return 0;
}
ULONG VulkanD3DSurfaceClass::AddRef()  { if (Container != NULL) return Container->AddRef();  return (ULONG)++RefCount; }
ULONG VulkanD3DSurfaceClass::Release()
{
	if (Container != NULL) return Container->Release();   // rule 3: a level surface IS its texture's count
	RefCount--;
	if (RefCount > 0) return (ULONG)RefCount;
	if (OwnsHandle && Live_Backend(Backend) != NULL) Backend->Destroy_Surface(Handle);
	delete this;
	return 0;
}
```

with the same shape for `VulkanD3DVertexBufferClass`/`VulkanD3DIndexBufferClass`. Points that
matter:

- **Level surfaces forward to their container.** This is the §1.1 rule-3 model, and it is what
  makes deleting the level-surface wrappers in `~VulkanD3DTextureClass` safe: the texture's count
  cannot reach zero while any caller holds a level surface, because that caller's reference *is* a
  texture reference. (Before this slice the level surface kept its own count and a caller holding
  one across the texture's last `Release` would have been left with a dangling wrapper. Nothing in
  the engine does that, but the contract now matches.)
- **`OwnsHandle`.** Only the wrapper made by `CreateImageSurface` owns a backend handle (render
  targets in this engine are textures with `D3DUSAGE_RENDERTARGET`, freed through
  `Destroy_Texture`). The wrappers `GetRenderTarget`/`GetDepthStencilSurface` view the
  device's default targets, which the device holds and frees at `Shutdown`; they never call
  `Destroy_Surface`.
- **`Live_Backend`.** `TheLiveBackend` is set when `VulkanRenderBackendClass` creates the device and
  cleared before `Shutdown()`. A `Release()` that runs after the device is gone — the engine's
  static destructors do this — frees the wrapper and touches nothing else, rather than calling
  into a deleted backend.
- No engine call site changes; consumers still spell `IDirect3DTexture8::GetSurfaceLevel`,
  `IDirect3DSurface8::Release` and go through `DX8Wrapper → RenderBackendClass`. No new direct
  `IDirect3DDevice8` call sites (`check-d3d8-surface.py`/`check-backend-coverage.py`, §8).

## 4. The linear scan, fixed second, measured on its own

`Get_Surface_Level(texture, 0)` must return the *same* `SurfaceHandle*` every call — the engine
compares the surface it saved against the one it restores in `Set_Render_Target`, and the
framebuffer cache is keyed on surface identity — so it looked for an existing one:

```cpp
for (SurfaceHandle* existing : owned_surfaces_)
	if (existing->owner == texture) return existing;
```

That is O(live surfaces) per call, on a call the text path makes once per string per frame, over
a list that §2 made grow by ~134/s. It is a **PORT DEFECT** (D3D8 has no such scan) but a
*performance* one, and fixing it alone would have made the leak cheaper and quieter, which is the
one thing the slice forbade. Order of work, each in its own commit, each measured:

| state | `owned_surfaces_` at frame 300 | `Get_Surface_Level` first → last 10-frame window |
|---|---|---|
| leak present, scan kept (`6bd98904d` with `ZH_RENDER_NO_RESOURCE_DESTROY=1`, i.e. `main`'s behaviour) | 4,800 above bound | 265 → 4,905 (earlier run 271 → 6,549) |
| leak fixed, scan kept (`6bd98904d`) | at bound | 198 → 119; 131 → 165 (two runs) |
| leak fixed, scan replaced by `TextureHandle::level0` (`f93bce585`) | at bound | 90 → 136; 168 → 132; 108 → 135 (three runs) |
| leak *re-enabled*, pointer in place (negative control today) | 4,800 above bound | 250 → 188; 150 → 115 |

(All rows: `zh-resource-lifetime --frames 300`, lavapipe, Linux x86-64, 2026-09-03; the
negative-control rows are what the gate prints on every run; the spread between runs is timer
noise on a ~100 ns call.) Two readings. First, **rows 2 and 3 are the same number**: once the
leak is fixed the scan is over a list of a handful of surfaces in the spike, and of 443 in the
real game (§5.1) — at ~1 ns per compare and ~2 calls per frame that is not a real cost, and this
document does not claim it as one. The pointer commit stands on its own as an O(1) identity lookup
(and removes `Note_Texture_Layout`'s copy of the same walk); it can be dropped without touching
the fix. Second, the last row: with the pointer in place the scan symptom is gone *even while the
leak is present* — exactly the "fixing the scan alone would hide a leak" case. That is why the
gate asserts live counts, not scan cost, and why the negative control must fail for the counts
(§6).

`Note_Texture_Layout` used the same scan to keep a level surface's `layout` in step with its
texture's and now uses the pointer. The `Destroy_*` entry points themselves do a `std::find` over
the live vector, which is bounded now (443 surfaces / 375 textures in the real game, §5) and
costs well under a microsecond; it is listed in §7 rather than optimised here.

## 5. Real game, before and after — Linux x86-64, lavapipe; Apple Silicon UNMEASURED

### 5.0 Units

`scripts/native-resource-probe.py` reads each `owned_*` vector's size as
`(_M_finish - _M_start) / sizeof(element)`. Its first version omitted the division and the first
before-run log therefore printed 71,224 surfaces where there were 8,903; the JSON quoted below was
re-derived from that run (every value divided by 8, the pointer size) and the fix verified against
the live fixed process, where the corrected `owned_surfaces_` (443) equals the backend's own
`resource_stats_.live_surfaces` (443). All counts below are elements.

Method (`scripts/native-resource-probe.py`, new in this slice): the native strict-link binary
(`build/native/native_strict_link`, 980/980 objects, 0 unresolved) runs from a disposable
directory holding hard links to the retail Zero Hour 1.04 `.big` set
(`s3://cc-mac-game-data/zerohour104_gamedata_full.7z`, SHA-256
`d9ddd811…ac1ae4`), `STRING_InstallPath` pointing at the base game's `Generals/` so both archive
sets load, `-win -xres 800 -yres 600 -noshellmap`, Xvfb `:99`, `VK_ICD_FILENAMES` = lavapipe. The
shell is driven with real X11 input (`xdotool`: Solo Play → Skirmish → Play Game, Alpine Assault,
one Easy AI) into a live map with the in-game UI up — the UI is what draws the text. The probe then
attaches with GDB once a second for 20 minutes and reads, from the paused process, the four
`owned_*` vector sizes, `resource_stats_` (after only; before has no such member), RSS from
`/proc/<pid>/status`, and the engine's own 30-entry instantaneous-FPS ring
(`W3DDisplay::updateAverageFPS`'s `fpsHistory`) from which the frame-time percentiles are made.
Nothing in the engine is instrumented. The baseline binary is `main` at `6e182ac34` built the same
way in a second worktree; the fixed binary is this branch.

### 5.1 Owned-object counts — steady state, not a slower slope

| | before (`6e182ac34`) | after (this branch) |
|---|---|---|
| run length | 1,201.8 s (20.0 min), 537 samples | 1,200.7 s (20.0 min), 550 samples |
| `owned_surfaces_` first → last (max) | 8,903 → 27,735 (**+18,832**, +15.7/s) | 443 → 443 (max 443, **+0**) |
| `owned_textures_` first → last (max) | 4,636 → 14,052 (**+9,416**, +7.8/s) | 375 → 375 (max 375, **+0**) |
| `owned_vbs_` / `owned_ibs_` | 93 / 81 → 93 / 81 | 92 / 81 → 92 / 81 |
| `resource_stats_` textures created / destroyed | (no such member on `main`) | 6,310 / 5,935 → 16,280 / 15,905 (**+9,970 / +9,970**) |
| `resource_stats_` surfaces created / destroyed | (no such member on `main`) | 12,311 / 11,868 → 32,251 / 31,808 (**+19,940 / +19,940**) |
| `retired_pending` at last sample (max over run) | n/a | 3 (9) — the current frame's, freed at its next `Begin_Scene` |
| logic frames advanced | 439 → 4,561 | 392 → 4,789 |

The before run leaked 1 texture and exactly 2 surfaces per string rebuild (ratio 2.000 : 1, as §2
predicts); the after run *created* them at the same rate (+8.3 textures/s against +7.8/s leaked
before — same UI, same map, slightly higher frame rate) and destroyed every one. The
375 live textures / 443 live surfaces are the map's and UI's resident set, and the difference
443 − 375 = 68 is the number of standalone (`CreateImageSurface`) surfaces the shell and HUD keep.

On the Mac's rates: §1.1 of `playability-probe.md` gives +134 surfaces/s at 30 FPS; the Linux
before run gives +15.7/s at ~7 FPS, i.e. ~2.2 surfaces per frame here against ~4.5 per frame there
if the Mac figure is in elements, or ~0.56 per frame if it carries the 8× of §5.0. Either is
consistent with a per-frame text rebuild; which it is has to be read on the Mac.

The after run's *created* counters keep rising at the same per-second rate as before — the text
path still makes and releases its texture and two surfaces per string per frame — and the
*destroyed* counters track them exactly, which is what "the fix" means: the engine's behaviour is
unchanged, and the backend now does what D3D8 did with the releases.

### 5.2 RSS

| | before | after |
|---|---|---|
| RSS first → last (max) | 1,061.1 MB → 1,373.9 MB (**+312.8 MB** over 20.0 min, +15.6 MB/min) | 923.3 MB → 923.3 MB (max 923.3 MB; 923,312,128 bytes at first, last and peak: **+0**) |

Both runs start above 900 MB because lavapipe keeps its "device memory" in host RAM and the map's
own textures are large; the *slope* is what the leak owned. **Apple Silicon: UNMEASURED.** The
M1 Pro's 32 → 184 MB over 21.8 min in `playability-probe.md` §1.1 is the number this fix should
flatten there, and it has not been re-measured on that machine.

### 5.3 Frame time — measured, and not the number the Mac needs

From the engine's FPS ring, sampled once a second (so a sample of frames, ~4,100 of them per run,
not every frame); `frame_window_seconds` 120 at each end:

| ms | before, first 120 s | before, last 120 s | after, first 120 s | after, last 120 s |
|---|---|---|---|---|
| frames seen | 356 | 439 | 400 | 440 |
| mean | 332.3 | 275.8 | 296.1 | 273.4 |
| p50 | 139.3 | 126.8 | 123.9 | 124.2 |
| p99 | 1,791.8 | 1,320.8 | 2,550.8 | 1,304.6 |
| worst | 2,661.9 | 1,493.1 | 4,179.5 | 2,551.4 |

Honest reading: on lavapipe the frame is dominated by software rasterisation of an 800×600 3D view
(~130 ms median, seconds at the tail when the probe's own `ptrace` stop lands mid-frame — the
probe records its stop cost, ~1.2 s per sample, and those stops are *in* the ring). Twenty minutes
of leak puts ~28 k entries under the scan, × ~1 ns ≈ 0.03 ms per `Get_Surface_Level`, per string
— invisible under 124 ms. The before run's frame time did *not*
degrade in 20 minutes, so **this run neither reproduces nor refutes the Mac's 30 → 17 FPS decay
after hours**; it proves the counts, not the FPS recovery. What it would take: the same probe on
the M1 Pro (`scripts/macos-playability-probe.py run`, which reads the same ring) over ≥ 2 h on the
fixed binary, compared with `playability-probe.md` §2.1. Owed; **UNMEASURED** here.

### 5.4 What this measurement does not cover

- The Mac (all of it): counts, RSS, frame time — **UNMEASURED**, by instruction. The Linux numbers
  above are on the same backend code with a different ICD; the leak and the fix are in
  platform-independent code (`vulkan_backend.cpp`, `vulkanrenderbackend.cpp`), so there is no
  platform-dependent value in this slice — the MoltenVK CI job runs the §6 gate, which is the
  synthetic half of the Mac evidence.
- The campaign: the probe ran a skirmish. The text path is the same (`W3DDisplayString::draw`).
- Sound: Linux had no ALSA device, so the run was silent (`docs/porting/playability-probe.md` §3 is
  about a different defect; not renderer evidence either way).

## 6. The gate: `zh-resource-lifetime` and `scripts/ci/check-resource-lifetime.py`

`spikes/renderer/src/resource_lifetime.cpp` models the text path *at the backend seam*, one
level below `Render2DSentenceClass` so it needs no engine, fonts or retail data — the same layer
`check-draw-capacity.py` and `check-untyped-vertex-buffer.py` test at. Per frame, 8 "strings", each:

```text
Create_Image_Surface(64x64, A8R8G8B8)   Surface_Bits -> fill with a per-string colour
Create_Lockable_Texture(64x64)          Get_Surface_Level(tex, 0)
Copy_Rects(image -> level0)             Begin_Scene / Clear / Set_Texture / Draw_Triangles (a quad)
Read_Back_Color_Target -> the drawn texel must equal the string's colour (or the frame is a lie)
Destroy_Surface(level0)  (no-op by rule 3)   Destroy_Surface(image)   Destroy_Texture(tex)
```

The `Get_Surface_Level` call is timed over the first and last 10 frames. After the frames, one idle
`Begin_Scene`/`End_Scene` so `Flush_Retired` runs, then `Resource_Stats` is printed and compared
with the pre-workload snapshot. The workload exits non-zero if live textures or surfaces are above
their starting bound, printing `resource-lifetime: live surfaces N exceed bound M`.

The checker (`--frames 300` default, `--validation` always) runs it twice:

1. **Under test.** Asserts the *shape* first, so a cut-down workload cannot pass by printing less:
   exactly 300 × 8 strings, 2,400 textures and 4,800 surfaces created, 2,400 of 2,400 texels
   verified, the validation layer loaded and 0 messages. Then: live textures and surfaces back at
   their bound, exactly 2,400 / 4,800 destroyed, `retired_pending` 0, and `Get_Surface_Level`'s
   last window no more than 4× its first once above a 1 µs noise floor.
2. **Negative control**, `ZH_RENDER_NO_RESOURCE_DESTROY=1`. Must exit non-zero; must still have the
   full shape (all texels verify — the leak is not a rendering fault); live textures must be exactly
   +2,400 and surfaces exactly +4,800, and the workload must have printed the "exceed bound" line for
   both. If the control passes, the checker fails with "this gate cannot fail and proves nothing";
   if it fails for any other reason, it says so and fails.

Output on this branch (Linux, lavapipe, 2026-09-03):

```text
after textures created 2401 destroyed 2400 live 1
after surfaces created 4800 destroyed 4800 live 0
after retired pending 0
texels verified 2400 of 2400
resource-lifetime: PASS
negative control: Get_Surface_Level 150 -> 115 ns/call over 300 frames with the leak
negative control: with ZH_RENDER_NO_RESOURCE_DESTROY every texel still verifies and live
  textures/surfaces end 2400/4800 above their bound, so the workload fails for the leak, as it must
OK: 2400 text textures (+4800 surfaces) created, drawn with every texel verified, and destroyed
  over 300 frames; live counts back at their bound, Get_Surface_Level 108 -> 135 ns/call, the
  validation layer active and silent
```

(The `live 1` texture at both ends is the workload's own colour target, created before the
snapshot; the bound is the snapshot, not zero.) **SYNTHETIC-ONLY** by construction; §5 is the real
half.

CI: `.github/workflows/native-port-ci.yml`, step "Resource lifetime, live counts back at their
bound after N frames of text" in the Linux/lavapipe job and "Resource lifetime through MoltenVK" in
the macOS arm64 job, both writing to the step summary like their neighbours.

## 7. Remaining leaks and costs found, not fixed — ranked, with evidence

1. **The text renderer rebuilds every dirty string's texture every frame** (engine, Windows
   behaviour, not a leak). Evidence: §5.1 after-run `textures_created` rises at the same rate
   the before run leaked, ≈8.3/s at ~7 FPS, ≈1.1 textures + 2.2 surfaces per frame, each with a
   `vkCreateImage`/`vkAllocateMemory` and a mapped `VkBuffer`. Cost is now bounded and paid per
   frame; at 30 FPS it would be ≈35 image creations/s. A texture cache keyed on the
   string would remove it, but it is engine behaviour shared with Windows and belongs to a
   performance slice, not this one. Class: not a defect; a cost.
2. **`Destroy_*`/`Retire_Surface` `std::find` over the live vectors** (backend, PORT DEFECT,
   small). O(live) per destroy, live ≈ 375–443 in the game (§5.1), ≈3 destroys per frame:
   ≈1,300 pointer compares per frame, ~1 µs. Evidence: §5.1 after-run vector sizes. Would be an
   `unordered_set` or an index-in-handle; left because it is bounded and the slice's rule was one
   seam per PR. `Retire_Surface` also walks `framebuffers_` (≈ a dozen entries).
3. **`owned_vbs_` 92–93 / `owned_ibs_` 81, flat for 20 minutes in both runs** — not a leak in this
   workload, but the engine's dynamic vertex/index buffers are recycled by `DX8Wrapper`, so their
   `Release()` reaching zero rarely happens in a skirmish; the new `Destroy_Vertex_Buffer`/
   `Destroy_Index_Buffer` paths are exercised only by shutdown here and by nothing in the gate.
   Evidence: §5.1. **UNMEASURED** on a campaign transition, where W3D asset unload does release
   them; a map-change probe is the measurement owed.
4. **Everything freed at `Shutdown()` still leaks at process exit** because every quit is a crash
   (`playability-probe.md` §7, `~ObjectPoolClass` in `exit`). Not this slice's mechanism; listed
   because "RSS returns to baseline at quit" cannot be shown until it is fixed.
5. **Staging pool never shrinks** (`renderer-resource-seam.md` §4.1: "Nothing is unmapped or freed
   until shutdown"). By design; bounded by the high-water mark of concurrently locked bytes; the
   §5.2 after-run RSS slope is the place it would show, and the number is in the table.

## 8. Verification run for this slice

```text
python3 -m flake8 --max-line-length=100 scripts/                       clean
actionlint .github/workflows/*.yml                                     clean
python3 scripts/ci/check-resource-lifetime.py --binary build/spike/zh-resource-lifetime   OK (§6)
python3 scripts/ci/check-spike-render.py ...                           VERIFY_RENDER
python3 scripts/ci/check-draw-capacity.py --self-check ...             VERIFY_DRAW
python3 scripts/ci/check-untyped-vertex-buffer.py ...                  VERIFY_UNTYPED
python3 scripts/ci/check-bc-textures.py ...                            VERIFY_BC
python3 scripts/ci/check-d3d8-surface.py && check-backend-coverage.py  VERIFY_SURFACE (one gate, run together)
python3 spikes/renderer/tools/d3d8-lock-scan.py --check                clean
python3 spikes/renderer/tools/surface-lock-audit.py --check            clean
CLANGXX=clang++-14 scripts/native-build.py --level 1..4 --with-shims --strict-link
                                                                       980/980, 0 unresolved, binary produced
python3 scripts/ci/check-generated-baselines.py && scripts/porting-status.py   VERIFY_BASELINES
```

Windows: no file compiled into the D3D8 backend changes; `vulkanrenderbackend.cpp` and the spike
are off-Windows only. Reference-counting semantics on the D3D8 backend are untouched.
