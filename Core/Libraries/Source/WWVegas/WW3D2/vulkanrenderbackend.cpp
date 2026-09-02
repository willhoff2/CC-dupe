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
 *                 Project Name : ww3d                                                         *
 *                                                                                             *
 *                    File Name : vulkanrenderbackend.cpp                                      *
 *                                                                                             *
 *---------------------------------------------------------------------------------------------*/

#ifndef _WIN32

// The spike's headers first, deliberately.  They redeclare the D3D8 enum vocabulary inside
// namespace spike, and d3d8types.h defines some of those spellings as function-like macros
// (D3DTS_WORLD is `D3DTS_WORLDMATRIX(0)`), so including d3d8.h first turns the spike's
// enumerators into macro invocations and neither header parses.  In this order both do, and the
// only cost is that this file must not name spike::D3DTS_* / spike::D3DTSS_TCI_* after the D3D8
// headers arrive - it names the raw values it is handed instead, which is what the seam passes.
#include "render_backend.h"
#include "png_write.h"

#include "vulkanrenderbackend.h"

#include "platform/platform_window.h"
#include "wwdebug.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

/***********************************************************************************************
** The unimplemented-call ledger.
**
** An entry here is a finding, not a fallback: the engine asked for a D3D8 operation this backend
** cannot perform, the call returned a D3D8 failure code, and this is where that fact is kept so
** running the engine enumerates the gaps.
***********************************************************************************************/

namespace
{

enum { MAX_UNIMPLEMENTED_KINDS = 32 };

VulkanRenderBackendClass::UnimplementedCallClass UnimplementedCalls[MAX_UNIMPLEMENTED_KINDS];
unsigned UnimplementedKindCount = 0;

/*
** Record one unimplemented entry point and return the HRESULT the caller should return.  The
** first occurrence is logged; the rest are only counted, because the engine reaches some of
** these once per frame and a log line per frame would bury the ones that happen once.
*/
HRESULT Record_Unimplemented(const char * name, const char * why, HRESULT result)
{
	for (unsigned index = 0; index < UnimplementedKindCount; index++) {
		if (strcmp(UnimplementedCalls[index].Name, name) == 0) {
			UnimplementedCalls[index].Count++;
			return result;
		}
	}
	if (UnimplementedKindCount < MAX_UNIMPLEMENTED_KINDS) {
		UnimplementedCalls[UnimplementedKindCount].Name = name;
		UnimplementedCalls[UnimplementedKindCount].Why = why;
		UnimplementedCalls[UnimplementedKindCount].Count = 1;
		UnimplementedKindCount++;
	}
	WWDEBUG_SAY(("VulkanRenderBackend: unimplemented D3D8 entry point %s (%s); returning 0x%x\n",
		name, why, (unsigned)result));
	return result;
}

/***********************************************************************************************
** D3D8 <-> spike type translation.
***********************************************************************************************/

/*
** D3DFORMAT -> spike::TextureFormat.  False for a format the spike's texture path has no
** encoding for at all, which is a refusal to create the texture rather than a substitution:
** silently handing back a different format is how a port ends up with the wrong pixels and no
** failing call to blame.
*/
bool Translate_Format(D3DFORMAT format, spike::TextureFormat & out)
{
	switch (format) {
		case D3DFMT_A8R8G8B8:	out = spike::TextureFormat::A8R8G8B8; return true;
		case D3DFMT_X8R8G8B8:	out = spike::TextureFormat::X8R8G8B8; return true;
		case D3DFMT_R8G8B8:		out = spike::TextureFormat::R8G8B8; return true;
		case D3DFMT_A4R4G4B4:	out = spike::TextureFormat::A4R4G4B4; return true;
		case D3DFMT_A1R5G5B5:	out = spike::TextureFormat::A1R5G5B5; return true;
		case D3DFMT_R5G6B5:		out = spike::TextureFormat::R5G6B5; return true;
		case D3DFMT_L8:			out = spike::TextureFormat::L8; return true;
		case D3DFMT_A8:			out = spike::TextureFormat::A8; return true;
		case D3DFMT_A8L8:		out = spike::TextureFormat::A8L8; return true;
		case D3DFMT_V8U8:		out = spike::TextureFormat::V8U8; return true;
		case D3DFMT_P8:			out = spike::TextureFormat::P8; return true;
		case D3DFMT_DXT1:		out = spike::TextureFormat::DXT1; return true;
		case D3DFMT_DXT2:		out = spike::TextureFormat::DXT2; return true;
		case D3DFMT_DXT3:		out = spike::TextureFormat::DXT3; return true;
		case D3DFMT_DXT4:		out = spike::TextureFormat::DXT4; return true;
		case D3DFMT_DXT5:		out = spike::TextureFormat::DXT5; return true;
		default:				return false;
	}
}

/*
** Bytes per row of a level, needed for the D3DLOCKED_RECT pitch of a surface this layer owns
** (the image surfaces, which are plain host memory) and for GetLevelDesc's Size.
*/
unsigned Format_Bytes_Per_Pixel(spike::TextureFormat format)
{
	switch (format) {
		case spike::TextureFormat::A8R8G8B8:
		case spike::TextureFormat::X8R8G8B8:
			return 4;
		case spike::TextureFormat::R8G8B8:
			return 3;
		case spike::TextureFormat::A4R4G4B4:
		case spike::TextureFormat::A1R5G5B5:
		case spike::TextureFormat::R5G6B5:
		case spike::TextureFormat::A8L8:
		case spike::TextureFormat::V8U8:
			return 2;
		default:
			return 1;
	}
}

bool Is_Compressed(spike::TextureFormat format)
{
	return format == spike::TextureFormat::DXT1 || format == spike::TextureFormat::DXT2
		|| format == spike::TextureFormat::DXT3 || format == spike::TextureFormat::DXT4
		|| format == spike::TextureFormat::DXT5;
}

/*
** D3D8's lock flags, filtered to the three the spike's lock contract distinguishes.  The rest
** (D3DLOCK_NOSYSLOCK and friends) are scheduling hints with no Vulkan meaning, so dropping them
** cannot change what the caller sees.
*/
unsigned Translate_Lock_Flags(DWORD flags)
{
	unsigned translated = spike::LOCK_NONE;
	if ((flags & D3DLOCK_READONLY) != 0) translated |= spike::LOCK_READONLY;
	if ((flags & D3DLOCK_DISCARD) != 0) translated |= spike::LOCK_DISCARD;
	if ((flags & D3DLOCK_NOOVERWRITE) != 0) translated |= spike::LOCK_NOOVERWRITE;
	return translated;
}

void Translate_Matrix(const D3DMATRIX & source, spike::Matrix4x4 & out)
{
	// D3DMATRIX is row-major floats and so is spike::Matrix4x4::m, so this is a copy, spelled
	// as a loop rather than a memcpy so a layout change breaks the build instead of the pixels.
	for (int row = 0; row < 4; row++) {
		for (int column = 0; column < 4; column++) {
			out.m[row][column] = source.m[row][column];
		}
	}
}

void Translate_Matrix(const spike::Matrix4x4 & source, D3DMATRIX & out)
{
	for (int row = 0; row < 4; row++) {
		for (int column = 0; column < 4; column++) {
			out.m[row][column] = source.m[row][column];
		}
	}
}

} // anonymous namespace

/***********************************************************************************************
** The D3D8 resource interfaces, implemented over the spike's handles.
**
** These exist because the engine calls IDirect3DTexture8::LockRect, IDirect3DSurface8::GetDesc,
** IDirect3DVertexBuffer8::Lock and AddRef/Release *directly*, in 213 places across 19 files.
** Abstracting those call sites is a separate slice; until it happens, a non-D3D8 backend has to
** hand back objects that answer those calls.  Reference counting is real (the engine's texture
** cache and DX8Wrapper's shadow state both rely on it); Release() at zero destroys.
***********************************************************************************************/

namespace
{

class VulkanD3DTextureClass;

/*
** IDirect3DSurface8 over either a level of a spike texture (GetSurfaceLevel, the render target,
** the depth/stencil buffer) or a system-memory image surface (CreateImageSurface).  D3D8 hands
** the engine one interface for all of those and CopyRects/SetRenderTarget take them
** interchangeably, which is why this class does too.
**
** Two kinds of surface live here, and the difference is visible to callers:
**
**   * a spike::SurfaceHandle surface (level 0 of a texture, the default targets, an image
**     surface).  It has an image view and a layout, so it can be a render target and a CopyRects
**     endpoint, and it locks through Surface_Bits.
**   * a mip-level surface (GetSurfaceLevel(n) for n > 0), which has no handle.  The spike serves a
**     level of a mip chain through Lock_Texture/Unlock_Texture (usage class C4), not through a
**     per-level image view, so this surface locks that way and is NOT usable as a render target or
**     a CopyRects endpoint: both refuse it rather than silently substituting the default target or
**     level 0, which is what its null handle would otherwise have meant.
*/
class VulkanD3DSurfaceClass : public IDirect3DSurface8
{
public:
	VulkanD3DSurfaceClass(spike::RenderBackend * backend, spike::SurfaceHandle * handle,
		VulkanD3DTextureClass * container, unsigned width, unsigned height,
		spike::TextureFormat format, D3DPOOL pool, DWORD usage, unsigned level = 0);
	virtual ~VulkanD3DSurfaceClass();

	spike::SurfaceHandle * Peek_Handle() const { return Handle; }
	// True for a GetSurfaceLevel(n > 0) surface: it locks through its texture's mip level rather
	// than through a surface handle, and it is not a target.
	bool Is_Mip_Level_Surface() const { return Handle == NULL && Container != NULL; }
	spike::TextureFormat Peek_Format() const { return Format; }

	STDMETHOD(QueryInterface)(REFIID riid, void ** object);
	STDMETHOD_(ULONG, AddRef)();
	STDMETHOD_(ULONG, Release)();
	STDMETHOD(GetDevice)(IDirect3DDevice8 ** device);
	STDMETHOD(SetPrivateData)(REFGUID guid, CONST void * data, DWORD size, DWORD flags);
	STDMETHOD(GetPrivateData)(REFGUID guid, void * data, DWORD * size);
	STDMETHOD(FreePrivateData)(REFGUID guid);
	STDMETHOD(GetContainer)(REFIID riid, void ** container);
	STDMETHOD(GetDesc)(D3DSURFACE_DESC * desc);
	STDMETHOD(LockRect)(D3DLOCKED_RECT * locked_rect, CONST RECT * rect, DWORD flags);
	STDMETHOD(UnlockRect)();

private:
	// True when the surface names a level of a lockable texture - every level of such a texture,
	// including level 0, whose handle exists only so that the surface can still be a render
	// target and a CopyRects endpoint.  A lock of one of those has to go through the texture's
	// own level funnel, because that is the funnel that uploads what the caller wrote.
	bool Locks_Through_Texture() const;
	HRESULT Lock_Texture_Level(D3DLOCKED_RECT * locked_rect, CONST RECT * rect, DWORD flags);
	HRESULT Unlock_Texture_Level();

	spike::RenderBackend * Backend;
	spike::SurfaceHandle * Handle;
	VulkanD3DTextureClass * Container;
	unsigned Width;
	unsigned Height;
	spike::TextureFormat Format;
	D3DPOOL Pool;
	DWORD Usage;
	// The mip level this surface names, for a surface that has no handle of its own.
	unsigned Level;
	int RefCount;
};

/*
** IDirect3DTexture8 over a spike texture handle.
**
** Lock usage classes served here (docs/porting/renderer-resource-seam.md): C1 whole-level
** writes and C2 partial-rect writes from the texture loaders, C3 read-back when the engine
** locks with D3DLOCK_READONLY, C4 mip chains locked level by level, and C7/C8 pointers the
** engine keeps past Unlock - all of which are the spike's Lock_Texture/Unlock_Texture contract,
** unchanged.  This class adds no lock semantics of its own; it only forwards.
*/
class VulkanD3DTextureClass : public IDirect3DTexture8
{
public:
	VulkanD3DTextureClass(spike::RenderBackend * backend, spike::TextureHandle * handle,
		unsigned width, unsigned height, unsigned levels, spike::TextureFormat format,
		D3DPOOL pool, DWORD usage);
	virtual ~VulkanD3DTextureClass();

	spike::TextureHandle * Peek_Handle() const { return Handle; }
	// A render target texture's image is written by the GPU and its levels are not lockable, so a
	// lock of one of its surfaces is a read-back and belongs on the surface path; every other
	// texture the backend hands out is lockable level by level.
	bool Is_Lockable() const { return (Usage & D3DUSAGE_RENDERTARGET) == 0; }

