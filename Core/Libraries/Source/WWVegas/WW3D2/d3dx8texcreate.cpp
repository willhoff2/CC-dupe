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

// D3DXCreateTexture(), D3DXCreateCubeTexture(), D3DXCreateVolumeTexture() and
// D3DXGetErrorStringA() off Windows, where they come from d3dx8.lib -- plus the one entry point in
// this family that is a refusal rather than an implementation,
// D3DXCreateTextureFromFileExA(), which is called out below and in
// docs/porting/startability.md rather than quietly returning a texture nobody drew.
//
// THESE ARE NOT TEXTURE CREATION. THE DEVICE CREATES THE TEXTURE.
//
// Every one of the three creation helpers ends in the corresponding Create*() call on whatever
// device the engine has: the IDirect3DDevice8 the caller passed when there is one, and otherwise
// the installed RenderBackendClass, which is what the engine has off Windows and is why these
// helpers no longer refuse every call the engine makes (see Resolve_Target below).
//
// What D3DX adds -- and what the engine relies on it adding, because dx8wrapper.cpp says so
// in a comment at every call site -- is the fitting: filling in the arguments the caller left as
// D3DX_DEFAULT, obeying the device's power-of-two/square/maximum-extent caps, turning a mip level
// count of zero into a whole chain, and retrying with a different format when the device rejects
// the one asked for ("NOTE: If 'format' is not supported as a texture format, this function will
// find the closest format that is supported and use that instead", _Create_DX8_Texture()).
//
// That fitting is arithmetic and policy, so it lives in Plan()/Should_Try_Next_Format() in
// d3dx8texcreate.h where scripts/native-d3dx8-entrypoints-test.py can assert it without a device.
// The entry points themselves are the loop around it, and the entry-point test now covers the
// lines that call the device too: it passes a recording RenderBackendClass and asserts the shape,
// format and level count that arrive at CreateTexture()/CreateCubeTexture()/CreateVolumeTexture().
// What no test off Windows can cover is the IDirect3DDevice8 branch, because there is no D3D8
// device to call there; on Windows d3dx8.lib is used and this file is not compiled at all.
//
// WHAT THE FITTING DOES, AND WHERE IT DIFFERS FROM D3DX
//
//   * D3DX_DEFAULT or zero extents become the other extent, or 256 when neither is given, which
//     is D3DXCheckTextureRequirements' documented substitution. Zero is folded in with
//     D3DX_DEFAULT deliberately: the header says extents "must be non-zero", and a zero that
//     reached the device would be D3DERR_INVALIDCALL with no explanation.
//   * D3DPTEXTURECAPS_POW2 rounds each extent UP to a power of two (D3DX rounds up, so an image
//     is never silently cropped), unless D3DPTEXTURECAPS_NONPOW2CONDITIONAL is also set, which is
//     the cap that says "non-power-of-two is allowed with restrictions the engine's usage meets"
//     (no wrapping, no mipmapping on that texture). D3DPTEXTURECAPS_SQUAREONLY then takes the
//     larger extent for both. Both caps apply to plain textures only; a cube texture has a single
//     edge length, and the volume caps are separate.
//   * The extents are clamped LAST, to MaxTextureWidth/MaxTextureHeight (MaxVolumeExtent for a
//     volume texture), and the clamp re-establishes squareness rather than undoing it. A clamp is
//     reported on stderr because it silently changes what the artist drew.
//   * MipLevels of zero or D3DX_DEFAULT becomes the complete chain, floor(log2(max extent)) + 1,
//     counting the volume depth for a volume texture as D3D does. Without
//     D3DPTEXTURECAPS_MIPMAP (or _MIPCUBEMAP/_MIPVOLUMEMAP for the other two shapes) it becomes
//     1 instead of the whole chain, which is what asking a device with no mip support for a mip
//     chain has to mean.
//   * Format substitution is a documented behaviour with an undocumented list, so the list here
//     is the narrow one the engine's own formats need: alpha-carrying formats fall back through
//     A8R8G8B8, and formats without alpha through X8R8G8B8 and R5G6B5. D3DFMT_UNKNOWN starts at
//     A8R8G8B8. Real D3DX asks IDirect3D8::CheckDeviceFormat() first and can pick formats
//     outside this list; this asks the device to create the texture and moves on to the next
//     candidate when it says no, which reaches the same place with one wasted call and without
//     this file having to model the adapter format. A substitution prints one line, because a
//     texture that came back in a different format than the caller chose is a rendering
//     difference somebody will otherwise chase.
//
// WHAT IS DELIBERATELY NOT SUBSTITUTED: anything for a render target or a depth/stencil surface.
// dx8wrapper.cpp's render-target path reads the HRESULT itself -- D3DERR_NOTAVAILABLE means "this
// device cannot give you one", and D3DERR_OUTOFVIDEOMEMORY makes it release cached textures and
// the mesh cache and try again. Retrying inside here in a different format would answer a
// question it did not ask and hide the one it did.

