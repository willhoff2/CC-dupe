# Port review — what is verified, what is wrong, and what needs a human decision

A skeptical review of the port work so far (PRs #1–#4 and everything under `docs/porting/`,
`scripts/`, `spikes/renderer/`, `Core/Libraries/Source/WWVegas/WWLib/platform/`,
`Core/Libraries/Source/OpenALAudioDevice/`). Every number below was re-measured from the source,
not copied from the existing documents. Where I could not measure something, it says so.

Measurement environment: Ubuntu 22.04, clang 14.0.0, x86-64, 8 cores. Tree measured is
`682b413d9` (the merge of PRs #1–#4) unless stated otherwise, with the pinned vendor headers
(dx8, gamespy, miles, lzhl) present in `build/docker/_deps`.

---

## 0. Read this first: the premise that the work is "merged to main" is false

`origin/main` contains **PR #1 only**. PRs #2, #3 and #4 were opened with
`devin/1785605665-native-port-probe` as their base, not `main`, and were merged into that branch:

| PR | base | merged |
|---|---|---|
| #1 native-port probe | `main` | yes → `1011d40b0`, on `main` |
| #2 D3D8 surface + Vulkan spike | `devin/1785605665-native-port-probe` | yes → `02521ead9`, **not on `main`** |
| #3 GameEngine probe + OpenAL | `devin/1785605665-native-port-probe` | yes → `691dd1e4d`, **not on `main`** |
| #4 WWLib platform layer | `devin/1785605665-native-port-probe` | yes → `682b413d9`, **not on `main`** |

`main` is 19 commits ahead of the stack and the stack is 6 commits ahead of `main`. So none of the
renderer spike, the OpenAL backend, the GameEngine probe or the `WWLib/platform/` layer exists on
`main` today. Anyone reading `main` sees a plan document whose corrections are on a branch.

Measured, not inferred: a test merge of `682b413d9` into current `origin/main` produces **zero
textual conflicts**. But `main` has since taken upstream changes that touch the same areas
(`#3032` restored relative include visibility across WWVegas, `#3008` split `CommandXlat`), so the
probe numbers must be re-run after the rebase before they can be quoted against `main`.

**This is slice 1. Nothing else should be quoted as the state of the project until it lands.**

---

## 1. Verification of the claims

### 1.1 D3D8 surface — the claims hold

| Claim | Verdict | My measurement |
|---|---|---|
| 62 distinct D3D8 methods | **correct** | 62 (52 `IDirect3DDevice8` + 10 `IDirect3D8`) |
| 458 total call sites | **correct** | 458 by the repo's scanner; 460 by an independent scan I wrote |
| 82 behind `DX8CALL`/`_HRES`/`_D3D` | **correct** | 82 |
| 376 sites bypass `DX8Wrapper` | **correct** | 376 (378 by my scan, the 2 extra are `Release()` calls) |
| 13,517 LOC across 21 `dx8*` files | **correct** | 13,517 / 21 |

Reproduced with `python3 spikes/renderer/tools/d3d8-surface-scan.py` and again with an
independently written regex scan that strips comments and counts per line. Agreement to within
0.5% on a regex-based method is as good as this technique gets. The concentration claim also holds:
`W3DWater.cpp` 129, `W3DShaderManager.cpp` 103, `W3DVolumetricShadow.cpp` 38,
`W3DProjectedShadow.cpp` 25 direct sites.

Two caveats the renderer doc already states and I confirm: the scan is a **lower bound** (it misses
calls split across lines and any device pointer alias it does not know about — I checked for other
aliases and found only `device` in `W3DProfilerFrameCapture.cpp`, already covered), and it counts
*sites*, not *semantics*. 62 methods is not 62 units of work: `SetRenderState` alone is ~100 D3D
render states, each needing its own Vulkan mapping.

**Verdict: the D3D8 numbers are the most trustworthy figures in the whole document set.**

### 1.2 Probe counts — two of three correct, the headline is stale, and the number does not mean what it says

| Claim | Verdict | My measurement |
|---|---|---|
| 0/586 GameEngine TUs compile natively | **correct** | 0/207 Core + 0/379 GeneralsMD = 0/586 |
| 514/586 GameEngine TUs compile shimmed | **correct** | 169/207 + 345/379 = 514/586 |
| 114/151 clean in `Core/Libraries` | **off by one, and input-dependent** | **115/151** with vendor headers present; 114/151 without them |
| "106 of 147 (72%)" (plan doc, Phase 0) | **stale** | superseded; the plan was never updated after PR #3/#4 grew the denominator |
| "107/733" (probe report) | **stale** | now 115/737 |

The 114-vs-115 difference is `Core/Libraries/Source/Compression`, which needs `lzhl.h` from
`build/docker/_deps`. So the headline probe number silently depends on whether an untracked,
un-versioned dependency directory exists on the machine that ran it. That is a reproducibility
defect, not a rounding error: the report claims vendor headers are used, and the quoted figure was
produced without them.

**The much bigger problem is what the 86% shimmed figure measures.** Three things make it not mean
"86% of the engine is portable":

1. **It excludes the entire renderer and device layer.** The probe covers 737 of the ~991 in-scope
   `.cpp` files. The 238 it does not cover are `WW3D2` (73), `Core/GameEngineDevice` (70),
   `GeneralsMD/Code/GameEngineDevice` (40), `GeneralsMD/.../WWVegas` (35), `WWAudio` (19) and
   `Main` (1) — i.e. precisely the D3D8- and Win32-dense code. **No file in the renderer or device
   layer has ever been compiled off Windows in this project.**
2. **It is compiled in a type environment where the 32-bit types are 64 bits.** See §1.4. The
   shims' README advertises "64-bit-correct handle types … exactly as in the real LLP64 Win32
   headers", but `DWORD`, `LONG` and `ULONG` are `unsigned long`/`long`, which are 64-bit on
   LP64 hosts. LLP64 keeps them at 32.
3. **`-fsyntax-only` cannot see the class of bug that Phase 2 exists to fix.** I proved this: I
   patched both the shim and the engine's own `bittype.h` so `DWORD`/`ULONG`/`uint32`/`sint32` are
   genuinely 32-bit and re-ran the shimmed probe. Result: **636/737 clean instead of 637/737.**
   Making the fundamental integer widths correct changes the probe's answer by one translation
   unit, while changing `sizeof(ChunkHeader)` — the header of every `.w3d` asset — from 16 bytes
   back to 8. The probe is blind to the thing that will actually stop the game from starting.

**Verdict: the counts are honest; the interpretation printed next to them is not.** "86% of the
engine's own C++ compiles as 64-bit C++" is true and worth knowing. "The engine is 86% portable"
does not follow and should be struck from the plan.

### 1.3 Miles surface — the claim is inflated by comments, and conflates two different sets

| Claim | Verdict | My measurement |
|---|---|---|
| 114 distinct `AIL_*` identifiers | **wrong** — counts comments | **105** with comments stripped (114 if you count them) |
| 101 of them are functions | **wrong** | **94** distinct `AIL_*` functions are actually called |
| 13 constants/macros | **wrong** | **11** |
| 230 reference sites | **wrong** | **197** function call sites + 20 constant references = **217** |
| 10 source files | **correct** | 10 |

The 114/230 figures come from a scan that did **not** strip comments, unlike the D3D8 scan in the
same PR series. Nine of the identifiers exist only inside comments — e.g.
`AIL_set_sample_volume` and `AIL_sample_pan`, which appear solely in an upstream fix note reading
"upgrades miles call from legacy `AIL_set_sample_volume`". Counting a deprecated API named in a
comment as part of the surface to be reimplemented is exactly the error the doc set was written to
avoid.

"101 functions" is also the wrong *set*: 101 is the number of `AIL_*` functions **declared in the
replacement `mss.h`** (verified: 101 declared, 101 defined, `nm` on the built objects agrees). The
engine calls 94 of them; 3 more are reached through compatibility macros
(`AIL_3D_object_user_data` → `AIL_3D_user_data`, and 2 similar); 10 declared entry points are never
called by in-scope code.

This does not change the doc's conclusion — the plan's original "roughly ten call sites" is still
wrong by an order of magnitude, and 94 functions is still a real subsystem, not a shim. It changes
the credibility of the numbers, and the estimate derived from them is ~15% high on call-site count.

**Independently verified as correct:** the OpenAL backend compiles clean under
`clang++ -std=c++17 -m64 -Wall -Wextra` (5 TUs, **zero warnings**), and it is interface-complete —
every declared `AIL_*` entry point has a definition.

**Independently verified as unproven:** nothing in it has produced a sound. There is no test, no CI
job builds it (CI has Windows presets only), and `cmake/openal.cmake` is only reachable from a
non-Windows configure that cannot currently complete, because the non-Windows branch of the top
level `CMakeLists.txt` also skips `dx8.cmake` and `bink.cmake`, which the renderer and video code
still need. So the OpenAL backend has never been configured, linked or run as part of this project.

### 1.4 A defect nobody has recorded: the engine's own 32-bit types are 64 bits off Windows

`Core/Libraries/Source/WWVegas/WWLib/bittype.h`:

```cpp
typedef unsigned long   uint32;   // 8 bytes on LP64
typedef signed long     sint32;   // 8 bytes on LP64
typedef unsigned long   DWORD;    // 8 bytes on LP64
typedef unsigned long   ULONG;
```

Measured on this machine: `sizeof(uint32) == 8`, and a struct with the layout of
`ChunkIO`'s `ChunkHeader { uint32 ChunkType; uint32 ChunkSize; }` is **16 bytes, not 8**. That
struct is read straight off disk:

```cpp
// Core/Libraries/Source/WWVegas/WWLib/chunkio.cpp:425
if (File->Read(&HeaderStack[StackIndex], sizeof(ChunkHeader)) != sizeof(ChunkHeader)) {
```

`uint32` appears 637 times in in-scope code, and `w3d_file.h` — the on-disk format of every model,
mesh, animation and hierarchy in the game — uses it 191 times across its structures. Until
`bittype.h` is fixed, a native build cannot load a single `.w3d` asset. The probe reports every one
of those translation units as clean.

This is cheap to fix (change four typedefs, then chase the fallout) but it must be sequenced
*before* anyone tries to bring up the renderer, or they will debug the renderer against garbage
geometry. Related, in the same header family: `WWMath::Is_Valid_Double` does
`(unsigned long *)(&x) + 1` to reach the high word of a `double` — on LP64 that pointer arithmetic
advances 8 bytes and reads **past the end of the object**.

### 1.5 Other claims I checked

| Claim | Verdict |
|---|---|
| Vulkan spike renders a textured triangle, 0 validation errors, 2 pipelines for 2 draws | **correct** — I built and ran it: `device: llvmpipe (LLVM 15.0.7), 2 VkPipeline(s) created for 2 draws, validation messages: 0, OK` |
| Spike "was written to be MoltenVK-compatible" | **wrong in a specific, fixable way** — see §2.1 |
| "~20 `HWND` files and 49 `windows.h` files in scope" | **close** — I measure 21 and 50 |
| "`GeneralsMD/Code/GameEngine`: 1 file includes `windows.h`, 1 references `HWND`" | **true but misleading** — the one file is `PreRTS.h`, force-included by all 382 TUs |
| "~30 Win32 headers pulled in by `PreRTS.h`" | **overstated** — 21 Win32-specific headers (plus `atlbase.h` and `dinput.h`) |
| "19 inline x86 `__asm` blocks" | **undercount** — 62 `__asm` statements in 17 in-scope files (19 files if comments are counted). But roughly half are already behind `_MSC_VER && _M_IX86` guards with C++ fallbacks, so the *work* is smaller than 62 and larger than 19 |
| "16 `.nvp`/`.nvv` shader sources, 158 non-comment lines" | **close but low** — 16 files in the ZH+Core paths, 117 non-comment lines; 24 files / ~1,040 lines if `Generals/` is included |
| The plan's total, "~3,100–6,100 h" | **internally inconsistent** — the plan's own table sums to 3,150–6,150, the prose says 3,100–6,100, and `next-slice-scope.md` revises the same total to 2,800–5,600 from a "3,000–6,000" baseline that appears nowhere else |
| The shims "do not link and are a measurement instrument" | **correct, and the docs are commendably clear about it** |
| `WWLib/platform/` layer | **real code, but partial** — 33 call sites in `thread.cpp`/`mutex.cpp`/`registry.cpp`, all behind `#ifndef _WIN32`. It does **not** cover `GeneralsMD/Code/GameEngine/Source/Common/System/registry.cpp`, a second, entirely separate registry implementation calling `RegOpenKeyEx` directly, which is the one the game actually uses to find its user data directory |

---

## 2. Decisions that need a human

### 2.1 Renderer backend

**Options.**

| Option | For | Against |
|---|---|---|
| **A. Hand-written Vulkan behind `DX8Wrapper`** (current recommendation) | Spike proves the two architectural risks (immutable pipelines, `SetTextureStageState`) are solvable; one backend serves macOS + Linux; full control over the 100+ render states | Every one of 62 methods and ~100 render states is hand-written; ~1,000–1,700 h; fixed-function lighting/fog/material emulation is uncosted work that the spike does not do |
| **B. DXVK / d3d8to9 translation layer** | Enormously more mature; DXVK ships a `d3d8` frontend today; someone else maintains the state translation | DXVK's D3D8/9 frontend is Windows-COM-shaped and expects a Windows-ish environment; you inherit a large dependency you cannot debug; macOS support is via MoltenVK anyway, so it does not remove the MoltenVK risk — it adds a layer on top of it |
| **C. SDL3 GPU API** | Portable across Vulkan/Metal/D3D12 with no MoltenVK dependency at all; SDL3 is likely wanted for windowing/input regardless | Youngest option, smallest track record; you still write the whole D3D8 emulation, just against a different target; SDL3's GPU API has no fixed-function anything either |
| **D. Native Metal backend** | Best macOS behaviour and tooling | Abandons Linux; two backends to maintain; largest total work |

**My recommendation: A, but do not start it yet, and spend a day on B and C first.**

Reasoning. The spike is genuine evidence for A and I re-ran it. But two things weaken the
recommendation as written:

- The doc says the spike "was written to be MoltenVK-compatible (Vulkan 1.1 core only, no
  extensions beyond swapchain)". That is the wrong compatibility criterion. MoltenVK is a
  *portability driver*: with a current Khronos loader, `vkEnumeratePhysicalDevices` will not report
  it unless the instance is created with `VK_KHR_portability_enumeration` and
  `VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR`, and `vkCreateDevice` must enable
  `VK_KHR_portability_subset` when the driver advertises it. `spikes/renderer/src/vulkan_backend.cpp`
  does neither (grep: no occurrence of "portability"). As written the spike will most likely fail
  at device enumeration on macOS. This is a ~20-line fix, but it means the "and therefore MoltenVK"
  step has not merely been unverified — the code contains a concrete, known obstacle to it. *I have
  no macOS machine here; this is inference from the Vulkan portability specification, not a
  measurement. It is cheap to settle and should be settled first.*
