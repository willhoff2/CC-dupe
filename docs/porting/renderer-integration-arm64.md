# The engine's own renderer path on Apple Silicon: how far it gets

This is the Apple Silicon half of the render-backend slice. The Linux half — `RenderBackendClass`'s
second implementation, `VulkanRenderBackendClass` over the reusable `spikes/renderer` backend — is
described in `docs/porting/renderer-integration.md`; that document is a lavapipe measurement and says
so. Everything below was measured by running the engine's own code path on the hardware, with the
game's own allocator, and nothing stubbed to make a result appear.

Hardware and toolchain, measured on the box rather than quoted:

| | |
|---|---|
| host | Apple M1 Pro, 16 GiB, macOS 26.6.1 (25G76), `arm64-apple-darwin25.6.0` |
| compiler | Apple clang 21.0.0 (`clang-2100.1.1.101`) |
| driver | MoltenVK 1.4.2, Vulkan headers 1.4.357.0, validation layers 1.4.357.0, glslang 16.5.0 |
| symbol rename | Homebrew LLVM 22.1.8 `llvm-objcopy`/`llvm-nm` (cctools has no `--redefine-sym`) |

Native build, levels 1-4 shimmed with `--strict-link`: **977/978** objects, one compile failure
(`WWLib/regexpr.cpp`, GNU-only `reg_syntax_t` — the same macOS-only failure
`docs/porting/first-native-run-arm64.md` §1 records, unchanged and not worked around), strict link
clean, executable produced. The denominator moved from 977 to 978 because this slice adds one engine
translation unit, `vulkanrenderbackend.cpp`.

## 1. What the macOS half of the harness got wrong

`scripts/native-render-backend-run.py`'s Darwin branch had never been executed. Four corrections
were needed; each is a finding about the platform, not a typo.

1. **MoltenVK's ICD is not where an SDK install puts it.** The script looked only under
   `share/vulkan/icd.d`. Homebrew's keg puts the manifest at
   `/opt/homebrew/opt/molten-vk/etc/vulkan/icd.d/MoltenVK_icd.json`. With the wrong path,
   `VK_ICD_FILENAMES` was never set, the loader found no driver, and the engine reported
   `Enumerate_Adapters: vkCreateInstance failed with VkResult -6` → `DX8Wrapper::Init FAILED`. The
   keg paths are now tried first, the SDK-style paths second.
2. **Homebrew's validation manifest names its dylib by leaf name.**
   `VkLayer_khronos_validation.json` has `"library_path": "libVkLayer_khronos_validation.dylib"`, so
   the loader can only open it with `/opt/homebrew/lib` on `DYLD_LIBRARY_PATH`. Without it the layer
   silently fails to load and "0 validation messages" would have meant "nothing was validated".
3. **`DYLD_*` does not survive into `lldb`'s inferior.** `lldb` is SIP-protected, so dyld strips
   every `DYLD_*` variable from the environment it passes on and the layer cannot be found however
   the debugger is invoked from a shell. The script grew a `--lldb` mode that sets
   `target.env-vars` inside the session instead, which is where the launched process reads it.
4. **`lldb -o` stops at the first signal.** The runbook's `lldb -b -o run -o bt -o "frame variable"`
   printed the crash and then exited without a backtrace: with `-o`, remaining commands are skipped
   once the process stops on a signal. `--lldb` writes a command file and uses `-s`, which does run
   `bt`. This is why the transcripts below have a backtrace and the first attempt did not.

A fifth defect is in the ladder rather than the harness: **`scripts/ci/check-spike-render.py` could
not load the validation layer on macOS**, and reported the honest but misleading
`the spike exited 1 / no image`. The cause is not the script's own environment — it is that a
`DYLD_LIBRARY_PATH` exported in the shell never reaches it, because the usual `python3` on this box
is a shim script run by `/bin/bash`, and `/bin/bash` is SIP-protected, so dyld drops the variable
before python starts. Verified directly: the same variable is present when pyenv's real interpreter
is exec'd and empty through `/bin/bash -c`. The gate now builds the child's `DYLD_LIBRARY_PATH`
itself, so it is meaningful regardless of which interpreter reached it.

