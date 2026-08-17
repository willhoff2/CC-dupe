# The base game's archives: the install path, its silent failure, and #113/#114 on Metal

Zero Hour is not self-contained. `Win32BIGFileSystem::init()` (and its C++17 twin
`StdBIGFileSystem::init()`) mounts the current directory's archives unconditionally and then, under
`RTS_ZEROHOUR`, mounts the **base game's** archives from the `InstallPath` setting
(`platform_settings.cpp` prefixes the lookup with `STRING_`, so the file holds
`STRING_InstallPath`; the apparent name mismatch is not a defect).

Two assets the retail shell needs live only in the base game's archives:
`Art\Textures\mainmenubackdropuserinterface.tga` and `Art\W3D\new_skybox.W3D`. When that mount
fails, a debug build asserts ("Be 1337! Go install Generals!") and **a release build says nothing at
all**: the only symptoms are a magenta placeholder backdrop and a null `m_skyBox`. That silence
caused a full misdiagnosis — `real-input-menu-drive.md` §4.1 concluded the depot was missing
`new_skybox.W3D`, and it was not.

Everything below was measured on the machine in `macos-hardware-verification.md` §0 (Apple M1 Pro,
macOS 26.6.1, `backingScaleFactor` 2.00, MoltenVK 1.4.2, AppleClang), on
`961b95eb6` (`#114`) plus this slice.

## 1. The defect the retarget uncovered: the mask has no separator

The setting was pointing at a previous slice's throwaway run directory. Pointing it at the real
depot — `/Users/willhoff/devin-work/zh-data/ZH_Generals`, 15 `.big` archives, verified by name, not
assumed — did **not** mount them. The reason is one character:

```cpp
loadBigFilesFromDirectory(installPath, "*.big");   // directory + mask, concatenated
```

`loadBigFilesFromDirectory` concatenates the directory and the mask directly, so it searched the
depot's parent for `ZH_Generals*.big` and found nothing. The Windows registry value written by the
retail installer ends in a separator, so Windows never sees this; a hand-written setting on any
platform does. Both filesystem implementations now normalise a temporary mount path and leave the
value the user configured untouched:

```cpp
AsciiString mountPath = installPath;
if (rts::baseGameInstallNeedsSeparator(mountPath.str()))
  mountPath.concat('\\');
```

`rts::baseGameInstallNeedsSeparator` (`Core/GameEngine/Include/Common/BaseGameInstallReport.h`)
returns false for a value already ending in `\` or `/` — i.e. for every Windows registry value — so
the Windows path is byte-for-byte what it was. **Class: port defect**, fixed here.

## 2. The diagnostic: three distinct failures, none of them fatal

`reportBaseGameInstall` classifies the outcome and emits it outside any debug-only guard —
`DEBUG_LOG` plus, on non-Windows, `stderr`. The existing `DEBUG_ASSERTCRASH` is unchanged, and
nothing here aborts: Zero Hour's own archives still mount and the game still runs.

| Status | Cause | Message says |
|---|---|---|
| `BASE_GAME_INSTALL_NOT_CONFIGURED` | setting empty or absent | which setting and which file to write it in |
| `BASE_GAME_INSTALL_PATH_MISSING` | set, but `TheLocalFileSystem->doesFileExist()` says no | the path that does not resolve |
| `BASE_GAME_INSTALL_NO_ARCHIVES` | path resolves, `loadBigFilesFromDirectory` mounted nothing | that the directory holds no `.big` files |
| `BASE_GAME_INSTALL_MOUNTED` | success | *nothing* — a working install is silent |

The three failures are different repairs, which is why they are three messages rather than one.

`scripts/native-base-game-install-test.py` is the gate, wired into `native-port-ci.yml` and needing
no game data. It compiles and runs the focused test
(`Core/GameEngine/Source/Common/System/tests/base_game_install_report_test.cpp`) over the classifier,
the message text, buffer truncation, the silent-success case and the separator rule, then statically
asserts of **both** `Win32BIGFileSystem::init()` (the class `Win32GameEngine::createArchiveFileSystem()`
actually instantiates) and `StdBIGFileSystem::init()` that they call the reporter outside any
debug-only conditional, write to `stderr`, and pass a separator-terminated mount path. It strips
comments first, so a commented-out reporter call does not satisfy it — verified by mutation: with
the pre-fix shape restored the gate fails, and with the reporter commented out it fails.

### Do not symlink the two archive sets together

Combining the base game's 15 archives and Zero Hour's 20 in one directory is not an alternative
mount strategy: the sets share file names and `Data\INI\Weapon.ini` then fails to parse. That parse
error is the only crash record the previous session left on disk. Nothing in the repo recommends the
symlink; this paragraph exists so nothing starts to.

## 3. What the retarget bought, read out of the live process

Read with LLDB in the running native arm64 binary, base archives mounted:

```text
TheArchiveFileSystem->m_archiveFileMap                                        size = 35
doesFileExist("Art\\Textures\\mainmenubackdropuserinterface.tga", 0)         true
doesFileExist("Art\\W3D\\new_skybox.W3D", 0)                                 true
doesFileExist("Data\\INI\\Weapon.ini", 0)                                    true
TheWaterRenderObj->m_skyBox                                                  0x508000d20 (non-null)
```

35 = 15 base + 20 Zero Hour, mounted as two sets from two directories. So
`real-input-menu-drive.md` §4.1 is answered and its classification was wrong: the data was present
all along, the mount was broken. The null-skybox guard added by #115 is no longer exercised as a
null-pointer case; it stays, because it is the correct guard for an install that genuinely lacks the
asset, and Windows behaviour is unchanged where `m_skyBox` is non-null.

**Attaching** LLDB to an already-running process was refused throughout
(`error: attach failed (Not allowed to attach to process)`) even though the binary carries
`com.apple.security.get-task-allow`, because `DevToolsSecurity -status` reports developer mode
disabled. Launching the binary **under** LLDB works and answers the same questions; use that.

## 4. #113 and #114 on Metal, with numbers

Both landed with Linux/lavapipe evidence only. Under a validation layer proven loaded by the
`vulkan_manifests.py --require-layer` recipe (a `validation messages: 0` from a run that did not
print `validation layer: loaded` is worth nothing):

| Gate | Result on this GPU |
|---|---|
| `spikes/renderer/tools/macos-window-check.sh` | `SUMMARY: PASS`, real `NSWindow` + `CAMetalLayer`, `device: Apple M1 Pro` |
| `zh-hidpi-tests-cocoa --window --min-scale 2.0` | `9 checks, 0 failed`, backing scale 2.00 read off the panel, 100x80 points → **200x160 pixels**, coverage edge at x=100 one pixel wide |
| `check-hidpi-scale.py` (injected 2.00/1.00/1.25) | `35 checks, 0 failed` |
| `zh-resource-lock-tests --validation`, both swizzle modes | `0 case(s) failed, 0 skipped` |
| `zh-fixedfunc-tests --validation`, both swizzle modes | `0 case(s) failed, 6 pending` |
| `check-spike-render.py` | `0/480000 pixels differ by more than 2/255`, worst channel delta 1 |

The #114 case, on Metal, in both swizzle modes:

```text
PASS CopyRects A4R4G4B4   A4R4G4B4 host surface into an A4R4G4B4 texture: mean |delta| 0.000,
                          controls 117.2 (bytes reinterpreted) and 65.9 (flat)
