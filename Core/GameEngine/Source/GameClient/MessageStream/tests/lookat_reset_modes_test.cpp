/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 TheSuperHackers
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// Scroll the camera, end the game the way ScriptActions::doVictory/doDefeat do, and ask whether
// the tactical view is still mouse-locked.
//
// WindowTranslator drops every message before the window manager sees it while
// TheTacticalView->isMouseLocked() and the in-game UI is not scrolling. LookAtTranslator locks the
// view when a scroll starts and only stopScrolling() unlocks it; doDisableInput() called
// resetModes(), which cleared m_isScrolling without stopScrolling(). A game that ended mid-scroll
// therefore reached the score screen locked, with InGameUI::reset() clearing the one exception --
// and no shell button answered a click. Measured on the Mac: docs/porting/game-end-mouse-lock.md.
//
// Real code throughout: LookAtTranslator, InGameUI (a subclass supplying its two pure virtuals),
// the engine's ViewDummy and MouseDummy, linked from the archives scripts/native-build.py builds.
// Scrolling is started through translateGameMessage() with the raw messages a player produces.
// Run through scripts/native-lookat-reset-modes-test.py.

#include "PreRTS.h"

#include "Common/CriticalSection.h"
#include "Common/GameMemory.h"
#include "Common/GlobalData.h"
#include "Common/MessageStream.h"
#include "Common/NameKeyGenerator.h"
#include "Common/PlayerList.h"
#include "Common/Science.h"
#include "GameLogic/RankInfo.h"
#include "GameClient/GameClient.h"
#include "GameClient/InGameUI.h"
#include "GameClient/KeyDefs.h"
#include "GameClient/LookAtXlat.h"
#include "GameClient/Mouse.h"
#include "GameClient/View.h"

#include <cstdio>

namespace
{

int Failures = 0;

void Check(const char *what, bool ok)
{
	if (!ok)
		Failures++;
	std::printf("%-72s %s\n", what, ok ? "ok" : "FAILED");
	std::fflush(stdout);
}

// InGameUI is abstract only in these two; everything under test is the real base class.
class HarnessInGameUI : public InGameUI
{
public:
	virtual void draw() override {}
	virtual View *createView(bool) override { return nullptr; }
};

// Only asked for its frame number, by View::isUserControlLocked() when a scroll breaks camera locks.
class HarnessGameClient : public GameClient
{
public:
	virtual void createRayEffectByTemplate(const Coord3D *, const Coord3D *, const ThingTemplate *) override {}
	virtual void addScorch(const Coord3D *, Real, Scorches) override {}
	virtual Drawable *friend_createDrawable(const ThingTemplate *, DrawableStatusBits) override { return nullptr; }
	virtual void setTeamColor(Int, Int, Int) override {}
	virtual void setTextureLOD(Int) override {}
	virtual void notifyTerrainObjectMoved(Object *) override {}
	virtual Display *createGameDisplay() override { return nullptr; }
	virtual InGameUI *createInGameUI() override { return nullptr; }
	virtual GameWindowManager *createWindowManager() override { return nullptr; }
	virtual FontLibrary *createFontLibrary() override { return nullptr; }
	virtual DisplayStringManager *createDisplayStringManager() override { return nullptr; }
	virtual VideoPlayerInterface *createVideoPlayer() override { return nullptr; }
	virtual TerrainVisual *createTerrainVisual() override { return nullptr; }
	virtual Keyboard *createKeyboard() override { return nullptr; }
	virtual Mouse *createMouse() override { return nullptr; }
	virtual SnowManager *createSnowManager() override { return nullptr; }
	virtual void setFrameRate(Real) override {}
};

// main()'s prologue in GeneralsMD/Code/Main/PlatformMain.cpp, in storage that is never destroyed.
ImmortalCriticalSection AsciiStringSection;
ImmortalCriticalSection UnicodeStringSection;
ImmortalCriticalSection DmaSection;
ImmortalCriticalSection MemoryPoolSection;
ImmortalCriticalSection DebugLogSection;

void send(LookAtTranslator &translator, GameMessage::Type type, Int key, Int state)
{
	GameMessage *message = newInstance(GameMessage)(type);
	message->appendIntegerArgument(key);
	message->appendIntegerArgument(state);
	translator.translateGameMessage(message);
	deleteInstance(message);
}

void sendRightButton(LookAtTranslator &translator, GameMessage::Type type)
{
	GameMessage *message = newInstance(GameMessage)(type);
	ICoord2D pixel = { 400, 300 };
	message->appendPixelArgument(pixel);
	translator.translateGameMessage(message);
	deleteInstance(message);
}

// ScriptActions::doDisableInput(), the two lines of it that reach this state; the rest of it
// (mouse visibility, selection, control bar tooltips) does not touch the scroll or the lock.
void disableInputAsTheGameEnds()
{
	TheInGameUI->setInputEnabled(false);
	TheLookAtTranslator->resetModes();
}

// What WindowTranslator::translateGameMessage asks before it lets a message reach the windows.
bool windowTranslatorDropsShellClicks()
{
	return TheTacticalView->isMouseLocked() && !TheInGameUI->isScrolling();
}

void endGameWhileScrolling(const char *how, void (*startScrolling)(LookAtTranslator &))
{
	std::printf("-- %s\n", how);
	TheInGameUI->setInputEnabled(true);
	startScrolling(*TheLookAtTranslator);
	Check("  scrolling locks the tactical view (precondition)",
		TheTacticalView->isMouseLocked() && TheInGameUI->isScrolling());

	disableInputAsTheGameEnds();
	Check("  disabling input stops the in-game UI scrolling", !TheInGameUI->isScrolling());
	Check("  the tactical view is unlocked after the end-of-game reset", !TheTacticalView->isMouseLocked());

	// InGameUI::reset() on the way to the shell clears the in-game UI's scrolling flag.
	TheInGameUI->setScrolling(FALSE);
	Check("  the score screen's clicks reach the window manager", !windowTranslatorDropsShellClicks());
}

void startKeyScroll(LookAtTranslator &translator)
{
	send(translator, GameMessage::MSG_RAW_KEY_DOWN, KEY_RIGHT, KEY_STATE_DOWN);
}

void startRightDragScroll(LookAtTranslator &translator)
{
	sendRightButton(translator, GameMessage::MSG_RAW_MOUSE_RIGHT_BUTTON_DOWN);
}

}	// namespace

