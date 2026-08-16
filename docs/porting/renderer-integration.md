# Connecting the renderer to the engine, and where the engine then stops

`RenderBackendClass` is the seam under `DX8Wrapper` (`docs/porting/renderer-seam.md`). Until this
slice it had exactly one implementation, `D3D8RenderBackendClass`, compiled only under `_WIN32`, so
off Windows the pointer was `nullptr` by construction:

```cpp
#ifdef _WIN32
RenderBackendClass *DX8Wrapper::RenderBackend = &TheD3D8RenderBackend;
#else
RenderBackendClass *DX8Wrapper::RenderBackend = nullptr;
#endif
```

That is the null dereference `docs/porting/first-native-run-arm64.md` measured on an M1 Pro at
`dx8wrapper.cpp:307`, `if (!RenderBackend->Open())`. It was never a missing null check; it was the
missing second implementation. This slice writes it, from the renderer that
`docs/porting/renderer-surface.md` and the spike ladder already verify pixel-for-pixel.

Everything below is measured on this Linux x86-64 box against Mesa **lavapipe** (llvmpipe, LLVM
15.0.7), with `clang++-14`. §7 says what only an Apple Silicon Mac can decide.

## 1. What was written, and what moved

| File | Lines | What it is |
|---|---:|---|
| `Core/Libraries/Source/WWVegas/WW3D2/vulkanrenderbackend.h` | 218 | the non-Windows `RenderBackendClass` declaration, `#ifndef _WIN32` |
| `Core/Libraries/Source/WWVegas/WW3D2/vulkanrenderbackend.cpp` | 2156 | its implementation: a translation layer over `spike::RenderBackend` |
| `Core/Libraries/Source/WWVegas/WW3D2/tests/native_render_run.cpp` | 210 | the runtime harness: window → `DX8Wrapper::Init` → device → frames → ledger |
| `scripts/native-render-backend-run.py` | 228 | compiles, links and runs that harness against the native build's own archives |

**Nothing was copied.** `spikes/renderer/CMakeLists.txt` now builds its two backend sources
(`src/state_translate.cpp`, `src/vulkan_backend.cpp`) as a static library, `zh-render-backend`, and
every spike executable links it instead of compiling those sources again. `scripts/native-build.py`
compiles the *same two files* into a `libsupport_renderbackend.a` support archive and links it into
the engine, so the renderer the engine calls and the renderer the ladder verifies cannot diverge.
The spike gained public API (`Present()`, lockable vertex/index buffers, adapter enumeration) but no
second copy of anything exists.

Support archives are a platform dependency of the measured libraries rather than one of them, which
is how the window and audio backends are already handled, so the renderer does not enter the
objects / translation-unit denominators. The one new *engine* translation unit is
`vulkanrenderbackend.cpp`: levels 1-4 objects **977/977 → 978/978**, 0 compile failures, strict link
still clean at **0 unresolved symbols** with an executable produced (82.6 MiB, ELF 64-bit x86-64).

Windows is untouched at runtime: `vulkanrenderbackend.{h,cpp}` are wholly inside `#ifndef _WIN32`,
and the only change to `dx8wrapper.cpp` is which of the two backends the non-Windows branch of that
one static initialiser points at. The D3D8 path is still the oracle.

## 2. Shape of the translation layer

The seam speaks D3D8: COM interfaces, `HRESULT`, `D3DFORMAT`, `D3DLOCKED_RECT`. The spike speaks
opaque handles and `bool`. The adapter is the only place those two meet, and it owns the COM-shaped
resource wrappers the engine holds:

| Engine holds | Adapter class | Wraps |
|---|---|---|
| `IDirect3DTexture8*` | `VulkanD3DTextureClass` | one `spike::TextureHandle` and its mip metadata |
| `IDirect3DSurface8*` | `VulkanD3DSurfaceClass` | a level of a texture, or a standalone image surface |
| `IDirect3DVertexBuffer8*` | `VulkanD3DVertexBufferClass` | a `spike::VertexBufferHandle` |
| `IDirect3DIndexBuffer8*` | `VulkanD3DIndexBufferClass` | a `spike::IndexBufferHandle` (16-bit) |