```

`117.2` is the defect itself — the reinterpreting copy that drew every retail menu label twice — so
the residual is measuring something, and it is `0.000`.

The retail menu, rendered on this display, at the 800x600 default:

```text
Vulkan backend: backing scale 2.00, so a 800x600 point back buffer renders at 1600x1200 pixels
window #446  bounds=464,131 800x632 points   (632 = 600 client + title bar)
screenshot                 1600x1264 pixels
```

The colour target is **1600x1200 pixels, not 800x600 upscaled**, which is #113's claim on real
hardware. The main-menu frame shows the real backdrop art rather than the magenta placeholder, and
every label is drawn **once** — #114's claim on real hardware. Neither fix is wrong on Metal.

## 5. The re-driven shell, and what is still broken

`scripts/macos-input-drive.py` (#115) was re-run with real `CGEventPost` input and the base archives
mounted; Accessibility and Screen Recording are both granted
(`AXIsProcessTrusted`, `CGPreflightPostEventAccess`, `CGPreflightScreenCaptureAccess` all true).
Because LLDB owns the process for the readback above, the drive used the `post` subcommand against
the running pid rather than a second attach.

What changed: the main menu is correct art at full resolution, Solo Play and the USA campaign
difficulty shell navigate, and a campaign mission starts with a non-null skybox.

What did not: the campaign mission's frames are **heavily corrupted** — block artefacts, fragmented
or missing text, dark green/purple tiles — and no clean sky frame was obtained. The run also emits
thousands of the pre-existing `spike limit: more than 64 draws per frame` messages. This is not the
archive mount (the assets resolve, measured above) and not the skybox guard (`m_skyBox` is
non-null). **Class: unimplemented path / port defect in the 3D scene path, not this slice** — it
needs the draw-batching limit and the terrain/text passes, and it is scoped separately.

Screenshots are retail-derived and therefore deliberately **not** in the repo; they are attached to
the session that produced this document.

## 6. Classification summary

| Finding | Class |
|---|---|
| `loadBigFilesFromDirectory` given a directory without the mask separator | **port defect**, fixed here |
| Release build silent on an unmountable base install | **port defect** (diagnosability), fixed here |
| `STRING_InstallPath` pointing at a throwaway run directory | missing data / config, fixed here |
| `real-input-menu-drive.md` §4.1 "`new_skybox.W3D` is not in the depot" | wrong; superseded by §3 above |
| #113 half-resolution target on a scale-2 panel | port defect, fixed by #113, **confirmed on Metal** |
| #114 A4R4G4B4 `CopyRects` reinterpretation | port defect, fixed by #114, **confirmed on Metal** |
| Corrupted campaign mission frames on Metal | unimplemented path, out of scope, reported |
| LLDB attach refused with developer mode disabled | host configuration, worked around |

## 7. What a Mac still did not verify

The macOS numbers here are AppleClang's and do **not** go into
`docs/porting/ci-baselines/*.json`, which are the clang-14 Linux ratchet (decision 5 of
`decisions-resolved.md`). The Wine/VC6 Windows build and the retail replay gate did not run on this
host; they are the CI gates for this PR.
