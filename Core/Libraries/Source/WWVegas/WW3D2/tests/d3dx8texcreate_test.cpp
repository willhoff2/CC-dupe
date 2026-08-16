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
 *  What the D3DX 8 texture-creation helpers decide before they call the device, and the error   *
 *  strings the engine's two error loggers print.                                               *
 *                                                                                             *
 *  These are the arguments a texture is created with. Every defect here is invisible at link    *
 *  time and nearly invisible at run time: a mip level count one too high is a device rejection  *
 *  in one place and a black bottom mip in another; a power-of-two rounding that rounds DOWN is  *
 *  every texture in the game one step blurrier; a clamp that breaks squareness is a cube map    *
 *  that is not a cube. So the plan is asserted as values, per shape.                            *
 *                                                                                             *
 *  ASSERTED HERE:                                                                              *
 *    - extents pass through untouched on a permissive device, and UNSPECIFIED_EXTENT/zero are         *
 *      substituted the way D3DXCheckTextureRequirements documents (the other extent, else 256); *
 *    - D3DPTEXTURECAPS_POW2 rounds UP, D3DPTEXTURECAPS_NONPOW2CONDITIONAL turns that off, and   *
 *      D3DPTEXTURECAPS_SQUAREONLY takes the larger extent;                                     *
 *    - MaxTextureWidth/Height clamp last, and squareness survives the clamp;                    *
 *    - a cube texture is square on both edges and a volume texture is bounded by                *
 *      MaxVolumeExtent, not by the 2D maxima;                                                  *
 *    - MipLevels 0/UNSPECIFIED_EXTENT becomes the full chain (and the chain is floor(log2)+1, counting *
 *      depth only for a volume texture), a device without the matching mip cap gets 1, and an    *
 *      over-long request is capped at the chain length;                                        *
 *    - format candidates: the requested format is first, alpha-carrying formats fall back        *
 *      through alpha-carrying ones only, and D3DFMT_UNKNOWN starts at A8R8G8B8;                 *
 *    - the retry policy: a format rejection is retried for a managed texture and NEVER for a    *
 *      render target or a depth/stencil surface, and D3DERR_OUTOFVIDEOMEMORY is never retried,  *
 *      because dx8wrapper.cpp's render-target path reads those two codes itself;                *
 *    - D3DXGetErrorStringA(): known codes by name, an unknown code as its hex value, a short    *
 *      buffer truncated with a terminator and no write past its end, and a null buffer          *
 *      rejected rather than dereferenced;                                                      *
 *    - D3DXCreateTextureFromFileExA() is a refusal that cannot be mistaken for success -- a     *
 *      failing HRESULT with a null out-parameter, which is the contract its one caller relies   *
 *      on when it falls back to the missing-texture checkerboard.                              *
 *                                                                                             *
 *  NOT ASSERTED HERE: the device calls themselves. There is no IDirect3DDevice8 implementation   *
 *  off Windows yet, so there is nothing to call and nothing to fake that would prove anything    *
 *  the plan does not already prove. When a backend exists, the loop in d3dx8texcreate.cpp is     *
 *  ten lines and this is the file to extend.                                                   *
 *                                                                                             *
 *  Run through scripts/native-d3dx8-entrypoints-test.py.                                       *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#include "d3dx8texcreate.h"

// D3DX_DEFAULT is ULONG_MAX, which does not fit the UINT parameters these entry points take;
// what the implementation sees is the truncation, so that is what the test passes.
static const unsigned UNSPECIFIED_EXTENT = 0xffffffffu;

#include <stdio.h>
#include <string.h>

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

static void Check_Equal(unsigned actual, unsigned expected, const char * what)
{
	_Checks++;
	if (actual != expected) {
		_Failures++;
		printf("FAIL: %s -- expected %u, got %u\n", what, expected, actual);
	}
}

/*
**	A device that allows everything, which is the baseline every other case is a departure from.
*/
static D3DCAPS8 Permissive_Caps()
{
	D3DCAPS8 caps;
	memset(&caps, 0, sizeof(caps));
	caps.TextureCaps = D3DPTEXTURECAPS_MIPMAP | D3DPTEXTURECAPS_MIPCUBEMAP
		| D3DPTEXTURECAPS_MIPVOLUMEMAP | D3DPTEXTURECAPS_CUBEMAP | D3DPTEXTURECAPS_VOLUMEMAP;
	caps.MaxTextureWidth = 4096;
	caps.MaxTextureHeight = 4096;
	caps.MaxVolumeExtent = 256;
	return caps;
}

