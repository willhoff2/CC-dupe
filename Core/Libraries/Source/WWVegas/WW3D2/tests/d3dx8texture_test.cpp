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

/***********************************************************************************************
 *                                                                                             *
 *  Pixel tests for D3DXLoadSurfaceFromSurface() and D3DXFilterTexture(), which WW3D2's         *
 *  d3dx8texture.cpp implements off Windows and d3dx8.lib provides on Windows.                  *
 *                                                                                             *
 *  These two are pixel work, so "it returned D3D_OK" proves nothing: a box average that        *
 *  averages the wrong footprint, a 5-bit channel widened by 8 instead of by 255/31, or a pitch  *
 *  ignored, all return D3D_OK and produce a picture that is quietly wrong for the life of the   *
 *  port. Every assertion below is on the bytes.                                                *
 *                                                                                             *
 *  The surfaces and textures are implemented here rather than obtained from a device: D3DX      *
 *  itself only locks, reads and writes, and giving it plain memory is what makes it testable    *
 *  with no device, no window and no driver. The fakes are deliberately hostile in two ways --   *
 *  a pitch wider than the row, and guard bytes after each row -- because both of those are      *
 *  where a plausible implementation goes wrong.                                                *
 *                                                                                             *
 *  TWO TIERS OF ASSERTION, marked in the messages:                                             *
 *                                                                                             *
 *   [D3DX]   Documented D3DX behaviour. This file builds on Windows against d3dx8.lib too, and  *
 *            these must hold there. If one does not, the implementation here is wrong.          *
 *   [CHOICE] A choice the D3DX documentation does not pin down and that no call site in this     *
 *            engine can distinguish: the rounding of a box average, and the fact that LINEAR    *
 *            and TRIANGLE are served by the area average rather than by a tent. These are       *
 *            asserted so that they cannot drift silently, but a Windows run may legitimately    *
 *            disagree by one least-significant bit -- see docs/porting/d3dx-texture-seam.md.    *
 *                                                                                             *
 *  Run through scripts/native-d3dx8texture-test.py.                                            *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#include <d3dx8core.h>
#include <d3dx8tex.h>

#include <stdio.h>
#include <string.h>
#include <vector>

static int _Failures = 0;
static int _Checks = 0;

static void Check(bool condition, const char * what)
{
	_Checks++;
	if (!condition) {
		_Failures++;
		printf("FAIL: %s\n", what);
	}
}

static void Check_Colour(D3DCOLOR actual, D3DCOLOR expected, const char * what)
{
	_Checks++;
	if (actual != expected) {
		_Failures++;
		printf("FAIL: %s: got 0x%08x, expected 0x%08x\n", what, unsigned(actual),
			unsigned(expected));
	}
}

static void Check_HRESULT_Failed(HRESULT hr, const char * what)
{
	_Checks++;
	if (SUCCEEDED(hr)) {
		_Failures++;
		printf("FAIL: %s: returned success (0x%08x) where it must fail\n", what, unsigned(hr));
	}
}

static void Check_HRESULT_Ok(HRESULT hr, const char * what)
{
	_Checks++;
	if (FAILED(hr)) {
		_Failures++;
		printf("FAIL: %s: returned 0x%08x\n", what, unsigned(hr));
	}
}


/***********************************************************************************************
 *  A surface backed by a std::vector, with a pitch wider than its rows and a guard byte       *
 *  pattern in the slack so that writing past a row is a visible failure rather than luck.     *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static const unsigned char GUARD_BYTE = 0xcd;
static const int PITCH_SLACK = 13;			// odd, so nothing lines up by accident

static unsigned Format_Bytes(D3DFORMAT format)
{
	switch (format) {
		case D3DFMT_A8R8G8B8:
		case D3DFMT_X8R8G8B8:
			return 4;
		case D3DFMT_R8G8B8:
			return 3;
		case D3DFMT_R5G6B5:
		case D3DFMT_A1R5G5B5:
		case D3DFMT_X1R5G5B5:
		case D3DFMT_A4R4G4B4:
		case D3DFMT_X4R4G4B4:
		case D3DFMT_A8L8:
			return 2;
		case D3DFMT_A8:
		case D3DFMT_L8:
			return 1;
		default:
			return 4;						// enough room for the formats that must be rejected
	}
}


class TestSurface : public IDirect3DSurface8
{
public:
	TestSurface(unsigned width, unsigned height, D3DFORMAT format)
		: Width(width), Height(height), Format(format),
		  Bytes(Format_Bytes(format)),
		  Pitch(int(width * Format_Bytes(format)) + PITCH_SLACK),
		  References(1), Locks(0), Unlocks(0), Read_Only_Lock(false),
		  Storage(size_t(Pitch) * height, GUARD_BYTE)
	{
	}

	// Pixels, in this surface's own format, as raw memory.
	unsigned char * Row(unsigned y) { return &Storage[size_t(y) * size_t(Pitch)]; }

	void Set_Raw(unsigned x, unsigned y, unsigned value)
	{
		memcpy(Row(y) + x * Bytes, &value, Bytes);
	}

	unsigned Get_Raw(unsigned x, unsigned y)
	{
		unsigned value = 0;
		memcpy(&value, Row(y) + x * Bytes, Bytes);
		return value;
	}

	void Fill_Raw(unsigned value)
	{
		for (unsigned y = 0; y < Height; ++y) {
			for (unsigned x = 0; x < Width; ++x) Set_Raw(x, y, value);
		}
	}

	// Every byte of every row's slack still holds the guard pattern.
	bool Slack_Is_Intact()
	{
		for (unsigned y = 0; y < Height; ++y) {
			const unsigned char * slack = Row(y) + Width * Bytes;
			for (int i = 0; i < PITCH_SLACK; ++i) {
				if (slack[i] != GUARD_BYTE) return false;
			}
		}
		return true;
	}

	/*** IUnknown ***/
	HRESULT __stdcall QueryInterface(REFIID, void **) override { return E_NOINTERFACE; }
	ULONG __stdcall AddRef() override { return ++References; }
	ULONG __stdcall Release() override { return --References; }

	/*** IDirect3DSurface8 ***/
	HRESULT __stdcall GetDevice(IDirect3DDevice8 ** device) override
	{
		if (device != nullptr) *device = nullptr;
		return E_NOTIMPL;
	}
	HRESULT __stdcall SetPrivateData(REFGUID, CONST void *, DWORD, DWORD) override
	{
		return E_NOTIMPL;
	}
	HRESULT __stdcall GetPrivateData(REFGUID, void *, DWORD *) override { return E_NOTIMPL; }
	HRESULT __stdcall FreePrivateData(REFGUID) override { return E_NOTIMPL; }
	HRESULT __stdcall GetContainer(REFIID, void **) override { return E_NOTIMPL; }

	HRESULT __stdcall GetDesc(D3DSURFACE_DESC * desc) override
	{
		if (desc == nullptr) return D3DERR_INVALIDCALL;
		memset(desc, 0, sizeof(*desc));
		desc->Format = Format;
		desc->Type = D3DRTYPE_SURFACE;
		desc->Usage = 0;
		desc->Pool = D3DPOOL_MANAGED;
		desc->Size = UINT(size_t(Pitch) * Height);
		desc->Width = Width;
		desc->Height = Height;
		return D3D_OK;
	}

	HRESULT __stdcall LockRect(D3DLOCKED_RECT * locked, CONST RECT * rect, DWORD flags) override
	{
		if (locked == nullptr) return D3DERR_INVALIDCALL;
		Locks++;
		Read_Only_Lock = (flags & D3DLOCK_READONLY) != 0;
		unsigned char * bits = &Storage[0];
		if (rect != nullptr) {
			bits += size_t(rect->top) * size_t(Pitch) + size_t(rect->left) * Bytes;
		}
		locked->Pitch = Pitch;
		locked->pBits = bits;
		return D3D_OK;
	}

	HRESULT __stdcall UnlockRect() override
	{
		Unlocks++;
		return D3D_OK;
	}

	unsigned Width;
	unsigned Height;
	D3DFORMAT Format;
	unsigned Bytes;
	int Pitch;
	ULONG References;
	int Locks;
	int Unlocks;
	bool Read_Only_Lock;
	std::vector<unsigned char> Storage;
};


