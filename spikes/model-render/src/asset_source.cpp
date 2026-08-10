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

#include "asset_source.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <dirent.h>

namespace zh
{

std::string asset_key(const std::string &path)
{
	size_t start = 0;
	for (size_t i = 0; i < path.size(); ++i) {
		if (path[i] == '\\' || path[i] == '/') {
			start = i + 1;
		}
	}
	std::string key = path.substr(start);
	std::transform(key.begin(), key.end(), key.begin(),
	               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return key;
}

namespace
{

bool ends_with_ci(const std::string &s, const char *suffix)
{
	const size_t n = std::string(suffix).size();
	if (s.size() < n) {
		return false;
	}
	for (size_t i = 0; i < n; ++i) {
		if (std::tolower(static_cast<unsigned char>(s[s.size() - n + i])) !=
		    std::tolower(static_cast<unsigned char>(suffix[i]))) {
			return false;
		}
	}
	return true;
}

std::string replace_extension(const std::string &name, const char *ext)
{
	const size_t dot = name.rfind('.');
	return (dot == std::string::npos ? name : name.substr(0, dot)) + ext;
}

} // namespace

bool AssetSource::open_directory(const std::string &data_dir, std::string &error)
{
	DIR *dir = ::opendir(data_dir.c_str());
	if (dir == nullptr) {
		error = "cannot open data directory " + data_dir;
		return false;
	}

	// Sorted so the lookup order -- and therefore which archive wins a duplicate name --
	// does not depend on readdir order.
	std::vector<std::string> archive_names;
	while (const dirent *entry = ::readdir(dir)) {
		const std::string name = entry->d_name;
		if (ends_with_ci(name, ".big")) {
			archive_names.push_back(name);
		}
	}
	::closedir(dir);
	if (archive_names.empty()) {
		error = "no .big archives in " + data_dir;
		return false;
	}
	std::sort(archive_names.begin(), archive_names.end());

	Archives.reserve(archive_names.size());
	for (const std::string &name : archive_names) {
		BigArchive archive;
		std::string read_error;
		if (!read_big_archive((data_dir + "/" + name).c_str(), archive, read_error)) {
			error = read_error;
			return false;
		}
		Archives.push_back(std::move(archive));
		ArchiveNames.push_back(name);
	}

	for (size_t a = 0; a < Archives.size(); ++a) {
		for (size_t e = 0; e < Archives[a].Entries.size(); ++e) {
			Index.push_back({asset_key(Archives[a].Entries[e].Name), a, e});
		}
	}
	std::stable_sort(Index.begin(), Index.end(),
	          [](const IndexedEntry &l, const IndexedEntry &r) { return l.key < r.key; });
	return true;
}

bool AssetSource::find(const std::string &name, AssetLocation &out, bool try_dds_for_tga) const
{
	std::vector<std::string> candidates;
	candidates.push_back(asset_key(name));
	if (try_dds_for_tga && ends_with_ci(name, ".tga")) {
		candidates.push_back(asset_key(replace_extension(name, ".dds")));
	}

	for (const std::string &key : candidates) {
		auto it = std::lower_bound(Index.begin(), Index.end(), key,
		                           [](const IndexedEntry &l, const std::string &k) {
			                           return l.key < k;
		                           });
		if (it == Index.end() || it->key != key) {
			continue;
		}
		const BigArchive &archive = Archives[it->archive_index];
		const BigEntry &entry = archive.Entries[it->entry_index];
		out.archive = ArchiveNames[it->archive_index];
		out.stored_name = entry.Name;
		out.size = entry.Size;
		return true;
	}
	return false;
}

bool AssetSource::read(const AssetLocation &where, std::vector<uint8_t> &out,
                       std::string &error) const
{
	for (size_t a = 0; a < Archives.size(); ++a) {
		if (ArchiveNames[a] != where.archive) {
			continue;
		}
		for (const BigEntry &entry : Archives[a].Entries) {
			if (entry.Name != where.stored_name) {
				continue;
			}
			std::FILE *file = std::fopen(Archives[a].Path.c_str(), "rb");
			if (file == nullptr) {
				error = "cannot open " + Archives[a].Path;
				return false;
			}
			out.resize(entry.Size);
			bool ok = std::fseek(file, static_cast<long>(entry.Offset), SEEK_SET) == 0;
			if (ok && entry.Size > 0) {
				ok = std::fread(out.data(), 1, entry.Size, file) == entry.Size;
			}
			std::fclose(file);
			if (!ok) {
				error = "short read of " + where.stored_name;
				return false;
			}
			return true;
		}
	}
	error = "entry " + where.stored_name + " not found in " + where.archive;
	return false;
}

bool AssetSource::read_by_name(const std::string &name, std::vector<uint8_t> &out,
                               AssetLocation &where, std::string &error,
                               bool try_dds_for_tga) const
{
	if (!find(name, where, try_dds_for_tga)) {
		error = "no archive entry named " + name;
		return false;
	}
	return read(where, out, error);
}

} // namespace zh
