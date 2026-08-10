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

// zh-asset-inspect: reads retail Zero Hour data with the engine's own parsing code, natively.
//
// Takes paths on the command line. It ships no data and hardcodes no path.

#include "big_archive.h"
#include "dds_inspect.h"
#include "w3d_dump.h"

#include "chunkio.h"
#include "iostruct.h"
#include "Lib/BaseTypeCore.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace
{

int usage()
{
	std::fprintf(stderr,
	             "usage:\n"
	             "  zh-asset-inspect sizes\n"
	             "      sizeof() the on-disk structures as compiled on this host\n"
	             "  zh-asset-inspect inventory <data-dir>\n"
	             "      list every .big archive in a directory with its header fields\n"
	             "  zh-asset-inspect big <archive.big> [--list] [--limit N]\n"
	             "      parse and validate one archive's table of contents\n"
	             "  zh-asset-inspect extract <archive.big> <entry-substring> <dest-file>\n"
	             "      copy one entry out of an archive\n"
	             "  zh-asset-inspect w3d <file.w3d> [--limit N]\n"
	             "      walk a model's chunk tree with the engine's ChunkLoadClass\n"
	             "  zh-asset-inspect sweep <archive.big> <scratch-dir> [.w3d|.dds] [--limit N]\n"
	             "      parse every asset of that type in an archive, report how many validate\n"
	             "  zh-asset-inspect dds <file.dds>\n"
	             "      decode a texture header and report its Vulkan block format\n");
	return 2;
}

void print_sizes()
{
	std::printf("host: %zu-bit pointers, %s endian\n", sizeof(void *) * 8,
	            (*reinterpret_cast<const uint16_t *>("\1\0") == 1) ? "little" : "big");
	std::printf("\n%-34s %5s %s\n", "type", "bytes", "on-disk requirement");
	std::printf("%-34s %5zu %s\n", "ChunkHeader", sizeof(ChunkHeader), "8  <- .w3d chunk header");
	std::printf("%-34s %5zu %s\n", "MicroChunkHeader", sizeof(MicroChunkHeader), "2");
	std::printf("%-34s %5zu %s\n", "IOVector2Struct", sizeof(IOVector2Struct), "8");
	std::printf("%-34s %5zu %s\n", "IOVector3Struct", sizeof(IOVector3Struct), "12");
	std::printf("%-34s %5zu %s\n", "IOVector4Struct", sizeof(IOVector4Struct), "16");
	std::printf("%-34s %5zu %s\n", "IOQuaternionStruct", sizeof(IOQuaternionStruct), "16");
	std::printf("\n%-34s %5s\n", "scalar", "bytes");
	std::printf("%-34s %5zu  (WWLib/bittype.h)\n", "uint32", sizeof(uint32));
	std::printf("%-34s %5zu  (WWLib/bittype.h)\n", "sint32", sizeof(sint32));
	std::printf("%-34s %5zu  (WWLib/bittype.h)\n", "DWORD", sizeof(DWORD));
	std::printf("%-34s %5zu  (Lib/BaseTypeCore.h)\n", "Int", sizeof(Int));
	std::printf("%-34s %5zu  (Lib/BaseTypeCore.h)\n", "UnsignedInt", sizeof(UnsignedInt));
	std::printf("%-34s %5zu\n", "unsigned long", sizeof(unsigned long));
	std::printf("%-34s %5zu\n", "void *", sizeof(void *));

	if (sizeof(ChunkHeader) != 8) {
		std::printf("\nFAIL: ChunkHeader is %zu bytes. Reading a .w3d with this build "
		            "misinterprets the file from the very first header.\n",
		            sizeof(ChunkHeader));
	} else {
		std::printf("\nOK: ChunkHeader matches the 8-byte on-disk layout.\n");
	}
}

void print_problems(const std::vector<std::string> &problems, const char *label)
{
	if (problems.empty()) {
		std::printf("%s: all cross-checks passed\n", label);
		return;
	}
	std::printf("%s: %zu problem(s)\n", label, problems.size());
	for (const std::string &problem : problems) {
		std::printf("  - %s\n", problem.c_str());
	}
}

int command_big(const char *path, bool list, long limit)
{
	BigArchive archive;
	std::string error;
	if (!read_big_archive(path, archive, error)) {
		std::fprintf(stderr, "error: %s\n", error.c_str());
		return 1;
	}
	validate_big_archive(archive);

	std::printf("archive              %s\n", archive.Path.c_str());
	std::printf("magic                %s\n", archive.Magic);
	std::printf("size on disk         %" PRId64 "\n", archive.FileSizeOnDisk);
	std::printf("header size field    %u (little-endian)\n", archive.HeaderSizeField);
	std::printf("entry count          %u (big-endian)\n", archive.EntryCount);
	std::printf("first entry offset   %u (big-endian)\n", archive.FirstEntryOffsetField);
	std::printf("entries read         %zu\n", archive.Entries.size());
	std::printf("sum of entry sizes   %" PRId64 "\n", archive.TotalEntryBytes);
	std::printf("highest entry end    %" PRId64 "\n", archive.HighestEntryEnd);
	std::printf("bytes not in entries %" PRId64 "\n", archive.BytesUnaccountedFor);
	std::printf("longest entry name   %zu bytes (engine buffer is 260)\n",
	            archive.LongestNameLength);
	std::printf("zero-size entries    %" PRId64 "\n", archive.ZeroSizeEntries);
	std::printf("backslash paths      %" PRId64 " of %zu\n", archive.BackslashPathEntries,
	            archive.Entries.size());
	print_problems(archive.Problems, "validation");

	if (list) {
		std::printf("\n%12s %12s  %s\n", "offset", "size", "name");
		long shown = 0;
		for (const BigEntry &entry : archive.Entries) {
			if (limit >= 0 && shown >= limit) {
				std::printf("... %zu more\n", archive.Entries.size() - shown);
				break;
			}
			std::printf("%12u %12u  %s\n", entry.Offset, entry.Size, entry.Name.c_str());
			++shown;
		}
	}
	return archive.Problems.empty() ? 0 : 1;
}

int command_inventory(const char *dir)
{
	std::string command = "ls -1 '" + std::string(dir) + "'";
	std::FILE *pipe = popen(command.c_str(), "r");
	if (pipe == nullptr) {
		std::fprintf(stderr, "error: cannot list %s\n", dir);
		return 1;
	}

	std::printf("%-26s %12s %8s %10s  %s\n", "archive", "bytes", "entries", "sum(sizes)",
	            "status");
	char line[4096];
	int failures = 0;
	while (std::fgets(line, sizeof(line), pipe) != nullptr) {
		std::string name(line);
		while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) {
			name.pop_back();
		}
		if (name.size() < 4 || strcasecmp(name.c_str() + name.size() - 4, ".big") != 0) {
			continue;
		}

		const std::string full = std::string(dir) + "/" + name;
		BigArchive archive;
		std::string error;
		if (!read_big_archive(full.c_str(), archive, error)) {
			std::printf("%-26s %12s %8s %10s  ERROR: %s\n", name.c_str(), "-", "-", "-",
			            error.c_str());
			++failures;
			continue;
		}
		validate_big_archive(archive);
		std::printf("%-26s %12" PRId64 " %8u %10" PRId64 "  %s\n", name.c_str(),
		            archive.FileSizeOnDisk, archive.EntryCount, archive.TotalEntryBytes,
		            archive.Problems.empty() ? "ok" : archive.Problems.front().c_str());
		if (!archive.Problems.empty()) {
			++failures;
		}
	}
	pclose(pipe);
	return failures == 0 ? 0 : 1;
}