## 2. How far the engine gets

`python3 scripts/native-render-backend-run.py --stop-after-init --validation`:

```
== the window (points; the renderer converts at its own boundary)
WWPlatform::Window_Create                  ok
client size: 800x600 points

== DX8Wrapper::Init -> RenderBackend->Open() -> Enumerate_Devices()
DX8Wrapper::Init                           ok
render devices: 1
  0: Apple M1 Pro
WW3D::Get_Render_Device_Count > 0          ok

== the unimplemented-call ledger (each entry is a finding, not a fallback)
(empty: every D3D8 entry point the engine reached is implemented)

== shutdown
DX8Wrapper::Shutdown                       ok

stages failed: 0
```

- **Window, points and pixels.** `Window_Get_Client_Size` answers **800x600 points** for an 800x600
  window at `backingScaleFactor` **2.00** — the convention holds at 2x, which
  `docs/porting/renderer-integration.md` §7 could only assert at 1x. The pixel conversion is where
  it belongs: measured inside `platform_window_cocoa.mm`, `contentsScale` is 2.00 and the
  `CAMetalLayer`'s `drawableSize` is **1600x1200 pixels**.
- **Adapter.** MoltenVK enumerates exactly one device and the engine's device name is
  **`Apple M1 Pro`** (lavapipe reported `llvmpipe (LLVM 15.0.7, 256 bits)` in the same place).
- **Ledger.** Empty, as on Linux: every D3D8 entry point the engine reached before its wall is
  implemented. That is a statement about how far it got, not about the table in §3 of the Linux
  document.
- **Validation.** Silent. With the layer actually loaded (correction 2), the run produced no `VUID`,
  `validation` or `error` line.

## 3. `CreateDevice` succeeds on MoltenVK — with the game's allocator

Measured under `lldb` on the normally linked binary (no `--stdlib-new`), breaking on
`VulkanRenderBackendClass::Create_Device`:

```
frame #0: VulkanRenderBackendClass::Create_Device(adapter=0, device_type=D3DDEVTYPE_HAL,
          focus_window=0x0000000d33805e50, behavior_flags=128,
          present_parameters=0x0000000100b95a60) at vulkanrenderbackend.cpp:1033
(D3DPRESENT_PARAMETERS) *present_parameters = {
  BackBufferWidth = 800          BackBufferHeight = 600
  BackBufferFormat = D3DFMT_A8R8G8B8              BackBufferCount = 1
  MultiSampleType = D3DMULTISAMPLE_NONE           SwapEffect = D3DSWAPEFFECT_DISCARD
  Windowed = YES                 EnableAutoDepthStencil = YES
  AutoDepthStencilFormat = D3DFMT_D24S8
}
Return value: (HRESULT) $0 = 0                      <- D3D_OK
(lldb) p DX8Wrapper::RenderBackend->Has_Device()          (bool) true
(lldb) p VulkanRenderBackendClass::Unimplemented_Call_Kinds()   (unsigned int) 0
```

**This is the decisive observation for §5 of the Linux document.** On lavapipe, `vkCreateDevice`
crashed inside LLVM's JIT because the engine replaces the global `operator new` with `GameMemory.cpp`'s
pool allocator, whose `MEM_BOUND_ALIGNMENT` is 4. On MoltenVK the same 4-byte-aligned allocator is
active and `vkCreateDevice` returns a device: the crash Linux saw is **lavapipe's LLVM JIT, not a
port defect**, and it does not block the later slices. Confirmed from the other side too:
`--stdlib-new`, which renames those four symbols in a scratch copy of `GameMemory.cpp.o`, changes
nothing about where this run stops — same wall, same backtrace, same frame `#0`.

The alignment is still a latent defect (C++17 requires `__STDCPP_DEFAULT_NEW_ALIGNMENT__`, 16 on
arm64), and it is still a memory-manager slice rather than a renderer one. What this measurement
removes is the possibility that it blocks the renderer on the target.