class TestTexture : public IDirect3DTexture8
{
public:
	TestTexture(unsigned width, unsigned height, D3DFORMAT format, unsigned levels,
			D3DRESOURCETYPE type = D3DRTYPE_TEXTURE)
		: Type(type), References(1)
	{
		for (unsigned level = 0; level < levels; ++level) {
			Levels.push_back(new TestSurface(width, height, format));
			if (width > 1) width /= 2;
			if (height > 1) height /= 2;
		}
	}

	~TestTexture()
	{
		for (size_t i = 0; i < Levels.size(); ++i) delete Levels[i];
	}

	TestSurface * Level(unsigned level) { return Levels[level]; }

	/*** IUnknown ***/
	HRESULT __stdcall QueryInterface(REFIID, void **) override { return E_NOINTERFACE; }
	ULONG __stdcall AddRef() override { return ++References; }
	ULONG __stdcall Release() override { return --References; }

	/*** IDirect3DResource8 ***/
	HRESULT __stdcall GetDevice(IDirect3DDevice8 ** device) override
	{
		if (device != nullptr) *device = nullptr;
		return E_NOTIMPL;
	}
	HRESULT __stdcall SetPrivateData(REFGUID, CONST void *, DWORD, DWORD) override
	{
		return E_NOTIMPL;
	}
	HRESULT __stdcall GetPrivateData(REFGUID, void *, DWORD *) override { return E_NOTIMPL; }
	HRESULT __stdcall FreePrivateData(REFGUID) override { return E_NOTIMPL; }
	DWORD __stdcall SetPriority(DWORD) override { return 0; }
	DWORD __stdcall GetPriority() override { return 0; }
	void __stdcall PreLoad() override {}
	D3DRESOURCETYPE __stdcall GetType() override { return Type; }

	/*** IDirect3DBaseTexture8 ***/
	DWORD __stdcall SetLOD(DWORD) override { return 0; }
	DWORD __stdcall GetLOD() override { return 0; }
	DWORD __stdcall GetLevelCount() override { return DWORD(Levels.size()); }

	/*** IDirect3DTexture8 ***/
	HRESULT __stdcall GetLevelDesc(UINT level, D3DSURFACE_DESC * desc) override
	{
		if (level >= Levels.size()) return D3DERR_INVALIDCALL;
		return Levels[level]->GetDesc(desc);
	}

	HRESULT __stdcall GetSurfaceLevel(UINT level, IDirect3DSurface8 ** surface) override
	{
		if (surface == nullptr || level >= Levels.size()) return D3DERR_INVALIDCALL;
		Levels[level]->AddRef();
		*surface = Levels[level];
		return D3D_OK;
	}

