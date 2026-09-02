# The mission frame: three defects, none of them the pitch hypothesis

Hardware: Apple M1 Pro, macOS 26.6.1, arm64, `backingScaleFactor` 2.00, MoltenVK 1.4.2.
Retail depot `~/devin-work/zh-data` (Zero Hour 1.04 full + base-game `Generals/*.big` reachable
through the install path), native binary built by
`scripts/native-build.py --level 1..4 --with-shims --strict-link` (980/980 objects, 0 unresolved).
Linux comparison: aarch64 Ubuntu 22.04 container, lavapipe (`lvp_icd.aarch64.json`), for the
synthetic tests only; the retail mission was not run there (see §7).

Numbers here were measured on that machine with `scripts/mission-frame-trace.py`, which starts
the real binary with `ZH_RENDER_TRACE`, posts real `CGEventPost` clicks Main Menu -> Single Player
-> USA -> Easy, and reduces the backend's per-frame trace and `Read_Back_Color_Target()` PNGs.
Nothing in this document is a source of truth; the enforced numbers live in
`docs/porting/ci-baselines/*.json`.

## 0. The result in one table

Mission frames of the USA campaign, traced every 240th frame, one run each. "magenta" is the
fraction of read-back pixels within the missing-texture placeholder's colour (r>=200, g<=60,
b>=200); "unique" is distinct RGB triples in the 1600x1200 readback.

| binary | frame 1680 draws | frame 1920 draws | unique | magenta | black | missing-texture fallbacks |
| --- | --- | --- | --- | --- | --- | --- |
| `main` @ 632ba201f + deferred clear (§2) only | 69 | 69 | 1 | 0% | 100% | not counted |
| + `Has_Render_Device()` guards (§3) | 245 | 487 | 98,080 / 110,015 | 43.1% / 33.9% | 25.0% | **1024** |
| + DDS header fix (§4) | 242 | 408 | 107,423 / 118,669 | **0.00%** | 68.0% | **1** (`trstrtholecvr.tga`) |

Shell frames (240..1200) are identical across all three rows: 20/20/20/27/19 draws, 390k-447k
unique colours, 0% magenta, <0.4% black. The shell was never the problem and was not touched.

The standing hypothesis from `draws-per-frame.md` §5 — a size/pitch mismatch because the mission
viewport is inset — is **wrong**. Measured in every traced mission frame: render target
`800x600 device 1600x1200`, viewport `0,0 800x600`, scissor `0,0 1600x1200`, one target pointer,
identical to the shell. No render-to-texture target was ever set in a traced frame (the
`SetRenderTarget` list is empty in all runs). The "tiled small region" was the 128x128 magenta
missing texture tiled across every model and terrain patch whose texture failed to load; the
"letterboxing" is the mission's own intro cinematic (black bars, subtitle "Some..." drawn in the
frame), not a viewport defect.

## 1. Method

- `spikes/renderer/src/vulkan_backend.cpp` gained trace lines (behind `ZH_RENDER_TRACE`) for:
  every `Set_Viewport`/scissor/render-target the engine sets versus what the backend binds; every
  texture and surface creation with format, mip count, VkFormat, source pitch and expanded storage
  pitch; render-pass begin/end per draw index; clears including ones issued before `Begin_Scene`;
  dynamic-buffer DISCARDs that rewrite a region an earlier draw of the same frame reads.
- `Read_Back_Color_Target()` PNG per traced frame; classified by pixel counts, not by eye.
- `Core/Libraries/Source/WWVegas/WW3D2/textureloader.cpp` prints one `missing-texture` line per
  fallback to the placeholder when the trace is on; the script counts them.