#if !defined(_WIN32)

#include "d3dx8texcreate.h"

#include "renderbackend.h"

#include <stdio.h>
#include <string.h>

namespace D3DX8TexCreate
{

//
//	D3DX_DEFAULT is ULONG_MAX in the SDK headers, which is 64 bits wide here and does not fit the
//	UINT parameters these entry points take. What arrives is therefore the truncation, and that is
//	what to compare against; comparing against D3DX_DEFAULT itself would never match.
//
static const unsigned UNSPECIFIED = 0xffffffffu;

static unsigned Round_Up_To_Power_Of_Two(unsigned value)
{
	unsigned result = 1;
	while (result < value) {
		result <<= 1;
	}
	return result;
}

unsigned Full_Mip_Chain(unsigned width, unsigned height, unsigned depth)
{
	unsigned largest = width;
	if (height > largest) largest = height;
	if (depth > largest) largest = depth;

	unsigned levels = 1;
	while (largest > 1) {
		largest >>= 1;
		++levels;
	}
	return levels;
}

//
//	D3DX_DEFAULT is 0xffffffff, and the engine also has call sites that pass a literal 0 for "you
//	choose". Both mean the same thing here.
//
static bool Is_Unspecified(unsigned extent)
{
	return extent == 0 || extent == UNSPECIFIED;
}

static bool Has_Alpha(D3DFORMAT format)
{
	switch (format) {
		case D3DFMT_A8R8G8B8:
		case D3DFMT_A1R5G5B5:
		case D3DFMT_A4R4G4B4:
		case D3DFMT_A8:
		case D3DFMT_A8R3G3B2:
		case D3DFMT_A8P8:
		case D3DFMT_A8L8:
		case D3DFMT_A4L4:
		case D3DFMT_A2B10G10R10:
		case D3DFMT_DXT2:
		case D3DFMT_DXT3:
		case D3DFMT_DXT4:
		case D3DFMT_DXT5:
			return true;
		default:
			return false;
	}
}

static void Add_Format(PlanType & plan, D3DFORMAT format)
{
	if (plan.FormatCount >= MAX_FORMAT_CANDIDATES) return;
	for (unsigned i = 0; i < plan.FormatCount; ++i) {
		if (plan.Formats[i] == format) return;
	}
	plan.Formats[plan.FormatCount++] = format;
}

PlanType Plan(const D3DCAPS8 & caps, const RequestType & request, KindType kind)
{
	PlanType plan;
	memset(&plan, 0, sizeof(plan));

	unsigned width = request.Width;
	unsigned height = request.Height;
	unsigned depth = (kind == KIND_VOLUME) ? request.Depth : 1;

	/*
	**	A cube texture has one edge; D3DXCreateCubeTexture() passes its Size as the width and the
	**	height is not a separate argument, so make them equal before anything else looks at them.
	*/
	if (kind == KIND_CUBE) {
		height = width;
	}

	if (Is_Unspecified(width) && Is_Unspecified(height)) {
		width = height = 256;
	} else if (Is_Unspecified(width)) {
		width = height;
	} else if (Is_Unspecified(height)) {
		height = width;
	}
	if (kind == KIND_VOLUME && Is_Unspecified(depth)) {
		depth = 1;
	}

	const bool pow2_required = (caps.TextureCaps & D3DPTEXTURECAPS_POW2) != 0
		&& (caps.TextureCaps & D3DPTEXTURECAPS_NONPOW2CONDITIONAL) == 0;

	/*
	**	A cube texture's edge and a volume texture's extents are required to be powers of two by
	**	their own caps rather than by D3DPTEXTURECAPS_POW2, and every device that supports them at
	**	all supports them at power-of-two sizes, so round those unconditionally and the plain
	**	texture only when the cap says to.
	*/
	if (pow2_required || kind != KIND_TEXTURE) {
		width = Round_Up_To_Power_Of_Two(width);
		height = Round_Up_To_Power_Of_Two(height);
		if (kind == KIND_VOLUME) {
			depth = Round_Up_To_Power_Of_Two(depth);
		}
	}

	if (kind == KIND_TEXTURE && (caps.TextureCaps & D3DPTEXTURECAPS_SQUAREONLY) != 0) {
		if (width > height) height = width; else width = height;
	}

	unsigned max_width = (kind == KIND_VOLUME) ? unsigned(caps.MaxVolumeExtent)
		: unsigned(caps.MaxTextureWidth);
	unsigned max_height = (kind == KIND_VOLUME) ? unsigned(caps.MaxVolumeExtent)
		: unsigned(caps.MaxTextureHeight);
	const unsigned requested_width = width;
	const unsigned requested_height = height;
	const unsigned requested_depth = depth;

	if (max_width != 0 && width > max_width) width = max_width;
	if (max_height != 0 && height > max_height) height = max_height;
	if (kind == KIND_VOLUME && max_width != 0 && depth > max_width) depth = max_width;

	/*
	**	Clamping can break squareness that was just established, and a cube texture whose edges
	**	differ is not a cube texture at all, so re-establish both against the smaller extent.
	*/
	const bool square = (kind == KIND_CUBE)
		|| (kind == KIND_TEXTURE && (caps.TextureCaps & D3DPTEXTURECAPS_SQUAREONLY) != 0);
	if (square && width != height) {
		if (width < height) height = width; else width = height;
	}

	if (width != requested_width || height != requested_height || depth != requested_depth) {
		fprintf(stderr, "D3DX texture creation: %ux%ux%u exceeds this device's limits and was "
			"clamped to %ux%ux%u.\n", requested_width, requested_height, requested_depth,
			width, height, depth);
	}

	DWORD mip_cap = D3DPTEXTURECAPS_MIPMAP;
	if (kind == KIND_CUBE) mip_cap = D3DPTEXTURECAPS_MIPCUBEMAP;
	if (kind == KIND_VOLUME) mip_cap = D3DPTEXTURECAPS_MIPVOLUMEMAP;

	unsigned levels = request.MipLevels;
	if (levels == 0 || levels == UNSPECIFIED) {
		levels = Full_Mip_Chain(width, height, (kind == KIND_VOLUME) ? depth : 1);
	}
	if ((caps.TextureCaps & mip_cap) == 0) {
		levels = 1;
	}
	const unsigned chain = Full_Mip_Chain(width, height, (kind == KIND_VOLUME) ? depth : 1);
	if (levels > chain) {
		levels = chain;
	}

	if (request.Format != D3DFMT_UNKNOWN) {
		Add_Format(plan, request.Format);
	}
	if (request.Format == D3DFMT_UNKNOWN || Has_Alpha(request.Format)) {
		Add_Format(plan, D3DFMT_A8R8G8B8);
		Add_Format(plan, D3DFMT_A1R5G5B5);
		Add_Format(plan, D3DFMT_A4R4G4B4);
	} else {
		Add_Format(plan, D3DFMT_X8R8G8B8);
		Add_Format(plan, D3DFMT_R5G6B5);
	}

	plan.Width = width;
	plan.Height = height;
	plan.Depth = depth;
	plan.MipLevels = levels;
	return plan;
}

bool Should_Try_Next_Format(HRESULT result, DWORD usage)
{
	if (SUCCEEDED(result)) return false;
	if ((usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) != 0) return false;
	return result == D3DERR_INVALIDCALL || result == D3DERR_NOTAVAILABLE;
}

const char * Error_String(HRESULT hr)
{
	switch (hr) {
		case D3D_OK:								return "D3D_OK";
		case D3DERR_WRONGTEXTUREFORMAT:		return "D3DERR_WRONGTEXTUREFORMAT";
		case D3DERR_UNSUPPORTEDCOLOROPERATION:
			return "D3DERR_UNSUPPORTEDCOLOROPERATION";
		case D3DERR_UNSUPPORTEDCOLORARG:		return "D3DERR_UNSUPPORTEDCOLORARG";
		case D3DERR_UNSUPPORTEDALPHAOPERATION:
			return "D3DERR_UNSUPPORTEDALPHAOPERATION";
		case D3DERR_UNSUPPORTEDALPHAARG:		return "D3DERR_UNSUPPORTEDALPHAARG";
		case D3DERR_TOOMANYOPERATIONS:			return "D3DERR_TOOMANYOPERATIONS";
		case D3DERR_CONFLICTINGTEXTUREFILTER:
			return "D3DERR_CONFLICTINGTEXTUREFILTER";
		case D3DERR_UNSUPPORTEDFACTORVALUE:	return "D3DERR_UNSUPPORTEDFACTORVALUE";
		case D3DERR_CONFLICTINGRENDERSTATE:	return "D3DERR_CONFLICTINGRENDERSTATE";
		case D3DERR_UNSUPPORTEDTEXTUREFILTER:
			return "D3DERR_UNSUPPORTEDTEXTUREFILTER";
		case D3DERR_CONFLICTINGTEXTUREPALETTE:
			return "D3DERR_CONFLICTINGTEXTUREPALETTE";
		case D3DERR_DRIVERINTERNALERROR:		return "D3DERR_DRIVERINTERNALERROR";
		case D3DERR_NOTFOUND:						return "D3DERR_NOTFOUND";
		case D3DERR_MOREDATA:						return "D3DERR_MOREDATA";
		case D3DERR_DEVICELOST:					return "D3DERR_DEVICELOST";
		case D3DERR_DEVICENOTRESET:				return "D3DERR_DEVICENOTRESET";
		case D3DERR_NOTAVAILABLE:					return "D3DERR_NOTAVAILABLE";
		case D3DERR_OUTOFVIDEOMEMORY:			return "D3DERR_OUTOFVIDEOMEMORY";
		case D3DERR_INVALIDDEVICE:				return "D3DERR_INVALIDDEVICE";
		case D3DERR_INVALIDCALL:					return "D3DERR_INVALIDCALL";
		case D3DERR_DRIVERINVALIDCALL:			return "D3DERR_DRIVERINVALIDCALL";
		case D3DXERR_CANNOTMODIFYINDEXBUFFER:
			return "D3DXERR_CANNOTMODIFYINDEXBUFFER";
		case D3DXERR_INVALIDMESH:					return "D3DXERR_INVALIDMESH";
		case D3DXERR_CANNOTATTRSORT:				return "D3DXERR_CANNOTATTRSORT";
		case D3DXERR_SKINNINGNOTSUPPORTED:		return "D3DXERR_SKINNINGNOTSUPPORTED";
		case D3DXERR_TOOMANYINFLUENCES:			return "D3DXERR_TOOMANYINFLUENCES";
		case D3DXERR_INVALIDDATA:					return "D3DXERR_INVALIDDATA";
		case D3DXERR_LOADEDMESHASNODATA:			return "D3DXERR_LOADEDMESHASNODATA";
		case E_OUTOFMEMORY:						return "E_OUTOFMEMORY";
		case E_INVALIDARG:							return "E_INVALIDARG";
		case E_NOTIMPL:								return "E_NOTIMPL";
		case E_FAIL:									return "E_FAIL";
		default:											return nullptr;
	}
}

} // namespace D3DX8TexCreate

