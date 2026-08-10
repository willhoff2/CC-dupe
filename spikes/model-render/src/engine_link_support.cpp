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

// The definitions htree.cpp needs from translation units that will not compile or link
// natively yet. Each one is only reachable from an animation entry point this tool never
// calls -- it loads a hierarchy and poses it at the base transform. Anything that could be
// reached aborts instead of returning a plausible wrong answer.
//
// See docs/porting/native-model-render.md for what stopped each real translation unit.

#include "hanim.h"
#include "lookuptable.h"
#include "wwhack.h"
#include "ww3d.h"

#include <cstdio>
#include <cstdlib>

// ww3d.cpp defines the frame clock, but it includes dx8wrapper.h -> d3d8.h. Zero is the value
// ww3d.cpp itself starts from, and only the animation paths ever advance it.
unsigned int WW3D::SyncTime = 0;
unsigned int WW3D::PreviousSyncTime = 0;

// hanim.cpp compiles, but linking it drags in WWSaveLoad's persist factories and WWLib's
// Win32 critical sections. Only HTreeClass::Combo_Update reads these, which is animation
// blending.
namespace
{
[[noreturn]] void unreachable(const char *what)
{
	std::fprintf(stderr, "zh-model-render: %s was called; only the base pose is implemented\n",
	             what);
	std::abort();
}
} // namespace

HAnimClass *HAnimComboClass::Get_Motion(int) { unreachable("HAnimComboClass::Get_Motion"); }
HAnimClass *HAnimComboClass::Peek_Motion(int) { unreachable("HAnimComboClass::Peek_Motion"); }
float HAnimComboClass::Get_Frame(int) { unreachable("HAnimComboClass::Get_Frame"); }
float HAnimComboClass::Get_Weight(int) { unreachable("HAnimComboClass::Get_Weight"); }
PivotMapClass *HAnimComboClass::Get_Pivot_Weight_Map(int)
{
	unreachable("HAnimComboClass::Get_Pivot_Weight_Map");
}

// WWMath::Init() fills the fast trig tables this tool wants, but it also brings in
// Do_Force_Links() and the lookup-table manager, whose translation units pull WWSaveLoad's
// persist factories and WWLib's Win32 CriticalSectionClass into the link. Neither the curve
// classes nor the lookup tables are consulted when loading and posing a hierarchy: the force
// links exist only to keep MSVC from dropping the modules, and the lookup tables serve
// texture remap curves.
DECLARE_FORCE_LINK(curve)
DECLARE_FORCE_LINK(hermitespline)
DECLARE_FORCE_LINK(catmullromspline)
DECLARE_FORCE_LINK(cardinalspline)
DECLARE_FORCE_LINK(tcbspline)
void LookupTableMgrClass::Init() {}
void LookupTableMgrClass::Shutdown() {}
