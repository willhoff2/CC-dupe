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

// D3DXLoadSurfaceFromSurface() and D3DXFilterTexture() off Windows, where they come from
// d3dx8.lib. Both are pixel work rather than API plumbing, so scripts/native-d3dx8texture-test.py
// asserts the pixels: the whole point of a mip chain is that the small levels look like the big
// one, and a wrong average or a channel expanded by the wrong scale is a picture that is slightly
// wrong forever rather than a crash anyone can find.
//
// WHAT THESE ARE ASKED TO DO IN THIS ENGINE
//
//   missingtexture.cpp     builds the magenta/black "missing texture" checkerboard at level 0 in
//                          A8R8G8B8 and then walks the mip chain, each level filtered from the one
//                          above with D3DX_FILTER_BOX. Source and destination formats are equal
//                          and the ratio is exactly 2:1.
//   W3DTreeBuffer.cpp,     upload level 0, then D3DXFilterTexture(..., 0, D3DX_FILTER_BOX) for the
//   TerrainTex.cpp         rest of the chain. Same shape: equal formats, 2:1.
//   dx8wrapper.cpp         one whole-surface D3DX_FILTER_BOX copy into a fresh texture's level 0
//                          (equal sizes, so no filtering happens at all) followed by the same
//                          D3DXFilterTexture() mip walk.
//   surfaceclass.cpp       Draw()/Copy() between two surfaces with explicit RECTs:
//                          D3DX_FILTER_NONE for the unscaled case and D3DX_FILTER_TRIANGLE for the
//                          stretched one, and here the two surfaces may differ in format.
//
// WHAT IS REPRODUCED, AND WHAT IS ASSERTED INSTEAD OF D3DX
//
// Everything goes through the D3DCOLOR A8R8G8B8 layout, one pixel at a time.
//
//   * Channel widening replicates the high bits: a 5-bit 31 becomes 255, a 4-bit 8 becomes 0x88.
//     That is the standard normalising expansion (v * 255 / max, exactly, for these widths) and it
//     is what the engine's own converters in bitmaphandler.h do, so a round trip through here and
//     a round trip through the engine agree.
//   * Channel narrowing keeps the high bits (truncation). D3DX is documented only as "converts",
//     and a rounding narrowing differs from a truncating one by one least-significant bit; the
//     test asserts truncation because that is what the engine's own converters do, and because no
//     call site above narrows -- every call site either keeps the format or widens it.
//   * A format with no alpha reads as opaque, per the D3DX colour-key documentation's "opaque
//     black == 0xff000000". A8 reads with colour 0, and L8/A8L8 read luminance into all three
//     colour channels.
//   * D3DX_FILTER_BOX averages the source pixels covering each destination pixel, per channel,
//     unpremultiplied, rounding halves up. For the 2:1 case every call site above uses this is an
//     exact four-pixel average and the rounding is the only free choice, which the test pins. For
//     a ratio that is not an integer the footprint is whole source pixels rather than partial
//     coverage, which is an approximation of D3DX and says so out loud once per run.
//   * D3DX_FILTER_POINT takes the nearest source pixel. D3DX_FILTER_LINEAR and
//     D3DX_FILTER_TRIANGLE are served by the same area average as BOX, which is *not* a tent
//     filter: it says so out loud once per run rather than pretending. The one caller,
//     SurfaceClass::Copy()'s stretched path, gets a correctly-scaled image that is slightly
//     sharper than Windows would give it.
//   * D3DX_FILTER_NONE copies without scaling and leaves destination pixels with no source pixel
//     behind them transparent black, which is what the documentation specifies.
//   * A non-zero ColorKey turns source pixels equal to it into transparent black before
//     filtering, so a filtered edge fades towards transparency rather than towards the key colour.
//   * The mirror and dither flags are ignored, loudly. Nothing in the engine passes them.
//
// NOT reproduced, and failing rather than guessing: palettised formats (a non-NULL PALETTEENTRY*
// is rejected), the DXT block formats, the bump/luminance-pair formats such as U8V8, depth
// formats, cube and volume textures, and any conversion whose destination is A8, L8 or A8L8 from a
// source of a different format -- colour-to-luminance has no one right answer and no call site
// needs it. Every one of those returns D3DERR_INVALIDCALL after a line on stderr; DX8_ErrorCode()
// at the call sites turns that into the engine's usual D3D error report.