//
//	What a creation helper creates through: the IDirect3DDevice8 the caller passed, or - when
//	that is null, which is every call the engine makes behind a non-D3D8 backend - the installed
//	RenderBackendClass. Exactly one of the two is set; both null means there is no device at all
//	yet, which is a refusal rather than a null dereference.
//
//	The D3D8 member is named D3DDevice8 rather than Device so that the D3D8 surface scanner counts
//	the calls made on it: the scanner drops `Device` as too generic a receiver name, which would
//	hide this file's direct call sites from the gate whose job is to budget them.
//
struct TargetType
{
	LPDIRECT3DDEVICE8			D3DDevice8;
	RenderBackendClass *		Backend;

	bool Is_Valid() const { return D3DDevice8 != nullptr || Backend != nullptr; }
	HRESULT Get_Caps(D3DCAPS8 & caps) const
	{
		memset(&caps, 0, sizeof(caps));
		if (D3DDevice8 != nullptr) return D3DDevice8->GetDeviceCaps(&caps);
		return Backend->GetDeviceCaps(&caps);
	}
};

//
//	A device argument wins when there is one, so a caller that holds a real IDirect3DDevice8 (the
//	D3D8 backend, and the tests that pass a stub) reaches exactly what it passed. Only a null
//	device falls through to the backend, and only once the backend has a device: before
//	Create_Device() there is nothing to create with, and the request has to fail rather than
//	reach a half-built backend.
//
static TargetType Resolve_Target(LPDIRECT3DDEVICE8 device)
{
	TargetType target = { device, nullptr };
	if (device != nullptr) return target;
	RenderBackendClass * backend = D3DX8TexCreate::Peek_Render_Backend();
	if (backend != nullptr && backend->Has_Device()) target.Backend = backend;
	return target;
}

