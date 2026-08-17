# The retail main menu, rendered correctly: 2D composition through CopyRects

The retail Zero Hour main menu loads natively — `MainMenu.wnd`, a 207-window shell, retail fonts and
button geometry — but on the first measured frames everything behind the buttons was a magenta field
with speckle noise and every label was drawn twice, horizontally offset. This slice reproduces both
symptoms on Linux, measures what the engine actually asks the backend for, fixes the one that is a
port defect, and classifies the rest.

Everything below was measured on Linux/x86-64, Vulkan through **lavapipe**, `clang++-14`, with the
binary `scripts/native-build.py --level 1..4 --with-shims --strict-link` produces, run as
`./zh -win -noshellmap -nologo -xres 800 -yres 600` from a directory outside the repository with a
retail Zero Hour install. **Nothing here is evidence about Apple Silicon**; §7 says what a Mac session
must run to make the same claims there. No retail byte, decoded texel or PNG is committed.

## 1. The two symptoms were two different findings

| Symptom | Cause | Classification |
|---|---|---|
| magenta-and-noise field behind the buttons | the base-game `Generals/Textures.big` was not loaded, so `mainmenubackdropuserinterface.tga` was absent and the shell drew the missing-texture placeholder | **missing data** (installation/settings, not code) |
| every label drawn twice, horizontally offset | `CopyRects` copied a 2-byte-per-texel host surface into a 4-byte-per-texel image as raw bytes | **port defect** (fixed here) |
| `Decode_Fvf: unsupported FVF 0x0`, twice per run | the two shadow managers create vertex buffers whose format comes from a declaration, not an FVF; the backend has no declaration path | **unimplemented path** (not a menu defect; §5) |
| `Copy_Rects: source and destination formats differ`, once per run on the reporter's Mac | not reproduced on Linux in four instrumented runs (0 occurrences) | **not reproduced**; §6 |

The magenta field and the doubled labels are independent: with the data present the background is
correct and the labels are still doubled, which is how the two were separated.

### 1.1 The missing data

Zero Hour reads *both* archive sets. `GeneralsMD/*.big` alone gives a shell whose art is mostly there
and whose backdrop is not. The base game's archives must be reachable through the install path in the
settings file the native port reads (`filesystem-and-registry.md`):

```ini
[SOFTWARE\Electronic Arts\EA Games\Generals]
STRING_InstallPath = /path/to/install/Generals/
```

The trailing separator is required: the runtime appends `Generals` and `GeneralsMD` to it, so without
it the two directories it looks in are `…/GeneralsGenerals` and `…/GeneralsGeneralsMD`, both absent,
and the failure is silent. Symlinking both archive sets into one run directory is *not* an
alternative — the two sets share file names, the duplicates collide and `Data\INI\Weapon.ini` fails
to parse.

Nothing in the engine was changed for this. It is recorded because the placeholder it produces —
magenta blocks with speckle — reads exactly like a sampling or upload defect, and a session that
starts fixing the renderer for it will fix nothing.

## 2. What the engine asks CopyRects for

Instrumented at the backend entry point, the retail menu makes **313** `CopyRects` calls and every
one of them has the same shape:

```
COPY src 64x64 fmt=44 pitch=128 tb=2 sys=1 | dst 64x64 fmt=44 pitch=256 tb=4 sys=0 rects=0
```

`fmt=44` is `VK_FORMAT_B8G8R8A8_UNORM` — the *Vulkan* format of both sides. Read the rest: the source
is system memory with a 128-byte pitch for 64 texels, i.e. **2 bytes per texel**; the destination is
an image with a 256-byte pitch for 64 texels, i.e. **4 bytes per texel**. `rects=0` is D3D8's
whole-surface copy.

The call site is the shell's text composition, not a blit that runs twice:

```
Render2DSentenceClass::Build_Textures                 render2dsentence.cpp:373
  TextureClass(desc.Width, desc.Width, WW3D_FORMAT_A4R4G4B4, MIP_LEVELS_1)
  DX8Wrapper::_Copy_DX8_Rects(curr_surface, nullptr, 0, texture_surface, nullptr)
```

