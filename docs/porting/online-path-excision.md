# Excising the GameSpy online path off Windows

GameSpy matchmaking and multiplayer are cut scope for this port: campaign and skirmish only. The
[online-absent menu seam](online-absent-menu-seam.md) made the single-player menu path compile with
the online path absent, and left behind exactly the question it said it would: a native build with
**81 unresolved GameSpy SDK symbols** and three online units that still failed to compile. That is
what this slice finishes. (81 was that slice's levels 1-3 figure; the levels 1-4 strict link this
slice is measured against counts 82.)

The SDK is not missing. `cmake/gamespy.cmake` fetches
[TheSuperHackers/GamespySDK](https://github.com/TheSuperHackers/GamespySDK), which defines every one
of those symbols, and until this slice `Core/GameEngine` linked it unconditionally. So the 82
unresolved symbols in the strict link were a link **refused**, and the only correct way for them to
go to zero is for nothing off Windows to reference the SDK any more. Linking it would have re-enabled
cut scope; stubbing it would have written an online client that always fails. Neither is here: **no
GameSpy function is defined by this diff**, on any platform.

Measured, `./scripts/ci/fetch-probe-deps.sh` then
`CLANGXX=clang++-14 python3 scripts/native-build.py --level 1 --level 2 --level 3 --level 4
--with-shims --strict-link`:

| | Before (main at `ee059ee3d`) | After |
| --- | ---: | ---: |
| objects | 969 / 972 | **972 / 972** |
| compile failures | 3 (all three these) | **0** |
| unresolved symbols (strict link) | 173 | **73** |
| — `cut-scope-not-linked` (GameSpy SDK) | 82 | **0** |
| — `compile-blocked` | 18 | **0** |
| — `no-definition-anywhere` | 22 | **22** (unchanged; slice 3's) |
| — `library-not-linked` / `harness-artefact` | 42 / 9 | 42 / 9 |

`Core/GameEngine` goes from 207/210 to **210/210** objects, and with slice 1's `dx8wrapper.cpp` fix
already on main these were the last three compile failures in the level 1-4 build: **every
translation unit the harness builds now compiles**, and both piles that a compile failure feeds —
`compile-blocked` and `cut-scope-not-linked` — are empty. The strict link still fails and still
produces no executable, for the 73 symbols that are other slices': 42 `library-not-linked`, 9
`harness-artefact` and the 22 `no-definition-anywhere` slice 3 owns.

(An earlier revision of this branch measured 971/972 and 150 unresolved against main at `6df9b180a`,
before `dx8wrapper.cpp` compiled. Those figures do not transfer; the table above is a re-measurement
after rebasing.)

## The mechanism: one build definition, `RTS_HAS_GAMESPY`

`Core/GameEngine/CMakeLists.txt` links `gamespy::gamespy` and defines `RTS_HAS_GAMESPY` **on Windows
only**. Off Windows the SDK's include directories stay on the include path — engine headers such as
`PeerDefs.h` declare their own types in terms of `PEER` and `GPProfile`, and rewriting the engine's
own data model is not this slice — but no SDK code is linked and, after this diff, none is called.

Two guard spellings appear, and the distinction is deliberate:

- **`RTS_HAS_GAMESPY`** guards *use of the SDK*. It is what makes the 82 symbols go away.
- **`_WIN32`** guards *Win32 platform surfaces* that have no portable equivalent (Windows SNMP, the
  `icmp.dll` ping). Those would still be Windows-only if the SDK were present.

Windows compiles every line it compiled before: `RTS_HAS_GAMESPY` is always defined there, and every
`_WIN32` branch is the pre-existing code unchanged.

## Per-file decision: shim or excise

The three compile failures, with the answer and the reason. Two are excisions and one is a shim,
which is the split the diagnostics predicted.

| Unit | First diagnostic | Decision | Why |
| --- | --- | --- | --- |
| `GameSpy/MainMenuUtils.cpp` | `unknown type name 'HANDLE'` | **shim, then excise the servserv client** | Mixed file. Its write-access probe is CRT (`_open`/`_close`/`_O_CREAT`/`_S_IREAD`) and gets the existing `Utility/path_compat.h` spellings — that part is portable and should compile. Everything else in it is GameSpy's HTTP SDK talking to `servserv.generals.ea.com`: patch check, MOTD, config fetch, online player counts, and the `CreateThread`-based async DNS lookup that exists only to reach that host. That is the online path, and it is excised, entry points kept (below). |
| `GameSpy/StagingRoomGameInfo.cpp` | `unknown type name 'AsnObjectIdentifier'` | **excise the SNMP function; keep the file** | The type comes from the Windows SNMP SDK, and the hint is right: `GetLocalChatConnectionAddress()` loads `inetmib1.dll` and walks the MIB-II TCP connection table to find which local address the **GameSpy chat connection** is using, so a hosted game can advertise it. Both halves — an SNMP agent and a chat connection — exist only when online does. There is no portable SNMP agent to walk, and if NAT discovery is ever wanted it is UPnP/NAT-PMP, a new implementation. Compiled on Windows only, with no fallback definition; the caller audit below is what justifies that. The other 90% of the file is the staging-room `GameInfo` the menus use, and builds either way. |
| `GameSpy/Thread/PingThread.cpp` | `unknown type name 'HOSTENT'` | **shim, then excise the ICMP round trip** | `HOSTENT` came from a direct `<winsock.h>` include; that is the established seam's job and the file now includes `Utility/socket_compat.h`, so the thread, its request queue and its hostname resolution are portable and *run*. Only `doPing()` is Win32: `IcmpCreateFile`/`IcmpSendEcho`, hand-declared because they never had a public header, from `icmp.dll` loaded at run time. Off Windows it returns `-1`, which is the value this code already returns on a Windows box where `LoadLibrary("ICMP.DLL")` fails, and which every caller already handles as "no ping". |

### Caller audit for `GetLocalChatConnectionAddress()`

Excising a function with no fallback definition is only safe if nothing outside the excised path can
reach it, so this is the search rather than an inference. `grep -rn GetLocalChatConnectionAddress`
over the whole tree (excluding `.git`) returns, in code:

| Site | Reaches the excised definition? |
| --- | --- |
| `Core/.../GameSpy/PeerDefs.h:285` | declaration only |
| `Core/.../GameSpy/StagingRoomGameInfo.cpp:119` | the definition itself, now `_WIN32`-only |
| `Core/.../GameSpy/Thread/PeerThread.cpp:2292` | **yes — the only live call in any build.** It is inside the peer SDK's connect/room callback path, which `RTS_HAS_GAMESPY` compiles out, so off Windows the call disappears with the callee |
| `Core/.../GameSpy/Thread/PeerThread.cpp:1412` | commented out |
| `Generals/.../GameNetwork/GameSpyGameInfo.{h,cpp}` and `GeneralsMD/.../GameNetwork/GameSpyGameInfo.{h,cpp}` | no. Each declares *and defines its own* copy (`GameSpyGameInfo.cpp:97`, SNMP and all) and calls that local copy at line 443, so neither binds to the Core definition — and both files are commented out of `CMakeLists.txt` in Generals, GeneralsMD and Core (`# unused`), so they are built on no platform |
| `Generals/.../GameNetwork/GameSpy.cpp:964` | no. Same story: the file is commented out of `Generals/Code/GameEngine/CMakeLists.txt:1017`, and it includes `GameSpyGameInfo.h`, so it would bind to that per-game copy |

So on Windows nothing changes, and off Windows there is exactly one caller and it is compiled out by
the same guard as the callee. **Finding for a later slice:** those two per-game files carry an
unguarded duplicate of the SNMP code. They are dead in every configuration today, so this slice does
not touch them; whoever revives them owns giving them the same treatment.

The three biggest SDK consumers in the strict link needed the same treatment, since compiling the
three files above without them would only have moved their symbols from `compile-blocked` into
`cut-scope-not-linked`:

| Unit | Symbols | What is compiled out off Windows | What stays |
| --- | ---: | --- | --- |
| `Thread/PeerThread.cpp` | 47 | the peer / server-browser / QR2 client: chat, rooms, staging rooms, hosting, and the thread body that services them | the message queue the menus hold, and the local stat bookkeeping — this unit is one of the five the seam gate requires to compile |
| `Thread/BuddyThread.cpp` | 18 | the GameSpy Presence connection and its callbacks | the buddy message queue the menus own |
| `Thread/PersistentStorageThread.cpp` | 15 | the stats/persistence backend: connect, authenticate, read and write a player's persisted stats | the queue, and the `PSPlayerStats` parsing single-player-facing code reads |

## Why the empty thread bodies are not stubs

Off Windows, `PeerThreadClass::Thread_Function()`, `BuddyThreadClass::Thread_Function()` and
`PSThreadClass::Thread_Function()` have empty bodies. That is a structural requirement, not a way to
make a count fall:

- They are overrides of the engine's own `ThreadClass`, not GameSpy API. Nothing in this diff defines
  a name the SDK owns.
- Their translation units **must** build: the menus hold the message queues declared in them, and
  `PeerThread.cpp` is one of the units `check-online-absent-seam.py` requires by name. A class whose
  pure-virtual worker is undefined does not link, so the choice is an empty worker or deleting the
  queue the single-player menus use.
- An empty worker is the honest behaviour: a thread with no network backend has nothing to service.
  The queues never report a connection, which is a state the menus already handle because it is what
  they see on Windows before login and after a dropped connection.

## What a single-player player sees

Nothing on the campaign or skirmish path changes, and no single-player behaviour was removed to make
a number fall. The online-facing differences off Windows, all of them states this code already
reaches on Windows when the network is unavailable:

- **Online / Multiplayer button.** `StartPatchCheck()` takes its own `LOOKUP_FAILED` path:
  `cantConnectBeforeOnline = TRUE`, then `startOnline()`, which is the existing "cannot connect to
  servserv" message box. No patch is queued, the download menu is not raised, and no login screen
  appears. The button is not hidden or disabled — the game says it cannot connect, truthfully.
- **Main-menu online player counts and overall stats.** Not fetched, because fetching them is an
  HTTP request to servserv. The labels keep whatever they had; nothing is faked.
- **Patch download.** `WWDownload` and `DownloadManager` are built and portable
  ([ww3d2-and-download-headers.md](ww3d2-and-download-headers.md)); what is absent is the servserv
  patch *check* that would tell them what to fetch. `check-download-seam.py` still gates the
  downloader and its consumers as unchanged.
- **The `/host` chat debug command** (`InGameChat.cpp`, `WOLLobbyMenu.cpp`, `WOLGameSetupMenu.cpp`,
  both games) reports `thread:` without the `qr2:` field, because `getQR2HostingStatus()` is inside
  the SDK. The command still works; only the QR2 half of one debug line is gone. Windows prints both
  fields as before.

## Findings for other slices

- **`no-definition-anywhere` is still 22.** Unlike slice 1, which took that pile from 9 to 22 by
  making a file compile, compiling these three exposed no call to anything nothing defines: the code
  they newly contribute to the link is either portable engine code or absent. Those 22 remain slice
  3's; nothing here implements or hides them.
- **`dx8wrapper.cpp`** is slice 1's and landed in #79 before this rebase. Not touched.
- **The link configuration and harness** stay slice 4's. The only build change here is the Windows/
  non-Windows split of the GameSpy library in `Core/GameEngine/CMakeLists.txt`, which is this seam.

## Gates

`scripts/ci/check-online-absent-seam.py` already asserted both halves of this seam, and its baseline
tightens rather than widens: the per-symbol GameSpy budget in
`docs/porting/ci-baselines/online-absent-seam.json` goes from **81 symbols to 0**, so any new SDK
reference anywhere in the engine now fails CI naming the symbol, and the five menu-path units are
still required by name.

```sh
python3 scripts/ci/check-online-absent-seam.py --results /tmp/l4.json
python3 scripts/ci/check-download-seam.py     --results /tmp/l4.json
python3 scripts/ci/check-native-build-baseline.py --results /tmp/l4.json
```

Windows is verified by the Wine/VC6 build and the retail replay check
(`.agents/skills/windows-build-and-replays`), not by inspection: every non-Windows branch this slice
adds is unreachable there, but the claim is only worth what the build says.
