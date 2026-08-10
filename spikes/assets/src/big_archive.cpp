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

#include "big_archive.h"

#include "Utility/endian_compat.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

namespace
{

// StdBIGFileSystem reads each entry's stored path into `char buffer[_MAX_PATH]` with no bound
// check. _MAX_PATH is 260 in the MSVC headers, so a longer stored path is a stack overrun.
// The tool records how close the real archives come to that limit.
constexpr size_t WIN32_MAX_PATH = 260;

bool read_exact(std::FILE *fp, void *dest, size_t bytes)
{
	return std::fread(dest, 1, bytes, fp) == bytes;
}

int64_t file_size_on_disk(const char *path)
{
	struct stat st;
	if (stat(path, &st) != 0) {
		return -1;
	}
	return static_cast<int64_t>(st.st_size);
}

} // namespace

bool read_big_archive(const char *path, BigArchive &out, std::string &error)
{
	std::FILE *fp = std::fopen(path, "rb");
	if (fp == nullptr) {
		error = std::string("cannot open ") + path;
		return false;
	}

	out.Path = path;
	out.FileSizeOnDisk = file_size_on_disk(path);

	if (!read_exact(fp, out.Magic, 4)) {
		error = "file is shorter than the 4-byte magic";
		std::fclose(fp);
		return false;
	}
	if (std::strcmp(out.Magic, "BIGF") != 0 && std::strcmp(out.Magic, "BIG4") != 0) {
		error = std::string("not a BIG archive; magic is '") + out.Magic + "'";
		std::fclose(fp);
		return false;
	}

	// StdBIGFileSystem reads this dword raw (no byte swap), so it is little-endian.
	UnsignedInt header_size = 0;
	UnsignedInt entry_count = 0;
	UnsignedInt first_entry_offset = 0;
	if (!read_exact(fp, &header_size, 4) || !read_exact(fp, &entry_count, 4) ||
	    !read_exact(fp, &first_entry_offset, 4)) {
		error = "file is shorter than the 16-byte header";
		std::fclose(fp);
		return false;
	}
	out.HeaderSizeField = le32toh(header_size);
	out.EntryCount = betoh(entry_count);
	out.FirstEntryOffsetField = betoh(first_entry_offset);

	// The engine seeks to a hardcoded 0x10 rather than to the offset it just read.
	if (std::fseek(fp, 0x10, SEEK_SET) != 0) {
		error = "cannot seek to the table of contents";
		std::fclose(fp);
		return false;
	}

	out.Entries.reserve(out.EntryCount);
	for (UnsignedInt i = 0; i < out.EntryCount; ++i) {
		UnsignedInt offset = 0;
		UnsignedInt size = 0;
		if (!read_exact(fp, &offset, 4) || !read_exact(fp, &size, 4)) {
			error = "table of contents is truncated at entry " + std::to_string(i);
			std::fclose(fp);
			return false;
		}

		BigEntry entry;
		entry.Offset = betoh(offset);
		entry.Size = betoh(size);

		int c;
		while ((c = std::fgetc(fp)) != 0) {
			if (c == EOF) {
				error = "table of contents ends mid-name at entry " + std::to_string(i);
				std::fclose(fp);
				return false;
			}
			entry.Name.push_back(static_cast<char>(c));
		}

		out.Entries.push_back(std::move(entry));
	}

	std::fclose(fp);
	return true;
}