//
//	A substitution is a chosen fallback, so it is counted in the backend's ledger under the class
//	of format it replaced: a block-compressed request that came back uncompressed is the one the
//	mission-frame work cares about (every level then takes the engine's software decode at 4x the
//	memory), and it must not be confused with a plain 8888 request that came back 16-bit.
//
static void Record_Substitution(RenderBackendClass & backend, D3DFORMAT requested)
{
	switch (requested) {
		case D3DFMT_DXT1:
		case D3DFMT_DXT2:
		case D3DFMT_DXT3:
		case D3DFMT_DXT4:
		case D3DFMT_DXT5:
			backend.Record_Unserviceable(
				"D3DXCreateTexture(block-compressed format substituted)",
				"the device refused the DXTn format, so the texture was created uncompressed");
			return;
		default:
			backend.Record_Unserviceable(
				"D3DXCreateTexture(format substituted)",
				"the device refused the requested format, so a fallback format was created");
			return;
	}
}

//
//	The three creation helpers. Each one plans, then walks the candidate formats, and reports what
//	it substituted. On failure the out-parameter is written null before anything else, so a caller
//	that ignores the HRESULT cannot read a pointer that was never set.
//
template <typename OutType, typename CreateType>
static HRESULT Create_Fitted(const TargetType & target, D3DX8TexCreate::KindType kind,
	const D3DX8TexCreate::RequestType & request, OutType ** out, CreateType create)
{
	if (out != nullptr) *out = nullptr;
	if (!target.Is_Valid() || out == nullptr) return D3DERR_INVALIDCALL;

	D3DCAPS8 caps;
	HRESULT caps_result = target.Get_Caps(caps);
	if (FAILED(caps_result)) return caps_result;

	const D3DX8TexCreate::PlanType plan = D3DX8TexCreate::Plan(caps, request, kind);

	HRESULT first = D3DERR_INVALIDCALL;
	for (unsigned i = 0; i < plan.FormatCount; ++i) {
		HRESULT result = create(plan, plan.Formats[i], out);
		if (SUCCEEDED(result)) {
			if (i != 0) {
				fprintf(stderr, "D3DX texture creation: this device rejected format %d, so the "
					"texture was created as format %d instead.\n",
					int(plan.Formats[0]), int(plan.Formats[i]));
				if (target.Backend != nullptr) Record_Substitution(*target.Backend, plan.Formats[0]);
			}
			return result;
		}
		*out = nullptr;
		if (i == 0) first = result;
		if (!D3DX8TexCreate::Should_Try_Next_Format(result, request.Usage)) return result;
	}
	return first;
}

