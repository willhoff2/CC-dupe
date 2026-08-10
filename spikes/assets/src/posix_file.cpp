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

#include "posix_file.h"

#include <sys/stat.h>

PosixFileClass::PosixFileClass(const char *filename) : Filename(filename ? filename : "")
{
}

PosixFileClass::~PosixFileClass()
{
	Close();
}

const char *PosixFileClass::Set_Name(const char *filename)
{
	Filename = filename ? filename : "";
	return Filename.c_str();
}

bool PosixFileClass::Is_Available(int)
{
	struct stat st;
	return stat(Filename.c_str(), &st) == 0;
}

int PosixFileClass::Open(const char *filename, int rights)
{
	Set_Name(filename);
	return Open(rights);
}

int PosixFileClass::Open(int rights)
{
	Close();
	Handle = std::fopen(Filename.c_str(), (rights & WRITE) ? "w+b" : "rb");
	return Handle != nullptr;
}

int PosixFileClass::Read(void *buffer, int size)
{
	if (Handle == nullptr || size <= 0) {
		return 0;
	}
	return static_cast<int>(std::fread(buffer, 1, static_cast<size_t>(size), Handle));
}

int PosixFileClass::Seek(int pos, int dir)
{
	if (Handle == nullptr) {
		return -1;
	}
	if (std::fseek(Handle, pos, dir) != 0) {
		return -1;
	}
	return static_cast<int>(std::ftell(Handle));
}

int PosixFileClass::Size()
{
	if (Handle == nullptr) {
		return 0;
	}
	const long here = std::ftell(Handle);
	std::fseek(Handle, 0, SEEK_END);
	const long end = std::ftell(Handle);
	std::fseek(Handle, here, SEEK_SET);
	return static_cast<int>(end);
}

int PosixFileClass::Write(const void *buffer, int size)
{
	if (Handle == nullptr || size <= 0) {
		return 0;
	}
	return static_cast<int>(std::fwrite(buffer, 1, static_cast<size_t>(size), Handle));
}

void PosixFileClass::Close()
{
	if (Handle != nullptr) {
		std::fclose(Handle);
		Handle = nullptr;
	}
}
