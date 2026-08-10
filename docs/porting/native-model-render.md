# Native retail model render

## Result

`spikes/model-render/` loads `avcrusader.W3D` from the retail BIG archives, parses it with the engine's `ChunkLoadClass` and W3D records, uploads its retail DXT1/TGA textures, and renders it headlessly through Vulkan/MoltenVK on an Apple M1 Pro.

The result is **geometrically correct and mechanically cross-checked, but not a complete engine-faithful frame**. Twelve of thirteen meshes render correctly. `TURRETFX01`, a four-triangle muzzle-effect mesh, is deliberately omitted because its `EXTnkMzl01.tga` reference does not exist in any of the 20 supplied retail archives. Drawing it white would make a plausible but wrong image. House colour is the raw retail placeholder texture rather than the runtime player-colour treatment, which is why the flat white patches are there.

Lighting is the backend's own fixed-function path (PR #21): one `D3DLIGHT_DIRECTIONAL`, `D3DRS_AMBIENT`, and the W3D vertex material passed per batch as D3D8 material state with `D3DMCS_MATERIAL` sources. The CPU reference implements the same D3D8 equation independently, so the agreement below is a check on the backend's lighting against a real asset, not a comparison of one implementation with itself.

Evidence:

- [`spikes/model-render/evidence/avcrusader-moltenvk.png`](../../spikes/model-render/evidence/avcrusader-moltenvk.png): MoltenVK render with retail textures and mip chain.
- [`spikes/model-render/evidence/avcrusader-cpu-reference.png`](../../spikes/model-render/evidence/avcrusader-cpu-reference.png): independent scalar CPU rasterisation.

## Native engine-code boundary

These engine translation units compile and link natively into the tool:

- `WWLib/chunkio.cpp`
- `WWMath/matrix3d.cpp`, `matrix4.cpp`, `quat.cpp`, `wwmath.cpp`
- `WW3D2/htree.cpp`, `pivot.cpp`
- `WWStub/wwallocstub.cpp`, `wwdebugstub.cpp`

`w3d_model.cpp` uses `PosixFileClass`, `ChunkLoadClass`, `W3dMeshHeader3Struct`, the other records in `w3d_file.h`, and `HTreeClass`. The two headers in `scripts/native-port-shims/` declare only `D3DMATRIX`, `D3DXMATRIX`, `D3DXVECTOR4` and the two `D3DXVec4*` helpers; they implement no D3D runtime.

Those declarations are what the WWMath conversion operators and the GameEngine's `Bezier` code name, so adding them moves the shimmed probe: `WWMath` gains `matrix3d.cpp` and `matrix4.cpp` (33 -> 35 clean), and the `GeneralsMD/Code/GameEngine` `Bezier` translation units keep compiling because `D3DXVECTOR4` resolves. The `native-port-probe-shimmed` baseline is refreshed for `WWMath` accordingly.

The engine's direct model path still does not compile:

```text
meshmdl.cpp -> meshmdl.h -> dx8wrapper.h:46:10: fatal error: 'd3d8.h' file not found
ww3d.cpp -> ww3d.h -> dx8wrapper.h:46:10: fatal error: 'd3d8.h' file not found
```

`hanim.cpp` compiles, but linking it pulls in WWSaveLoad persist factories, WWLib file factories/multilists, and Win32 critical sections. The tool only needs a static base pose, so `engine_link_support.cpp` narrowly supplies the animation-only symbols referenced by `htree.cpp`; those stubs abort if called. It also supplies the zero-initialised `WW3D` frame clock that normally lives in unbuildable `ww3d.cpp`. This is not a replacement for `MeshModelClass::Load_W3D` or animated-model support.

## Retail Crusader cross-check

Input provenance reported by the tool:

```text
W3DZH.big:Art\W3D\avcrusader.W3D, 43,382 bytes
145 parsed chunks, 13 meshes, 20 hierarchy pivots, one HLOD array (LOD 0)
```

Every mesh's parsed vertex/triangle arrays match `W3D_CHUNK_MESH_HEADER3`, all indices are in range, every normal array matches its vertex array, and every calculated vertex bound is inside the header min/max. Totals are 676 vertices and 352 triangles; the rendered 12 meshes contain 668 vertices and 348 triangles after omitting the muzzle effect.

UVs are finite and range from `-0.1183` to `1.0023`; the small excursion is intentional wrapping, not clamped corruption. Texture lookup results are:

- `avcrusader.tga` -> `TexturesZH.big:Art\Textures\avcrusader.dds` (256x256 DXT1, nine mips)
- `Housecolor.tga` -> `W3DZH.big:Art\W3D\Housecolor.tga` (256x256 A8R8G8B8 TGA)
- `EXTnkMzl01.tga` -> unresolved in every supplied archive; affected mesh omitted

The Apple M1 Pro reports sampled-image support for DXT1, DXT3, DXT5, and A8R8G8B8. The retail DXT1 bytes and all nine mip levels are uploaded directly; there is no transcode in the GPU path. TGA is decoded to BGRA8.

## Coordinate and image validation

