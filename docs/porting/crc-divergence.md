# Why the native simulation diverges from the recorded Windows run

The headless native build ticks a retail replay to the end and is deterministic against itself, but
**none** of its 134 checkpoint CRCs equal the recorded ones. This report says *where* that starts, *what
state* differs, and *which mechanism* produces each difference. It fixes one cause and leaves the rest
as measured findings.

Cross-platform CRC equality is **not a shipping requirement** — retail replay/save compatibility is out
of scope. Its value is as an **oracle**: it is the only mechanism that can say the native simulation
computes the same game as the known-good Windows build, and the previous wave showed how badly that
oracle is needed (13,500 "successful" frames of an empty map).

## 0. What was measured, and with what

| Thing | Value |
|---|---|
| Commit | this branch, off `main` @ `8c6dfaab0` (after #96/#98/#99/#100) |
| Host | Ubuntu 22.04, x86-64 Linux — **not** Apple Silicon (see §6) |
| Native binary | `build/native/native_strict_link`, ELF 64-bit x86-64, 979/979 objects, 0 unresolved |
| Windows binary | `./scripts/docker-build.sh` VC6 build, `RTS_BUILD_OPTION_DEBUG=OFF`, run under Wine |
| Data | `zerohour104_gamedata_full.7z` + `generals108_gamedata_trimmed.7z`, extracted outside the repo |
| Replay | `GeneralsReplays/GeneralsZH/1.04/Replays/366648.rep`, `[RANK] Arctic Arena ZH v1`, 7:30 |
| Instrument | `CRCDiag` (this PR): opt-in, env-gated, does not touch the production CRC |

`CRCDiag` is off unless `CNC_CRC_DIAG` names a log file. When on it records, per frame: every
`xferUser` blob with its bytes, every object's running CRC with its template name, each subsystem
marker, the frame total, and every replay-vs-local checkpoint comparison. `CNC_CRC_DIAG_CONTINUE=1`
additionally keeps a mismatching playback running so a whole run can be compared instead of only its
first differing checkpoint. Neither the CRC algorithm, the checkpoint interval, nor any recorded
baseline was changed; the markers were already in the production stream as `MARKER:` strings.

## 1. Where it diverges: initialisation, not simulation

**Checkpoint 1 already differs.** The first comparison the replay makes is at frame 110, and it differs:

```
C 110 LOCAL C6FA1978 REPLAY 659E2F68 MISMATCH      <- native
C 110 LOCAL 659E2F68 REPLAY 659E2F68 MATCH         <- Windows, same replay
```

All 134 native checkpoints differ. So this is not accumulated simulation drift: the world is already
different before the first checkpoint. Bisecting further, **frame 0 differs** — that is, the state
straight out of map load, before any logic runs:

| Frame | Native total | Windows total |
|---|---|---|
| 0 | `91C82211` | `75994115` |
| 100 | `C6FA1978` | `659E2F68` |

## 2. Where in *state*: 13 of 440,035 raw-byte records at frame 0

Frame 0 checksums 440,035 `xferUser` blobs. Comparing them record-by-record between the two runs:

| Run | Differing frame-0 records |
|---|---|
| native, before the fix in §3.1 | **132** of 440,035 |
| native, after it | **13** of 440,035 |
| Windows, before vs after the fix | **0** — byte-identical, 440,035/440,035 |

Structure is not the problem, and this is worth stating precisely because it eliminates several
candidates at once: at frames 0 and 100 the two runs emit the **same number of objects (221, then 222),
in the same order, with the same object IDs and the same template names**, and the same subsystem
marker sequence (`Objects`, `ThePartitionManager`, `TheModuleFactory`, `ThePlayerList`, `TheAI`,
`TAiData`, `AIGroup`, `GameSave`). Every differing record is a 48-byte `Matrix3D` inside `Object::crc()`,
and every difference is one of: a differently signed zero, a 1-ULP float, or `1.0f` against
`0.99999994f`.

The divergence also does **not** amplify over the measured window — it stays at a handful of ULPs:

| Frame | Differing raw-byte records |
|---|---|
| 0 | 13 of 440,035 |
| 100 | 14 of 12,731 |
| 1000 | 16 of 12,890 |

That is the shape of the whole problem. The native build is playing the same game to within a few ULPs,
but a whole-world CRC is all-or-nothing: one differing byte anywhere makes every checkpoint mismatch.

## 3. The mechanisms, ranked, with how each was tested

Contribution is attributed by **how many differing frame-0 raw-byte records a mechanism accounts for**,
plus, where a mechanism is not visible at frame 0, a direct A/B of the same expression on both
compilers. That metric is honest about what it is: a count of differing serialised records at one
instant, not a claim about long-run influence.

### 3.1 CONFIRMED — MSVC folds the constant multiplies out of `setOrientation`'s cross products (119/132 records; **fixed here**)

`Thing::setOrientation()` built the rotation by crossing the heading with the constant Z axis. Those
cross products only ever multiply by `0.0f` and `1.0f`. VC6 folds them; clang, which may not assume
`0.0f * x` is `+0.0f`, keeps them — so the two builds wrote **differently signed zeroes** into a matrix
that `Object::crc()` checksums raw (`xfer->xferUser((Matrix3D *)getTransformMatrix(), sizeof(Matrix3D))`).

Tested: compiled the exact expression under both compilers over 20,001 angles (`-3.15..3.15`) and
diffed all nine matrix elements as bit patterns; the zero elements disagree, and disagree only at
axis-aligned angles — which is where most map objects sit. Then measured in the engine: writing the
rotation out directly took frame 0 from 132 differing records to 13, and left the Windows run
byte-identical (§2), including all 31 of its matching checkpoints.

This is the fix in this PR. It is a spelling change in the existing style, not a behaviour change:
Windows output is unchanged, measured, not argued.

### 3.2 CONFIRMED — excess intermediate precision on the Windows side (10 of the remaining 13 records; the dominant cause for the rest of the run)

MSVC/VC6 evaluates `float` expressions on the x87 stack with the CRT's 53-bit precision control, i.e.
at **double** precision, and only rounds to `float` on store. clang on x86-64 (SSE) and on arm64 (NEON)
evaluates them at single precision. Every multi-operation float expression in the engine is therefore
allowed to differ.

Tested two ways.

1. In the engine: the 10 remaining terrain-aligned objects differ exactly as this predicts — Windows
   stores a normalised axis of `0.99999994f` (`FF FF 7F 3F`) where native stores `1.0f`, from
   `Coord3D::normalize()`/`makeAlignToNormalMatrix()` off `TerrainLogic::alignOnTerrain()`.
2. Standalone A/B of `GetGameLogicRandomValueReal()`'s arithmetic — `((Real)v * theMultFactor) * delta
   + lo` — over 1,500 draws, VC6 vs clang:

   | Native evaluation | Records differing from VC6 |
   |---|---|
   | `float` (as written) | **833 of 1,500** |
   | `double` intermediates | **0 of 1,500** |
   | `long double` intermediates | 0 of 1,500 |

   So VC6's answer *is* the double-precision answer, for every sample. 55% of random real draws differ
   natively. Nothing in the replay's first 110 frames drew one — but over a 7:30 game this dwarfs
   everything else in §3.

Not fixed here, and it is not a one-line fix: it needs either the checksummed float expressions
deliberately spelled with `double` intermediates, or a decision to stop requiring cross-platform
bit-exactness. See §5.

### 3.3 CONFIRMED — transcendental functions differ between VC6's CRT and libm (1 of the remaining 13 records)

`Sin`/`Cos`/`Tan`/`ACos`/`ASin` are `sinf`/`cosf`/... and the two libraries are not bit-identical.

Tested: same-input sweeps of 20,001 values under both compilers, compared as bit patterns.

| Function | Inputs differing |
|---|---|
| `cosf` | 268 of 20,001 (1.34%) |
| `sinf` | 255 of 20,001 (1.27%) |

One frame-0 record is exactly this: a rotation element off by 1 ULP (`C5 F5 65 BE` vs `C4 F5 65 BE`)
for an object at a non-axis-aligned angle. Unfixable by flags; it needs the engine to carry its own
implementations if bit-exactness is required.

### 3.4 CONFIRMED by codegen, unmeasurable on this host — FMA contraction (mitigated here)

clang contracts `a*b+c` into one fused multiply-add wherever the target has one. Tested by compiling
`float f(float a,float b,float c){return a*b+c;}` for both targets:

| Target | default | `-ffp-contract=off` |
|---|---|---|
| `aarch64-linux-gnu` | `fmadd` | `fmul` + `fadd` |
| `x86_64-linux-gnu` | `mulss` + `addss` | `mulss` + `addss` |

So on the actual Apple Silicon target the engine's float sums would be computed differently from both
x86-64 native and Windows, silently. `cmake/native/CMakeLists.txt` now sets `-ffp-contract=off`. On
this x86-64 host the flag is a **no-op** (identical codegen), so it cannot be credited with any of the
measured records; it is a correctness measure for the arm64 target, and confirming it there needs an
arm64 run (§6).

### 3.5 EXCLUDED — the PRNG's integer sequence

Tested: replicated `seedRandom()`/`randomValue()` standalone, VC6 vs clang, three seeds × 500 draws =
1,500 values, compared as hex. **Bit-identical, all 1,500.** The generator is the engine's own
add-with-carry code on `UnsignedInt`, not libc `rand()`; `InitRandom()` is seeded from the replay, not
from `time()`, on this path. Only its *float conversion* differs, which is §3.2, not the generator.

libc `rand()` does exist in the tree (`wwmath.cpp`, `aabtreecull.cpp`, `quat.cpp`, `Snow.cpp`) and its
implementation certainly differs, but none of those call sites is on the logic path being checksummed
here — that is excluded by call-site inspection only, so treat it as unmeasured for anything else.

### 3.6 EXCLUDED — struct layout, padding and blob width

Tested: `scripts/native-layout-test.py` (`CLANGXX=clang++-14`) and `scripts/xfer-blob-audit.py` both
pass, and the engine-level evidence is stronger than either — a layout or padding difference in a
48-byte `Matrix3D` or in any other checksummed blob would change the *record lengths* or shift bytes
wholesale. All 440,035 frame-0 records have identical lengths and identical bytes but for 13 records
differing in one to three float elements.

### 3.7 EXCLUDED — container iteration order and allocator-address dependence

Tested: the object-level diagnostics. If iteration order or an allocator address leaked into the state,
the object stream would reorder. Native and Windows emit the same 221 objects at frame 0 and the same
222 at frame 100, in the same order, with the same IDs and templates, and the same marker sequence.

### 3.8 UNMEASURED — uninitialised reads

Attempted with Valgrind against the native binary; it aborts before running the game with
`debuginfo reader: Possibly corrupted debuginfo file. I can't recover.` MemorySanitizer is not an
option either — it needs every dependency instrumented, and this binary links prebuilt FFmpeg, OpenAL
and zlib. So this remains a genuine possibility, and it is *consistent* with per-platform
self-consistency. What it would take: a Valgrind run against a binary built with `-gline-tables-only`,
or an MSan build of the simulation-only subset (no audio/video deps) — a slice of its own.

Weak counter-evidence, not a measurement: an uninitialised read would have to land in one of the 13
differing records and nowhere else, and those 13 are all explained by §3.1–3.3.

### 3.9 UNMEASURED — locale-dependent parsing

Tested as far as this environment allows: ran the native replay under `C` and under `C.utf8`; all 134
checkpoints identical. No other locale could be generated (`localedef` has no charmaps installed:
`character map file 'UTF-8' not found`), so a comma-decimal locale — the classic `atof()` failure — was
**not** tested. Partial counter-evidence: INI-derived data agrees, since both runs produce identical
template names and object counts.

## 4. A finding about the oracle itself: it is only valid for the first 3,111 frames

Running the same replay through the **Windows** build, the recorded CRCs match for 31 checkpoints and
then stop:

```
C 3111 LOCAL 0A8ED60F REPLAY 0A8ED60F MATCH
C 3211 LOCAL 04D05717 REPLAY BC803967 MISMATCH
```

This is not caused by anything in this PR: an unmodified `main` build, without the diagnostics,
mismatches at the same frame. It also moves with the *data* staged (frame 3211 with the full archive,
3611 with the trimmed one), which points at data or Wine, not at the engine change.

Consequences, stated plainly:

- The recording is a valid oracle for frames 0–3111 only. Beyond that, "native differs from the
  recording" says nothing, because Windows differs from it too.
- Comparing native against a **Windows run** (as §1–§3 do) is the sound comparison, and it does not
  inherit this limit.
- **The retail replay gate could not be run as a verdict in this environment.** Under Wine, with the
  CI's own trimmed archives, registry key and layout, unmodified `main` fails all 10 replays. So the
  gate's local exit code measures the environment, not the change. What was done instead: the same
  Windows build pre- and post-change, on the same data, is byte-identical at frames 0, 100 and 1000
  (440,035 records at frame 0), produces identical CRCs at all 134 checkpoints, and diverges from the
  recording at the identical frame. CI's `check-replays.yml` on real Windows remains the verdict.

## 5. Is cross-platform lock-step reachable? A recommendation, not a decision

**Reachable in principle, but it is a project, not a fix** — and the user should decide whether to buy
it, because it is not needed for the stated scope (single-player campaign and skirmish; retail
replay/save compatibility already out of scope).

What it would take, given §3: every checksummed float expression spelled to evaluate at double
precision to match x87 (§3.2), the engine carrying its own `sinf`/`cosf`/`sqrtf` (§3.3), and each new
divergence found by exactly the bisect-in-time/bisect-in-state loop used here. That is broad,
invasive, touches Windows code paths, and each step must keep the Windows build bit-identical.

The recommendation is to **stop at oracle-grade agreement, not bit-exactness**: keep the
`CRCDiag`-based native-vs-Windows comparison as the wave's acceptance instrument, require that the
divergence stays within a bounded handful of ULP-level records and that structure (object count, IDs,
order, templates) matches exactly, and fix the cheap bit-exact causes as they are found (§3.1 is one).
Pursuing full lock-step is only worth it if cross-platform multiplayer or shared replays ever come back
into scope — at which point §3.2 is the thing to schedule first.

## 6. What could not be measured here

- **Apple Silicon.** Everything above is x86-64 Linux. arm64 has FMA by default (§3.4) and a different
  libm, so its divergence set is a superset of this one and its measurement is still outstanding.
- **Uninitialised reads** (§3.8) and **non-C locales** (§3.9).
- **The native shutdown crash.** The headless run reaches the end of the replay, emits all 134
  checkpoints, and then exits 139 in `ObjectPoolClass<MultiListNodeClass,256>::~ObjectPoolClass`
  (`mempool.h:208`) during teardown. It is after the last checkpoint and does not affect any number
  here, but it is a real defect and needs its own slice.