#if !defined(_WIN32)

#include "d3dx8texture.h"

#include <stdio.h>
#include <string.h>

namespace
{

/*
**	Loads and stores go through memcpy rather than a cast: a surface's pitch is the driver's
**	business and nothing guarantees it is a multiple of the pixel size, so a row can start at an
**	odd address. Every compiler this port uses turns these into the same single instruction a cast
**	would have produced, without the misaligned access.
*/
inline unsigned Load32(const unsigned char * at)
{
	unsigned value;
	memcpy(&value, at, sizeof(value));
	return value;
}

inline unsigned Load16(const unsigned char * at)
{
	unsigned short value;
	memcpy(&value, at, sizeof(value));
	return value;
}

inline void Store32(unsigned char * at, unsigned value)
{
	memcpy(at, &value, sizeof(value));
}

inline void Store16(unsigned char * at, unsigned value)
{
	const unsigned short narrowed = (unsigned short)value;
	memcpy(at, &narrowed, sizeof(narrowed));
}

/*
**	One line on stderr, the same shape WWPlatform::Win32::Report_Stub() uses, for the cases this
**	implementation refuses or approximates. Callers keep their own `static bool` so that a filter
**	inside a per-frame copy says so once rather than every frame.
*/
void Report(bool & reported, const char * api, const char * detail)
{
	if (reported) return;
	reported = true;
	fprintf(stderr, "!!! %s(): %s\n", api, detail);
}

inline unsigned Expand(unsigned value, unsigned bits)
{
	/*
	**	Bit replication, which for 1, 4, 5 and 6 bit channels is exactly value * 255 / max.
	*/
	unsigned result = value;
	unsigned filled = bits;
	while (filled < 8) {
		result = (result << bits) | value;
		filled += bits;
	}
	return (result >> (filled - 8)) & 0xff;
}

inline unsigned char * Row(const D3DX8Texture::SurfaceView & view, unsigned y)
{
	return static_cast<unsigned char *>(view.Bits) + static_cast<int>(y) * view.Pitch;
}

}	// anonymous namespace


