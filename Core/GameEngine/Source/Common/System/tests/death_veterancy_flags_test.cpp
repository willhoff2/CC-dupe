/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 TheSuperHackers
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

/***********************************************************************************************
 *                                                                                             *
 *  The bit assignment of DeathTypeFlags and VeterancyLevelFlags, pinned for the zero-valued    *
 *  enumerators DEATH_NORMAL and LEVEL_REGULAR.                                                 *
 *                                                                                             *
 *  WHY THIS EXISTS. Both flag sets put DeathType dt on bit (dt - 1). For DEATH_NORMAL (0) and  *
 *  LEVEL_REGULAR (0) that shift count is -1, which is undefined behaviour. The 32-bit retail   *
 *  build got away with it: `unsigned long` is 32 bits there, the hardware truncates the shift  *
 *  count to 5 bits, and the flag lands on bit 31 -- which DEATH_TYPE_FLAGS_ALL (0xffffffff)    *
 *  has set, so a normal death passes DieMuxData::isDieApplicable's gate. On LP64 the count     *
 *  truncates to 6 bits instead, `1UL << 63` has no bit in a 32-bit mask, and the gate rejects  *
 *  EVERY normal death: no die module body runs, nothing is destroyed, and corpses keep a live  *
 *  AI (docs/porting/death-flag-shift.md).                                                      *
 *                                                                                             *
 *  WHAT THIS MEASURES. Windows is the oracle -- this code is in the determinism path and the   *
 *  replay gate compares against it -- so the expectations below are the 32-bit assignment      *
 *  written out literally, not a tidier one:                                                    *
 *                                                                                             *
 *    * DEATH_NORMAL / LEVEL_REGULAR are bit 31, every other enumerator e is bit (e - 1);       *
 *    * the DEATH_TYPE_FLAGS_ALL / VETERANCY_LEVEL_FLAGS_ALL defaults DieMuxData starts from    *
 *      accept every enumerator, and the _NONE defaults accept none;                            *
 *    * get/set/clear agree with each other and no two enumerators share a bit, which is what   *
 *      makes the INI parsers' `+TYPE` / `-TYPE` tokens mean what they say.                     *
 *                                                                                             *
 *  On 32-bit Windows this file passed before the fix as well; that is the point of it. It      *
 *  fails on LP64 before the fix, at the two DEATH_NORMAL / LEVEL_REGULAR checks and at the     *
 *  round trips that go through them.                                                           *
 *                                                                                             *
 *  Run through scripts/native-death-veterancy-flags-test.py.                                   *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#define DEFINE_DEATH_NAMES

#include "Common/GameCommon.h"
#include "GameLogic/Damage.h"

#include <stdio.h>

static int _Failures = 0;
static int _Checks = 0;

static void Check(bool condition, const char * what)
{
	_Checks++;
	if (!condition) {
		_Failures++;
		printf("FAIL: %s\n", what);
	}
}

static void Check_Equal(UnsignedInt actual, UnsignedInt expected, const char * what)
{
	_Checks++;
	if (actual != expected) {
		_Failures++;
		printf("FAIL: %s (got 0x%08x, expected 0x%08x)\n", what, actual, expected);
	}
}

// The bit the 32-bit retail build assigns, written out without a shift by -1: the truncation the
// hardware performs there, done in the test's own terms so it is an expectation and not a copy of
// the implementation.
static UnsignedInt Retail_Bit(Int enumerator)
{
	return (enumerator == 0) ? 0x80000000u : (1u << (enumerator - 1));
}


/***********************************************************************************************
 *  DeathType.                                                                                  *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
static void Test_Death_Types()
{
	// The defect, stated once and directly: a normal death must pass the default mask, which is
	// what DieMuxData's constructor and INI::parseDeathTypeFlags both start from.
	Check(getDeathTypeFlag(DEATH_TYPE_FLAGS_ALL, DEATH_NORMAL),
		"DEATH_TYPE_FLAGS_ALL accepts DEATH_NORMAL");
	Check_Equal(setDeathTypeFlag(DEATH_TYPE_FLAGS_NONE, DEATH_NORMAL), 0x80000000u,
		"DEATH_NORMAL is bit 31, as it is on 32-bit Windows");

	UnsignedInt seen = 0;
	for (Int type = 0; type < DEATH_NUM_TYPES; type++) {
		const DeathType death = (DeathType)type;
		const UnsignedInt bit = setDeathTypeFlag(DEATH_TYPE_FLAGS_NONE, death);
		char label[128];
		snprintf(label, sizeof(label), "DeathType %s has the bit 32-bit Windows gives it",
			TheDeathNames[type]);

		Check_Equal(bit, Retail_Bit(type), label);
		Check(getDeathTypeFlag(DEATH_TYPE_FLAGS_ALL, death),
			"DEATH_TYPE_FLAGS_ALL accepts every death type");
		Check(!getDeathTypeFlag(DEATH_TYPE_FLAGS_NONE, death),
			"DEATH_TYPE_FLAGS_NONE accepts no death type");
		Check(getDeathTypeFlag(bit, death), "set then get round trips");
		Check(!getDeathTypeFlag(clearDeathTypeFlag(DEATH_TYPE_FLAGS_ALL, death), death),
			"clear then get round trips");
		// Distinct bits are what makes the INI parsers' +TYPE / -TYPE tokens independent.
		Check((seen & bit) == 0, "no two death types share a bit");
		seen |= bit;
	}

	// The INI case this is really about: a die module written `DeathTypes = ALL -CRUSHED` must
	// still run for a normal death.
	Check(getDeathTypeFlag(clearDeathTypeFlag(DEATH_TYPE_FLAGS_ALL, DEATH_CRUSHED), DEATH_NORMAL),
		"clearing DEATH_CRUSHED leaves DEATH_NORMAL set");
}


/***********************************************************************************************
 *  VeterancyLevel. Same shift, same zero-valued first enumerator.                              *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
static void Test_Veterancy_Levels()
{
	Check(getVeterancyLevelFlag(VETERANCY_LEVEL_FLAGS_ALL, LEVEL_REGULAR),
		"VETERANCY_LEVEL_FLAGS_ALL accepts LEVEL_REGULAR");
	Check_Equal(setVeterancyLevelFlag(VETERANCY_LEVEL_FLAGS_NONE, LEVEL_REGULAR), 0x80000000u,
		"LEVEL_REGULAR is bit 31, as it is on 32-bit Windows");

	UnsignedInt seen = 0;
	for (Int level = LEVEL_FIRST; level <= LEVEL_LAST; level++) {
		const VeterancyLevel veterancy = (VeterancyLevel)level;
		const UnsignedInt bit = setVeterancyLevelFlag(VETERANCY_LEVEL_FLAGS_NONE, veterancy);
		// TheVeterancyNames is defined in GameCommon.cpp, which this test does not link.
		char label[128];
		snprintf(label, sizeof(label), "VeterancyLevel %d has the bit 32-bit Windows gives it",
			level);

		Check_Equal(bit, Retail_Bit(level), label);
		Check(getVeterancyLevelFlag(VETERANCY_LEVEL_FLAGS_ALL, veterancy),
			"VETERANCY_LEVEL_FLAGS_ALL accepts every level");
		Check(!getVeterancyLevelFlag(VETERANCY_LEVEL_FLAGS_NONE, veterancy),
			"VETERANCY_LEVEL_FLAGS_NONE accepts no level");
		Check(getVeterancyLevelFlag(bit, veterancy), "set then get round trips");
		Check(!getVeterancyLevelFlag(clearVeterancyLevelFlag(VETERANCY_LEVEL_FLAGS_ALL, veterancy),
			veterancy), "clear then get round trips");
		Check((seen & bit) == 0, "no two veterancy levels share a bit");
		seen |= bit;
	}
}


/***********************************************************************************************
 *  The width the mask is stored in. `unsigned long` is 32 bits on Windows and 64 on LP64, which *
 *  is the whole reason the two platforms disagreed; the flags themselves are 32 bits on both.   *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
static void Test_Flag_Width()
{
	Check_Equal((UnsignedInt)sizeof(DeathTypeFlags), 4u, "DeathTypeFlags is 32 bits");
	Check_Equal((UnsignedInt)sizeof(VeterancyLevelFlags), 4u, "VeterancyLevelFlags is 32 bits");
	Check_Equal(DEATH_TYPE_FLAGS_ALL, 0xffffffffu, "DEATH_TYPE_FLAGS_ALL is every bit");
	Check_Equal(VETERANCY_LEVEL_FLAGS_ALL, 0xffffffffu, "VETERANCY_LEVEL_FLAGS_ALL is every bit");
}


int main()
{
	Test_Death_Types();
	Test_Veterancy_Levels();
	Test_Flag_Width();

	printf("%d checks, %d failure(s)\n", _Checks, _Failures);
	return (_Failures == 0) ? 0 : 1;
}
