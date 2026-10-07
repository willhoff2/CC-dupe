# Late-game frame cost: what the original does, what the port adds, what to measure next

User question: *"I'd be curious to understand more about the slowness late game and what's causing
it. I think it's certainly when there's a lot of things to render in the game. I'm not sure how the
original does that on Windows / natively."*

This is stage 1 of a measurement: source reading plus the data earlier probes already recorded. No
game was launched for it. Stage 2 measured on the Mac, using the plan in §7: its results are §10.

**Stage 2 in short (§10).** An optimised build makes late-game logic **4x faster** on replayed
games, and the rendered late median falls from 53 to 37 ms. Plain `-O2` **desyncs replays**: Apple
clang fuses `sinf`+`cosf` into `__sincosf_stret` in `Locomotor.cpp`; adding `-fmath-errno` plays
both recordings in sync. Optimised, the late frame is the renderer's GPU waits, and with the
`perf/text-render-no-flush` backend the replay holds 30 fps. A `-replay` run draws nothing until a
flag is cleared (§10.6). The "heavier game" rule came out NOT CONFIRMED (1.43x, bar 2x).

Evidence categories, never blended: **MEASURED** (method named, data path given), **INFERRED**
(from source or from general D3D8/driver knowledge, not run), **UNMEASURED**.

## 0. Answer in short

- **The late-game slowdown is mostly not rendering.** At game minute ~8 with 950-1,200 objects,
  the main thread spends **35-52 ms per frame in game logic**, and **19-38 ms of that is the
  pathfinder** (A* in `Pathfinder::processPathfindQueue`). This holds in all five 32-minute
  runs on both binaries (MEASURED, §3.1). Rendering the 3D scene costs the CPU **1-2 ms** per
  frame in the same profiles. Turning draw calls into Vulkan calls costs another **1 ms**.
- **The renderer's cost is mostly waiting.** The renderer takes ~30 ms per frame on `main`:
  - ~20-23 ms is waiting for the GPU. The port runs CPU and GPU one after the other; it never
    overlaps them.
  - ~4 ms is per-texture upload waits.
  - ~5 ms is CPU work, mostly re-rendering text.

  (MEASURED, §3.2-3.3)
- **The original does the same logic work on Windows.** The game loop is lockstep: one logic
  frame per rendered frame. So a heavy logic frame slows the game clock rather than dropping
  frames. This is the retail loop, measured here too: logic frames per second equal fps once
  below 30 (§1.1, §3.1). Pathfinding cost is retail behaviour (INFERRED: same source).
- **The largest port-specific factor is the build, not the renderer.** Every binary behind every
  published Mac frame-time figure is compiled **unoptimised (`-O0`)**: engine, logic,
  `DX8Wrapper` and Vulkan backend alike. This was MEASURED by disassembly in §3.4.
  `scripts/native-build.py` configures `CMAKE_BUILD_TYPE=Debug` and adds no `-O` flag. The
  Windows retail build is optimised. This is hypothesis H1 (§5): it is the one most likely to
  account for most of the port-specific share. Its size is UNMEASURED; stage 2's first
  measurement settles it.
- **The "unexplained" late-game regression on `perf/text-render-no-flush`** is most likely
  different games, not branch CPU cost (§4).
  - The branch changed no engine code.
  - Within single runs of either binary, the time outside the renderer jumps 2-3x at a constant
    object count, so object count does not fix the logic load.
  - At game minute ~8, logic per frame is the same on both binaries.

  A replay settles it.
- **Replays: yes.** The native arm64 binary already replays its own recordings headless (MEASURED
  in `combat-probe.md`). Every heavy run left its recording in its user data, including the
  101 ms-late run. §6 gives the commands.

## 1. How the original structures a frame (source; Windows/D3D8)

Paths are relative to the repository root. This tree is the TheSuperHackers fork. Where it changed
the loop, the untouched EA code is cited as `3d0ee53a0:path:line`.

### 1.1 The loop: client (draw) first, then at most one logic step

- **Order.** `GameEngine::update` runs radar, audio, then `TheGameClient->UPDATE()`, which draws and
  presents, then the network, then `TheGameLogic->UPDATE()`
  (`GeneralsMD/Code/GameEngine/Source/Common/GameEngine.cpp:938`, `:950`). The original has the same
  order (`3d0ee53a0:GeneralsMD/Code/GameEngine/Source/Common/GameEngine.cpp:746`).
- **One logic step at most per pass.** Logic is gated by `canUpdateRegularGameLogic`, an
  accumulator capped at one step per pass (`GameEngine.cpp:880-917`). Logic is nominally 30 Hz
  (`LOGICFRAMES_PER_SECOND`). There is no catch-up, so a slow frame makes the game run slower; it
  does not skip frames.
- **Frame limiting.** The fork limits with `TheFramePacer->update()` (`GameEngine.cpp:1041`). The
  original busy-waited on `Sleep(0)` (`3d0ee53a0:...GameEngine.cpp:881`).
- **MEASURED here** (§3.1, `logic/s` column): logic frames per wall second equal the rendered fps in
  every window below 30 fps.

### 1.2 `GameClient::update` → `W3DDisplay::draw`

**`GameClient::update`** (`GeneralsMD/Code/GameEngine/Source/GameClient/GameClient.cpp`):

1. Input, the window manager, then **a loop over every drawable**: shroud status, then
   `updateDrawable()` (`:638-687`).
2. Terrain visual, display update, `TheDisplay->DRAW()` (`:723`), the display-string manager, the
   shell and `InGameUI`.

**`W3DDisplay::draw`** (`GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DDisplay.cpp`):

1. Dynamic LOD (`:1813`).
2. Terrain tracks and the shroud texture.
3. `updateViews()`, which runs `Drawable::draw()` for every drawable in the view's AABB, found by a
   linear walk over all drawables (`Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DView.cpp:1694`,
   `GameClient.cpp:804-821`).
4. Particle update, once per logic frame.
5. The water mirror and projected-shadow render targets.
6. `WW3D::Begin_Render` (`:1981`), `drawViews()` (`:1998`), the in-game UI, the mouse, then
   `WW3D::End_Render` (`:2080`).
   - `End_Render` flushes the sorting renderer, then calls `DX8Wrapper::End_Scene`, i.e.
     `EndScene` + **`Present`** (`Core/Libraries/Source/WWVegas/WW3D2/dx8wrapper.cpp:1689-1701`).
   - It then **invalidates every cached render state**
     (`GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/ww3d.cpp:1129`), so state filtering starts
     cold each frame.

### 1.3 The scene: `RTS3DScene` passes

`GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DScene.cpp`.

**`Customized_Render`:**

- `Visibility_Check` frustum-culls **every** render object in the scene, not only on-screen ones
  (`:396-537`).
- Terrain draws first.
- `renderOneObject` (`:583`) runs per visible object. It builds a light environment by walking
  the global, scene and dynamic lights, then calls `robj->Render`.

**`Flush` (`:846-891`)**, in order:

| pass | what scales it |
|---|---|
| decal / projected shadows (`DoShadows(false)`) | per shadow; projected shadows re-render each receiver |
| `TheDX8MeshRenderer.Flush()`, opaque meshes | per visible mesh × texture category × pass |
| occluded-object stencil pass | per occluder/occludee, capped at 512 |
| trees | bounded (per texture bucket) |
| stencil shadow volumes (`DoShadows(true)`) | per shadow, 2 draws per volume plus a full-screen quad (`.../Shadow/W3DVolumetricShadow.cpp:3503-3595`) |
| water | bounded, except mirror types 1/2, which re-render the scene |
| translucent objects | per object, capped at 512 |
| particles | per system, ≤ 512 points per draw group |
| `SortingRendererClass::Flush()` | per sorted node; insertion is linear, so O(n²); nodes beyond 4,096 are dropped (`Core/Libraries/Source/WWVegas/WW3D2/sortingrenderer.cpp:325-354`) |

### 1.4 Batching, and what D3D8 made cheap

- **State filtering.** `DX8Wrapper` drops redundant `SetRenderState`, `SetTextureStageState` and
  `SetTexture` calls before they reach the device (`dx8wrapper.h:975`, `:1005`, `:1029`). It applies
  the rest lazily per draw from dirty bits (`Apply_Render_State_Changes`, `dx8wrapper.cpp:2225-2368`).
  Transforms and materials are not filtered (`dx8wrapper.h:876-878`, `:947-953`).
