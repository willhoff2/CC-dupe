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
 *    - the device calls: a recording RenderBackendClass receives the PLANNED width, height,      *
 *      depth, mip count, format and pool for all three shapes, its caps are what the fitting     *
 *      used, a format it rejects is retried on it, and a backend with no device yet (or none at  *
 *      all) is a refusal with a null out-parameter rather than a call.                           *
 *                                                                                             *
 *  NOT ASSERTED HERE: the IDirect3DDevice8 branch of those same calls. There is no D3D8 device   *
 *  implementation off Windows to call, and on Windows d3dx8.lib is used and this file is not     *
 *  compiled at all -- the Windows behaviour is the oracle, not something this can check.        *
 *                                                                                             *
 *  Run through scripts/native-d3dx8-entrypoints-test.py.                                       *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#include "d3dx8texcreate.h"

#include "renderbackend.h"

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

/***********************************************************************************************
**	The device calls.
**
**	Off Windows the device the engine has is a RenderBackendClass, not an IDirect3DDevice8, so
**	these helpers ask the installed backend when their device argument is null - which it always
**	is, because dx8wrapper.cpp passes _Get_D3D_Device8() and that answers null behind a non-D3D8
**	backend. Before this, every one of those calls returned D3DERR_INVALIDCALL without calling
**	anything, and MissingTexture::_Init() locked the null result.
**
**	Asserted below: the arguments that arrive at the backend are the PLANNED ones (the fitting is
**	not bypassed), the caps come from the backend, the format retry walks the candidates through
**	it, and a backend without a device is a refusal rather than a call.
***********************************************************************************************/

class RecordingBackendClass : public RenderBackendClass
{
public:
	struct CallType
	{
		unsigned		Width;
		unsigned		Height;
		unsigned		Depth;
		unsigned		Levels;
		DWORD			Usage;
		D3DFORMAT	Format;
		D3DPOOL		Pool;
	};

	RecordingBackendClass() :
		DeviceExists(true),
		Caps(Permissive_Caps()),
		CapsResult(D3D_OK),
		RejectFirst(0),
		TextureCalls(0),
		CubeCalls(0),
		VolumeCalls(0)
	{
		memset(&LastTexture, 0, sizeof(LastTexture));
		memset(&LastCube, 0, sizeof(LastCube));
		memset(&LastVolume, 0, sizeof(LastVolume));
	}

	bool				DeviceExists;
	D3DCAPS8			Caps;
	HRESULT			CapsResult;
	//	How many creation attempts to reject with a format error before accepting one, which is
	//	how a device that does not support the requested format behaves.
	unsigned			RejectFirst;

	unsigned			TextureCalls;
	unsigned			CubeCalls;
	unsigned			VolumeCalls;
	CallType			LastTexture;
	CallType			LastCube;
	CallType			LastVolume;

	virtual bool Has_Device() const { return DeviceExists; }
	virtual HRESULT GetDeviceCaps(D3DCAPS8* caps)
	{
		if (FAILED(CapsResult)) return CapsResult;
		*caps = Caps;
		return D3D_OK;
	}

	virtual HRESULT CreateTexture(UINT width, UINT height, UINT levels, DWORD usage,
		D3DFORMAT format, D3DPOOL pool, IDirect3DTexture8** texture)
	{
		TextureCalls++;
		const CallType call = { width, height, 1, levels, usage, format, pool };
		LastTexture = call;
		if (TextureCalls <= RejectFirst) return D3DERR_INVALIDCALL;
		//	A non-null pointer nobody dereferences: what is under test is the arguments and the
		//	HRESULT, and the caller only stores what it is handed.
		*texture = reinterpret_cast<IDirect3DTexture8 *>(this);
		return D3D_OK;
	}

