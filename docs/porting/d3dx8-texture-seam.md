# The D3DX 8 texture, FVF and shader entry points

`docs/porting/d3dx8-math-seam.md` covers the `D3DXMatrix*`/`D3DXVec*` half of `d3dx8.lib`. This
document covers the four non-math entry points the engine calls, which are not conventions to match
but behaviour to reproduce:

| Entry point | Called by | Definition (off Windows only) |
|---|---|---|
| `D3DXGetFVFVertexSize` | `dx8fvf.cpp` | `WW3D2/d3dx8fvf.cpp` |
| `D3DXFilterTexture` | `W3DTreeBuffer.cpp`, `texture.cpp` | `WW3D2/d3dx8texture.cpp` |
| `D3DXLoadSurfaceFromSurface` | `missingtexture.cpp`, `surfaceclass.cpp` | `WW3D2/d3dx8texture.cpp` |
| `D3DXAssembleShader` | `W3DWater.cpp` | `WW3D2/d3dx8shader.cpp` |

All four are compiled only off Windows: Windows links the real `d3dx8.lib`, and a duplicate symbol
would break the 13 Windows configurations. `scripts/native-port-shims/d3dx8core.h` is the vendored
declaration all four match.

The tests are `WW3D2/tests/d3dx8fvf_test.cpp`, `d3dx8texture_test.cpp` and `d3dx8shader_test.cpp`,
run by `scripts/native-d3dx8-entrypoints-test.py` and by the `native-port-ci` workflow: 101 + 101 +
14 checks. `native-build.py` drops their `main()` objects from the archives before linking, as it does
for the other standalone tests.

## 1. `D3DXGetFVFVertexSize` — one right answer per bitmask

A pure function of the FVF bits, so it is table-tested against the documented D3D8 layout rather than
against a behaviour. The two traps, both in the test:

- the texture-coordinate **count** is `(FVF & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT`, and
  each set's **dimension** is two bits of the high half starting at bit 16, with
  `D3DFVF_TEXCOORDSIZE2` as the zero default — so `D3DFVF_TEX2` alone is two sets of two floats, not
  two floats;
- `D3DFVF_TEXTUREFORMAT1..4`'s numeric values are in the order 2, 3, 4, 1, so a `switch` written in
  the obvious order is wrong for 1-D and 4-D coordinate sets.

The FVFs `dx8fvf.cpp` itself builds are cross-checked against that file's own understanding of the
same bits, so the two cannot disagree silently.

## 2. `D3DXFilterTexture` and `D3DXLoadSurfaceFromSurface` — pixels, with the approximations named

Both go through the `D3DCOLOR` A8R8G8B8 layout one pixel at a time. What is asserted, and where the
port knowingly differs from Windows, is written out at the top of `d3dx8texture.cpp`; the summary:

- **Exact, and tested:** channel widening replicates high bits (5-bit 31 → 255, 4-bit 8 → 0x88, i.e.
  `v * 255 / max`), matching the engine's own converters in `bitmaphandler.h`; formats without alpha
  read as opaque; `D3DX_FILTER_BOX` at 2:1 is the exact four-pixel average with halves rounded up,
  which is every mip-chain call site; `D3DX_FILTER_POINT` is nearest-neighbour;
  `D3DX_FILTER_NONE` copies unscaled and leaves uncovered destination pixels transparent black; a
  non-zero `ColorKey` becomes transparent black *before* filtering, so edges fade to transparency
  rather than to the key colour.
- **Approximated, and reported out loud once per run:** a non-integer scale ratio uses whole source
  pixels rather than partial coverage; `D3DX_FILTER_LINEAR`/`TRIANGLE` are served by the same area
  average, which is not a tent filter — the one caller, `SurfaceClass::Copy()`'s stretched path, gets
  a correctly-scaled image very slightly sharper than Windows would give it. Narrowing truncates
  rather than rounds (a 1-LSB difference from a rounding narrowing; no call site narrows).
- **Refused, with `D3DERR_INVALIDCALL` and a line on stderr:** palettised formats, DXT blocks,
  bump/luminance-pair formats, depth formats, cube and volume textures, and colour-to-luminance
  conversions. `DX8_ErrorCode()` at the call sites turns that into the engine's usual D3D error
  report.

Windows is the oracle for the pixels, and this was **not** compared against Windows' `d3dx8.lib`
output byte-for-byte: no such comparison harness exists in this repository. What the test asserts
instead is the list above — exact expected pixels for each named case — so any future comparison has
something specific to contradict.

### The two lock sites these add, and why they are their own usage class