- Option B was dismissed without a spike. DXVK's D3D8 frontend covers a much larger share of the
  62 methods than a from-scratch backend will in its first year. The right way to close this is not
  argument but a half-day experiment: build DXVK's `d3d8` on macOS/MoltenVK and see how far it gets.

Whichever is chosen, one thing is unconditional and should start immediately: **route the 376
direct `IDirect3DDevice8` call sites through `DX8Wrapper` while still on D3D8 and Windows.** It is
mechanical, incrementally verifiable against a green Windows build, and it is required by A, B and C
alike. It is the only renderer work that is free of the backend decision.

### 2.2 `PreRTS.h` and the Win32 boundary

**Options.** (a) A real Win32 compatibility layer that links. (b) Surgery: strip the Win32 include
block out of `PreRTS.h` and let the ~50 files that genuinely need Win32 include it themselves.
(c) Per-subsystem abstraction with no Win32 vocabulary at all.

**Recommendation: (b) then (c). Do not build (a).**

The measurement supports this and the docs already half-say it. Only 21 in-scope files reference
`HWND` and 50 include `windows.h`; the reason all 382 GameEngine TUs are Win32-dependent is one
forced include. Surgery converts a 382-file problem into a ~50-file problem, is testable
continuously against the Windows build, and does not require deciding the eventual abstraction.
Building a real Win32 compat layer (a) means implementing Win32 semantics you will then delete —
and the 100 TUs that still fail *with* the shims are dominated by things you would never
reimplement (`DbgHelp` stack walking, `AllocConsole`, named pipes in the bundled `debug`/`profile`
test programs, `CreatePipe` in `WorkerProcess.cpp`). Those want deleting or `#ifdef`-ing, not
porting.