- `scripts/native-render-backend-run.py` (the engine's own init sequence without retail data)
  gained two stages that print the device-presence question both ways (§3).
- `spikes/renderer/src/fixedfunc_tests.cpp` gained `Case_Clear_Before_Begin_Scene` (§2), run on
  both platforms with a negative control.

## 2. Defect 1 — PORT DEFECT (backend): a `Clear()` before `Begin_Scene()` was dropped

Call: `IDirect3DDevice8::Clear` from `WW3D::Begin_Render()`
(`GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/ww3d.cpp`), which clears colour+Z and *then*
calls `Begin_Scene()`. Engine intent: D3D8 allows `Clear` outside a scene. Backend: it began the
render pass at `Begin_Scene` with `LOAD_OP_LOAD` and had no pass to record the clear into, so the
clear was silently lost; every frame started on the previous frame's colour and depth.

Proof (synthetic, both platforms): `zh-fixedfunc-tests` `Case_Clear_Before_Begin_Scene` draws a
quad, then clears before `Begin_Scene`, draws nothing, and reads back. With the fix the target is the
clear colour on Mac/MoltenVK and Linux/lavapipe; with the deferral disabled (negative control) it
fails identically on both with the stale quad. So: PORT DEFECT, not a MoltenVK subset issue.

Fix: the backend records a pre-scene clear as pending and turns it into the pass's
`LOAD_OP_CLEAR` at `Begin_Scene` (trace line `deferred_from_before_begin_scene, color=1 z=1`, one
per frame in every traced frame above). Windows behaviour unchanged (D3D8 backend untouched).

Why it only showed in mission frames: the shell draws opaque full-screen 2D over everything each
frame; the mission's 3D viewport has sky/terrain that do not cover the frame, so stale depth and
colour survive. On its own this fix produced the 100%-black row above — the clear now happened, but
nothing was drawn into it, which led to §3.

## 3. Defect 2 — PORT DEFECT (seam): "is there a device" was asked of a pointer that is null off Windows

Call: 21 guards of the form `if (!DX8Wrapper::_Get_D3D_Device8()) return;` in
`W3DDisplay.cpp` (`draw()`, `updateViews`-adjacent paths, device-lost handling), `W3DScene.cpp`,
`W3DShroud.cpp`, `W3DSmudge.cpp`, `W3DSnow.cpp`, `W3DMouse.cpp`, `BaseHeightMap.cpp`,
`W3DProjectedShadow.cpp`, `W3DVolumetricShadow.cpp`, `ww3d.cpp`. Engine intent: skip drawing when
there is no device (Windows: `D3DDevice == NULL` before `CreateDevice` or after loss). Backend:
`_Get_D3D_Device8()` returns `RenderBackend->Peek_D3D_Device8()`, which the Vulkan backend
answers with `nullptr` by design — there is no `IDirect3DDevice8` — so every one of those guards
took the "no device" branch while a device existed. The 3D view was never updated or drawn; the 69
draws per frame were the HUD and 2D layers only, constant for 800 frames.

Measurement (in-process, Mac, `scripts/native-render-backend-run.py`, this tree):

```
== the device-presence guard
_Get_D3D_Device8() = 0x0, Has_Render_Device() = 1
Has_Render_Device with a device            ok
_Get_D3D_Device8 is null off Windows       ok  (so it cannot be the guard)
```

Fix: `DX8Wrapper::Has_Render_Device()` (`RenderBackend != nullptr && RenderBackend->Has_Device()`)
and the 21 presence guards converted to it. Every converted site was read: each asks only whether a
device exists, none dereferences the D3D8 interface. On Windows `D3D8RenderBackendClass::Has_Device()`
is `D3DDevice != nullptr`, the same predicate the old test evaluated, so Windows behaviour is
unchanged by construction; no simulation or serialisation path is touched, so the replay gate was
not run. Effect: mission draws per frame went from a constant 69 to 112-487 and varying (the
camera moves through the cinematic), and the readback went from 1 colour to ~100k.

Why only mission frames: the shell's menu is 2D and is drawn before these guards are consulted; the
traces here run with `-noshellmap`, so whether the 3D shell map was also gated by them was not
measured in this slice.

## 4. Defect 3 — PORT DEFECT (engine data structure): the on-disk DDS header struct is 136 bytes on LP64

Call: `DDSFileClass::DDSFileClass` (`GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/ddsfile.cpp`)
does `file->Read(&SurfaceDesc, sizeof(LegacyDDSURFACEDESC2))` and then rejects the file if
`read_bytes != SurfaceDesc.Size`. Engine intent: `LegacyDDSURFACEDESC2` mirrors the 124-byte DX7
`DDSURFACEDESC2` on disk; on Win32 `void* Surface` (the old `lpSurface`) is 4 bytes and
`sizeof == 124`. On arm64/x86-64 the pointer is 8 bytes and 8-aligned:

```
sizeof=136 offsetof(Surface)=40 offsetof(PixelFormat)=80     (clang, arm64, this tree before the fix)
```

so 136 bytes were read, `136 != 124`, `LevelSizes` stayed null, `Is_Available()` was false, and
`TextureLoadTaskClass::Begin_Compressed_Load` returned false for **every DDS texture in the game**
(3,615 of 3,748 entries in `Textures.big` and 3,496 of 3,546 in `TexturesZH.big` are `.dds`).
The loader then tried the `.tga` name, which does not exist for those, and applied the 128x128
magenta `MissingTexture`. That is the tiled region and the magenta blocks of the recorded symptom;
the dark-green blocks were not separately attributed and are not claimed here.

Measurement: `missing-texture` fallbacks per run, 1024 -> 1; magenta fraction of the mission readback
43.1%/33.9% -> 0.00% on both traced mission frames, shell frames unchanged. The one remaining
fallback, `trstrtholecvr.tga`, is absent from both archives and is MISSING DATA (or retail's own
fallback), not a loader fault.

Fix: `Surface` is `unsigned` (a 32-bit on-disk field, never a host pointer — nothing reads it) and a
`static_assert(sizeof(LegacyDDSURFACEDESC2) == 124)` makes any future layout drift fail the compile
rather than paint magenta. Windows: 4-byte pointer -> 4-byte unsigned, identical layout.

Why only mission frames: the shell's 2D art and fonts are `.tga` (loaded by the Targa path, which
never touched this struct); models and terrain detail in a mission are `.dds`. The 3D shell map also
uses `.dds` models; the traces here run with `-noshellmap`, so the earlier "pixel-correct" shell-map
claim was neither confirmed nor contradicted by this slice — the same defect applied there.