Reference counting is the engine's, so it is real: `AddRef`/`Release` count, and the final `Release`
destroys the spike resource. The frame is a three-call mapping:

| `DX8Wrapper` | `spike::RenderBackend` |
|---|---|
| `Begin_Scene()` | `Begin_Scene()` |
| `End_Scene()` | `End_Scene(false)` |
| `Present()` | `Present()` — acquire, blit the colour target, transition to `PRESENT_SRC_KHR`, present |

## 3. The unimplemented-call ledger

Every D3D8 operation this backend cannot perform returns a D3D8 failure code and records itself.
`Record_Unimplemented()` logs the first occurrence, counts the rest, and the harness prints the
table at exit; `VulkanRenderBackendClass::Unimplemented_Call()` exposes it to any other consumer.
None of these returns `D3D_OK`, and none substitutes a different format, size or resource for the
one asked for — a wrong-pixels bug with no failing call to blame is worse than a failure.

| Entry point | Why it cannot be served |
|---|---|
| `IDirect3D8::CreateDevice(adapter selection)` | `spike::Create_Vulkan_Backend` picks the physical device itself |
| `IDirect3D8::GetDeviceCaps(non-HAL)` | the backend has no reference or software device type |
| `IDirect3D8::GetAdapterDisplayMode` | the platform reported no display bounds |
| `IDirect3DDevice8::Clear(rect list)` | `spike::RenderBackend::Clear` clears whole attachments |
| `IDirect3DDevice8::Present(sub-rect or other window)` | the swapchain presents the whole colour target to its own window |
| `IDirect3DDevice8::CreateAdditionalSwapChain` | the backend owns exactly one swapchain |
| `IDirect3DDevice8::Reset(new back buffer size)` | the swapchain is rebuilt but the colour target keeps its resolution |
| `IDirect3DDevice8::GetFrontBuffer` | the presented image cannot be read back |
| `IDirect3DDevice8::GetBackBuffer(index or stereo)` | there is one colour target and it is not stereo |
| `IDirect3DDevice8::ProcessVertices` | no CPU/compute vertex transform path |
| `IDirect3DDevice8::ResourceManagerDiscardBytes` | no managed pool to evict |
| `IDirect3DDevice8::CreateTexture(format)` | the `D3DFORMAT` has no `spike::TextureFormat` |
| `IDirect3DDevice8::CreateTexture(render target format)` | render-target textures are 8888 only |
| `IDirect3DDevice8::CreateTexture(unsupported format)` | the Vulkan device cannot sample the format |
| `IDirect3DDevice8::CreateImageSurface(format)` | the `D3DFORMAT` has no `spike::TextureFormat` |
| `IDirect3DDevice8::CreateVertexBuffer(FVF)` | the FVF is not one `spike::Decode_Fvf` decodes |
| `IDirect3DDevice8::CreateIndexBuffer(32-bit indices)` | the backend binds `VK_INDEX_TYPE_UINT16` only |
| `IDirect3DDevice8::CreateVertexShader(untranslatable)` | the `vs.1.1` token stream is not one it can translate |
| `IDirect3DDevice8::CreatePixelShader(untranslatable)` | the `ps.1.1` token stream is not one it can translate |
| `IDirect3DDevice8::LightEnable(TRUE)` | no separate enable for a previously set light |
| `IDirect3DDevice8::SetGammaRamp` | Vulkan exposes no display gamma ramp |
| `IDirect3DDevice8::ShowCursor`, `SetCursorProperties`, `SetCursorPosition` | the cursor belongs to the window seam, not the renderer |
| `IDirect3DSurface8::LockRect(sub-rect)` | `spike::RenderBackend::Surface_Bits` locks whole surfaces |
| `IDirect3D*8::GetDevice` (4 interfaces) | there is no `IDirect3DDevice8` object off Windows — §5 |
| `IDirect3D*8::{Set,Get}PrivateData` (4 interfaces) | no private-data store |

Two of these are load-bearing refusals rather than gaps:

- `CheckDeviceFormat` answers **no** for cube maps, which is how the engine is told not to call
  `CreateCubeTexture` — the seam has no cube or volume texture method at all, so a "yes" would be a
  lie the engine could not act on.
- `LockRect` with a sub-rect **fails** instead of quietly locking the whole surface. A partial lock
  that silently widens is exactly the class of bug that made #63 (a surface view whose layout
  diverged from its image) invisible to the compiler.

At the runtime wall of §4 the ledger is **empty**: every D3D8 entry point the engine actually
reached before stopping is implemented. That is a measurement of how far the engine got, not a claim
that the list above is unreachable.

## 4. How far the engine gets, and exactly where it stops

```sh
CLANGXX=clang++-14 python3 scripts/native-render-backend-run.py                     # the wall
CLANGXX=clang++-14 python3 scripts/native-render-backend-run.py --stop-after-init   # green, prints the ledger
```

The harness is the engine's own code path, not a mock: `WWPlatform::Window_Create` →
`DX8Wrapper::Init` → `WW3D::Get_Render_Device_Count/Name` → `WW3D::Set_Render_Device` →
`DX8Wrapper::Begin_Scene`/`Clear`/`End_Scene`/`Present` → `DX8Wrapper::Shutdown`. It does not call
`WW3D::Init()`, because the rest of what that does (the dazzle INI, the animated-sound manager)
needs a retail install this box has not got, and `DX8Wrapper::Init()` is the part of it this slice
is about.

Reached, in order:

```
WWPlatform::Window_Create                  ok
client size: 800x600 points
DX8Wrapper::Init                           ok          <- #87's EXC_BAD_ACCESS was here
render devices: 1
  0: llvmpipe (LLVM 15.0.7, 256 bits)
WW3D::Get_Render_Device_Count > 0          ok
```

so `RenderBackend->Open()`, `GetAdapterCount()`, `GetAdapterIdentifier()` and the mode enumeration
all work through the adapter, and `Window_Get_Client_Size` still answers in **points** (800x600 for
an 800x600 window) with the pixel conversion left at the renderer boundary.

Then `WW3D::Set_Render_Device(0, ...)` → `DX8Wrapper::Create_Device()`, which succeeds — caps,
adapter identifier and `CreateDevice` (a real `vkCreateDevice`, a real swapchain) all return
`D3D_OK` — and immediately calls `Do_Onetime_Device_Dependent_Inits()`, where it dies:

```
Thread 1 "native_render_r" received signal SIGSEGV, Segmentation fault.
#0  MissingTexture::_Init()
#1  DX8Wrapper::Do_Onetime_Device_Dependent_Inits()
#2  DX8Wrapper::Create_Device()
#3  DX8Wrapper::Set_Render_Device(int, int, int, int, int, bool, bool, bool)
#4  WW3D::Set_Render_Device(int, int, int, int, int, bool, bool, bool)
#5  main at Core/Libraries/Source/WWVegas/WW3D2/tests/native_render_run.cpp:168
```

The faulting instruction is `mov (%rdi),%rax` — the vtable load of `tex->LockRect(...)` in
`missingtexture.cpp` on a `tex` that is `nullptr`:

```cpp
IDirect3DTexture8* tex = DX8Wrapper::_Create_DX8_Texture(..., MIP_LEVELS_ALL);
D3DLOCKED_RECT locked_rect;
DX8_ErrorCode(tex->LockRect(0, &locked_rect, &rect, 0));   // <- tex is null
```

**First failing call, and why.** `_Create_DX8_Texture` does not ask the backend for the texture. It
calls `D3DXCreateTexture(DX8Wrapper::_Get_D3D_Device8(), ...)`, and `_Get_D3D_Device8()` is

```cpp
static IDirect3DDevice8* _Get_D3D_Device8()
{ return RenderBackend ? RenderBackend->Peek_D3D_Device8() : nullptr; }
```