- **Mesh renderer.** Meshes are pre-bucketed by FVF container, then pass, then texture category
  (`dx8renderer.cpp`). A category sets its textures, material and shader **once**
  (`:1685-1710`). Then, **per instance**, it sets lights and the world transform and issues
  one `DrawIndexedPrimitive` (`:1805`, `:1851-1858`). There is no instancing.
  - Skins are deformed on the CPU every frame into the dynamic VB (`:1288-1429`).
- **Dynamic buffers.** There is one global dynamic VB and one global dynamic IB.
  - The first lock of a frame uses `DISCARD`, every later one `NOOVERWRITE`
    (`dx8vertexbuffer.cpp:852-856`).
  - The offsets reset at every `Begin_Render`.
  - **On D3D8 neither flag ever waits for the GPU** (INFERRED, runtime model): DISCARD renames the
    buffer, and NOOVERWRITE is a promise not to touch in-flight bytes.
- **Text.** The text path works in three steps:
  1. A string's glyphs are re-rasterised only when its text or font changed
     (`W3DDisplayString.cpp:169-189`).
  2. `Render2DSentenceClass::Build_Textures` then makes a **new A4R4G4B4 texture per pending
     surface** and `CopyRects` the glyphs into it (`render2dsentence.cpp:326-373`).
  3. Each string costs at least one texture-renderer draw.

  On D3D8, creating a managed texture and a system-memory → texture `CopyRects` are driver-side
  allocations and copies, with no CPU wait for the GPU (INFERRED).
- **Present and render-ahead.** D3D8's `Present` queues the frame and returns. The driver lets
  the CPU run up to a few frames ahead of the GPU. So a frame costs roughly max(CPU, GPU), not
  CPU + GPU (INFERRED: driver behaviour of the period, not measured here).

### 1.5 What grows with the late game

| grows with | work | evidence of its size in the port (§3) |
|---|---|---|
| **pending path requests and how crowded the map is** | `AI::update` → `Pathfinder::processPathfindQueue`. Up to `PATHFIND_CELLS_PER_FRAME = 5000` cells per logic frame, checked only *between* requests, so one long search can overshoot (`Core/GameEngine/Source/GameLogic/AI/AIPathfind.cpp:126`, `:6087`). Per-cell cost includes `checkForMovement` over units in the cell and relationship lookups | **19-38 ms/frame** at minute ~8 (MEASURED) |
| all objects | the rest of logic: AI state machines, `PartitionManager::getClosestObjects`, weapons, production | 9-23 ms/frame (MEASURED: logic minus pathfinding) |
| all drawables | client `updateDrawable` loop, two region scans, `Visibility_Check`, mouse-pick `castRay` over the render list every frame (`W3DScene.cpp:341-370`), every volumetric shadow's `Update` | together < 1 ms/frame (MEASURED: `Drawable::updateDrawable` 0.2-0.9 ms) |
| visible objects | draw modules, HLOD bones, light environments, one draw per mesh × category × pass, skinning, shadow volumes | `RTS3DScene::Render` 0.9-1.7 ms/frame with the probe's camera on the player's base (MEASURED) |
| changed text | text texture rebuild | 4.4-6.5 ms/frame in the port at `-O0` (MEASURED); retail does the same rebuilds more cheaply (INFERRED) |
| bounded | terrain tiles, trees, shroud, water type 0, GUI | — |

**Dynamic LOD does not shed render work.** `applyDynamicLODLevel` changes only particle/debris
skipping and `m_slowDeathScale` (`GeneralsMD/Code/GameEngine/Source/Common/GameLOD.cpp:714-725`).
Shadows are static options. Mesh LOD is disabled (`W3DScene.cpp:526-532`).

**What Windows retail sees (INFERRED; nothing here was run on Windows).**
- The same logic code runs on Windows, so the pathfinding load and the lockstep slowdown are
  retail behaviour. The VC6 build is optimised.
- Big skirmishes are commonly reported to slow down late on Windows too. That is not measured in
  this repo. The `heavy-skirmish.md` scene on Windows retail remains the open control.

## 2. What the port does differently for the same work

Backend file: `spikes/renderer/src/vulkan_backend.cpp` (VB) on `main` (`030a7c11b`). The engine
reaches it through `Core/Libraries/Source/WWVegas/WW3D2/vulkanrenderbackend.cpp` (VRB), a pure
forwarding layer. `DX8Wrapper`'s redundant-state filtering (§1.4) still runs before the seam, so
the backend sees the same filtered call stream D3D8 did.

| D3D8 operation | retail cost (INFERRED) | port (cited) | measured size, late game |
|---|---|---|---|
| state setters | driver shadow copy | shadow copy only (VB:3379-3471) | — |
| `DrawIndexedPrimitive` | validate + emit | `Prepare_Draw` (VB:4103-4246) does all of the following with nothing deduplicated: fills a ~5.7 KB uniform block (INFERRED from `state_translate.h:190-261`); makes 8 sampler hash lookups; one `vkUpdateDescriptorSets` (2 writes, 8 image descriptors); hashes a 100-byte pipeline key into an `unordered_map` keyed by hash only (VB:3632-3637); 9 Vulkan calls per draw | `Prepare_Draw` **0.9-1.2 ms/frame** (MEASURED, `sample`) |
| dynamic VB/IB `DISCARD` / `NOOVERWRITE` | never waits | 3-region ring, no wait in steady state (VB:3327-3367) | none seen |
| `CreateTexture` / `Unlock` | driver allocation / async upload | each is its own `vkQueueSubmit` + `vkQueueWaitIdle` one-shot (VB:1298-1320, 3033-3072, 3239-3262) | **~12-13 waits, 4.0-4.4 ms/frame** (MEASURED, frame log, `present-pipelining.md` §4.2) |
| text `CopyRects` A4R4G4B4 → texture | async copy | MoltenVK lacks A4R4G4B4, so the texels are expanded on the CPU. Then `Flush_Frame_Commands(false)` (VB:5364) submits the frame so far and waits for the GPU **mid-frame** | 10-11 flushes per frame; 25-26 ms of queue-idle waits per frame including the one-shots (MEASURED, `text-render-flush.md` §1, §4) |
| `Present` | queue and return; GPU runs ahead | submits the blit, then `vkQueueWaitIdle`: the CPU waits for the whole frame's GPU work (VB:4636-4659) | 20-23 ms when nothing waited earlier (MEASURED, `ZH_RENDER_SYNC_PRESENT=1` run) |
| back buffer size | typically ~2 Mpx on a period or 1080p display | Retina fullscreen renders 3456x2234 = **7.7 Mpx** | ~1.2 ms GPU per Mpx in the early skirmish (MEASURED, `fullscreen-frame-cost.md` §4) |
| compiled code | VC6, optimised | **`-O0`**, with arguments spilled to the stack and no inlining (§3.4) | UNMEASURED; the H1 measurement |

Two side findings from reading the seam. Neither is a cost, and both are INFERRED, not run:

- **`DrawPrimitiveUP` from the engine may be dropped silently.** VRB passes FVF 0 (VRB:1796-1797),
  and `Draw_Primitive_UP` returns before counting the draw when `Decode_Fvf(0)` fails
  (VB:4300-4303, `state_translate.cpp:226`). The callers are the shader manager's full-screen
  quads and `W3DSmudge`.
- **The pipeline cache compares hashes only.** A 64-bit collision would bind the wrong pipeline
  (VB:3635).

## 3. Attribution of late-game frame time (MEASURED, from existing runs)

**Data.**
- `~/devin-work/present-pipelining/out/heavy-{before-1,before-2,after-1,after-2,aftersync-1}`:
  32-minute 8-player runs, M1 Pro, fullscreen 1728x1117. `before` = `main@6a593e409`;
  `after` = `perf/text-render-no-flush@dc7594b17`; `aftersync` = the same with
  `ZH_RENDER_SYNC_PRESENT=1`.