`Render2DSentenceClass` rasterises glyphs into a system-memory **A4R4G4B4** surface (`Blit_Char`
through lock class C1, `surface-lock-audit.py`'s `render2dsentence.cpp:908`) and then copies that
surface into level 0 of an **A4R4G4B4** texture. Both sides are the same D3D8 format, as D3D8
requires, and one is 2 bytes per texel while the other is 4 — because the seam emulates A4R4G4B4,
which lavapipe and MoltenVK do not both offer, with `VK_FORMAT_B8G8R8A8_UNORM`:

```cpp
case TextureFormat::A4R4G4B4:
    p.vk = VK_FORMAT_B8G8R8A8_UNORM;
    p.expand_to_bgra8 = true;      // Plan_For, vulkan_backend.cpp
```

## 3. The defect: equal D3D8 formats, unequal bytes

`Copy_Rects` gated on the Vulkan format:

```cpp
if (source->vk_format != destination->vk_format) { …refuse… }
```

Every CPU-expanded format shares `VK_FORMAT_B8G8R8A8_UNORM`, so that test passed and the copy
proceeded as a plain `vkCmdCopyBufferToImage` with `bufferRowLength = width`. Two host texels were
therefore consumed per destination texel: each pair of 16-bit glyph texels became one 32-bit texel,
so a glyph row was written half as wide and the row that followed it landed beside it. On screen that
is the label drawn twice, offset — one label, resampled into two.

That is also why the validation layer was silent: an image of 64×64 4-byte texels received exactly
64×64×4 bytes. Every Vulkan rule was obeyed. Only the meaning was wrong.

### 3.1 The fix

Three changes, all in the resource seam, no new lock class and no second upload path:

1. `SurfaceHandle` now carries the **D3D8** format as well as the Vulkan one. It is not derivable from
   `vk_format` (many D3D8 formats share one), and `Get_Surface_Level` records the texture's D3D8
   format rather than its image's.
2. `Copy_Rects` compares the D3D8 formats — the pair D3D8 itself requires to match — and its refusal
   message now prints both.
3. When the format is one the seam emulates (`Plan_For(...).expand_to_bgra8`) and one side is host
   memory while the other is an image, the copy goes through `Copy_Rects_Converting`, which expands
   (or contracts) the rectangle on the CPU with the seam's existing `Expand_To_Bgra8` /
   `Contract_From_Bgra8` passes and stages it through the existing staging pool — one acquire per
   call, not per rectangle. It is the same upload path addressed by rectangle.

The read direction (image → host surface) needs the inverse pass, which does not exist for every
format. It is refused **before anything is copied**, and loudly:

```cpp
if (!source->system_memory() && !Contractable_From_Bgra8(source->format)) {
    std::fprintf(stderr, "Copy_Rects: no contraction for format %d; the read direction "
                         "cannot be served\n", static_cast<int>(source->format));
    return false;
}
```

`Contractable_From_Bgra8` exists so that question can be asked without running a dummy conversion
pass; the contraction itself now calls it, so the two cannot disagree. Formats served today:
`R8G8B8`, `X8R8G8B8`, `L8`, `A8`, `A8L8`, `A4R4G4B4`. Anything else is a documented refusal, not a
plausible write.

`render_backend.h`'s contract states the rule the old code did not encode: equal D3D8 formats are not
necessarily equal bytes, so an implementation may have to convert *representations* on the way
through — what it must never do is reinterpret one as the other.

## 4. Evidence

### 4.1 The gate (synthetic, exact, committed, runs on a Mac)

`zh-resource-lock-tests`, which CI runs in both swizzle modes, gained two cases. They fill a host
A4R4G4B4 surface from a deterministic 16-bit pattern, `CopyRects` it into an A4R4G4B4 texture, draw
the texture and compare the **readback** against the expansion computed independently in the test:

```
PASS CopyRects A4R4G4B4        A4R4G4B4 host surface into an A4R4G4B4 texture: mean |delta| 0.000,
                               controls 117.2 (bytes reinterpreted) and 65.9 (flat);
                               sub-rect at (12,12) correct and the rest untouched
PASS CopyRects format refusal  A4R4G4B4 into A8R8G8B8 refused, though both images are B8G8R8A8
validation messages: 0
0 case(s) failed, 0 skipped
```

The first wrong-image control is not generic: it models *this* defect, the two 16-bit host texels a
32-bit read would combine. With the conversion disabled the case fails against it as predicted —

```
FAIL CopyRects A4R4G4B4   worst texel got=(0,0,1,0) expected=(238,221,204,238);
                          mean |delta| 105.997 to the reference, 27.946 to the byte-reinterpretation model
```

— so the residual is 0.000 against the reference and 117.2 and 65.9 against controls, and the case is
known to fail when the fix is removed. The second case matters because both surfaces' images are
`B8G8R8A8`: a backend comparing Vulkan formats would accept an A4R4G4B4 → A8R8G8B8 copy.

### 4.2 The retail frame (Linux/lavapipe, not committed)

`VulkanRenderBackendClass::Measure_Frame` on the colour target of the live retail menu, 800×600,
480,000 pixels, validation silent. Two independent measurements, both against references that are not
the renderer:

**The background is the shell's own art.** `Art\Textures\mainmenubackdropuserinterface.tga`
(1024×1024, 24-bit, uncompressed) was read straight out of `Generals/Textures.big` by an independent
Python TGA reader and compared with the readback over the battlefield interior (x 60–520, y 40–540),
which no overlay art covers:

| image compared with the retail backdrop | mean \|delta\| |
|---|---|
| the measured frame | **3.33** |
| the same reference misaligned by 4 px | 17.6 (5.3×) |
| the same reference misaligned by 8 px | 26.9 (8.1×) |
| the frame measured before the data was present (magenta placeholder) | 140.3 (42×) |

The alignment sensitivity is the point: a frame that merely "looks like a battlefield" does not get
3.33 against the retail texel grid, and the magenta control is what a plausible-looking wrong frame
scores.

**The label is drawn once.** Per button, the glyph ink column profile inside the button (inset past
the frame art) is measured and correlated with itself:

| | before the fix | after |
|---|---|---|
| strongest self-similarity lag, all six buttons | **+32 px in every one of the six** | no common lag (+19, +39, +15, +21, +29, +21) |
| ink columns, `ButtonLoadReplay` … `ButtonMultiplayer` | 18 … 65 | 34 … 78 |

A single lag shared by six labels of different lengths is a second copy of each label 32 px to the
right — 64 texels of glyph surface consumed two-to-one is 32 texels of output, which is the mechanism
in §3 measured on screen. After the fix no such lag exists, and the ink is wider because the glyph
rows are no longer interleaved into half the width. Region residuals between the two frames locate
the change: 9.10 inside the button panel, **0.118** outside it, i.e. the fix moved the text and
nothing else.

## 5. FVF 0 is not part of this

D3D8 allows a zero FVF: a vertex buffer whose format is described by a vertex declaration/shader
rather than by an FVF. The measurement says that is exactly what the engine means here. Both
occurrences, with their call stacks:

```
VulkanBackend::Create_Lockable_Vertex_Buffer(bytes=49152, fvf=0x0, dynamic=true)
  VulkanRenderBackendClass::CreateVertexBuffer(length=49152, usage=520, fvf=0)
  DX8Wrapper::Create_DX8_Vertex_Buffer(...)
  W3DVolumetricShadowManager::ReAcquireResources()

VulkanBackend::Create_Lockable_Vertex_Buffer(bytes=786432, fvf=0x0, dynamic=true)
  … W3DProjectedShadowManager::ReAcquireResources()
```

from, in `W3DVolumetricShadow.cpp`:

```cpp
DX8Wrapper::Create_DX8_Vertex_Buffer(
    SHADOW_VERTEX_SIZE*sizeof(SHADOW_DYNAMIC_VOLUME_VERTEX),
    D3DUSAGE_WRITEONLY|D3DUSAGE_DYNAMIC, 0, D3DPOOL_DEFAULT, &shadowVertexBufferD3D);
```

So the zero is deliberate engine state, not state that was never set, and the backend's response is
already honest: `Decode_Fvf` fails, buffer creation returns null and says so — it does not invent a
layout. Nothing about it touches the menu: these are the in-game shadow managers, they are created at
device (re)acquire, and the menu never draws from them. Serving them means implementing the
declaration-driven vertex path (`SetVertexShader` with a declaration), which is a seam of its own and
deliberately not attempted here. **Unimplemented path**, owner: a future renderer slice.

## 5.1 How this sits with the HiDPI slice's point/pixel rule (#113)

#113 landed on the same seam and gave the backend two size units: `width_`/`height_` in points, which
is what every surface advertises, and `device_width_`/`device_height_` in pixels, which is what the
default target's image actually holds. Because a rectangle copy cannot resample, it refuses
`Copy_Rects` (and the default back-buffer lock paths) whenever the default target is scaled. That
refusal now runs *before* the format comparison and the conversion dispatch, so any surface that
reaches the conversion advertises exactly the pixels its image holds: the rectangles the conversion
walks are in one unit and there is no point/pixel ambiguity to reintroduce.

The two rules also cannot both apply to one copy, for a second, independent reason. The only surfaces
whose `Surface_Render_Scale` can differ from 1 are the device's own default colour and depth targets,
and those are `A8R8G8B8`/`B8G8R8A8` — not one of the CPU-expanded formats (`X8R8G8B8` without a view
swizzle, `A4R4G4B4`, `L8`, `A8`, `A8L8`) that `Plan_For(...).expand_to_bgra8` selects, and the
conversion dispatches on exactly that. Render-to-texture surfaces, which are what a conversion copy
touches, always have scale 1. So a scaled copy is refused and never converted, and a converted copy is
never scaled; neither slice's tests were dropped, and both suites plus `check-hidpi-scale.py` pass on
the rebased tree.

## 6. What was found in the 2D/shell path and deliberately left

- **`Copy_Rects: source and destination formats differ` on the reporter's Mac** — once per run there,
  **zero times in four instrumented Linux runs**, and all 313 Linux calls are the text composition
  above. It is therefore not what doubled the labels, and its pair is unknown. The refusal now prints
  both D3D8 formats, so a Mac session can name it in one run; §7 has the command.
- **The image-to-host direction of the conversion** is implemented and unit-tested but no retail call
  site reaches it (all 313 are host→image). Treat its retail behaviour as unmeasured.
- **HiDPI point-versus-pixel sizing of the colour target and viewport** (§8.4 of
  `apple-silicon-verification.md`) — #113 owns `VulkanBackend`'s swapchain/`Present` sizing; nothing
  here touches it, and §5.1 records how the two rules meet.
- **GUI callbacks driven by real clicks** — another slice, on the reporter's Mac. This one asserts on
  a rendered frame, not on input.
- **`missingtexture.cpp`'s `CopyRects`** (A8R8G8B8 texture level → image surface) goes through the
  unconverted path, which is correct: A8R8G8B8 is stored as it is given.
- Fonts, button geometry and the logo texture were already right and were not touched.

## 7. Linux-only, and what a Mac session must run

Everything in §2, §4.2 and §6 is Linux/lavapipe. What a Mac session should run, in order:

```sh
# 1. the committed gate, on MoltenVK: the exact-residual proof of the fix
cmake -S spikes/renderer -B build/spike-mac -G Ninja -DCMAKE_BUILD_TYPE=Release -DSPIKE_USE_SDL2=OFF
cmake --build build/spike-mac
export VK_ICD_FILENAMES=$(find "$(brew --prefix molten-vk)/" -name MoltenVK_icd.json | head -1)
build/spike-mac/zh-resource-lock-tests --validation
ZH_SPIKE_NO_VIEW_SWIZZLE=1 build/spike-mac/zh-resource-lock-tests --validation

# 2. the retail menu, with the diagnostics counted (both must be 0 for CopyRects)
./zh -win -noshellmap -nologo -xres 800 -yres 600 2>&1 | grep -c 'Copy_Rects:'

# 3. the frame, and the same two measurements as 4.2
#    Measure_Frame(0,0,0,0,"menu.png",proof) from the debugger, per apple-silicon-verification.md 9
```

If step 2 is non-zero on a Mac, the message now names the two D3D8 formats: that is the unexplained
Mac refusal of §6 and it is a finding, not a regression of this fix. Step 1 failing on MoltenVK while
passing on lavapipe would mean MoltenVK offers a native 4-4-4-4 format and the emulation plan differs
there — `Plan_For` is the place to look.

## 8. Provenance

The code and this document were generated by an LLM (Devin) and reviewed and re-measured by it; the
numbers above are its own measurements on the machine described, not quotations. Human polishing:
none beyond the review that produced the slice's scope.