int command_extract(const char *path, const char *needle, const char *dest)
{
	BigArchive archive;
	std::string error;
	if (!read_big_archive(path, archive, error)) {
		std::fprintf(stderr, "error: %s\n", error.c_str());
		return 1;
	}

	std::string lowered_needle(needle);
	std::transform(lowered_needle.begin(), lowered_needle.end(), lowered_needle.begin(), ::tolower);

	for (const BigEntry &entry : archive.Entries) {
		std::string lowered(entry.Name);
		std::transform(lowered.begin(), lowered.end(), lowered.begin(), ::tolower);
		if (lowered.find(lowered_needle) == std::string::npos) {
			continue;
		}
		if (!extract_big_entry(archive, entry, dest, error)) {
			std::fprintf(stderr, "error: %s\n", error.c_str());
			return 1;
		}
		std::printf("%s  (offset %u, %u bytes) -> %s\n", entry.Name.c_str(), entry.Offset,
		            entry.Size, dest);
		return 0;
	}

	std::fprintf(stderr, "error: no entry matching '%s'\n", needle);
	return 1;
}

int command_w3d(const char *path, long limit)
{
	W3dDump dump;
	std::string error;
	const size_t cap = limit > 0 ? static_cast<size_t>(limit) : 200000;
	if (!dump_w3d(path, dump, error, cap)) {
		std::fprintf(stderr, "error: %s\n", error.c_str());
		std::fprintf(stderr, "sizeof(ChunkHeader) = %zu (must be 8)\n", sizeof(ChunkHeader));
		return 1;
	}
	validate_w3d_dump(dump);

	std::printf("file                 %s\n", dump.Path.c_str());
	std::printf("size                 %" PRId64 "\n", dump.FileSize);
	std::printf("sizeof(ChunkHeader)  %zu (on-disk layout is 8)\n", sizeof(ChunkHeader));
	std::printf("chunks read          %zu\n", dump.Chunks.size());
	std::printf("top-level bytes      %" PRId64 " (must equal the file size)\n",
	            dump.TopLevelBytes);
	std::printf("unknown chunk ids    %" PRId64 "\n", dump.UnknownIds);
	print_problems(dump.Problems, "validation");

	std::printf("\n%10s %10s %6s  %s\n", "offset", "size", "sub", "chunk");
	int64_t position = 0;
	std::vector<int64_t> cursor(64, 0);
	for (const W3dChunk &chunk : dump.Chunks) {
		std::string indent(static_cast<size_t>(chunk.Depth) * 2, ' ');
		const std::string name =
		    chunk.Name.empty() ? std::string("<unknown id>") : chunk.Name;
		char id_text[32];
		std::snprintf(id_text, sizeof(id_text), "0x%08X", chunk.Id);
		std::printf("%10" PRId64 " %10u %6s  %s%s %s\n", position, chunk.Size,
		            chunk.HasSubChunks ? "yes" : "no", indent.c_str(), id_text, name.c_str());
		position += chunk.HasSubChunks ? 8 : static_cast<int64_t>(chunk.Size) + 8;
	}
	(void)cursor;

	return dump.Problems.empty() ? 0 : 1;
}