- Plus `~/devin-work/heavy-skirmish/run1` (the PR #170 build) and `~/devin-work/text-render/{before,after}`.

**Method.** All folded by the new `scripts/macos-late-game-attribution.py`. It converts each
`sample` profile's main-thread shares into **ms per frame**: share × the mean frame interval of
the two windows bracketing the profile. Binaries with different frame lengths are then compared
in time, not in percentages.

### 3.1 Main thread at game minute ~7-8 (profile round 4), ms per frame

| run | objects | mean frame | logic | of which pathfind | client total | of which `W3DDisplay::draw` | text (`Build_Textures`) | mid-frame flush | one-shot waits | Present |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| before-1 (main) | 951-1,193 | 78.1 | 50.3 | 37.5 | 27.6 | 27.2 | 23.3 | 19.5 | 4.9 | 0.9 |
| before-2 (main) | 937-1,007 | 61.4 | 44.3 | 35.6 | 17.0 | 16.8 | 14.5 | 12.4 | 2.8 | 0.5 |
| after-1 (pipelined) | 1,194-1,255 | 63.9 | 52.2 | 34.6 | 9.3 | 8.7 | 4.4 | 0 | 5.3 | 0.1 |
| after-2 (pipelined) | 971-1,109 | 57.8 | 43.5 | 21.0 | 12.1 | 11.6 | 5.3 | 0 | 6.4 | 0.1 |
| aftersync-1 | 953-979 | 69.9 | 34.8 | 19.3 | 34.7 | 34.2 | 5.0 | 0 | 5.7 | 25.0 |

Columns overlap: "text" includes the flush it triggers, and "one-shot waits" includes texture
creation inside text. Logic below 30 fps advances exactly one frame per rendered frame in every
window (`logic/s` = fps ± 0.1 in the script's window table).

What the profiles of the pipelined runs (`after-1`/`after-2`, round 4) put inside
`W3DDisplay::draw` (8.7-11.6 ms):

| item | ms/frame |
|---|---|
| `W3DDisplayString::draw` (text, including its texture creation) | 5.5-6.5 |
| `vkQueueWaitIdle` (one-shots) | 4.7-5.8 |
| `RTS3DScene::Render` (the whole 3D scene traversal and its draws) | 0.9-1.7 |
| `DX8Wrapper::Draw` | 1.0-1.2 |
| `spike::VulkanBackend::Prepare_Draw` | 0.9-1.2 |
| `Fill_Draw_Uniforms` | 0.2-0.3 |
| `vkUpdateDescriptorSets` | < 0.2 |
| pipeline lookup | < 0.2 |

**Caveat: the probe's camera sits on the player's base.** It does not follow the fighting, so
these are the draw costs of a base view, not of a battle on screen (UNMEASURED; see H6).

Inside logic (`before-2`, round 4, share of the main thread):

| function | share |
|---|---:|
| `AI::update` | 60.0 % |
| `Pathfinder::processPathfindQueue` → `findPath` → `internalFindPath` | 57.9 % |
| `examineNeighboringCells` | 54.2 % |
| `iterateCellsAlongLine` | 45.0 % |
| `checkForMovement` | 30.1 % |
| `Object/Team/Player::getRelationship` | ~7 % |
| `ScriptEngine` | 3.2 % |
| `AIUpdateInterface::update` (the unit state machines) | 5.2 % |

This corrects `probe/heavy-skirmish:docs/porting/heavy-skirmish.md` §1, which attributed the
growth to `AIUpdateInterface::update`. Most of the logic time is the shared pathfinder, reached
through `SubsystemInterface::UPDATE` → `AI::update`.

### 3.2 Late windows (game minute ≥ 12)

| run | objects | fps | interval median | renderer median (scene + present) | outside-renderer median |
|---|---|---:|---:|---:|---:|
| before-2 (main) | 1,017-1,097 | 13.4-15.7 | 47.4-52.6 | 29.6-29.9 (28.9 + 1.0) | 17.2-22.6 |
| before-1 (main) | 1,316-1,387 | 10.7-12.7 | 60.9-98.7 | 30.2-32.6 | 30.5-66.3 |
| after-1 / after-2 (pipelined) | 1,137-1,397 | 11.4-14.4 | 80.5-89.7 | 8.7-9.8 | 71.5-80.1 |
| aftersync-1 | 1,049-1,087 | 10.0-12.4 | 91.0-108.1 | 29.0-31.2 (7.3 + 23.0) | 59.2-76.9 |

"Outside the renderer" is logic, the client update apart from drawing, and the pacer's sleep. The
pacer's sleep is 0 in every late profile. Frames are bimodal (`present-pipelining.md` §4.3): light
frames and heavy-logic frames.

### 3.3 Where a late frame on `main` goes (~1,050 objects, `before-2` late)

| part | ms | evidence |
|---|---:|---|
| frame interval, median / mean | ~50 / ~68 | MEASURED, frame log |
| **logic** (pathfinding ~60-80 % of it) | ~18-22 median, ~40 mean | MEASURED: outside-renderer median; mean from profile × interval |
| client update apart from drawing | < 1 | MEASURED, `sample` |
| renderer, serialised: GPU execution waited for (mid-frame flushes + Present) | ~20-23 | MEASURED proxy: `present_ms` once nothing waits mid-frame (aftersync) |
| renderer: texture-create/upload one-shot waits | ~4 | MEASURED, `queue_wait_ms` on the branch |
| renderer: CPU (text ~4-5, 3D scene + translation ~2-3) | ~5-8 | MEASURED, `sample` |

So on `main` a late frame is **logic + GPU + waits + renderer CPU, end to end**. On Windows it
would be roughly **max(logic + renderer CPU, GPU)** (INFERRED, §1.4), with every CPU term compiled
optimised.

### 3.4 Every measured binary is unoptimised (MEASURED)

`scripts/native-build.py:1172` configures `-DCMAKE_BUILD_TYPE=Debug`. `cmake/native/CMakeLists.txt`
adds no `-O`, and the compile databases of the macOS builds show only `-g`
(`build/native-macos-arm64-0911`, `-0930b`). The new `scripts/macos-binary-opt-level.py` checks the
machine code itself.

- **Method.** It disassembles hot functions and counts two signatures of clang `-O0` code:
  - unconditional branches to the very next instruction;
  - the share of loads and stores to stack slots.
- **Self-check.** `--self-check` compiles a control function at `-O0` and `-O2`. The heuristic
  separates them: 6 next-instruction branches and 40 % stack traffic at `-O0`, against 0 and 0 % at
  `-O2`. Result: PASS.
- **Result.** Every probed function is `O0-like` in every binary: `Pathfinder::getCell`,
  `examineNeighboringCells`, `checkForMovement`, `Object::getRelationship`,
  `DX8Wrapper::Apply_Render_State_Changes` and `spike::VulkanBackend::Prepare_Draw`.
  - `examineNeighboringCells`: 768 instructions, 99 next-instruction branches, 36 % stack-slot
    traffic.
  - `getCell` is not even inlined; it shows up as its own 6-7 % frame in the profiles.
- **Binaries checked.** The `present-pipelining` `zh-before`/`zh-after`; the `text-render`
  `zh-textflush-{before,after}`; the `build-hitch` `zh-instr`; the `fs-scale` binaries; and the
  PR #170 `zh-validate`, the build the user's own report came from.

`tunnel-guard-null-attack-state.md` mentions "unoptimised, as `scripts/native-build.py` builds it"
in passing. No performance document so far had accounted for it.

## 4. The unexplained late-game regression on `perf/text-render-no-flush`

`present-pipelining.md` §4.6 found the branch binary's late game at 101.4 ms median with synchronous
Present, against `main`'s 48-52 ms at similar object counts. It left open whether that is a heavier
game state or a real CPU cost of the branch.

**What the existing data says:**

1. **The branch changed no engine code** (MEASURED, `git diff --stat 6a593e409 dc7594b17`).
   - Only `spikes/renderer/` (the backend, its tests, the window spike) and scripts and docs
     changed.
   - Logic, client and `DX8Wrapper` are the same source, built the same way (both `-O0`).
   - A branch cost outside the renderer would have to come from the backend itself, from work it
     does outside `Begin_Scene`…`End_Scene`.
2. **At game minute ~8, logic per frame is the same on both binaries** (§3.1).
   - Before: 44-50 ms. Pipelined: 44-52 ms. Aftersync: 35 ms, the *lowest*.
   - Nothing on the branch makes the same kind of game slower.
3. **Object count does not fix the logic load: within one run the outside-renderer time jumps 2-3x
   at a constant count** (MEASURED, `macos-late-game-attribution.py`):
   - `after-1`: 1,194 → 1,218 objects (rounds 4 → 6); outside median 26.7 → 79.2 ms.
   - `aftersync-1`: 1,077 → 1,024 objects (rounds 6 → 7); 32.3 → 59.7 ms, then 76.9 at 1,056.
   - `before-1` (main): 1,267 → 1,316 objects; 41.5 → 66.3 ms, then back to 30.5.

   Each binary shows such a step, so the step is the game, not the binary. The pathfinder's budget
   is per frame, but its per-cell cost grows with crowding and its overshoot with long searches
   (§1.5). A plausible driver is AI armies stuck on long or unreachable paths (INFERRED).
4. **No resource drift on the branch** (MEASURED, `monitor.csv`).
   - Footprint at 31 min: 671-676 MB before, 682-706 MB after. No runaway growth.
   - Main-thread CPU is ~96 % when pipelined and 70-78 % with a synchronous Present, as expected.
5. **Probe input diverges the games from the first minutes.** The probe sends real-time input, so
   the after runs reached 1,200+ objects by minute ~8, while the before runs had 800-1,000
   (`present-pipelining.md` §4.2).

**Reading (INFERRED, strongly supported):** the 101 ms run played a heavier game. There is no sign
of a branch CPU cost, but one cannot be excluded without a fixed workload.

**What settles it (stage 2, §7 step 3):**
- Replay `heavy-aftersync-1`'s own recording headless on the `main` binary.
  - If its minute 10-16 logic costs per frame what the branch run showed (~60-75 ms of
    outside-renderer time), the game state explains it.
- Then play the same recording rendered on `zh-before` and on `zh-after` with
  `ZH_RENDER_SYNC_PRESENT=1`.
  - The same logic on both makes any frame-time difference the branch's.
- The pre-registered verdict in `present-pipelining.md` stands as recorded. This only says what it
  was measured against.

## 5. Ranked hypotheses for the port-specific share

Ranked by expected win on a late `main` frame (~50 ms median, ~68 ms mean, ~1,050 objects). Wins
are INFERRED estimates until stage 2 measures them.

| # | hypothesis | expected win | confirm or refute with |
|---|---|---|---|
| **H1** | **The `-O0` build.** Logic (~40 ms mean), client and renderer CPU (~5-8 ms) all run unoptimised; retail is optimised. Pathfinding is pointer-chasing over small non-inlined accessors (`getCell`, `getRelationship`), the code `-O0` slows most | **largest: ~15-45 ms/frame.** Logic 2-4x faster is typical for such code (INFERRED); renderer CPU smaller | Build an `-O2 -fno-strict-aliasing` binary from the same commit. `-fno-strict-aliasing` is what upstream's MinGW build uses (`cmake/mingw.cmake:24`). Check it with `macos-binary-opt-level.py`. Play the same recording headless on both: wall seconds per 10 game minutes, no `CRC Mismatch` line. Then play it rendered with the frame log (§7 steps 1-2, 4) |
| **H2** | **CPU and GPU never overlap.** Present (and on `main`, the text flushes) waits for the GPU every frame, so frame = CPU + GPU. D3D8 overlapped them | up to min(CPU, GPU) ≈ **15-20 ms**; 10 ms measured mid-game on the branch; grows in relative terms once H1 shrinks the CPU side | Rendered replay of one recording on `zh-before`, `zh-after` and `zh-after` + `ZH_RENDER_SYNC_PRESENT=1` (§7 step 5). Same logic, so the medians compare directly |
| **H3** | **Retina fullscreen fill cost.** 7.7 Mpx vs ~2 Mpx for a typical Windows player. GPU is ~20-23 ms late, and while serialised it adds 1:1 | **~7-12 ms** while H2 stands; ~0 once overlapped and CPU-bound | Rendered replay at `-win -xres 1728 -yres 1117` vs a 1-px-per-point variant (`ZH_RENDER_SCALE`, the uncommitted candidate in `fullscreen-frame-cost.md` §5): compare `present_ms` with `ZH_RENDER_SYNC_PRESENT=1`. Apple's Metal HUD (`MTL_HUD_ENABLED=1`, not yet tried with MoltenVK here) would show GPU time per frame without a capture |
| **H4** | **One-shot submit + `vkQueueWaitIdle` per texture creation and upload.** ~12-13 per frame late, mostly text textures | **~3.5-4 ms** (MEASURED size of the waits) | Count `upload_submits`/one-shots and their ms per frame. The branch's frame-log columns (`queue_idle_waits`, `queue_wait_ms`) need porting to `main`. Then batch uploads (`build-placement-hitch.md` §6.1) and re-measure on the replay |
| **H5** | **Text textures rebuilt per changed string, plus CPU A4R4G4B4 expansion.** The rebuild is retail behaviour; the expansion, image creation and (on `main`) the flush are port costs | ~4-6 ms at `-O0` including H4's waits; **~1-2 ms** beyond H4 after H1 | Counter: strings rebuilt per frame and textures created per frame (`Render2DSentenceClass::Build_Textures`), plus `sample` on the `-O2` binary. If a few strings rebuild every frame (clock, money, "Building: N%"), caching their textures removes the work |
| **H6** | **Per-draw translation.** 9 Vulkan calls, a full descriptor update with 8 samplers, a ~5.7 KB uniform fill and a 100-byte key hash, none deduplicated | **< 1 ms** in the probe's base view (MEASURED 0.9-1.2 ms); UNMEASURED with a battle on screen | `ZH_RENDER_DRAW_REPORT=<n>` for draws per frame, plus `sample` share of `Prepare_Draw`, with the replay camera on a battle. Dedup binds and descriptor writes only if draws × per-draw cost is > 3 ms |
| **H7** | **GPU per-pixel cost of the fixed-function uber-shader** (one 8-stage fragment shader for everything) vs D3D8 hardware or driver-specialised paths | part of the 20-23 ms GPU; split UNMEASURED | GPU time per frame with `MTL_HUD_ENABLED=1`, at two render scales (H3). If GPU time does not fall with pixels, it is per-draw or per-vertex work, not fill |

**Not port-specific: pathfinding saturation and AI load.** Logic is 60-80 % pathfinding late, and
it steps up with game state (§4). It is retail behaviour, so not a fix target here; changing it
would change the simulation and break replay compatibility. It is the floor that H1 shrinks,
not removes.

## 6. Can the native build play back a recorded replay? Yes

- **Code path.** `-replay <file>.rep` (`GeneralsMD/Code/GameEngine/Source/Common/CommandLine.cpp:429-451`)
  queues the file and turns off the intro, sizzle and shell map. `GameMain` then calls
  `ReplaySimulation::simulateReplays` (`GameMain.cpp:48-50`). Two modes:
  - **with `-headless`**: logic only, as fast as the CPU allows, no renderer. It prints
    `Elapsed Time: mm:ss Game Time: mm:ss/mm:ss` every 10 game minutes and at the end, and
    `CRC Mismatch in Frame N` on divergence
    (`Core/GameEngine/Source/Common/ReplaySimulation.cpp:82-123`).
  - **without `-headless`**: `TheRecorder->playbackFile()` then `TheGameEngine->execute()`
    (`ReplaySimulation.cpp:59-80`). This is the full game rendering the replay, frame pacer
    included. A CRC mismatch **pauses** the game (`Recorder.cpp`, `handleCRCMessage`).
- **Divergence is checked.** Skirmish replays carry a logic CRC every `REPLAY_CRC_INTERVAL` = 100
  frames (`GameLogic.cpp:3768-3790`, `Recorder.cpp:60`). A playback that diverges from the
  recording says so.
- **No version gate in release builds.** The version, build-time and exe-CRC comparison is inside
  `#ifdef DEBUG_CRASHING` (`Recorder.cpp:1136-1180`). A recording from one native build plays on
  another, which A/B needs.
- **Measured precedent.** `combat-probe.md` §0/§10 ran an arm64 binary
  `./zh -headless -replay 00000000.rep` on its own live recording, under LLDB. The replay shutdown
  then hits the known `ObjectPoolClass` `SIGSEGV` after the last frame (`playability-probe.md` §7).
  **So the exit code is not the verdict; the printed lines are.**
- **Recordings already on disk** (MEASURED, file listing). Each run's user data holds
  `Command and Conquer Generals Zero Hour Data/Replays/00000000.rep`:

  | run | size | covers |
  |---|---:|---|
  | `heavy-before-1` | 2.3 MB | ~min 15 |
  | `heavy-before-2` | 2.7 MB | ~min 18 |
  | `heavy-after-1` | 2.5 MB | |
  | `heavy-after-2` | 2.6 MB | |
  | `heavy-aftersync-1` | 2.4 MB | the 101 ms case, ~min 16 |
  | `text-render/{before,after}` | 1.8-2.0 MB | |

  All were recorded by `6a593e409`-based binaries. Since then only
  `AITNGuard.cpp` changed (#171, a null check on a path that crashed). So they should play on
  `main@030a7c11b` without divergence (INFERRED; the CRC check confirms it either way).
- **UNVERIFIED** (at stage 1; stage 2 ran the rendered path, see §10.6: it draws nothing until
  `m_breakTheMovie` is cleared, and the camera is the observer's start view).
  - The *rendered* (`-replay` without `-headless`) path on macOS has not been run.
  - What the camera shows during playback: INFERRED to be the local player's start view, which
    matches the probe's base view.
  - The header's replay name is a single `L` character. This looks like a wide-string formatting
    quirk, and matters only to the in-game replay menu, not to `-replay`.

**Commands.** `RUN` holds the retail data and the binaries; `UD` is a *copy* of the run's user data,
never the original.

```sh
RUN=~/devin-work/present-pipelining/run
UD=<scratch>/ud-before2 && cp -R ~/devin-work/present-pipelining/out/heavy-before-2/userdata "$UD"

# logic only: deterministic, no GPU
cd "$RUN" && CNC_USER_DATA="$UD" /usr/bin/time -l ./zh-O0 -headless -replay 00000000.rep

# rendered: the same logic, frame log on, fullscreen as in the probes
cd "$RUN" && CNC_USER_DATA="$UD" ZH_RENDER_FRAME_LOG=<out>/frames.csv \
    ./zh-O0 -nologo -xres 1728 -yres 1117 -replay 00000000.rep
```

## 7. Stage-2 measurement plan (in order; each step's rule is written before its data)

1. **Build the pair from `main@030a7c11b`.** A is today's build. B is the same with optimisation,
   in a fresh build directory, because CMake reads the flags only at first configure:

   ```sh
   arch -arm64 python3 scripts/native-build.py --level 1 --level 2 --level 3 --level 4 \
       --with-shims --strict-link --build-dir build/native-macos-arm64-o0
   CFLAGS="-O2 -fno-strict-aliasing" CXXFLAGS="-O2 -fno-strict-aliasing" \
   arch -arm64 python3 scripts/native-build.py --level 1 --level 2 --level 3 --level 4 \
       --with-shims --strict-link --build-dir build/native-macos-arm64-o2
   python3 scripts/macos-binary-opt-level.py --self-check
   python3 scripts/macos-binary-opt-level.py build/native-macos-arm64-o{0,2}/native_strict_link
   lipo -archs build/native-macos-arm64-o2/native_strict_link; sysctl -n sysctl.proc_translated
   ```

   **Gate:** A reports `O0-like` and B reports `optimised` on every symbol; `arm64`; `0`. Copy both
   into `RUN` and re-sign with `get-task-allow`.
2. **H1, logic only.**
   - Headless replay of `heavy-before-2` and of `heavy-aftersync-1`'s recording on A and B, twice
     each: 8 runs.
   - Record the `Elapsed Time` lines, `/usr/bin/time -l` (user CPU, peak RSS) and any
     `CRC Mismatch`.
   - Take a 10 s `sample` once the game-time line passes 10:00.
   - **Proposed rule:**
     - B is a valid optimised build only if neither recording prints a mismatch on B.
       - A mismatch on B but not A means `-O2` exposed undefined behaviour. That is a finding in
         its own right: bisect it with `CRCDiag`, `crc-divergence.md`.
     - H1 is confirmed if B's wall time for game minutes 10-end is ≤ 0.6x A's on both recordings.
3. **Item 3.** From step 2's A runs, compare the two recordings' wall time per logic frame for game
   minutes 10-end.
   - If `aftersync-1`'s game costs ≥ 2x `before-2`'s per frame, the game-state reading of §4 is
     confirmed.
   - Otherwise a branch CPU cost is back on the table, and step 5 tests it.
4. **H1, rendered.**
   - Rendered replay of `heavy-before-2` on A and B, frame log on, fullscreen 1728x1117.
   - First verify that the rendered path runs at all (§6 UNVERIFIED). If playback pauses, a CRC
     mismatch fired.
   - Compare medians and p95 over the late segment, aligned by frame index. One logic frame per
     rendered frame makes index ≈ logic frame plus a constant load offset.
   - Adding a `logic_frame` column to the frame log makes the alignment exact (step 6).
5. **H2 and §4.**
   - Rendered replay of `heavy-aftersync-1`'s recording on `zh-before`, on `zh-after`, and on
     `zh-after` with `ZH_RENDER_SYNC_PRESENT=1`. These are the `present-pipelining` binaries:
     `-O0`, same engine source.
   - Same logic on all three: a frame-time difference is the backend's.
   - Then repeat the pipelined/sync pair on B. With the CPU smaller, overlap matters more.
6. **Counters to add.** One env-gated instrumentation commit; no behaviour change. Frame-log
   columns:
   - `logic_frame`;
   - `logic_ms` (`GameLogic::update` wall time);
   - `client_ms` (`GameClient::update` minus `DRAW`);
   - `pathfind_cells` and `pathfind_paths` (`processPathfindQueue` already computes both, and its
     `PROFILER_PLOT` lines name them);
   - `draws_issued` (from `DrawStats`);
   - `text_textures_created`;
   - one-shot count and ms. Port `perf/build-placement-hitch`'s columns, which are not on `main`.

   These turn every later frame log into an attribution without `sample`.
7. **H3/H7 (GPU).** Same rendered replay with `MTL_HUD_ENABLED=1` (GPU ms per frame) and windowed
   vs fullscreen. Add the render-scale candidate if it lands first.
8. **H6 (big battle on screen).** Only if steps 2-7 leave a gap. Point the replay camera at a fight
   (input during playback scrolls the camera without touching the simulation; INFERRED) and read
   `ZH_RENDER_DRAW_REPORT` and `Prepare_Draw`'s share.

## 8. UNMEASURED and limits

- **Every number here comes from `-O0` binaries.** The shares in §3 will move once H1 is measured.
  Pathfinding is likely to shrink most, so the renderer's relative share will rise.
- **The Windows retail control** (the same scene or replay on Windows) does not exist.
  "Retail behaviour" claims about Windows frame time are INFERRED from source.
- **The probe's camera never looks at a battle.** Draw counts in a late fight are unknown;
  `draws-per-frame.md` measured only 69-78 per frame early in a mission.
- **`sample` profiles cover game minutes 0-8 of the 32-minute runs, plus minute ~19 in
  `heavy-skirmish` run 1.** No profile covers the late windows in which the branch runs stepped up.
- **The D3D8 runtime and driver behaviour in §1.4 is general knowledge, not measured.** This
  includes render-ahead, and DISCARD/NOOVERWRITE and managed textures never waiting.

## 9. Reproducing this document's numbers

```sh
python3 scripts/macos-late-game-attribution.py \
    ~/devin-work/present-pipelining/out/heavy-{before-1,before-2,after-1,after-2,aftersync-1}
python3 scripts/macos-late-game-attribution-test.py   # the folding code's unit tests
python3 scripts/macos-binary-opt-level.py --self-check
python3 scripts/macos-binary-opt-level.py ~/devin-work/present-pipelining/run/zh-{before,after} \
    ~/devin-work/cocoa-keys/run/zh-validate
git diff --stat 6a593e409 dc7594b17         # the branch changed no engine source
git diff --stat 6a593e409 030a7c11b -- GeneralsMD Core   # since the recordings: AITNGuard only
```

The run data, profiles and recordings stay outside the repository.

## 10. Stage 2: measurement on the Mac

### 10.1 Pre-registered decision rules (written before any stage-2 data)

Recorded verbatim as given for stage 2, then applied mechanically. They are not tuned after the
data.

- **H1 (-O0 build):** B (optimised, `-O2 -fno-strict-aliasing`, same commit `030a7c11b`) is valid
  only if headless replay playback prints no `CRC Mismatch` / desync on both recordings. H1
  CONFIRMED if B's headless wall time over game minutes 10→end is ≤ 0.6× A's on both recordings
  (2 runs each); and, rendered, B's late-game (≥ min 12 of replay) median frame time is ≥ 8 ms
  lower than A's. If B desyncs: H1 is NOT landable as-is — find the first diverging subsystem if
  cheap, else report.
- **H2 (no CPU/GPU overlap):** only meaningful on top of B. Report numbers; no landing decision in
  this stage.
- **"Heavier game" explanation of the old branch regression:** CONFIRMED if A's per-logic-frame
  cost differs ≥ 2× between the two recordings at the same game minute.

**Operational definitions, fixed with the rules:**

- *Same commit.* A and B are built from `030a7c11b` plus two off-by-default measurement hooks
  (`ZH_LOGIC_FRAME_LOG`, `ZH_ENGINE_FRAME_LOG`, §10.2). With the variables unset neither hook
  reads the clock or writes anything; both A and B carry them, so the pair differs only in flags.
- *Headless wall time, minutes 10→end.* From `ZH_LOGIC_FRAME_LOG`: the end of the last logic
  frame (`start_ms + logic_ms`) minus `start_ms` of logic frame 18,000 (10 × 60 × 30). The
  binary's own `Elapsed Time` lines (1 s resolution) are the cross-check. Per recording, the mean
  of B's two runs is compared with the mean of A's two runs; all four runs are reported.
- *Desync.* Any `CRC Mismatch` line on stdout/stderr, or a run that stops short of the recording's
  last frame for any reason other than the known post-last-frame `SIGSEGV`.
- *Per-logic-frame cost at the same game minute.* Mean `logic_ms` per logic frame on A (both runs
  pooled), over the game minutes both recordings cover from minute 10 on. CONFIRMED if the larger
  recording's mean is ≥ 2× the smaller's over that common window. Per-minute ratios are reported
  too but do not decide.
- *Rendered late-game frame time.* From `ZH_ENGINE_FRAME_LOG`: the difference between successive
  passes' `start_ms`, over passes whose `logic_frame` ≥ 21,600 (game minute 12) up to the
  recording's last frame. One run per binary; median and p95 reported.

### 10.2 What was built and run

All runs: M1 Pro (10 cores, 16 GB), macOS 26, AppleClang 21 (`clang-2100.3.34.2`), arm64
(`lipo -archs` = `arm64`, `sysctl.proc_translated` = 0), one game process at a time, each on a
private copy of the recording's user data. Run data stays outside the repository
(`~/devin-work/late-game/`).

| binary | flags | commit | `macos-binary-opt-level.py` |
|---|---|---|---|
| **A** | as `native-build.py` builds today (`CMAKE_BUILD_TYPE=Debug`, `-g`, no `-O`) | `aa606e0d8` | every probed symbol `O0-like` |
| **B** (pre-registered) | `-O2 -fno-strict-aliasing` | same | every probed symbol `optimised`; `Pathfinder::getCell` inlined away (no symbol) |
| **B2** (added after B failed, §10.3) | `-O2 -fno-strict-aliasing -fmath-errno` | same | same as B |

- **How the flags were passed.** `CXXFLAGS`/`OBJCXXFLAGS` in the environment of the first
  `native-build.py` configure of a fresh `--build-dir`. CMake copies them into `CMAKE_CXX_FLAGS`
  (checked in `CMakeCache.txt` and in `compile_commands.json`: `-O2 -fno-strict-aliasing -g
  -ffp-contract=off`). `native-build.py` needs no change. A full build takes ~3.5 min at either
  level.
- **Commit.** `030a7c11b` plus two measurement hooks, both dormant unless their variable is set:
  - `ZH_LOGIC_FRAME_LOG=<csv>`: one row per `GameLogic::update`: `frame`, `start_ms`,
    `logic_ms`, `ai_ms` (`TheAI->UPDATE()`, i.e. the pathfinder and AI players), `objects`.
  - `ZH_ENGINE_FRAME_LOG=<csv>`: one row per `GameEngine::update` pass (one rendered frame):
    `logic_frame`, `start_ms`, `client_ms` (`GameClient::UPDATE`, which draws and presents),
    `logic_ms`.
- **Scripts.** `scripts/macos-late-game-replay.py` runs one replay on one binary;
  `scripts/macos-late-game-replay-summary.py` folds runs into the tables below and applies §10.1
  (unit-tested by `-test.py`); `scripts/macos-opt-divergence-bisect.py` finds which optimised
  archive, object file and flag desyncs a replay.
- **Recordings.** `heavy-before-2` plays to logic frame 32,098 (17:49 of game time) and
  `heavy-aftersync-1` to 28,148 (15:38), on every in-sync run. Their headers carry no frame count
  (the probe killed the recording game), so "played to the end" means "reached the frame every A run
  reaches". Neither covers the probes' full 32 minutes.

### 10.3 B goes out of sync: `__sincosf_stret` in `Locomotor.cpp` (MEASURED)

**B desyncs on both recordings, every time** (MEASURED): `CRC Mismatch in Frame 700` on
`heavy-before-2` (every one of more than ten runs, including two timed ones) and `in Frame 2600` on
`heavy-aftersync-1` (one run). A plays both to the end in sync, four
runs out of four. By the pre-registered rule, **B is invalid and H1 is not landable as-is.**

Finding the cause was cheap, so it was done:

1. **What differs.** `CNC_CRC_DIAG` on A and B, then `CNC_CRC_DIAG_BYTES=700`: checkpoint 600
   matches. At 700 the first differing record is object 250, a `SupW_AmericaVehicleDozer`.
   Its position bytes are equal; the rotation elements of its `Matrix3D` differ in the low bits
   (`0x3F57D5A7` vs `0x3F57D62C`). A turning unit's heading drifts.
2. **Which code.** `macos-opt-divergence-bisect.py` relinks mixed binaries:
   - one optimised archive at a time, the rest `-O0`: only `libgeneralsmd_code_gameengine.a`
     desyncs;
   - halving its 381 members: **`Locomotor.cpp.o` alone desyncs**, and the other 380 members
     optimised do not (to frame 800).
3. **Which optimisation.** `Locomotor.cpp` recompiled with variants, everything else `-O0`:

   | `Locomotor.cpp` flags | frame 700 |
   |---|---|
   | `-O2 -fno-strict-aliasing` | mismatch |
   | `-O1` | mismatch |
   | `-O2 … -fno-vectorize -fno-slp-vectorize` | mismatch |
   | `-O2 … -fwrapv` | mismatch |
   | `-O2 … -ftrivial-auto-var-init=zero` | mismatch |
   | `-O0 -ftrivial-auto-var-init=pattern` | in sync |
   | `-O2 … -fno-builtin` | **in sync** |
   | `-O2 … -fmath-errno` | **in sync** |

4. **Mechanism.** Apple clang defaults to `-fno-math-errno` on Darwin, so `sinf`/`cosf` are pure
   intrinsics. When `sinf(x)` and `cosf(x)` of the same `x` meet after inlining, the backend
   fuses them into one call to **`__sincosf_stret`**, whose results are not bit-identical to the
   separate calls. `nm -u` on the object: `-O0` imports `_sinf`, `_cosf`; `-O2` imports
   `___sincosf_stret` instead. The two fused sites are `Locomotor::rotateObjAroundLocoPivot`
   (inlined `Matrix3D::In_Place_Pre_Rotate_Z`) and `tryToRotateVector3D` (inlined
   `Matrix3D::Set(axis, angle)`). The whole B binary imports `___sincosf_stret`; B2 imports none.

It is a floating-point *library* difference, not undefined behaviour: uninitialised-variable
patterns, wrap-around and vectorisation each change nothing. It is the same class of problem as
`-ffp-contract=off` (`crc-divergence.md`), one level up.

**B2 adds `-fmath-errno` and plays both recordings to the end in sync, four runs out of four**
(MEASURED). B2 was not pre-registered: everything measured on it below is reported against the
same thresholds, but as a post-hoc result, not a rule outcome. The 380 other archive members passed
only to frame 800 of one recording in the bisection; B2's full-length runs on both recordings are
the evidence for the whole binary. Two recordings are not a proof that no other optimisation
changes the simulation anywhere.

### 10.4 H1, logic only: headless replay (MEASURED)

Wall seconds from logic frame 18,000 (game minute 10) to the end of the recording, from
`ZH_LOGIC_FRAME_LOG`. The binary's own `Elapsed Time` lines agree to the second (A, `before-2`:
04:47 → 13:23 = 516 s).

| binary | recording | runs | CRC | wall s, min 10→end | logic ms/frame, min 10→end | of which AI (`TheAI->UPDATE`) | peak RSS MB |
|---|---|---:|---|---:|---:|---:|---:|
| A | `before-2` | 2 | in sync to frame 32,098 | 516.1 / 514.8 | 36.5 | 23.9 (65 %) | 285 |
| A | `aftersync-1` | 2 | in sync to frame 28,148 | 554.8 / 555.5 | 54.6 | 40.7 (75 %) | 285-291 |
| B | `before-2` | 2 | **mismatch at frame 700** | — | — | — | 237-255 |
| B | `aftersync-1` | 1 | **mismatch at frame 2,600** | — | — | — | 243 |
| B2 | `before-2` | 2 | in sync to frame 32,098 | 130.2 / 128.4 | 9.1 | 6.3 (69 %) | 281-284 |
| B2 | `aftersync-1` | 2 | in sync to frame 28,148 | 136.5 / 137.5 | 13.5 | 10.4 (77 %) | 287 |

**Rule outcomes.**

- **H1 (pre-registered, B): B INVALID.** B desyncs on both recordings, so H1 is not landable as-is
  (§10.3).
- **H1 headless part, post hoc on B2:** B2/A = **0.251** (`before-2`) and **0.247**
  (`aftersync-1`), against the ≤ 0.6 bar. Optimisation makes the logic **~4.0x** faster; the
  pathfinder-heavy AI share barely moves (65 → 69 %, 75 → 77 %), so the pathfinder speeds up about
  as much as the rest.
- **Peak memory** is unchanged by optimisation (~285 MB either way, headless).

A `sample` of the main thread at game minute 12 (10 s) puts the same shape on both builds: A on
`aftersync-1`: `AI::update` 77.5 %, `processPathfindQueue` 75.7 %, `examineNeighboringCells`
72.1 %, `checkForMovement` 30.4 %, `Object::getRelationship` 5.3 %. B2 on `before-2`: 67.2 %,
67.0 %, 63.3 %, 25.7 %, **0.2 %** (inlined away). The pathfinder is still two-thirds of the logic
at `-O2`; it is the retail algorithm, so it stays the logic floor (§5).

### 10.5 "Heavier game" (pre-registered): NOT CONFIRMED (MEASURED)

A's mean logic cost per frame over game minutes 10-15 (the window both recordings cover):
`aftersync-1` **53.5 ms**, `before-2` **37.4 ms**: ratio **1.43**, below the 2x bar. Per game
minute (A, run 1, ms/frame and mean object count):

| minute | 3 | 8 | 10 | 11 | 12 | 13 | 14 | 15 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `before-2` | 9.4 (544) | 39.1 (951) | 29.6 (1,015) | 33.0 (1,036) | 44.1 (1,057) | 42.5 (1,026) | 38.0 (1,048) | 31.9 (984) |
| `aftersync-1` | 18.8 (573) | 52.0 (998) | 45.3 (1,034) | 48.6 (1,035) | 48.9 (1,063) | 57.2 (1,110) | 67.2 (1,111) | 63.8 (1,069) |
| ratio | 2.0 | 1.3 | 1.5 | 1.5 | 1.1 | 1.3 | 1.8 | 2.0 |

So the `aftersync-1` game *is* heavier, by 1.1-2.0x minute by minute at similar object counts, but
not by the pre-registered 2x. What the replay does settle (§4): the same code (A) costs 50-67 ms of
logic per frame in that game's minutes 12-15. The branch run's late *outside-renderer* median was
59-77 ms (§3.2), so its logic time is accounted for by the game itself, at the same binary
optimisation; a branch CPU cost is not needed to explain it (INFERRED from the match, not from a
rendered A/B of that recording). The recording ends at 15:38, so the branch run's later windows
are not covered.

### 10.6 H1, rendered: the same recording played on screen (MEASURED)

**A command-line replay does not draw on its own (MEASURED, new finding).** `-replay` without
`-headless` runs the game loop, but the screen stays blank and the backend presents nothing:
`ZH_RENDER_FRAME_LOG` stays empty, and a `sample` shows `W3DDisplay::draw` never reaching
`WW3D::Begin_Render`. LLDB shows why: `TheWritableGlobalData->m_breakTheMovie` is `true`.
`Intro::doPostIntro()` sets it (`Core/GameEngine/Source/GameClient/Intro.cpp:255`), and only
`MainMenuInit` and the single-player load screen clear it. A `-replay` run reaches neither, and
`W3DDisplay::draw` skips the whole render block while it is set (`W3DDisplay.cpp:1981`). The
runner therefore clears the flag once through LLDB at logic frame ~40 and detaches; the binaries
are unchanged. This is a bug of its own, and the fix is small (clear the flag when a replay
simulation starts), but it is a behaviour change and was left for stage 3. Whether Windows has it
too is UNMEASURED; the code path is not platform-specific.

**Setup.** `heavy-before-2`, fullscreen 1728x1117 points (3456x2234 pixels), one run per binary,
frame pacer as shipped. The replay camera is the observer's start view, on a supply stash in the
map centre, with the observer's per-player money overlay (8 lines) on screen (screenshot checked,
not committed). Both runs stayed in sync to frame 32,098.

Late window: logic frames 21,600-32,097 (game minutes 12-17:49), 10,498 passes each.

| binary | median frame ms | p95 | fps | client (draw + present) median | logic median | backend `scene_ms` median | `present_ms` median |
|---|---:|---:|---:|---:|---:|---:|---:|
| A | **53.3** | 144.0 | 13.8 | 33.4 | 17.6 | 30.9 | 1.0 |
| B2 | **37.0** | 61.0 | 24.0 | 30.9 | 4.9 | 28.8 | 1.0 |

- **Post hoc on B2:** the median falls by **16.3 ms**, against the ≥ 8 ms bar; p95 falls by 83 ms.
  With §10.4, B2 meets both halves of the H1 thresholds. B (the pre-registered binary) was not
  played rendered, because it desyncs.
- A's late median (53.3 ms) matches the live `heavy-before-2` probe's 47-53 ms (§3.2), so the
  replay reproduces the live late game's frame cost.
- **Optimisation removes logic, not renderer time.** Logic median 17.6 → 4.9 ms; the client update
  (drawing and presenting) only 33.4 → 30.9 ms, because most of it is waiting for the GPU.

**Where a late B2 frame goes** (`sample`, 10 s at game minute 14, ms = share × the window's mean
frame of 48.7 ms; the sampled window is heavier than the median):

| part | share | ms/frame |
|---|---:|---:|
| logic (`GameLogic::update`) | 29.8 % | 14.5 |
| … of which the pathfinder | 20.7 % | 10.1 |
| client update | 70.0 % | 34.1 |
| … text: `Render2DSentenceClass::Build_Textures` | 61.3 % | 29.9 |
| … … mid-frame `Flush_Frame_Commands` (the text flush) | 52.6 % | 25.6 |
| … … one-shot submits (`End_One_Shot`) | 12.1 % | 5.9 |
| … all `vkQueueWaitIdle` | 62.2 % | 30.3 |
| … the 3D scene (`RTS3DScene::*`) | 3.9 % | 1.9 |
| … per-draw translation (`Prepare_Draw`) | 0.8 % | 0.4 |
| … `Present` | 2.4 % | 1.1 |

So once the code is optimised, **the late replay frame is the text path's GPU waits**: the
mid-frame flush that `perf/text-render-no-flush` removes, plus one-shot uploads. Logic is a third
of the frame; the 3D scene and the draw translation are noise. Caveat: the replay observer shows
eight players' money, which a live player does not (INFERRED to rebuild text often; the rebuild
count per frame is UNMEASURED), so this text load is likely above a live game's. The A profile
of the same minute (mean frame 125.8 ms during the sample, an outlier stretch) puts 68 % in logic
and 52 % in the pathfinder.

### 10.7 H2 on top of B2: overlapping CPU and GPU (MEASURED, numbers only)

`perf/text-render-no-flush`'s backend (`dc7594b17`: no mid-frame text flush, `Present` does not
wait) on this commit's engine, built like B2. The branch changed only `spikes/renderer/src/`, and
`main` has not touched that directory since its base, so `git checkout dc7594b17 --
spikes/renderer/src/` over this branch is the branch's backend on today's engine. Same replay,
same window as §10.6, one run each, both in sync to frame 32,098.