The current `scripts/native-port-shims/` should **not** grow into the platform layer, and the
shims' own README says so. Worth reinforcing: it should be deleted once the probe is retired, or it
will be mistaken for one.

### 2.3 Save/load and serialisation

Better news than the plan implies. `Xfer` is already a typed interface (`xferInt`, `xferReal`,
`xferUnsignedShort`, …) over `BaseTypeCore.h` typedefs that are already fixed-width
(`typedef uint32_t UnsignedInt`). The exposure is narrower than "rewrite the serialisation":

- **261 `xferUser(&x, sizeof(x))` raw-blob sites** in game code. Most are enums and `Matrix3D`
  (fine); each still needs an eyeball. This is the bulk of the work and it is auditing, not
  rewriting.
- **`WideChar` is `wchar_t` = 4 bytes** on Linux/macOS vs 2 on Windows.
  `xferUnicodeString` writes `sizeof(WideChar) * len`, so every save containing a unicode string
  changes size and layout. This is also the direct cause of the 14 `static_assert` failures on
  `sizeof(LANMessage) <= 476` that the probe reports.
- **The `.w3d`/chunk binary readers** (§1.4) are a *separate* and more urgent problem than saves.

**Recommendation:** typedef `WideChar` to `char16_t` (not `wchar_t`) engine-wide, fix `bittype.h`
to fixed-width types, then audit the 261 blob sites. Do **not** design a new save format; retail
save compatibility is already dropped, and a format change buys nothing that fixing the widths does
not. Add a `static_assert` on the size of every struct that is read or written as a blob — that is
the regression test this codebase is missing and it costs almost nothing.