static D3DX8TexCreate::RequestType Request(unsigned width, unsigned height, unsigned depth,
	unsigned levels, DWORD usage, D3DFORMAT format)
{
	D3DX8TexCreate::RequestType request;
	request.Width = width;
	request.Height = height;
	request.Depth = depth;
	request.MipLevels = levels;
	request.Usage = usage;
	request.Format = format;
	return request;
}

static void Test_Extents()
{
	const D3DCAPS8 permissive = Permissive_Caps();

	D3DX8TexCreate::PlanType plan = D3DX8TexCreate::Plan(permissive,
		Request(100, 60, 1, 1, 0, D3DFMT_A8R8G8B8), D3DX8TexCreate::KIND_TEXTURE);
	Check_Equal(plan.Width, 100, "a permissive device leaves the width alone");
	Check_Equal(plan.Height, 60, "a permissive device leaves the height alone");

	plan = D3DX8TexCreate::Plan(permissive,
		Request(UNSPECIFIED_EXTENT, UNSPECIFIED_EXTENT, 1, 1, 0, D3DFMT_A8R8G8B8),
		D3DX8TexCreate::KIND_TEXTURE);
	Check_Equal(plan.Width, 256, "both extents unspecified become 256");
	Check_Equal(plan.Height, 256, "both extents unspecified become 256");

	plan = D3DX8TexCreate::Plan(permissive,
		Request(UNSPECIFIED_EXTENT, 64, 1, 1, 0, D3DFMT_A8R8G8B8), D3DX8TexCreate::KIND_TEXTURE);
	Check_Equal(plan.Width, 64, "an unspecified width takes the height");

	plan = D3DX8TexCreate::Plan(permissive,
		Request(48, 0, 1, 1, 0, D3DFMT_A8R8G8B8), D3DX8TexCreate::KIND_TEXTURE);
	Check_Equal(plan.Height, 48, "a zero height takes the width rather than reaching the device");

	D3DCAPS8 pow2 = permissive;
	pow2.TextureCaps |= D3DPTEXTURECAPS_POW2;
	plan = D3DX8TexCreate::Plan(pow2, Request(100, 60, 1, 1, 0, D3DFMT_A8R8G8B8),
		D3DX8TexCreate::KIND_TEXTURE);
	Check_Equal(plan.Width, 128, "POW2 rounds the width up, never down");
	Check_Equal(plan.Height, 64, "POW2 rounds the height up, never down");

	D3DCAPS8 conditional = pow2;
	conditional.TextureCaps |= D3DPTEXTURECAPS_NONPOW2CONDITIONAL;
	plan = D3DX8TexCreate::Plan(conditional, Request(100, 60, 1, 1, 0, D3DFMT_A8R8G8B8),
		D3DX8TexCreate::KIND_TEXTURE);
	Check_Equal(plan.Width, 100, "NONPOW2CONDITIONAL means POW2 does not apply");

	D3DCAPS8 square = pow2;
	square.TextureCaps |= D3DPTEXTURECAPS_SQUAREONLY;
	plan = D3DX8TexCreate::Plan(square, Request(128, 32, 1, 1, 0, D3DFMT_A8R8G8B8),
		D3DX8TexCreate::KIND_TEXTURE);
	Check_Equal(plan.Width, 128, "SQUAREONLY keeps the larger extent");
	Check_Equal(plan.Height, 128, "SQUAREONLY squares up rather than down");

	D3DCAPS8 small = permissive;
	small.MaxTextureWidth = 256;
	small.MaxTextureHeight = 256;
	plan = D3DX8TexCreate::Plan(small, Request(1024, 512, 1, 1, 0, D3DFMT_A8R8G8B8),
		D3DX8TexCreate::KIND_TEXTURE);
	Check_Equal(plan.Width, 256, "the device maximum clamps the width");
	Check_Equal(plan.Height, 256, "the device maximum clamps the height");

	small.TextureCaps |= D3DPTEXTURECAPS_SQUAREONLY;
	small.MaxTextureHeight = 128;
	plan = D3DX8TexCreate::Plan(small, Request(1024, 512, 1, 1, 0, D3DFMT_A8R8G8B8),
		D3DX8TexCreate::KIND_TEXTURE);
	Check_Equal(plan.Width, 128, "a clamp that breaks squareness is squared again");
	Check_Equal(plan.Height, 128, "a clamp that breaks squareness is squared again");
}