## 4. Where it stops: the same D3DX wall, one call earlier than the crash

`python3 scripts/native-render-backend-run.py --keep --validation --lldb`:

```
Process stopped: EXC_BAD_ACCESS (code=1, address=0x0)
  * frame #0: MissingTexture::_Init() at missingtexture.cpp:79
    frame #1: DX8Wrapper::Do_Onetime_Device_Dependent_Inits() at dx8wrapper.cpp:370
    frame #2: DX8Wrapper::Create_Device() at dx8wrapper.cpp:591
    frame #3: DX8Wrapper::Set_Render_Device(dev=0, width=800, height=600, bits=32, windowed=1,
              resize_window=false, reset_device=false, restore_assets=true) at dx8wrapper.cpp:1117
    frame #4: WW3D::Set_Render_Device(dev=0, width=800, height=600, bits=32, windowed=1, ...)
              at ww3d.cpp:446
    frame #5: main at native_render_run.cpp:168
(lldb) frame variable
(IDirect3DTexture8 *) tex = nullptr
(RECT) rect = (left = 0, top = 0, right = 128, bottom = 128)
```

The failing *call* is one frame further in, caught on its own breakpoint:

```
frame #0: D3DXCreateTexture(pDevice=0x0000000000000000, Width=128, Height=128, MipLevels=0,
          Usage=0, Format=D3DFMT_A8R8G8B8, Pool=D3DPOOL_MANAGED, ppTexture=0x000000016fdfe3f8)
          at d3dx8texcreate.cpp:372
frame #1: DX8Wrapper::_Create_DX8_Texture(width=128, height=128, format=WW3D_FORMAT_A8R8G8B8,
          mip_level_count=MIP_LEVELS_ALL, pool=D3DPOOL_MANAGED, rendertarget=false)
          at dx8wrapper.cpp:2434
frame #2: MissingTexture::_Init() at missingtexture.cpp:64
...
Return value: (HRESULT) $1 = -2005530516           <- 0x8876086C, D3DERR_INVALIDCALL
```

`_Get_D3D_Device8()` answers `nullptr` because there is no `IDirect3DDevice8` behind a non-D3D8
backend, `Create_Fitted()` refuses a null device with `D3DERR_INVALIDCALL` without calling anything,
`_Create_DX8_Texture` returns null, and `MissingTexture::_Init()` dereferences it unchecked.

**Classification: unimplemented renderer path exposed as a port defect in the seam's coverage — not
missing data, and not MoltenVK.** Identical to the Linux wall, for the same reason, and the fix is
the same next slice: route the `D3DX8TexCreate` helpers through `RenderBackendClass`, and make
`_Create_DX8_Texture`'s callers treat a null texture as the failure it is. Nothing about it is
Apple-specific, which is itself the result: the platform-specific layers (portability enumeration,
`CAMetalLayer` surface, swapchain) did not produce a wall of their own before it.

## 5. A new finding: the engine's backend is compiled without a swapchain

`Create_Device` returning `D3D_OK` does **not** mean the engine could present. `scripts/native-build.py`
compiles `spikes/renderer/src/vulkan_backend.cpp` into `libsupport_renderbackend.a` with
`SPIKE_SHADER_DIR` as its only definition — **`SPIKE_WITH_PLATFORM_WINDOW` is not defined**, so in the
engine's copy `Create_Instance` requests no surface extensions, `Create_Swapchain` and
`Build_Swapchain` compile to `return true`, and no `VkSurfaceKHR` is ever created for the Cocoa
window the harness makes. Objectively, from the archive the engine links:

```
$ nm -um build/native/libsupport_renderbackend.a | grep -i 'swapchain\|Vulkan_Surface'
(nothing; only _vkAcquireNextImageKHR is referenced, from the unguarded Present())
```

`VulkanBackend::Present()` opens with `if (swapchain_ == VK_NULL_HANDLE) return true;`, so past the
D3DX wall `DX8Wrapper::Present()` would have reported success while nothing reached the screen. That
is exactly the shape of result this slice is meant not to produce, so it is recorded here as the
second thing the next slices must fix, and no picture is claimed for the engine.