`D3DXLoadSurfaceFromSurface` locks both surfaces at once — the source `D3DLOCK_READONLY`, the
destination with no flags — so `spikes/renderer/tools/d3d8-lock-scan.py` sees two new
`IDirect3DSurface8::LockRect` sites (five sites counting the unlocks) that no existing usage class
in `d3d8-lock-classes.json` describes. They are classified as a new **C9, surface-to-surface convert,
two locks held at once**, and the classification is a statement about hazard semantics, not
bookkeeping:

- The **source** lock is a full read of texels the GPU may have written, i.e. C3's cost: the Vulkan
  backend has to `vkCmdCopyImageToBuffer`, submit and wait on a fence before `Lock` returns.
- The **destination** lock is the case where the preserve-by-default rule in `decisions-resolved.md`
  may be relaxed, and it is relaxed only because the code makes it true rather than assumed:
  `Blit()` writes *every* pixel of the destination view with `Write_Pixel` — the destination loop is
  `for (y) for (x) Write_Pixel(...)` over the whole view with no early-out and no read of the
  destination's prior contents. So when the destination rect is the whole surface, nothing outside
  the written set exists and the staging allocation may be discarded rather than pre-filled. The
  moment `pDestRect` is a sub-rect that stops being true: the texels outside the rect must survive,
  which is C2, so the class is written to require the preserving behaviour in general and to permit
  discard only for the whole-surface case. Both of the sites the engine actually has today are
  whole-surface (`pDestRect == nullptr` from `D3DXFilterTexture`, and from `missingtexture.cpp`).
- `D3DXFilterTexture` reaches these locks with two mip levels of the **same** texture locked
  simultaneously (level *n-1* read, level *n* written), which is why the class also requires the
  per-resource staging allocation to be addressable per level. This is the shape of the two worst
  bugs in this port so far (#52 reading released staging, #63 a surface view whose layout diverged
  from its image), so it is stated in the table rather than left for the backend to infer.

The lock scanner's totals move with those sites: 226 → 239 references, 72 → 74 refcount, 95 → 100
lock/unlock, 19 → 20 files. `spikes/renderer/tools/surface-lock-audit.py --check` is unchanged
(it counts `SurfaceClass::Lock` callers, and this code locks `IDirect3DSurface8` directly), and
the renderer spike still runs with 0 validation messages and a 0/480000-pixel diff.

## 3. `D3DXAssembleShader` — refused, loudly, and what that costs

It assembles DirectX shader *assembly* into bytecode. This port is fixed-function-first, so there is
no assembler here and none is bolted in: the entry point nulls its out-parameters, prints one line
naming itself, and returns `E_NOTIMPL`. The test asserts exactly that — in particular that the
compiled-shader buffer is left null, so a caller that ignores the `HRESULT` cannot mistake a buffer
for bytecode.

The caller is `W3DWater.cpp`, which assembles three `ps.1.1` shaders and keeps each handle in
`m_riverWaterPixelShader`, `m_waterPixelShader` and `m_trapezoidWaterPixelShader`. On failure it never
reaches `Create_DX8_Pixel_Shader()`, never releases a buffer it does not have, and leaves all three at
the `0` they were initialised to — and every use of all three is guarded by `if (handle)`. **The
cost, precisely:** the river water loses its sparkle layer and its per-pixel reflection constant, the
trapezoidal water applies the shroud in texture stage 1 instead of inside the shader (a branch that
exists for hardware without pixel shaders), and the environment-mapped water reflection (`texbem`) is
not applied. Water still renders in its fixed-function form, which is the form the port targets.

The call is additionally behind `if (W3DShaderManager::getChipset() >=
DC_GENERIC_PIXEL_SHADER_1_1)`, computed from the device's reported `D3DCAPS8::PixelShaderVersion`, so
a backend that reports no pixel-shader support never reaches it. That makes this line a useful
signal: if it ever prints, a backend has started claiming pixel-shader capability it cannot honour.

If the port ever wants these three shaders, the honest route is the one `W3DShaderManager` already
uses for `shaders\wave.pso` — precompiled bytecode as an asset, or a translation to the backend's own
shading language.

## 4. Still unresolved: the five D3DX texture *creation* entry points

`D3DXCreateTexture`, `D3DXCreateCubeTexture`, `D3DXCreateVolumeTexture`,
`D3DXCreateTextureFromFileExA` and `D3DXGetErrorStringA` (all referenced by `dx8wrapper.cpp`, which
began compiling off Windows in #79) are **not** in this slice and are the whole of the
`no-definition-anywhere` pile at levels 1-4 afterwards. They are a different seam: the first three are
device-resource creation belonging with the renderer backend, and `D3DXCreateTextureFromFileExA` is an
image *decode* path (DDS/TGA, format fitting, mip generation) that deserves the same
exact-pixel treatment as §2 rather than a signature.