static void Test_Cube_And_Volume()
{
	const D3DCAPS8 permissive = Permissive_Caps();

	D3DX8TexCreate::PlanType plan = D3DX8TexCreate::Plan(permissive,
		Request(100, 100, 1, 1, 0, D3DFMT_A8R8G8B8), D3DX8TexCreate::KIND_CUBE);
	Check_Equal(plan.Width, 128, "a cube edge is rounded to a power of two");
	Check_Equal(plan.Height, plan.Width, "a cube texture's edges are equal");

	D3DCAPS8 small_cube = permissive;
	small_cube.MaxTextureWidth = 64;
	small_cube.MaxTextureHeight = 32;
	plan = D3DX8TexCreate::Plan(small_cube, Request(256, 256, 1, 1, 0, D3DFMT_A8R8G8B8),
		D3DX8TexCreate::KIND_CUBE);
	Check_Equal(plan.Width, 32, "a clamped cube edge takes the smaller maximum");
	Check_Equal(plan.Height, 32, "a clamped cube texture is still a cube");

	plan = D3DX8TexCreate::Plan(permissive, Request(300, 300, 300, 1, 0, D3DFMT_A8R8G8B8),
		D3DX8TexCreate::KIND_VOLUME);
	Check_Equal(plan.Width, 256, "a volume texture is bounded by MaxVolumeExtent");
	Check_Equal(plan.Depth, 256, "a volume texture's depth is bounded by MaxVolumeExtent");

	plan = D3DX8TexCreate::Plan(permissive, Request(64, 64, 5, 1, 0, D3DFMT_A8R8G8B8),
		D3DX8TexCreate::KIND_VOLUME);
	Check_Equal(plan.Depth, 8, "a volume depth is rounded up to a power of two");

	plan = D3DX8TexCreate::Plan(permissive, Request(64, 64, UNSPECIFIED_EXTENT, 1, 0, D3DFMT_A8R8G8B8),
		D3DX8TexCreate::KIND_VOLUME);
	Check_Equal(plan.Depth, 1, "an unspecified volume depth is one slice");
}

static void Test_Mip_Levels()
{
	const D3DCAPS8 permissive = Permissive_Caps();

	Check_Equal(D3DX8TexCreate::Full_Mip_Chain(1, 1, 1), 1, "a 1x1 texture has one level");
	Check_Equal(D3DX8TexCreate::Full_Mip_Chain(256, 256, 1), 9, "256x256 has nine levels");
	Check_Equal(D3DX8TexCreate::Full_Mip_Chain(256, 64, 1), 9,
		"the chain follows the larger extent");
	Check_Equal(D3DX8TexCreate::Full_Mip_Chain(100, 60, 1), 7,
		"a non-power-of-two chain is floor(log2)+1");

	D3DX8TexCreate::PlanType plan = D3DX8TexCreate::Plan(permissive,
		Request(256, 256, 1, 0, 0, D3DFMT_A8R8G8B8), D3DX8TexCreate::KIND_TEXTURE);
	Check_Equal(plan.MipLevels, 9, "zero levels means the whole chain");

	plan = D3DX8TexCreate::Plan(permissive,
		Request(256, 256, 1, UNSPECIFIED_EXTENT, 0, D3DFMT_A8R8G8B8), D3DX8TexCreate::KIND_TEXTURE);
	Check_Equal(plan.MipLevels, 9, "UNSPECIFIED_EXTENT levels means the whole chain");

	plan = D3DX8TexCreate::Plan(permissive,
		Request(256, 256, 1, 4, 0, D3DFMT_A8R8G8B8), D3DX8TexCreate::KIND_TEXTURE);
	Check_Equal(plan.MipLevels, 4, "an explicit level count is kept");

	plan = D3DX8TexCreate::Plan(permissive,
		Request(4, 4, 1, 99, 0, D3DFMT_A8R8G8B8), D3DX8TexCreate::KIND_TEXTURE);
	Check_Equal(plan.MipLevels, 3, "more levels than the chain has is capped at the chain");

	D3DCAPS8 no_mips = permissive;
	no_mips.TextureCaps &= ~DWORD(D3DPTEXTURECAPS_MIPMAP);
	plan = D3DX8TexCreate::Plan(no_mips, Request(256, 256, 1, 0, 0, D3DFMT_A8R8G8B8),
		D3DX8TexCreate::KIND_TEXTURE);
	Check_Equal(plan.MipLevels, 1, "a device with no mip support gets a single level");

	/*
	**	The mip caps are per shape: a device may mip a 2D texture and not a cube map.
	*/
	D3DCAPS8 no_cube_mips = permissive;
	no_cube_mips.TextureCaps &= ~DWORD(D3DPTEXTURECAPS_MIPCUBEMAP);
	plan = D3DX8TexCreate::Plan(no_cube_mips, Request(256, 256, 1, 0, 0, D3DFMT_A8R8G8B8),
		D3DX8TexCreate::KIND_CUBE);
	Check_Equal(plan.MipLevels, 1, "no MIPCUBEMAP means one cube level");
	plan = D3DX8TexCreate::Plan(no_cube_mips, Request(256, 256, 1, 0, 0, D3DFMT_A8R8G8B8),
		D3DX8TexCreate::KIND_TEXTURE);
	Check_Equal(plan.MipLevels, 9, "and it does not affect a 2D texture");

	plan = D3DX8TexCreate::Plan(permissive, Request(64, 64, 256, 0, 0, D3DFMT_A8R8G8B8),
		D3DX8TexCreate::KIND_VOLUME);
	Check_Equal(plan.MipLevels, 9, "a volume chain counts the depth");
}

