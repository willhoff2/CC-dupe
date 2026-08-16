# Loading a retail map and ticking TheGameLogic, headless, off Windows

This is a probe report, not a slice. It measures how far the simulation — `TheGameLogic`, the map
loader, the `Xfer`/CRC path — actually gets on a 64-bit non-Windows host against real retail Zero Hour
data, and where it stops. Only one defect is fixed here (§2, its own commit); everything else is
recorded and routed.

The headline is not the crash. The headline is that **the engine loads a retail skirmish map, reports
success, and simulates 13,500 frames of a world with none of the map's objects in it** — a silent
wrong answer, produced by a build in which the assertion that was written to catch exactly this cannot
be compiled.

## 0. What was measured, and with what

| Thing | Value |
|---|---|
| Host | Ubuntu 22.04, x86-64, Linux — **not** Apple Silicon. See §8 |
| Compiler | `clang++-14`, `scripts/native-build.py --level 1..4 --with-shims --strict-link` |
| Binary | `build/native/native_strict_link`, ELF 64-bit x86-64, objects 977/977, 0 unresolved |
| Data | `zerohour104_gamedata_trimmed.7z` from the replay-CI bucket, extracted outside the repo |
| Generals data | `generals108_gamedata_trimmed.7z`, needed because ZH loads the base game's `.big`s |
| Driver | the engine's own `-headless -replay <file>` path — no renderer, no window |
| Replay | `GeneralsReplays/GeneralsZH/1.04/Replays/366648.rep` (in-repo, retail 1.04, 7:30 long) |
| Map | `[RANK] Arctic Arena ZH v1`, loaded from `MapsZH.big` as the replay names it |
| Harness | `spikes/sim` + `scripts/native-sim-probe.py`, for the pieces the game path cannot isolate |

`-headless` skips `WW3D::Init` (`W3DDisplay.cpp:814`), so the null `RenderBackend` that stopped the
first native run (`first-native-run-arm64.md` §3) is not on this path. **The simulation is reachable
today without a renderer.** That is the enabling result for everything below.

### Data provenance caveat, stated up front

Both archives download at the expected size (Zero Hour: 53,307,598 bytes exactly) but **neither
SHA-256 matches `.github/workflows/check-replays.yml`**:

| Archive | Workflow expects | Measured |
|---|---|---|
| Zero Hour | `6837FE1E…AC05E21` | `2d137f6cd51609b517345fc7ab780dfbf4547eca0dd689e7212d8506a36b1b13` |
| Generals | `37A351AA…CDB06372` | `15332b5dca7d94f672be061954c5cd09525a4adc689623fbcb3d1d6101600f3c` |

Every conclusion below that depends on the *bytes* of a `.big` — above all the cross-platform CRC
comparison in §5 — is therefore conditional. The mismatch is either a repack of the same content or a
different content, and this probe cannot tell which. Resolving it is a prerequisite for treating the
replay CRC as an oracle. The map/`.rep` files used for §1 and §3 are the in-repo ones, so those
results do not depend on the archives.

## 1. Can a `.map` be read? At the container level, yes

`sim_probe chunks` drives `DataChunkInput` over a real map with no engine init at all:

```
$ sim_probe chunks GeneralsReplays/GeneralsZH/1.04/Maps/tansooo/tansooo.map
HeightMapData    version=4  dataSize=211628
BlendTileData    version=8  dataSize=1781662
WorldInfo        version=1  dataSize=24
SidesList        version=3  dataSize=2817
ObjectsList      version=3  dataSize=60623
PolygonTriggers  version=4  dataSize=83
GlobalLighting   version=3  dataSize=872
WaypointsList    version=1  dataSize=4
RESULT chunks topLevelChunks=8
```

The CHUNKY container, its name table, its version and length fields all read correctly at 64-bit. The
`.map` format is not endian- or width-sensitive at this level.

Two path defects stand between that and the game finding a map on its own:

- **`MapCache` finds nothing.** `MapCache::loadMapsFromDisk()` builds `"%s\\"` patterns and rejects any
  entry whose path has no `'\'` in it (`MapUtil.cpp`, `reverseFind('\\')` → `DEBUG_CRASH("Couldn't find
  \\ in map name!")`), then requires the `dir\name\name.map` nesting spelled with backslashes. Native
  enumeration returns forward slashes, so `sim_probe mapcache` over a directory of real maps returns
  `cache entries=0`. Not fatal for a replay, which names its map explicitly; fatal for skirmish, which
  picks from the cache.
- **Case and separator.** The engine asks the filesystem for
  `…/command and conquer generals zero hour data/maps\[rank] arctic arena zh v1\…` — lowercased, with
  literal backslashes. `StdLocalFileSystem` translates separators when it *opens* a file but the
  lowercasing is not undone, so on a case-sensitive filesystem the open fails. This probe worked around
  it outside the repo with lowercase symlinks; it is the filesystem seam's to fix
  (`filesystem-and-registry.md`), not this probe's.

## 2. The allocator: every object the game builds was misaligned (fixed here)

With the map path worked around, `GameEngine::init()` died at `GameEngine.cpp:577`:

```
Thread 1 received signal SIGSEGV
=> 0x…<GameEngine::init+3406>: movaps %xmm0,0x10(%rdi)
fault addr mod 16 = 8
#0 GameEngine::init () at GameEngine.cpp:577   // MSGNEW(...) RankInfoStore()
#1 Win32GameEngine::init () at Win32GameEngine.cpp:96
#2 GameMain () at GameMain.cpp:46
```

`MEM_BOUND_ALIGNMENT` was 4 and user data began at `sizeof(MemoryPoolSingleBlock)` past the block
header, which is 24 bytes at 64-bit — so **every** pointer the engine's allocator returned was
`8 mod 16`, and the global `operator new` routes through that allocator. Clang's aligned SSE store
into a freshly constructed `RankInfoStore` faults. The same would be true of NEON aligned pairs on
arm64.

This is a port defect in the memory manager, it is not owned by any of the four in-flight slices, and
nothing in the simulation can be measured behind it, so it is fixed in its own commit: the bound
becomes 16 except on 32-bit Windows, which keeps 4 and therefore keeps its exact historical layout,
and the header size is rounded up to the bound. Verified by measurement, not by inspection — reverting
just this change reproduces the SIGSEGV above at the same line, and restoring it gets init through.

Note what this implies about the value of syntax-only and link-only measurements: 977/977 objects and
0 unresolved symbols were achieved with an allocator that could not return a usable pointer.

## 3. Loading the map: the silent failure (slice A's boundary, not touched)

The replay's map opens, `WorldHeightMap(pStrm, logicalDataOnly=true)` runs, `loadMap` returns TRUE,
and the game proceeds. `MapObject::getFirstMapObject()` then returns null and `GameLogic.cpp:1881`
iterates an empty list. **0 objects.** No exception, no assert, no log line.

Instrumenting `DataChunkInput` at chunk open/close shows why. Reading the `SidesList` chunk, which
opens at file offset 717,696 with `dataSize=3523`:

```
sides count=14  tell=717700
  entry tell=717706 keyAndType=00000803
  entry tell=717712 keyAndType=00000900
  entry tell=717717 keyAndType=00000a04   <- DICT_UNICODESTRING 'playerDisplayName'
  entry tell=717751 keyAndType=00000000   <- garbage: 34 bytes consumed, not 18
  entry tell=717756 keyAndType=06000000   <- garbage
SIDE i=0 done tell=717775
SIDE i=1 done tell=914579                 <- one readDict() ate 196,804 bytes, to EOF
teams-section tell=914579
before nested parse tell=914579 teams=21845
after nested parse tell=914579 atEndOfChunk=1
```

The mechanism, exactly:

1. `DataChunkInput::readUnicodeString()` (`DataChunk.cpp:963`) reads a 16-bit length and then
   `len * sizeof(WideChar)` bytes. Retail files store 2 bytes per character; `wchar_t` is 4 bytes off
   Windows. For the 8-character `playerDisplayName` it consumed 32 payload bytes instead of 16.
2. The dictionary read is now 16 bytes out of phase, so the following key/type words are garbage.
3. Side 1's `readDict()` reads a garbage entry count and runs the stream to EOF.
4. `SidesList::ParseSidesDataChunk` then does a *nested* `file.parse()` for `PlayerScriptsList`
   (`SidesList.cpp:300`) — at EOF, which returns TRUE.
5. So does the outer `parse()`. `ObjectsList` (`dataSize=190923`), `PolygonTriggers` and
   `WaypointsList` are never reached: the parser walked past them inside a runaway string read.
6. `WorldHeightMap` throws nothing, `loadMap` reports success, and the simulation starts on a map with
   terrain and no objects.

The guardrail exists — `DEBUG_ASSERTCRASH(file.atEndOfChunk(), ("Incorrect data file length."))`, the
last line of `ParseSidesDataChunk` — and would have fired at once. See §6 for why it could not.

This is the wide-character disk boundary, which is slice A's. It was **not** fixed. To measure what
lies behind it, a throwaway experiment (kept out of this PR; the diff is 72 lines across
`DataChunk.cpp`, `LocalFile.cpp::readWideChar` and `Recorder.cpp`'s `ARGUMENTDATATYPE_WIDECHAR`, all
reading 2-byte units off Windows) was applied locally and the run repeated. Results in §4 and §5 are
labelled with which build produced them. `LocalFile.cpp`/`Recorder.cpp` matter because the `.rep`
header itself does not parse without them, so **no replay can be read off Windows today** either.

## 4. Can `TheGameLogic` tick? Yes — 13,500 frames

Driver: the engine's own `-headless -replay`, which is `RECORDERMODETYPE_PLAYBACK` feeding recorded
commands into `TheCommandList` and running `GameLogic::update()` with no renderer. Frame state was
read out of the process at each CRC checkpoint (`GameLogic.cpp:3778`) under gdb rather than trusting a
log line.