Classification note: this is a PORT DEFECT (LP64 layout of an on-disk struct), reproducible on any
64-bit target by arithmetic; it is not MoltenVK-specific. It was compiled on Linux by the strict
build every CI run and produced no warning, which is exactly the class of silent 64-bit bug the
`-fsyntax-only` era could not see.

## 5. Residual defects, named and classified

Ranked by how much of the current mission readback they affect.

1. **Model textures decode to noise — RESOLVED IN ANALYSIS, fixed on the seam, real-game
   confirmation owed** (`block-compressed-textures.md`). It was neither candidate below: the caps
   were truthful, but `Create_Lockable_Texture` had no compressed layout, so `CreateTexture(DXTn)`
   failed, off-Windows D3DX substituted 8888 with `D3D_OK`, and `Load_Compressed_Mipmap`
   `memcpy`d compressed bytes into the 8888 lock (UNIMPLEMENTED PATH hidden by an uncounted
   substitution). BC1/2/3 are now created for real and the substitution is a ledger entry. The
   text that follows is the original analysis, kept for the record. With DDS files now readable, every texture created in a mission frame is `vk=44`
   (`B8G8R8A8_UNORM`), formats 0/1 (`A8R8G8B8`/`X8R8G8B8`), and *no* DXT texture is created
   (creation trace: 3,851 `vk=44` creations, 0 in BC formats). So `Get_Valid_Texture_Format()` is
   converting DXT1/5 to 8888 and the engine's software decoder `DDSFileClass::Get_4x4_Block` writes
   the level into the locked 8888 surface. Trees and units in the readback show high-frequency
   multi-coloured noise consistent with either (a) the decoder's `dest_pitch`/`dest_bpp` arithmetic
   meeting the backend's expanded pitch (`pitch0=128 expand=1` -> surface pitch 256 on 64x64 levels
   is already visible in the trace for format 3), or (b) `DX8Caps::SupportDXTC` being false because
   `CheckDeviceFormat` answered NOTAVAILABLE for BC formats, forcing this path at all. Measurement
   to isolate: a `zh-resource-lock-tests` case that locks a 64x64 8888 level, writes a known
   pattern through the D3D8 pitch and reads it back (decides (a)); and the `sampled_formats` bits
   printed at enumeration for `DXT1/3/5` (decides (b)). Cost: (a) is a pitch fix in the lock path,
   under a day; (b) is enabling BC upload — the backend already maps DXT1/3/5 to BC1/2/3 and
   `zh-feature-probe` asserts DXT5 sampling — so it is a caps-enumeration fix plus a compressed
   `Copy_Level_To_Surface` fast path, one to two sessions. Terrain (`TerrainTex.cpp`, `A1R5G5B5`
   then 8888) is unaffected and reads back with correct detail.
