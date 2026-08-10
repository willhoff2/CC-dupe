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

struct DdsInfo
{
	std::string Path;
	int64_t FileSize = 0;
	uint32_t Width = 0;
	uint32_t Height = 0;
	uint32_t MipCount = 0;
	uint32_t LinearSize = 0;
	uint32_t PitchOrLinearFlagSet = 0;
	std::string FourCC;      // DXT1 / DXT3 / DXT5, or "" for uncompressed
	std::string VulkanFormat; // the BC format this maps to
	uint32_t RgbBitCount = 0;

	int64_t ExpectedPayloadBytes = 0; // computed from dimensions + mip count + block size
	std::vector<std::string> Problems;
};

bool read_dds_header(const char *path, DdsInfo &out, std::string &error);
bool validate_dds(DdsInfo &info);

// The Vulkan block format a D3D8-era FourCC maps to, or "" if it is not a BC format.
std::string dds_vulkan_format(const std::string &fourcc);
