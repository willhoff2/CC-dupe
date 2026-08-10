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

#include "texture_load.h"

#include <cstring>

namespace zh
{

const char *tex_format_name(TexFormat format)
{
	switch (format) {
	case TexFormat::Bc1: return "BC1";
	case TexFormat::Bc2: return "BC2";
	case TexFormat::Bc3: return "BC3";
	case TexFormat::Bgra8: return "BGRA8";
	default: return "unknown";
	}
}

size_t block_bytes(TexFormat format)
{
	switch (format) {
	case TexFormat::Bc1: return 8;
	case TexFormat::Bc2:
	case TexFormat::Bc3: return 16;
	default: return 0;
	}
}

namespace
{

uint32_t read_le32(const uint8_t *p) { return static_cast<uint32_t>(p[0]) | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
uint16_t read_le16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

size_t level_bytes(TexFormat format, uint32_t width, uint32_t height)
{
	if (format == TexFormat::Bgra8) {
		return static_cast<size_t>(width) * height * 4;
	}
	const size_t blocks_x = (width + 3) / 4;
	const size_t blocks_y = (height + 3) / 4;
	return blocks_x * blocks_y * block_bytes(format);
}

bool load_dds(const std::vector<uint8_t> &bytes, TextureImage &out, std::string &error)
{
	if (bytes.size() < 128) {
		error = "DDS shorter than its header";
		return false;
	}
	const uint8_t *p = bytes.data();
	if (read_le32(p + 4) != 124) {
		error = "DDS header size field is not 124";
		return false;
	}
	out.height = read_le32(p + 12);
	out.width = read_le32(p + 16);
	const uint32_t declared_mips = read_le32(p + 28);
	const uint32_t pf_flags = read_le32(p + 80);
	const char fourcc[5] = {static_cast<char>(p[84]), static_cast<char>(p[85]),
	                        static_cast<char>(p[86]), static_cast<char>(p[87]), '\0'};
	const uint32_t rgb_bit_count = read_le32(p + 88);
	const uint32_t alpha_mask = read_le32(p + 104);

	const bool has_fourcc = (pf_flags & 0x4) != 0;
	if (has_fourcc && std::strcmp(fourcc, "DXT1") == 0) {
		out.format = TexFormat::Bc1;
		out.vulkan_format = "VK_FORMAT_BC1_RGBA_UNORM_BLOCK";
		out.has_alpha = true; // DXT1 carries 1-bit alpha; whether it is used is per-block
	} else if (has_fourcc && std::strcmp(fourcc, "DXT3") == 0) {
		out.format = TexFormat::Bc2;
		out.vulkan_format = "VK_FORMAT_BC2_UNORM_BLOCK";
		out.has_alpha = true;
	} else if (has_fourcc && std::strcmp(fourcc, "DXT5") == 0) {
		out.format = TexFormat::Bc3;
		out.vulkan_format = "VK_FORMAT_BC3_UNORM_BLOCK";
		out.has_alpha = true;
	} else if (!has_fourcc && rgb_bit_count == 32) {
		out.format = TexFormat::Bgra8;
		out.vulkan_format = "VK_FORMAT_B8G8R8A8_UNORM";
		out.has_alpha = alpha_mask != 0;
	} else {
		error = std::string("unsupported DDS pixel format (fourcc '") + fourcc + "', " +
		        std::to_string(rgb_bit_count) + " bpp)";
		return false;
	}
	out.format_name = has_fourcc ? fourcc : "A8R8G8B8";

	const uint32_t mip_count = declared_mips == 0 ? 1 : declared_mips;
	const uint8_t *payload = p + 128;
	const size_t available = bytes.size() - 128;
	size_t offset = 0;
	for (uint32_t level = 0; level < mip_count; ++level) {
		const uint32_t w = out.width >> level ? out.width >> level : 1;
		const uint32_t h = out.height >> level ? out.height >> level : 1;
		const size_t size = level_bytes(out.format, w, h);
		if (offset + size > available) {
			// Truncated chain: keep the levels that are actually present rather than
			// reading past the end. Reported by the caller.
			break;
		}
		out.mips.push_back({w, h, offset, size});
		offset += size;
	}
	if (out.mips.empty()) {
		error = "DDS payload too short for even the base level";
		return false;
	}
	out.data.assign(payload, payload + offset);
	out.file_bytes = bytes.size();
	return true;
}

bool load_tga(const std::vector<uint8_t> &bytes, TextureImage &out, std::string &error)
{
	if (bytes.size() < 18) {
		error = "TGA shorter than its header";
		return false;
	}
	const uint8_t *p = bytes.data();
	const uint8_t id_length = p[0];
	const uint8_t colour_map_type = p[1];
	const uint8_t image_type = p[2];
	out.width = read_le16(p + 12);
	out.height = read_le16(p + 14);
	const uint8_t bpp = p[16];
	const uint8_t descriptor = p[17];
	const bool top_origin = (descriptor & 0x20) != 0;

	if (colour_map_type != 0 || (image_type != 2 && image_type != 10)) {
		error = "TGA is not an uncompressed or RLE true-colour image (type " +
		        std::to_string(image_type) + ")";
		return false;
	}
	if (bpp != 24 && bpp != 32) {
		error = "TGA is " + std::to_string(bpp) + " bpp; only 24 and 32 are handled";
		return false;
	}

	const size_t pixel_count = static_cast<size_t>(out.width) * out.height;
	const size_t source_pixel_bytes = bpp / 8;
	std::vector<uint8_t> pixels(pixel_count * 4, 0xff);
	size_t cursor = 18u + id_length;

	auto emit = [&](size_t index, const uint8_t *src) {
		pixels[index * 4 + 0] = src[0];
		pixels[index * 4 + 1] = src[1];
		pixels[index * 4 + 2] = src[2];
		pixels[index * 4 + 3] = source_pixel_bytes == 4 ? src[3] : 0xff;
	};

	if (image_type == 2) {
		if (cursor + pixel_count * source_pixel_bytes > bytes.size()) {
			error = "TGA payload truncated";
			return false;
		}
		for (size_t i = 0; i < pixel_count; ++i) {
			emit(i, p + cursor + i * source_pixel_bytes);
		}
	} else {
		size_t written = 0;
		while (written < pixel_count) {
			if (cursor >= bytes.size()) {
				error = "TGA RLE stream truncated";
				return false;
			}
			const uint8_t packet = p[cursor++];
			const size_t run = (packet & 0x7f) + 1u;
			if (packet & 0x80) {
				if (cursor + source_pixel_bytes > bytes.size() || written + run > pixel_count) {
					error = "TGA RLE run overruns the image";
					return false;
				}
				for (size_t i = 0; i < run; ++i) {
					emit(written + i, p + cursor);
				}
				cursor += source_pixel_bytes;
			} else {
				if (cursor + run * source_pixel_bytes > bytes.size() ||
				    written + run > pixel_count) {
					error = "TGA raw run overruns the image";
					return false;
				}
				for (size_t i = 0; i < run; ++i) {
					emit(written + i, p + cursor + i * source_pixel_bytes);
				}
				cursor += run * source_pixel_bytes;
			}
			written += run;
		}
	}

	out.format = TexFormat::Bgra8;
	out.format_name = bpp == 32 ? "TGA A8R8G8B8" : "TGA R8G8B8";
	out.vulkan_format = "VK_FORMAT_B8G8R8A8_UNORM";
	out.has_alpha = bpp == 32;
	out.data.resize(pixel_count * 4);
	// TGA rows run bottom-to-top unless bit 5 of the descriptor says otherwise, while both
	// D3D and Vulkan address texel (0,0) as the top-left. Flip on load, once.
	const size_t row_bytes = static_cast<size_t>(out.width) * 4;
	for (uint32_t y = 0; y < out.height; ++y) {
		const uint32_t source_row = top_origin ? y : (out.height - 1 - y);
		std::memcpy(out.data.data() + y * row_bytes, pixels.data() + source_row * row_bytes,
		            row_bytes);
	}
	out.mips.push_back({out.width, out.height, 0, out.data.size()});
	out.file_bytes = bytes.size();
	return true;
}

// --- BC decoding, for the CPU reference only -------------------------------------------

void decode_bc1_colours(const uint8_t *block, uint8_t rgba[16][4], bool punchthrough_alpha)
{
	const uint16_t c0 = read_le16(block);
	const uint16_t c1 = read_le16(block + 2);
	uint8_t palette[4][4];
	auto expand = [](uint16_t c, uint8_t *out) {
		const uint32_t r = (c >> 11) & 0x1f;
		const uint32_t g = (c >> 5) & 0x3f;
		const uint32_t b = c & 0x1f;
		out[0] = static_cast<uint8_t>((r << 3) | (r >> 2));
		out[1] = static_cast<uint8_t>((g << 2) | (g >> 4));
		out[2] = static_cast<uint8_t>((b << 3) | (b >> 2));
		out[3] = 255;
	};
	expand(c0, palette[0]);
	expand(c1, palette[1]);
	if (c0 > c1 || !punchthrough_alpha) {
		for (int i = 0; i < 3; ++i) {
			palette[2][i] = static_cast<uint8_t>((2 * palette[0][i] + palette[1][i]) / 3);
			palette[3][i] = static_cast<uint8_t>((palette[0][i] + 2 * palette[1][i]) / 3);
		}
		palette[2][3] = 255;
		palette[3][3] = 255;
	} else {
		for (int i = 0; i < 3; ++i) {
			palette[2][i] = static_cast<uint8_t>((palette[0][i] + palette[1][i]) / 2);
			palette[3][i] = 0;
		}
		palette[2][3] = 255;
		palette[3][3] = 0;
	}
	const uint32_t bits = read_le32(block + 4);
	for (int i = 0; i < 16; ++i) {
		std::memcpy(rgba[i], palette[(bits >> (i * 2)) & 0x3], 4);
	}
}

void decode_bc3_alpha(const uint8_t *block, uint8_t alpha[16])
{
	uint8_t values[8];
	values[0] = block[0];
	values[1] = block[1];
	if (values[0] > values[1]) {
		for (int i = 1; i < 7; ++i) {
			values[i + 1] = static_cast<uint8_t>(((7 - i) * values[0] + i * values[1]) / 7);
		}
	} else {
		for (int i = 1; i < 5; ++i) {
			values[i + 1] = static_cast<uint8_t>(((5 - i) * values[0] + i * values[1]) / 5);
		}
		values[6] = 0;
		values[7] = 255;
	}
	uint64_t bits = 0;
	for (int i = 0; i < 6; ++i) {
		bits |= static_cast<uint64_t>(block[2 + i]) << (8 * i);
	}
	for (int i = 0; i < 16; ++i) {
		alpha[i] = values[(bits >> (i * 3)) & 0x7];
	}
}

} // namespace

bool load_texture(const std::vector<uint8_t> &bytes, TextureImage &out, std::string &error)
{
	if (bytes.size() >= 4 && std::memcmp(bytes.data(), "DDS ", 4) == 0) {
		return load_dds(bytes, out, error);
	}
	return load_tga(bytes, out, error);
}

bool decode_mip_to_rgba(const TextureImage &image, size_t mip, std::vector<uint8_t> &rgba,
                        std::string &error)
{
	if (mip >= image.mips.size()) {
		error = "mip level out of range";
		return false;
	}
	const MipLevel &level = image.mips[mip];
	rgba.assign(static_cast<size_t>(level.width) * level.height * 4, 0);
	const uint8_t *source = image.data.data() + level.offset;

	if (image.format == TexFormat::Bgra8) {
		for (size_t i = 0; i < static_cast<size_t>(level.width) * level.height; ++i) {
			rgba[i * 4 + 0] = source[i * 4 + 2];
			rgba[i * 4 + 1] = source[i * 4 + 1];
			rgba[i * 4 + 2] = source[i * 4 + 0];
			rgba[i * 4 + 3] = source[i * 4 + 3];
		}
		return true;
	}

	const size_t bytes_per_block = block_bytes(image.format);
	if (bytes_per_block == 0) {
		error = "cannot decode this format";
		return false;
	}
	const uint32_t blocks_x = (level.width + 3) / 4;
	const uint32_t blocks_y = (level.height + 3) / 4;
	for (uint32_t by = 0; by < blocks_y; ++by) {
		for (uint32_t bx = 0; bx < blocks_x; ++bx) {
			const uint8_t *block = source + (static_cast<size_t>(by) * blocks_x + bx) * bytes_per_block;
			uint8_t texels[16][4];
			uint8_t alpha[16];
			for (int i = 0; i < 16; ++i) {
				alpha[i] = 255;
			}
			switch (image.format) {
			case TexFormat::Bc1:
				decode_bc1_colours(block, texels, true);
				break;
			case TexFormat::Bc2:
				decode_bc1_colours(block + 8, texels, false);
				for (int i = 0; i < 16; ++i) {
					const uint8_t nibble = (block[i / 2] >> ((i & 1) ? 4 : 0)) & 0xf;
					alpha[i] = static_cast<uint8_t>(nibble * 17);
				}
				break;
			case TexFormat::Bc3:
				decode_bc1_colours(block + 8, texels, false);
				decode_bc3_alpha(block, alpha);
				break;
			default:
				error = "cannot decode this format";
				return false;
			}
			for (int i = 0; i < 16; ++i) {
				const uint32_t x = bx * 4 + (i % 4);
				const uint32_t y = by * 4 + (i / 4);
				if (x >= level.width || y >= level.height) {
					continue;
				}
				const size_t dest = (static_cast<size_t>(y) * level.width + x) * 4;
				rgba[dest + 0] = texels[i][0];
				rgba[dest + 1] = texels[i][1];
				rgba[dest + 2] = texels[i][2];
				rgba[dest + 3] = image.format == TexFormat::Bc1 ? texels[i][3] : alpha[i];
			}
		}
	}
	return true;
}

} // namespace zh
