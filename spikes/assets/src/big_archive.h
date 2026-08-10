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

// Reader for the retail .big archive format.
//
// The read sequence deliberately mirrors StdBIGFileSystem::openArchiveFile
// (Core/GameEngineDevice/Source/StdDevice/Common/StdBIGFileSystem.cpp) field for field:
// same field order, same widths, same big-endian conversions via the engine's own
// Utility/endian_compat.h. The engine class itself cannot be linked standalone (it pulls in
// AsciiString, GameMemory, the DEBUG_ macros and the whole ArchiveFileSystem hierarchy), so
// this is a transcription of that algorithm, not the class itself.
#pragma once

#include "Lib/BaseTypeCore.h"

#include <cstdint>
#include <string>
#include <vector>

struct BigEntry
{
	UnsignedInt Offset = 0;
	UnsignedInt Size = 0;
	std::string Name; // full stored path, backslash-separated, as it appears on disk
};

struct BigArchive
{
	std::string Path;
	int64_t FileSizeOnDisk = 0;

	char Magic[5] = {};
	UnsignedInt HeaderSizeField = 0;     // second dword, little-endian in the format
	UnsignedInt EntryCount = 0;          // third dword, big-endian
	UnsignedInt FirstEntryOffsetField = 0; // fourth dword, big-endian
	std::vector<BigEntry> Entries;

	// Populated by validate().
	std::vector<std::string> Problems;
	size_t LongestNameLength = 0;
	int64_t HighestEntryEnd = 0;
	int64_t TotalEntryBytes = 0;
	int64_t OverlappingPairs = 0;
	int64_t BytesUnaccountedFor = 0;
	int64_t ZeroSizeEntries = 0;
	int64_t BackslashPathEntries = 0;
};

// Returns false and fills `error` if the file cannot be opened or is not a BIG archive.
bool read_big_archive(const char *path, BigArchive &out, std::string &error);

// Cross-checks the parsed table of contents against the file on disk. Fills out.Problems.
// Returns true when no problem was found.
bool validate_big_archive(BigArchive &archive);

// Copies one entry's bytes out of the archive into `dest_path`.
bool extract_big_entry(const BigArchive &archive, const BigEntry &entry, const char *dest_path,
                       std::string &error);