	HRESULT __stdcall LockRect(UINT level, D3DLOCKED_RECT * locked, CONST RECT * rect,
		DWORD flags) override
	{
		if (level >= Levels.size()) return D3DERR_INVALIDCALL;
		return Levels[level]->LockRect(locked, rect, flags);
	}

	HRESULT __stdcall UnlockRect(UINT level) override
	{
		if (level >= Levels.size()) return D3DERR_INVALIDCALL;
		return Levels[level]->UnlockRect();
	}

	HRESULT __stdcall AddDirtyRect(CONST RECT *) override { return D3D_OK; }

	D3DRESOURCETYPE Type;
	ULONG References;
	std::vector<TestSurface *> Levels;
};


static HRESULT Blit(TestSurface & destination, TestSurface & source, DWORD filter,
	D3DCOLOR colour_key = 0, const RECT * destination_rect = nullptr,
	const RECT * source_rect = nullptr)
{
	return D3DXLoadSurfaceFromSurface(&destination, nullptr, destination_rect, &source, nullptr,
		source_rect, filter, colour_key);
}


/***********************************************************************************************
 *  The 2:1 box average, which is what every mip chain in this engine is made of.               *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
static void Test_Box_Average_2_To_1()
{
	TestSurface source(2, 2, D3DFMT_A8R8G8B8);
	TestSurface destination(1, 1, D3DFMT_A8R8G8B8);

	// Four exactly-averaging colours: the average is 0x40406080 with nothing to round.
	source.Set_Raw(0, 0, 0x00000000);
	source.Set_Raw(1, 0, 0x40408000);
	source.Set_Raw(0, 1, 0x80800080);
	source.Set_Raw(1, 1, 0xc0c08080);
	destination.Fill_Raw(0xdeadbeef);

	Check_HRESULT_Ok(Blit(destination, source, D3DX_FILTER_BOX), "a 2:1 box filter succeeds");
	Check_Colour(destination.Get_Raw(0, 0), 0x60604040,
		"[D3DX] a 2:1 box filter averages all four source pixels per channel");
	Check(destination.Slack_Is_Intact(), "a blit does not write past the end of a row");

	/*
	**	Each channel is averaged on its own and unpremultiplied: a transparent white and an opaque
	**	white average to a half-transparent white, not to a darkened one.
	*/
	source.Set_Raw(0, 0, 0x00ffffff);
	source.Set_Raw(1, 0, 0x00ffffff);
	source.Set_Raw(0, 1, 0xffffffff);
	source.Set_Raw(1, 1, 0xffffffff);
	Check_HRESULT_Ok(Blit(destination, source, D3DX_FILTER_BOX), "the alpha case succeeds");
	Check_Colour(destination.Get_Raw(0, 0), 0x80ffffff,
		"[D3DX] the average is unpremultiplied: alpha averages without touching the colours");

	// 0xff and 0x00 average to 0x7f.5, which is where the rounding rule becomes visible.
	source.Set_Raw(0, 0, 0xff000000);
	source.Set_Raw(1, 0, 0xff000000);
	source.Set_Raw(0, 1, 0xffff0000);
	source.Set_Raw(1, 1, 0xffff0000);
	Check_HRESULT_Ok(Blit(destination, source, D3DX_FILTER_BOX), "the rounding case succeeds");
	Check_Colour(destination.Get_Raw(0, 0), 0xff800000,
		"[CHOICE] an exact half rounds up (0 and 255 average to 128, not 127)");
}


/***********************************************************************************************
 *  Pitch, footprint and non-square shapes.                                                     *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
static void Test_Footprint()
{
	/*
	**	A 4x4 whose quadrants are constant: each destination pixel of the 2x2 must be its own
	**	quadrant's colour and nothing else. An implementation that averaged the wrong 2x2 window,
	**	or that walked rows by width instead of by pitch, gets a different answer for at least one
	**	of them.
	*/
	TestSurface source(4, 4, D3DFMT_A8R8G8B8);
	TestSurface destination(2, 2, D3DFMT_A8R8G8B8);

	static const D3DCOLOR quadrant[2][2] = {
		{ 0xff112233, 0xff445566 },
		{ 0xff778899, 0xffaabbcc },
	};
	for (unsigned y = 0; y < 4; ++y) {
		for (unsigned x = 0; x < 4; ++x) source.Set_Raw(x, y, quadrant[y / 2][x / 2]);
	}

	Check_HRESULT_Ok(Blit(destination, source, D3DX_FILTER_BOX), "a 4x4 to 2x2 box filter");
	for (unsigned y = 0; y < 2; ++y) {
		for (unsigned x = 0; x < 2; ++x) {
			Check_Colour(destination.Get_Raw(x, y), quadrant[y][x],
				"[D3DX] each destination pixel averages only its own 2x2 source footprint");
		}
	}

	/*
	**	The last mip levels of a non-square texture: 4x1 down to 2x1 averages horizontally only,
	**	and a 1-pixel dimension stays 1 pixel rather than reading a row that is not there.
	*/
	TestSurface wide(4, 1, D3DFMT_A8R8G8B8);
	TestSurface narrow(2, 1, D3DFMT_A8R8G8B8);
	wide.Set_Raw(0, 0, 0xff000000);
	wide.Set_Raw(1, 0, 0xff020000);
	wide.Set_Raw(2, 0, 0xff000400);
	wide.Set_Raw(3, 0, 0xff000600);
	Check_HRESULT_Ok(Blit(narrow, wide, D3DX_FILTER_BOX), "a 4x1 to 2x1 box filter");
	Check_Colour(narrow.Get_Raw(0, 0), 0xff010000,
		"[D3DX] a one-pixel-high surface averages along the row only");
	Check_Colour(narrow.Get_Raw(1, 0), 0xff000500,
		"[D3DX] the second pixel of a 4x1 to 2x1 filter");

	// Equal sizes with a filter set: a copy, which is what dx8wrapper's level-0 blit relies on.
	TestSurface same_source(3, 3, D3DFMT_A8R8G8B8);
	TestSurface same_destination(3, 3, D3DFMT_A8R8G8B8);
	for (unsigned y = 0; y < 3; ++y) {
		for (unsigned x = 0; x < 3; ++x) same_source.Set_Raw(x, y, 0xff000000 | (y * 3 + x));
	}
	Check_HRESULT_Ok(Blit(same_destination, same_source, D3DX_FILTER_BOX), "an equal-size blit");
	bool identical = true;
	for (unsigned y = 0; y < 3; ++y) {
		for (unsigned x = 0; x < 3; ++x) {
			if (same_destination.Get_Raw(x, y) != same_source.Get_Raw(x, y)) identical = false;
		}
	}
	Check(identical, "[D3DX] an equal-size box filter copies the pixels unchanged");
}