	virtual HRESULT CreateCubeTexture(UINT edge_length, UINT levels, DWORD usage, D3DFORMAT format,
		D3DPOOL pool, IDirect3DCubeTexture8** cube_texture)
	{
		CubeCalls++;
		const CallType call = { edge_length, edge_length, 1, levels, usage, format, pool };
		LastCube = call;
		if (CubeCalls <= RejectFirst) return D3DERR_INVALIDCALL;
		*cube_texture = reinterpret_cast<IDirect3DCubeTexture8 *>(this);
		return D3D_OK;
	}

	virtual HRESULT CreateVolumeTexture(UINT width, UINT height, UINT depth, UINT levels,
		DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DVolumeTexture8** volume_texture)
	{
		VolumeCalls++;
		const CallType call = { width, height, depth, levels, usage, format, pool };
		LastVolume = call;
		if (VolumeCalls <= RejectFirst) return D3DERR_INVALIDCALL;
		*volume_texture = reinterpret_cast<IDirect3DVolumeTexture8 *>(this);
		return D3D_OK;
	}

	//	The rest of the seam, which these entry points do not touch. Each one refuses, so a helper
	//	that started calling something else would fail rather than appear to work.
	virtual bool Open() { return false; }
	virtual void Release_Interface() {}
	virtual void Free_Library() {}
	virtual bool Has_Interface() const { return false; }
	virtual HRESULT Create_Device(UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*) { return D3DERR_INVALIDCALL; }
	virtual void Release_Device() {}
	virtual HRESULT BeginScene() { return D3DERR_INVALIDCALL; }
	virtual HRESULT EndScene() { return D3DERR_INVALIDCALL; }
	virtual HRESULT Clear(DWORD, CONST D3DRECT*, DWORD, D3DCOLOR, float, DWORD) { return D3DERR_INVALIDCALL; }
	virtual HRESULT Present(CONST RECT*, CONST RECT*, HWND, CONST RGNDATA*) { return D3DERR_INVALIDCALL; }
	virtual HRESULT SetRenderState(D3DRENDERSTATETYPE, DWORD) { return D3DERR_INVALIDCALL; }
	virtual HRESULT GetRenderState(D3DRENDERSTATETYPE, DWORD*) { return D3DERR_INVALIDCALL; }
	virtual HRESULT SetTextureStageState(DWORD, D3DTEXTURESTAGESTATETYPE, DWORD) { return D3DERR_INVALIDCALL; }
	virtual HRESULT SetTexture(DWORD, IDirect3DBaseTexture8*) { return D3DERR_INVALIDCALL; }
	virtual HRESULT SetTransform(D3DTRANSFORMSTATETYPE, CONST D3DMATRIX*) { return D3DERR_INVALIDCALL; }
	virtual HRESULT GetTransform(D3DTRANSFORMSTATETYPE, D3DMATRIX*) { return D3DERR_INVALIDCALL; }
	virtual HRESULT SetViewport(CONST D3DVIEWPORT8*) { return D3DERR_INVALIDCALL; }
	virtual HRESULT GetViewport(D3DVIEWPORT8*) { return D3DERR_INVALIDCALL; }
	virtual HRESULT SetMaterial(CONST D3DMATERIAL8*) { return D3DERR_INVALIDCALL; }
	virtual HRESULT SetLight(DWORD, CONST D3DLIGHT8*) { return D3DERR_INVALIDCALL; }
	virtual HRESULT LightEnable(DWORD, BOOL) { return D3DERR_INVALIDCALL; }
	virtual HRESULT SetClipPlane(DWORD, CONST float*) { return D3DERR_INVALIDCALL; }
	virtual HRESULT CreateVertexShader(CONST DWORD*, CONST DWORD*, DWORD*, DWORD) { return D3DERR_INVALIDCALL; }
	virtual HRESULT DeleteVertexShader(DWORD) { return D3DERR_INVALIDCALL; }
	virtual HRESULT SetVertexShader(DWORD) { return D3DERR_INVALIDCALL; }
	virtual HRESULT SetVertexShaderConstant(DWORD, CONST void*, DWORD) { return D3DERR_INVALIDCALL; }
	virtual HRESULT CreatePixelShader(CONST DWORD*, DWORD*) { return D3DERR_INVALIDCALL; }
	virtual HRESULT DeletePixelShader(DWORD) { return D3DERR_INVALIDCALL; }
	virtual HRESULT SetPixelShader(DWORD) { return D3DERR_INVALIDCALL; }
	virtual HRESULT SetPixelShaderConstant(DWORD, CONST void*, DWORD) { return D3DERR_INVALIDCALL; }
	virtual HRESULT CreateVertexBuffer(UINT, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer8**) { return D3DERR_INVALIDCALL; }
	virtual HRESULT CreateIndexBuffer(UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DIndexBuffer8**) { return D3DERR_INVALIDCALL; }
	virtual HRESULT CreateImageSurface(UINT, UINT, D3DFORMAT, IDirect3DSurface8**) { return D3DERR_INVALIDCALL; }
	virtual HRESULT CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS*, IDirect3DSwapChain8**) { return D3DERR_INVALIDCALL; }
	virtual HRESULT UpdateTexture(IDirect3DBaseTexture8*, IDirect3DBaseTexture8*) { return D3DERR_INVALIDCALL; }
	virtual HRESULT CopyRects(IDirect3DSurface8*, CONST RECT*, UINT, IDirect3DSurface8*, CONST POINT*) { return D3DERR_INVALIDCALL; }
	virtual HRESULT SetStreamSource(UINT, IDirect3DVertexBuffer8*, UINT) { return D3DERR_INVALIDCALL; }
	virtual HRESULT SetIndices(IDirect3DIndexBuffer8*, UINT) { return D3DERR_INVALIDCALL; }
	virtual HRESULT DrawPrimitive(D3DPRIMITIVETYPE, UINT, UINT) { return D3DERR_INVALIDCALL; }
	virtual HRESULT DrawIndexedPrimitive(D3DPRIMITIVETYPE, UINT, UINT, UINT, UINT) { return D3DERR_INVALIDCALL; }
	virtual HRESULT DrawPrimitiveUP(D3DPRIMITIVETYPE, UINT, CONST void*, UINT) { return D3DERR_INVALIDCALL; }
	virtual HRESULT ProcessVertices(UINT, UINT, UINT, IDirect3DVertexBuffer8*, DWORD) { return D3DERR_INVALIDCALL; }
	virtual HRESULT GetRenderTarget(IDirect3DSurface8**) { return D3DERR_INVALIDCALL; }
	virtual HRESULT GetDepthStencilSurface(IDirect3DSurface8**) { return D3DERR_INVALIDCALL; }
	virtual HRESULT SetRenderTarget(IDirect3DSurface8*, IDirect3DSurface8*) { return D3DERR_INVALIDCALL; }
	virtual HRESULT GetFrontBuffer(IDirect3DSurface8*) { return D3DERR_INVALIDCALL; }
	virtual HRESULT GetBackBuffer(UINT, D3DBACKBUFFER_TYPE, IDirect3DSurface8**) { return D3DERR_INVALIDCALL; }
	virtual HRESULT TestCooperativeLevel() { return D3DERR_INVALIDCALL; }
	virtual HRESULT Reset(D3DPRESENT_PARAMETERS*) { return D3DERR_INVALIDCALL; }
	virtual HRESULT ValidateDevice(DWORD*) { return D3DERR_INVALIDCALL; }
	virtual UINT GetAvailableTextureMem() { return 0; }
	virtual HRESULT ResourceManagerDiscardBytes(DWORD) { return D3DERR_INVALIDCALL; }
	virtual HRESULT GetDisplayMode(D3DDISPLAYMODE*) { return D3DERR_INVALIDCALL; }
	virtual BOOL ShowCursor(BOOL) { return 0; }
	virtual HRESULT SetCursorProperties(UINT, UINT, IDirect3DSurface8*) { return D3DERR_INVALIDCALL; }
	virtual void SetCursorPosition(UINT, UINT, DWORD) {}
	virtual void SetGammaRamp(DWORD, CONST D3DGAMMARAMP*) {}
	virtual UINT GetAdapterCount() { return 0; }
	virtual HRESULT GetAdapterIdentifier(UINT, DWORD, D3DADAPTER_IDENTIFIER8*) { return D3DERR_INVALIDCALL; }
	virtual UINT GetAdapterModeCount(UINT) { return 0; }
	virtual HRESULT EnumAdapterModes(UINT, UINT, D3DDISPLAYMODE*) { return D3DERR_INVALIDCALL; }
	virtual HRESULT GetAdapterDisplayMode(UINT, D3DDISPLAYMODE*) { return D3DERR_INVALIDCALL; }
	virtual HRESULT CheckDeviceType(UINT, D3DDEVTYPE, D3DFORMAT, D3DFORMAT, BOOL) { return D3DERR_INVALIDCALL; }
	virtual HRESULT CheckDeviceFormat(UINT, D3DDEVTYPE, D3DFORMAT, DWORD, D3DRESOURCETYPE, D3DFORMAT) { return D3DERR_INVALIDCALL; }
	virtual HRESULT CheckDeviceMultiSampleType(UINT, D3DDEVTYPE, D3DFORMAT, BOOL, D3DMULTISAMPLE_TYPE) { return D3DERR_INVALIDCALL; }
	virtual HRESULT CheckDepthStencilMatch(UINT, D3DDEVTYPE, D3DFORMAT, D3DFORMAT, D3DFORMAT) { return D3DERR_INVALIDCALL; }
	virtual HRESULT GetDeviceCaps(UINT, D3DDEVTYPE, D3DCAPS8*) { return D3DERR_INVALIDCALL; }
};