static void Test_Formats()
{
	const D3DCAPS8 permissive = Permissive_Caps();

	D3DX8TexCreate::PlanType plan = D3DX8TexCreate::Plan(permissive,
		Request(64, 64, 1, 1, 0, D3DFMT_A1R5G5B5), D3DX8TexCreate::KIND_TEXTURE);
	Check(plan.FormatCount >= 2, "there is a fallback format to try");
	Check(plan.Formats[0] == D3DFMT_A1R5G5B5, "the requested format is tried first");
	bool alpha_only = true;
	for (unsigned i = 0; i < plan.FormatCount; ++i) {
		if (plan.Formats[i] == D3DFMT_X8R8G8B8 || plan.Formats[i] == D3DFMT_R5G6B5) {
			alpha_only = false;
		}
	}
	Check(alpha_only, "an alpha format never falls back to one without alpha");

	plan = D3DX8TexCreate::Plan(permissive, Request(64, 64, 1, 1, 0, D3DFMT_R5G6B5),
		D3DX8TexCreate::KIND_TEXTURE);
	Check(plan.Formats[0] == D3DFMT_R5G6B5, "the requested opaque format is tried first");
	Check(plan.FormatCount >= 2 && plan.Formats[1] == D3DFMT_X8R8G8B8,
		"an opaque format falls back to X8R8G8B8");

	plan = D3DX8TexCreate::Plan(permissive, Request(64, 64, 1, 1, 0, D3DFMT_UNKNOWN),
		D3DX8TexCreate::KIND_TEXTURE);
	Check(plan.Formats[0] == D3DFMT_A8R8G8B8, "D3DFMT_UNKNOWN starts at A8R8G8B8");
	for (unsigned i = 0; i < plan.FormatCount; ++i) {
		Check(plan.Formats[i] != D3DFMT_UNKNOWN, "D3DFMT_UNKNOWN is never handed to the device");
	}

	Check(plan.FormatCount <= D3DX8TexCreate::MAX_FORMAT_CANDIDATES,
		"the candidate list stays inside its array");
}

static void Test_Retry_Policy()
{
	Check(D3DX8TexCreate::Should_Try_Next_Format(D3DERR_INVALIDCALL, 0),
		"a managed texture retries a format rejection");
	Check(D3DX8TexCreate::Should_Try_Next_Format(D3DERR_NOTAVAILABLE, 0),
		"a managed texture retries an unavailable format");
	Check(!D3DX8TexCreate::Should_Try_Next_Format(D3D_OK, 0), "success is not retried");
	Check(!D3DX8TexCreate::Should_Try_Next_Format(D3DERR_OUTOFVIDEOMEMORY, 0),
		"running out of video memory is not a format problem");

	/*
	**	These two are the contract dx8wrapper.cpp's render-target path depends on: it wants to see
	**	D3DERR_NOTAVAILABLE and D3DERR_OUTOFVIDEOMEMORY itself.
	*/
	Check(!D3DX8TexCreate::Should_Try_Next_Format(D3DERR_NOTAVAILABLE, D3DUSAGE_RENDERTARGET),
		"a render target's D3DERR_NOTAVAILABLE reaches the caller");
	Check(!D3DX8TexCreate::Should_Try_Next_Format(D3DERR_INVALIDCALL, D3DUSAGE_RENDERTARGET),
		"a render target is never silently given a different format");
	Check(!D3DX8TexCreate::Should_Try_Next_Format(D3DERR_INVALIDCALL, D3DUSAGE_DEPTHSTENCIL),
		"a depth/stencil surface is never silently given a different format");
}

