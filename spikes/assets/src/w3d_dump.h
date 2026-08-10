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

#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct W3dChunk
{
	uint32_t Id = 0;
	uint32_t Size = 0;       // payload size, MSB masked off
	bool HasSubChunks = false;
	int Depth = 0;
	int64_t HeaderOffset = 0;
	std::string Name;        // from the engine's own w3d_file.h enum, or "" if unknown
};

struct W3dDump
{
	std::string Path;
	int64_t FileSize = 0;
	std::vector<W3dChunk> Chunks;

	// Cross-checks.
	int64_t TopLevelBytes = 0;   // sum of (payload + 8) over depth-0 chunks
	int64_t UnknownIds = 0;
	int64_t SizeMismatches = 0;  // container chunks whose children do not fill them exactly
	std::vector<std::string> Problems;
	bool TruncatedEarly = false;
};

// Walks the chunk tree with the engine's ChunkLoadClass. `max_chunks` bounds runaway parses of
// a file being read with the wrong header size.
bool dump_w3d(const char *path, W3dDump &out, std::string &error, size_t max_chunks = 200000);

bool validate_w3d_dump(W3dDump &dump);
