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

// Prints the memory figures CPUDetectClass's static initialiser recorded, which is the
// GameLODManager's RAM test input. scripts/native-physical-memory-test.py compares them with the
// operating system's own; see docs/porting/physical-memory.md.

#include "cpudetect.h"

#include <stdio.h>

int main()
{
	printf("total physical memory: %llu\n",
		(unsigned long long)CPUDetectClass::Get_Total_Physical_Memory());
	printf("available physical memory: %llu\n",
		(unsigned long long)CPUDetectClass::Get_Available_Physical_Memory());
	return 0;
}