	STDMETHOD(QueryInterface)(REFIID riid, void ** object);
	STDMETHOD_(ULONG, AddRef)();
	STDMETHOD_(ULONG, Release)();
	STDMETHOD(GetDevice)(IDirect3DDevice8 ** device);
	STDMETHOD(SetPrivateData)(REFGUID guid, CONST void * data, DWORD size, DWORD flags);
	STDMETHOD(GetPrivateData)(REFGUID guid, void * data, DWORD * size);
	STDMETHOD(FreePrivateData)(REFGUID guid);
	STDMETHOD_(DWORD, SetPriority)(DWORD priority);
	STDMETHOD_(DWORD, GetPriority)();
	STDMETHOD_(void, PreLoad)();
	STDMETHOD_(D3DRESOURCETYPE, GetType)();
	STDMETHOD_(DWORD, SetLOD)(DWORD lod);
	STDMETHOD_(DWORD, GetLOD)();
	STDMETHOD_(DWORD, GetLevelCount)();
	STDMETHOD(GetLevelDesc)(UINT level, D3DSURFACE_DESC * desc);
	STDMETHOD(GetSurfaceLevel)(UINT level, IDirect3DSurface8 ** surface);
	STDMETHOD(LockRect)(UINT level, D3DLOCKED_RECT * locked_rect, CONST RECT * rect, DWORD flags);
	STDMETHOD(UnlockRect)(UINT level);
	STDMETHOD(AddDirtyRect)(CONST RECT * dirty_rect);

private:
	spike::RenderBackend * Backend;
	spike::TextureHandle * Handle;
	unsigned Width;
	unsigned Height;
	unsigned Levels;
	spike::TextureFormat Format;
	D3DPOOL Pool;
	DWORD Usage;
	DWORD LOD;
	int RefCount;
	// GetSurfaceLevel hands out a surface the texture owns, and the engine expects the same
	// pointer for the same level (it compares them when restoring a render target), so they are
	// created once and kept.
	std::vector<VulkanD3DSurfaceClass *> Surfaces;
};

bool VulkanD3DSurfaceClass::Locks_Through_Texture() const
{
	// A level above 0 has no handle and nothing else it could lock through.  Level 0 does have a
	// handle - it has to, so that the surface can be a render target and a CopyRects endpoint -
	// but Surface_Bits is a *read-back* mapping of the image: bytes written into it are never
	// uploaded, so a caller that locks level 0 to write it (W3DVideoBuffer's frame upload, the
	// texture loaders, W3DShroud, the radar) would see every HRESULT succeed, read its own bytes
	// back, and still sample the image the texture was created with.  Those writes travel the
	// texture's own level funnel instead - the same Lock_Texture/Unlock_Texture pair the mip
	// levels use, so there is no second upload path.  A render target texture is excluded: its
	// levels are not lockable, and reading one back through Surface_Bits is the C8 read hazard
	// the seam already serves.
	return Container != NULL && Container->Is_Lockable();
}

/*
** IDirect3DVertexBuffer8 / IDirect3DIndexBuffer8 over the spike's host-mapped buffers.  Class
** C5 (dynamic ring) when D3DUSAGE_DYNAMIC is set and C6 (static fill) when it is not; the ring
** behaviour is the spike's, selected by that flag and not reinterpreted here.
*/
class VulkanD3DVertexBufferClass : public IDirect3DVertexBuffer8
{
public:
	VulkanD3DVertexBufferClass(spike::RenderBackend * backend, spike::VertexBufferHandle * handle,
		unsigned bytes, DWORD usage, DWORD fvf, D3DPOOL pool);
	virtual ~VulkanD3DVertexBufferClass();

	spike::VertexBufferHandle * Peek_Handle() const { return Handle; }

	STDMETHOD(QueryInterface)(REFIID riid, void ** object);
	STDMETHOD_(ULONG, AddRef)();
	STDMETHOD_(ULONG, Release)();
	STDMETHOD(GetDevice)(IDirect3DDevice8 ** device);
	STDMETHOD(SetPrivateData)(REFGUID guid, CONST void * data, DWORD size, DWORD flags);
	STDMETHOD(GetPrivateData)(REFGUID guid, void * data, DWORD * size);
	STDMETHOD(FreePrivateData)(REFGUID guid);
	STDMETHOD_(DWORD, SetPriority)(DWORD priority);
	STDMETHOD_(DWORD, GetPriority)();
	STDMETHOD_(void, PreLoad)();
	STDMETHOD_(D3DRESOURCETYPE, GetType)();
	STDMETHOD(Lock)(UINT offset, UINT size, BYTE ** data, DWORD flags);
	STDMETHOD(Unlock)();
	STDMETHOD(GetDesc)(D3DVERTEXBUFFER_DESC * desc);

private:
	spike::RenderBackend * Backend;
	spike::VertexBufferHandle * Handle;
	unsigned Bytes;
	DWORD Usage;
	DWORD FVF;
	D3DPOOL Pool;
	int RefCount;
};

class VulkanD3DIndexBufferClass : public IDirect3DIndexBuffer8
{
public:
	VulkanD3DIndexBufferClass(spike::RenderBackend * backend, spike::IndexBufferHandle * handle,
		unsigned bytes, DWORD usage, D3DFORMAT format, D3DPOOL pool);
	virtual ~VulkanD3DIndexBufferClass();

	spike::IndexBufferHandle * Peek_Handle() const { return Handle; }