/***********************************************************************************************
 *  Format conversion, which is the other half of what these entry points are for.              *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
static void Test_Format_Conversion()
{
	/*
	**	Widening: a 5-bit or 4-bit channel at full scale must reach 0xff, not 0xf8 or 0xf0. This is
	**	the mistake that leaves a light grey sky slightly too dark and is invisible in isolation.
	*/
	{
		TestSurface source(1, 1, D3DFMT_R5G6B5);
		TestSurface destination(1, 1, D3DFMT_A8R8G8B8);
		source.Set_Raw(0, 0, 0xffff);				// all channels full
		Check_HRESULT_Ok(Blit(destination, source, D3DX_FILTER_NONE), "R5G6B5 to A8R8G8B8");
		Check_Colour(destination.Get_Raw(0, 0), 0xffffffff,
			"[D3DX] a full 5/6-bit channel widens to 0xff and the missing alpha reads opaque");

		source.Set_Raw(0, 0, (16u << 11) | (32u << 5) | 16u);	// half scale in each channel
		Check_HRESULT_Ok(Blit(destination, source, D3DX_FILTER_NONE), "a half-scale R5G6B5 pixel");
		Check_Colour(destination.Get_Raw(0, 0), 0xff848284,
			"[D3DX] channels widen by bit replication (16/31 -> 0x84, 32/63 -> 0x82)");
	}

	{
		TestSurface source(1, 1, D3DFMT_A4R4G4B4);
		TestSurface destination(1, 1, D3DFMT_A8R8G8B8);
		source.Set_Raw(0, 0, 0x8123);
		Check_HRESULT_Ok(Blit(destination, source, D3DX_FILTER_NONE), "A4R4G4B4 to A8R8G8B8");
		Check_Colour(destination.Get_Raw(0, 0), 0x88112233,
			"[D3DX] a 4-bit channel widens by replication: 8 -> 0x88, 1 -> 0x11");
	}

	{
		TestSurface source(1, 1, D3DFMT_A1R5G5B5);
		TestSurface destination(1, 1, D3DFMT_A8R8G8B8);
		source.Set_Raw(0, 0, 0x0000);				// alpha bit clear
		Check_HRESULT_Ok(Blit(destination, source, D3DX_FILTER_NONE), "A1R5G5B5 to A8R8G8B8");
		Check_Colour(destination.Get_Raw(0, 0), 0x00000000,
			"[D3DX] a clear 1-bit alpha widens to fully transparent");
		source.Set_Raw(0, 0, 0x8000);
		Check_HRESULT_Ok(Blit(destination, source, D3DX_FILTER_NONE), "an opaque A1R5G5B5 pixel");
		Check_Colour(destination.Get_Raw(0, 0), 0xff000000,
			"[D3DX] a set 1-bit alpha widens to fully opaque");
	}

	{
		// X8R8G8B8 has no alpha, so it reads opaque whatever the unused byte holds.
		TestSurface source(1, 1, D3DFMT_X8R8G8B8);
		TestSurface destination(1, 1, D3DFMT_A8R8G8B8);
		source.Set_Raw(0, 0, 0x00204060);
		Check_HRESULT_Ok(Blit(destination, source, D3DX_FILTER_NONE), "X8R8G8B8 to A8R8G8B8");
		Check_Colour(destination.Get_Raw(0, 0), 0xff204060,
			"[D3DX] a format with no alpha channel reads as opaque");
	}

	{
		// R8G8B8's three bytes are blue, green, red in memory order.
		TestSurface source(1, 1, D3DFMT_R8G8B8);
		TestSurface destination(1, 1, D3DFMT_A8R8G8B8);
		source.Row(0)[0] = 0x33;
		source.Row(0)[1] = 0x22;
		source.Row(0)[2] = 0x11;
		Check_HRESULT_Ok(Blit(destination, source, D3DX_FILTER_NONE), "R8G8B8 to A8R8G8B8");
		Check_Colour(destination.Get_Raw(0, 0), 0xff112233,
			"[D3DX] R8G8B8 stores blue, green, red in ascending bytes");
	}

	{
		// Narrowing keeps the high bits, which is what the engine's own converters do.
		TestSurface source(1, 1, D3DFMT_A8R8G8B8);
		TestSurface destination(1, 1, D3DFMT_R5G6B5);
		source.Set_Raw(0, 0, 0xff8f8f8f);
		Check_HRESULT_Ok(Blit(destination, source, D3DX_FILTER_NONE), "A8R8G8B8 to R5G6B5");
		Check_Colour(destination.Get_Raw(0, 0), (0x11u << 11) | (0x23u << 5) | 0x11u,
			"[CHOICE] narrowing truncates to the high bits rather than rounding");
	}

	{
		// L8 and A8 read into all three colours and into alpha respectively.
		TestSurface luminance(1, 1, D3DFMT_L8);
		TestSurface destination(1, 1, D3DFMT_A8R8G8B8);
		luminance.Row(0)[0] = 0x40;
		Check_HRESULT_Ok(Blit(destination, luminance, D3DX_FILTER_NONE), "L8 to A8R8G8B8");
		Check_Colour(destination.Get_Raw(0, 0), 0xff404040,
			"[D3DX] L8 expands its luminance into all three colour channels");

		TestSurface alpha(1, 1, D3DFMT_A8);
		alpha.Row(0)[0] = 0x40;
		Check_HRESULT_Ok(Blit(destination, alpha, D3DX_FILTER_NONE), "A8 to A8R8G8B8");
		Check_Colour(destination.Get_Raw(0, 0), 0x40000000,
			"[D3DX] A8 expands into alpha with black colour");
	}

	{
		// A conversion combined with a filter: both halves have to happen, in that order.
		TestSurface source(2, 2, D3DFMT_A8R8G8B8);
		TestSurface destination(1, 1, D3DFMT_R5G6B5);
		source.Set_Raw(0, 0, 0xffff0000);
		source.Set_Raw(1, 0, 0xffff0000);
		source.Set_Raw(0, 1, 0xff000000);
		source.Set_Raw(1, 1, 0xff000000);
		Check_HRESULT_Ok(Blit(destination, source, D3DX_FILTER_BOX), "filter and convert at once");
		Check_Colour(destination.Get_Raw(0, 0), (0x10u << 11),
			"[D3DX] the average is taken at full precision and narrowed once, not per pixel");
	}
}