bool has_extension(const std::string &name, const char *extension)
{
	const size_t length = std::strlen(extension);
	return name.size() > length &&
	       strcasecmp(name.c_str() + name.size() - length, extension) == 0;
}

// Parses every .w3d or .dds entry of an archive so the pass rate is a population statistic
// rather than one hand-picked file. Entries are extracted into `scratch` one at a time and the
// scratch file is reused, so the tool never leaves a copy of the game data behind.
int command_sweep(const char *archive_path, const char *scratch_dir, const char *extension,
                  long limit)
{
	BigArchive archive;
	std::string error;
	if (!read_big_archive(archive_path, archive, error)) {
		std::fprintf(stderr, "error: %s\n", error.c_str());
		return 1;
	}

	const std::string scratch = std::string(scratch_dir) + "/zh-asset-inspect-scratch.bin";
	long parsed = 0;
	long validated = 0;
	long failed_parse = 0;
	long failed_validation = 0;
	int64_t chunks_total = 0;
	std::vector<std::string> first_failures;
	std::map<std::string, long> formats;
	const bool sweeping_textures = strcasecmp(extension, ".dds") == 0;

	for (const BigEntry &entry : archive.Entries) {
		if (!has_extension(entry.Name, extension)) {
			continue;
		}
		if (limit > 0 && parsed >= limit) {
			break;
		}
		if (!extract_big_entry(archive, entry, scratch.c_str(), error)) {
			std::fprintf(stderr, "extract failed for %s: %s\n", entry.Name.c_str(),
			             error.c_str());
			++failed_parse;
			continue;
		}

		++parsed;
		if (sweeping_textures) {
			DdsInfo info;
			if (!read_dds_header(scratch.c_str(), info, error)) {
				++failed_parse;
				if (first_failures.size() < 10) {
					first_failures.push_back(entry.Name + ": " + error);
				}
				continue;
			}
			formats[info.FourCC.empty() ? "(uncompressed)" : info.FourCC] += 1;
			if (validate_dds(info)) {
				++validated;
			} else {
				++failed_validation;
				if (first_failures.size() < 10) {
					first_failures.push_back(entry.Name + ": " + info.Problems.front());
				}
			}
			continue;
		}

		W3dDump dump;
		if (!dump_w3d(scratch.c_str(), dump, error)) {
			++failed_parse;
			if (first_failures.size() < 10) {
				first_failures.push_back(entry.Name + ": " + error);
			}
			continue;
		}
		chunks_total += static_cast<int64_t>(dump.Chunks.size());
		if (validate_w3d_dump(dump)) {
			++validated;
		} else {
			++failed_validation;
			if (first_failures.size() < 10) {
				first_failures.push_back(entry.Name + ": " + dump.Problems.front());
			}
		}
	}
	std::remove(scratch.c_str());

	std::printf("archive              %s\n", archive.Path.c_str());
	std::printf("sizeof(ChunkHeader)  %zu (on-disk layout is 8)\n", sizeof(ChunkHeader));
	std::printf("%-4s entries parsed  %ld\n", extension, parsed);
	std::printf("all checks passed    %ld\n", validated);
	std::printf("parse failures       %ld\n", failed_parse);
	std::printf("validation failures  %ld\n", failed_validation);
	if (!sweeping_textures) {
		std::printf("chunks walked        %" PRId64 "\n", chunks_total);
	}
	for (const auto &format : formats) {
		std::printf("format %-14s %ld  %s\n", format.first.c_str(), format.second,
		            dds_vulkan_format(format.first).c_str());
	}
	for (const std::string &failure : first_failures) {
		std::printf("  - %s\n", failure.c_str());
	}
	return (failed_parse == 0 && failed_validation == 0) ? 0 : 1;
}

