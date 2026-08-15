# Video (Bink out, FFmpeg in) and the harness's missing headers

Two failure classes in the native 64-bit build were build-system-shaped rather than engine-shaped:

* **Bink**, 6 translation units failing on `'bink.h' file not found`. The Bink SDK is a Windows-only
  binary dependency — `cmake/bink.cmake` is included only from the 32-bit Windows branch of the top
  level `CMakeLists.txt` — and the decision recorded in `review-and-decisions.md` §9 is that the port
  plays video through the existing `RTS_BUILD_OPTION_FFMPEG` path. So Bink has to *compile out* off
  Windows. It is not ported, and it is not replaced by something that pretends to decode.
* **Three headers the native harness never provisioned**: `libavcodec/avcodec.h`,
  `stb_image_write.h`, and — a diagnosis, not a dependency — `'Common/File.h'`, which is
  `Common/file.h` on a case-sensitive filesystem.

## 1. Measured

Linux/x86-64, clang 14, `scripts/native-build.py --level 1 --level 2 --level 3 --with-shims`, with
`./scripts/ci/fetch-probe-deps.sh` run first. Both columns were measured in this session; the
"before" column is a fresh run of `main` at `eb9e98110` in a clean worktree, which reproduced the
committed baseline exactly.

| | before (`main`) | after |
|---|---:|---:|
| Object files (shimmed, levels 1-2-3) | 756 / 835 | **763 / 835** |
| Translation units producing no object | 79 | **72** |
| Undefined symbols at link | 341 | **334** |
| `'bink.h' file not found` | 6 | **0** |
| `libavcodec/avcodec.h`, `stb_image_write.h`, `Common/File.h` not found | 3 | **0** |
| Probe-clean units that fail to compile anyway | 0 | **0** |

The seven new objects are `BinkVideoPlayer.cpp`, `FFmpegFile.cpp`, `FFmpegVideoPlayer.cpp`,
`stb_image_write_impl.cpp`, `W3DTankDraw.cpp`, `W3DTankTruckDraw.cpp` and `W3DTruckDraw.cpp`.

The 6th Bink unit, `W3DDisplay.cpp`, no longer fails on `bink.h` — it now fails on `'WinMain.h' file
not found`, which belongs to the window/entry-point slice, and `W3DGameClient.cpp` now fails on
`TheD3D8RenderBackend`, which belongs to the renderer slice. Both are reported, not fixed here.

`scripts/native-port-probe.py`'s own target list does not include `GameEngineDevice`, so its counts
move only by the new dependency set and by a `WWMath` unit that landed on `main` after the baseline
was last refreshed: native 663/751 → 663/752, shimmed 703/751 → 704/752. All four baseline JSONs and
`STATUS.md` were regenerated with the scripts.

## 2. Bink compiles out, it does not disappear

`BinkVideoPlayer.{h,cpp}` stay in the source list and stay byte-for-byte on Windows. Both are
wrapped in `#ifdef RTS_HAS_BINK`, so off Windows the header declares nothing and the implementation
is an empty translation unit — one that now produces an object file, which is why the count went up.

`RTS_HAS_BINK` is defined by `Core/GameEngineDevice/CMakeLists.txt`, guarded on the `binkstub` target
existing, which is exactly the condition under which `bink.h` exists:

```cmake
if(TARGET binkstub)
    target_link_libraries(corei_gameenginedevice_public INTERFACE binkstub)
    target_compile_definitions(corei_gameenginedevice_public INTERFACE RTS_HAS_BINK)
endif()
```

That mirrors how the FFmpeg block a few lines below already defines `RTS_HAS_FFMPEG`. The
unconditional `binkstub` link was removed from the same file and from `GeneralsMD/Code/Main`, where
it becomes the same `if(TARGET binkstub)` guard — a one-line change in another slice's directory,
disclosed in the PR, without which no non-Windows configure can link.

**The draw files never needed Bink.** `W3DTankDraw.cpp`, `W3DTankTruckDraw.cpp` and
`W3DTruckDraw.cpp` reach `bink.h` only transitively, through `W3DGameClient.h`'s include of
`BinkVideoPlayer.h`; nothing in them names a Bink type. They were not edited. `W3DGameClient.h` now
selects a backend instead of including Bink unconditionally:

```cpp
#ifdef RTS_HAS_FFMPEG
	virtual VideoPlayerInterface *createVideoPlayer() { return NEW FFmpegVideoPlayer; }
#elif defined(RTS_HAS_BINK)
	virtual VideoPlayerInterface *createVideoPlayer() override { return NEW BinkVideoPlayer; }
#else
	virtual VideoPlayerInterface *createVideoPlayer() override { return NEW NullVideoPlayer; }
#endif
```

