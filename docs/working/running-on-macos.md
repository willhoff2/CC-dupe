# Running the native macOS build

How to get the game on screen on an Apple Silicon Mac, start to finish. Everything here was run on
2026-09-11 against `main` at `097101125`; the numbers are what that run produced, not estimates.

The build takes ~20 minutes from cold. Section 0 is once per machine; after that, playing again is
section 3 alone.

## Does it need the internet?

**No.** Once the repo is cloned, the dependencies are fetched and the build is done, the game runs
fully offline — no launcher, no account, no phone-home.

- **GameSpy matchmaking and online multiplayer are cut scope and physically absent**, not disabled:
  `docs/porting/online-path-excision.md` records that no GameSpy function is defined off Windows
  and the SDK is not linked at all. Campaign and skirmish are what this port targets.
- The retail patch downloader is a shim off Windows; nothing contacts EA.
- All game content is read from the local retail install.

Internet is needed only for the one-time setup: `git clone`, `brew install`, and
`fetch-probe-deps.sh` (which downloads vendored third-party headers into `build/docker/_deps`).
Once those have run, you can build and play on a plane.

## 0. Once per machine

**0.1 Toolchain and libraries.** `zlib` and `openal-soft` are keg-only, and macOS has no
`/usr/lib/libz.dylib` to link against, so the harness probes the Homebrew keg paths:

```sh
xcode-select --install                       # `xcrun --show-sdk-path` must answer
brew install zlib openal-soft cmake ninja pkg-config sevenzip
./scripts/ci/fetch-probe-deps.sh             # vendored headers into build/docker/_deps
```

**0.2 Point the game at the base game's archives.** Zero Hour is not self-contained — the shell
needs `mainmenubackdropuserinterface.tga` and `new_skybox.W3D`, which live only in base Generals.
A release build says *nothing* when this mount fails; you just get a magenta backdrop and no sky.

`~/Library/Application Support/Command and Conquer Generals Zero Hour/Registry.ini`:

```ini
[SOFTWARE\Electronic Arts\EA Games\Generals]
STRING_InstallPath=/Users/willhoff/devin-work/zh-data/ZH_Generals
```

Do **not** symlink the two archive sets into one directory — the names collide and
`Data\INI\Weapon.ini` fails to parse. See [`../porting/base-game-install-path.md`](../porting/base-game-install-path.md).

**0.3 Optional: edge scrolling in windowed mode.** The engine disables screen-edge camera scroll
outside fullscreen by default, which makes a windowed game feel like the camera is stuck. In
`~/Library/Application Support/Command and Conquer Generals Zero Hour Data/Options.ini`:

```ini
ScreenEdgeScrollEnabledInWindowedApp = yes
```

Camera drag is the **right** mouse button; left-drag is the selection box and middle-drag rotates.

**0.4 Grants, only if you want to script input or screenshots.** The terminal needs Accessibility
(to post input) and Screen Recording (to capture). Playing by hand needs neither.

```sh
python3 scripts/macos-input-drive.py capabilities   # all three must be True
```

## 1. Build

```sh
python3 scripts/native-build.py --level 1 --level 2 --level 3 --level 4 \
    --with-shims --strict-link --jobs 8 \
    --build-dir build/native-macos-arm64-$(date +%m%d) \
    --report ~/devin-work/run$(date +%m%d)/build.md \
    --json   ~/devin-work/run$(date +%m%d)/build.json
```

A good run ends with `981 objects, 0 failures`, `strict link succeeded: 0 unresolved symbol(s)` and
a ~26.8 MiB binary at `<build-dir>/native_strict_link`. Confirm it is really arm64 — `lipo -archs`
should say `arm64` and nothing else.

Do not write macOS figures over `docs/porting/ci-baselines/*.json`; those are the clang-14 Linux
ratchet and a different compiler's numbers are not comparable.

## 2. Make a run directory

The game runs from a directory of symlinks into the retail install, so nothing retail is copied and
nothing retail lands in the repo:

```sh
RUN=~/devin-work/run0911/run
mkdir -p "$RUN"
for entry in ~/devin-work/zh-data/*; do ln -sfn "$entry" "$RUN/$(basename "$entry")"; done
cp build/native-macos-arm64-0911/native_strict_link "$RUN/zh"
```

That is 53 symlinks, `Data/` among them — which is how the loose `Data/Cursors/*.ani` reach the
mouse seam. A tree with no `Data/Cursors` gets the platform's default arrow, silently.

