# The shroud's out-of-grid lookup under river water

A campaign mission dies in the render path on both non-Windows platforms:

```text
EXC_BAD_ACCESS (KERN_INVALID_ADDRESS)
W3DShroud::getShroudLevel(int, int)
getRiverVertexDiffuse(W3DShroud*, float, float, float, float, float, int)
WaterRenderObjClass::drawRiverWater(PolygonTrigger*)
WaterRenderObjClass::renderWater()
WaterRenderObjClass::Render(RenderInfoClass&)
```

`getShroudLevel` indexes `m_srcTextureData` with `x*2 + y*m_srcTexturePitch` and only ever tested the
far end of the grid (`x < m_numCellsX && y < m_numCellsY`), so a negative cell subtracts from the
pointer and reads memory that precedes the shroud's own allocation. #103 recorded the crashing cell as
`x=48, y=-1`.

The question that decides the fix is not "where does the guard go" but **why the coordinate is out of
range**: either (a) the river polygon legitimately extends past the shroud grid, or (b) the
world-to-cell conversion is wrong on this port and a bounds guard would hide it while shading the
water from the wrong cell. §2 answers that with a measurement; it is (a), and the coordinate this port
computes is the coordinate Windows computes. §3 is therefore about *what value the edge should have*
rather than about avoiding a fault.

Companion documents: [real-input-menu-drive.md](real-input-menu-drive.md) §4.2 (where this crash was
recorded and deliberately left) and §6 item 3 (this slice), and
[headless-simulation-probe.md](headless-simulation-probe.md) (`sim_probe`, extended here).

## 1. The map that reaches it — and why #115's mission did not

The path needs a `PolygonTrigger` whose water body is a *river*: `drawRiverWater` is only called for
triggers `isWaterArea()` with more than four points, and only the river branch shades vertices with the
shroud. The USA mission driven with real input in #115 has no such trigger, which is why that drive
reached a running mission and never touched this code.

`sim_probe rivers` (new, `spikes/sim/src/sim_probe.cpp`) parses a `.map` outside the game — the
`HeightMapData` header for the map's dimensions and border, then `PolygonTriggers` through the engine's
own `PolygonTrigger::ParsePolygonTriggersDataChunk` — and converts every water-area vertex to a shroud
cell the way the water path did. Over the 171 `.map` files of the retail install (116 Zero Hour, 55
Generals), **46 have at least one water-area vertex outside the shroud grid** (35 and 11). The first USA
campaign mission is one of them:

| | |
|---|---|
| map | `MapsZH/Maps/MD_USA01/MD_USA01.map` |
| trigger | `Water Area 5`, 104 points, of which 13 fall outside the grid |
| map / border | 610x460 cells, border 70 |
| shroud grid | 118x80 cells of 40 world units (`PartitionCellSize`) |
| source pitch | 236 bytes |

and its vertices in the border ring, as measured:

| vertex | world (x, y) | cell (truncating, as shipped) | byte offset from `m_srcTextureData` |
|---|---|---|---|
| point 55 | (1923, -79) | (48, -1) | **-140** |
| point 54 | (1910, -149) | (47, -3) | -614 |
| point 52 | (2069, -158) | (51, -3) | -606 |
| point 0 | (-320, 1759) | (-8, 43) | 10132 — inside the surface, but 16 bytes short of row 43, so a pixel of row 42 |
| point 53 | (1927, -204) | (48, -5) | -1084 |

All 13 lie *inside* the map's border ring (`outsideBorderRing=0`), which is the first hint at the
answer: they are authored geometry, not junk. Point 55 is #103's `x=48, y=-1` — the same map, trigger
and vertex, reached from a `.map` file rather than from a crash report — and it reads 140 bytes
*before* the shroud's source surface.

## 2. Which of (a) and (b): the conversion is right, the geometry is outside

The shroud grid covers the **playable** extent only. `W3DShroud::init` sizes it from the height map
with the border taken off, while world coordinates put the playable area's near corner at `(0,0)` and
the map's border ring at *negative* world coordinates —
`BaseHeightMap.cpp`'s `ADJUST_FROM_INDEX_TO_REAL(k) ((k - m_map->getBorderSizeInline()) * MAP_XY_FACTOR)`
is the same statement from the terrain's side. A vertex authored into the border ring therefore has no
cell, on any platform.