### 2.4 Upstream: stay mergeable, or hard-fork

Measured: current `main` is 19 commits ahead of the port stack, of which most are upstream
refactors, and the test merge is conflict-free today. So mergeability is currently cheap — but the
port has not yet made any of the changes that break it. Fixing `bittype.h`, changing `WideChar`,
and restructuring `PreRTS.h` all conflict directly with upstream's 1.04 replay-parity goal (upstream
CI literally runs a replay-compatibility check, `check-replays.yml`).

**Recommendation: stay a rebasing fork with a one-way flow — take from upstream, do not try to give
back — and put an explicit, cheap gate on it.** Concretely: keep the Windows presets green in CI
(they are the only thing that keeps upstream merges mechanical), rebase on a fixed cadence rather
than opportunistically, and accept that once §2.3 lands you are permanently incompatible with
upstream replays and should stop pretending otherwise. Hard-forking now buys nothing; continuing to
target upstream *mergeability* (as opposed to *merge-from-ability*) will quietly veto the port's
core decisions.

Not a decision but a policy gap: **nothing in CI covers any of this work.** No native probe run, no
OpenAL build, no spike build. All four PRs' numbers will rot on the next upstream merge. A single
Linux CI job running `native-port-probe.py` and building the spike and the OpenAL backend is a few
hours of work and is the cheapest thing on this whole list.