2. **Black sky / 68% black in the cinematic frames (unclassified).** The camera in frames
   1440-1920 looks up a cliff; the retail game draws a skybox or sky plane there. Whether the sky is
   culled (a `W3DShaderManager` path the port skips), textured with a failed decode from item 1, or
   simply not part of this map's cinematic is not measured. Not claimed either way.
3. **`Decode_Fvf: unsupported FVF 0x0` twice at startup** — unchanged, pre-mission, not causal
   (already established in `draws-per-frame.md`).
4. **Per-draw readback splits the render pass (SYNTHETIC-ONLY).** `--per-draw` changes pass
   boundaries and load ops; its pixel deltas are diagnostic only and were not used for any claim
   here.
5. **Within-frame dynamic-buffer overruns**: counted (`ring_overruns`) and 0 in every traced frame,
   so the ring is not a current defect; the counter stays as a guard.

## 6. What is proven and what is not

Proven by measurement: the three mechanisms in §2-§4 (each with an in-process before/after
number); the pitch/viewport hypothesis is false for this corruption; the shell is unchanged.

**Not proven: that the mission renders correctly.** It does not yet — item 1 in §5 is visible in
the readback as noise on every model — and no per-pixel oracle against Windows exists for a mission
frame. The claim of this slice is narrower: the tiled/magenta/ghosted corruption has a named cause
that is fixed, and the next wall is named with the measurement that will decide it.

## 7. Linux/lavapipe

- §2 reproduces and its fix is verified on Linux/lavapipe (synthetic test + negative control).
- §3 and §4 are compile-time facts of the LP64 ABI shared by Linux x86-64 clang-14 and macOS
  arm64; the `static_assert` in `ddsfile.h` is now evaluated by the Linux strict build on every
  CI run, which is the Linux measurement for §4. The Linux container available here has no C++
  compiler to print `sizeof` locally.
- The retail mission was **not** driven on Linux: the outpost's container has no retail data
  mounted and no window/input harness; `scripts/mission-frame-trace.py` posts macOS `CGEventPost`
  events. Reproducing the full mission there needs the trimmed+full retail objects in the container
  and an X11/SDL input driver equivalent to `scripts/macos-input-drive.py` — roughly one session.

## 8. Gates run on this tree (Mac)

`flake8` clean; `actionlint` (`SHELLCHECK_OPTS=--severity=error`, as CI) clean;
`check-spike-render.py` 0/480000 pixels differ, validation silent; `zh-feature-probe`,
`zh-fixedfunc-tests` (0 failed, 6 pending), `zh-resource-lock-tests` (0 failed) all with validation
silent; `check-draw-capacity.py --self-check` 4096 draws sustained, 0 dropped, 0 aliased;
`check-d3d8-surface.py` and `check-backend-coverage.py` both exact matches; strict native build
980/980, 0 undefined; `check-generated-baselines.py` OK; `porting-status.py` regenerated
`STATUS.md` with no diff.