So the negative `y` is not a coordinate that arrived wrong. **Measured against the Windows build**, not
argued: `spikes/sim/tools/shroud-oracle-vc6.cpp` copies the engine's own float conversions out of
`BaseType.h` (`fast_float_trunc`/`fast_float_floor`, inline asm included), is compiled by the repo's
VC6 container and run for the same vertices. Its output, beside this port's:

| world (x, y) | VC6 `(Int)(w/cell)` | native `sim_probe rivers` | VC6 `REAL_TO_INT_FLOOR` | native `W3DShroud` (`shroudbounds`) |
|---|---|---|---|---|
| (1923, -79) | (48, -1) | (48, -1) | (48, -2) | (48, -2) |
| (1910, -149) | (47, -3) | (47, -3) | (47, -4) | (47, -4) |
| (2069, -158) | (51, -3) | (51, -3) | (51, -4) | (51, -4) |
| (-320, 1759) | (-8, 43) | (-8, 43) | (-9, 43) | (-9, 43) |
| (-1, -1) | (0, 0) | (0, 0) | (-1, -1) | (-1, -1) |
| (-40.5, 10) | (-1, 0) | (-1, 0) | (-2, 0) | (-2, 0) |
| (0, 0) / (100, 100) / (4719, 3199) | (0,0) / (2,2) / (117,79) | same | same | same |

Every cell agrees, digit for digit, on both conversions. So:

* the coordinate is authored: `-79` comes out of the `.map` file as an integer, through the engine's own
  chunk parser; no float conversion, signedness question or truncation reaches it before the divide;
* the conversion is not off on this port. VC6/x86 and clang/x86-64 produce the same cell for the
  shipped truncation *and* for the engine's fast floor — including the fast floor's own quirk, which
  the oracle also shows: an exact negative multiple goes one cell lower ((-320, 1759) → -9, not -8),
  because `fast_float_floor` subtracts 0.99999994 from any negative. That is Windows behaviour, so it
  is kept rather than corrected;
* what differs across platforms is only what the out-of-range *read* does. Windows reads 140 bytes
  before a heap allocation and gets whatever is there, silently; this port's allocator faults.

**Classification: port defect only in its symptom — a latent out-of-bounds read in shared game code
(present on Windows, unobservable there), plus authored map geometry that legitimately lies outside the
shroud grid. It is case (a).** Nothing here is synthetic, missing data, or an unimplemented path.

## 3. What the edge value should be, and the fix

Because it is (a), "add a guard and return something" would be a choice about *rendering*, so the value
has to come from the engine rather than from what avoids the fault. It does:
`W3DShroud::fillBorderShroudData` fills the unused source row and the whole border of the destination
shroud texture with `m_boderShroudLevel`, and the shroud pass samples that texture with clamp
addressing. **The level the shroud pass already shows outside the playable grid is
`m_boderShroudLevel`** — so that is what a river vertex out there is shaded with, and the water now
agrees with the terrain beside it instead of with an arbitrary cell.

Note what the shipped truncation would have done had it not faulted: `(Int)(-1.0f/40)` is `0`, so every
world position in the first border cell claimed cell `0` and was shaded with the *near corner of the
grid*. Flooring is what makes "outside" detectable at all, which is why the fix floors before it
tests.

The lookup, in both trees' `W3DShroud.cpp`:

```cpp
	if (x >= 0 && y >= 0 && x < m_numCellsX && y < m_numCellsY)
```

and a world-position accessor beside it, so the one caller holding world coordinates does not repeat
the conversion:

```cpp
W3DShroudLevel W3DShroud::getShroudLevelAtWorldPos(Real x, Real y)
{
	const Int cellX = REAL_TO_INT_FLOOR(x / m_cellWidth);
	const Int cellY = REAL_TO_INT_FLOOR(y / m_cellHeight);

	if (cellX < 0 || cellY < 0 || cellX >= m_numCellsX || cellY >= m_numCellsY)
		return m_boderShroudLevel;

	return getShroudLevel(cellX, cellY);
}
```

with `getRiverVertexDiffuse` in `Core/.../Water/W3DWater.cpp` calling it instead of converting itself.

**Windows behaviour.** Inside the grid nothing changes: the same cell, the same pixel, the same level.
Outside it, Windows previously read memory before the allocation — undefined behaviour whose value was
never a shroud level — and now reads the border level. There is no Windows behaviour to preserve at
that vertex, only a Windows crash-in-waiting removed.

