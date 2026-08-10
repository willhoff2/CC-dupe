#include "png_write.h"

#include <cstdio>

namespace spike {
namespace {

uint32_t Crc32(const unsigned char* data, size_t len, uint32_t crc = 0) {
	static uint32_t table[256];
	static bool ready = false;
	if (!ready) {
		for (uint32_t i = 0; i < 256; ++i) {
			uint32_t c = i;
			for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
			table[i] = c;
		}
		ready = true;
	}
	crc = crc ^ 0xffffffffu;
	for (size_t i = 0; i < len; ++i) crc = table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
	return crc ^ 0xffffffffu;
}

void Append_Be32(std::string& out, uint32_t v) {
	out.push_back(static_cast<char>((v >> 24) & 0xff));
	out.push_back(static_cast<char>((v >> 16) & 0xff));
	out.push_back(static_cast<char>((v >> 8) & 0xff));
	out.push_back(static_cast<char>(v & 0xff));
}

void Append_Chunk(std::string& out, const char* type, const std::string& payload) {
	Append_Be32(out, static_cast<uint32_t>(payload.size()));
	std::string typed(type, 4);
	typed += payload;
	out += typed;
	Append_Be32(out, Crc32(reinterpret_cast<const unsigned char*>(typed.data()), typed.size()));
}

} // namespace

bool Write_Png(const std::string& path, const std::string& rgba, uint32_t width,
               uint32_t height) {
	if (rgba.size() < static_cast<size_t>(width) * height * 4) return false;

	// raw scanlines with the PNG filter byte
	std::string raw;
	raw.reserve((static_cast<size_t>(width) * 4 + 1) * height);
	for (uint32_t y = 0; y < height; ++y) {
		raw.push_back(0); // filter type 0
		raw.append(rgba, static_cast<size_t>(y) * width * 4, static_cast<size_t>(width) * 4);
	}

	// zlib stream with stored (uncompressed) deflate blocks
	std::string z;
	z.push_back(0x78);
	z.push_back(0x01);
	size_t offset = 0;
	while (offset < raw.size()) {
		const size_t block = raw.size() - offset < 65535 ? raw.size() - offset : 65535;
		const bool last = offset + block >= raw.size();
		z.push_back(static_cast<char>(last ? 1 : 0));
		z.push_back(static_cast<char>(block & 0xff));
		z.push_back(static_cast<char>((block >> 8) & 0xff));
		z.push_back(static_cast<char>((~block) & 0xff));
		z.push_back(static_cast<char>(((~block) >> 8) & 0xff));
		z.append(raw, offset, block);
		offset += block;
	}
	// adler-32 of the raw data
	uint32_t a = 1, b = 0;
	for (unsigned char c : raw) {
		a = (a + c) % 65521;
		b = (b + a) % 65521;
	}
	Append_Be32(z, (b << 16) | a);

	std::string png("\x89PNG\r\n\x1a\n", 8);
	std::string ihdr;
	Append_Be32(ihdr, width);
	Append_Be32(ihdr, height);
	ihdr.push_back(8); // bit depth
	ihdr.push_back(6); // colour type RGBA
	ihdr.push_back(0);
	ihdr.push_back(0);
	ihdr.push_back(0);
	Append_Chunk(png, "IHDR", ihdr);
	Append_Chunk(png, "IDAT", z);
	Append_Chunk(png, "IEND", std::string());

	FILE* f = std::fopen(path.c_str(), "wb");
	if (!f) return false;
	const bool ok = std::fwrite(png.data(), 1, png.size(), f) == png.size();
	std::fclose(f);
	return ok;
}

} // namespace spike
