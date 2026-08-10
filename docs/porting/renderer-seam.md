# The renderer backend seam

`DX8Wrapper` now talks to an abstract `RenderBackendClass` instead of to
`IDirect3DDevice8` / `IDirect3D8` directly. There is exactly one implementation,
`D3D8RenderBackendClass`, which contains the D3D8 calls that used to be expanded inline from
the `DX8CALL` macros. No Vulkan code is involved; this document describes the socket, not a
second plug.

Files:

| File | Lines | What it is |
|---|---:|---|
| `Core/Libraries/Source/WWVegas/WW3D2/renderbackend.h` | 209 | the interface: 70 pure virtuals |
| `Core/Libraries/Source/WWVegas/WW3D2/d3d8renderbackend.h` | 192 | the D3D8 implementation's declaration, `#ifdef _WIN32` |
| `Core/Libraries/Source/WWVegas/WW3D2/d3d8renderbackend.cpp` | 460 | the D3D8 implementation, `#ifdef _WIN32` |

## 1. Where the seam sits, in numbers

Before this slice (measured by `spikes/renderer/tools/d3d8-surface-scan.py`, the state
`docs/porting/renderer-surface.md` §0.5 records):

| | Count |
|---|---:|
| D3D8 call sites in the engine | 155 |
| ...of which outside `dx8wrapper.{h,cpp}` | 0 |
| distinct `IDirect3DDevice8` methods reached | 53 |
| distinct `IDirect3D8` methods reached | 10 |
| device-level call sites (`DX8CALL`, `DX8CALL_HRES`, `DX8CALL_RAW*`) | 129 |
| adapter-level call sites (`DX8CALL_D3D`, `DX8CALL_RAW_D3D*`) | 26 |

After this slice, the same scan reports:

| | Count |
|---|---:|
| call sites reaching the backend through the `DX8CALL*` macros | 155 |
| direct D3D8 calls inside `d3d8renderbackend.cpp` (seam-owned) | 64 |
| direct D3D8 calls anywhere else | 0 |

The 155 macro sites did not move and their spelling did not change: the `DX8CALL*` macros now
expand to `DX8Wrapper::Get_Render_Backend()->x` instead of
`DX8Wrapper::_Get_D3D_Device8()->x`. That is deliberate — it keeps the diff to the ~13.5 kLOC
of `dx8*` files small enough to review, and it keeps the per-call-site instrumentation
(`DX8_Assert()`, `DX8_ErrorCode()`, `Increment_DX8_CallCount()`) exactly where it was.

The backend's 64 direct calls are 53 device methods + 11 `IDirect3D8` methods
(`CreateDevice` joins the 10 previously counted, because creating the device is now the
backend's job rather than the wrapper's). One direct call per method — the implementation is a
flat forwarding layer with no logic in it.

Two further wrapper call sites reach the backend without a macro:
`DX8Wrapper::_Create_DX8_ZTexture` calls `Get_Render_Backend()->CreateTexture` twice. Those
were `DX8Wrapper::_Get_D3D_Device8()->CreateTexture` before, deliberately not `DX8CALL`
because they inspect the `HRESULT` themselves and treat `D3DERR_NOTAVAILABLE` /
`D3DERR_OUTOFVIDEOMEMORY` as expected outcomes. They must not assert and must not bump the
call counter, so they stay outside the macros. The surface scanner had not been counting them
at all: it required the opening `(` on the same line as the method name, and both of these wrap
it onto the next line. That regex is fixed in this slice; no other site was hiding behind it.

## 2. Shape of the interface

The interface is `RenderBackendClass`, one virtual per D3D8 method the wrapper reaches,
keeping the D3D8 method name and argument list. Groups:

| Group | Methods | Notes |
|---|---:|---|
| backend/device lifecycle | 7 | `Open`, `Release_Interface`, `Free_Library`, `Has_Interface`, `Create_Device`, `Release_Device`, `Has_Device` |
| frame | 4 | `BeginScene`, `EndScene`, `Clear`, `Present` |
| device state | 12 | render states, texture stage states, textures, transforms, viewport, material, lights, clip planes |
| shaders and constants | 8 | vertex/pixel shader create/delete/set + constants |
| resource creation and copies | 7 | textures, vertex/index buffers, image surfaces, swap chain, `UpdateTexture`, `CopyRects` |
| submission | 6 | stream/index binding, `DrawPrimitive*`, `ProcessVertices` |
| render targets and buffers | 5 | `Get/SetRenderTarget`, depth stencil, front/back buffer |
| status, caps, memory | 7 | `TestCooperativeLevel`, `Reset`, `ValidateDevice`, texture memory, `D3DCAPS8`, display mode |
| cursor and gamma | 4 | |
| adapter enumeration and format probes | 10 | the `IDirect3D8` surface |

Only the seven lifecycle entry points do not mirror a D3D8 method, because device *ownership*
is what actually moved. `Create_Device` takes no out parameter: the backend keeps the device.
`Open` / `Release_Interface` / `Free_Library` are three separate calls rather than
open/close pairs because `DX8Wrapper::Shutdown` releases the interface, then drops cached
texture references and caps, and only then frees the library; collapsing them would reorder
shutdown.

Names were deliberately *not* translated into engine vocabulary. Keeping `SetRenderState` as
`SetRenderState` makes the mapping from each of the 155 call sites to a backend entry point
one-for-one auditable, and makes it impossible for the move to silently change an argument.
Renaming is a mechanical follow-up that is better done once a second backend exists to say
what the vocabulary should be.