| | map broken (§3), tree as committed | with the slice-A experiment applied |
|---|---|---|
| objects at frame 0 | **0** | **221** |
| objects at frame 13,500 | 0 | 362, having peaked at 376 |
| `GetGameLogicRandomSeedCRC()` | advances | advances |
| frames reached | 13,500 (7:30/7:30, the replay's full length) | 13,500 (7:30/7:30) |
| frame CRC | **constant `06b88758` for all 134 checkpoints** | varies every checkpoint |

So the simulation loop, the message stream, the partition/pathfind/AI updates and object creation and
destruction all execute at 64-bit off Windows for the full length of a real 7:30 replay. Units are
built and destroyed (221 → 376 → 362).

What this does **not** establish: that any of it is correct. The property verified is "13,500
iterations of `GameLogic::update()` completed without crashing, with a plausibly evolving object
count". Nothing here compares a single unit's position, health or order against Windows.

The constant-CRC column is the point of the table. A CRC that never changes across 13,500 frames is
the signature of an empty world, and it was produced by a run that reported no error at all. This is
the "silently wrong value" of the probe brief, and it is worth more than the crash: a fix to the
allocator plus a renderer would have produced a game that *ran*, on maps with no objects on them.

## 5. The CRC: deterministic on this platform, and it does not match Windows

Same-platform determinism (the minimum bar) — **met**, on the experiment build with a populated world:
two runs, 134 checkpoints each, byte-identical CRC sequences (`diff` clean), and the 136-entry
frame-state series identical too. The standalone `XferCRC`/`CRC` path is stable in isolation as well:

```
$ sim_probe xfercrc …/tansooo.map 3   -> RESULT xfercrc bytes=106110 runs=3 crc=2465AFE3 stable=yes
$ sim_probe filecrc  …/tansooo.map 3  -> RESULT filecrc  bytes=106110 runs=3 crc=6314F5FD stable=yes
```

Cross-platform agreement with the Windows-recorded replay — **not achieved**. The playback compares
the CRC the recording stored against the one this build computes (`Recorder.cpp:1007`):

```
CRC Mismatch in Frame 110      (reproduced identically on 2 of 2 direct runs)
cmp frame=111 windows=659e2f68 native=1d39673e
cmp frame=211 windows=9196e16b native=5b1e190c
cmp frame=311 windows=5e0f94d5 native=4476f04d
```

The comparison is queue-lagged by design, so a naive first-mismatch frame proves little. What does
prove something: of the 134 CRCs this build computed and the 134 the recording carries, **zero values
are shared, at any alignment shift**. The two simulations differ by the first checkpoint (frame 100),
and the divergence is not a bookkeeping offset.

That is expected rather than surprising — `xfer-64bit-audit.md` and `raw-blob-audit.md` already
predicted it statically — but it is now measured on real data: 71 raw-blob `xferUser` sites and 13
enums with no fixed underlying type feed this CRC (`scripts/xfer-blob-audit.py`), so any pointer,
`size_t` or padding byte inside a serialised struct changes it at 64-bit. **Which** field diverges
first was not determined; §7 scopes that.

Suppressing the mismatch in the debugger (`set var newCRC = playbackCRC`) lets playback run to
completion, which is how the 13,500-frame figure in §4 was obtained; without suppression the engine
stops the replay at the first mismatch. That suppression is a measurement device, not a fix.

## 6. Why it was silent: no assertion-enabled native build exists

`DEBUG_ASSERTCRASH` compiles to `((void)0)` unless `DEBUG_CRASHING` is defined, which `Debug.h` derives
from `RTS_DEBUG`. Every native figure this project has ever published, this one included, comes from a
build without it. Asking for one:

```
$ CXXFLAGS="-DRTS_DEBUG -DDISABLE_DEBUG_STACKTRACE -DDISABLE_DEBUG_PROFILE" \
  python3 scripts/native-build.py --level 1..4 --with-shims --strict-link
objects 967/977, undefined symbols 113, binary produced: no
```

10 translation units fail, on 6 distinct debug-only Win32 spellings:

| Failure | Files |
|---|---|
| `DebugBreak` not declared (the shim has `__debugbreak`) | `Debug.cpp`, `CRCDebug.cpp` |
| `GetPrecisionTimer` / `GetPrecisionTimerTicksPerSec` missing off Windows | `SubsystemInterface.cpp`, `GameEngine.cpp`, `GameLogic.cpp`, `W3DAssetManager.cpp` |
| `WSABASEERR` | `GameResultsThread.cpp` |
| `GetUserNameA` | `LANAPI.cpp` |
| `MAX_COMPUTERNAME_LENGTH` | `Recorder.cpp` |
| debug-log call passes a `const char*` where a struct is indexed | `StdLocalFileSystem.cpp` |

`Debug.cpp` itself is one of them, which is why the link then loses `DebugCrash`/`DebugLog` 113 times.
So the native port has no build in which any of the engine's ~thousands of assertions can fire. The
map corruption in §3 is exactly the class of bug those assertions were written to catch, and it went
undetected through a full 7:30 simulation. This is the cheapest high-value slice in the report.

## 7. Blockers, classified

| # | Blocker | Class | Owner |
|---|---|---|---|
| 1 | Pool allocator returned `8 mod 16` pointers; aligned SSE/NEON stores fault | port defect | **fixed here**, §2 |
| 2 | `readUnicodeString` reads `len*sizeof(WideChar)`; map `SidesList` desyncs and swallows `ObjectsList` | port defect | slice A (widechar) |
| 3 | `.rep` header unreadable off Windows (`LocalFile::readWideChar`, `ARGUMENTDATATYPE_WIDECHAR`) | port defect | slice A (widechar) |
| 4 | Chunk parse reports success at EOF instead of failing (`SidesList.cpp:300`, nested `parse()`) | port defect, exposed by 2 | simulation slice |
| 5 | `MapCache` requires `'\'` in enumerated paths → 0 maps; lowercased map path with backslashes | unimplemented path | filesystem seam |
| 6 | No `RTS_DEBUG` native build: 10 TUs, 113 unresolved | unimplemented path | new slice, §6 |
| 7 | Frame CRC disagrees with the Windows recording from the first checkpoint | port defect (serialisation width) | `xfer-64bit-audit.md` slice |
| 8 | SIGSEGV at exit in `ObjectPoolClass<MultiListNodeClass,256>::~ObjectPoolClass` (`mempool.h:208`), *after* the replay completes | port defect | simulation slice |
| 9 | Archive SHA-256s do not match the workflow's | data/provenance | user / replay CI |
| 10 | `sim_probe replayhdr`/`mapcache` SIGSEGV inside `RecorderClass::init` → `GameSlot::setState` | harness limitation, not an engine finding | this harness |

## 8. What could not be determined

- **Whether any of this holds on Apple Silicon.** This was measured on Linux x86-64. The alignment fix
  is if anything more necessary on arm64, and the widechar defect is identical (4-byte `wchar_t`), but
  no arm64 run was performed here.
- **Whether the simulation is correct** in any sense beyond "it ticks and its object count moves". No
  per-object state was compared with anything. With the CRC disagreeing from frame 100, the honest
  reading is that it is *not* correct yet.
- **Which serialised field diverges first.** Requires a per-`Xfer`-call trace on both platforms, or
  `XferDeepCRC` bisection. Not attempted.
- **Whether the 221 objects loaded with the experiment applied are the *right* 221**, or their
  positions and dictionaries correct. Only the count and the pointer chain were checked.
- **Whether skirmish (not replay) can start**, which needs `MapCache` (blocker 5) and the shell or a
  synthetic `GameInfo`.
- **Whether the exit-time crash (blocker 8) is independent** of earlier corruption.
- **Whether the trimmed archives are sufficient**: they were, for everything reached here. Note that
  the Zero Hour archive alone is not — `Data\english\Language` is not in it, and init reads it; it
  comes from `English.big` in the *Generals* archive, which the ZH build loads via the base game's
  `InstallPath` setting. The hash mismatch (blocker 9) makes any byte-level claim provisional.

## 9. What the next slice on this subsystem should be scoped to

In this order, because each unblocks the measurement of the next:

1. **Make `RTS_DEBUG` build natively.** 6 spellings, 10 translation units, no engine logic. Exit
   criterion: `scripts/native-build.py … -DRTS_DEBUG` produces a binary with 0 unresolved symbols, and
   CI builds it. Everything below is dramatically cheaper afterwards, because the engine starts telling
   you where it went wrong.
2. **Slice A finishes the chunk-file widechar boundary** (blockers 2 and 3) and adds, as its gate, a
   check that a real `.map` yields a non-zero `MapObject` count — not just that it parses. The failure
   mode this probe found is "parses fine, loads nothing".
3. **Make a truncated chunk parse fail loudly** (blocker 4): `DataChunkInput::parse` reaching EOF with
   chunk data outstanding should be an error even in a release build. This is the difference between a
   port bug and a silently wrong game.
4. **Then, and only then, the CRC.** With a correctly populated map, bisect the first diverging `Xfer`
   field against a Windows run of the same replay, starting from the 71 raw-blob sites the audit
   already lists. Exit criterion: a named field, not a frame number.

A headless replay run is a good CI gate for this subsystem once 1-3 land: it needs no renderer, it
takes 10 seconds of CPU for 13,500 frames, and "object count > 0 and the CRC sequence is not constant"
would have caught the failure documented here.