## 3. Play

```sh
cd ~/devin-work/run0911/run && arch -arm64 ./zh -win
```

Fullscreen is a window sized to the display, not a mode switch, so it needs the size passed
explicitly — on the project's M1 Pro (1728x1117 points):

```sh
cd ~/devin-work/run0911/run && arch -arm64 ./zh -xres 1728 -yres 1117
```

Without `-xres/-yres` the fullscreen path leaves an 800x600 window at the top-left: `Apply_Fullscreen()`
sizes the window to the screen, then `DX8Wrapper::Resize_And_Position_Window()` shrinks it back to
`ResolutionWidth/Height`, because on Windows D3D8 would have switched the display mode to match and
off Windows there is no mode switch. See §"Known" below.

Other useful flags: `-noshellmap` (skip the animated menu background), `-nologo` (skip the intro
movie; Escape also skips it).

**To quit:** the `zh` menu's **Quit zh** item, Cmd-Q, or the window's close button. All three run
the engine's own shutdown and exit 0. (Before #160 the menu was empty and Cmd-Q did nothing, so the
only way out was Force Quit — which then aborted in static destruction.)

Healthy startup on an M1 Pro: window up in a few seconds, 30 fps, ~400 MB RSS at the menu. The log
carries three benign notices — `GetSystemDirectoryA()`, `SetThreadExecutionState()` and swscaler's
`No accelerated colorspace conversion` — and should carry no `LoadCursorFromFile` line at all, since
that only prints when a cursor is missing or undecodable.

To capture audio diagnostics while playing, prefix the launch with
`OPENAL_AUDIO_DIAG=/tmp/audio.log`.

## 4. Drive it and look at it (scripted, optional)

```sh
python3 scripts/macos-input-drive.py key        --pid <pid> --binary <run dir>/zh --key 53   # Escape
python3 scripts/macos-input-drive.py snapshot   --pid <pid> --binary <run dir>/zh            # shell_stack, fps
python3 scripts/macos-input-drive.py buttons    --pid <pid> --binary <run dir>/zh            # GameWindow tree
python3 scripts/macos-input-drive.py screenshot --pid <pid> --binary <run dir>/zh --out /tmp/menu.png
```

Pass `--binary` as well as `--pid` or the LLDB attach fails with `SBTarget is invalid`. Note the
game's pid is the child of any wrapper shell, not the shell itself.

`snapshot`'s `shell_stack` is the reliable answer to "which screen am I on" — the screenshot is not,
and a screenshot never captures the cursor.

## Known, as of this build

- **Fullscreen needs explicit `-xres/-yres`** to fill the screen, as above. MEASURED; the underlying
  window-mode seam defect is unowned.
- **Attaching LLDB to an already-running process is refused** (`Not allowed to attach to process`)
  while `DevToolsSecurity -status` reports developer mode disabled. Launch *under* LLDB instead.
- **A constant crackle under all audio**, human-observed on this Mac and recorded as a PORT DEFECT in
  `../porting/sound-effects-chain.md` §6. Not fixed by #155 or #156, which address a different,
  intermittent stall. Leading suspect is below the shim (alsoft missing the CoreAudio deadline);
  mechanism INFERRED.
- **`exit()` from anywhere other than the normal quit path still aborts** in
  `ThreadClass::Switch_Thread()` -> `EventClass::Wait()` (`platform_thread.cpp:76`), a mutex
  destroyed under a live thread. #159 fixed the OpenAL instance of this family and #160 stopped the
  quit path reaching it; this one is open and unowned.
- The intro `.bik` shows a corrupted column down the left edge. The menu render immediately after is
  clean, so it is confined to the video path.

### Fixed since the previous revision of this guide

- **Dead units stayed on the map, kept a live AI and blocked terrain** — a squad dying on a bridge
  walled it off. `getDeathTypeFlag`'s `1UL << (dt - 1)` put `DEATH_NORMAL` on bit 63 at 64 bits
  instead of bit 31, so `DieMuxData::isDieApplicable` rejected every normal death and no die module
  ever ran. Fixed in #161 (`../porting/death-flag-shift.md`), which the Windows replay gate passed.
- **No way to quit**, an empty app menu and an inert Cmd-Q; **the cursor invisible** at the menu
  (thousands of stacked reference-counted `[NSCursor hide]` calls); **zoom behaving like a switch**
  on a trackpad (precise scroll deltas scaled as if they were wheel notches). All three fixed in #160.