| build | median frame ms | p95 | mean fps | client median | logic median | `present_ms` median | one-shot waits / frame | their ms |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| B2, `main` backend (§10.6) | 37.0 | 61.0 | 24.0 | 30.9 | 4.9 | 1.0 | — | — |
| B2 + branch backend, `ZH_RENDER_SYNC_PRESENT=1` | 33.4 | 57.4 | 25.5 | 27.8 | 4.8 | **21.2** | 12 | 3.4 |
| B2 + branch backend, pipelined | **33.3** | **38.8** | **29.2** | **6.8** | 6.3 | 0.1 | 12 | 3.5 |

- **Optimised and pipelined, the late replay runs at the 30 fps the logic is capped at.** The
  median sits on the pacer's 33.3 ms; a `sample` at minute 14 has the pacer sleeping 34 % of the
  main thread (~12 ms per frame), logic 45 % (16 ms in that heavier window), pathfinder 29 %, text
  11 %, one-shot waits 13 %, the 3D scene 7 %.
- **The GPU's frame is ~21 ms** (`present_ms` with synchronous Present, which then waits for the
  whole frame's GPU work) at 3456x2234 pixels. Serialised, it adds 1:1 to the CPU's ~12-17 ms and
  the frame misses 30 fps often (p95 57 ms); overlapped, it hides behind the pacer (p95 39 ms).
