/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 TheSuperHackers
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

/*
**	Win32 .ANI / .CUR decoding. Format references: the RIFF ACON layout is the one documented in
**	the Windows SDK's winuser.h comments and the "ANI" file format notes; the .CUR / .ICO layout is
**	ICONDIR / ICONDIRENTRY / BITMAPINFOHEADER from winuser.h and wingdi.h. Everything is little
**	endian and read byte-wise, so this compiles and runs identically on every host.
**
**	A .CUR frame is a DIB with height doubled: the top half (in memory, bottom-up) is the XOR
**	colour bitmap, the lower half the 1-bit AND mask. Transparent where AND is 1 and XOR is 0;
**	opaque where AND is 0. Pixels with AND=1 and XOR!=0 are Win32's "invert the screen" pixels,
**	which no native cursor API has; they are decoded as opaque, which is the usual reading.
*/

#include "platform_cursor.h"

#include <stdio.h>
#include <string.h>

namespace WWPlatform
{

namespace
{

unsigned Read_U16(const unsigned char * p)
{
	return static_cast<unsigned>(p[0]) | (static_cast<unsigned>(p[1]) << 8);
}

unsigned Read_U32(const unsigned char * p)
{
	return static_cast<unsigned>(p[0]) | (static_cast<unsigned>(p[1]) << 8) |
		(static_cast<unsigned>(p[2]) << 16) | (static_cast<unsigned>(p[3]) << 24);
}

bool Tag_Is(const unsigned char * p, const char * tag)
{
	return memcmp(p, tag, 4) == 0;
}

bool Fail(std::string & error, const char * what)
{
	error = what;
	return false;
}

/*
**	Row stride of a DIB scanline: rows are padded to 4 bytes.
*/
size_t Dib_Stride(unsigned width, unsigned bits)
{
	return ((static_cast<size_t>(width) * bits + 31) / 32) * 4;
}

}	// anonymous namespace


bool Cursor_Decode_Ico(const unsigned char * data, size_t size, CursorFrame & out,
	std::string & error)
{
	/*
	**	ICONDIR: idReserved(2) idType(2) idCount(2), then idCount ICONDIRENTRY of 16 bytes:
	**	bWidth bHeight bColorCount bReserved wPlanes/xHotspot wBitCount/yHotspot dwBytesInRes
	**	dwImageOffset. For a cursor (idType 2) the two words are the hotspot.
	*/
	if (size < 6 + 16) return Fail(error, "ICO: shorter than ICONDIR + one ICONDIRENTRY");
	if (Read_U16(data) != 0) return Fail(error, "ICO: idReserved is not 0");
	unsigned type = Read_U16(data + 2);
	if (type != 1 && type != 2) return Fail(error, "ICO: idType is neither icon (1) nor cursor (2)");
	unsigned count = Read_U16(data + 4);
	if (count < 1) return Fail(error, "ICO: idCount is 0");

	const unsigned char * entry = data + 6;
	unsigned hotspot_x = type == 2 ? Read_U16(entry + 4) : 0;
	unsigned hotspot_y = type == 2 ? Read_U16(entry + 6) : 0;
	unsigned bytes = Read_U32(entry + 8);
	unsigned offset = Read_U32(entry + 12);
	if (offset > size || bytes > size - offset) return Fail(error, "ICO: image lies outside the file");
	if (bytes < 40) return Fail(error, "ICO: image shorter than a BITMAPINFOHEADER");

	const unsigned char * image = data + offset;
	if (image[0] == 0x89 && image[1] == 'P' && image[2] == 'N' && image[3] == 'G') {
		return Fail(error, "ICO: PNG compressed frame is not supported");
	}

	/*
	**	BITMAPINFOHEADER: biSize biWidth biHeight biPlanes(2) biBitCount(2) biCompression ...
	**	biClrUsed at +32. biHeight is the doubled height (XOR + AND).
	*/
	unsigned header_size = Read_U32(image);
	if (header_size < 40) return Fail(error, "ICO: BITMAPINFOHEADER biSize < 40");
	if (header_size > bytes) return Fail(error, "ICO: BITMAPINFOHEADER overruns the image");
	int width = static_cast<int>(Read_U32(image + 4));
	int doubled_height = static_cast<int>(Read_U32(image + 8));
	unsigned bits = Read_U16(image + 14);
	unsigned compression = Read_U32(image + 16);
	unsigned colours_used = Read_U32(image + 32);

	if (width <= 0 || width > 1024) return Fail(error, "ICO: biWidth out of range");
	if (doubled_height <= 0 || doubled_height > 2048 || (doubled_height & 1) != 0) {
		return Fail(error, "ICO: biHeight is not a positive even (XOR + AND) height");
	}
	if (compression != 0) return Fail(error, "ICO: biCompression is not BI_RGB");
	if (bits != 1 && bits != 4 && bits != 8 && bits != 24 && bits != 32) {
		return Fail(error, "ICO: biBitCount is not 1, 4, 8, 24 or 32");
	}
	int height = doubled_height / 2;

	/*
	**	Palette: present for <= 8 bpp. biClrUsed of 0 means the full 2^bits table.
	*/
	size_t palette_entries = 0;
	if (bits <= 8) {
		palette_entries = colours_used != 0 ? colours_used : (static_cast<size_t>(1) << bits);
		if (palette_entries > (static_cast<size_t>(1) << bits)) {
			return Fail(error, "ICO: biClrUsed larger than the pixel format allows");
		}
	}
	const unsigned char * palette = image + header_size;
	size_t xor_stride = Dib_Stride(width, bits);
	size_t and_stride = Dib_Stride(width, 1);
	const unsigned char * xor_bits = palette + palette_entries * 4;
	const unsigned char * and_bits = xor_bits + xor_stride * height;
	size_t needed = static_cast<size_t>(and_bits - image) + and_stride * height;
	if (needed > bytes) return Fail(error, "ICO: XOR + AND bitmaps overrun the image");

	out.Width = width;
	out.Height = height;
	out.Hotspot_X = static_cast<int>(hotspot_x);
	out.Hotspot_Y = static_cast<int>(hotspot_y);
	out.Bits_Per_Pixel = static_cast<int>(bits);
	out.Pixels_BGRA.assign(static_cast<size_t>(width) * height * 4, 0);

	for (int y = 0; y < height; ++y) {
		/*
		**	DIBs are bottom-up: the first row in memory is the bottom of the image.
		*/
		const unsigned char * xor_row = xor_bits + xor_stride * (height - 1 - y);
		const unsigned char * and_row = and_bits + and_stride * (height - 1 - y);
		unsigned char * dst = &out.Pixels_BGRA[static_cast<size_t>(y) * width * 4];

		for (int x = 0; x < width; ++x, dst += 4) {
			unsigned char b = 0, g = 0, r = 0, a = 255;
			switch (bits) {
			case 32:
				b = xor_row[x * 4 + 0];
				g = xor_row[x * 4 + 1];
				r = xor_row[x * 4 + 2];
				a = xor_row[x * 4 + 3];
				break;
			case 24:
				b = xor_row[x * 3 + 0];
				g = xor_row[x * 3 + 1];
				r = xor_row[x * 3 + 2];
				break;
			default: {
				unsigned index;
				if (bits == 8) {
					index = xor_row[x];
				} else if (bits == 4) {
					index = (xor_row[x / 2] >> ((x & 1) ? 0 : 4)) & 0xF;
				} else {
					index = (xor_row[x / 8] >> (7 - (x & 7))) & 1;
				}
				if (index < palette_entries) {
					b = palette[index * 4 + 0];
					g = palette[index * 4 + 1];
					r = palette[index * 4 + 2];
				}
				break;
			}
			}

			/*
			**	Below 32 bits the AND mask is the only transparency: 1 over black is transparent,
			**	1 over a colour is Win32's screen-invert pixel, drawn opaque here. At 32 bits the
			**	alpha channel is authoritative, except that many 32-bit cursors carry an all-zero
			**	alpha channel and rely on the mask alone; that is fixed up after the loop.
			*/
			if (bits != 32) {
				bool masked = ((and_row[x / 8] >> (7 - (x & 7))) & 1) != 0;
				a = (masked && b == 0 && g == 0 && r == 0) ? 0 : 255;
			}
			dst[0] = b;
			dst[1] = g;
			dst[2] = r;
			dst[3] = a;
		}
	}

	if (bits == 32) {
		bool any_alpha = false;
		for (size_t i = 3; i < out.Pixels_BGRA.size(); i += 4) {
			if (out.Pixels_BGRA[i] != 0) { any_alpha = true; break; }
		}
		if (!any_alpha) {
			for (int y = 0; y < height; ++y) {
				const unsigned char * and_row = and_bits + and_stride * (height - 1 - y);
				unsigned char * dst = &out.Pixels_BGRA[static_cast<size_t>(y) * width * 4];
				for (int x = 0; x < width; ++x) {
					bool masked = ((and_row[x / 8] >> (7 - (x & 7))) & 1) != 0;
					dst[x * 4 + 3] = masked ? 0 : 255;
				}
			}
		}
	}

	return true;
}


namespace
{

/*
**	Walk the chunks of a RIFF list body, recursing into LIST chunks, collecting the ones the
**	decoder needs. RIFF chunks are 'tag'(4) size(4) data(size) padded to even length.
*/
struct AconChunks
{
	const unsigned char * Anih;
	size_t Anih_Size;
	const unsigned char * Seq;
	size_t Seq_Size;
	std::vector<const unsigned char *> Icons;
	std::vector<size_t> Icon_Sizes;

