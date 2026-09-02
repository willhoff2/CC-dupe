# The skirmish player-slot controls: what was empty, and what the chosen configuration reaches

`docs/porting/menu-path-probe.md` §3 (at `297baec6c`, 2026-08-16) reported that on the real
`SkirmishGameOptionsMenu` only one combo column rendered, its entries read `None`, the other columns
were blank, and opening a combo showed an empty list — so side, colour, team and AI difficulty could not
be chosen and "single-player skirmish works" rested on whatever defaults `TheSkirmishGameInfo` held.
The slice brief asked for the cause to be classified as MISSING DATA, a PORT DEFECT or an
UNIMPLEMENTED PATH (GameSpy excision) before anything was fixed.

**Classification: a PORT DEFECT in the renderer, not in the GUI — the 64-draw descriptor cap that #119
removed.** The skirmish screen issues 122 draws per frame; the probe's binary could issue 64. The
controls, their data and the state they write were intact the whole time. No GUI, data or network
code changes in this slice; the deliverable is the measurement, the reproduction of the original
symptom under the negative control, and the proof that a configuration chosen through the real
controls is the configuration the game starts with.

## 1. Ruling the three classes in or out

| Class | Checked | Result |
|---|---|---|
| MISSING DATA | `SkirmishGameOptionsMenu.wnd` parses; `PopulatePlayerTemplateComboBox` reads `ThePlayerTemplateStore` (retail `PlayerTemplate.ini`), `PopulateColorComboBox` reads `TheMultiplayerSettings->getColor(i)` (`Multiplayer.ini`), `PopulateTeamComboBox` reads `GUI:Team`/`GUI:None` from `generals.csf`; all resolve with the full 1.04 data set | **out** — every list is populated with the retail entries (China/GLA/USA + generals, the retail colour names, `None`/`1`..`4`) |
| PORT DEFECT (64-bit / path / wide-char / narrow field) | `GadgetComboBoxSetItemData`/`GetItemData` carry `Int` colour/template/team indices cast through `void *` (including `-1`), which round-trip on a 64-bit pointer without truncation; no real pointer travels as an item datum; the `WindowMsgData` widening (#107) already covers every pointer-carrying gadget message on this screen; `UpdateSlotList()` and `skirmishUpdateSlotList()` run and write `GameSlot` state | **out for the GUI** — but see §2: the symptom is a *renderer* port defect |
| UNIMPLEMENTED PATH (GameSpy excision) | `SkirmishGameOptionsMenuInit()` → `init()`/`clearSlotList()`/`reset()`/`enterGame()` → `InitSkirmishGameGadgets()` → `EnableSlotListUpdates(TRUE)` → `skirmishUpdateSlotList()`; none of it is behind the excised WOL/GameSpy code, and `GUIUtil.cpp`'s helpers are shared with the (excised) `WOLGameSetupMenu` but compiled and reached from the skirmish menu | **out** |

## 2. Reproducing the reported symptom on purpose

The renderer's draw accounting has a negative control (`draws-per-frame.md`): `ZH_RENDER_MAX_DRAWS=64`
refuses to grow past the old fixed allocation. The current strict-link binary, same data, same
`Registry.ini`, run twice to the skirmish screen at `-xres 800 -yres 600`:

| | Draws per frame on the skirmish screen | What is on screen |
|---|---|---|
| default | `requested 122 issued 122 dropped 0 (peak 123, capacity 256 in 1 block(s))` | `Players`, `Color`, `Army`, `Team` columns with eight rows each; slot 0 the local player, slot 1 an AI, slots 2-7 `Open`; every combo opens a populated list |
| `ZH_RENDER_MAX_DRAWS=64` | `draw resources exhausted at draw 64 (capacity 64): the frame will be missing geometry`, every frame | **exactly the §3 report**: one combo column (`Team`, entries `None`), the `Players`/`Color`/`Army` columns and their headers absent, the map preview and buttons present |

The screen's draw order puts the slot-list columns after the 64th draw, so the cap removed precisely
the controls the report said were missing, and left the ones it said were there. Nothing else differs
between the two runs. This is also why the original observer saw an empty list on opening a combo: the
drop-down's own draws are past the cap too.

The probe that recorded the symptom ran at `297baec6c`; the cap was removed at `c06bf1eaf` (#119,
2026-08-17), and the "empty controls" note was never re-measured afterwards. It is stale, not wrong.

## 3. Proof that a chosen configuration takes effect

The requirement was a *chosen* non-default side, colour and AI difficulty, selected through the real
controls, and the resulting `TheSkirmishGameInfo` and players. Done with real X pointer events on the
strict-link binary (this branch), retail 1.04 data, `-xres 800 -yres 600`, reading engine state under
gdb (`AsciiString`/`UnicodeString` storage read directly; `ThePlayerList` walked by index).

Choices made through the combo boxes (each opened by a click on its arrow, the entry chosen by a click
on its row):

| Slot | Control | Default | Chosen |
|---|---|---|---|
| 0 (local) | Army | `Random` | **China** |
| 0 (local) | Color | `???` (random) | **Purple** |
| 1 | Players | `Open` | **Hard Army** (`SLOT_BRUTAL_AI`) |
| 1 | Army | `Random` | **GLA** |
| 1 | Color | `???` | **Red** |

`TheSkirmishGameInfo` after the choices, before `Play Game`:

```text
TheSkirmishGameInfo map = 'maps\alpine assault\alpine assault.map'
  slot 0 state=SLOT_PLAYER    color=6 template=3 team=-1 startPos=-1 name='devin-box'
  slot 1 state=SLOT_BRUTAL_AI color=1 template=4 team=-1 startPos=-1 name='Hard Army'
```

`template=3` is `China` and `template=4` is `GLA` in the retail `PlayerTemplate.ini` order the combo
was populated from; `color=6` is `Purple` and `color=1` is `Red` in `Multiplayer.ini`. The choices are
also persisted to `SkirmishPreferences.ini` in the user-data directory
(`S=Hdevin-box,0,0,TT,6,3,-1,1,1:CH,1,4,-1,-1:O:O:O:O:O:O:`), which is how the next launch comes up
showing them.

After `Play Game`, in the running game:

```text
TheGameLogic->getGameMode() = GAME_SKIRMISH
TheSkirmishGameInfo
  slot 0 state=SLOT_PLAYER    color=6 template=3 team=1  startPos=1 name='devin-box'
  slot 1 state=SLOT_BRUTAL_AI color=1 template=4 team=-1 startPos=0 name='Hard Army'
ThePlayerList playerCount = 5
  player 2 side='China' template='China' type=PLAYER_HUMAN    color=0xff9600c8 displayName='devin-box' aiDifficulty=-
  player 3 side='GLA'   template='GLA'   type=PLAYER_COMPUTER color=0xffff0000 displayName='Hard Army' aiDifficulty=DIFFICULTY_HARD
```

(players 0, 1 and 4 are the neutral, `Civilian` and `Observer` players the map always carries.)

- Side/faction: slot templates 3/4 became players whose `side` and template are `China`/`GLA`.
- Colour: `0xff9600c8` is `Multiplayer.ini` colour 6 (Purple, RGB 150/0/200) and `0xffff0000` colour
  1 (Red), i.e. `Player::m_color` was set from the slot's colour index through `TheMultiplayerSettings`.
- AI difficulty: `SLOT_BRUTAL_AI` reached `GameLogic::startNewGame()`'s
  `TheKey_skirmishDifficulty = DIFFICULTY_HARD` and the AI player's `getAIDifficulty()` reads
  `DIFFICULTY_HARD`; the human player has no `AIPlayer` (`-`).
- Team: `team=-1` (`None`) for the AI is what was chosen; slot 0's `team=1` came from the persisted
  preferences (the `Team` combo for slot 0 showed `2`, which is 0-based team 1). No team was changed
  through the control in this run, so team selection is not among the proved choices.

**Answer to `startup-to-mission-start.md` §10's open question:** the WND controls produce the same
`TheSkirmishGameInfo` shape the §3 headless harness set by hand (`GameSlot` state + colour + template,
`enterGame()`, `startGame()`), and go further than the harness did — the harness used `SLOT_EASY_AI`
and default colours; the GUI-driven run reaches `ThePlayerList` with the chosen side, colour and
difficulty. The headless skirmish evidence is therefore not weaker than it looked.

## 4. What is not proved here

- Team selection through the `Team` combo (see above). The control populates and the persisted value
  reaches the slot, but no change was made through it in this run.
- Start position (the map-preview click), starting cash, game speed and the superweapon checkbox.
- The above on macOS/Apple Silicon: the mechanism is the shared GUI path and the same renderer cap
  #119 measured there, but this run is Linux x86-64.
- The `Open` slots' `Color` reads `???` — that is the retail random-colour label (`GUI:???`), not a
  missing string.

## 5. Consequence for the documents

`menu-path-probe.md` §3's paragraph and §7 item 3 are marked superseded, pointing here. No code is
changed by this slice; the "fix" was #119, and this note is the measurement that ties the symptom to
it.