**Determinism.** Everything touched is `GameClient`: `W3DShroud` is the render-side shroud texture and
`getRiverVertexDiffuse` computes a vertex diffuse colour. No `GameLogic` state, no
`PartitionManager` (whose own `worldToCell` is simulation state and is deliberately untouched), no
CRC input. The replay gate agrees — see §5.

## 4. The gate and the negative control

`scripts/ci/check-shroud-bounds.py` is two halves:

* **the measurement.** `sim_probe shroudbounds` mmaps a 118x80 shroud grid **between two `PROT_NONE`
  pages** and fills every valid cell with a pixel that reports level 255, then calls the engine's own
  `getShroudLevelAtWorldPos` for §1's measured vertices. An index outside the grid faults there on
  every platform instead of quietly reading a neighbour, so the check does not depend on an
  allocator's luck. Every out-of-grid vertex has to answer with the border level (60 in the harness,
  chosen distinct from anything the filled grid can report) and every in-grid vertex with 255.
  The probe is refused if it is older than `sim_probe.cpp` or either tree's `W3DShroud.cpp`, because
  that mistake was made here: with the bound reverted but the probe not rebuilt, the measurement half
  passed against the previous tree's binary.
* **the source check**, over both trees, because the native build compiles GeneralsMD only: the bound,
  the flooring conversion, the border return, and the water path's use of the world accessor rather
  than its own conversion.

The grid is handed to the shroud the way `init()` gets it — off a `LockRect` of the surface it holds —
so the shroud is in the state it is in during a frame, including the debug configuration's assertion
that it has a source surface at all. The surface is the only thing in the harness standing in for the
platform, and its pages are the point of the exercise. `shroudbounds` also opens no file and therefore
does not bring the archive file system up: `StdBIGFileSystem::init()` asserts on the empty install path
of a machine with no retail install, which is every CI runner, and this gate needs neither.

CI runs it in the **debug** configuration (`native-port-ci.yml`), where the engine's assertions are
live, so a fixture that lied about the shroud's state would be stopped by the engine rather than
measured.

Passing, with the fix:

```text
RESULT shroudbounds cellsX=118 cellsY=80 cellSize=40.00 borderLevel=60 gridLevel=255
RESULT shroudpoint world=(1923.00,-79.00) cell=(48,-2) inGrid=no level=60
RESULT shroudpoint world=(-320.00,1759.00) cell=(-9,43) inGrid=no level=60
RESULT shroudpoint world=(0.00,0.00) cell=(0,0) inGrid=yes level=255
RESULT shroudpoint world=(4719.00,3199.00) cell=(117,79) inGrid=yes level=255
RESULT shroudpoint world=(4720.00,3200.00) cell=(118,80) inGrid=no level=60
RESULT shroudbounds points=10 survived=yes
OK: every river vertex outside the shroud grid answers with the border shroud level, and neither
tree's bound or flooring conversion is missing
```

**Negative control.** With `GeneralsMD`'s implementation put back the way it shipped — upper bounds
only, truncating conversion, no border return — rebuilt and re-run (in both configurations):

```text
RESULT shroudbounds cellsX=118 cellsY=80 cellSize=40.00 borderLevel=60 gridLevel=255

FAIL: the shroud's out-of-grid lookup is not bounded
  - the probe exited -11 -- an out-of-grid vertex read outside the shroud grid, which is the crash
    this gate exists for
  - no result for world (1923, -79) -- MD_USA01 'Water Area 5' point 55, the vertex #103 crashed on
```

`SIGSEGV` on the first out-of-grid vertex, before any point is answered: the same read, the same
vertex, the same signal as the reported crash, and the gate fails.

The two bounds fail differently, which is why the gate has both halves. Removing only
`getShroudLevel`'s own bound leaves the accessor's, so nothing faults and the *source* half is what
fails — the latent read is back for any future caller that indexes cells directly, which is exactly
how this bug shipped. Removing the accessor's is what reaches the guard pages.

### In the game, on the map that crashed

The harness is the gate; the mission is the claim. MD_USA01 was driven from the shell with real mouse
input (SOLO PLAY → USA campaign → mission 1 → Easy) under `gdb`, three times: with the fix, with only
`GeneralsMD`'s `W3DShroud.cpp` reverted, and with the fix restored.

