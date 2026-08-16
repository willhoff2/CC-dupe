# The D3DX route to the backend, the swapchain define, and the engine's first frame

Wave 8 slice 1. The predecessor, `docs/porting/renderer-integration.md`, put a real Vulkan device
under `DX8Wrapper` and measured where the engine then stopped: `MissingTexture::_Init()` locking a
null texture, because `_Create_DX8_Texture` calls `D3DXCreateTexture(_Get_D3D_Device8(), ...)` and
there is no `IDirect3DDevice8` behind a non-D3D8 backend. This slice routes the D3DX creation entry
points through `RenderBackendClass`, fixes the missing `SPIKE_WITH_PLATFORM_WINDOW` the arm64 outpost
found, and drives the engine until it presents a frame whose contents are then read back and
measured.

What is proven here is proven **on Linux against lavapipe**. §6 says what only Apple Silicon can
decide, and §5 says exactly which claim the frame proof does and does not support.

## 1. Reproducing the wall first, on current `main`

Before any edit, on `main` at `8c6dfaab0` (which contains #96, #98, #99 and #100), under gdb because
`lldb` is not on this box:

```
Thread 1 "native_render_r" received signal SIGSEGV, Segmentation fault.
#0  MissingTexture::_Init() at missingtexture.cpp
#1  DX8Wrapper::Do_Onetime_Device_Dependent_Inits()
#2  DX8Wrapper::Create_Device() ... #4 WW3D::Set_Render_Device()
```

Unchanged from the predecessor's measurement, and unchanged from the M1 Pro's: same call, same null
`tex`, `D3DXCreateTexture` returning `D3DERR_INVALIDCALL` without calling anything. The wall had not
moved, so the fix below is aimed at the thing that was actually measured.

## 2. The D3DX helpers now ask the backend

`d3dx8texcreate.cpp` is d3dx8.lib's own creation entry points reimplemented off Windows, and it was
the one part of the D3D8 surface that still needed an `IDirect3DDevice8*`. It now resolves a target
before it plans anything:

```cpp
struct TargetType {
    LPDIRECT3DDEVICE8   D3DDevice8;   // Windows, or a caller that has a real device
    RenderBackendClass* Backend;      // off Windows, the installed backend
};

static TargetType Resolve_Target(LPDIRECT3DDEVICE8 device)
{
    TargetType target = { device, nullptr };
    if (device != nullptr) return target;                       // unchanged behaviour
    RenderBackendClass* backend = D3DX8TexCreate::Peek_Render_Backend();
    if (backend != nullptr && backend->Has_Device()) target.Backend = backend;
    return target;
}
```

- `D3DXCreateTexture`, `D3DXCreateCubeTexture` and `D3DXCreateVolumeTexture` route through it.
  `RenderBackendClass` gained `CreateCubeTexture`/`CreateVolumeTexture` to make that possible;
  `D3D8RenderBackendClass` forwards both to the device, so **Windows behaviour is byte-for-byte the
  old call**: a non-null device short-circuits `Resolve_Target` before a backend is even consulted.
- `Plan()`, the mip calculation, the format-candidate list, the substitution message and the retry
  loop are untouched — the change is *where the device comes from*, not what gets asked for.
- With no backend, or a backend with no device, the helpers still fail and still null the out
  parameter. Nothing was made to succeed by lowering a check.
- `D3DXCreateTextureFromFileExA` is still an honest `E_NOTIMPL`: the engine did not reach it in this
  run, and routing it would mean writing a DDS/TGA decoder into a creation helper on the strength of
  a guess. It is in the ledger, not implemented.
- The member had to be named `D3DDevice8` rather than `Device`, because `check-d3d8-surface.py`
  deliberately ignores generic receiver names — the rename is what made these four calls *visible* to
  the gate, and `d3d8-direct-allowlist.json` moves 3 → 4 with that reason recorded.

`Peek_Render_Backend()` is defined off Windows in `dx8wrapper.cpp` (the serialised renderer
chokepoint) and returns `DX8Wrapper::Get_Render_Backend()`, so the D3DX layer does not gain its own
notion of which backend is installed.

`d3dx8texcreate_test.cpp` grew a `RecordingBackendClass` that implements the whole
`RenderBackendClass` interface and records what it was asked for: dispatch through the backend when
the raw device is null, the full mip chain, extent clamping, format rejection and retry, the cube and
volume paths, a caps failure, a backend without a device, and no backend at all — 102 checks, 0
failures, sanitiser probe clean.

## 3. The swapchain define, and why `Present()` can no longer lie

The arm64 outpost found `scripts/native-build.py` compiled the backend **without**
`SPIKE_WITH_PLATFORM_WINDOW`, so the engine's copy had no surface and no swapchain, and `Present()`
would have returned success having presented nothing. Two changes, because the define alone is a
thing that can quietly come back:

1. `scripts/native-build.py` defines `SPIKE_WITH_PLATFORM_WINDOW` for the shared backend sources, and
   `scripts/ci/check-swapchain-compiled.py` reads the archive the build produced and fails unless
   `vkCreateSwapchainKHR`, `vkAcquireNextImageKHR` and `vkQueuePresentKHR` are referenced from it —
   which they cannot be without the define. It runs in the `native-build-renderer` job, and
   `native-render-backend-run.py` calls the same function on the copy it is about to link, so no run
   can report a flip an archive without a swapchain could not have performed. Negative control:
   pointed at `libsupport_windowbackend.a`, which has no Vulkan in it, the gate fails and says why.
2. `VulkanBackend::Present()` **refuses** when `swapchain_ == VK_NULL_HANDLE`: it prints once that a
   present with no swapchain presents nothing, and returns false. A windowed init that was asked for
   presentation and has no swapchain path fails instead of continuing. The headless spike now calls
   `End_Scene(false)` rather than `End_Scene(true)`, so it no longer claims a flip it cannot do; its
   proof was always the framebuffer readback, and still is.

The engine side is observable rather than assumed: `DX8Wrapper::Get_FrameCount()` advances only on a
successful present, so the harness records it before and after.

## 4. Where the engine went next, and the wall that appeared there

With D3DX routed, the engine got past texture creation and stopped one call later — still inside
`MissingTexture::_Init()`, but for a different reason:

```
Thread 1 "native_render_r" received signal SIGSEGV
#0  MissingTexture::_Init() at missingtexture.cpp:116
116     dst->Release();

src = 0x7ffff3da5a30   dst = 0x0   i = 1   tex = 0x7ffff317b710
locked_rect = {Pitch = 512, pBits = 0x555556d5c000}
rect = {left = 0, top = 0, right = 128, bottom = 128}
```

`D3DXCreateTexture` returned a texture, level 0 locked, `GetLevelCount()` reported a full mip chain —
and `tex->GetSurfaceLevel(1, &dst)` failed, because the backend served surface level 0 only
(`Get_Surface_Level: only level 0 is served`). The engine ignores that HRESULT, hands the null `dst`
to `D3DXLoadSurfaceFromSurface`, and crashes on `dst->Release()`.

**Classification: UNIMPLEMENTED PATH in the adapter, reached by the engine — now implemented for the
shape the engine asks for.** It is not a missing-data problem and not an engine defect (though the
unchecked HRESULT is a latent one, on Windows too, after `D3DERR_OUTOFVIDEOMEMORY`). The backend can
already give the engine what it wants here: mip levels above 0 are lockable through
`Lock_Texture`/`Unlock_Texture`, which `zh-resource-lock-tests` covers. What was missing was a
surface *name* for them.

So `VulkanD3DSurfaceClass` now distinguishes two kinds of surface, without inventing anything:

```cpp
bool Is_Mip_Level_Surface() const { return Handle == NULL && Container != NULL; }
```

- Level 0 still gets a real `spike::SurfaceHandle` from `Backend->Get_Surface_Level(Handle, 0)`.
- Levels above 0 get a wrapper with **no** surface handle, remembering its parent texture and its
  level. Its `LockRect`/`UnlockRect` delegate to `VulkanD3DTextureClass::LockRect(Level, ...)`, i.e.
  the already-classified texture-lock funnel. Level 0 is not aliased, no handle is fabricated, and no
  mip level is silently skipped.
- Because such a wrapper has no image view, `CopyRects` and `SetRenderTarget` **refuse** it through
  `Record_Unimplemented` — two new ledger entries rather than two paths that would appear to work.
- The deliberate refusal from #96 stands unchanged: a `LockRect` with a sub-rect on a handle-backed
  surface still **fails**, because `Surface_Bits` locks whole surfaces only. It was not widened to
  make this run pass; the engine's sub-rect lock here is on a *texture level*, which is a different
  funnel with its own bounds.

`d3d8-lock-scan.py --check` is unchanged at 100 classified sites in 9 classes, and
`surface-lock-audit.py --check` matches the committed audit: the new path routes into existing
funnels rather than adding a lock.

## 5. The frame, and exactly what was verified about it

```sh
CLANGXX=clang++-14 python3 scripts/native-render-backend-run.py \
    --keep --validation --frame-png /tmp/engine-frame.png
```

```
WWPlatform::Window_Create                  ok
client size: 800x600 points
DX8Wrapper::Init                           ok
render devices: 1
  0: llvmpipe (LLVM 15.0.7, 256 bits)
WW3D::Set_Render_Device                    ok
frame 0: submitted / frame 1: submitted / frame 2: submitted
frames submitted                           ok
frames actually presented                  ok  (FrameCount advanced 3, device lost no)

== what was in the frame (read back from the colour target)
800x600, 480000/480000 pixels within 2 of the clear colour 26,51,115
centre pixel rgba = 25,51,114,0; channel range r 25..25 g 51..51 b 114..114
wrote /tmp/engine-frame.png
frame contents are the engine's clear colour ok

== the unimplemented-call ledger (each entry is a finding, not a fallback)
(empty: every D3D8 entry point the engine reached is implemented)

validation messages: 0
validation layer silent                    ok

DX8Wrapper::Shutdown                       ok
stages failed: 0
harness exit code: 0
```

The point of this project is that "it presented" is not a result, so the harness asserts four
separate things and each can fail on its own:

| Claim | How it is observed | Would catch |
|---|---|---|
| the engine's own path ran | `DX8Wrapper::Init` → `Set_Render_Device` → `Begin_Scene`/`Clear`/`End_Scene` | a mock or a spike-only path |
| a present actually happened | `Get_FrameCount()` advanced by exactly 3, `Is_Device_Lost()` false | `Present()` returning success without a swapchain |
| the frame held what was asked for | `Read_Back_Color_Target()`, every one of 480,000 pixels within 2/255 of the requested clear colour, centre pixel and per-channel min/max printed | a black window, a stale frame, a wrong colour space, a partial clear |
| the Vulkan was legal | validation layer requested, `Validation_Message_Count()` **0** | a layout or barrier bug the pixels do not show |

Negative control, `--no-present`: the same run reports `no frame presented without a flip ok
(FrameCount advanced 0)` — so the frame-count assertion is measuring the flip, not counting loops.

**What this frame is not.** It is the engine's clear colour, produced by the harness's three-frame
loop; it is **not** a campaign or skirmish scene, and no `.big` from the retail archive was loaded in
this run (the harness deliberately does not call `WW3D::Init()`, which needs a retail install). Two
further honesty limits:

- 25,51,114 against a requested 26,51,115 is the sRGB rounding of the 0.10/0.20/0.45 float clear, not
  a tolerance being used to hide a mismatch: the channel range is a single value per channel, so
  every pixel is identical.
- The readback is of `color_target_`, which `Present()` blits into the acquired swapchain image. So
  the proof is "the image that was blitted into the swapchain contained exactly this", plus "the
  present succeeded and the frame count advanced" — not a capture of the window's front buffer. A
  compositor-level capture is the next rung, and on this box there is no compositor.

The path from here to a scene is `WW3D::Init()` and the asset load, which is a separate slice: this
one ends at the first honest frame through the engine's renderer.

## 6. What only Apple Silicon can decide

- Whether the routed D3DX helpers, the mip-level surface path and the swapchain define get the same
  frame through MoltenVK. Everything above is lavapipe; the `CAMetalLayer` surface and the swapchain
  blit are only *compiled* here.
- Whether the frame is correct at `backingScaleFactor` 2.00. The points/pixels boundary is asserted
  here at 1.00x only (800x600 points, 800x600 pixels), so the interesting case is untested from here.
- Whether MoltenVK accepts the mip-level texture locks with the same layouts. lavapipe's validation
  layer is silent; MoltenVK's is a different implementation of the same rules, and the
  `ZH_SPIKE_NO_VIEW_SWIZZLE=1` variants pass here precisely because that difference has bitten before.

## 7. Verification actually run

| Gate | Result |
|---|---|
| `check-spike-render.py --tolerance 2 --max-differing 0` | 0/480000 pixels differ; worst channel delta 0 |
| `zh-feature-probe` | OK, 0 validation messages |
| `zh-fixedfunc-tests --validation` | 0 failed, 6 pending; 0 validation messages |
| `zh-resource-lock-tests --validation` | 0 failed, 0 skipped; 0 validation messages |
| both with `ZH_SPIKE_NO_VIEW_SWIZZLE=1` | same |
| `check-staging-cost.py --self-check` | within the ceiling in both swizzle modes; the ceiling still rejects the pre-pool behaviour |
| `d3d8-lock-scan.py --check` | 100 sites, 100 classified, 9 classes |
| `surface-lock-audit.py --check` | matches the committed audit |
| `check-d3d8-surface.py` | direct call surface matches the allowlist exactly (4 sites, all in `d3dx8texcreate.cpp`); seam-owned 66 |
| `check-backend-coverage.py` | matches the committed baseline exactly |
| `native-build.py --level 1..4 --with-shims --strict-link` | **978/978** objects, 0 unresolved, executable produced |
| `d3dx8texcreate_test.cpp` | 102 checks, 0 failures, sanitiser probe clean |
| `native-render-backend-run.py --keep --validation --frame-png` | 0 stages failed; the table in §5 |
| `native-render-backend-run.py --no-present` | 0 stages failed; FrameCount advanced 0 |
| `check-swapchain-compiled.py` | OK on the built archive; fails on a Vulkan-less archive (negative control) |
| flake8, actionlint, `check-generated-baselines.py` | clean |

The Windows build and the replay gate were not run: this diff adds no engine behaviour on Windows —
`D3D8RenderBackendClass`'s three new methods forward to the same `IDirect3DDevice8` calls the D3DX
helpers made directly before, and `Resolve_Target` returns the passed device unchanged when it is
non-null, which is always the case on Windows. Nothing in serialisation, simulation or game execution
is touched.