- `frame_wait_ms` and `inflight_waits` are 0 in the pipelined run: the CPU never waits for a
  previous frame's GPU work, so the GPU is not yet the limit at 30 fps.
- Still present on every build: **12 one-shot submit + `vkQueueWaitIdle` per frame, ~3.5 ms**
  (H4), now about a tenth of the frame budget.

### 10.8 Hypotheses re-ranked after optimisation

Late `heavy-before-2` replay frame on B2 (`main` backend), median 37 ms; the 30 fps cap is 33.3 ms.

| rank | hypothesis | status after stage 2 | what is left of it |
|---|---|---|---|
| 1 | **H1, the `-O0` build** | logic 4.0x faster headless; rendered median −16.3 ms, p95 −83 ms. Not landable as `-O2` alone: it desyncs (§10.3); `-O2 -fmath-errno` plays both recordings in sync | the largest single win, measured |
| 2 | **H2 + H5 flush, CPU/GPU serialisation** | on B2 it is now the largest remaining cost: the client update is 31 ms median, ~26 ms of it waiting in mid-frame text flushes. With the branch backend, client 31 → 7 ms and the replay holds 30 fps | ~4-24 ms, depending on how much text is rebuilt (median 37 → 33.3, p95 61 → 39) |
| 3 | **H4, one-shot upload waits** | unchanged by optimisation: 12 per frame, ~3.5 ms | ~3.5 ms |
| 4 | **H3/H7, GPU per frame** | ~21 ms at 7.7 Mpx; hidden once pipelined, so not on the critical path at 30 fps. Not split into fill vs per-draw (no render-scale run) | 0 ms while pipelined; headroom for heavier scenes is ~12 ms |
| 5 | **H5 beyond the flush, CPU text rebuild** | `Build_Textures` CPU is a few ms; its size in a live game is unknown because the replay observer shows 8 players' money | ~1-4 ms (INFERRED) |
| 6 | **H6, per-draw translation** | `Prepare_Draw` 0.4-0.5 ms per frame on B2 (observer camera, not a battle) | < 1 ms |
| — | pathfinder (retail behaviour) | still 63-77 % of logic at `-O2`; 6-16 ms per frame late | the logic floor; not a port fix |

