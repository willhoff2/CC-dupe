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

#include "dds_inspect.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace
{

constexpr uint32_t DDPF_FOURCC = 0x00000004;
constexpr uint32_t DDPF_RGB = 0x00000040;
constexpr uint32_t DDSD_LINEARSIZE = 0x00080000;

uint32_t read_le32(const unsigned char *p)
{
	return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
	       (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

int block_bytes_for(const std::string &fourcc)
{
	if (fourcc == "DXT1") return 8;
	if (fourcc == "DXT2" || fourcc == "DXT3" || fourcc == "DXT4" || fourcc == "DXT5") return 16;
	return 0;
}

} // namespace

// Maps the D3D8-era FourCC to the Vulkan format the MoltenVK spike checked for support.
std::string dds_vulkan_format(const std::string &fourcc)
{
	if (fourcc == "DXT1") return "VK_FORMAT_BC1_RGBA_UNORM_BLOCK";
	if (fourcc == "DXT2" || fourcc == "DXT3") return "VK_FORMAT_BC2_UNORM_BLOCK";
	if (fourcc == "DXT4" || fourcc == "DXT5") return "VK_FORMAT_BC3_UNORM_BLOCK";
	return std::string();
}

bool read_dds_header(const char *path, DdsInfo &out, std::string &error)
{
	std::FILE *fp = std::fopen(path, "rb");
	if (fp == nullptr) {
		error = std::string("cannot open ") + path;
		return false;
	}

	unsigned char header[128];
	const size_t got = std::fread(header, 1, sizeof(header), fp);
	std::fseek(fp, 0, SEEK_END);
	out.FileSize = std::ftell(fp);
	std::fclose(fp);

	if (got != sizeof(header)) {
		error = "file is shorter than a 128-byte DDS header";
		return false;
	}
	if (std::memcmp(header, "DDS ", 4) != 0) {
		error = "not a DDS file";
		return false;
	}

	out.Path = path;
	const uint32_t flags = read_le32(header + 8);
	out.Height = read_le32(header + 12);
	out.Width = read_le32(header + 16);
	out.LinearSize = read_le32(header + 20);
	out.MipCount = read_le32(header + 28);
	out.PitchOrLinearFlagSet = (flags & DDSD_LINEARSIZE) ? 1 : 0;

	const uint32_t pf_flags = read_le32(header + 80);
	if (pf_flags & DDPF_FOURCC) {
		out.FourCC.assign(reinterpret_cast<const char *>(header + 84), 4);
		out.VulkanFormat = dds_vulkan_format(out.FourCC);
	}
	if (pf_flags & DDPF_RGB) {
		out.RgbBitCount = read_le32(header + 88);
	}

	return true;
}

bool validate_dds(DdsInfo &info)
{
	info.Problems.clear();

	const int block = block_bytes_for(info.FourCC);
	if (block == 0) {
		info.ExpectedPayloadBytes = 0;
		if (!info.FourCC.empty() && info.VulkanFormat.empty()) {
			info.Problems.push_back("FourCC '" + info.FourCC +
			                        "' is not one of the BC formats MoltenVK was tested for");
		}
		return info.Problems.empty();
	}

	// Sum the block-compressed size of every mip level, which is what the file must hold.
	const uint32_t levels = std::max<uint32_t>(info.MipCount, 1);
	uint32_t width = info.Width;
	uint32_t height = info.Height;
	int64_t total = 0;
	for (uint32_t level = 0; level < levels; ++level) {
		const int64_t blocks_x = (width + 3) / 4;
		const int64_t blocks_y = (height + 3) / 4;
		total += blocks_x * blocks_y * block;
		width = std::max<uint32_t>(width / 2, 1);
		height = std::max<uint32_t>(height / 2, 1);
	}
	info.ExpectedPayloadBytes = total;

	const int64_t actual_payload = info.FileSize - 128;
	if (actual_payload != total) {
		info.Problems.push_back("payload is " + std::to_string(actual_payload) +
		                        " bytes; " + std::to_string(levels) + " mips of " +
		                        std::to_string(info.Width) + "x" + std::to_string(info.Height) +
		                        " " + info.FourCC + " need " + std::to_string(total));
	}

	const int64_t top_mip = ((info.Width + 3) / 4) * static_cast<int64_t>((info.Height + 3) / 4) *
	                        block;
	if (info.PitchOrLinearFlagSet && info.LinearSize != 0 &&
	    static_cast<int64_t>(info.LinearSize) != top_mip) {
		info.Problems.push_back("dwPitchOrLinearSize is " + std::to_string(info.LinearSize) +
		                        "; the top mip is " + std::to_string(top_mip) + " bytes");
	}

	return info.Problems.empty();
}