int command_dds(const char *path)
{
	DdsInfo info;
	std::string error;
	if (!read_dds_header(path, info, error)) {
		std::fprintf(stderr, "error: %s\n", error.c_str());
		return 1;
	}
	validate_dds(info);

	std::printf("file                 %s\n", info.Path.c_str());
	std::printf("size                 %" PRId64 "\n", info.FileSize);
	std::printf("dimensions           %ux%u\n", info.Width, info.Height);
	std::printf("mip levels           %u\n", info.MipCount);
	std::printf("fourcc               %s\n", info.FourCC.empty() ? "(uncompressed)"
	                                                             : info.FourCC.c_str());
	if (!info.VulkanFormat.empty()) {
		std::printf("vulkan format        %s\n", info.VulkanFormat.c_str());
	}
	if (info.RgbBitCount != 0) {
		std::printf("rgb bit count        %u\n", info.RgbBitCount);
	}
	std::printf("expected payload     %" PRId64 "\n", info.ExpectedPayloadBytes);
	print_problems(info.Problems, "validation");
	return info.Problems.empty() ? 0 : 1;
}

long option_value(int argc, char **argv, const char *name, long fallback)
{
	for (int i = 0; i + 1 < argc; ++i) {
		if (std::strcmp(argv[i], name) == 0) {
			return std::strtol(argv[i + 1], nullptr, 10);
		}
	}
	return fallback;
}

bool has_flag(int argc, char **argv, const char *name)
{
	for (int i = 0; i < argc; ++i) {
		if (std::strcmp(argv[i], name) == 0) {
			return true;
		}
	}
	return false;
}

} // namespace

int main(int argc, char **argv)
{
	if (argc < 2) {
		return usage();
	}
	const std::string command = argv[1];

	if (command == "sizes") {
		print_sizes();
		return sizeof(ChunkHeader) == 8 ? 0 : 1;
	}
	if (command == "inventory" && argc >= 3) {
		return command_inventory(argv[2]);
	}
	if (command == "big" && argc >= 3) {
		return command_big(argv[2], has_flag(argc, argv, "--list"),
		                   option_value(argc, argv, "--limit", 50));
	}
	if (command == "extract" && argc >= 5) {
		return command_extract(argv[2], argv[3], argv[4]);
	}
	if (command == "w3d" && argc >= 3) {
		return command_w3d(argv[2], option_value(argc, argv, "--limit", 0));
	}
	if (command == "sweep" && argc >= 4) {
		const char *extension = (argc >= 5 && argv[4][0] != '-') ? argv[4] : ".w3d";
		return command_sweep(argv[2], argv[3], extension, option_value(argc, argv, "--limit", 0));
	}
	if (command == "dds" && argc >= 3) {
		return command_dds(argv[2]);
	}
	return usage();
}