namespace D3DX8Texture
{

unsigned Bytes_Per_Pixel(D3DFORMAT format)
{
	switch (format) {
		case D3DFMT_A8R8G8B8:
		case D3DFMT_X8R8G8B8:
			return 4;
		case D3DFMT_R8G8B8:
			return 3;
		case D3DFMT_R5G6B5:
		case D3DFMT_X1R5G5B5:
		case D3DFMT_A1R5G5B5:
		case D3DFMT_A4R4G4B4:
		case D3DFMT_X4R4G4B4:
		case D3DFMT_A8L8:
			return 2;
		case D3DFMT_A8:
		case D3DFMT_L8:
			return 1;
		default:
			return 0;
	}
}


bool Format_Is_Supported(D3DFORMAT format)
{
	return Bytes_Per_Pixel(format) != 0;
}


static bool Is_Luminance_Or_Alpha_Only(D3DFORMAT format)
{
	return format == D3DFMT_A8 || format == D3DFMT_L8 || format == D3DFMT_A8L8;
}


D3DCOLOR Read_Pixel(const SurfaceView & view, unsigned x, unsigned y)
{
	const unsigned char * pixel = Row(view, y) + x * Bytes_Per_Pixel(view.Format);

	switch (view.Format) {
		case D3DFMT_A8R8G8B8:
			return Load32(pixel);

		case D3DFMT_X8R8G8B8:
			return 0xff000000u | (Load32(pixel) & 0x00ffffffu);

		case D3DFMT_R8G8B8:
			// Little-endian byte order for this format is blue, green, red.
			return 0xff000000u | (unsigned(pixel[2]) << 16) | (unsigned(pixel[1]) << 8)
				| unsigned(pixel[0]);

		case D3DFMT_R5G6B5: {
			const unsigned value = Load16(pixel);
			return 0xff000000u
				| (Expand((value >> 11) & 0x1f, 5) << 16)
				| (Expand((value >> 5) & 0x3f, 6) << 8)
				| Expand(value & 0x1f, 5);
		}

		case D3DFMT_A1R5G5B5:
		case D3DFMT_X1R5G5B5: {
			const unsigned value = Load16(pixel);
			const unsigned alpha = (view.Format == D3DFMT_X1R5G5B5)
				? 0xffu : (((value >> 15) & 0x1) != 0 ? 0xffu : 0x00u);
			return (alpha << 24)
				| (Expand((value >> 10) & 0x1f, 5) << 16)
				| (Expand((value >> 5) & 0x1f, 5) << 8)
				| Expand(value & 0x1f, 5);
		}

		case D3DFMT_A4R4G4B4:
		case D3DFMT_X4R4G4B4: {
			const unsigned value = Load16(pixel);
			const unsigned alpha = (view.Format == D3DFMT_X4R4G4B4)
				? 0xffu : Expand((value >> 12) & 0xf, 4);
			return (alpha << 24)
				| (Expand((value >> 8) & 0xf, 4) << 16)
				| (Expand((value >> 4) & 0xf, 4) << 8)
				| Expand(value & 0xf, 4);
		}

		case D3DFMT_A8:
			return unsigned(pixel[0]) << 24;

		case D3DFMT_L8: {
			const unsigned luminance = pixel[0];
			return 0xff000000u | (luminance << 16) | (luminance << 8) | luminance;
		}

		case D3DFMT_A8L8: {
			// The high byte of this two byte format is alpha.
			const unsigned value = Load16(pixel);
			const unsigned luminance = value & 0xff;
			return ((value >> 8) << 24) | (luminance << 16) | (luminance << 8) | luminance;
		}

		default:
			return 0;
	}
}


void Write_Pixel(const SurfaceView & view, unsigned x, unsigned y, D3DCOLOR argb)
{
	unsigned char * pixel = Row(view, y) + x * Bytes_Per_Pixel(view.Format);

	const unsigned alpha = (argb >> 24) & 0xff;
	const unsigned red = (argb >> 16) & 0xff;
	const unsigned green = (argb >> 8) & 0xff;
	const unsigned blue = argb & 0xff;

	switch (view.Format) {
		case D3DFMT_A8R8G8B8:
			Store32(pixel, argb);
			break;

		case D3DFMT_X8R8G8B8:
			Store32(pixel, 0xff000000u | (argb & 0x00ffffffu));
			break;

		case D3DFMT_R8G8B8:
			pixel[0] = (unsigned char)blue;
			pixel[1] = (unsigned char)green;
			pixel[2] = (unsigned char)red;
			break;

		case D3DFMT_R5G6B5:
			Store16(pixel, ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3));
			break;

		case D3DFMT_A1R5G5B5:
			Store16(pixel,
				((alpha >> 7) << 15) | ((red >> 3) << 10) | ((green >> 3) << 5) | (blue >> 3));
			break;

		case D3DFMT_X1R5G5B5:
			Store16(pixel, (1u << 15) | ((red >> 3) << 10) | ((green >> 3) << 5) | (blue >> 3));
			break;

		case D3DFMT_A4R4G4B4:
			Store16(pixel,
				((alpha >> 4) << 12) | ((red >> 4) << 8) | ((green >> 4) << 4) | (blue >> 4));
			break;

		case D3DFMT_X4R4G4B4:
			Store16(pixel, (0xfu << 12) | ((red >> 4) << 8) | ((green >> 4) << 4) | (blue >> 4));
			break;

		case D3DFMT_A8:
			pixel[0] = (unsigned char)alpha;
			break;

		case D3DFMT_L8:
			// Only reached for a same-format copy, where red == green == blue == luminance.
			pixel[0] = (unsigned char)red;
			break;

		case D3DFMT_A8L8:
			Store16(pixel, (alpha << 8) | red);
			break;

		default:
			break;
	}
}


