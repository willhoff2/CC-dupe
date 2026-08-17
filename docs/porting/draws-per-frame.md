# Draws per frame: what the 64-draw cap did, and what it did not cause

Hardware: Apple M1 Pro, macOS 26.6.1, arm64, `backingScaleFactor` 2.00, MoltenVK 1.4.2.
Retail depot `~/devin-work/zh-data`, native binary built by
`scripts/native-build.py --level 1..4 --with-shims --strict-link`
(980/980 objects, 0 unresolved symbols).

Numbers here were measured on that machine. The enforced one lives in
`docs/porting/ci-baselines/draw-capacity.json` and is re-measured by
`scripts/ci/check-draw-capacity.py`; nothing in this document is the source of truth.

## 1. What the cap actually did on overflow

The old backend preallocated `kMaxDrawsPerFrame = 64` descriptor sets and one uniform buffer
sized for 64 draws. The question that decides whether this is one defect or two is what the 65th
draw did: **drop**, or **reuse a descriptor set another draw still needs** (which would draw
geometry with a neighbour's textures and state).

`spikes/renderer/src/draw_capacity.cpp` answers it per draw rather than by looking at a picture.
Each draw gets its own 1x1 texture whose RGB encodes its index, its own texture-factor alpha
encoding the same index, and its own transform placing it in a unique 4x4 tile of a 512x512
target. One readback then classifies every tile:

| tile content | verdict |
| --- | --- |
| still the clear colour | the draw was **dropped** |
| another draw's texture id | **descriptor-set alias** |
| right texture, another draw's alpha | **uniform-buffer alias** |
| anything else | mismatch |

Measured against the old fixed allocation (reproduced exactly by the `ZH_RENDER_MAX_DRAWS`
test limit, which caps allocation at the same 64 sets):

```
$ ZH_RENDER_MAX_DRAWS=64 build/spike-mac/zh-draw-capacity --draws 4096
tiles correct           64
tiles dropped           4032
tiles aliased           0
tiles uniform-aliased   0
tiles mismatched        0
first dropped draw      64
backend draws requested 4096
backend draws issued    64
backend draws dropped   4032
backend peak draws      64
descriptor sets         64 in 1 block(s)
draw-capacity: FAIL
```

**The cap dropped draws. It did not alias a live descriptor set or a live uniform slice.** The
first 64 draws were pixel-correct; draw 64 onward never reached the target. So the cap deletes
geometry from a frame; it does not paint geometry with another draw's textures.

In the real game the same shape holds — `issued` never exceeds the capacity and the shortfall is
counted, not silently absorbed (`ZH_RENDER_DRAW_REPORT=120`, campaign mission through
`scripts/macos-input-drive.py`):

```
draw resources exhausted at draw 64 (capacity 64): the frame will be missing geometry
draws/frame: requested 129 issued 64 dropped 65 (peak 64, capacity 64 in 1 block(s), dropped total 54971)
draws/frame: requested 125 issued 64 dropped 61 (peak 64, capacity 64 in 1 block(s), dropped total 70228)
draws/frame: requested  65 issued 64 dropped  1 (peak 64, capacity 64 in 1 block(s), dropped total 85436)
```

A mission-load frame asked for **129** draws and got 64 of them: half the frame's geometry was
thrown away. Steady in-mission frames on this map asked for 65–78. That is far below the
"thousands" the slice brief assumed — worth recording, because it changes what the cap can
possibly explain.

## 2. The fits-in-the-cap experiment

Three frames from the same binary, same retail data, same window (800x600 points, 1600x1200
pixels):

| frame | draws requested | issued | dropped | picture |
| --- | --- | --- | --- | --- |
| shell menu + 3D shell map | 19–28 | all | 0 | **correct** — backdrop, units, smoke, labels |
| mission, old fixed cap of 64 | 65–129 | 64 | 1–65 | corrupt |
| mission, growable allocation | 69–78 | all | **0** | **still corrupt** |

The third row is the decisive one. With every requested draw issued and zero dropped, the
mission frame is still corrupted in the same way: repeated/garbled tiles, ghosted text,
magenta and dark-green blocks. A sub-cap 3D frame (the shell map, a real W3D scene with terrain,
models, particles) is meanwhile pixel-correct.

**Conclusion: the cap was a real defect — it silently removed up to half of a mission frame's
geometry — but it is not the cause of the mission corruption. There is a second, independent
defect.** Both had to be measured because the cap's log flood made it the obvious suspect, and it
is not the culprit.

`Decode_Fvf: unsupported FVF 0x0` appears exactly twice at startup in *both* the clean-shell run
and the corrupt-mission runs, before mission entry, so it does not correlate with the corruption
either. It stays classified as an unimplemented vertex-declaration path (#114), not as the cause.

## 3. The fix: growable per-draw allocation, not a bigger constant

A larger constant would only move the cliff, so the preallocation was replaced with a block
allocator in `spikes/renderer/src/vulkan_backend.cpp`:

- draws are served in blocks of `kDrawsPerBlock = 256`; each block owns one `VkDescriptorPool`,
  its own descriptor sets, one host-visible uniform buffer, and one persistent mapping;
- `Draw_Slot(index, ...)` grows the block list on demand and hands every draw its **own**
  descriptor set and its **own** `minUniformBufferOffsetAlignment`-aligned uniform slice, so no
  draw can ever be handed a slot another draw is still using;
- blocks are retained across frames (steady state allocates nothing) and destroyed at backend
  shutdown, after the existing device-idle teardown;
- a draw that cannot be served is **not** reported as a successful frame: `Prepare_Draw` returns
  false, prints once per frame that the frame will be missing geometry, and increments
  `DrawStats::draws_dropped` / `draws_dropped_total` (`render_backend.h`).

Cost of this shape, stated plainly: one descriptor pool + one buffer + one mapping per 256 draws,
allocated the first time a frame reaches that depth, never freed until shutdown. It does not
recycle sets within a frame, so peak memory tracks the deepest frame ever rendered
(≈ 256 sets and 256 uniform slices per block). It needs no extra synchronisation precisely
because no slot is reused inside a frame.

Two environment hooks are available for measurement, not for behaviour:
`ZH_RENDER_MAX_DRAWS=N` refuses to grow past N draws (used as the negative control) and
`ZH_RENDER_DRAW_REPORT=N` prints the draw accounting every N frames.

## 4. The number, and the gate

Sustained, with every draw's own texture and uniform data verified by readback:

```
$ python3 scripts/ci/check-draw-capacity.py --binary build/spike/zh-draw-capacity --self-check
```

- **4096 draws per frame, over 3 consecutive frames, 0 dropped, 0 aliased, 0 mismatched**
  (16 blocks). The workload also ran clean at 16384 draws, the maximum the 512x512 / 4x4-tile
  target can distinguish; 4096 is the committed floor.
- The gate re-measures and compares against `docs/porting/ci-baselines/draw-capacity.json`
  (generated — regenerate with `--update`, never hand-edit), and fails if fewer draws are issued
  than requested, if any tile is dropped, aliased or mismatched, or if the descriptor capacity or
  block count regresses.
- `--self-check` additionally runs the negative control shown in §1 —
  `ZH_RENDER_MAX_DRAWS=64` — and fails if that run *passes*, so a gate that cannot catch the old
  fixed-cap defect is itself a failure.
- Validation-layer runs use the `--require-layer` recipe
  (`eval "$(python3 scripts/ci/vulkan_manifests.py --require-layer --print-env)"`); the measured
  run reported `validation layer: loaded` with `validation messages: 0`.

The gate runs in `native-port-ci.yml` on Linux/lavapipe and on macOS arm64/MoltenVK.

## 5. Residual defects, named and classified

1. **Mission-frame corruption — port defect, mechanism not yet isolated.** With draw accounting
   correct (69–78 draws issued, 0 dropped) a running campaign mission still renders repeated and
   garbled blocks, ghosted glyphs and magenta/dark-green tiles, while the HUD sidebar and the
   shell map render correctly. The repetition pattern (the same small region tiled across the
   frame) and the vertical letterboxing of the mission frame point at a size/pitch mismatch on
   the in-mission path rather than at per-draw state, but that is a hypothesis, not a
   measurement: this slice did not isolate it. It is not the cap, and it is not explained by any
   dropped draw.
2. **`Decode_Fvf: unsupported FVF 0x0` — unimplemented path.** Present identically in clean and
   corrupt runs (see §2). Unchanged by this slice.
3. **Exit-time `SIGSEGV` in `ObjectPoolClass<MultiListNodeClass, 256>::~ObjectPoolClass` during
   `__cxa_finalize_ranges` — port defect, out of scope.** Observed once when quitting the native
   binary; the crash is in static teardown after `exit`, not in a frame, and is unrelated to draw
   allocation (the run never printed a draw-resource-exhausted line). Recorded here so it is not
   rediscovered as a renderer symptom.
4. **Synthetic-only:** the 16384-draw result is a property of the workload's tile grid, not a
   measured engine requirement. Real mission frames on this map need two orders of magnitude
   fewer draws.

Screenshots of the mission before and after the allocation change are attached to the session,
not committed — they are retail-derived.