* **With the fix:** the mission runs — frame counter **5173 → 6043, +870 frames over ~127 s** — the
  process is alive afterwards and no `SIGSEGV`, `Segmentation` or abort appears on stdout/stderr. The
  crashing path really executes: a breakpoint on `getShroudLevelAtWorldPos` is hit with **world
  (1923, -79)**, the vertex above, under `getRiverVertexDiffuse` → `drawRiverWater` → `renderWater`
  → `Render`, and `finish` returns that instance's live `m_boderShroudLevel` (0 there; the field is
  game state, so the value is read from the instance rather than assumed).
* **Reverted, rebuilt, same drive:** `SIGSEGV` in
  `W3DShroud::getShroudLevel(x=48, y=-1)` at `W3DShroud.cpp:272`, through the identical chain, with
  `srcData=0x55555e306000 pitch=236` — the read one row before the allocation. The reported crash,
  on the reported vertex, on demand.

Two traps worth naming, both of which produced a wrong answer before they were noticed:
`scripts/native-build.py` **does not relink `build/native/native_strict_link` without `--strict-link`**,
so a reverted tree otherwise keeps running the old binary; and the gate's probe must be rebuilt, which
is now the gate's own business (above) rather than the operator's. Also note `getShroudLevel` is
inlined into `getShroudLevelAtWorldPos` in the release build, so a breakpoint belongs on the caller.

One caveat on what was *seen*: the 3D terrain and water present as a blank view in this port (a
pre-existing renderer limitation of the concurrent renderer slice; HUD, objectives and minimap draw).
So "the river is on screen" is established by the debugger hitting the river polygon's own authored
vertices, not by water pixels.

## 5. Verification

Run on this branch, on x86-64 Linux, unless stated otherwise:

| gate | result |
|---|---|
| `./scripts/docker-build.sh` (Wine/VC6, **both** trees) | green — `generalszh.exe` 6361156 bytes and `generalsv.exe` 6012993 bytes, so VC6 accepts the change in `Generals` as well as `GeneralsMD` |
| `scripts/native-build.py --level 1..4 --with-shims --strict-link` | 980/980 objects, 0 failures, 0 unresolved symbols; unchanged from the baseline (no translation unit added or removed, so no `ci-baselines/*.json` is regenerated) |
| `scripts/ci/check-shroud-bounds.py --build-dir build/native` | OK (§4), and FAIL with the shipped lookup restored |
| MD_USA01 driven in-game under gdb, with and without the fix | +870 frames over ~127 s and the border level returned at world (1923, -79); `SIGSEGV` at `getShroudLevel(48, -1)` once reverted (§4) |
| `scripts/porting-status.py --check` | `OK: docs/porting/STATUS.md matches the baselines` |
| `check-d3d8-surface.py`, `check-backend-coverage.py`, `d3d8-lock-scan.py --check`, `surface-lock-audit.py --check` | all four OK — 100/100 lock sites classified, allowlist and coverage baseline untouched (this slice adds no D3D8 call and no lock) |
| `python3 -m flake8 scripts/`, `classify-changes.py --self-check`, `actionlint` | clean; 15/15 classification cases |
| retail replay gate | **not run here, by construction**: the authoritative gate is `check-replays.yml` on `windows-2022`, and per [replay-check-gamedata.md](replay-check-gamedata.md) a Wine result is differential only and never quotable as the gate. It runs on the PR. |

On determinism, the argument the replay gate then confirms rather than establishes: the three
translation units this changes are `W3DShroud.cpp` (both trees) and `W3DWater.cpp`, all
`GameEngineDevice` render code; no `GameLogic` TU, no `PartitionManager`, no CRC or replay input is
touched, and inside the grid the returned level is bit-identical to before.

The in-game drive and its negative control are in §4.

## 6. What remains open

* **In-game selection semantics** (`real-input-menu-drive.md` §4.5) — untouched by this slice, and
  still not classified. A posted drag draws a box while `selected_count` stays 1 and
  `frame_selection_changed` stays 0, which remains consistent with no selectable object under the box,
  an already-selected object, or an input defect. This slice deliberately does not upgrade any of
  those to a classification.
* **The other 45 maps** with water-area vertices outside the grid are now shaded with the border level
  rather than faulting, but only MD_USA01 was driven in the game.
* The shipped truncation is still present anywhere else a caller converts world coordinates to shroud
  cells by hand; the water path was the only such caller found.