	AconChunks() : Anih(nullptr), Anih_Size(0), Seq(nullptr), Seq_Size(0) {}
};

bool Walk(const unsigned char * p, const unsigned char * end, AconChunks & out, std::string & error,
	int depth)
{
	if (depth > 8) return Fail(error, "ANI: LIST nesting too deep");
	while (p + 8 <= end) {
		unsigned chunk_size = Read_U32(p + 4);
		const unsigned char * body = p + 8;
		if (chunk_size > static_cast<size_t>(end - body)) {
			return Fail(error, "ANI: chunk overruns its parent");
		}
		if (Tag_Is(p, "LIST")) {
			if (chunk_size < 4) return Fail(error, "ANI: LIST shorter than its type tag");
			// The 'fram' list holds the icon chunks; 'INFO' holds strings we do not need. Both
			// are walked because the icon chunks matter wherever they are.
			if (!Walk(body + 4, body + chunk_size, out, error, depth + 1)) return false;
		} else if (Tag_Is(p, "anih")) {
			out.Anih = body;
			out.Anih_Size = chunk_size;
		} else if (Tag_Is(p, "seq ")) {
			out.Seq = body;
			out.Seq_Size = chunk_size;
		} else if (Tag_Is(p, "icon")) {
			out.Icons.push_back(body);
			out.Icon_Sizes.push_back(chunk_size);
		}
		p = body + chunk_size + (chunk_size & 1);
	}
	return true;
}

}	// anonymous namespace


bool Cursor_Decode(const unsigned char * data, size_t size, CursorFile & out, std::string & error)
{
	if (data == nullptr || size < 12) return Fail(error, "cursor file shorter than a RIFF header");

	if (!Tag_Is(data, "RIFF")) {
		/*
		**	Not RIFF: the only other thing LoadCursorFromFile() accepts is a bare .CUR.
		*/
		if (!Cursor_Decode_Ico(data, size, out.First, error)) return false;
		out.Frame_Count = 1;
		out.Step_Count = 1;
		out.Display_Rate_Jiffies = 0;
		return true;
	}

	unsigned riff_size = Read_U32(data + 4);
	if (!Tag_Is(data + 8, "ACON")) return Fail(error, "ANI: RIFF form is not ACON");
	const unsigned char * end = data + 8 + (riff_size < size - 8 ? riff_size : size - 8);

	AconChunks chunks;
	if (!Walk(data + 12, end, chunks, error, 0)) return false;
	if (chunks.Anih == nullptr) return Fail(error, "ANI: no anih chunk");

	/*
	**	ANIHEADER: cbSize cFrames cSteps cx cy cBitCount cPlanes JifRate bfAttributes, 9 DWORDs.
	**	AF_ICON (bit 0) means each frame is an embedded .CUR/.ICO; AF_SEQUENCE (bit 1) means a
	**	'seq ' chunk maps steps to frames.
	*/
	if (chunks.Anih_Size < 36) return Fail(error, "ANI: anih shorter than ANIHEADER");
	unsigned frames = Read_U32(chunks.Anih + 4);
	unsigned steps = Read_U32(chunks.Anih + 8);
	unsigned rate = Read_U32(chunks.Anih + 28);
	unsigned flags = Read_U32(chunks.Anih + 32);
	if ((flags & 1) == 0) return Fail(error, "ANI: raw bitmap frames (AF_ICON clear) not supported");
	if (frames == 0) return Fail(error, "ANI: cFrames is 0");
	if (chunks.Icons.size() < frames) {
		char text[96];
		snprintf(text, sizeof(text), "ANI: anih promises %u frames, file holds %u icon chunks",
			frames, static_cast<unsigned>(chunks.Icons.size()));
		error = text;
		return false;
	}

	/*
	**	The first displayed frame is what a static cursor shows: seq[0] when there is a sequence,
	**	frame 0 otherwise.
	*/
	unsigned first = 0;
	if ((flags & 2) != 0 && chunks.Seq != nullptr && chunks.Seq_Size >= 4) {
		first = Read_U32(chunks.Seq);
		if (first >= frames) return Fail(error, "ANI: seq[0] names a frame past cFrames");
	}

	if (!Cursor_Decode_Ico(chunks.Icons[first], chunks.Icon_Sizes[first], out.First, error)) {
		return false;
	}
	out.Frame_Count = static_cast<int>(frames);
	out.Step_Count = static_cast<int>(steps);
	out.Display_Rate_Jiffies = static_cast<int>(rate);
	return true;
}

}	// namespace WWPlatform