HRESULT WINAPI D3DXCreateTexture(
	LPDIRECT3DDEVICE8 pDevice,
	UINT Width,
	UINT Height,
	UINT MipLevels,
	DWORD Usage,
	D3DFORMAT Format,
	D3DPOOL Pool,
	LPDIRECT3DTEXTURE8 * ppTexture)
{
	const D3DX8TexCreate::RequestType request = { Width, Height, 1, MipLevels, Usage, Format };
	const TargetType target = Resolve_Target(pDevice);
	return Create_Fitted(target, D3DX8TexCreate::KIND_TEXTURE, request, ppTexture,
		[&target, Usage, Pool](const D3DX8TexCreate::PlanType & plan, D3DFORMAT format,
				LPDIRECT3DTEXTURE8 * out) {
			if (target.D3DDevice8 != nullptr) {
				return target.D3DDevice8->CreateTexture(plan.Width, plan.Height, plan.MipLevels, Usage,
					format, Pool, out);
			}
			return target.Backend->CreateTexture(plan.Width, plan.Height, plan.MipLevels, Usage,
				format, Pool, out);
		});
}

HRESULT WINAPI D3DXCreateCubeTexture(
	LPDIRECT3DDEVICE8 pDevice,
	UINT Size,
	UINT MipLevels,
	DWORD Usage,
	D3DFORMAT Format,
	D3DPOOL Pool,
	LPDIRECT3DCUBETEXTURE8 * ppCubeTexture)
{
	const D3DX8TexCreate::RequestType request = { Size, Size, 1, MipLevels, Usage, Format };
	const TargetType target = Resolve_Target(pDevice);
	return Create_Fitted(target, D3DX8TexCreate::KIND_CUBE, request, ppCubeTexture,
		[&target, Usage, Pool](const D3DX8TexCreate::PlanType & plan, D3DFORMAT format,
				LPDIRECT3DCUBETEXTURE8 * out) {
			if (target.D3DDevice8 != nullptr) {
				return target.D3DDevice8->CreateCubeTexture(plan.Width, plan.MipLevels, Usage, format,
					Pool, out);
			}
			return target.Backend->CreateCubeTexture(plan.Width, plan.MipLevels, Usage, format,
				Pool, out);
		});
}