What *is* verified on this hardware is the renderer and the window seam it will use, separately:
`build/spike/zh-window-spike-cocoa` (the same `platform_window_cocoa.mm` the engine calls, compiled
*with* `SPIKE_WITH_PLATFORM_WINDOW`) creates the `CAMetalLayer` surface, presents 300 frames to its
swapchain, reads the colour target back, finds the geometry rather than the clear colour, and sees no
validation message. That is a spike result, not an engine result.

## 6. Second wall, after shutdown: the allocator during `exit`

The `--stop-after-init` run reports `stages failed: 0` and a clean `DX8Wrapper::Shutdown`, then dies
with signal 11 *after* the harness has finished, during process teardown. The backtrace is thousands
of frames of the same cycle:

```
CriticalSection::enter -> std::recursive_mutex::lock -> std::system_error -> std::string
  -> operator new -> DynamicMemoryAllocator::allocateBytesImplementation
  -> DynamicMemoryAllocator::allocateBytesDoNotZeroImplementation
  -> ScopedCriticalSection::ScopedCriticalSection -> CriticalSection::enter ...
...
libopenal.1.dylib std::basic_ofstream::~basic_ofstream -> __cxa_finalize_ranges -> exit -> dyld start
```

A static destructor in a dependency (OpenAL's, here) allocates after the game's memory manager has
been torn down; `recursive_mutex::lock` fails, and building the `system_error` string re-enters the
dead allocator, which locks again. This is an engine lifecycle defect, unrelated to `Create_Device`
and unrelated to the renderer — it is reported separately so the two are not conflated, and it is
why the harness's exit code is `-11` on a run whose stages all passed.

## 7. Verification actually run, on this hardware

| Gate | Result |
|---|---|
| `check-spike-render.py` vs `spikes/renderer/docs/spike-triangle.png` | **0/480000** pixels differ by more than 2/255; worst channel delta 1 at (400, 76); layer active and silent |
| `zh-feature-probe` | 9 cases, 0 failed, 0 validation messages |
| `zh-fixedfunc-tests --validation` | 0 failed, 6 pending, 0 validation messages |
| `zh-resource-lock-tests --validation` | 0 failed, 0 skipped, 0 validation messages |
| both of the above with `ZH_SPIKE_NO_VIEW_SWIZZLE=1` | same results, 0 validation messages |
| `zh-window-spike-cocoa` | 300 frames presented to the window's swapchain, readback contains the geometry, 0 validation messages |
| `check-d3d8-surface.py` + `check-backend-coverage.py` | pass, unchanged budgets |
| `flake8 --max-line-length=100 scripts/` | clean |

No new resource lock was introduced, so the C1-C9 table in
`spikes/renderer/tools/d3d8-lock-classes.json` is unchanged.

## 8. What could not be measured, and why

- **The swapchain path the engine would take.** It does not exist in the engine's build (§5), so
  there is nothing to measure; the extent, format and present mode reported here come from the spike
  built with the window backend, not from the engine.
- **The swapchain extent from inside the engine binary.** `libsupport_renderbackend.a` is compiled
  without `-g`, so `lldb` cannot resolve `vulkan_backend.cpp`'s locals in that process. The
  points→pixels conversion was measured on the Cocoa side instead (`drawableSize` 1600x1200 at
  `contentsScale` 2.00).
- **Anything past `MissingTexture::_Init()`.** The subsequent one-time inits (`TextureLoader::Init`,
  `TheDX8MeshRenderer.Init`, `PointGroupClass::_Init`, `ShatterSystem::Init`) are unreachable until
  the D3DX seam lands, so their walls remain unmeasured.
- **A presented engine frame, and therefore any screenshot of one.** Two independent reasons: the
  D3DX wall (§4) and the missing swapchain (§5).
- **`actionlint`** reports pre-existing shellcheck findings (SC2086, SC2129, SC2155, SC2001, SC2016,
  SC2126) in workflow files this slice does not touch; no workflow file was modified.