/***********************************************************************************************
 *  The filters that are not the box filter.                                                    *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
static void Test_Filters()
{
	/*
	**	D3DX_FILTER_NONE: "no scaling or filtering will take place. Pixels outside the bounds of
	**	the source image are assumed to be transparent black." SurfaceClass::Copy() relies on the
	**	first half; the second is what stops a smaller source from leaving stale pixels behind.
	*/
	{
		TestSurface source(2, 2, D3DFMT_A8R8G8B8);
		TestSurface destination(4, 4, D3DFMT_A8R8G8B8);
		source.Fill_Raw(0xff112233);
		destination.Fill_Raw(0xdeadbeef);
		Check_HRESULT_Ok(Blit(destination, source, D3DX_FILTER_NONE), "an unscaled copy");
		Check_Colour(destination.Get_Raw(1, 1), 0xff112233,
			"[D3DX] FILTER_NONE copies pixel for pixel without scaling");
		Check_Colour(destination.Get_Raw(3, 3), 0x00000000,
			"[D3DX] FILTER_NONE leaves pixels outside the source transparent black");
	}

	/*
	**	D3DX_FILTER_POINT downsamples by taking one source pixel, so a checkerboard stays hard
	**	rather than turning grey. This is the difference the caller is asking for when it passes
	**	POINT rather than BOX.
	*/
	{
		TestSurface source(4, 4, D3DFMT_A8R8G8B8);
		TestSurface destination(2, 2, D3DFMT_A8R8G8B8);
		for (unsigned y = 0; y < 4; ++y) {
			for (unsigned x = 0; x < 4; ++x) {
				source.Set_Raw(x, y, ((x + y) % 2 == 0) ? 0xff000000 : 0xffffffff);
			}
		}
		Check_HRESULT_Ok(Blit(destination, source, D3DX_FILTER_POINT), "a point-filtered downscale");
		bool hard = true;
		for (unsigned y = 0; y < 2; ++y) {
			for (unsigned x = 0; x < 2; ++x) {
				const D3DCOLOR pixel = destination.Get_Raw(x, y);
				if (pixel != 0xff000000 && pixel != 0xffffffff) hard = false;
			}
		}
		Check(hard, "[D3DX] FILTER_POINT takes a source pixel rather than averaging");

		// And the box filter on the same checkerboard is the grey the average demands.
		Check_HRESULT_Ok(Blit(destination, source, D3DX_FILTER_BOX), "a box-filtered checkerboard");
		Check_Colour(destination.Get_Raw(0, 0), 0xff808080,
			"[CHOICE] a two-black-two-white footprint averages to 0x80");
	}

	/*
	**	D3DX_FILTER_TRIANGLE, which SurfaceClass::Copy() passes for its stretched path. Off
	**	Windows it is served by the area average, so what is asserted is that the image is scaled
	**	correctly and stays within the source's range -- not that it matches a tent filter, which
	**	it does not.
	*/
	{
		TestSurface source(4, 4, D3DFMT_A8R8G8B8);
		TestSurface destination(2, 2, D3DFMT_A8R8G8B8);
		for (unsigned y = 0; y < 4; ++y) {
			for (unsigned x = 0; x < 4; ++x) {
				source.Set_Raw(x, y, 0xff000000 | (0x10u * (y * 4 + x)));
			}
		}
		Check_HRESULT_Ok(Blit(destination, source, D3DX_FILTER_TRIANGLE), "a triangle-filtered blit");
		bool monotonic = destination.Get_Raw(0, 0) < destination.Get_Raw(1, 0)
			&& destination.Get_Raw(1, 0) < destination.Get_Raw(0, 1)
			&& destination.Get_Raw(0, 1) < destination.Get_Raw(1, 1);
		Check(monotonic,
			"[D3DX] a filtered downscale of a gradient is still a gradient in the same direction");
		Check((destination.Get_Raw(0, 0) & 0xff) >= 0x00 && (destination.Get_Raw(1, 1) & 0xff)
			<= 0xf0, "[D3DX] a filtered downscale stays inside the source's range");
		Check_Colour(destination.Get_Raw(0, 0), 0xff000028,
			"[CHOICE] TRIANGLE is served by the area average off Windows, not by a tent");
	}

	/*
	**	D3DX_DEFAULT, which dx8wrapper.cpp passes for both of D3DXLoadSurfaceFromSurface()'s
	**	filter arguments. It is ULONG_MAX, so it is 64 bits wide here and 32 on Windows and arrives
	**	truncated through a DWORD parameter -- a real trap, because an implementation that compared
	**	against the unnarrowed constant would never recognise it, read its low three bits as a
	**	filter type of 7 and fall out of the switch. It must behave as the documented default,
	**	which for a downscale is the same filtered result as TRIANGLE.
	*/
	{
		TestSurface source(4, 4, D3DFMT_A8R8G8B8);
		TestSurface destination(2, 2, D3DFMT_A8R8G8B8);
		for (unsigned y = 0; y < 4; ++y) {
			for (unsigned x = 0; x < 4; ++x) {
				source.Set_Raw(x, y, 0xff000000 | (0x10u * (y * 4 + x)));
			}
		}
		Check_HRESULT_Ok(Blit(destination, source, DWORD(D3DX_DEFAULT)),
			"a blit with D3DX_DEFAULT for the filter");
		Check_Colour(destination.Get_Raw(0, 0), 0xff000028,
			"[D3DX] D3DX_DEFAULT filters like TRIANGLE rather than falling out of the switch");
	}

	/*
	**	Magnification with a filter: every destination pixel must come from somewhere, and the
	**	nearest source pixel is the only defensible answer for an average over less than a pixel.
	*/
	{
		TestSurface source(2, 2, D3DFMT_A8R8G8B8);
		TestSurface destination(4, 4, D3DFMT_A8R8G8B8);
		source.Set_Raw(0, 0, 0xff010101);
		source.Set_Raw(1, 0, 0xff020202);
		source.Set_Raw(0, 1, 0xff030303);
		source.Set_Raw(1, 1, 0xff040404);
		destination.Fill_Raw(0xdeadbeef);
		Check_HRESULT_Ok(Blit(destination, source, D3DX_FILTER_BOX), "a magnifying blit");
		Check_Colour(destination.Get_Raw(0, 0), 0xff010101,
			"[D3DX] magnification samples the covering source pixel");
		Check_Colour(destination.Get_Raw(3, 3), 0xff040404,
			"[D3DX] the far corner of a magnified image comes from the far corner of the source");
	}
}


