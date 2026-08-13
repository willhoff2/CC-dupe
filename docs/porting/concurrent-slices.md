# Concurrent slices — ownership register

Several sessions work port slices in parallel. This file is how they avoid colliding: claim the paths
your slice owns *before* you start, and check that nobody else holds them. It is deliberately a plain
table rather than tooling — the cost of a stale row is a rebase, the cost of two slices silently
editing `dx8wrapper.cpp` is a day.

Hand-maintained. `STATUS.md`, by contrast, is generated; do not put measurements here.

## In flight

| Slice | Owns (paths) | Exit criterion | PR |
|---|---|---|---|
| _example_ `platform/audio-device` | `Core/Libraries/Source/OpenALAudioDevice/**` | every declared `AIL_*` symbol defined; `check-openal-symbols.py` green | #28 |
| `platform/macos-window-compile` | `Core/Libraries/Source/WWVegas/WWLib/platform/platform_window_cocoa.mm`, `spikes/renderer/src/metal_surface_probe.mm`, `spikes/renderer/tools/macos-window-check.sh`, `spikes/renderer/CMakeLists.txt`, `docs/porting/window-event-loop.md`, the `window-seam-macos` job in `.github/workflows/native-port-ci.yml` | a green (not `continue-on-error`) `macos-15` job that compiles the Cocoa backend and asserts the Vulkan/Metal surface facts the runner can prove | this PR |

Remove your row when the PR merges.

## Serialised paths

At most **one** in-flight slice may touch each of these, because a second concurrent edit produces a
conflict that is not mechanical to resolve, or a measurement that cannot be attributed:

- `docs/porting/ci-baselines/*.json` — two slices refreshing baselines produce numbers neither can
  reproduce.
- `scripts/native-port-probe.py` and `scripts/porting-status.py` — changing the measurement while
  someone else is measuring invalidates both results.
- `Core/Libraries/Source/WWVegas/WW3D2/dx8wrapper.{h,cpp}` and `d3d8renderbackend.cpp` — the renderer
  chokepoint; every renderer slice wants it.
- the root `CMakeLists.txt` and `cmake/*.cmake` — build wiring conflicts are silent until link time.

If your slice needs a serialised path that someone else holds, resequence rather than sharing it.

## Rules that keep concurrent work compatible

1. Branch from current `main`, never from another slice's branch.
2. One seam per PR; no drive-by edits outside your claimed paths.
3. Merge one port PR at a time; after each merge, every in-flight slice rebases onto `main` and
   re-measures before quoting a figure.
4. Additive over invasive: add a seam, a shim or a new implementation file rather than reshaping a
   header that other slices are reading.
5. Prefer extending `scripts/native-port-shims/` and `scripts/ci/` over editing shared engine headers.
6. If two slices genuinely need the same file, that is an escalation, not a merge conflict to be
   resolved later.