bool Blit(const SurfaceView & destination, const SurfaceView & source, DWORD filter,
	D3DCOLOR colour_key)
{
	static bool reported_format = false;
	static bool reported_luminance = false;
	static bool reported_tent = false;
	static bool reported_partial = false;
	static bool reported_flags = false;
	static bool reported_filter = false;

	if (!Format_Is_Supported(source.Format) || !Format_Is_Supported(destination.Format)) {
		Report(reported_format, "D3DXLoadSurfaceFromSurface",
			"only the uncompressed non-palettised D3D8 formats are converted off Windows; "
			"the DXT, bump and depth formats are not");
		return false;
	}

	if (Is_Luminance_Or_Alpha_Only(destination.Format) && destination.Format != source.Format) {
		Report(reported_luminance, "D3DXLoadSurfaceFromSurface",
			"converting colour to an A8/L8/A8L8 destination is not implemented off Windows");
		return false;
	}

	if (destination.Width == 0 || destination.Height == 0) return true;
	if (source.Width == 0 || source.Height == 0) return false;

	/*
	**	D3DX_DEFAULT is ULONG_MAX, which is 64 bits wide on this platform and 32 on Windows, so a
	**	caller that passes it through a DWORD parameter delivers the truncated value. Narrow the
	**	constant the same way rather than comparing against the wider one, which never matches.
	*/
	if (filter == DWORD(D3DX_DEFAULT)) filter = D3DX_FILTER_TRIANGLE;

	if ((filter & 0xffff0000u) != 0) {
		Report(reported_flags, "D3DXLoadSurfaceFromSurface",
			"the D3DX_FILTER mirror and dither flags are ignored off Windows");
	}

	const DWORD type = filter & 0x7;
	bool point_sample = false;

	switch (type) {
		case D3DX_FILTER_NONE:
		case D3DX_FILTER_POINT:
			point_sample = true;
			break;
		case D3DX_FILTER_BOX:
			break;
		case D3DX_FILTER_LINEAR:
		case D3DX_FILTER_TRIANGLE:
			Report(reported_tent, "D3DXLoadSurfaceFromSurface",
				"the linear and triangle filters are served by the box/area average off Windows, "
				"which is sharper than a tent filter");
			break;
		default:
			Report(reported_filter, "D3DXLoadSurfaceFromSurface",
				"unknown D3DX_FILTER type; the box/area average was used");
			break;
	}

	const bool copy_without_scaling = (type == D3DX_FILTER_NONE);

	if (!point_sample
			&& ((source.Width % destination.Width) != 0 || (source.Height % destination.Height) != 0)
			&& (source.Width > destination.Width || source.Height > destination.Height)) {
		Report(reported_partial, "D3DXLoadSurfaceFromSurface",
			"a non-integer scale averages whole source pixels rather than weighting partial "
			"coverage, so it is an approximation of D3DX's box filter");
	}

	for (unsigned y = 0; y < destination.Height; ++y) {
		for (unsigned x = 0; x < destination.Width; ++x) {

			if (copy_without_scaling) {
				/*
				**	"No scaling or filtering will take place. Pixels outside the bounds of the
				**	source image are assumed to be transparent black."
				*/
				D3DCOLOR pixel = 0;
				if (x < source.Width && y < source.Height) {
					pixel = Read_Pixel(source, x, y);
					if (colour_key != 0 && pixel == colour_key) pixel = 0;
				}
				Write_Pixel(destination, x, y, pixel);
				continue;
			}

			if (point_sample) {
				const unsigned sx = (x * source.Width) / destination.Width;
				const unsigned sy = (y * source.Height) / destination.Height;
				D3DCOLOR pixel = Read_Pixel(source, sx, sy);
				if (colour_key != 0 && pixel == colour_key) pixel = 0;
				Write_Pixel(destination, x, y, pixel);
				continue;
			}

			/*
			**	The source footprint of this destination pixel, at least one pixel wide and high
			**	so that magnification degenerates to point sampling rather than to nothing.
			*/
			const unsigned x0 = (x * source.Width) / destination.Width;
			const unsigned y0 = (y * source.Height) / destination.Height;
			unsigned x1 = ((x + 1) * source.Width + destination.Width - 1) / destination.Width;
			unsigned y1 = ((y + 1) * source.Height + destination.Height - 1) / destination.Height;
			if (x1 <= x0) x1 = x0 + 1;
			if (y1 <= y0) y1 = y0 + 1;
			if (x1 > source.Width) x1 = source.Width;
			if (y1 > source.Height) y1 = source.Height;

			unsigned sum_a = 0;
			unsigned sum_r = 0;
			unsigned sum_g = 0;
			unsigned sum_b = 0;
			unsigned count = 0;

			for (unsigned sy = y0; sy < y1; ++sy) {
				for (unsigned sx = x0; sx < x1; ++sx) {
					D3DCOLOR pixel = Read_Pixel(source, sx, sy);
					if (colour_key != 0 && pixel == colour_key) pixel = 0;
					sum_a += (pixel >> 24) & 0xff;
					sum_r += (pixel >> 16) & 0xff;
					sum_g += (pixel >> 8) & 0xff;
					sum_b += pixel & 0xff;
					++count;
				}
			}

			const unsigned half = count / 2;
			const D3DCOLOR averaged = (((sum_a + half) / count) << 24)
				| (((sum_r + half) / count) << 16)
				| (((sum_g + half) / count) << 8)
				| ((sum_b + half) / count);
			Write_Pixel(destination, x, y, averaged);
		}
	}

	return true;
}

}	// namespace D3DX8Texture