HRESULT WINAPI D3DXCreateVolumeTexture(
	LPDIRECT3DDEVICE8 pDevice,
	UINT Width,
	UINT Height,
	UINT Depth,
	UINT MipLevels,
	DWORD Usage,
	D3DFORMAT Format,
	D3DPOOL Pool,
	LPDIRECT3DVOLUMETEXTURE8 * ppVolumeTexture)
{
	const D3DX8TexCreate::RequestType request = { Width, Height, Depth, MipLevels, Usage, Format };
	const TargetType target = Resolve_Target(pDevice);
	return Create_Fitted(target, D3DX8TexCreate::KIND_VOLUME, request, ppVolumeTexture,
		[&target, Usage, Pool](const D3DX8TexCreate::PlanType & plan, D3DFORMAT format,
				LPDIRECT3DVOLUMETEXTURE8 * out) {
			if (target.D3DDevice8 != nullptr) {
				return target.D3DDevice8->CreateVolumeTexture(plan.Width, plan.Height, plan.Depth,
					plan.MipLevels, Usage, format, Pool, out);
			}
			return target.Backend->CreateVolumeTexture(plan.Width, plan.Height, plan.Depth,
				plan.MipLevels, Usage, format, Pool, out);
		});
}

HRESULT WINAPI D3DXGetErrorStringA(HRESULT hr, LPSTR pBuffer, UINT BufferLen)
{
	if (pBuffer == nullptr || BufferLen == 0) return D3DERR_INVALIDCALL;

	char text[64];
	const char * known = D3DX8TexCreate::Error_String(hr);
	if (known == nullptr) {
		/*
		**	Unknown codes are reported as the number, which is what makes the report useful for a
		**	backend-specific HRESULT this table has never heard of. The sign matters: an HRESULT
		**	is a signed 32-bit value and every failure code has the top bit set.
		*/
		snprintf(text, sizeof(text), "Unknown error 0x%08x", unsigned(hr));
		known = text;
	}

	/*
	**	Truncating rather than failing is what D3DX does: the buffer at both call sites in
	**	dx8wrapper.cpp is 256 bytes and every name here fits, but a caller with a short buffer
	**	gets as much of the name as fits and a terminator, not an overrun.
	*/
	const size_t length = strlen(known);
	const size_t copy = (length < size_t(BufferLen) - 1) ? length : size_t(BufferLen) - 1;
	memcpy(pBuffer, known, copy);
	pBuffer[copy] = '\0';
	return D3D_OK;
}