### 2.5 Apple Silicon: Rosetta vs native arm64, and the assembly

**Recommendation: native arm64 only. Do not spend an hour on Rosetta.**

Rosetta 2 translates x86-64, not 32-bit x86 (Apple removed 32-bit support in macOS Catalina), so it
does not rescue the existing 32-bit build at all — you would still have to do Phase 2 in full to get
an x86-64 binary, and then you would ship a slower, translated one. Rosetta is also announced as
deprecated in future macOS releases. There is no version of "use Rosetta" that saves work here.

The assembly is not the obstacle: 62 `__asm` statements in 17 files, of which the numerically
largest (`vp.cpp`, an SSE vector processor; `wwmath.h`'s `Sin`/`Cos`/`Float_To_Long`) already have
guarded C++ or Intel-compiler-only paths. The real items are `mutex.h`'s `lock bts` spinlock,
`wwmemlog.cpp`, `cpudetect.cpp` (CPUID, needs an arm64 answer, not a translation) and
`profile_funclevel.cpp` (which is a profiler and should simply be disabled off Windows).

One trap worth stating explicitly, because the plan says "delete the 19 inline `__asm` blocks in
favour of plain C++" as though it were behaviour-preserving: it is not. `Float_To_Long`'s x87
`fistp` rounds to nearest-even; the existing non-x86 fallback is `(long)f`, which truncates toward
zero. Those disagree for every negative and every .5 value. This is live gameplay math. Every asm
deletion needs the semantics checked, not just the syntax.

### 2.6 Where the retail data comes from — the largest unanswered question

What the code actually does today, measured:

- `WinMain.cpp` calls `GetModuleFileName` + `SetCurrentDirectory` to chdir to the executable's
  directory, then everything is loaded relative to the cwd (`.big` archives via
  `ArchiveFileSystem`, plus loose `Data\`, `Maps\`).
- The **user data** directory is built from the Windows registry (`UserDataLeafName` under
  `SOFTWARE\Electronic Arts\EA Games\Command and Conquer Generals Zero Hour`) combined with
  `SHGetKnownFolderPath(FOLDERID_Documents)`.
- Path handling is Windows-shaped throughout: **195 string literals containing backslashes** in
  GameEngine sources and **120 `'\\'` character literals** (`strrchr(buffer, '\\')` and friends).
  `.big` archives are case-sensitively indexed against a case-insensitive filesystem's habits.

So there are three separate sub-decisions and none has been made:

1. **Data discovery.** Recommendation: a single `CNC_GAME_DATA` env var, then a documented search
   order (`.app` bundle `Resources/`, `~/Library/Application Support/…`, cwd), with a clear error
   naming the searched paths. Do not port the registry lookup.
2. **Path separators and case.** Recommendation: one normalisation choke point in `FileSystem`
   (`\` → `/`, case-insensitive lookup for archive members), not 315 site edits. This is the single
   most likely source of "it builds and then immediately can't find anything".
3. **Licensing/packaging.** The build must never ship EA assets. The user supplies a retail
   install; the `.app` locates it. This needs to be written down before anyone builds an installer.

**And the unmeasurable one:** *nothing in this project has ever been tested against real retail
game data.* Not the OpenAL backend, not the archive reader, not the asset pipeline. Every estimate
below is for code that has never met its inputs.

---

## 3. Risks, ranked by how much they can move the estimate

1. **Renderer parity beyond the proof-of-concept (±1,000 h, could double Phase 4).** The spike does
   two draws. Not covered at all: fixed-function lighting/fog/materials, all eight texture stages,
   render targets and pass restructuring, DXT/cube/volume textures and mipmaps, dynamic
   vertex/index buffers, managed-pool shadow copies, device loss, and the ~100 individual render
   states behind `SetRenderState`. This is the critical path and the widest error bar.
2. **MoltenVK is unverified and the spike has a known blocker for it (§2.1) (±300 h, or fatal to
   option A).** Settle this in the first week. If MoltenVK cannot serve the engine, the renderer
   decision changes and so does the whole estimate.
3. **Nothing has been run against retail data (unbounded).** Asset loading, `.big` archives, audio
   formats, map parsing, campaign scripts. The first day someone points the build at a real
   installation will produce a class of bugs none of the current measurements can predict.
4. **The renderer and device layer have never been compiled off Windows (±400 h).** 238 in-scope
   translation units, the D3D8- and Win32-densest in the codebase, are outside every measurement
   made so far. Their portability is currently an assumption.
5. **Silent 64-bit layout corruption (±200 h, and it will cost days of confusing debugging).**
   §1.4. Cheap to fix, expensive to discover late, and invisible to the probe.
6. **Audio is interface-complete but functionally unproven (±250 h).** No audible output has ever
   been produced. Known gaps in the backend's own documentation: no MP2/MP3 streaming (Zero Hour
   ships `.mp3` music and `.mp2` speech), filters accepted but not applied, occlusion approximated,
   speaker selection ignored, stream looping a no-op. Plus the Miles semantics that are easy to get
   subtly wrong: preallocated voice pools, EOS callbacks arriving on the mixer thread, matched
   ADPCM allocate/free.
7. **Upstream merge burden (±150 h/yr, growing).** No CI covers the port; every upstream merge can
   silently invalidate it.
8. **Float determinism on arm64 (±100 h, or a campaign-breaking class of bug).** Replacing x87 asm
   with SSE/NEON changes results (§2.5); arm64 FMA contraction changes them again. Single-player
   tolerates more than multiplayer, but scripted mission triggers and the CRC/replay machinery do
   not vanish just because online play is out of scope.
9. **Video (±100 h).** Bink is Windows-only; the `RTS_BUILD_OPTION_FFMPEG` path exists but is
   `OFF` by default and has not been built or run on any platform in this project.
10. **The build system itself (±100 h).** The non-Windows branch of `CMakeLists.txt` currently skips
    `dx8.cmake` and `bink.cmake` as well as Miles, so no native configure can currently succeed.
    Nobody has produced a native build target for the game at all — only for a standalone spike.

**Could invalidate the plan outright:** only two. (i) MoltenVK or the chosen backend proving unable
to serve the fixed-function feature set at acceptable performance on Apple Silicon. (ii) The retail
data being materially different from what the code expects in a way that is only discoverable by
trying. Everything else on this list is expensive, not fatal.

---

## 4. Corrected effort estimate

The existing ~3,100–6,200 h figure should be replaced, less because the magnitude is wrong than
because its composition is: it is three mutually inconsistent totals (§1.5), it costs phases that
are already partly done, it omits 238 translation units nobody has measured, and it sizes Phase 2
("64-bit correctness, 600–1,200 h") with no measurement behind it at all.

Rebuilt bottom-up from things I measured:

| Slice | Hours | Basis |
|---|---:|---|
| Rebase the stack onto `main`, native CI, native configure that completes | 80–160 | 6 commits, conflict-free today; `dx8.cmake`/`bink.cmake` gaps |
| Fixed-width type correctness (`bittype.h`, `WideChar`, 261 blob sites, 123 pointer↔int casts, `Dict`'s pointer-packing) | 250–500 | measured site counts; mostly mechanical, `Dict` and the memory pools are not |
| `PreRTS.h` surgery + real platform layer (window, input, timing, files, paths, two registries) | 350–650 | 50 `windows.h` files, 21 `HWND` files, 315 path-literal sites, ~100 residual shimmed failures |
| Route 376 direct D3D8 sites through `DX8Wrapper` (still on D3D8/Windows) | 150–250 | measured; mechanical and verifiable |
| Renderer backend to parity | 900–1,700 | 62 methods, ~100 render states, 8 texture stages, 16 shaders (117 lines), plus everything in risk #1 |
| MoltenVK/arm64 bring-up of the backend | 60–150 | includes the portability fix and Apple GPU format constraints |
| Compile and port the 238 unmeasured renderer/device/`Main` TUs | 200–500 | never measured; wide by construction |
| Audio to audible parity (MP2/MP3, filters, voice pools, EOS threading) | 200–400 | 94 functions, 197 call sites, all currently untested |
| Video via FFmpeg | 80–200 | path exists, never built |
| Campaign: cutscenes, speech/subtitles, cinematic camera, scripting verification | 300–600 | ~20 missions |
| Integration, performance, `.app` packaging, notarisation, QA | 500–900 | does not parallelise |
| **Total** | **3,100–6,000** | |

Two things to say plainly about this number.

**It is not more optimistic than the old one — it is differently composed and better grounded.**
Roughly 40% of it (renderer + MoltenVK + the unmeasured device layer) is one interlocking risk. If
the renderer goes badly the total goes to 8,000 h; if MoltenVK works on day one and DXVK's d3d8
frontend turns out usable, 2,500 h is reachable. The range above is the middle of that, not a
confidence interval.

**These are engineering hours and exclude:** acquiring and legally handling retail game data;
Apple developer account, code signing and notarisation turnaround; Apple Silicon hardware for
whoever does the renderer; and QA calendar time for ~20 campaign missions. Those are waits and
access problems, not engineering, and they do not compress with more engineers.

**Critical path:** rebase → fixed-width types → renderer backend → integration/QA. The renderer
alone is 1,100–1,950 h of it and takes at most 2–3 people usefully. With 3–4 engineers the calendar
is ~12–20 months; with one, it is not a finishable project.

*In terms of my own throughput rather than a team's: the first four slices below are one session
each, and most of the mechanical work (type widths, call-site routing, path normalisation) is
session-sized. The renderer and the retail-data integration are not — they are open-ended
debugging against real assets, and that is where the calendar actually goes.*

---

## 5. Recommended next slices

**Slice 1 — Land the stack on `main`, and put it in CI. Serial, blocks everything.**
Rebase `682b413d9` onto current `main`, re-run both probes, update the numbers in the docs, and add
one Linux CI job that runs `native-port-probe.py`, builds `spikes/renderer/` and builds the OpenAL
backend. Until this lands, three of the four PRs are invisible on `main` and every number in the
docs is unreproducible against it. **~1 session.**

**Slices 2, 3 and 4 run in parallel with each other, after slice 1.**

**Slice 2 — Fix the fundamental integer widths.** `bittype.h` to fixed-width types, `WideChar` to
`char16_t`, `static_assert` on the size of every struct read or written as a blob, then chase the
fallout. Do this before anyone debugs the renderer against `.w3d` files, or they will be debugging
the wrong thing. Verified by keeping the Windows build green plus the new asserts. **~1 session
for the mechanical part; the fallout is unbounded but self-announcing.**

**Slice 3 — Settle the renderer backend decision with two experiments, not arguments.** (a) Add
`VK_KHR_portability_enumeration`/`portability_subset` to the spike and run it on macOS/arm64 —
this is hours, and it is the single highest information-per-hour task available. (b) Time-box a day
on DXVK's `d3d8` frontend on MoltenVK. Then write the decision down. **~1 session, needs an Apple
Silicon machine.**

**Slice 4 — Route the 376 direct D3D8 call sites through `DX8Wrapper`, still on D3D8.** Mechanical,
incremental, verifiable against a green Windows build, required by every backend option. Start with
`W3DWater.cpp` (129) and `W3DShaderManager.cpp` (103). **~1–2 sessions.**

**Slice 5 — `PreRTS.h` surgery. Serial, after slices 1 and 2.** Remove the Win32 include block,
push includes down to the ~50 files that need them, extend the probe to `WW3D2`, both
`GameEngineDevice` trees and `Main` so the remaining surface is measured rather than assumed, and
retire `scripts/native-port-shims/` before someone mistakes it for a platform layer. Only after
this does a native configure that reaches the game (rather than a spike) become meaningful.

Deliberately **not** in the next five: writing the Vulkan backend (blocked on slice 3), the audio
functional work (blocked on retail data), and anything to do with saves (blocked on slice 2).

---

## Appendix — how to reproduce every number above

```sh
# D3D8 surface: 62 methods / 458 sites / 82 macro
python3 spikes/renderer/tools/d3d8-surface-scan.py

# Probe, native and shimmed. --deps-dir must point at the fetched vendor headers or
# the Compression target loses one TU and you get 114/151 instead of 115/151.
python3 scripts/native-port-probe.py --report /tmp/native.md --jobs 8
python3 scripts/native-port-probe.py --with-shims --report /tmp/shimmed.md --jobs 8

# The width experiment: set DWORD/ULONG/uint32/sint32 to fixed-width types in both
# WWLib/bittype.h and scripts/native-port-shims/windows.h, then re-run the shimmed probe.
# 637/737 -> 636/737, while sizeof(ChunkHeader) goes 16 -> 8.

# Miles surface: strip comments before counting, unlike the original scan.
# 105 distinct identifiers, 94 called as functions at 197 sites, 11 constants at 20 refs.

# OpenAL backend builds clean and is interface-complete:
sudo apt-get install -y libopenal-dev
for f in Core/Libraries/Source/OpenALAudioDevice/*.cpp; do
  clang++ -std=c++17 -m64 -c -Wall -Wextra \
    -ICore/Libraries/Source/OpenALAudioDevice \
    -ICore/Libraries/Source/OpenALAudioDevice/mss -o /tmp/$(basename $f).o $f || echo "FAIL $f"
done
nm /tmp/*.o | grep ' T ' | grep -c AIL_    # 101, matching 101 declared in mss.h

# Vulkan spike, verified working on llvmpipe:
sudo apt-get install -y cmake ninja-build libvulkan-dev vulkan-validationlayers \
                        glslang-tools mesa-vulkan-drivers
cmake -S spikes/renderer -B /tmp/spikebuild -G Ninja && cmake --build /tmp/spikebuild
/tmp/spikebuild/zh-renderer-spike --out /tmp/spike.png
```