	STDMETHOD(QueryInterface)(REFIID riid, void ** object);
	STDMETHOD_(ULONG, AddRef)();
	STDMETHOD_(ULONG, Release)();
	STDMETHOD(GetDevice)(IDirect3DDevice8 ** device);
	STDMETHOD(SetPrivateData)(REFGUID guid, CONST void * data, DWORD size, DWORD flags);
	STDMETHOD(GetPrivateData)(REFGUID guid, void * data, DWORD * size);
	STDMETHOD(FreePrivateData)(REFGUID guid);
	STDMETHOD_(DWORD, SetPriority)(DWORD priority);
	STDMETHOD_(DWORD, GetPriority)();
	STDMETHOD_(void, PreLoad)();
	STDMETHOD_(D3DRESOURCETYPE, GetType)();
	STDMETHOD(Lock)(UINT offset, UINT size, BYTE ** data, DWORD flags);
	STDMETHOD(Unlock)();
	STDMETHOD(GetDesc)(D3DINDEXBUFFER_DESC * desc);

private:
	spike::RenderBackend * Backend;
	spike::IndexBufferHandle * Handle;
	unsigned Bytes;
	DWORD Usage;
	D3DFORMAT Format;
	D3DPOOL Pool;
	int RefCount;
};

// --- surface ------------------------------------------------------------------------------

VulkanD3DSurfaceClass::VulkanD3DSurfaceClass(spike::RenderBackend * backend,
	spike::SurfaceHandle * handle, VulkanD3DTextureClass * container, unsigned width,
	unsigned height, spike::TextureFormat format, D3DPOOL pool, DWORD usage, unsigned level) :
	Backend(backend),
	Handle(handle),
	Container(container),
	Width(width),
	Height(height),
	Format(format),
	Pool(pool),
	Usage(usage),
	Level(level),
	RefCount(1)
{
}

VulkanD3DSurfaceClass::~VulkanD3DSurfaceClass()
{
	// A level surface and the default targets are owned by their texture or by the backend; an
	// image surface's memory belongs to the backend's pool either way, and the spike frees all
	// of it at Shutdown.  So there is nothing to release here, which is D3D8's ownership too:
	// GetSurfaceLevel's surface does not own its level.
}

HRESULT VulkanD3DSurfaceClass::QueryInterface(REFIID, void ** object)
{
	// The engine never calls QueryInterface on a surface; the D3DX helpers that would are on
	// the Windows side of the seam.  Refusing is honest and cannot be mistaken for support.
	if (object != NULL) *object = NULL;
	return E_NOINTERFACE;
}

ULONG VulkanD3DSurfaceClass::AddRef()
{
	return (ULONG)++RefCount;
}

ULONG VulkanD3DSurfaceClass::Release()
{
	RefCount--;
	if (RefCount > 0) return (ULONG)RefCount;
	// A surface handed out by GetSurfaceLevel is owned by its texture, which keeps it alive for
	// its own lifetime, so reaching zero here frees only the standalone surfaces.
	if (Container == NULL) delete this;
	return 0;
}

HRESULT VulkanD3DSurfaceClass::GetDevice(IDirect3DDevice8 ** device)
{
	if (device != NULL) *device = NULL;
	return Record_Unimplemented("IDirect3DSurface8::GetDevice",
		"there is no IDirect3DDevice8 off Windows", D3DERR_INVALIDCALL);
}

HRESULT VulkanD3DSurfaceClass::SetPrivateData(REFGUID, CONST void *, DWORD, DWORD)
{
	return Record_Unimplemented("IDirect3DSurface8::SetPrivateData",
		"no private data store", D3DERR_INVALIDCALL);
}

HRESULT VulkanD3DSurfaceClass::GetPrivateData(REFGUID, void *, DWORD *)
{
	return Record_Unimplemented("IDirect3DSurface8::GetPrivateData",
		"no private data store", D3DERR_NOTFOUND);
}

HRESULT VulkanD3DSurfaceClass::FreePrivateData(REFGUID)
{
	return D3DERR_NOTFOUND;
}

HRESULT VulkanD3DSurfaceClass::GetContainer(REFIID, void ** container)
{
	if (container == NULL) return D3DERR_INVALIDCALL;
	if (Container == NULL) return D3DERR_INVALIDCALL;
	Container->AddRef();
	*container = Container;
	return D3D_OK;
}

HRESULT VulkanD3DSurfaceClass::GetDesc(D3DSURFACE_DESC * desc)
{
	if (desc == NULL) return D3DERR_INVALIDCALL;
	memset(desc, 0, sizeof(*desc));
	desc->Format = D3DFMT_A8R8G8B8;
	// The engine reads Width/Height/Format from this (SurfaceClass::Get_Description); reporting
	// the format the surface was created with matters, so it is translated back rather than
	// assumed.
	switch (Format) {
		case spike::TextureFormat::X8R8G8B8:	desc->Format = D3DFMT_X8R8G8B8; break;
		case spike::TextureFormat::R8G8B8:		desc->Format = D3DFMT_R8G8B8; break;
		case spike::TextureFormat::A4R4G4B4:	desc->Format = D3DFMT_A4R4G4B4; break;
		case spike::TextureFormat::A1R5G5B5:	desc->Format = D3DFMT_A1R5G5B5; break;
		case spike::TextureFormat::R5G6B5:		desc->Format = D3DFMT_R5G6B5; break;
		case spike::TextureFormat::L8:			desc->Format = D3DFMT_L8; break;
		case spike::TextureFormat::A8:			desc->Format = D3DFMT_A8; break;
		case spike::TextureFormat::A8L8:		desc->Format = D3DFMT_A8L8; break;
		case spike::TextureFormat::V8U8:		desc->Format = D3DFMT_V8U8; break;
		case spike::TextureFormat::P8:			desc->Format = D3DFMT_P8; break;
		case spike::TextureFormat::DXT1:		desc->Format = D3DFMT_DXT1; break;
		case spike::TextureFormat::DXT2:		desc->Format = D3DFMT_DXT2; break;
		case spike::TextureFormat::DXT3:		desc->Format = D3DFMT_DXT3; break;
		case spike::TextureFormat::DXT4:		desc->Format = D3DFMT_DXT4; break;
		case spike::TextureFormat::DXT5:		desc->Format = D3DFMT_DXT5; break;
		default:								break;
	}
	desc->Type = D3DRTYPE_SURFACE;
	desc->Usage = Usage;
	desc->Pool = Pool;
	desc->MultiSampleType = D3DMULTISAMPLE_NONE;
	desc->Width = Width;
	desc->Height = Height;
	desc->Size = Width * Height * Format_Bytes_Per_Pixel(Format);
	if (Is_Compressed(Format)) {
		const unsigned block_bytes = (Format == spike::TextureFormat::DXT1) ? 8 : 16;
		desc->Size = ((Width + 3) / 4) * ((Height + 3) / 4) * block_bytes;
	}
	return D3D_OK;
}

HRESULT VulkanD3DSurfaceClass::LockRect(D3DLOCKED_RECT * locked_rect, CONST RECT * rect,
	DWORD flags)
{
	if (locked_rect == NULL || Backend == NULL) return D3DERR_INVALIDCALL;
	// A surface that names a level of a lockable texture locks that level, which is the spike's
	// own per-level lock (usage classes C1/C4) and therefore serves a sub-rect honestly, unlike
	// Surface_Bits below - and, unlike Surface_Bits, uploads the bytes the caller writes.
	if (Locks_Through_Texture()) return Lock_Texture_Level(locked_rect, rect, flags);
	if (Handle == NULL) return D3DERR_INVALIDCALL;
	if (rect != NULL) {
		// Usage class C9 (the D3DX surface locks) and SurfaceClass::Lock both lock whole
		// surfaces; the spike's Surface_Bits has no sub-rect form, so a partial surface lock is
		// refused rather than served as a whole-surface lock the caller would then index wrongly.
		return Record_Unimplemented("IDirect3DSurface8::LockRect(sub-rect)",
			"spike::RenderBackend::Surface_Bits locks whole surfaces only", D3DERR_INVALIDCALL);
	}
	(void)flags;
	spike::LockedRect bits;
	if (!Backend->Surface_Bits(Handle, bits)) return D3DERR_INVALIDCALL;
	locked_rect->pBits = bits.bits;
	locked_rect->Pitch = (INT)bits.pitch;
	return D3D_OK;
}

HRESULT VulkanD3DSurfaceClass::UnlockRect()
{
	// A texture level's unlock is its texture's, which is what uploads the level the caller just
	// wrote: skipping it would leave the image holding whatever it was created with while every
	// HRESULT said the write had landed.
	if (Locks_Through_Texture()) return Unlock_Texture_Level();
	// Surface_Bits hands out a persistent host mapping (usage class C7: the engine keeps the
	// pointer past the unlock), so there is nothing to undo.  Saying so is not the same as
	// pretending: the mapping is still valid, which is exactly what the callers rely on.
	return D3D_OK;
}

HRESULT VulkanD3DSurfaceClass::Lock_Texture_Level(D3DLOCKED_RECT * locked_rect, CONST RECT * rect,
	DWORD flags)
{
	return Container->LockRect(Level, locked_rect, rect, flags);
}

HRESULT VulkanD3DSurfaceClass::Unlock_Texture_Level()
{
	return Container->UnlockRect(Level);
}

// --- texture ------------------------------------------------------------------------------

VulkanD3DTextureClass::VulkanD3DTextureClass(spike::RenderBackend * backend,
	spike::TextureHandle * handle, unsigned width, unsigned height, unsigned levels,
	spike::TextureFormat format, D3DPOOL pool, DWORD usage) :
	Backend(backend),
	Handle(handle),
	Width(width),
	Height(height),
	Levels(levels),
	Format(format),
	Pool(pool),
	Usage(usage),
	LOD(0),
	RefCount(1)
{
	Surfaces.resize(levels, NULL);
}

VulkanD3DTextureClass::~VulkanD3DTextureClass()
{
	for (unsigned index = 0; index < Surfaces.size(); index++) {
		delete Surfaces[index];
	}
}

HRESULT VulkanD3DTextureClass::QueryInterface(REFIID, void ** object)
{
	if (object != NULL) *object = NULL;
	return E_NOINTERFACE;
}

ULONG VulkanD3DTextureClass::AddRef()
{
	return (ULONG)++RefCount;
}

ULONG VulkanD3DTextureClass::Release()
{
	RefCount--;
	if (RefCount > 0) return (ULONG)RefCount;
	// The spike owns the image until Shutdown (it has no per-resource destroy entry point), so
	// this frees the wrapper only.  That is a real difference from D3D8 and it is a leak of
	// device memory across a long session, recorded in docs/porting/renderer-integration.md
	// rather than papered over here.
	delete this;
	return 0;
}

HRESULT VulkanD3DTextureClass::GetDevice(IDirect3DDevice8 ** device)
{
	if (device != NULL) *device = NULL;
	return Record_Unimplemented("IDirect3DTexture8::GetDevice",
		"there is no IDirect3DDevice8 off Windows", D3DERR_INVALIDCALL);
}

HRESULT VulkanD3DTextureClass::SetPrivateData(REFGUID, CONST void *, DWORD, DWORD)
{
	return Record_Unimplemented("IDirect3DTexture8::SetPrivateData",
		"no private data store", D3DERR_INVALIDCALL);
}

HRESULT VulkanD3DTextureClass::GetPrivateData(REFGUID, void *, DWORD *)
{
	return Record_Unimplemented("IDirect3DTexture8::GetPrivateData",
		"no private data store", D3DERR_NOTFOUND);
}

HRESULT VulkanD3DTextureClass::FreePrivateData(REFGUID)
{
	return D3DERR_NOTFOUND;
}

DWORD VulkanD3DTextureClass::SetPriority(DWORD)
{
	// D3D8's managed-pool eviction priority.  The spike has no managed pool, so the priority is
	// accepted and ignored; nothing observable depends on the returned previous value.
	return 0;
}

DWORD VulkanD3DTextureClass::GetPriority()
{
	return 0;
}

void VulkanD3DTextureClass::PreLoad()
{
	// D3D8 uploads a managed texture to video memory here.  The spike uploads at Unlock, so the
	// texture is already resident and there is genuinely nothing to do.
}

D3DRESOURCETYPE VulkanD3DTextureClass::GetType()
{
	return D3DRTYPE_TEXTURE;
}

DWORD VulkanD3DTextureClass::SetLOD(DWORD lod)
{
	const DWORD previous = LOD;
	LOD = lod;
	return previous;
}

DWORD VulkanD3DTextureClass::GetLOD()
{
	return LOD;
}

DWORD VulkanD3DTextureClass::GetLevelCount()
{
	return (DWORD)Levels;
}

HRESULT VulkanD3DTextureClass::GetLevelDesc(UINT level, D3DSURFACE_DESC * desc)
{
	if (desc == NULL || level >= Levels) return D3DERR_INVALIDCALL;
	unsigned width = Width >> level;
	unsigned height = Height >> level;
	if (width == 0) width = 1;
	if (height == 0) height = 1;
	VulkanD3DSurfaceClass level_surface(Backend, NULL, NULL, width, height, Format, Pool, Usage);
	const HRESULT result = level_surface.GetDesc(desc);
	if (SUCCEEDED(result)) desc->Type = D3DRTYPE_TEXTURE;
	return result;
}

HRESULT VulkanD3DTextureClass::GetSurfaceLevel(UINT level, IDirect3DSurface8 ** surface)
{
	if (surface == NULL) return D3DERR_INVALIDCALL;
	*surface = NULL;
	if (level >= Levels) return D3DERR_INVALIDCALL;
	if (Surfaces[level] == NULL) {
		// Level 0 is asked of the backend because that surface has to BE the texture's image as
		// far as the render target and CopyRects paths are concerned.  Above level 0 the spike
		// serves no per-level view, so the surface is a named level of this texture instead: it
		// locks through Lock_Texture and refuses to be a target.  MissingTexture::_Init() and
		// SurfaceClass's mip walk need exactly the lock, not a view.
		spike::SurfaceHandle * handle = NULL;
		if (level == 0) {
			handle = Backend->Get_Surface_Level(Handle, level);
			if (handle == NULL) return D3DERR_INVALIDCALL;
		}
		unsigned width = Width >> level;
		unsigned height = Height >> level;
		if (width == 0) width = 1;
		if (height == 0) height = 1;
		Surfaces[level] = new VulkanD3DSurfaceClass(Backend, handle, this, width, height, Format,
			Pool, Usage, level);
	}
	Surfaces[level]->AddRef();
	*surface = Surfaces[level];
	return D3D_OK;
}

HRESULT VulkanD3DTextureClass::LockRect(UINT level, D3DLOCKED_RECT * locked_rect,
	CONST RECT * rect, DWORD flags)
{
	if (locked_rect == NULL || level >= Levels) return D3DERR_INVALIDCALL;
	spike::LockRect lock_rect;
	const spike::LockRect * lock_rect_pointer = NULL;
	if (rect != NULL) {
		lock_rect.left = (unsigned)rect->left;
		lock_rect.top = (unsigned)rect->top;
		lock_rect.right = (unsigned)rect->right;
		lock_rect.bottom = (unsigned)rect->bottom;
		lock_rect_pointer = &lock_rect;
	}
	spike::LockedRect locked;
	if (!Backend->Lock_Texture(Handle, level, lock_rect_pointer, Translate_Lock_Flags(flags),
			locked)) {
		return D3DERR_INVALIDCALL;
	}
	locked_rect->pBits = locked.bits;
	locked_rect->Pitch = (INT)locked.pitch;
	return D3D_OK;
}

HRESULT VulkanD3DTextureClass::UnlockRect(UINT level)
{
	if (level >= Levels) return D3DERR_INVALIDCALL;
	return Backend->Unlock_Texture(Handle, level) ? D3D_OK : D3DERR_INVALIDCALL;
}

HRESULT VulkanD3DTextureClass::AddDirtyRect(CONST RECT *)
{
	// D3D8 uses the dirty-rect list to decide what UpdateTexture copies.  The spike's
	// Update_Texture copies whole levels, so the hint changes nothing and dropping it cannot
	// lose data - it can only copy more than D3D8 would.
	return D3D_OK;
}

// --- vertex buffer ------------------------------------------------------------------------

VulkanD3DVertexBufferClass::VulkanD3DVertexBufferClass(spike::RenderBackend * backend,
	spike::VertexBufferHandle * handle, unsigned bytes, DWORD usage, DWORD fvf, D3DPOOL pool) :
	Backend(backend),
	Handle(handle),
	Bytes(bytes),
	Usage(usage),
	FVF(fvf),
	Pool(pool),
	RefCount(1)
{
}

VulkanD3DVertexBufferClass::~VulkanD3DVertexBufferClass()
{
}

HRESULT VulkanD3DVertexBufferClass::QueryInterface(REFIID, void ** object)
{
	if (object != NULL) *object = NULL;
	return E_NOINTERFACE;
}

ULONG VulkanD3DVertexBufferClass::AddRef()
{
	return (ULONG)++RefCount;
}

ULONG VulkanD3DVertexBufferClass::Release()
{
	RefCount--;
	if (RefCount > 0) return (ULONG)RefCount;
	delete this;
	return 0;
}

HRESULT VulkanD3DVertexBufferClass::GetDevice(IDirect3DDevice8 ** device)
{
	if (device != NULL) *device = NULL;
	return Record_Unimplemented("IDirect3DVertexBuffer8::GetDevice",
		"there is no IDirect3DDevice8 off Windows", D3DERR_INVALIDCALL);
}

HRESULT VulkanD3DVertexBufferClass::SetPrivateData(REFGUID, CONST void *, DWORD, DWORD)
{
	return Record_Unimplemented("IDirect3DVertexBuffer8::SetPrivateData",
		"no private data store", D3DERR_INVALIDCALL);
}

HRESULT VulkanD3DVertexBufferClass::GetPrivateData(REFGUID, void *, DWORD *)
{
	return Record_Unimplemented("IDirect3DVertexBuffer8::GetPrivateData",
		"no private data store", D3DERR_NOTFOUND);
}

HRESULT VulkanD3DVertexBufferClass::FreePrivateData(REFGUID)
{
	return D3DERR_NOTFOUND;
}

DWORD VulkanD3DVertexBufferClass::SetPriority(DWORD)
{
	return 0;
}

DWORD VulkanD3DVertexBufferClass::GetPriority()
{
	return 0;
}

void VulkanD3DVertexBufferClass::PreLoad()
{
}

D3DRESOURCETYPE VulkanD3DVertexBufferClass::GetType()
{
	return D3DRTYPE_VERTEXBUFFER;
}

HRESULT VulkanD3DVertexBufferClass::Lock(UINT offset, UINT size, BYTE ** data, DWORD flags)
{
	if (data == NULL) return D3DERR_INVALIDCALL;
	*data = NULL;
	// D3D8: size 0 locks from the offset to the end of the buffer.
	const UINT locked_size = (size == 0) ? (Bytes - offset) : size;
	void * bits = NULL;
	if (!Backend->Lock_Vertex_Buffer(Handle, offset, locked_size, Translate_Lock_Flags(flags),
			&bits)) {
		return D3DERR_INVALIDCALL;
	}
	*data = (BYTE *)bits;
	return D3D_OK;
}

HRESULT VulkanD3DVertexBufferClass::Unlock()
{
	return Backend->Unlock_Vertex_Buffer(Handle) ? D3D_OK : D3DERR_INVALIDCALL;
}

HRESULT VulkanD3DVertexBufferClass::GetDesc(D3DVERTEXBUFFER_DESC * desc)
{
	if (desc == NULL) return D3DERR_INVALIDCALL;
	memset(desc, 0, sizeof(*desc));
	desc->Format = D3DFMT_VERTEXDATA;
	desc->Type = D3DRTYPE_VERTEXBUFFER;
	desc->Usage = Usage;
	desc->Pool = Pool;
	desc->Size = Bytes;
	desc->FVF = FVF;
	return D3D_OK;
}

// --- index buffer -------------------------------------------------------------------------

VulkanD3DIndexBufferClass::VulkanD3DIndexBufferClass(spike::RenderBackend * backend,
	spike::IndexBufferHandle * handle, unsigned bytes, DWORD usage, D3DFORMAT format,
	D3DPOOL pool) :
	Backend(backend),
	Handle(handle),
	Bytes(bytes),
	Usage(usage),
	Format(format),
	Pool(pool),
	RefCount(1)
{
}

VulkanD3DIndexBufferClass::~VulkanD3DIndexBufferClass()
{
}

HRESULT VulkanD3DIndexBufferClass::QueryInterface(REFIID, void ** object)
{
	if (object != NULL) *object = NULL;
	return E_NOINTERFACE;
}

ULONG VulkanD3DIndexBufferClass::AddRef()
{
	return (ULONG)++RefCount;
}

ULONG VulkanD3DIndexBufferClass::Release()
{
	RefCount--;
	if (RefCount > 0) return (ULONG)RefCount;
	delete this;
	return 0;
}

HRESULT VulkanD3DIndexBufferClass::GetDevice(IDirect3DDevice8 ** device)
{
	if (device != NULL) *device = NULL;
	return Record_Unimplemented("IDirect3DIndexBuffer8::GetDevice",
		"there is no IDirect3DDevice8 off Windows", D3DERR_INVALIDCALL);
}

HRESULT VulkanD3DIndexBufferClass::SetPrivateData(REFGUID, CONST void *, DWORD, DWORD)
{
	return Record_Unimplemented("IDirect3DIndexBuffer8::SetPrivateData",
		"no private data store", D3DERR_INVALIDCALL);
}

HRESULT VulkanD3DIndexBufferClass::GetPrivateData(REFGUID, void *, DWORD *)
{
	return Record_Unimplemented("IDirect3DIndexBuffer8::GetPrivateData",
		"no private data store", D3DERR_NOTFOUND);
}

HRESULT VulkanD3DIndexBufferClass::FreePrivateData(REFGUID)
{
	return D3DERR_NOTFOUND;
}

DWORD VulkanD3DIndexBufferClass::SetPriority(DWORD)
{
	return 0;
}

DWORD VulkanD3DIndexBufferClass::GetPriority()
{
	return 0;
}

void VulkanD3DIndexBufferClass::PreLoad()
{
}

D3DRESOURCETYPE VulkanD3DIndexBufferClass::GetType()
{
	return D3DRTYPE_INDEXBUFFER;
}

HRESULT VulkanD3DIndexBufferClass::Lock(UINT offset, UINT size, BYTE ** data, DWORD flags)
{
	if (data == NULL) return D3DERR_INVALIDCALL;
	*data = NULL;
	const UINT locked_bytes = (size == 0) ? (Bytes - offset) : size;
	// The engine's index buffers are 16-bit throughout (DX8IndexBufferClass), and so is the
	// spike's Set_Index_Buffer, so the byte offsets D3D8 uses convert exactly.
	void * bits = NULL;
	if (!Backend->Lock_Index_Buffer(Handle, offset / 2, locked_bytes / 2,
			Translate_Lock_Flags(flags), &bits)) {
		return D3DERR_INVALIDCALL;
	}
	*data = (BYTE *)bits;
	return D3D_OK;
}

HRESULT VulkanD3DIndexBufferClass::Unlock()
{
	return Backend->Unlock_Index_Buffer(Handle) ? D3D_OK : D3DERR_INVALIDCALL;
}

HRESULT VulkanD3DIndexBufferClass::GetDesc(D3DINDEXBUFFER_DESC * desc)
{
	if (desc == NULL) return D3DERR_INVALIDCALL;
	memset(desc, 0, sizeof(*desc));
	desc->Format = Format;
	desc->Type = D3DRTYPE_INDEXBUFFER;
	desc->Usage = Usage;
	desc->Pool = Pool;
	desc->Size = Bytes;
	return D3D_OK;
}

} // anonymous namespace