//
//	AND THE ONE THAT IS NOT IMPLEMENTED.
//
//	D3DXCreateTextureFromFileExA() decodes an image file and uploads it. Its only caller is
//	DX8Wrapper::_Create_DX8_Texture(const char *filename, ...), which already handles failure by
//	returning MissingTexture::_Get_Missing_Texture() -- the magenta-and-black checkerboard the
//	engine builds for exactly this case. So a refusal is safe, and it is visible: every texture
//	loaded through this path shows as the missing-texture pattern rather than as a plausible
//	picture, which is the difference between an obvious gap and a subtle one.
//
//	It is refused rather than written because it is not a D3DX-shaped job in this tree. The engine
//	has its own image decoders (targa.cpp, ddsfile.cpp) and its own asset pipeline
//	(textureloader.cpp / TextureLoadTaskClass) reading through TheFileFactory, not through the
//	filesystem D3DX would read. Implementing it here would mean either duplicating those decoders
//	or building a second file-access path, and the port's own answer -- routing this call site
//	through the engine's loader, which is where DDS and TGA already work -- is a renderer change
//	and belongs in the renderer slice that owns TextureLoader.
//
//	Until then: no texture loads from a file off Windows. See docs/porting/startability.md.
//
HRESULT WINAPI D3DXCreateTextureFromFileExA(
	LPDIRECT3DDEVICE8 pDevice,
	LPCSTR pSrcFile,
	UINT Width,
	UINT Height,
	UINT MipLevels,
	DWORD Usage,
	D3DFORMAT Format,
	D3DPOOL Pool,
	DWORD Filter,
	DWORD MipFilter,
	D3DCOLOR ColorKey,
	D3DXIMAGE_INFO * pSrcInfo,
	PALETTEENTRY * pPalette,
	LPDIRECT3DTEXTURE8 * ppTexture)
{
	(void)pDevice; (void)Width; (void)Height; (void)MipLevels; (void)Usage; (void)Format;
	(void)Pool; (void)Filter; (void)MipFilter; (void)ColorKey; (void)pPalette;

	if (ppTexture != nullptr) *ppTexture = nullptr;
	if (pSrcInfo != nullptr) memset(pSrcInfo, 0, sizeof(*pSrcInfo));

	fprintf(stderr, "!!! D3DXCreateTextureFromFileExA() is not implemented off Windows: image "
		"decoding for this path belongs to the engine's own TextureLoader (targa.cpp, ddsfile.cpp) "
		"rather than to a reimplemented D3DX. Refused \"%s\"; the caller uses the missing-texture "
		"checkerboard.\n", (pSrcFile != nullptr) ? pSrcFile : "(null)");

	return E_NOTIMPL;
}

#endif // !_WIN32