bool validate_big_archive(BigArchive &archive)
{
	archive.Problems.clear();
	archive.LongestNameLength = 0;
	archive.HighestEntryEnd = 0;
	archive.TotalEntryBytes = 0;
	archive.OverlappingPairs = 0;
	archive.ZeroSizeEntries = 0;
	archive.BackslashPathEntries = 0;

	if (archive.Entries.size() != archive.EntryCount) {
		archive.Problems.push_back("entry count in header (" + std::to_string(archive.EntryCount) +
		                           ") does not match entries read (" +
		                           std::to_string(archive.Entries.size()) + ")");
	}

	if (static_cast<int64_t>(archive.HeaderSizeField) != archive.FileSizeOnDisk) {
		archive.Problems.push_back("header size field (" + std::to_string(archive.HeaderSizeField) +
		                           ") != file size on disk (" +
		                           std::to_string(archive.FileSizeOnDisk) + ")");
	}

	std::vector<const BigEntry *> ordered;
	ordered.reserve(archive.Entries.size());
	for (const BigEntry &entry : archive.Entries) {
		archive.LongestNameLength = std::max(archive.LongestNameLength, entry.Name.size());
		archive.TotalEntryBytes += entry.Size;
		const int64_t end = static_cast<int64_t>(entry.Offset) + entry.Size;
		archive.HighestEntryEnd = std::max(archive.HighestEntryEnd, end);

		if (end > archive.FileSizeOnDisk) {
			archive.Problems.push_back("entry '" + entry.Name + "' ends at " + std::to_string(end) +
			                           ", past the end of the file");
		}
		// PatchZH.big holds zero-size entries at offset 0 whose names are wildcards ("Data\*").
		// They are deletion markers for the patcher, not file data, so only a non-empty entry
		// pointing into the header is a parse failure.
		if (entry.Offset < 0x10 && entry.Size > 0) {
			archive.Problems.push_back("entry '" + entry.Name + "' starts inside the header");
		}
		if (entry.Size == 0) {
			++archive.ZeroSizeEntries;
		}
		if (entry.Name.find('\\') != std::string::npos) {
			++archive.BackslashPathEntries;
		}
		if (entry.Name.size() + 1 > WIN32_MAX_PATH) {
			archive.Problems.push_back("entry name is " + std::to_string(entry.Name.size()) +
			                           " bytes, over the engine's 260-byte read buffer");
		}
		ordered.push_back(&entry);
	}

	std::sort(ordered.begin(), ordered.end(),
	          [](const BigEntry *a, const BigEntry *b) { return a->Offset < b->Offset; });
	for (size_t i = 1; i < ordered.size(); ++i) {
		if (ordered[i - 1]->Size == 0) {
			continue;
		}
		const int64_t prev_end =
		    static_cast<int64_t>(ordered[i - 1]->Offset) + ordered[i - 1]->Size;
		if (prev_end > static_cast<int64_t>(ordered[i]->Offset)) {
			++archive.OverlappingPairs;
		}
	}
	if (archive.OverlappingPairs > 0) {
		archive.Problems.push_back(std::to_string(archive.OverlappingPairs) +
		                           " pairs of entries overlap in the data region");
	}

	archive.BytesUnaccountedFor = archive.FileSizeOnDisk - archive.TotalEntryBytes;

	return archive.Problems.empty();
}

bool extract_big_entry(const BigArchive &archive, const BigEntry &entry, const char *dest_path,
                       std::string &error)
{
	std::FILE *src = std::fopen(archive.Path.c_str(), "rb");
	if (src == nullptr) {
		error = "cannot reopen " + archive.Path;
		return false;
	}
	if (std::fseek(src, static_cast<long>(entry.Offset), SEEK_SET) != 0) {
		error = "cannot seek to entry offset";
		std::fclose(src);
		return false;
	}

	std::FILE *dst = std::fopen(dest_path, "wb");
	if (dst == nullptr) {
		error = std::string("cannot create ") + dest_path;
		std::fclose(src);
		return false;
	}

	std::vector<char> buffer(64 * 1024);
	UnsignedInt remaining = entry.Size;
	while (remaining > 0) {
		const size_t want = std::min<size_t>(buffer.size(), remaining);
		const size_t got = std::fread(buffer.data(), 1, want, src);
		if (got == 0) {
			error = "short read while extracting";
			std::fclose(src);
			std::fclose(dst);
			return false;
		}
		std::fwrite(buffer.data(), 1, got, dst);
		remaining -= static_cast<UnsignedInt>(got);
	}

	std::fclose(src);
	std::fclose(dst);
	return true;
}