/***********************************************************************************************
** VulkanRenderBackendClass
***********************************************************************************************/

struct VulkanRenderBackendClass::InternalsStruct
{
	spike::RenderBackend * Backend;
	bool InterfaceOpen;
	std::vector<spike::AdapterInfo> Adapters;
	// The adapter the device was actually created on, so GetDeviceCaps(device) answers from the
	// same measurements Create_Device chose with.
	spike::AdapterInfo DeviceAdapter;
	bool HaveDeviceAdapter;
	D3DPRESENT_PARAMETERS PresentParameters;
	// The default targets, wrapped once: GetRenderTarget/GetDepthStencilSurface hand these out
	// every time the engine saves and restores a render target, and the engine compares the
	// pointers, so they have to be stable.
	VulkanD3DSurfaceClass * DefaultColorSurface;
	VulkanD3DSurfaceClass * DefaultDepthSurface;

	InternalsStruct() :
		Backend(NULL),
		InterfaceOpen(false),
		HaveDeviceAdapter(false),
		DefaultColorSurface(NULL),
		DefaultDepthSurface(NULL)
	{
		memset(&PresentParameters, 0, sizeof(PresentParameters));
	}
};

VulkanRenderBackendClass TheVulkanRenderBackend;

VulkanRenderBackendClass::VulkanRenderBackendClass() :
	Internals(new InternalsStruct())
{
}

VulkanRenderBackendClass::~VulkanRenderBackendClass()
{
	Release_Device();
	delete Internals;
	Internals = NULL;
}

/*
** Whether the Vulkan validation layer is asked for.  Off by default because it costs frame time
** and prints to stderr; on with ZH_VULKAN_VALIDATION=1, which is how the native run in
** docs/porting/renderer-integration.md was measured.
*/
static bool Validation_Requested()
{
	const char * setting = getenv("ZH_VULKAN_VALIDATION");
	return setting != NULL && setting[0] == '1';
}

// --- lifecycle ----------------------------------------------------------------------------

bool VulkanRenderBackendClass::Open()
{
	if (Internals->InterfaceOpen) return true;
	// D3D8's Open() is LoadLibrary + Direct3DCreate8: it makes the *adapter* queries possible
	// and creates no device.  The Vulkan equivalent is enumerating the physical devices through
	// a temporary instance, which is what spike::Enumerate_Adapters does.
	if (!spike::Enumerate_Adapters(Internals->Adapters, Validation_Requested())) {
		WWDEBUG_SAY(("VulkanRenderBackend: no Vulkan adapter could be enumerated\n"));
		return false;
	}
	Internals->InterfaceOpen = true;
	WWDEBUG_SAY(("VulkanRenderBackend: %u Vulkan adapter(s)\n",
		(unsigned)Internals->Adapters.size()));
	for (unsigned index = 0; index < Internals->Adapters.size(); index++) {
		WWDEBUG_SAY(("  adapter %u: %s (%s), %u MiB device-local\n", index,
			Internals->Adapters[index].name.c_str(),
			Internals->Adapters[index].driver.c_str(),
			(unsigned)(Internals->Adapters[index].device_memory_bytes / (1024 * 1024))));
	}
	return true;
}

void VulkanRenderBackendClass::Release_Interface()
{
	Release_Device();
	Internals->Adapters.clear();
	Internals->InterfaceOpen = false;
}

void VulkanRenderBackendClass::Free_Library()
{
	// The Vulkan loader is linked, not loaded by hand, so there is no library to free.  D3D8's
	// two-step shutdown exists because Direct3DCreate8 came out of a LoadLibrary; this half of
	// it has nothing to do here.
}

bool VulkanRenderBackendClass::Has_Interface() const
{
	return Internals->InterfaceOpen;
}

HRESULT VulkanRenderBackendClass::Create_Device(UINT adapter, D3DDEVTYPE device_type,
	HWND focus_window, DWORD behavior_flags, D3DPRESENT_PARAMETERS* present_parameters)
{
	if (!Internals->InterfaceOpen) return D3DERR_INVALIDCALL;
	if (present_parameters == NULL) return D3DERR_INVALIDCALL;
	if (adapter >= Internals->Adapters.size()) return D3DERR_INVALIDCALL;
	if (Internals->Backend != NULL) return D3DERR_INVALIDCALL;
	(void)device_type;
	(void)behavior_flags;

	// Renderer boundary: these are already pixels.  DX8Wrapper computes the back buffer size
	// from the resolution the engine chose and the window's client size in points is converted
	// on the way in (docs/porting/decisions-resolved.md), so the backend is handed pixels and
	// this layer does not scale them again.
	unsigned width = present_parameters->BackBufferWidth;
	unsigned height = present_parameters->BackBufferHeight;
	if (width == 0 || height == 0) {
		WWDEBUG_SAY(("VulkanRenderBackend: Create_Device with a 0-sized back buffer\n"));
		return D3DERR_INVALIDCALL;
	}

	spike::RenderBackend * backend = spike::Create_Vulkan_Backend(Validation_Requested(),
		focus_window == NULL);
	if (backend == NULL) return E_OUTOFMEMORY;
	if (!backend->Init((void *)focus_window, width, height)) {
		// The backend prints the Vulkan call that failed; this records which D3D8 request it
		// was refusing, so the two halves of the failure are in one log.
		WWDEBUG_SAY(("VulkanRenderBackend: spike::RenderBackend::Init(%ux%u, window %p) failed\n",
			width, height, (void *)focus_window));
		delete backend;
		return D3DERR_NOTAVAILABLE;
	}
	Internals->Backend = backend;
	Internals->PresentParameters = *present_parameters;
	Internals->HaveDeviceAdapter = backend->Get_Adapter_Info(Internals->DeviceAdapter);
	if (Internals->HaveDeviceAdapter
			&& Internals->DeviceAdapter.device_id != Internals->Adapters[adapter].device_id) {
		// The spike picks its own physical device (discrete first).  D3D8 lets the caller pick,
		// and the engine does pick, so a mismatch is reported rather than hidden: the caps the
		// engine was given came from the adapter it asked about, and the device it got may not
		// be that one.
		WWDEBUG_SAY(("VulkanRenderBackend: adapter %u was requested (device 0x%x) but the "
			"backend chose device 0x%x\n", adapter,
			(unsigned)Internals->Adapters[adapter].device_id,
			(unsigned)Internals->DeviceAdapter.device_id));
		Record_Unimplemented("IDirect3D8::CreateDevice(adapter selection)",
			"spike::Create_Vulkan_Backend picks the physical device itself", D3D_OK);
	}
	WWDEBUG_SAY(("VulkanRenderBackend: device created, %ux%u on %s\n", width, height,
		backend->Device_Description()));
	return D3D_OK;
}

void VulkanRenderBackendClass::Release_Device()
{
	if (Internals == NULL) return;
	delete Internals->DefaultColorSurface;
	Internals->DefaultColorSurface = NULL;
	delete Internals->DefaultDepthSurface;
	Internals->DefaultDepthSurface = NULL;
	if (Internals->Backend != NULL) {
		Internals->Backend->Shutdown();
		delete Internals->Backend;
		Internals->Backend = NULL;
	}
	Internals->HaveDeviceAdapter = false;
}

bool VulkanRenderBackendClass::Has_Device() const
{
	return Internals->Backend != NULL;
}

// --- frame --------------------------------------------------------------------------------

HRESULT VulkanRenderBackendClass::BeginScene()
{
	if (Internals->Backend == NULL) return D3DERR_INVALIDCALL;
	Internals->Backend->Begin_Scene();
	return D3D_OK;
}

HRESULT VulkanRenderBackendClass::EndScene()
{
	if (Internals->Backend == NULL) return D3DERR_INVALIDCALL;
	// D3D8 does not present here; DX8Wrapper::End_Scene decides that separately and calls
	// Present() below when it wants a flip.
	Internals->Backend->End_Scene(false);
	return D3D_OK;
}

HRESULT VulkanRenderBackendClass::Clear(DWORD count, CONST D3DRECT* rects, DWORD flags,
	D3DCOLOR color, float z, DWORD stencil)
{
	if (Internals->Backend == NULL) return D3DERR_INVALIDCALL;
	if (count != 0 && rects != NULL) {
		// D3D8's rect list clears sub-rectangles; the spike clears the whole attachment.  The
		// engine's only non-null-rect clear would be a partial clear, so refusing is the honest
		// answer - a full clear would erase pixels the caller asked to keep.
		return Record_Unimplemented("IDirect3DDevice8::Clear(rect list)",
			"spike::RenderBackend::Clear clears whole attachments", D3DERR_INVALIDCALL);
	}
	const bool clear_color = (flags & D3DCLEAR_TARGET) != 0;
	const bool clear_depth = (flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)) != 0;
	const float red = (float)((color >> 16) & 0xff) / 255.0f;
	const float green = (float)((color >> 8) & 0xff) / 255.0f;
	const float blue = (float)(color & 0xff) / 255.0f;
	const float alpha = (float)((color >> 24) & 0xff) / 255.0f;
	Internals->Backend->Clear(clear_color, clear_depth, red, green, blue, alpha, z,
		(uint32_t)stencil);
	return D3D_OK;
}