which this backend answers with `nullptr`, because **there is no `IDirect3DDevice8` object behind a
non-D3D8 backend** — that is the point of the seam. `d3dx8texcreate.cpp`'s `Create_Fitted()` sees a
null device, returns `D3DERR_INVALIDCALL` without calling anything, and `_Create_DX8_Texture`
returns null. `MissingTexture::_Init()` then dereferences it without checking, so the visible
symptom is a null dereference in the engine and the actual defect is one layer down.

**Classification: a port defect in the seam's coverage, not an unimplemented renderer path and not
missing data.** The renderer can create this texture — `VulkanRenderBackendClass::CreateTexture`
handles `A8R8G8B8` with a full mip chain, and `zh-resource-lock-tests` locks and samples back
exactly this shape (C1, C4, C6). What is missing is a route from the *D3DX* creation entry points to
the backend: they are the one part of the D3D8 surface that still takes an `IDirect3DDevice8*`
(`check-d3d8-surface.py` budgets `d3dx8texcreate.cpp`'s 3 direct calls for exactly this reason).
That is the next slice, and it is a seam change rather than renderer work:

1. give the `D3DX8TexCreate` helpers a `RenderBackendClass*` entry point — its fitting logic already
   works from a `D3DCAPS8`, which the backend supplies — and point `dx8wrapper.cpp`'s D3DX call
   sites at it off Windows, re-budgeting the allowlist;
2. make `MissingTexture::_Init()` (and the other `_Create_DX8_Texture` callers) handle a null
   texture as the failure it is, since on Windows it can also be null after
   `D3DERR_OUTOFVIDEOMEMORY`;
3. then the one-time inits continue into `TextureLoader::Init`, `TheDX8MeshRenderer.Init`,
   `PointGroupClass::_Init` and `ShatterSystem::Init`, each of which is a new wall to measure.

Until (1) lands the engine cannot present a frame, so **this slice does not claim a picture on the
screen.** The three-frame `Begin_Scene`/`Clear`/`End_Scene`/`Present` loop in the harness is
reachable only past that wall; the spike's own presentation path is what is verified today, by the
ladder in §6.

## 5. The second finding: the engine's allocator and the driver

Before the texture wall, the default harness stops earlier, inside `vkCreateDevice`:

```
llvm::SelectionDAGISel::SelectionDAGISel(...)
llvm::MCJIT::finalizeObject()
libvulkan_lvp.so ... vkCreateDevice()
spike::VulkanBackend::Pick_Device() <- Init() <- VulkanRenderBackendClass::Create_Device()
```

The engine replaces the global `operator new`/`delete` with `GameMemory.cpp`'s pool allocator, whose
`MEM_BOUND_ALIGNMENT` is **4** (`GameMemory.cpp`, and `roundUpMemBound()` in `GameMemoryInit.cpp`).
lavapipe's LLVM JIT allocates through the global operator and does not survive 4-byte-aligned
returns on x86-64. `scripts/native-render-backend-run.py --stdlib-new` renames those four symbols in
a *scratch copy* of `GameMemory.cpp.o` so libstdc++'s allocator serves the run; that is how the
texture wall in §4 was reached, and it is a **diagnostic mode, not a fix** — nothing in the tree
changes, and no shipped configuration bypasses the game allocator.

What this does and does not establish:

- **Measured:** with the game allocator active, this driver crashes in its JIT during device
  creation; with libstdc++'s, the same code creates a device and a swapchain.
- **Not established:** that MoltenVK has the same problem. It is a Metal driver with no LLVM JIT in
  this path. Whether the engine's 4-byte alignment is a real constraint on the target, or only on
  lavapipe, is a Mac measurement (§7).
- Either way the alignment is a latent defect: C++17 requires `operator new` to return memory
  aligned to `__STDCPP_DEFAULT_NEW_ALIGNMENT__` (16 on x86-64 and arm64), and the engine's does not.
  Fixing that is a memory-manager slice, not a renderer one.

## 6. Verification actually run

Renderer ladder, on lavapipe with the validation layer loaded and silent:

| Gate | Result |
|---|---|
| `check-spike-render.py` vs `spikes/renderer/docs/spike-triangle.png` | **0/480000** pixels differ by more than 2/255; worst channel delta 0 |
| `zh-feature-probe` | 0 failures |
| `zh-fixedfunc-tests --validation` | 0 failed, 6 pending; 0 validation messages |
| `zh-resource-lock-tests --validation` | 0 failed, 0 skipped; 0 validation messages; 12 lock classes pass (C1-C6, C8, L8, pool recycling) |
| both of the above with `ZH_SPIKE_NO_VIEW_SWIZZLE=1` (MoltenVK's case) | same, 0 validation messages |
| `check-d3d8-surface.py` | direct D3D8 call sites **3**, allowlist 3 — exact; seam-owned 64 |
| `check-backend-coverage.py` | matches the committed baseline exactly (44/47 backend methods, 48 render states, 23 stage states, 17 cascade ops) |

Native build and probes, `clang++-14`, levels 1-4 shimmed with `--strict-link`: **978/978** objects,
0 compile failures, 0 unresolved symbols, executable produced. Probe unchanged at 671/760 native and
716/760 shimmed, both gated. `flake8 --max-line-length=100 scripts/` and `actionlint` clean.

No new resource lock was introduced in the spike by this slice, so the C1-C9 classification of
`spikes/renderer/tools/d3d8-lock-classes.json` is unchanged; the adapter's `LockRect`/`Unlock` paths
route into the already-classified `Surface_Bits` / buffer-lock funnels, and the C9 D3DX surface-lock
class stays where #85 put it — the D3DX creation entry points do not reach the backend yet (§4).

## 7. What only a Mac can decide

- Whether the engine reaches the same wall on MoltenVK, or an earlier one. The adapter is
  platform-neutral, but `VK_KHR_portability_enumeration`, the `CAMetalLayer` surface and the
  swapchain blit are only *compiled* here.
- Whether the allocator finding of §5 is lavapipe-specific.
- Whether the presented frame is correct at `backingScaleFactor` 2.00 once §4's next slice lands:
  the points/pixels boundary is asserted here only at 1.00x.

Nothing in this document should be read as an Apple Silicon result. `spikes/renderer` itself is
verified on an M1 Pro under MoltenVK 1.4.2 (`docs/porting/moltenvk-findings.md`,
`docs/porting/macos-hardware-verification.md`); the *engine calling it* is not, yet.

### 7.1 The outpost run, exactly

`scripts/native-render-backend-run.py` now selects Mach-O link flags, the Cocoa/QuartzCore/Metal/IOKit
frameworks, MoltenVK's ICD and LLVM's `objcopy`/`nm` when `sys.platform == "darwin"`. That selection
is written from the documented toolchain differences and has never been executed — this slice was
measured on Linux — so treat any correction it needs as part of the outpost's findings rather than as
an accident. Prerequisites: `brew install llvm molten-vk vulkan-headers glslang` (llvm because
cctools cannot rename a symbol, so the game's `main()` could not otherwise be moved aside).

```sh
./scripts/ci/fetch-probe-deps.sh
python3 scripts/native-build.py --level 1 --level 2 --level 3 --level 4 \
    --with-shims --strict-link

# the ledger first: this stops before the D3DX wall, so it prints what the engine asked for
python3 scripts/native-render-backend-run.py --stop-after-init --validation

# then the wall itself: --keep leaves the linked binary in place for lldb
python3 scripts/native-render-backend-run.py --keep --validation
lldb -b -o run -o bt -o "frame variable" -- build/native/render-run/native_render_run

# the allocator control (§5): does MoltenVK survive the pool allocator where lavapipe did not?
python3 scripts/native-render-backend-run.py --keep --validation --stdlib-new
```

What to record, in the shape §4 uses: window creation and the client size **in points**, MoltenVK's
adapter string, whether `CreateDevice` succeeds, the first failing call with its arguments and
`HRESULT`, the backtrace, the ledger contents, and whether `--stdlib-new` changes the outcome. The
last of those decides §5: if the normal allocator survives `vkCreateDevice` on MoltenVK, the
4-byte-alignment finding is lavapipe's LLVM JIT and not a port defect; if it crashes there too, it is
the engine's and it blocks every later slice.
