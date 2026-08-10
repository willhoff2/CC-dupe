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

// Walks a .w3d file using the engine's own ChunkLoadClass, compiled natively at 64-bit.
// Nothing here reimplements the chunk format: the parse is whatever chunkio.cpp does.

#include "w3d_dump.h"

#include "chunk_names.h"
#include "posix_file.h"

#include "chunkio.h"

#include <cstdio>
#include <sys/stat.h>

namespace
{

// The on-disk chunk header is two 32-bit fields. If sizeof(ChunkHeader) is anything else, the
// engine's reads are misaligned with the file and every id/size below is meaningless.
constexpr size_t ON_DISK_CHUNK_HEADER_BYTES = 8;

void walk(ChunkLoadClass &loader, W3dDump &out, int depth, size_t max_chunks)
{
	while (loader.Open_Chunk()) {
		if (out.Chunks.size() >= max_chunks) {
			out.TruncatedEarly = true;
			loader.Close_Chunk();
			return;
		}

		W3dChunk chunk;
		chunk.Id = static_cast<uint32_t>(loader.Cur_Chunk_ID());
		chunk.Size = static_cast<uint32_t>(loader.Cur_Chunk_Length());
		chunk.HasSubChunks = loader.Contains_Chunks() != 0;
		chunk.Depth = depth;
		chunk.Name = w3d_chunk_name(chunk.Id);
		out.Chunks.push_back(chunk);

		if (chunk.HasSubChunks) {
			walk(loader, out, depth + 1, max_chunks);
		}

		loader.Close_Chunk();
		if (out.TruncatedEarly) {
			return;
		}
	}
}

} // namespace

bool dump_w3d(const char *path, W3dDump &out, std::string &error, size_t max_chunks)
{
	struct stat st;
	if (stat(path, &st) != 0) {
		error = std::string("cannot stat ") + path;
		return false;
	}
	out.Path = path;
	out.FileSize = static_cast<int64_t>(st.st_size);

	PosixFileClass file(path);
	if (!file.Open(FileClass::READ)) {
		error = std::string("cannot open ") + path;
		return false;
	}

	ChunkLoadClass loader(&file);
	walk(loader, out, 0, max_chunks);
	file.Close();

	if (out.Chunks.empty()) {
		error = "no chunks were read";
		return false;
	}
	return true;
}

bool validate_w3d_dump(W3dDump &dump)
{
	dump.Problems.clear();
	dump.TopLevelBytes = 0;
	dump.UnknownIds = 0;
	dump.SizeMismatches = 0;

	if (sizeof(ChunkHeader) != ON_DISK_CHUNK_HEADER_BYTES) {
		dump.Problems.push_back(
		    "sizeof(ChunkHeader) is " + std::to_string(sizeof(ChunkHeader)) +
		    " bytes as compiled; the on-disk header is 8. Every offset below is wrong.");
	}

	if (dump.TruncatedEarly) {
		dump.Problems.push_back("chunk walk hit the safety limit and was truncated");
	}

	// Container chunks must be exactly filled by their children (payload + 8 bytes of header
	// each). Walking depth-first, accumulate each chunk's consumed bytes into its parent.
	std::vector<int64_t> consumed_at_depth(64, 0);
	int previous_depth = -1;
	for (size_t i = 0; i < dump.Chunks.size(); ++i) {
		const W3dChunk &chunk = dump.Chunks[i];
		if (chunk.Depth >= static_cast<int>(consumed_at_depth.size())) {
			dump.Problems.push_back("chunk nesting deeper than 64 levels");
			break;
		}

		// Leaving a subtree: check every level we popped out of.
		for (int depth = previous_depth; depth > chunk.Depth; --depth) {
			consumed_at_depth[depth] = 0;
		}

		if (chunk.HasSubChunks) {
			consumed_at_depth[chunk.Depth + 1] = 0;
		} else if (chunk.Size > dump.FileSize) {
			dump.Problems.push_back("chunk 0x" + std::to_string(chunk.Id) + " claims " +
			                        std::to_string(chunk.Size) +
			                        " payload bytes, more than the whole file");
		}

		if (chunk.Depth == 0) {
			dump.TopLevelBytes += static_cast<int64_t>(chunk.Size) + ON_DISK_CHUNK_HEADER_BYTES;
		}
		if (chunk.Name.empty()) {
			++dump.UnknownIds;
		}
		previous_depth = chunk.Depth;
	}

	// Sum of children must equal the container's declared size, for every container.
	for (size_t i = 0; i < dump.Chunks.size(); ++i) {
		if (!dump.Chunks[i].HasSubChunks) {
			continue;
		}
		const int child_depth = dump.Chunks[i].Depth + 1;
		int64_t children_bytes = 0;
		for (size_t j = i + 1; j < dump.Chunks.size(); ++j) {
			if (dump.Chunks[j].Depth < child_depth) {
				break;
			}
			if (dump.Chunks[j].Depth == child_depth) {
				children_bytes +=
				    static_cast<int64_t>(dump.Chunks[j].Size) + ON_DISK_CHUNK_HEADER_BYTES;
			}
		}
		if (children_bytes != static_cast<int64_t>(dump.Chunks[i].Size)) {
			++dump.SizeMismatches;
		}
	}
	if (dump.SizeMismatches > 0) {
		dump.Problems.push_back(std::to_string(dump.SizeMismatches) +
		                        " container chunks are not exactly filled by their children");
	}

	if (dump.TopLevelBytes != dump.FileSize) {
		dump.Problems.push_back("top-level chunks account for " +
		                        std::to_string(dump.TopLevelBytes) + " bytes; the file is " +
		                        std::to_string(dump.FileSize));
	}
	if (dump.UnknownIds > 0) {
		dump.Problems.push_back(std::to_string(dump.UnknownIds) +
		                        " chunk ids are outside the W3D_CHUNK_* enum in w3d_file.h");
	}

	return dump.Problems.empty();
}