static void Test_Error_Strings()
{
	char buffer[64];

	Check(D3DXGetErrorStringA(D3DERR_INVALIDCALL, buffer, sizeof(buffer)) == D3D_OK,
		"a known error code is reported successfully");
	Check(strcmp(buffer, "D3DERR_INVALIDCALL") == 0, "and it is reported by name");

	Check(D3DXGetErrorStringA(D3DERR_OUTOFVIDEOMEMORY, buffer, sizeof(buffer)) == D3D_OK,
		"D3DERR_OUTOFVIDEOMEMORY is known");
	Check(strcmp(buffer, "D3DERR_OUTOFVIDEOMEMORY") == 0, "and named");

	Check(D3DXGetErrorStringA(HRESULT(0x8badf00d), buffer, sizeof(buffer)) == D3D_OK,
		"an unknown code is still reported");
	Check(strstr(buffer, "8badf00d") != nullptr, "an unknown code is reported as its hex value");

	/*
	**	A short buffer, with a guard byte after it: the engine's callers pass 256 bytes, but a
	**	truncation that walked one past the end would be a stack overwrite in a logging path.
	*/
	char guarded[10];
	memset(guarded, '#', sizeof(guarded));
	Check(D3DXGetErrorStringA(D3DERR_INVALIDCALL, guarded, 5) == D3D_OK,
		"a short buffer truncates rather than fails");
	Check(strlen(guarded) == 4, "a short buffer gets BufferLen-1 characters");
	Check(guarded[4] == '\0', "and a terminator inside it");
	for (unsigned i = 5; i < sizeof(guarded); ++i) {
		Check(guarded[i] == '#', "nothing is written past BufferLen");
	}

	Check(D3DXGetErrorStringA(D3DERR_INVALIDCALL, nullptr, 64) == D3DERR_INVALIDCALL,
		"a null buffer is rejected rather than dereferenced");
	Check(D3DXGetErrorStringA(D3DERR_INVALIDCALL, buffer, 0) == D3DERR_INVALIDCALL,
		"a zero-length buffer is rejected");
}

static void Test_File_Load_Refusal()
{
	IDirect3DTexture8 * texture = reinterpret_cast<IDirect3DTexture8 *>(0xdeadbeef);
	D3DXIMAGE_INFO info;
	memset(&info, 0xcd, sizeof(info));

	HRESULT hr = D3DXCreateTextureFromFileExA(nullptr, "Data/Textures/example.tga", UNSPECIFIED_EXTENT,
		UNSPECIFIED_EXTENT, 0, 0, D3DFMT_UNKNOWN, D3DPOOL_MANAGED, D3DX_FILTER_BOX, D3DX_FILTER_BOX, 0,
		&info, nullptr, &texture);

	Check(FAILED(hr), "loading a texture from a file fails rather than pretending");
	Check(hr == E_NOTIMPL, "and says so with E_NOTIMPL rather than an invented code");
	Check(hr != 0, "`result != D3D_OK`, the exact test its caller makes, is true");
	Check(texture == nullptr, "the out-parameter is null even though the caller passed rubbish");

	/*
	**	The one caller passes nullptr for pSrcInfo, but a future one might not, and a caller that
	**	read an uninitialised D3DXIMAGE_INFO would decide things from stack garbage.
	*/
	Check(info.Width == 0 && info.Height == 0, "the image info is cleared, not left as it was");

	hr = D3DXCreateTextureFromFileExA(nullptr, nullptr, 0, 0, 0, 0, D3DFMT_UNKNOWN,
		D3DPOOL_MANAGED, 0, 0, 0, nullptr, nullptr, nullptr);
	Check(hr == E_NOTIMPL, "a null filename and null out-parameters are accepted, not crashed on");
}

int main()
{
	Test_Extents();
	Test_Cube_And_Volume();
	Test_Mip_Levels();
	Test_Formats();
	Test_Retry_Policy();
	Test_Error_Strings();
	Test_File_Load_Refusal();

	printf("%d checks, %d failures\n", _Checks, _Failures);
	return (_Failures == 0) ? 0 : 1;
}
