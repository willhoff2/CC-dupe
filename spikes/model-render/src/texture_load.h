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

// Retail texture decoding: DDS (DXT1/DXT3/DXT5 and uncompressed) and TGA, from memory.
//
// The mip chain is kept in its on-disk form so the BC payload can be handed to Vulkan
// unmodified -- Apple Silicon supports the BC formats natively, so nothing is transcoded on
// the GPU path. The CPU decoder exists only to give the render an independent referee.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace zh
{

enum class TexFormat
{
	Unknown,
	Bc1,   // DXT1
	Bc2,   // DXT3
	Bc3,   // DXT5
	Bgra8, // uncompressed, B,G,R,A in memory (D3DFMT_A8R8G8B8 order)
};

struct MipLevel
{
	uint32_t width = 0;
	uint32_t height = 0;
	size_t offset = 0; // into TextureImage::data
	size_t bytes = 0;
};

struct TextureImage
{
	TexFormat format = TexFormat::Unknown;
	std::string format_name;   // "DXT1"/"DXT5"/"A8R8G8B8" as it appears on disk
	std::string vulkan_format; // the VkFormat the backend uploads it as
	uint32_t width = 0;
	uint32_t height = 0;
	bool has_alpha = false;
	std::vector<MipLevel> mips;
	std::vector<uint8_t> data;

	// Provenance, for the report.
	std::string requested_name;
	std::string archive;
	std::string stored_name;
	size_t file_bytes = 0;
};

const char *tex_format_name(TexFormat format);
size_t block_bytes(TexFormat format);

// `bytes` is a whole file out of an archive. The container is sniffed from the magic.
bool load_texture(const std::vector<uint8_t> &bytes, TextureImage &out, std::string &error);

// Expands one mip level to 8-bit RGBA, top row first. Used by the CPU reference rasteriser
// and by the texture-decode self-check; the GPU never sees this.
bool decode_mip_to_rgba(const TextureImage &image, size_t mip, std::vector<uint8_t> &rgba,
                        std::string &error);

} // namespace zh