int main()
{
	TheAsciiStringCriticalSection = AsciiStringSection.get();
	TheUnicodeStringCriticalSection = UnicodeStringSection.get();
	TheDmaCriticalSection = DmaSection.get();
	TheMemoryPoolCriticalSection = MemoryPoolSection.get();
	TheDebugLogCriticalSection = DebugLogSection.get();
	initMemoryManager();

	TheWritableGlobalData = NEW GlobalData;
	// GameMessage's constructor stamps the local player's index.
	TheNameKeyGenerator = NEW NameKeyGenerator;
	TheNameKeyGenerator->init();
	TheRankInfoStore = NEW RankInfoStore;
	TheRankInfoStore->init();
	TheScienceStore = NEW ScienceStore;
	TheScienceStore->init();
	ThePlayerList = NEW PlayerList;
	TheGameClient = NEW HarnessGameClient;
	TheMouse = NEW MouseDummy;
	TheTacticalView = NEW ViewDummy;
	TheInGameUI = NEW HarnessInGameUI;
	LookAtTranslator *translator = NEW LookAtTranslator;

	endGameWhileScrolling("arrow key held as the game ends", startKeyScroll);
	// The arrow key is released on the score screen; the shell ignores it, as LookAtTranslator does.
	send(*translator, GameMessage::MSG_RAW_KEY_UP, KEY_RIGHT, KEY_STATE_UP);
	endGameWhileScrolling("right-drag scroll held as the game ends", startRightDragScroll);
	sendRightButton(*translator, GameMessage::MSG_RAW_MOUSE_RIGHT_BUTTON_UP);

	// The fix must not stop scrolling from working in the next game.
	std::printf("-- scrolling after input is re-enabled\n");
	TheInGameUI->setInputEnabled(true);
	startKeyScroll(*translator);
	Check("  a new key scroll locks the view again", TheTacticalView->isMouseLocked());
	send(*translator, GameMessage::MSG_RAW_KEY_UP, KEY_RIGHT, KEY_STATE_UP);
	Check("  releasing the key unlocks it", !TheTacticalView->isMouseLocked());

	std::printf("%d failed checks\n", Failures);
	std::fflush(stdout);
	return Failures == 0 ? 0 : 1;
}