HRESULT VulkanRenderBackendClass::Present(CONST RECT* source_rect, CONST RECT* dest_rect,
	HWND dest_window_override, CONST RGNDATA* dirty_region)
{
	if (Internals->Backend == NULL) return D3DERR_INVALIDCALL;
	if (source_rect != NULL || dest_rect != NULL || dest_window_override != NULL
			|| dirty_region != NULL) {
		return Record_Unimplemented("IDirect3DDevice8::Present(sub-rect or other window)",
			"the swapchain presents the whole colour target to its own window",
			D3DERR_INVALIDCALL);
	}
	return Internals->Backend->Present() ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

// --- device state -------------------------------------------------------------------------

HRESULT VulkanRenderBackendClass::SetRenderState(D3DRENDERSTATETYPE state, DWORD value)
{
	if (Internals->Backend == NULL) return D3DERR_INVALIDCALL;
	Internals->Backend->Set_DX8_Render_State((spike::D3DRENDERSTATETYPE)(unsigned)state,
		(uint32_t)value);
	return D3D_OK;
}

HRESULT VulkanRenderBackendClass::GetRenderState(D3DRENDERSTATETYPE state, DWORD* value)
{
	if (Internals->Backend == NULL || value == NULL) return D3DERR_INVALIDCALL;
	*value = (DWORD)Internals->Backend->Get_DX8_Render_State(
		(spike::D3DRENDERSTATETYPE)(unsigned)state);
	return D3D_OK;
}

HRESULT VulkanRenderBackendClass::SetTextureStageState(DWORD stage,
	D3DTEXTURESTAGESTATETYPE type, DWORD value)
{
	if (Internals->Backend == NULL) return D3DERR_INVALIDCALL;
	Internals->Backend->Set_DX8_Texture_Stage_State((uint32_t)stage,
		(spike::D3DTEXTURESTAGESTATETYPE)(unsigned)type, (uint32_t)value);
	return D3D_OK;
}

HRESULT VulkanRenderBackendClass::SetTexture(DWORD stage, IDirect3DBaseTexture8* texture)
{
	if (Internals->Backend == NULL) return D3DERR_INVALIDCALL;
	spike::TextureHandle * handle = NULL;
	if (texture != NULL) {
		// Every texture the engine can bind came out of CreateTexture below, so it is one of
		// this file's wrappers; a static_cast is safe and a dynamic_cast would only hide a bug
		// in which some other implementation reached here.
		handle = ((VulkanD3DTextureClass *)texture)->Peek_Handle();
	}
	Internals->Backend->Set_Texture((uint32_t)stage, handle);
	return D3D_OK;
}

HRESULT VulkanRenderBackendClass::SetTransform(D3DTRANSFORMSTATETYPE state,
	CONST D3DMATRIX* matrix)
{
	if (Internals->Backend == NULL || matrix == NULL) return D3DERR_INVALIDCALL;
	spike::Matrix4x4 translated;
	Translate_Matrix(*matrix, translated);
	Internals->Backend->Set_Transform((spike::D3DTRANSFORMSTATETYPE)(unsigned)state, translated);
	return D3D_OK;
}

HRESULT VulkanRenderBackendClass::GetTransform(D3DTRANSFORMSTATETYPE state, D3DMATRIX* matrix)
{
	if (Internals->Backend == NULL || matrix == NULL) return D3DERR_INVALIDCALL;
	spike::Matrix4x4 shadowed;
	Internals->Backend->Get_Transform((spike::D3DTRANSFORMSTATETYPE)(unsigned)state, shadowed);
	Translate_Matrix(shadowed, *matrix);
	return D3D_OK;
}

HRESULT VulkanRenderBackendClass::SetViewport(CONST D3DVIEWPORT8* viewport)
{
	if (Internals->Backend == NULL || viewport == NULL) return D3DERR_INVALIDCALL;
	spike::ViewportRect rect;
	rect.x = (int32_t)viewport->X;
	rect.y = (int32_t)viewport->Y;
	rect.width = (uint32_t)viewport->Width;
	rect.height = (uint32_t)viewport->Height;
	rect.min_z = viewport->MinZ;
	rect.max_z = viewport->MaxZ;
	Internals->Backend->Set_Viewport(rect);
	return D3D_OK;
}

HRESULT VulkanRenderBackendClass::GetViewport(D3DVIEWPORT8* viewport)
{
	if (Internals->Backend == NULL || viewport == NULL) return D3DERR_INVALIDCALL;
	spike::ViewportRect rect;
	Internals->Backend->Get_Viewport(rect);
	viewport->X = (DWORD)rect.x;
	viewport->Y = (DWORD)rect.y;
	viewport->Width = (DWORD)rect.width;
	viewport->Height = (DWORD)rect.height;
	viewport->MinZ = rect.min_z;
	viewport->MaxZ = rect.max_z;
	return D3D_OK;
}

HRESULT VulkanRenderBackendClass::SetMaterial(CONST D3DMATERIAL8* material)
{
	if (Internals->Backend == NULL || material == NULL) return D3DERR_INVALIDCALL;
	spike::MaterialState state;
	state.diffuse[0] = material->Diffuse.r;
	state.diffuse[1] = material->Diffuse.g;
	state.diffuse[2] = material->Diffuse.b;
	state.diffuse[3] = material->Diffuse.a;
	state.ambient[0] = material->Ambient.r;
	state.ambient[1] = material->Ambient.g;
	state.ambient[2] = material->Ambient.b;
	state.ambient[3] = material->Ambient.a;
	state.specular[0] = material->Specular.r;
	state.specular[1] = material->Specular.g;
	state.specular[2] = material->Specular.b;
	state.specular[3] = material->Specular.a;
	state.emissive[0] = material->Emissive.r;
	state.emissive[1] = material->Emissive.g;
	state.emissive[2] = material->Emissive.b;
	state.emissive[3] = material->Emissive.a;
	state.power = material->Power;
	Internals->Backend->Set_Material(state);
	return D3D_OK;
}

HRESULT VulkanRenderBackendClass::SetLight(DWORD index, CONST D3DLIGHT8* light)
{
	if (Internals->Backend == NULL || light == NULL) return D3DERR_INVALIDCALL;
	spike::LightState state;
	state.type = (uint32_t)light->Type;
	state.diffuse[0] = light->Diffuse.r;
	state.diffuse[1] = light->Diffuse.g;
	state.diffuse[2] = light->Diffuse.b;
	state.diffuse[3] = light->Diffuse.a;
	state.specular[0] = light->Specular.r;
	state.specular[1] = light->Specular.g;
	state.specular[2] = light->Specular.b;
	state.specular[3] = light->Specular.a;
	state.ambient[0] = light->Ambient.r;
	state.ambient[1] = light->Ambient.g;
	state.ambient[2] = light->Ambient.b;
	state.ambient[3] = light->Ambient.a;
	state.position[0] = light->Position.x;
	state.position[1] = light->Position.y;
	state.position[2] = light->Position.z;
	state.direction[0] = light->Direction.x;
	state.direction[1] = light->Direction.y;
	state.direction[2] = light->Direction.z;
	state.range = light->Range;
	state.falloff = light->Falloff;
	state.attenuation0 = light->Attenuation0;
	state.attenuation1 = light->Attenuation1;
	state.attenuation2 = light->Attenuation2;
	state.theta = light->Theta;
	state.phi = light->Phi;
	Internals->Backend->Set_Light((uint32_t)index, &state);
	return D3D_OK;
}

HRESULT VulkanRenderBackendClass::LightEnable(DWORD index, BOOL enable)
{
	if (Internals->Backend == NULL) return D3DERR_INVALIDCALL;
	if (enable) {
		// D3D8 re-enables the light the slot already holds.  The spike's Set_Light(index, null)
		// is the disable, and its state is shadowed, so an enable with no accompanying SetLight
		// has nothing to restore from here.  The engine always sets the light it enables
		// (LightEnvironmentClass), so this is reported rather than guessed at.
		return Record_Unimplemented("IDirect3DDevice8::LightEnable(TRUE)",
			"the backend has no separate enable for a previously set light", D3DERR_INVALIDCALL);
	}
	Internals->Backend->Set_Light((uint32_t)index, NULL);
	return D3D_OK;
}

HRESULT VulkanRenderBackendClass::SetClipPlane(DWORD index, CONST float* plane)
{
	if (Internals->Backend == NULL || plane == NULL) return D3DERR_INVALIDCALL;
	Internals->Backend->Set_Clip_Plane((uint32_t)index, plane);
	return D3D_OK;
}

// --- shaders ------------------------------------------------------------------------------

HRESULT VulkanRenderBackendClass::CreateVertexShader(CONST DWORD* declaration,
	CONST DWORD* function, DWORD* handle, DWORD usage)
{
	if (Internals->Backend == NULL || handle == NULL) return D3DERR_INVALIDCALL;
	*handle = 0;
	const spike::ShaderHandle created = Internals->Backend->Create_Vertex_Shader(
		(const uint32_t *)declaration, (const uint32_t *)function, (uint32_t)usage);
	if (created == spike::kNullShader) {
		// The spike returns kNullShader for a token stream it cannot translate, which is a
		// finding about that shader rather than about this call: the failure is passed on.
		return Record_Unimplemented("IDirect3DDevice8::CreateVertexShader(untranslatable)",
			"the vs.1.1 token stream is not one the backend can translate", D3DERR_INVALIDCALL);
	}
	*handle = (DWORD)created;
	return D3D_OK;
}

HRESULT VulkanRenderBackendClass::DeleteVertexShader(DWORD handle)
{
	if (Internals->Backend == NULL) return D3DERR_INVALIDCALL;
	Internals->Backend->Delete_Vertex_Shader((spike::ShaderHandle)handle);
	return D3D_OK;
}

HRESULT VulkanRenderBackendClass::SetVertexShader(DWORD handle)
{
	if (Internals->Backend == NULL) return D3DERR_INVALIDCALL;
	// D3D8 overloads this: an FVF bitfield selects the fixed-function pipeline, a handle from
	// CreateVertexShader selects a program.  The engine uses both (15 of its 23 call sites pass
	// an FVF), so an FVF here is the fixed-function path and needs no shader bound.  A typed
	// vertex buffer carries its own FVF; the FVF set here is the layout of an *untyped* one
	// (CreateVertexBuffer with FVF 0, the two shadow managers), read at draw time.
	if ((handle & D3DFVF_RESERVED0) == 0 && handle >= D3DFVF_XYZ) {
		Internals->Backend->Set_Vertex_Shader(spike::kNullShader);
		Internals->Backend->Set_Fixed_Function_Fvf((uint32_t)handle);
		return D3D_OK;
	}
	// Under a program the stream's layout is the program's D3DVSD_* declaration, which the
	// backend decoded at Create_Vertex_Shader and applies to untyped buffers at draw time.
	// No FVF is left bound so a program without a usable declaration is refused and counted
	// rather than read with a stale FVF.
	Internals->Backend->Set_Fixed_Function_Fvf(0);
	Internals->Backend->Set_Vertex_Shader((spike::ShaderHandle)handle);
	return D3D_OK;
}

HRESULT VulkanRenderBackendClass::SetVertexShaderConstant(DWORD reg, CONST void* constant_data,
	DWORD constant_count)
{
	if (Internals->Backend == NULL || constant_data == NULL) return D3DERR_INVALIDCALL;
	Internals->Backend->Set_Vertex_Shader_Constant((uint32_t)reg, constant_data,
		(uint32_t)constant_count);
	return D3D_OK;
}

HRESULT VulkanRenderBackendClass::CreatePixelShader(CONST DWORD* function, DWORD* handle)
{
	if (Internals->Backend == NULL || handle == NULL) return D3DERR_INVALIDCALL;
	*handle = 0;
	const spike::ShaderHandle created = Internals->Backend->Create_Pixel_Shader(
		(const uint32_t *)function);
	if (created == spike::kNullShader) {
		return Record_Unimplemented("IDirect3DDevice8::CreatePixelShader(untranslatable)",
			"the ps.1.1 token stream is not one the backend can translate", D3DERR_INVALIDCALL);
	}
	*handle = (DWORD)created;
	return D3D_OK;
}

HRESULT VulkanRenderBackendClass::DeletePixelShader(DWORD handle)
{
	if (Internals->Backend == NULL) return D3DERR_INVALIDCALL;
	Internals->Backend->Delete_Pixel_Shader((spike::ShaderHandle)handle);
	return D3D_OK;
}

HRESULT VulkanRenderBackendClass::SetPixelShader(DWORD handle)
{
	if (Internals->Backend == NULL) return D3DERR_INVALIDCALL;
	Internals->Backend->Set_Pixel_Shader((spike::ShaderHandle)handle);
	return D3D_OK;
}

HRESULT VulkanRenderBackendClass::SetPixelShaderConstant(DWORD reg, CONST void* constant_data,
	DWORD constant_count)
{
	if (Internals->Backend == NULL || constant_data == NULL) return D3DERR_INVALIDCALL;
	Internals->Backend->Set_Pixel_Shader_Constant((uint32_t)reg, constant_data,
		(uint32_t)constant_count);
	return D3D_OK;
}

// --- resource creation --------------------------------------------------------------------

HRESULT VulkanRenderBackendClass::CreateTexture(UINT width, UINT height, UINT levels,
	DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DTexture8** texture)
{
	if (Internals->Backend == NULL || texture == NULL) return D3DERR_INVALIDCALL;
	*texture = NULL;
	spike::TextureFormat translated;
	if (!Translate_Format(format, translated)) {
		return Record_Unimplemented("IDirect3DDevice8::CreateTexture(format)",
			"the D3DFORMAT has no spike::TextureFormat", D3DERR_INVALIDCALL);
	}
	if ((usage & D3DUSAGE_RENDERTARGET) != 0) {
		if (translated != spike::TextureFormat::A8R8G8B8
				&& translated != spike::TextureFormat::X8R8G8B8) {
			return Record_Unimplemented("IDirect3DDevice8::CreateTexture(render target format)",
				"the backend's render target textures are 8888 only", D3DERR_INVALIDCALL);
		}
		spike::TextureHandle * handle = Internals->Backend->Create_Render_Target_Texture(width,
			height);
		if (handle == NULL) return D3DERR_INVALIDCALL;
		*texture = new VulkanD3DTextureClass(Internals->Backend, handle, width, height, 1,
			translated, pool, usage);
		return D3D_OK;
	}
	if (!Internals->Backend->Supports_Texture_Format(translated)) {
		// A refusal, not a substitution: the caller asked whether this format works and the
		// device says no.  DX8Caps asks the same question through CheckDeviceFormat first, so
		// reaching here means the engine ignored that answer, which is worth the log line.
		return Record_Unimplemented("IDirect3DDevice8::CreateTexture(unsupported format)",
			"the Vulkan device cannot sample the format", D3DERR_INVALIDCALL);
	}
	// levels 0 means "the whole chain" in D3D8.
	unsigned level_count = levels;
	if (level_count == 0) {
		level_count = 1;
		unsigned w = width;
		unsigned h = height;
		while (w > 1 || h > 1) {
			w = (w > 1) ? (w >> 1) : 1;
			h = (h > 1) ? (h >> 1) : 1;
			level_count++;
		}
	}
	spike::TextureHandle * handle = Internals->Backend->Create_Lockable_Texture(width, height,
		translated, level_count);
	if (handle == NULL) return D3DERR_INVALIDCALL;
	*texture = new VulkanD3DTextureClass(Internals->Backend, handle, width, height, level_count,
		translated, pool, usage);
	return D3D_OK;
}

HRESULT VulkanRenderBackendClass::CreateCubeTexture(UINT edge_length, UINT levels, DWORD usage,
	D3DFORMAT format, D3DPOOL pool, IDirect3DCubeTexture8** cube_texture)
{
	(void)edge_length; (void)levels; (void)usage; (void)format; (void)pool;
	if (cube_texture != NULL) *cube_texture = NULL;
	// The spike has no cube image and no cube sampler, so there is nothing to hand back.  This
	// is reached only through D3DXCreateCubeTexture, whose engine callers ask DX8Caps first
	// (D3DPTEXTURECAPS_CUBEMAP is not set in the caps this backend reports), so an entry here
	// means something created a cube texture without asking - which is what makes it a finding.
	return Record_Unimplemented("IDirect3DDevice8::CreateCubeTexture",
		"the backend has no cube texture image or sampler", D3DERR_INVALIDCALL);
}

HRESULT VulkanRenderBackendClass::CreateVolumeTexture(UINT width, UINT height, UINT depth,
	UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
	IDirect3DVolumeTexture8** volume_texture)
{
	(void)width; (void)height; (void)depth; (void)levels; (void)usage; (void)format; (void)pool;
	if (volume_texture != NULL) *volume_texture = NULL;
	// As above for 3D images: D3DPTEXTURECAPS_VOLUMEMAP is absent from the reported caps.
	return Record_Unimplemented("IDirect3DDevice8::CreateVolumeTexture",
		"the backend has no 3D texture image or sampler", D3DERR_INVALIDCALL);
}

HRESULT VulkanRenderBackendClass::CreateVertexBuffer(UINT length, DWORD usage, DWORD fvf,
	D3DPOOL pool, IDirect3DVertexBuffer8** vertex_buffer)
{
	if (Internals->Backend == NULL || vertex_buffer == NULL) return D3DERR_INVALIDCALL;
	*vertex_buffer = NULL;
	const bool dynamic = (usage & D3DUSAGE_DYNAMIC) != 0;
	spike::VertexBufferHandle * handle = Internals->Backend->Create_Lockable_Vertex_Buffer(length,
		(uint32_t)fvf, dynamic);
	if (handle == NULL) {
		// Decode_Fvf refuses an FVF layout the backend has no vertex input state for, which is a
		// finding about that layout.
		return Record_Unimplemented("IDirect3DDevice8::CreateVertexBuffer(FVF)",
			"the FVF is not one spike::Decode_Fvf can decode", D3DERR_INVALIDCALL);
	}
	*vertex_buffer = new VulkanD3DVertexBufferClass(Internals->Backend, handle, length, usage, fvf,
		pool);
	return D3D_OK;
}

HRESULT VulkanRenderBackendClass::CreateIndexBuffer(UINT length, DWORD usage, D3DFORMAT format,
	D3DPOOL pool, IDirect3DIndexBuffer8** index_buffer)
{
	if (Internals->Backend == NULL || index_buffer == NULL) return D3DERR_INVALIDCALL;
	*index_buffer = NULL;
	if (format != D3DFMT_INDEX16) {
		return Record_Unimplemented("IDirect3DDevice8::CreateIndexBuffer(32-bit indices)",
			"the backend binds VK_INDEX_TYPE_UINT16 only", D3DERR_INVALIDCALL);
	}
	const bool dynamic = (usage & D3DUSAGE_DYNAMIC) != 0;
	spike::IndexBufferHandle * handle = Internals->Backend->Create_Lockable_Index_Buffer(
		length / sizeof(unsigned short), dynamic);
	if (handle == NULL) return D3DERR_INVALIDCALL;
	*index_buffer = new VulkanD3DIndexBufferClass(Internals->Backend, handle, length, usage,
		format, pool);
	return D3D_OK;
}

HRESULT VulkanRenderBackendClass::CreateImageSurface(UINT width, UINT height, D3DFORMAT format,
	IDirect3DSurface8** surface)
{
	if (Internals->Backend == NULL || surface == NULL) return D3DERR_INVALIDCALL;
	*surface = NULL;
	spike::TextureFormat translated;
	if (!Translate_Format(format, translated)) {
		return Record_Unimplemented("IDirect3DDevice8::CreateImageSurface(format)",
			"the D3DFORMAT has no spike::TextureFormat", D3DERR_INVALIDCALL);
	}
	spike::SurfaceHandle * handle = Internals->Backend->Create_Image_Surface(width, height,
		translated);
	if (handle == NULL) return D3DERR_INVALIDCALL;
	*surface = new VulkanD3DSurfaceClass(Internals->Backend, handle, NULL, width, height,
		translated, D3DPOOL_SYSTEMMEM, 0);
	return D3D_OK;
}

HRESULT VulkanRenderBackendClass::CreateAdditionalSwapChain(
	D3DPRESENT_PARAMETERS* present_parameters, IDirect3DSwapChain8** swap_chain)
{
	(void)present_parameters;
	if (swap_chain != NULL) *swap_chain = NULL;
	return Record_Unimplemented("IDirect3DDevice8::CreateAdditionalSwapChain",
		"the backend owns exactly one swapchain", D3DERR_INVALIDCALL);
}

HRESULT VulkanRenderBackendClass::UpdateTexture(IDirect3DBaseTexture8* source,
	IDirect3DBaseTexture8* destination)
{
	if (Internals->Backend == NULL || source == NULL || destination == NULL) {
		return D3DERR_INVALIDCALL;
	}
	spike::TextureHandle * from = ((VulkanD3DTextureClass *)source)->Peek_Handle();
	spike::TextureHandle * to = ((VulkanD3DTextureClass *)destination)->Peek_Handle();
	return Internals->Backend->Update_Texture(from, to) ? D3D_OK : D3DERR_INVALIDCALL;
}

HRESULT VulkanRenderBackendClass::CopyRects(IDirect3DSurface8* source_surface,
	CONST RECT* source_rects, UINT rect_count, IDirect3DSurface8* destination_surface,
	CONST POINT* dest_points)
{
	if (Internals->Backend == NULL || source_surface == NULL || destination_surface == NULL) {
		return D3DERR_INVALIDCALL;
	}
	std::vector<spike::LockRect> rects;
	std::vector<spike::SurfacePoint> points;
	for (UINT index = 0; index < rect_count && source_rects != NULL; index++) {
		spike::LockRect rect;
		rect.left = (unsigned)source_rects[index].left;
		rect.top = (unsigned)source_rects[index].top;
		rect.right = (unsigned)source_rects[index].right;
		rect.bottom = (unsigned)source_rects[index].bottom;
		rects.push_back(rect);
		spike::SurfacePoint point;
		if (dest_points != NULL) {
			point.x = (unsigned)dest_points[index].x;
			point.y = (unsigned)dest_points[index].y;
		}
		points.push_back(point);
	}
	if (((VulkanD3DSurfaceClass *)source_surface)->Is_Mip_Level_Surface() ||
			((VulkanD3DSurfaceClass *)destination_surface)->Is_Mip_Level_Surface()) {
		// A mip level above 0 has no image view of its own here, and Copy_Rects works between
		// surfaces.  Refusing says so; passing its null handle would have copied to or from the
		// wrong image.
		return Record_Unimplemented("IDirect3DDevice8::CopyRects(mip level above 0)",
			"a mip level surface has no image view, only a lockable level", D3DERR_INVALIDCALL);
	}
	if (Is_Compressed(((VulkanD3DSurfaceClass *)source_surface)->Peek_Format()) ||
			Is_Compressed(((VulkanD3DSurfaceClass *)destination_surface)->Peek_Format())) {
		// The spike's Copy_Rects is a texel copy (vkCmdCopyImage between 8888-class images); a
		// block-compressed endpoint would need block-aligned rects and a same-format pair, and
		// the engine's own compressed uploads go through LockRect, not through this.
		return Record_Unimplemented("IDirect3DDevice8::CopyRects(block-compressed surface)",
			"the backend's surface copy is texel-oriented; compressed levels upload via LockRect",
			D3DERR_INVALIDCALL);
	}
	spike::SurfaceHandle * from = ((VulkanD3DSurfaceClass *)source_surface)->Peek_Handle();
	spike::SurfaceHandle * to = ((VulkanD3DSurfaceClass *)destination_surface)->Peek_Handle();
	const bool copied = Internals->Backend->Copy_Rects(from,
		rects.empty() ? NULL : &rects[0], (uint32_t)rects.size(), to,
		points.empty() ? NULL : &points[0]);
	return copied ? D3D_OK : D3DERR_INVALIDCALL;
}

// --- submission ---------------------------------------------------------------------------

HRESULT VulkanRenderBackendClass::SetStreamSource(UINT stream_number,
	IDirect3DVertexBuffer8* stream_data, UINT stride)
{
	if (Internals->Backend == NULL) return D3DERR_INVALIDCALL;
	// A typed buffer's stride is implied by the FVF it was created with, which is how the
	// spike's vertex input state is built; the explicit stride only decides anything for an
	// untyped buffer, whose layout the backend resolves from the FVF bound at draw time.
	spike::VertexBufferHandle * handle = NULL;
	if (stream_data != NULL) {
		handle = ((VulkanD3DVertexBufferClass *)stream_data)->Peek_Handle();
	}
	Internals->Backend->Set_Vertex_Buffer(handle, (uint32_t)stream_number, (uint32_t)stride);
	return D3D_OK;
}

HRESULT VulkanRenderBackendClass::SetIndices(IDirect3DIndexBuffer8* index_data,
	UINT base_vertex_index)
{
	if (Internals->Backend == NULL) return D3DERR_INVALIDCALL;
	spike::IndexBufferHandle * handle = NULL;
	if (index_data != NULL) {
		handle = ((VulkanD3DIndexBufferClass *)index_data)->Peek_Handle();
	}
	Internals->Backend->Set_Index_Buffer(handle, (uint32_t)base_vertex_index);
	return D3D_OK;
}

HRESULT VulkanRenderBackendClass::DrawPrimitive(D3DPRIMITIVETYPE primitive_type,
	UINT start_vertex, UINT primitive_count)
{
	if (Internals->Backend == NULL) return D3DERR_INVALIDCALL;
	Internals->Backend->Draw_Primitive((uint32_t)primitive_type, (uint32_t)start_vertex,
		(uint32_t)primitive_count);
	return D3D_OK;
}

HRESULT VulkanRenderBackendClass::DrawIndexedPrimitive(D3DPRIMITIVETYPE primitive_type,
	UINT min_index, UINT num_vertices, UINT start_index, UINT primitive_count)
{
	if (Internals->Backend == NULL) return D3DERR_INVALIDCALL;
	Internals->Backend->Draw_Indexed_Primitive((uint32_t)primitive_type, (uint32_t)start_index,
		(uint32_t)primitive_count, (uint32_t)min_index, (uint32_t)num_vertices);
	return D3D_OK;
}

HRESULT VulkanRenderBackendClass::DrawPrimitiveUP(D3DPRIMITIVETYPE primitive_type,
	UINT primitive_count, CONST void* vertex_stream_zero_data,
	UINT vertex_stream_zero_stride)
{
	if (Internals->Backend == NULL || vertex_stream_zero_data == NULL) return D3DERR_INVALIDCALL;
	// D3D8 takes the layout from the currently set vertex shader/FVF, which DX8Wrapper set just
	// before this call; the spike wants it explicitly, and its shadowed render state is the only
	// place this layer can read it back from.  0 is what the backend treats as "the FVF I was
	// last given", so the FVF is not invented here.
	Internals->Backend->Draw_Primitive_UP((uint32_t)primitive_type, (uint32_t)primitive_count,
		vertex_stream_zero_data, (uint32_t)vertex_stream_zero_stride, 0);
	return D3D_OK;
}

HRESULT VulkanRenderBackendClass::ProcessVertices(UINT src_start_index, UINT dest_index,
	UINT vertex_count, IDirect3DVertexBuffer8* dest_buffer, DWORD flags)
{
	(void)src_start_index;
	(void)dest_index;
	(void)vertex_count;
	(void)dest_buffer;
	(void)flags;
	return Record_Unimplemented("IDirect3DDevice8::ProcessVertices",
		"the backend has no CPU/compute vertex transform path", D3DERR_INVALIDCALL);
}

// --- render targets -----------------------------------------------------------------------

HRESULT VulkanRenderBackendClass::GetRenderTarget(IDirect3DSurface8** render_target)
{
	if (Internals->Backend == NULL || render_target == NULL) return D3DERR_INVALIDCALL;
	*render_target = NULL;
	spike::SurfaceHandle * handle = Internals->Backend->Get_Render_Target();
	if (handle == NULL) return D3DERR_INVALIDCALL;
	if (Internals->DefaultColorSurface == NULL) {
		Internals->DefaultColorSurface = new VulkanD3DSurfaceClass(Internals->Backend, handle,
			NULL, Internals->PresentParameters.BackBufferWidth,
			Internals->PresentParameters.BackBufferHeight, spike::TextureFormat::A8R8G8B8,
			D3DPOOL_DEFAULT, D3DUSAGE_RENDERTARGET);
	}
	Internals->DefaultColorSurface->AddRef();
	*render_target = Internals->DefaultColorSurface;
	return D3D_OK;
}

HRESULT VulkanRenderBackendClass::GetDepthStencilSurface(
	IDirect3DSurface8** depth_stencil_surface)
{
	if (Internals->Backend == NULL || depth_stencil_surface == NULL) return D3DERR_INVALIDCALL;
	*depth_stencil_surface = NULL;
	spike::SurfaceHandle * handle = Internals->Backend->Get_Depth_Stencil_Target();
	if (handle == NULL) return D3DERR_NOTFOUND;
	if (Internals->DefaultDepthSurface == NULL) {
		Internals->DefaultDepthSurface = new VulkanD3DSurfaceClass(Internals->Backend, handle,
			NULL, Internals->PresentParameters.BackBufferWidth,
			Internals->PresentParameters.BackBufferHeight, spike::TextureFormat::A8R8G8B8,
			D3DPOOL_DEFAULT, D3DUSAGE_DEPTHSTENCIL);
	}
	Internals->DefaultDepthSurface->AddRef();
	*depth_stencil_surface = Internals->DefaultDepthSurface;
	return D3D_OK;
}

HRESULT VulkanRenderBackendClass::SetRenderTarget(IDirect3DSurface8* render_target,
	IDirect3DSurface8* new_z_stencil)
{
	if (Internals->Backend == NULL) return D3DERR_INVALIDCALL;
	spike::SurfaceHandle * color = NULL;
	spike::SurfaceHandle * depth = NULL;
	if ((render_target != NULL &&
				((VulkanD3DSurfaceClass *)render_target)->Is_Mip_Level_Surface()) ||
			(new_z_stencil != NULL &&
				((VulkanD3DSurfaceClass *)new_z_stencil)->Is_Mip_Level_Surface())) {
		// A null handle here means "the default target", so a mip level surface has to be refused
		// explicitly: rendering to the default target while the caller asked for a mip level is
		// precisely the silent wrong-image class this port keeps out.
		return Record_Unimplemented("IDirect3DDevice8::SetRenderTarget(mip level above 0)",
			"a mip level surface has no image view to render into", D3DERR_INVALIDCALL);
	}
	if (render_target != NULL) color = ((VulkanD3DSurfaceClass *)render_target)->Peek_Handle();
	if (new_z_stencil != NULL) depth = ((VulkanD3DSurfaceClass *)new_z_stencil)->Peek_Handle();
	return Internals->Backend->Set_Render_Target(color, depth) ? D3D_OK : D3DERR_INVALIDCALL;
}

HRESULT VulkanRenderBackendClass::GetFrontBuffer(IDirect3DSurface8* dest_surface)
{
	(void)dest_surface;
	return Record_Unimplemented("IDirect3DDevice8::GetFrontBuffer",
		"the swapchain's presented image cannot be read back", D3DERR_INVALIDCALL);
}

HRESULT VulkanRenderBackendClass::GetBackBuffer(UINT back_buffer, D3DBACKBUFFER_TYPE type,
	IDirect3DSurface8** surface)
{
	if (surface == NULL) return D3DERR_INVALIDCALL;
	*surface = NULL;
	if (back_buffer != 0 || type != D3DBACKBUFFER_TYPE_MONO) {
		return Record_Unimplemented("IDirect3DDevice8::GetBackBuffer(index or stereo)",
			"there is one colour target and it is not stereo", D3DERR_INVALIDCALL);
	}
	// The colour target the frame renders into *is* what gets presented, so this is the same
	// surface GetRenderTarget hands out rather than a swapchain image: the swapchain images are
	// only ever blit destinations and the engine cannot lock or bind them.
	return GetRenderTarget(surface);
}

// --- status, caps, memory -----------------------------------------------------------------

HRESULT VulkanRenderBackendClass::TestCooperativeLevel()
{
	// Vulkan has no lost-device state that maps onto this: a swapchain that goes out of date is
	// rebuilt inside Present.  So "the device is usable" is a true answer whenever there is one.
	return (Internals->Backend != NULL) ? D3D_OK : D3DERR_INVALIDCALL;
}

HRESULT VulkanRenderBackendClass::Reset(D3DPRESENT_PARAMETERS* present_parameters)
{
	if (Internals->Backend == NULL || present_parameters == NULL) return D3DERR_INVALIDCALL;
	// D3D8's Reset recreates every D3DPOOL_DEFAULT resource; the backend's colour target keeps
	// its resolution and only the swapchain is rebuilt, which is the cheap half.  A reset that
	// changes the back buffer size is therefore only partly served, and says so.
	const bool same_size =
		present_parameters->BackBufferWidth == Internals->PresentParameters.BackBufferWidth
		&& present_parameters->BackBufferHeight == Internals->PresentParameters.BackBufferHeight;
	if (!Internals->Backend->Resize_Presentation(present_parameters->BackBufferWidth,
			present_parameters->BackBufferHeight)) {
		return D3DERR_INVALIDCALL;
	}
	Internals->PresentParameters = *present_parameters;
	if (!same_size) {
		return Record_Unimplemented("IDirect3DDevice8::Reset(new back buffer size)",
			"the swapchain is rebuilt but the colour target keeps its resolution", D3D_OK);
	}
	return D3D_OK;
}

HRESULT VulkanRenderBackendClass::ValidateDevice(DWORD* num_passes)
{
	if (Internals->Backend == NULL || num_passes == NULL) return D3DERR_INVALIDCALL;
	// True of this backend rather than optimistic: the texture-stage cascade is executed by one
	// uber-shader, so any state combination it accepts at all is a single pass.  A combination
	// it does not accept fails at Set_DX8_Texture_Stage_State, not here.
	*num_passes = 1;
	return D3D_OK;
}

UINT VulkanRenderBackendClass::GetAvailableTextureMem()
{
	// D3D8 reports what is free; Vulkan reports heap sizes, and free-memory reporting needs
	// VK_EXT_memory_budget, which the backend does not enable.  The heap size is an upper bound
	// and is what the engine uses to pick texture detail, so it is reported as the measurement
	// it is - not as a free-memory figure.
	if (Internals->HaveDeviceAdapter) {
		const uint64_t bytes = Internals->DeviceAdapter.device_memory_bytes;
		return (UINT)((bytes > 0xffffffffull) ? 0xffffffffull : bytes);
	}
	return 0;
}

HRESULT VulkanRenderBackendClass::ResourceManagerDiscardBytes(DWORD bytes)
{
	(void)bytes;
	// D3D8 evicts managed-pool resources here.  The backend has no managed pool to evict from,
	// so this discarded nothing and must not claim otherwise.
	return Record_Unimplemented("IDirect3DDevice8::ResourceManagerDiscardBytes",
		"the backend has no managed pool to evict", D3DERR_INVALIDCALL);
}

/*
** Fill in a D3DCAPS8 from a measured adapter plus what this translation layer implements.
**
** Two kinds of field, kept apart deliberately:
**  - limits, which come from the Vulkan device (max texture size, anisotropy, memory);
**  - feature bits, which say what the *backend* does, not what Vulkan could do.  A bit is set
**    here only if the corresponding path exists in spikes/renderer; that is why, for example,
**    D3DPTEXTURECAPS_CUBEMAP is absent - Create_Texture has no cube form (a finding, listed in
**    docs/porting/renderer-integration.md).
*/
static void Fill_Caps(const spike::AdapterInfo & adapter, UINT adapter_ordinal,
	D3DDEVTYPE device_type, D3DCAPS8 * caps)
{
	memset(caps, 0, sizeof(*caps));
	caps->DeviceType = device_type;
	caps->AdapterOrdinal = adapter_ordinal;

	caps->Caps = 0;
	caps->Caps2 = D3DCAPS2_CANRENDERWINDOWED;
	caps->Caps3 = 0;
	caps->PresentationIntervals = D3DPRESENT_INTERVAL_ONE | D3DPRESENT_INTERVAL_IMMEDIATE;

	caps->DevCaps = D3DDEVCAPS_HWRASTERIZATION | D3DDEVCAPS_HWTRANSFORMANDLIGHT
		| D3DDEVCAPS_DRAWPRIMTLVERTEX | D3DDEVCAPS_DRAWPRIMITIVES2
		| D3DDEVCAPS_DRAWPRIMITIVES2EX | D3DDEVCAPS_TEXTUREVIDEOMEMORY
		| D3DDEVCAPS_TEXTURENONLOCALVIDMEM;

	caps->PrimitiveMiscCaps = D3DPMISCCAPS_CULLNONE | D3DPMISCCAPS_CULLCW | D3DPMISCCAPS_CULLCCW
		| D3DPMISCCAPS_COLORWRITEENABLE | D3DPMISCCAPS_MASKZ | D3DPMISCCAPS_BLENDOP;

	// D3DPRASTERCAPS_ZBIAS: Z_Bias_To_Depth_Bias_Constant_Factor exists and the pipeline uses
	// VK_DYNAMIC_STATE_DEPTH_BIAS, so this one is implemented rather than asserted.
	caps->RasterCaps = D3DPRASTERCAPS_ZTEST | D3DPRASTERCAPS_FOGVERTEX | D3DPRASTERCAPS_FOGTABLE
		| D3DPRASTERCAPS_MIPMAPLODBIAS | D3DPRASTERCAPS_ZBIAS;
	if (adapter.anisotropic_filtering) caps->RasterCaps |= D3DPRASTERCAPS_ANISOTROPY;

	caps->ZCmpCaps = D3DPCMPCAPS_NEVER | D3DPCMPCAPS_LESS | D3DPCMPCAPS_EQUAL
		| D3DPCMPCAPS_LESSEQUAL | D3DPCMPCAPS_GREATER | D3DPCMPCAPS_NOTEQUAL
		| D3DPCMPCAPS_GREATEREQUAL | D3DPCMPCAPS_ALWAYS;
	caps->AlphaCmpCaps = caps->ZCmpCaps;

	caps->SrcBlendCaps = D3DPBLENDCAPS_ZERO | D3DPBLENDCAPS_ONE | D3DPBLENDCAPS_SRCCOLOR
		| D3DPBLENDCAPS_INVSRCCOLOR | D3DPBLENDCAPS_SRCALPHA | D3DPBLENDCAPS_INVSRCALPHA
		| D3DPBLENDCAPS_DESTALPHA | D3DPBLENDCAPS_INVDESTALPHA | D3DPBLENDCAPS_DESTCOLOR
		| D3DPBLENDCAPS_INVDESTCOLOR | D3DPBLENDCAPS_SRCALPHASAT;
	caps->DestBlendCaps = caps->SrcBlendCaps;

	caps->ShadeCaps = D3DPSHADECAPS_COLORGOURAUDRGB | D3DPSHADECAPS_ALPHAGOURAUDBLEND
		| D3DPSHADECAPS_SPECULARGOURAUDRGB | D3DPSHADECAPS_FOGGOURAUD;

	// No cube or volume textures: the backend creates 2D images only.  Both are findings, not
	// omissions to be quietly filled in with a bit.
	caps->TextureCaps = D3DPTEXTURECAPS_PERSPECTIVE | D3DPTEXTURECAPS_ALPHA
		| D3DPTEXTURECAPS_PROJECTED | D3DPTEXTURECAPS_MIPMAP | D3DPTEXTURECAPS_ALPHAPALETTE;

	caps->TextureFilterCaps = D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR
		| D3DPTFILTERCAPS_MIPFPOINT | D3DPTFILTERCAPS_MIPFLINEAR | D3DPTFILTERCAPS_MAGFPOINT
		| D3DPTFILTERCAPS_MAGFLINEAR;
	if (adapter.anisotropic_filtering) {
		caps->TextureFilterCaps |= D3DPTFILTERCAPS_MINFANISOTROPIC
			| D3DPTFILTERCAPS_MAGFANISOTROPIC;
	}
	caps->CubeTextureFilterCaps = 0;
	caps->VolumeTextureFilterCaps = 0;

	caps->TextureAddressCaps = D3DPTADDRESSCAPS_WRAP | D3DPTADDRESSCAPS_MIRROR
		| D3DPTADDRESSCAPS_CLAMP | D3DPTADDRESSCAPS_BORDER | D3DPTADDRESSCAPS_INDEPENDENTUV
		| D3DPTADDRESSCAPS_MIRRORONCE;
	caps->VolumeTextureAddressCaps = 0;

	caps->LineCaps = D3DLINECAPS_TEXTURE | D3DLINECAPS_ZTEST | D3DLINECAPS_BLEND
		| D3DLINECAPS_ALPHACMP | D3DLINECAPS_FOG;

	caps->MaxTextureWidth = adapter.max_texture_dimension;
	caps->MaxTextureHeight = adapter.max_texture_dimension;
	caps->MaxVolumeExtent = 0;
	caps->MaxTextureRepeat = adapter.max_texture_dimension;
	caps->MaxTextureAspectRatio = adapter.max_texture_dimension;
	caps->MaxAnisotropy = (DWORD)adapter.max_anisotropy;
	caps->MaxVertexW = 1.0e10f;

	caps->GuardBandLeft = 0.0f;
	caps->GuardBandTop = 0.0f;
	caps->GuardBandRight = 0.0f;
	caps->GuardBandBottom = 0.0f;
	caps->ExtentsAdjust = 0.0f;

	// No stencil ops missing: the pipeline key carries all six D3D8 stencil operations.
	caps->StencilCaps = D3DSTENCILCAPS_KEEP | D3DSTENCILCAPS_ZERO | D3DSTENCILCAPS_REPLACE
		| D3DSTENCILCAPS_INCRSAT | D3DSTENCILCAPS_DECRSAT | D3DSTENCILCAPS_INVERT
		| D3DSTENCILCAPS_INCR | D3DSTENCILCAPS_DECR;

	caps->FVFCaps = D3DFVFCAPS_PSIZE | 8;

	// The cascade the uber-shader implements: 17 texture operations (measured, and the same 17
	// the engine uses - tools/texture-stage-scan.py).  Bump-map ops are not among them.
	caps->TextureOpCaps = D3DTEXOPCAPS_DISABLE | D3DTEXOPCAPS_SELECTARG1 | D3DTEXOPCAPS_SELECTARG2
		| D3DTEXOPCAPS_MODULATE | D3DTEXOPCAPS_MODULATE2X | D3DTEXOPCAPS_MODULATE4X
		| D3DTEXOPCAPS_ADD | D3DTEXOPCAPS_ADDSIGNED | D3DTEXOPCAPS_ADDSIGNED2X
		| D3DTEXOPCAPS_SUBTRACT | D3DTEXOPCAPS_ADDSMOOTH | D3DTEXOPCAPS_BLENDDIFFUSEALPHA
		| D3DTEXOPCAPS_BLENDTEXTUREALPHA | D3DTEXOPCAPS_BLENDFACTORALPHA
		| D3DTEXOPCAPS_BLENDCURRENTALPHA | D3DTEXOPCAPS_MODULATEALPHA_ADDCOLOR
		| D3DTEXOPCAPS_DOTPRODUCT3;

	caps->MaxTextureBlendStages = adapter.max_texture_stages;
	caps->MaxSimultaneousTextures = adapter.max_texture_stages;

	caps->VertexProcessingCaps = D3DVTXPCAPS_TEXGEN | D3DVTXPCAPS_MATERIALSOURCE7
		| D3DVTXPCAPS_DIRECTIONALLIGHTS | D3DVTXPCAPS_POSITIONALLIGHTS
		| D3DVTXPCAPS_LOCALVIEWER;
	caps->MaxActiveLights = 4;
	caps->MaxUserClipPlanes = 6;
	caps->MaxVertexBlendMatrices = 0;
	caps->MaxVertexBlendMatrixIndex = 0;

	caps->MaxPointSize = 1.0f;
	caps->MaxPrimitiveCount = adapter.max_primitive_count;
	caps->MaxVertexIndex = adapter.max_vertex_index;
	caps->MaxStreams = 1;
	caps->MaxStreamStride = 65535;

	// vs.1.1 and ps.1.1: the versions the backend's token-stream translation implements, which
	// is also every shader the game ships.
	caps->VertexShaderVersion = D3DVS_VERSION(1, 1);
	caps->MaxVertexShaderConst = 96;
	caps->PixelShaderVersion = D3DPS_VERSION(1, 1);
	caps->MaxPixelShaderValue = 1.0f;
}

HRESULT VulkanRenderBackendClass::GetDeviceCaps(D3DCAPS8* caps)
{
	if (Internals->Backend == NULL || caps == NULL) return D3DERR_INVALIDCALL;
	if (!Internals->HaveDeviceAdapter) return D3DERR_INVALIDCALL;
	Fill_Caps(Internals->DeviceAdapter, 0, D3DDEVTYPE_HAL, caps);
	return D3D_OK;
}

HRESULT VulkanRenderBackendClass::GetDeviceCaps(UINT adapter, D3DDEVTYPE device_type,
	D3DCAPS8* caps)
{
	if (caps == NULL) return D3DERR_INVALIDCALL;
	if (adapter >= Internals->Adapters.size()) return D3DERR_INVALIDCALL;
	if (device_type != D3DDEVTYPE_HAL) {
		// D3D8's reference rasteriser and software device.  There is no software Vulkan device
		// inside this backend; lavapipe is one, but it is an adapter, not a device type.
		return Record_Unimplemented("IDirect3D8::GetDeviceCaps(non-HAL)",
			"the backend has no reference or software device type", D3DERR_NOTAVAILABLE);
	}
	Fill_Caps(Internals->Adapters[adapter], adapter, device_type, caps);
	return D3D_OK;
}

HRESULT VulkanRenderBackendClass::GetDisplayMode(D3DDISPLAYMODE* mode)
{
	if (Internals->Backend == NULL || mode == NULL) return D3DERR_INVALIDCALL;
	mode->Width = Internals->PresentParameters.BackBufferWidth;
	mode->Height = Internals->PresentParameters.BackBufferHeight;
	mode->RefreshRate = 0;
	mode->Format = D3DFMT_X8R8G8B8;
	return D3D_OK;
}

// --- cursor and gamma ---------------------------------------------------------------------

BOOL VulkanRenderBackendClass::ShowCursor(BOOL show)
{
	(void)show;
	// The hardware cursor belongs to the window seam off Windows (WWPlatform::Window_Show_
	// Cursor), not to the renderer.  Answering FALSE says "the previous state was hidden and I
	// did nothing", which is true of this backend, and the ledger records that the engine asked.
	Record_Unimplemented("IDirect3DDevice8::ShowCursor",
		"the cursor is the window seam's, not the renderer's", D3DERR_INVALIDCALL);
	return FALSE;
}

HRESULT VulkanRenderBackendClass::SetCursorProperties(UINT x_hotspot, UINT y_hotspot,
	IDirect3DSurface8* cursor_bitmap)
{
	(void)x_hotspot;
	(void)y_hotspot;
	(void)cursor_bitmap;
	return Record_Unimplemented("IDirect3DDevice8::SetCursorProperties",
		"no D3D8 hardware cursor off Windows", D3DERR_INVALIDCALL);
}

void VulkanRenderBackendClass::SetCursorPosition(UINT x_screen_space, UINT y_screen_space,
	DWORD flags)
{
	(void)x_screen_space;
	(void)y_screen_space;
	(void)flags;
	Record_Unimplemented("IDirect3DDevice8::SetCursorPosition",
		"no D3D8 hardware cursor off Windows", D3DERR_INVALIDCALL);
}

void VulkanRenderBackendClass::SetGammaRamp(DWORD flags, CONST D3DGAMMARAMP* ramp)
{
	(void)flags;
	(void)ramp;
	// Vulkan has no gamma ramp; on macOS the equivalent is a CoreGraphics display transfer
	// function, which is the platform seam's business and not this backend's.
	Record_Unimplemented("IDirect3DDevice8::SetGammaRamp",
		"Vulkan exposes no display gamma ramp", D3DERR_INVALIDCALL);
}

// --- adapter enumeration and probes -------------------------------------------------------

UINT VulkanRenderBackendClass::GetAdapterCount()
{
	return (UINT)Internals->Adapters.size();
}

HRESULT VulkanRenderBackendClass::GetAdapterIdentifier(UINT adapter, DWORD flags,
	D3DADAPTER_IDENTIFIER8* identifier)
{
	if (identifier == NULL) return D3DERR_INVALIDCALL;
	if (adapter >= Internals->Adapters.size()) return D3DERR_INVALIDCALL;
	(void)flags;
	const spike::AdapterInfo & info = Internals->Adapters[adapter];
	memset(identifier, 0, sizeof(*identifier));
	strncpy(identifier->Driver, info.driver.c_str(), sizeof(identifier->Driver) - 1);
	strncpy(identifier->Description, info.name.c_str(), sizeof(identifier->Description) - 1);
	identifier->VendorId = info.vendor_id;
	identifier->DeviceId = info.device_id;
	identifier->SubSysId = 0;
	identifier->Revision = 0;
	identifier->WHQLLevel = 0;
	// VkPhysicalDeviceProperties::driverVersion is the driver's own encoding, which is what
	// D3D8's DriverVersion is too, so it is reported unchanged rather than mapped onto the
	// Windows driver-version convention DX8Caps's vendor hacks expect.
	identifier->DriverVersionLowPart = (DWORD)info.driver_version;
	identifier->DriverVersionHighPart = (DWORD)info.api_version;
	return D3D_OK;
}

UINT VulkanRenderBackendClass::GetAdapterModeCount(UINT adapter)
{
	if (adapter >= Internals->Adapters.size()) return 0;
	// One mode: the display's current one.  Vulkan cannot enumerate video modes at all (that is
	// the platform's job and, on macOS, CoreGraphics's), so inventing a list of resolutions the
	// display might support would be exactly the kind of guess this port does not make.  The
	// engine's resolution list therefore has one entry off Windows, which is a finding.
	return 1;
}

HRESULT VulkanRenderBackendClass::EnumAdapterModes(UINT adapter, UINT mode,
	D3DDISPLAYMODE* display_mode)
{
	if (display_mode == NULL) return D3DERR_INVALIDCALL;
	if (adapter >= Internals->Adapters.size() || mode != 0) return D3DERR_INVALIDCALL;
	return GetAdapterDisplayMode(adapter, display_mode);
}

HRESULT VulkanRenderBackendClass::GetAdapterDisplayMode(UINT adapter,
	D3DDISPLAYMODE* display_mode)
{
	if (display_mode == NULL) return D3DERR_INVALIDCALL;
	if (adapter >= Internals->Adapters.size()) return D3DERR_INVALIDCALL;
	int x = 0;
	int y = 0;
	int width = 0;
	int height = 0;
	if (!WWPlatform::Window_Display_Bounds(0, x, y, width, height) || width <= 0 || height <= 0) {
		return Record_Unimplemented("IDirect3D8::GetAdapterDisplayMode",
			"the platform reported no display bounds", D3DERR_INVALIDCALL);
	}
	// Window_Display_Bounds is in points, as the whole window seam is; a display mode is a
	// pixel quantity.  This is the renderer boundary, so the points are what the engine's mode
	// list wants for placing a window, and the backing scale is applied to the drawable by the
	// swapchain, not here (docs/porting/decisions-resolved.md).
	display_mode->Width = (UINT)width;
	display_mode->Height = (UINT)height;
	display_mode->RefreshRate = 0;
	display_mode->Format = D3DFMT_X8R8G8B8;
	return D3D_OK;
}

HRESULT VulkanRenderBackendClass::CheckDeviceType(UINT adapter, D3DDEVTYPE check_type,
	D3DFORMAT display_format, D3DFORMAT backbuffer_format, BOOL windowed)
{
	if (adapter >= Internals->Adapters.size()) return D3DERR_INVALIDCALL;
	if (check_type != D3DDEVTYPE_HAL) return D3DERR_NOTAVAILABLE;
	(void)windowed;
	// The swapchain's format is the surface's, chosen by the backend; what the engine is really
	// asking is whether it can present 32-bit colour, which it can.  16-bit display formats are
	// refused rather than accepted and then quietly presented as 32-bit.
	if (display_format != D3DFMT_X8R8G8B8 && display_format != D3DFMT_A8R8G8B8) {
		return D3DERR_NOTAVAILABLE;
	}
	if (backbuffer_format != D3DFMT_X8R8G8B8 && backbuffer_format != D3DFMT_A8R8G8B8) {
		return D3DERR_NOTAVAILABLE;
	}
	return D3D_OK;
}

HRESULT VulkanRenderBackendClass::CheckDeviceFormat(UINT adapter, D3DDEVTYPE device_type,
	D3DFORMAT adapter_format, DWORD usage, D3DRESOURCETYPE resource_type, D3DFORMAT check_format)
{
	if (adapter >= Internals->Adapters.size()) return D3DERR_INVALIDCALL;
	if (device_type != D3DDEVTYPE_HAL) return D3DERR_NOTAVAILABLE;
	(void)adapter_format;
	if (resource_type == D3DRTYPE_CUBETEXTURE || resource_type == D3DRTYPE_VOLUMETEXTURE) {
		// Answering "no" here is what stops the engine from calling CreateCubeTexture, which
		// this backend has no entry point for at all.
		return D3DERR_NOTAVAILABLE;
	}
	spike::TextureFormat translated;
	if (!Translate_Format(check_format, translated)) {
		// Depth/stencil formats reach here too (D3DFMT_D24S8 and friends).  The backend picks
		// its own depth format, so the engine's choice cannot be honoured and must not be
		// claimed: only the one it uses is accepted.
		if ((usage & D3DUSAGE_DEPTHSTENCIL) != 0) {
			return (check_format == D3DFMT_D24S8) ? D3D_OK : D3DERR_NOTAVAILABLE;
		}
		return D3DERR_NOTAVAILABLE;
	}
	if ((usage & D3DUSAGE_RENDERTARGET) != 0) {
		return (translated == spike::TextureFormat::A8R8G8B8
			|| translated == spike::TextureFormat::X8R8G8B8) ? D3D_OK : D3DERR_NOTAVAILABLE;
	}
	// Measured once, at enumeration, per format per device: the bit is set iff the Vulkan device
	// reports VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT for the VkFormat this format maps onto.
	const uint32_t bit = 1u << (int)translated;
	return ((Internals->Adapters[adapter].sampled_formats & bit) != 0) ? D3D_OK
		: D3DERR_NOTAVAILABLE;
}

HRESULT VulkanRenderBackendClass::CheckDeviceMultiSampleType(UINT adapter,
	D3DDEVTYPE device_type, D3DFORMAT surface_format, BOOL windowed,
	D3DMULTISAMPLE_TYPE multi_sample_type)
{
	if (adapter >= Internals->Adapters.size()) return D3DERR_INVALIDCALL;
	(void)device_type;
	(void)surface_format;
	(void)windowed;
	// The backend creates 1-sample attachments only.  D3DMULTISAMPLE_NONE is therefore the only
	// truthful yes.
	return (multi_sample_type == D3DMULTISAMPLE_NONE) ? D3D_OK : D3DERR_NOTAVAILABLE;
}

HRESULT VulkanRenderBackendClass::CheckDepthStencilMatch(UINT adapter, D3DDEVTYPE device_type,
	D3DFORMAT adapter_format, D3DFORMAT render_target_format, D3DFORMAT depth_stencil_format)
{
	if (adapter >= Internals->Adapters.size()) return D3DERR_INVALIDCALL;
	(void)device_type;
	(void)adapter_format;
	if (render_target_format != D3DFMT_X8R8G8B8 && render_target_format != D3DFMT_A8R8G8B8) {
		return D3DERR_NOTAVAILABLE;
	}
	return (depth_stencil_format == D3DFMT_D24S8) ? D3D_OK : D3DERR_NOTAVAILABLE;
}

// --- the ledger ---------------------------------------------------------------------------

unsigned VulkanRenderBackendClass::Unimplemented_Call_Kinds()
{
	return UnimplementedKindCount;
}

void VulkanRenderBackendClass::Record_Unserviceable(const char * name, const char * why)
{
	Record_Unimplemented(name, why, D3DERR_NOTAVAILABLE);
}

const VulkanRenderBackendClass::UnimplementedCallClass * VulkanRenderBackendClass::
	Unimplemented_Call(unsigned index)
{
	if (index >= UnimplementedKindCount) return NULL;
	return &UnimplementedCalls[index];
}

void VulkanRenderBackendClass::Log_Unimplemented_Calls()
{
	if (UnimplementedKindCount == 0) {
		WWDEBUG_SAY(("VulkanRenderBackend: no unimplemented D3D8 entry point was reached\n"));
		return;
	}
	WWDEBUG_SAY(("VulkanRenderBackend: %u unimplemented D3D8 entry point(s) were reached\n",
		UnimplementedKindCount));
	for (unsigned index = 0; index < UnimplementedKindCount; index++) {
		WWDEBUG_SAY(("  %s x%u (%s)\n", UnimplementedCalls[index].Name,
			UnimplementedCalls[index].Count, UnimplementedCalls[index].Why));
	}
}

long VulkanRenderBackendClass::Validation_Message_Count() const
{
	if (Internals->Backend == NULL) return -1;
	return (long)Internals->Backend->Validation_Message_Count();
}

bool VulkanRenderBackendClass::Measure_Frame(unsigned char expect_r, unsigned char expect_g,
	unsigned char expect_b, unsigned char tolerance, const char * png_path,
	FrameProofClass & proof)
{
	memset(&proof, 0, sizeof(proof));
	if (Internals->Backend == NULL) return false;

	std::string rgba;
	spike::SurfaceFormat format;
	if (!Internals->Backend->Read_Back_Color_Target(rgba, format)) return false;
	const unsigned long pixels = (unsigned long)format.width * (unsigned long)format.height;
	if (pixels == 0 || rgba.size() < pixels * 4) return false;

	proof.Width = format.width;
	proof.Height = format.height;
	proof.Pixels = pixels;
	const unsigned char * bits = (const unsigned char *)rgba.data();
	proof.MinRGB[0] = proof.MinRGB[1] = proof.MinRGB[2] = 255;
	for (unsigned long index = 0; index < pixels; index++) {
		const unsigned char * texel = bits + index * 4;
		bool matches = true;
		const unsigned char expected[3] = { expect_r, expect_g, expect_b };
		for (unsigned channel = 0; channel < 3; channel++) {
			const unsigned char value = texel[channel];
			if (value < proof.MinRGB[channel]) proof.MinRGB[channel] = value;
			if (value > proof.MaxRGB[channel]) proof.MaxRGB[channel] = value;
			const int delta = int(value) - int(expected[channel]);
			if (delta > int(tolerance) || -delta > int(tolerance)) matches = false;
		}
		if (matches) proof.Matching++;
	}
	const unsigned long centre = ((unsigned long)(format.height / 2) * format.width +
		format.width / 2) * 4;
	memcpy(proof.CentreRGBA, bits + centre, 4);

	if (png_path != NULL && !spike::Write_Png(std::string(png_path), rgba, format.width,
			format.height)) {
		return false;
	}
	return true;
}

#endif // !_WIN32