/***********************************************************************************************
 *                                                                                             *
 *  The D3DX entry points themselves. Locking is the only device-side work here: the surfaces   *
 *  are locked, handed to Blit() as plain memory, and unlocked. Nothing touches an              *
 *  IDirect3DDevice8, so the D3D8 call-surface gate is unaffected.                              *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

namespace
{

bool View_From_Locked_Surface(D3DX8Texture::SurfaceView & view, const D3DSURFACE_DESC & desc,
	const D3DLOCKED_RECT & locked, const RECT * rect)
{
	view.Bits = locked.pBits;
	view.Pitch = locked.Pitch;
	view.Width = desc.Width;
	view.Height = desc.Height;
	view.Format = desc.Format;

	if (rect == nullptr) return true;

	/*
	**	A sub-rectangle is just a smaller view whose origin has moved: the surface is locked
	**	whole, so the offset is applied here rather than passed down.
	*/
	if (rect->left < 0 || rect->top < 0 || rect->right < rect->left || rect->bottom < rect->top) {
		return false;
	}
	if (unsigned(rect->right) > desc.Width || unsigned(rect->bottom) > desc.Height) return false;

	const unsigned bytes = D3DX8Texture::Bytes_Per_Pixel(desc.Format);
	if (bytes == 0) return false;

	view.Bits = static_cast<unsigned char *>(locked.pBits)
		+ rect->top * locked.Pitch + rect->left * int(bytes);
	view.Width = unsigned(rect->right - rect->left);
	view.Height = unsigned(rect->bottom - rect->top);
	return true;
}

}	// anonymous namespace


