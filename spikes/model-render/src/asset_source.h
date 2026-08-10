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

// Locates assets by name across every .big archive in a data directory.
//
// The engine's ArchiveFileSystem does the same job: it merges every archive into one flat
// namespace and looks names up case-insensitively, because the stored paths are Windows
// paths. Two behaviours copied from it deliberately:
//
//   * lookup ignores case and treats '\' and '/' as the same separator, and
//   * a name whose extension is .tga is also tried as .dds, because that is what
//     TextureLoader does on Windows -- retail .w3d files reference "avcrusader.tga"
//     and the shipped file is "Art\Textures\avcrusader.dds".
#pragma once

#include "big_archive.h"

#include <cstdint>
#include <string>
#include <vector>

namespace zh
{

struct AssetLocation
{
	std::string archive;    // archive file name, e.g. "W3DZH.big"
	std::string stored_name; // full stored path with backslashes, as it appears in the archive
	uint32_t size = 0;
};

class AssetSource
{
public:
	// Reads the table of contents of every *.big in `data_dir`. Returns false on error.
	bool open_directory(const std::string &data_dir, std::string &error);

	// Case- and separator-insensitive lookup on the trailing path component. When
	// `try_dds_for_tga` is set, a .tga request also matches a .dds file of the same stem.
	bool find(const std::string &name, AssetLocation &out, bool try_dds_for_tga = true) const;

	bool read(const AssetLocation &where, std::vector<uint8_t> &out, std::string &error) const;

	// Convenience: find + read.
	bool read_by_name(const std::string &name, std::vector<uint8_t> &out, AssetLocation &where,
	                  std::string &error, bool try_dds_for_tga = true) const;

	size_t archive_count() const { return Archives.size(); }
	size_t entry_count() const { return Index.size(); }
	const std::vector<std::string> &archive_names() const { return ArchiveNames; }

private:
	struct IndexedEntry
	{
		std::string key; // lowercased trailing component
		size_t archive_index = 0;
		size_t entry_index = 0;
	};

	std::vector<BigArchive> Archives;
	std::vector<std::string> ArchiveNames;
	std::vector<IndexedEntry> Index;
};

// Lowercases and strips any directory prefix, turning "Art\W3D\Foo.W3D" into "foo.w3d".
std::string asset_key(const std::string &path);

} // namespace zh