The backend takes matrices in D3D8's row-vector convention and composes them `world * view * projection`, as `DX8Wrapper` does through `To_D3DMATRIX`; the tool transposes its Westwood column-vector matrices on the way in. Handing over the untransposed matrices draws the model somewhere off-target, which is how this was caught after the rebase onto #21.

W3D positions remain right-handed and Z-up. The camera uses Z as world-up, a right-handed view, and Vulkan's `[0,1]` depth range. The backend negates the clip-Y row before GLSL upload.

A synthetic UV gradient/checker (`--uv-test --flat-light`) made GPU and CPU images agree at 1.0000 coverage IoU and 0.03 mean channel error with culling disabled. That verifies top-origin V, wrap addressing, perspective interpolation, and the BGRA upload independently of the retail texture's appearance.

That test exposed an existing cull translation bug: D3D8 `D3DCULL_CW` had been mapped to Vulkan front-face culling. `vulkan_backend.cpp` negates clip Y and Vulkan's NDC Y points down, so the two flips cancel and a triangle's framebuffer winding equals its D3D screen winding; with `VK_FRONT_FACE_COUNTER_CLOCKWISE`, `D3DCULL_CW` must therefore cull the back face. Before the correction, the real model rendered its back faces and GPU/reference mean error was 58.67. After correction and equal culling rules:

```text
GPU covered pixels:       135,269
CPU covered pixels:       135,291
coverage IoU:              0.9946
mean absolute difference:  3.03 / 255 per channel
validation messages:       0
```

None of the spike's other cases could have caught the inversion, because they all run with `D3DCULL_NONE`. `zh-fixedfunc-tests` now has a `cull mode` case that draws one screen-clockwise and one screen-counter-clockwise triangle under each of `NONE`/`CW`/`CCW`, on both the pretransformed and the transformed path, and asserts which survives. Reinstating the old mapping makes it fail with `pretransformed CULLMODE=CW: clockwise triangle got (255,255,255,255) expected (0,0,0,0)`.

The final file enables trilinear mip filtering. Compared with the otherwise identical mip-0 GPU frame, its coverage IoU is 0.9948 and mean channel difference is 1.87. The independent reference intentionally uses mip 0 so the BC decoder, sampler convention, geometry, transforms, depth, culling, and shading can be compared without implementing GPU derivative/LOD selection a second time.

## Gaps found by a real asset

PR #21 closed most of what the first version of this list called a backend gap: the texture-stage cascade, the 21 FVFs, lighting and materials, fog, alpha test, ZBIAS, texture transforms, and 16 texture formats are all in the backend now, and this tool uses them rather than working around them. What remains:

In the engine/tool path:

- `MeshModelClass::Load_W3D` and the rest of the engine's own model runtime still do not compile natively (see the `d3d8.h` error above); the geometry here comes from this tool's own chunk parsing.
- Skinned/animated posing is not implemented; only the base hierarchy pose is used.
- No runtime house-colour mapper: the raw house-colour placeholder texture is shown.
- Missing runtime/generated muzzle texture: the effect mesh is omitted.
- Only material pass 0 and texture stage 0 are drawn, and W3D mapper arguments (animated/environment UV mappers) are not translated, so sampler addressing is wrap-only.
- Alpha blending state recorded by the W3D parser is not applied: the CPU reference does not blend, so applying it would silently weaken the cross-check. The Crusader has no blended pass.
- The texture loader reads DXT1/3/5 and 32-bit uncompressed DDS plus TGA; the backend samples 16 formats, so the narrower set is this tool's, not the backend's.

In the backend:

- The index path is 16-bit, so meshes above 65,535 vertices are rejected.
- No render-target/texture-render API, and no vertex or pixel shaders: the engine's shader paths have nothing to bind to.
- Alpha test is exercised only by state, not by content, in this image: the one alpha-tested material belongs to the omitted muzzle mesh.

## Rerun

The data directory is a runtime argument and is not compiled into the tool:

```sh
/opt/homebrew/bin/cmake -S spikes/model-render -B build/model-render \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
/opt/homebrew/bin/cmake --build build/model-render -j8

DYLD_LIBRARY_PATH=/opt/homebrew/lib build/model-render/zh-model-render \
  --data /path/to/zero-hour \
  --model avcrusader.W3D \
  --out crusader.png \
  --reference-out crusader-reference.png
```

The backend's cull mapping is covered independently of the retail data:

```sh
/opt/homebrew/bin/cmake -S spikes/renderer -B build/renderer -DSPIKE_USE_SDL2=OFF
/opt/homebrew/bin/cmake --build build/renderer -j8
DYLD_LIBRARY_PATH=/opt/homebrew/lib build/renderer/zh-fixedfunc-tests   # `cull mode` case
```

Optional diagnostics:

```sh
# Remove lighting and replace retail textures with a UV gradient/checker.
.../zh-model-render --data /path/to/zero-hour --flat-light --uv-test --out uv.png

# Force a cull mode to reproduce/check the winding decision.
.../zh-model-render --data /path/to/zero-hour --cull none|cw|ccw --out cull.png
```

A successful Crusader run ends with `PASS: 0 mechanical check(s) failed` and separately reports the one omitted unresolved texture as `PARTIAL`; that limitation must not be read as a complete game-faithful render.