/***********************************************************************************************
 *  Rectangles and the colour key.                                                              *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
static void Test_Rectangles_And_Colour_Key()
{
	/*
	**	SurfaceClass::Draw() and Copy() both pass explicit rectangles, and both expect everything
	**	outside the destination rectangle to be left alone -- it is a partial-surface update, not a
	**	whole-surface one.
	*/
	{
		TestSurface source(4, 4, D3DFMT_A8R8G8B8);
		TestSurface destination(4, 4, D3DFMT_A8R8G8B8);
		source.Fill_Raw(0xff112233);
		destination.Fill_Raw(0xff000000);

		RECT source_rect = { 0, 0, 2, 2 };
		RECT destination_rect = { 2, 2, 4, 4 };
		Check_HRESULT_Ok(Blit(destination, source, D3DX_FILTER_NONE, 0, &destination_rect,
			&source_rect), "a rectangle-to-rectangle copy");
		Check_Colour(destination.Get_Raw(3, 3), 0xff112233,
			"[D3DX] the destination rectangle receives the source rectangle");
		Check_Colour(destination.Get_Raw(0, 0), 0xff000000,
			"[D3DX] pixels outside the destination rectangle are untouched");
		Check_Colour(destination.Get_Raw(1, 2), 0xff000000,
			"[D3DX] the rectangle's left edge is respected");
		Check(destination.Slack_Is_Intact(), "a rectangle blit stays inside its rows");
	}

	// A rectangle that scales: 4x4 of the source into 2x2 of the destination.
	{
		TestSurface source(4, 4, D3DFMT_A8R8G8B8);
		TestSurface destination(4, 4, D3DFMT_A8R8G8B8);
		source.Fill_Raw(0xff204060);
		destination.Fill_Raw(0xff000000);
		RECT source_rect = { 0, 0, 4, 4 };
		RECT destination_rect = { 0, 0, 2, 2 };
		Check_HRESULT_Ok(Blit(destination, source, D3DX_FILTER_BOX, 0, &destination_rect,
			&source_rect), "a scaling rectangle blit");
		Check_Colour(destination.Get_Raw(1, 1), 0xff204060,
			"[D3DX] a scaling rectangle blit fills its rectangle");
		Check_Colour(destination.Get_Raw(2, 2), 0xff000000,
			"[D3DX] a scaling rectangle blit writes nothing outside its rectangle");
	}

	// An out-of-bounds rectangle is a failure, not a buffer overrun.
	{
		TestSurface source(4, 4, D3DFMT_A8R8G8B8);
		TestSurface destination(4, 4, D3DFMT_A8R8G8B8);
		RECT bad = { 0, 0, 8, 8 };
		Check_HRESULT_Failed(Blit(destination, source, D3DX_FILTER_NONE, 0, &bad, nullptr),
			"a destination rectangle larger than the surface is rejected");
		Check(destination.Slack_Is_Intact(), "a rejected rectangle wrote nothing");
	}

	/*
	**	The colour key: matching source pixels become transparent black *before* filtering, so a
	**	filtered edge fades towards transparency instead of towards the key colour.
	*/
	{
		TestSurface source(2, 2, D3DFMT_A8R8G8B8);
		TestSurface destination(1, 1, D3DFMT_A8R8G8B8);
		source.Set_Raw(0, 0, 0xffff00ff);			// the key
		source.Set_Raw(1, 0, 0xffff00ff);
		source.Set_Raw(0, 1, 0xff000000);
		source.Set_Raw(1, 1, 0xff000000);
		Check_HRESULT_Ok(Blit(destination, source, D3DX_FILTER_BOX, 0xffff00ff),
			"a colour-keyed filter");
		Check_Colour(destination.Get_Raw(0, 0), 0x80000000,
			"[D3DX] keyed pixels become transparent black before the average, not after");

		// And with no key set, the same magenta is just a colour.
		Check_HRESULT_Ok(Blit(destination, source, D3DX_FILTER_BOX, 0), "an unkeyed filter");
		Check_Colour(destination.Get_Raw(0, 0), 0xff800080,
			"[D3DX] a zero colour key keys nothing");
	}
}