### 10.9 Recommendation for stage 3

1. **Build the game optimised: `-O2 -fno-strict-aliasing -fmath-errno`, and keep
   `-ffp-contract=off`.** Expected win: logic ~4x faster (late logic 36-55 → 9-14 ms per frame on
   these recordings); rendered late median −16 ms, p95 roughly halved.
   - **How to gate it.** Add an optimisation level to `scripts/native-build.py` (for example
     `--optimize`, adding the flags in `cmake/native/CMakeLists.txt` rather than through the
     environment, so the compile database records them), and make it the default for the game
     binary. Gate on: `macos-binary-opt-level.py` reporting `optimised`; both recordings here
     played headless to the end with no `CRC Mismatch`; and the existing Linux replay-CRC gate.
     Ideally add one heavy skirmish recording to CI's replay set: these two found the problem
     within 700 frames.
   - **What it can break.** Determinism, in exactly the way §10.3 found: any libm call the
     optimiser rewrites (fusion into `sincos`, constant folding, `-fno-math-errno` purity)
     changes simulation floats. `-fmath-errno` covers the one observed. A more robust option is to
     route the simulation's trigonometry through one out-of-line, explicitly sequenced function
     (`Sin`/`Cos` in `Trig.cpp` already exist; `Matrix3D`'s inline `sinf`/`cosf` bypass them).
     Also possible: UB that `-O0` hid. None was seen in two full recordings; the replay CRC check
     is the guard. Debugging gets harder; keep `-g`.
   - **Cross-build replays.** Recordings made by `-O0` binaries play in sync on B2, so existing
     recordings stay valid.
2. **Land the non-blocking Present and the flush-free text path (`perf/text-render-no-flush`).** On
   top of item 1 it is what takes this replay from 24 to 29 fps and the p95 from 61 to 39 ms. Its
   own pre-registered verdict (`present-pipelining.md`) was measured at `-O0`, where logic hid it;
   re-judge it on an optimised build.
3. **Fix the command-line replay that does not draw** (§10.6): clear `m_breakTheMovie` when a
   replay simulation starts. One line, behaviour change, own PR. It makes rendered replays usable
   as the fixed workload for every later performance A/B.
4. **Then, in order of what is left:** batch the 12 one-shot upload waits (H4, ~3.5 ms); measure
   the text rebuild count in a live game before caching strings (H5); leave per-draw translation
   (H6) alone.

### 10.10 Limits and UNVERIFIED

- **One machine, one rendered run per build, one recording rendered.** Run-to-run spread of the
  rendered medians is UNMEASURED; headless timings repeat to within 1.4 % (A within 0.3 %).
- **The recordings stop at 15:38 and 17:49** of the 32-minute probe games. Later minutes, where
  the branch run's frame time stepped up, are not covered.
- **The replay camera is not the player's.** It shows the observer's start view and its money
  overlay, so draw counts and text load differ from a live game. A battle on screen (H6) is still
  unmeasured.
- **B2's determinism rests on two recordings** (both skirmish, same map). The bisection checked
  the other 380 optimised members only to frame 800.
- **What `-fmath-errno` itself costs is UNMEASURED**: B, the only binary without it, desyncs, so no
  like-for-like timing exists.
- **The `sample`-based splits** are single 10 s windows at minute ~14, heavier than the medians.
- **Windows.** Whether a VC6 or MSVC optimised build has the `__sincosf_stret` problem (it would not
  use that function, but may have its own fused forms) and the blank `-replay` screen is
  UNMEASURED.