FFmpeg keeps the priority it already had; Windows keeps Bink.

## 3. What is deliberately stubbed

`VideoDevice/Null/NullVideoPlayer.h` is the backend of last resort, selected only when neither
`RTS_HAS_BINK` nor `RTS_HAS_FFMPEG` is defined — which is the state of the native measurement
harness, since it compiles sources without linking FFmpeg's libraries. It opens nothing and
`DEBUG_CRASH`es on `init()`, `open()` and `load()`. It is a loud placeholder that keeps the video
seam type-complete for the harness; **it is not on the path to running the game**, and a native
build meant to play movies must configure `RTS_BUILD_OPTION_FFMPEG=ON`.

## 4. The three headers

| Symptom | Diagnosis | Fix |
|---|---|---|
| `'libavcodec/avcodec.h' file not found` in `FFmpegVideoPlayer.cpp` | the harness provisioned no FFmpeg headers | `fetch-probe-deps.sh` fetches the public headers at the version already pinned in `vcpkg-lock.json` |
| `'stb_image_write.h' file not found` in `stb_image_write_impl.cpp` | the harness provisioned no stb, though `cmake/stb.cmake` pins one | `fetch-probe-deps.sh` clones that same pinned commit |
| `'Common/File.h' file not found` in `FFmpegFile.cpp` | **not** a missing dependency: the file is `Core/GameEngine/Include/Common/file.h`. Windows' case-insensitive filesystem hides it | the include is spelled `Common/file.h` |

**No new dependency was added to the project.** FFmpeg is already declared in `vcpkg.json` and
pinned to `7.1.1` in `vcpkg-lock.json`; the real build takes the libraries from vcpkg. The harness
never links libav*, it only needs the headers to parse, so the fetch is a blobless partial clone of
the five public header directories of upstream tag `n7.1.1` (~14 MB, ~40 MB for the whole dependency
tree). `libavutil/avconfig.h` and `libavutil/ffversion.h` are normally written by FFmpeg's
`configure`; the script writes the two-macro versions of both rather than running `configure`, which
is recorded in the file it generates. Nothing is compiled from FFmpeg's sources.

Both new include roots (`ffmpeg-src`, `stb-src`) are in `FETCHED_DEP_INCLUDES` in
`scripts/native-port-probe.py`, so the probe and `native-build.py` agree on them, and both CI jobs
already run `fetch-probe-deps.sh` — the numbers reproduce on a clean runner rather than only here.
Adding dependencies changes the measured clean count, which is why the probe baselines record
`deps_present` and `check-probe-baseline.py` refuses to compare across a different dependency set.

## 5. The gate

`scripts/ci/check-video-headers.py`, run in the `native-build` job, fails if:

* a Bink include (`bink.h`, or `VideoDevice/Bink/BinkVideoPlayer.h`) appears in a file that does not
  guard on `RTS_HAS_BINK` — the regression this slice exists to prevent is someone re-introducing an
  unconditional Bink include;
* `stb-src/stb_image_write.h` or `ffmpeg-src/libavcodec/avcodec.h` is not provisioned;
* any of the four video translation units produces no object, or any recorded diagnostic mentions
  `bink.h`, `stb_image_write.h`, `libav*`/`libsw*` or `Common/File.h`;
* the number of quoted includes whose case does not match the file on disk exceeds 7, the count on
  `main` after this slice (2 of the 7 are under `Core/Tools`, permanently out of scope, and 5 are in
  the renderer/WWLib headers). This is a ratchet: the class that produced the `Common/File.h`
  failure cannot grow, and every fix should lower the budget.

## 6. Also changed: diagnostic attribution in `native-build.py`

Regenerating the native-build baseline recorded an *empty* first diagnostic for 56 of the 72 failing
units (61 of 79 on `main`), because the scrape only accepted an error line that named the `.cpp`,
and most errors are reported inside a header. It now follows clang's `In file included from` chain
back to the translation unit, and all 72 entries carry text — which is also what makes this slice's
gate on recorded diagnostics meaningful. This is a borrowed, measurement-only change to another
slice's file; the counts it reports are unaffected.

## 7. Still open

* No video has been decoded. `RTS_BUILD_OPTION_FFMPEG` has still never been configured, linked or
  run on any platform in this project; this slice makes the code compile, nothing more.
* `GeneralsMD/Code/Main` still produces no object at all (2 units, `WinMain.h`), so the executable
  does not link — the window/entry-point slice's territory.
* The remaining 72 compile failures are dominated by `TheD3D8RenderBackend` (renderer slice),
  `HFONT` (font slice), and GameSpy/`HRESULT` units that are out of scope.
* `Generals/Code/Main` still links `binkstub` unconditionally. It is out of scope permanently, and
  is only ever configured on 32-bit Windows, where the target exists.