/***********************************************************************************************
 *  D3DXFilterTexture(): the mip walk W3DTreeBuffer, TerrainTex and dx8wrapper rely on.          *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
static void Test_Filter_Texture()
{
	/*
	**	A full chain from an 8x8: level 1 is the average of 2x2 blocks of level 0, level 2 of
	**	level 1 and so on. Filtering every level from level 0 instead of from its predecessor
	**	gives the same answer for a smooth image and a different one here, because the pattern is
	**	chosen so that the two disagree.
	*/
	TestTexture texture(8, 8, D3DFMT_A8R8G8B8, 4);

	Check(texture.GetLevelCount() == 4, "the test texture has four levels");

	// Level 0: a vertical ramp, one value per row, so every average is exact.
	for (unsigned y = 0; y < 8; ++y) {
		for (unsigned x = 0; x < 8; ++x) {
			texture.Level(0)->Set_Raw(x, y, 0xff000000 | (0x10u * y));
		}
	}
	for (unsigned level = 1; level < 4; ++level) texture.Level(level)->Fill_Raw(0xdeadbeef);

	Check_HRESULT_Ok(D3DXFilterTexture(&texture, nullptr, 0, D3DX_FILTER_BOX),
		"a four-level box mip walk");

	// Rows 0 and 1 (0x00 and 0x10) average to 0x08, and so on down the chain.
	Check_Colour(texture.Level(1)->Get_Raw(0, 0), 0xff000008,
		"[D3DX] level 1 is the 2x2 average of level 0");
	// Level 1's last row averages level 0's rows 6 and 7: (0x60 + 0x70) / 2.
	Check_Colour(texture.Level(1)->Get_Raw(3, 3), 0xff000068,
		"[D3DX] the whole of level 1 is filtered, not just its first pixel");
	Check_Colour(texture.Level(2)->Get_Raw(0, 0), 0xff000018,
		"[D3DX] level 2 is filtered from level 1, not from level 0");
	Check_Colour(texture.Level(3)->Get_Raw(0, 0), 0xff000038,
		"[D3DX] the last level is the average of the whole image");
	Check(texture.Level(1)->Slack_Is_Intact() && texture.Level(2)->Slack_Is_Intact()
		&& texture.Level(3)->Slack_Is_Intact(), "the mip walk stays inside its rows");

	// Level 0 is the source and must not be touched.
	Check_Colour(texture.Level(0)->Get_Raw(0, 0), 0xff000000,
		"[D3DX] the source level is left alone");

	/*
	**	SrcLevel selects where the chain starts: levels above it keep whatever they held, and the
	**	level below it is filtered from it rather than from level 0.
	*/
	{
		TestTexture from_one(8, 8, D3DFMT_A8R8G8B8, 3);
		from_one.Level(0)->Fill_Raw(0xff000000);
		from_one.Level(1)->Fill_Raw(0xff102030);
		from_one.Level(2)->Fill_Raw(0xdeadbeef);
		Check_HRESULT_Ok(D3DXFilterTexture(&from_one, nullptr, 1, D3DX_FILTER_BOX),
			"a mip walk starting at level 1");
		Check_Colour(from_one.Level(2)->Get_Raw(0, 0), 0xff102030,
			"[D3DX] SrcLevel 1 filters level 2 from level 1");
		Check_Colour(from_one.Level(1)->Get_Raw(0, 0), 0xff102030,
			"[D3DX] SrcLevel is a source: it is not itself rewritten");
	}

	// A single-level texture is a walk with nothing to do, not an error.
	{
		TestTexture single(4, 4, D3DFMT_A8R8G8B8, 1);
		Check_HRESULT_Ok(D3DXFilterTexture(&single, nullptr, 0, D3DX_FILTER_BOX),
			"a single-level texture succeeds with nothing to filter");
	}

	// A non-square chain, which is where a mip level's dimensions stop halving independently.
	{
		TestTexture oblong(4, 2, D3DFMT_A8R8G8B8, 3);
		oblong.Level(0)->Fill_Raw(0xff404040);
		oblong.Level(1)->Fill_Raw(0xdeadbeef);
		oblong.Level(2)->Fill_Raw(0xdeadbeef);
		Check_HRESULT_Ok(D3DXFilterTexture(&oblong, nullptr, 0, D3DX_FILTER_BOX),
			"a 4x2 mip walk");
		Check_Colour(oblong.Level(2)->Get_Raw(0, 0), 0xff404040,
			"[D3DX] a constant image filters to itself at every level, whatever the shape");
	}
}