`spikes/renderer/src/render_backend.h` (the Vulkan spike's sketch) has ~20 methods and served
9 test cases. It is not a subset relationship: the spike has nothing for adapter enumeration,
cursor, gamma, shader constants, render targets, `CopyRects`/`UpdateTexture`,
`ProcessVertices`, `ValidateDevice` or the uncached probes, all of which the engine uses.

## 3. The hot path: what is virtual and what is not

Every method on `RenderBackendClass` is virtual, so every backend call costs one indirect
call. That is acceptable only because the wrapper does not reach the backend on the path that
matters:

```cpp
WWINLINE void DX8Wrapper::Set_DX8_Render_State(D3DRENDERSTATETYPE state, unsigned value)
{
	if (RenderStates[state]==value) return;     // no virtual call, no D3D8 call

	RenderStates[state]=value;
	DX8CALL(SetRenderState( state, value ));    // virtual call, as before a COM call
	DX8_RECORD_RENDER_STATE_CHANGE();
}
```

`Set_DX8_Render_State`, `Set_DX8_Texture_Stage_State` and `Set_DX8_Texture` are called
hundreds of times per frame and usually return at the shadow-state comparison. Those
comparisons, the arrays they read, the `WWINLINE`, and the early return are unchanged and
remain on the `DX8Wrapper` side. The backend is entered only where the previous code was about
to issue a real D3D8 call — i.e. where it was already paying for a COM vtable dispatch, which
is itself an indirect call. So the added cost on a state change is one extra indirect call on
top of one that was already there, and the cost on a redundant state set is zero.

The virtual methods that *are* on a per-draw path are `SetStreamSource`, `SetIndices`,
`DrawIndexedPrimitive`, `DrawPrimitive`, `SetVertexShader`, `SetPixelShader`,
`SetVertexShaderConstant` and `SetTransform`. Each of these already crossed a COM vtable per
call before the refactor and is called once per draw call, not once per state set, so one
additional indirect call per invocation is not measurable next to the driver-side work each
one triggers.

Instrumentation is untouched: `DX8_RECORD_RENDER_STATE_CHANGE`,
`DX8_RECORD_TEXTURE_STAGE_STATE_CHANGE`, `DX8_RECORD_TEXTURE_CHANGE`, `DX8_RECORD_DX8_CALLS`,
`DX8_RECORD_DRAW_CALLS`, `Increment_DX8_CallCount` and the matrix/material/light counters all
still live in `dx8wrapper.h`, on the wrapper side, and are still evaluated at the same points.

### `_Uncached` pass-throughs

`Set_DX8_Render_State_Uncached`, `Get_DX8_Render_State_Uncached`,
`Set_DX8_Texture_Stage_State_Uncached` and `Set_DX8_Texture_Uncached` exist because their call
sites (e.g. `W3DShaderManager`, the shadow renderers, screenshot/gamma paths) deliberately
bypass the shadow cache, poke the device, and restore the previous value afterwards. If they
wrote to the shadow arrays, the cache would then believe the restored value was still set and
the next cached set would be skipped, changing what is rendered.

They therefore remain separate: they call the backend unconditionally and never touch
`RenderStates` / `TextureStageStates` / `Textures`. Structurally that means the seam has two
kinds of state entry point — cached (compare, maybe call) and uncached (always call) — and a
second backend must not "simplify" that into one.

## 4. Lifecycle, device loss and reset

The backend instance is a single file-scope object, `TheD3D8RenderBackend`, and
`DX8Wrapper::RenderBackend` is a `RenderBackendClass*` initialised to point at it at static
initialisation time. It is not created in `Init` and not destroyed in `Shutdown`, because
`DX8Wrapper` is all-static and the engine asks it "is there a device yet?"
(`_Get_D3D_Device8()` as a boolean, see §6) both before `Init` and after `Shutdown`. Device
lost / reset / release changes state *inside* the backend; it never changes which backend is
installed.

Ordering, unchanged from before the seam:

| Step | Before | Now |
|---|---|---|
| `Init(lite=false)` | `LoadLibrary("D3D8.DLL")`, `GetProcAddress("Direct3DCreate8")`, `DbgHelpGuard` around `Direct3DCreate8`, store `IDirect3D8*`, `IsInitted=true`, `Enumerate_Devices()` | `RenderBackend->Open()` does the first four steps; the rest is unchanged |
| `Create_Device` | `GetDeviceCaps` / `GetAdapterIdentifier` probes, pick vertex processing, `DbgHelpGuard`, `CreateDevice`, retry with `D3DFMT_D16` on failure, `Do_Onetime_Device_Dependent_Inits()` | same, with the three D3D8 calls going through the backend; the `DbgHelpGuard` stays in `Create_Device` so that it still covers *both* the initial attempt and the `D3DFMT_D16` retry |
| `Reset_Device` | invalidate textures, unbind streams/indices, `m_pCleanupHook->ReleaseResources()`, deinit dynamic VB/IB, release textures, shut down shaders, zero shader constants, `TestCooperativeLevel` (raw), `Reset`, recreate textures, `ReAcquireResources`, invalidate cache, restore default states, re-init shaders | identical; only the `D3DDevice != nullptr` guard became `RenderBackend->Has_Device()` and the two D3D8 calls go through the backend |
| `Release_Device` | unbind textures and streams, `Do_Onetime_Device_Dependent_Shutdowns()`, `D3DDevice->Release()` | `RenderBackend->Release_Device()` performs the release; the unbinding and shutdowns stay in the wrapper, in the same order |
| `Shutdown` | `Set_Render_Target(nullptr)` + `Release_Device()` if a device exists, release `IDirect3D8`, delete caps, clear device tables, `FreeLibrary` | `Release_Interface()` and `Free_Library()` at the same two points |

The awkward part is `Reset_Device`, and the seam did not make it less awkward: it is a
wrapper-level orchestration of five subsystems (`WW3D`, `DynamicVBAccessClass`,
`DynamicIBAccessClass`, `DX8TextureManagerClass`, the shader system) around two device calls.
That orchestration is engine policy, so it stayed in `DX8Wrapper`; the backend only exposes
`TestCooperativeLevel` and `Reset`. A Vulkan backend has no equivalent of a lost device, but
it does have `VK_ERROR_OUT_OF_DATE_KHR` on swapchain resize, and this same
invalidate/recreate sequence is what it will have to drive — from the wrapper side, with
`TestCooperativeLevel` reporting whatever the swapchain's state maps onto.

`Reset_Device` reads `TestCooperativeLevel` through `DX8CALL_RAW_HRES`, i.e. without
asserting, because a lost device is an expected answer. That distinction between checked and
unchecked calls is preserved by having the backend return `HRESULT`s and never assert; all
assertion and logging policy is on the wrapper side.

## 5. Resources: measured, and deliberately not abstracted

`DX8Wrapper` hands out raw `IDirect3DTexture8*`, `IDirect3DSurface8*`,
`IDirect3DVertexBuffer8*` and `IDirect3DIndexBuffer8*`, and the engine stores them in
`TextureClass`, `SurfaceClass`, `DX8VertexBufferClass` etc. and calls methods *on the
resources*, which the 155-site device scan does not cover. Measured with
`spikes/renderer/tools/d3d8-resource-scan.py` (added in this slice):

**213 call sites on D3D8 resource interfaces, across 19 files.** Of those, 71 are reference
counting (`AddRef` / `Release` / `QueryInterface`) and **142 are real resource operations**
(lock, unlock, describe, get sub-resource, set LOD/priority).

| Interface | Sites | Distinct methods | Methods |
|---|---:|---:|---|
| `IDirect3DSurface8` | 84 | 5 | `Release` 34, `LockRect` 17, `UnlockRect` 16, `GetDesc` 13, `AddRef` 4 |
| `IDirect3DTexture8` | 50 | 8 | `GetSurfaceLevel` 17, `GetLevelCount` 12, `Release` 7, `AddRef` 5, `GetLevelDesc` 3, `SetLOD` 3, `UnlockRect` 2, `LockRect` 1 |
| `IDirect3DVertexBuffer8` | 33 | 3 | `Lock` 16, `Unlock` 12, `Release` 5 |
| `IDirect3DIndexBuffer8` | 25 | 3 | `Lock` 13, `Unlock` 9, `Release` 3 |
| `IDirect3DBaseTexture8` | 14 | 5 | `Release` 10, `AddRef` 1, `GetLevelCount` 1, `GetPriority` 1, `SetPriority` 1 |
| `IDirect3DVolumeTexture8` | 4 | 3 | `Release` 2, `GetLevelDesc` 1, `UnlockBox` 1 |
| `IDirect3DCubeTexture8` | 2 | 2 | `GetLevelDesc` 1, `UnlockRect` 1 |
| `IDirect3DSwapChain8` | 1 | 1 | `GetBackBuffer` 1 |

Top files:

| File | Sites |
|---|---:|
| `WW3D2/texture.cpp` | 37 |
| `WW3D2/dx8wrapper.cpp` | 24 |
| `W3DDevice/GameClient/TerrainTex.cpp` | 23 |
| `WW3D2/textureloader.cpp` | 22 |
| `WW3D2/surfaceclass.cpp` | 20 |
| `W3DDevice/GameClient/Shadow/W3DProjectedShadow.cpp` | 20 |
| `W3DDevice/GameClient/Shadow/W3DVolumetricShadow.cpp` | 14 |
| `WW3D2/dx8vertexbuffer.cpp` | 12 |
| 11 further files | ≤ 7 each |

**Decision: resources are not abstracted in this slice.** The seam operates on D3D8 resource
pointers. Reasons, in order:

1. 213 sites in 19 files is larger than the 155-site device surface this slice is replacing,
   and it is spread through asset loading, terrain, shadows and the texture manager rather
   than concentrated in one wrapper.
2. 71 of them are COM reference counting embedded in the ownership model of `TextureClass`,
   `SurfaceClass` and friends. Replacing that with a handle model is a lifetime redesign of
   the asset layer, not a mechanical substitution, and it is the part most likely to introduce
   leaks or use-after-free that only show up on a reset.
3. The lock/unlock sites (88 of the 142 operations) hand out a raw pointer and a pitch and let
   the caller write texels or vertices in place. A backend-neutral version of that has to
   decide staging-buffer policy, which is a Vulkan design question that should be answered by
   the Vulkan backend, not guessed at now.

A half-abstracted resource model would be worse than an honest un-abstracted one, so
`IDirect3DTexture8*` and friends appear in `renderbackend.h` as-is. Making them opaque is a
separately scoped slice, and the numbers above are its estimate.

## 6. Remaining holes in the seam, stated plainly

**`_Get_D3D_Device8()` / `_Get_D3D8()` still exist.** They now read through the backend
(`TheD3D8RenderBackend.Peek_D3D_Device8()`), but they still return raw D3D8 pointers. 26 sites
remain in `Core/` and `GeneralsMD/` outside `dx8wrapper.{h,cpp}`. 22 of them are
device-presence tests (`if (!DX8Wrapper::_Get_D3D_Device8()) return;`,
`DEBUG_ASSERTCRASH(..., "without device")`) that a `Has_Device()` predicate would satisfy
directly; the other four are `dx8webbrowser.cpp`, which passes the device into the embedded
browser control, and three sites in the WorldBuilder tools. Replacing the presence tests is easy but touches
engine files, which this slice was told not to do.

**`Generals/` (the base game) still has 5 direct device calls** —
`_Get_D3D_Device8()->TestCooperativeLevel()` and `->GetRenderState()` in `W3DScene.cpp`,
`W3DShroud.cpp`, `W3DDisplay.cpp`, `W3DVolumetricShadow.cpp`, `ww3d.cpp`. `Generals/` is
outside the scanner's scope (`in_scope()` excludes it) and outside the port's scope so far, so
they were left alone. The equivalent Zero Hour sites already go through
`DX8Wrapper::Test_Cooperative_Level()`.

**D3DX sits above the seam, not inside it.** 121 D3DX sites, 20 distinct D3DX entities, across
13 files in `Core/` + `GeneralsMD/` (excluding tools). They split cleanly:

| Kind | Sites |
|---|---:|
| matrix/vector maths (`D3DXVECTOR4`, `D3DXMatrix*`, `D3DXVec*`) | 93 |
| texture/surface helpers (`D3DXCreateTexture` ×4, `D3DXCreateCubeTexture` ×4, `D3DXFilterTexture` ×5, `D3DXLoadSurfaceFromSurface` ×3, `D3DXCreateVolumeTexture` ×2, `D3DXCreateTextureFromFileExA`) | 19 |
| shader assembly (`D3DXAssembleShader`) | 4 |
| odds and ends (`D3DXGetFVFVertexSize` ×3, `D3DXGetErrorStringA` ×2) | 5 |

The texture helpers live in `dx8wrapper.cpp` and take the device as their first argument —
which is the other reason the raw accessor survives. D3DX is deliberately *not* in
`RenderBackendClass`: it is a helper library layered on top of D3D8, and its halves want
opposite treatment. The maths (93 sites) is pure CPU code with no
device involvement and should become `WWMath` calls, independent of any backend. The texture
helpers (`D3DXCreateTexture`, `D3DXFilterTexture`, `D3DXLoadSurfaceFromSurface`,
`D3DXCreateTextureFromFileExA`) do mip generation, format conversion and file decoding; a
second backend must reimplement that functionality, so it belongs in the resource slice above,
not as `virtual HRESULT D3DXCreateTexture(...)` on the device interface.

**Capabilities stay D3D8-shaped.** `DX8Caps` stores a `D3DCAPS8` and the engine branches on
its fields (`MaxSimultaneousTextures`, `TextureCaps`, `PixelShaderVersion`,
`MaxActiveLights`, ...) in device code, shader code and options UI. Making caps
backend-neutral means designing a neutral capability vocabulary *and* rewriting every
consumer, which is a bigger diff than this whole slice. So `RenderBackendClass::GetDeviceCaps`
fills in a `D3DCAPS8` and a future backend synthesises one. The one change made here is that
`DX8Caps` no longer stores an `IDirect3D8*`: it held one purely to pass around and never
called anything through it, so the member and the constructor parameter are gone (two
constructors, two call sites). Format support probing still goes through the wrapper's
`DX8CALL_D3D`/`DX8CALL_RAW_D3D` path onto the backend's `CheckDeviceFormat` etc.

**Nothing prevents a new direct D3D8 call in a *new* file except CI.**
`scripts/ci/check-d3d8-surface.py` gates direct calls at 0. The scanner now classifies calls
inside `d3d8renderbackend.cpp` as kind `backend` rather than `direct`, via a fixed
`SEAM_FILES` list of implementation files rather than a numeric allowlist budget, so a D3D8
call added to any other file still fails the gate. The gate additionally fails if the backend
implementation stops containing D3D8 calls at all, which would mean the file was renamed and
the scanner had gone blind.

## 7. What a second backend still has to solve

Concretely, the things this seam does *not* answer, and which a Vulkan/MoltenVK backend has to
deal with:

1. **Resources.** 213 sites, 142 operations, 19 files (§5). Lock/unlock semantics on textures,
   vertex and index buffers with a raw pointer and a pitch; `GetSurfaceLevel` handing out a
   sub-resource that the engine reference-counts independently of its parent; `GetDesc` /
   `GetLevelDesc` returning D3D8 descriptors. Until this is done, a second backend must
   produce objects that implement the `IDirect3D*8` vtables, which is only sane for a
   translation layer (e.g. DXVK-style), not for a native backend.
2. **D3DX texture work.** Mip generation, format conversion, surface blitting and file
   decoding (§6). No Vulkan equivalent exists; it must be implemented or replaced with the
   engine's own DDS/TGA paths.
3. **Caps.** Synthesise a `D3DCAPS8` (§6), including `PixelShaderVersion` /
   `VertexShaderVersion`, which the shader manager compares against fixed values to choose
   between rendering paths.
4. **Shaders.** `CreateVertexShader`/`CreatePixelShader` take DirectX 8 shader assembly
   token streams (assembled by `D3DXAssembleShader`, 4 sites) and vertex declarations as
   `DWORD` streams. A second backend needs a translation step to SPIR-V, or the shaders have
   to be ported by hand. The fixed-function path (texture stage states, `SetMaterial`,
   `SetLight`, `LightEnable`, 12 device-state methods) needs an equivalent shader-based
   emulation, which is the single largest piece of work.
5. **Device loss and reset.** The `Reset_Device` orchestration in §4 has to be driven by
   something; on Vulkan the trigger is swapchain out-of-date/resize, not a lost device, and
   `TestCooperativeLevel` has to be given a sensible mapping.
6. **`DX8Wrapper`'s deferred state model.** The wrapper batches material, texture, shader and
   light state and applies it in `Apply_Render_State_Changes`. It is written against D3D8's
   stateful model; a backend with pipeline objects has to reconstruct pipelines from that
   state, which is where the seam's method-per-D3D8-call shape will start to hurt and where
   renaming/regrouping the interface should happen.
7. **The remaining holes.** `_Get_D3D_Device8()` (26 in-scope uses), the 5 direct calls in
   `Generals/`, and `dx8webbrowser.cpp` passing the device to an ActiveX control — the last of
   which is Windows-only by nature and simply will not exist on a native macOS/Linux backend.