//	What d3dx8texcreate.cpp asks for the installed backend. In the engine this is
//	DX8Wrapper::Get_Render_Backend() (defined in dx8wrapper.cpp); here it is whatever the case
//	under test installed, including nothing at all.
static RenderBackendClass * _Installed_Backend = nullptr;

RenderBackendClass * D3DX8TexCreate::Peek_Render_Backend()
{
	return _Installed_Backend;
}

static void Test_Backend_Creation()
{
	RecordingBackendClass backend;
	_Installed_Backend = &backend;

	/*
	**	MissingTexture::_Init()'s own shape: 32x32 A8R8G8B8 with the whole mip chain, created with
	**	a null device argument. This is the call the engine died on.
	*/
	IDirect3DTexture8 * texture = nullptr;
	HRESULT hr = D3DXCreateTexture(nullptr, 32, 32, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
		&texture);
	Check(hr == D3D_OK, "a null device argument creates through the installed backend");
	Check(texture != nullptr, "and hands back the texture the backend created");
	Check_Equal(backend.TextureCalls, 1, "the backend was asked exactly once");
	Check_Equal(backend.LastTexture.Width, 32, "the planned width reaches the backend");
	Check_Equal(backend.LastTexture.Height, 32, "the planned height reaches the backend");
	Check_Equal(backend.LastTexture.Levels, 6, "a mip count of zero reaches it as the full chain");
	Check_Equal(backend.LastTexture.Format, D3DFMT_A8R8G8B8, "the requested format is tried first");
	Check_Equal(backend.LastTexture.Pool, D3DPOOL_MANAGED, "the pool is passed through");

	/*
	**	The caps used for the fitting are the BACKEND's, not a permissive default: a backend with a
	**	2048 maximum must clamp, or the engine would ask a device for something it cannot make.
	*/
	backend.Caps.MaxTextureWidth = 128;
	backend.Caps.MaxTextureHeight = 128;
	hr = D3DXCreateTexture(nullptr, 256, 256, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture);
	Check(hr == D3D_OK, "a clamped request still creates");
	Check_Equal(backend.LastTexture.Width, 128, "the backend's own maximum extent is what clamps");

	/*
	**	Format substitution against the backend: it rejects the first candidate the way a device
	**	that cannot make that format does, and the next candidate is tried on the same backend.
	*/
	backend.Caps = Permissive_Caps();
	backend.TextureCalls = 0;
	backend.RejectFirst = 1;
	hr = D3DXCreateTexture(nullptr, 64, 64, 1, 0, D3DFMT_A4R4G4B4, D3DPOOL_MANAGED, &texture);
	Check(hr == D3D_OK, "a format the backend rejects is retried on it");
	Check_Equal(backend.TextureCalls, 2, "which took a second call to the backend");
	Check(backend.LastTexture.Format != D3DFMT_A4R4G4B4, "with a different format");

	/*
	**	Cube and volume creation reach the backend too. This backend accepts them; the Vulkan one
	**	refuses and records the refusal in its unimplemented ledger, which is the finding rather
	**	than something to paper over -- what is asserted here is that the call ARRIVES, because a
	**	refusal nobody reaches is not a measurement.
	*/
	backend.RejectFirst = 0;
	IDirect3DCubeTexture8 * cube = nullptr;
	hr = D3DXCreateCubeTexture(nullptr, 64, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &cube);
	Check(hr == D3D_OK, "a cube texture is created through the backend");
	Check_Equal(backend.CubeCalls, 1, "the backend's CreateCubeTexture is what was called");
	Check_Equal(backend.LastCube.Width, 64, "with the planned edge length");
	Check_Equal(backend.LastCube.Levels, 7, "and the full mip chain");

	IDirect3DVolumeTexture8 * volume = nullptr;
	hr = D3DXCreateVolumeTexture(nullptr, 32, 32, 16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
		&volume);
	Check(hr == D3D_OK, "a volume texture is created through the backend");
	Check_Equal(backend.VolumeCalls, 1, "the backend's CreateVolumeTexture is what was called");
	Check_Equal(backend.LastVolume.Depth, 16, "with the planned depth");

	/*
	**	A caps failure is the caller's answer, not a substituted default: fitting against zeroed
	**	caps would ask for a texture no device agreed to.
	*/
	backend.TextureCalls = 0;
	backend.CapsResult = D3DERR_INVALIDCALL;
	texture = reinterpret_cast<IDirect3DTexture8 *>(0xdeadbeef);
	hr = D3DXCreateTexture(nullptr, 64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture);
	Check(hr == D3DERR_INVALIDCALL, "a backend that cannot report caps fails the creation");
	Check(texture == nullptr, "and the out-parameter is null");
	Check_Equal(backend.TextureCalls, 0, "without creating anything");
	backend.CapsResult = D3D_OK;

	/*
	**	Before Create_Device() there is a backend but no device, and the request has to fail
	**	rather than reach a half-built one. Same for no backend at all, which is what the tests of
	**	the fitting above run with.
	*/
	backend.TextureCalls = 0;
	backend.DeviceExists = false;
	texture = reinterpret_cast<IDirect3DTexture8 *>(0xdeadbeef);
	hr = D3DXCreateTexture(nullptr, 64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture);
	Check(hr == D3DERR_INVALIDCALL, "a backend with no device yet refuses the creation");
	Check(texture == nullptr, "and the out-parameter is null rather than left as it was");
	Check_Equal(backend.TextureCalls, 0, "and nothing was created");

	_Installed_Backend = nullptr;
	texture = reinterpret_cast<IDirect3DTexture8 *>(0xdeadbeef);
	hr = D3DXCreateTexture(nullptr, 64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture);
	Check(hr == D3DERR_INVALIDCALL, "no backend at all refuses the creation");
	Check(texture == nullptr, "and the out-parameter is null");
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
	Test_Backend_Creation();

	printf("%d checks, %d failures\n", _Checks, _Failures);
	return (_Failures == 0) ? 0 : 1;
}
