/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
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

// Behaviour test for the base-game install-path diagnostic, driven by
// scripts/native-base-game-install-test.py.
//
// What is asserted, and the last one is the point of the test:
//
//   * the three ways the mount can fail -- nothing configured, a path that cannot be read, a
//     readable path with no archives in it -- are classified apart, and a successful mount reports
//     nothing at all;
//   * each failure's message names the path it is about and is distinct from the other two, since a
//     single "could not mount the base game" line would leave the reader to work out which repair
//     they need;
//   * the pre-fix shape reports *nothing* in a release build for any of the three. That is the
//     negative control: the silence is what cost a whole misdiagnosis (magenta backdrop and a
//     crash in a sky-drawing map, blamed on the renderer), and if the loud path regresses to it
//     the first assertions below fail while this one keeps passing.
//
// See docs/porting/base-game-install-path.md.

#include <stdio.h>
#include <string.h>

#include "Common/BaseGameInstallReport.h"

static int g_failures = 0;

static void check(bool condition, const char* what)
{
	printf("%s: %s\n", condition ? "ok" : "FAIL", what);
	if (!condition)
		++g_failures;
}

//-----------------------------------------------------------------------------------------------
// The two shapes of the archive file systems' RTS_ZEROHOUR block.
//-----------------------------------------------------------------------------------------------

// The shape it had: a debug-only assertion on the empty case, and no diagnostic of any kind once
// DEBUG_CRASHING is off. `out` receives what a release build would tell the player.
static void reportPreFix(const char* installPath, bool pathReadable, bool archivesMounted, char* out,
	size_t outSize)
{
	out[0] = 0;
	(void)pathReadable;
	(void)archivesMounted;
	if (installPath == nullptr || installPath[0] == 0)
	{
		// DEBUG_ASSERTCRASH(!installPath.isEmpty(), ("Be 1337! Go install Generals!")) compiles
		// to nothing here, so nothing is written.
		return;
	}
	// loadBigFilesFromDirectory()'s return value was discarded.
}

// The shape it has now.
static void reportNow(const char* installPath, bool pathReadable, bool archivesMounted, char* out,
	size_t outSize)
{
	const rts::BaseGameInstallStatus status = rts::classifyBaseGameInstall(installPath, pathReadable,
		archivesMounted);
	if (!rts::describeBaseGameInstall(status, installPath, out, outSize))
		out[0] = 0;
}

int main(void)
{
	const char* const basePath = "/depot/ZH_Generals";
	char message[512];
	char notConfigured[512];
	char pathMissing[512];
	char noArchives[512];

	// Classification: one status per repair.
	check(rts::classifyBaseGameInstall("", false, false) == rts::BASE_GAME_INSTALL_NOT_CONFIGURED,
		"an empty setting is NOT_CONFIGURED");
	check(rts::classifyBaseGameInstall(nullptr, false, false) == rts::BASE_GAME_INSTALL_NOT_CONFIGURED,
		"an absent setting is NOT_CONFIGURED");
	check(rts::classifyBaseGameInstall(basePath, false, false) == rts::BASE_GAME_INSTALL_PATH_MISSING,
		"a set path that cannot be read is PATH_MISSING");
	check(rts::classifyBaseGameInstall(basePath, true, false) == rts::BASE_GAME_INSTALL_NO_ARCHIVES,
		"a readable path with no archives is NO_ARCHIVES");
	check(rts::classifyBaseGameInstall(basePath, true, true) == rts::BASE_GAME_INSTALL_MOUNTED,
		"a mounted archive set is MOUNTED");

	// A successful mount says nothing, in either shape.
	reportNow(basePath, true, true, message, sizeof(message));
	check(message[0] == 0, "a successful mount reports nothing");

	// The three failures are audible, distinct, and name the path.
	reportNow("", false, false, notConfigured, sizeof(notConfigured));
	reportNow(basePath, false, false, pathMissing, sizeof(pathMissing));
	reportNow(basePath, true, false, noArchives, sizeof(noArchives));

	check(notConfigured[0] != 0, "an empty setting is reported");
	check(pathMissing[0] != 0, "an unreadable path is reported");
	check(noArchives[0] != 0, "a readable path with no archives is reported");
	check(strcmp(notConfigured, pathMissing) != 0 && strcmp(pathMissing, noArchives) != 0 &&
		strcmp(notConfigured, noArchives) != 0, "the three failures are three different messages");
	check(strstr(pathMissing, basePath) != nullptr && strstr(noArchives, basePath) != nullptr,
		"the messages about a set path quote that path");
	check(strstr(notConfigured, "InstallPath") != nullptr &&
		strstr(pathMissing, "InstallPath") != nullptr && strstr(noArchives, "InstallPath") != nullptr,
		"every message names the setting to repair");

	// The separator rule, which is what turned a real depot into "no archives": the mask is
	// concatenated onto the directory, so "/depot/ZH_Generals" + "*.big" searches /depot for
	// "ZH_Generals*.big". A value that already ends in a separator -- what the Windows registry
	// holds -- must be left exactly as it is.
	check(rts::baseGameInstallNeedsSeparator(basePath),
		"a directory without a trailing separator needs one");
	check(!rts::baseGameInstallNeedsSeparator("C:\\Program Files\\EA Games\\Generals\\"),
		"a Windows value ending in a backslash is left alone");
	check(!rts::baseGameInstallNeedsSeparator("/depot/ZH_Generals/"),
		"a value ending in a forward slash is left alone");
	check(!rts::baseGameInstallNeedsSeparator(""), "an empty setting needs no separator");
	check(!rts::baseGameInstallNeedsSeparator(nullptr), "an absent setting needs no separator");

	// Truncation: a short buffer still yields a terminated string rather than a walk off the end.
	char shortBuffer[24];
	memset(shortBuffer, 'x', sizeof(shortBuffer));
	reportNow(basePath, false, false, shortBuffer, sizeof(shortBuffer));
	check(shortBuffer[sizeof(shortBuffer) - 1] == 0, "a short buffer is terminated");
	check(shortBuffer[0] != 0, "a short buffer still carries the beginning of the message");

	// The negative control: the pre-fix shape is silent about all three.
	reportPreFix("", false, false, message, sizeof(message));
	check(message[0] == 0, "control: the pre-fix shape says nothing about an empty setting");
	reportPreFix(basePath, false, false, message, sizeof(message));
	check(message[0] == 0, "control: the pre-fix shape says nothing about an unreadable path");
	reportPreFix(basePath, true, false, message, sizeof(message));
	check(message[0] == 0, "control: the pre-fix shape says nothing about a path with no archives");

	printf("\n%d check(s) failed\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