/***********************************************************************************************
 *  What is refused rather than guessed. Each of these must fail; a silent success here is the   *
 *  failure mode the whole slice exists to avoid.                                                *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
static void Test_Refusals()
{
	{
		TestSurface source(4, 4, D3DFMT_DXT1);
		TestSurface destination(2, 2, D3DFMT_DXT1);
		Check_HRESULT_Failed(Blit(destination, source, D3DX_FILTER_BOX),
			"a DXT1 blit is refused rather than producing rubbish");
	}

	{
		TestSurface source(4, 4, D3DFMT_A8R8G8B8);
		TestSurface destination(4, 4, D3DFMT_L8);
		Check_HRESULT_Failed(Blit(destination, source, D3DX_FILTER_NONE),
			"a colour-to-luminance conversion is refused");
	}

	{
		TestSurface source(4, 4, D3DFMT_A8R8G8B8);
		TestSurface destination(4, 4, D3DFMT_A8R8G8B8);
		PALETTEENTRY palette[256];
		memset(palette, 0, sizeof(palette));
		Check_HRESULT_Failed(D3DXLoadSurfaceFromSurface(&destination, nullptr, nullptr, &source,
			palette, nullptr, D3DX_FILTER_NONE, 0), "a palettised source is refused");
		Check_HRESULT_Failed(D3DXLoadSurfaceFromSurface(&destination, palette, nullptr, &source,
			nullptr, nullptr, D3DX_FILTER_NONE, 0), "a palettised destination is refused");
	}

	{
		TestSurface surface(4, 4, D3DFMT_A8R8G8B8);
		Check_HRESULT_Failed(D3DXLoadSurfaceFromSurface(nullptr, nullptr, nullptr, &surface,
			nullptr, nullptr, D3DX_FILTER_NONE, 0), "a null destination surface is refused");
		Check_HRESULT_Failed(D3DXLoadSurfaceFromSurface(&surface, nullptr, nullptr, nullptr,
			nullptr, nullptr, D3DX_FILTER_NONE, 0), "a null source surface is refused");
	}

	{
		Check_HRESULT_Failed(D3DXFilterTexture(nullptr, nullptr, 0, D3DX_FILTER_BOX),
			"a null texture is refused");

		TestTexture cube(4, 4, D3DFMT_A8R8G8B8, 2, D3DRTYPE_CUBETEXTURE);
		Check_HRESULT_Failed(D3DXFilterTexture(&cube, nullptr, 0, D3DX_FILTER_BOX),
			"a cube texture is refused rather than filtered as a 2D texture");

		TestTexture texture(4, 4, D3DFMT_A8R8G8B8, 2);
		Check_HRESULT_Failed(D3DXFilterTexture(&texture, nullptr, 5, D3DX_FILTER_BOX),
			"a SrcLevel past the end of the chain is refused");

		PALETTEENTRY palette[256];
		memset(palette, 0, sizeof(palette));
		Check_HRESULT_Failed(D3DXFilterTexture(&texture, palette, 0, D3DX_FILTER_BOX),
			"a palettised texture is refused");
	}
}


/***********************************************************************************************
 *  Locking: every lock is released, including on the paths that fail. A leaked lock on a        *
 *  managed texture is a hang or a corrupt upload later, far from here.                          *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
static void Test_Locking()
{
	TestSurface source(4, 4, D3DFMT_A8R8G8B8);
	TestSurface destination(2, 2, D3DFMT_A8R8G8B8);

	Check_HRESULT_Ok(Blit(destination, source, D3DX_FILTER_BOX), "a blit for the lock count");
	Check(source.Locks == 1 && source.Unlocks == 1 && destination.Locks == 1
		&& destination.Unlocks == 1, "each surface is locked once and unlocked once");
	Check(source.Read_Only_Lock, "the source is locked read-only");
	Check(!destination.Read_Only_Lock, "the destination is not locked read-only");

	RECT bad = { 0, 0, 99, 99 };
	Check_HRESULT_Failed(Blit(destination, source, D3DX_FILTER_NONE, 0, &bad, nullptr),
		"a rejected rectangle for the lock count");
	Check(source.Locks == source.Unlocks && destination.Locks == destination.Unlocks,
		"a failed blit still unlocks both surfaces");

	TestTexture texture(4, 4, D3DFMT_A8R8G8B8, 3);
	Check_HRESULT_Ok(D3DXFilterTexture(&texture, nullptr, 0, D3DX_FILTER_BOX),
		"a mip walk for the reference count");
	Check(texture.Level(0)->References == 1 && texture.Level(1)->References == 1
		&& texture.Level(2)->References == 1,
		"the mip walk releases every surface it took a reference to");
}


int main()
{
	Test_Box_Average_2_To_1();
	Test_Footprint();
	Test_Format_Conversion();
	Test_Filters();
	Test_Rectangles_And_Colour_Key();
	Test_Filter_Texture();
	Test_Refusals();
	Test_Locking();

	printf("%d checks, %d failure(s)\n", _Checks, _Failures);
	return (_Failures == 0) ? 0 : 1;
}
