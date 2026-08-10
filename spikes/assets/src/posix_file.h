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

// A minimal POSIX implementation of the engine's FileClass interface, so the engine's own
// chunkio.cpp can be driven natively without pulling in WWLib's Win32-bound RawFileClass.
#pragma once

#include "WWFILE.h"

#include <cstdio>
#include <string>

class PosixFileClass : public FileClass
{
public:
	explicit PosixFileClass(const char *filename);
	~PosixFileClass() override;

	const char *File_Name() const override { return Filename.c_str(); }
	const char *Set_Name(const char *filename) override;
	int Create() override { return 0; }
	int Delete() override { return 0; }
	bool Is_Available(int forced = false) override;
	bool Is_Open() const override { return Handle != nullptr; }
	int Open(const char *filename, int rights = READ) override;
	int Open(int rights = READ) override;
	int Read(void *buffer, int size) override;
	int Seek(int pos, int dir = SEEK_CUR) override;
	int Size() override;
	int Write(const void *buffer, int size) override;
	void Close() override;

private:
	std::string Filename;
	std::FILE *Handle = nullptr;
};