HRESULT WINAPI D3DXLoadSurfaceFromSurface(
	LPDIRECT3DSURFACE8 pDestSurface,
	CONST PALETTEENTRY * pDestPalette,
	CONST RECT * pDestRect,
	LPDIRECT3DSURFACE8 pSrcSurface,
	CONST PALETTEENTRY * pSrcPalette,
	CONST RECT * pSrcRect,
	DWORD Filter,
	D3DCOLOR ColorKey)
{
	static bool reported_palette = false;

	if (pDestSurface == nullptr || pSrcSurface == nullptr) return D3DERR_INVALIDCALL;

	if (pDestPalette != nullptr || pSrcPalette != nullptr) {
		Report(reported_palette, "D3DXLoadSurfaceFromSurface",
			"palettised surfaces are not converted off Windows");
		return D3DERR_INVALIDCALL;
	}

	D3DSURFACE_DESC destination_desc;
	D3DSURFACE_DESC source_desc;
	HRESULT hr = pDestSurface->GetDesc(&destination_desc);
	if (FAILED(hr)) return hr;
	hr = pSrcSurface->GetDesc(&source_desc);
	if (FAILED(hr)) return hr;

	D3DLOCKED_RECT source_locked;
	hr = pSrcSurface->LockRect(&source_locked, nullptr, D3DLOCK_READONLY);
	if (FAILED(hr)) return hr;

	D3DLOCKED_RECT destination_locked;
	hr = pDestSurface->LockRect(&destination_locked, nullptr, 0);
	if (FAILED(hr)) {
		pSrcSurface->UnlockRect();
		return hr;
	}

	D3DX8Texture::SurfaceView destination_view;
	D3DX8Texture::SurfaceView source_view;
	bool ok = View_From_Locked_Surface(destination_view, destination_desc, destination_locked,
			pDestRect)
		&& View_From_Locked_Surface(source_view, source_desc, source_locked, pSrcRect);

	if (ok) {
		ok = D3DX8Texture::Blit(destination_view, source_view, Filter, ColorKey);
	}

	pDestSurface->UnlockRect();
	pSrcSurface->UnlockRect();

	return ok ? D3D_OK : D3DERR_INVALIDCALL;
}


HRESULT WINAPI D3DXFilterTexture(
	LPDIRECT3DBASETEXTURE8 pBaseTexture,
	CONST PALETTEENTRY * pPalette,
	UINT SrcLevel,
	DWORD Filter)
{
	static bool reported_palette = false;
	static bool reported_type = false;

	if (pBaseTexture == nullptr) return D3DERR_INVALIDCALL;

	if (pPalette != nullptr) {
		Report(reported_palette, "D3DXFilterTexture",
			"palettised textures are not filtered off Windows");
		return D3DERR_INVALIDCALL;
	}

	if (pBaseTexture->GetType() != D3DRTYPE_TEXTURE) {
		Report(reported_type, "D3DXFilterTexture",
			"only 2D textures are filtered off Windows; cube and volume textures are not, and "
			"nothing in the engine asks for them");
		return D3DERR_INVALIDCALL;
	}

	/*
	**	GetType() has just said what this is, so the downcast is the type check COM would do with
	**	QueryInterface(); the D3D8 interfaces are single-inheritance abstract classes.
	*/
	IDirect3DTexture8 * texture = static_cast<IDirect3DTexture8 *>(pBaseTexture);
	const DWORD levels = texture->GetLevelCount();

	if (SrcLevel >= levels) return D3DERR_INVALIDCALL;

	/*
	**	Each level is filtered from the level above rather than all of them from SrcLevel: that is
	**	both what D3DX does and the only way the result is a box average of the whole footprint.
	*/
	for (DWORD level = SrcLevel + 1; level < levels; ++level) {
		IDirect3DSurface8 * source = nullptr;
		IDirect3DSurface8 * destination = nullptr;

		HRESULT hr = texture->GetSurfaceLevel(level - 1, &source);
		if (SUCCEEDED(hr)) {
			hr = texture->GetSurfaceLevel(level, &destination);
		}
		if (SUCCEEDED(hr)) {
			hr = D3DXLoadSurfaceFromSurface(destination, nullptr, nullptr, source, nullptr,
				nullptr, Filter, 0);
		}

		if (destination != nullptr) destination->Release();
		if (source != nullptr) source->Release();

		if (FAILED(hr)) return hr;
	}

	return D3D_OK;
}

#endif // !_WIN32
