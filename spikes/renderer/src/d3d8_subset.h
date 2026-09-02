// Renderer spike: the subset of the Direct3D 8 type vocabulary that the Zero Hour
// engine actually pushes through DX8Wrapper.
//
// These are redeclared here (rather than including d3d8.h) so the spike builds on
// Linux and macOS with no Windows SDK. The values match d3d8types.h, because the
// engine stores raw D3D8 enum values in its own state caches
// (DX8Wrapper::RenderStates[256], TextureStageStates[8][32]) and a real backend has
// to keep accepting them.

#pragma once

#include <cstdint>

namespace spike {

using D3DCOLOR = uint32_t;

enum D3DTRANSFORMSTATETYPE {
	D3DTS_VIEW = 2,
	D3DTS_PROJECTION = 3,
	D3DTS_TEXTURE0 = 16,
	D3DTS_TEXTURE1 = 17,
	D3DTS_TEXTURE2 = 18,
	D3DTS_TEXTURE3 = 19,
	D3DTS_WORLD = 256,
};

// Number of texture stages the spike models. Measured, not guessed: literal
// D3DTSS_* call sites in Core/ reach stage 7 (Core/.../TerrainTex.cpp programs
// stages 0..7 for the 8-way terrain blend). See tools/texture-stage-scan.py.
constexpr uint32_t kMaxTextureStages = 8;
// D3D8 exposes 8 texture coordinate sets in the FVF; the engine's widest declared
// layout is DX8_FVF_XYZNDUV1TG3 with 4 sets (dx8fvf.h).
constexpr uint32_t kMaxTexCoordSets = 4;
// D3D8's fixed-function pipeline supports 8 simultaneous lights; the engine's
// LightEnvironmentClass caps itself at 4 (LightEnvironmentClass::MAX_LIGHTS).
constexpr uint32_t kMaxLights = 4;
// D3D8 exposes 6 user clip planes; DX8Wrapper::Set_DX8_Clip_Plane's only caller
// (the water reflection plane) asks for plane 0 and is commented out.
constexpr uint32_t kMaxClipPlanes = 6;

// --- ps.1.1 / vs.1.1 ---------------------------------------------------------
// Instruction slots per shader. The 16 shaders the engine ships are 158 lines of
// assembly in total; the longest (Trees.nvv) is 11 instructions.
constexpr uint32_t kMaxShaderInstructions = 32;
// ps.1.1 has c0..c7; vs.1.1 has c0..c95 and the engine's Trees.nvv reaches c33.
constexpr uint32_t kMaxPixelShaderConstants = 8;
constexpr uint32_t kMaxVertexShaderConstants = 96;
// v0..v15, four per ivec4 in the uniform block.
constexpr uint32_t kMaxVertexShaderInputs = 16;

// The D3D8 shader token bit fields, spelled as d3d8types.h defines them. The
// uber-shader decodes the same fields, so these values are shared with
// shaders/fixedfunc.vert and shaders/fixedfunc.frag by name.
constexpr uint32_t kD3DSI_OpcodeMask = 0x0000ffffu;
constexpr uint32_t kD3DSI_CommentSizeShift = 16;
constexpr uint32_t kD3DSI_CommentSizeMask = 0x7fff0000u;
constexpr uint32_t kD3DSP_RegnumMask = 0x00001fffu;
constexpr uint32_t kD3DSP_WritemaskShift = 16;
constexpr uint32_t kD3DSP_DstmodShift = 20;
constexpr uint32_t kD3DSP_DstshiftShift = 24;
constexpr uint32_t kD3DSP_RegtypeShift = 28;
constexpr uint32_t kD3DSP_SwizzleShift = 16;
constexpr uint32_t kD3DSP_SrcmodShift = 24;
// D3DVS_ADDRMODE_RELATIVE: `c[a0.x + n]`. In vs.1.1 the address register is
// implicitly a0.x and no extra address token follows (that arrived in vs.2.0).
constexpr uint32_t kD3DVS_AddrmodeRelative = 1u << 13;

// D3DSHADER_PARAM_REGISTER_TYPE, after the >> kD3DSP_RegtypeShift.
enum D3DShaderRegisterType {
	kRegTemp = 0,
	kRegInput = 1,
	kRegConst = 2,
	kRegAddrOrTexture = 3, // a0 in a vertex shader, t0..t3 in a pixel shader
	kRegRastOut = 4,       // oPos
	kRegAttrOut = 5,       // oD0, oD1
	kRegTexCrdOut = 6,     // oT0..oT3
};

// D3DSHADER_INSTRUCTION_OPCODE_TYPE, the subset the 16 shaders the engine ships
// actually use plus the ones that cost one line each. Anything else makes
// Create_Pixel_Shader/Create_Vertex_Shader fail rather than render something wrong.
enum D3DShaderOpcode {
	kSioNop = 0,
	kSioMov = 1,
	kSioAdd = 2,
	kSioSub = 3,
	kSioMad = 4,
	kSioMul = 5,
	kSioRcp = 6,
	kSioRsq = 7,
	kSioDp3 = 8,
	kSioDp4 = 9,
	kSioMin = 10,
	kSioMax = 11,
	kSioSlt = 12,
	kSioSge = 13,
	kSioExp = 14,
	kSioLog = 15,
	kSioLit = 16,
	kSioDst = 17,
	kSioLrp = 18,
	kSioFrc = 19,
	kSioM4x4 = 20,
	kSioM4x3 = 21,
	kSioM3x4 = 22,
	kSioM3x3 = 23,
	kSioM3x2 = 24,
	kSioTexCoord = 64,
	kSioTexKill = 65,
	kSioTex = 66,
	kSioTexBem = 67,
	kSioTexBemL = 68,
	kSioExpp = 78,
	kSioLogp = 79,
	kSioCnd = 80,
	kSioDef = 81,
	kSioComment = 0xfffe,
	kSioEnd = 0xffff,
};

// D3DVSD_* declaration tokens (IDirect3DDevice8::CreateVertexShader's pDeclaration).
constexpr uint32_t kD3DVSD_TokenTypeShift = 29;
constexpr uint32_t kD3DVSD_TokenNop = 0;
constexpr uint32_t kD3DVSD_TokenStream = 1;
constexpr uint32_t kD3DVSD_TokenStreamData = 2;
constexpr uint32_t kD3DVSD_TokenEnd = 7;
constexpr uint32_t kD3DVSD_VertexRegMask = 0x0000001fu;
constexpr uint32_t kD3DVSD_StreamNumberMask = 0x0000000fu;
constexpr uint32_t kD3DVSD_StreamTessMask = 0x10000000u;
// D3DVSD_REG's data type, D3DVSDT_*, in bits 16..19; D3DVSD_SKIP sets bit 28 instead
// and carries a DWORD count in the same bits.
constexpr uint32_t kD3DVSD_DataTypeShift = 16;
constexpr uint32_t kD3DVSD_DataTypeMask = 0x000f0000u;
constexpr uint32_t kD3DVSD_DataLoadMask = 0x10000000u;
constexpr uint32_t kD3DVSD_SkipCountMask = 0x000f0000u;
constexpr uint32_t kD3DVSDT_Float1 = 0;
constexpr uint32_t kD3DVSDT_Float2 = 1;
constexpr uint32_t kD3DVSDT_Float3 = 2;
constexpr uint32_t kD3DVSDT_Float4 = 3;
constexpr uint32_t kD3DVSDT_D3dColor = 4;
constexpr uint32_t kD3DVSDT_Ubyte4 = 5;
constexpr uint32_t kD3DVSDT_Short2 = 6;
constexpr uint32_t kD3DVSDT_Short4 = 7;
constexpr uint32_t kD3DVSD_End = 0xffffffffu;

// Only the render states the engine actually sets (53 of them; see
// docs/porting/renderer-surface.md). The spike implements the ones needed to draw.
enum D3DRENDERSTATETYPE {
	D3DRS_ZENABLE = 7,
	D3DRS_FILLMODE = 8,
	D3DRS_SHADEMODE = 9,
	D3DRS_ZWRITEENABLE = 14,
	D3DRS_ALPHATESTENABLE = 15,
	D3DRS_SRCBLEND = 19,
	D3DRS_DESTBLEND = 20,
	D3DRS_CULLMODE = 22,
	D3DRS_ZFUNC = 23,
	D3DRS_ALPHAREF = 24,
	D3DRS_ALPHAFUNC = 25,
	D3DRS_DITHERENABLE = 26,
	D3DRS_ALPHABLENDENABLE = 27,
	D3DRS_FOGENABLE = 28,
	D3DRS_SPECULARENABLE = 29,
	D3DRS_FOGCOLOR = 34,
	D3DRS_FOGTABLEMODE = 35,
	D3DRS_FOGSTART = 36,
	D3DRS_FOGEND = 37,
	D3DRS_FOGDENSITY = 38,
	D3DRS_ZBIAS = 47,
	D3DRS_RANGEFOGENABLE = 48,
	D3DRS_STENCILENABLE = 52,
	D3DRS_STENCILFAIL = 53,
	D3DRS_STENCILZFAIL = 54,
	D3DRS_STENCILPASS = 55,
	D3DRS_STENCILFUNC = 56,
	D3DRS_STENCILREF = 57,
	D3DRS_STENCILMASK = 58,
	D3DRS_STENCILWRITEMASK = 59,
	D3DRS_TEXTUREFACTOR = 60,
	D3DRS_CLIPPING = 136,
	D3DRS_LIGHTING = 137,
	D3DRS_AMBIENT = 139,
	D3DRS_FOGVERTEXMODE = 140,
	D3DRS_COLORVERTEX = 141,
	D3DRS_LOCALVIEWER = 142,
	D3DRS_NORMALIZENORMALS = 143,
	D3DRS_DIFFUSEMATERIALSOURCE = 145,
	D3DRS_SPECULARMATERIALSOURCE = 146,
	D3DRS_AMBIENTMATERIALSOURCE = 147,
	D3DRS_EMISSIVEMATERIALSOURCE = 148,
	// The bitmask that selects which SetClipPlane planes are applied. The engine
	// never sets it in the live path (its two call sites in W3DWater.cpp are
	// commented out), so it is here for Set_Clip_Plane's tests, not for demand.
	D3DRS_CLIPPLANEENABLE = 152,
	D3DRS_SOFTWAREVERTEXPROCESSING = 153,
	// Point sprites: W3DSnow draws every snow particle as one D3DPT_POINTLIST vertex
	// and lets D3D8 expand it, so these eight states are the whole snow system.
	D3DRS_POINTSIZE = 154,
	D3DRS_POINTSIZE_MIN = 155,
	D3DRS_POINTSPRITEENABLE = 156,
	D3DRS_POINTSCALEENABLE = 157,
	D3DRS_POINTSCALE_A = 158,
	D3DRS_POINTSCALE_B = 159,
	D3DRS_POINTSCALE_C = 160,
	D3DRS_PATCHSEGMENTS = 164,
	D3DRS_POINTSIZE_MAX = 166,
	D3DRS_COLORWRITEENABLE = 168,
	D3DRS_BLENDOP = 171,
	D3DRS_MAX = 256,
};

enum D3DTEXTURESTAGESTATETYPE {
	D3DTSS_COLOROP = 1,
	D3DTSS_COLORARG1 = 2,
	D3DTSS_COLORARG2 = 3,
	D3DTSS_ALPHAOP = 4,
	D3DTSS_ALPHAARG1 = 5,
	D3DTSS_ALPHAARG2 = 6,
	D3DTSS_BUMPENVMAT00 = 7,
	D3DTSS_BUMPENVMAT01 = 8,
	D3DTSS_BUMPENVMAT10 = 9,
	D3DTSS_BUMPENVMAT11 = 10,
	D3DTSS_TEXCOORDINDEX = 11,
	D3DTSS_ADDRESSU = 13,
	D3DTSS_ADDRESSV = 14,
	D3DTSS_BORDERCOLOR = 15,
	D3DTSS_MAGFILTER = 16,
	D3DTSS_MINFILTER = 17,
	D3DTSS_MIPFILTER = 18,
	D3DTSS_MAXANISOTROPY = 21,
	D3DTSS_BUMPENVLSCALE = 22,
	D3DTSS_BUMPENVLOFFSET = 23,
	D3DTSS_TEXTURETRANSFORMFLAGS = 24,
	D3DTSS_ADDRESSW = 25,
	D3DTSS_COLORARG0 = 26,
	D3DTSS_ALPHAARG0 = 27,
	D3DTSS_RESULTARG = 28,
	D3DTSS_MAX = 32,
};

// D3DTSS_TEXTURETRANSFORMFLAGS. The engine writes D3DTTFF_DISABLE, D3DTTFF_COUNT2
// and 259 == D3DTTFF_COUNT3|D3DTTFF_PROJECTED (Core/.../TerrainTex.cpp,
// Core/.../W3DShaderManager.cpp).
enum D3DTEXTURETRANSFORMFLAGS {
	D3DTTFF_DISABLE = 0,
	D3DTTFF_COUNT1 = 1,
	D3DTTFF_COUNT2 = 2,
	D3DTTFF_COUNT3 = 3,
	D3DTTFF_COUNT4 = 4,
	D3DTTFF_PROJECTED = 256,
};

// D3DTSS_TEXCOORDINDEX. The low 16 bits select an FVF texture coordinate set; the
// high bits select a generated coordinate instead. The engine uses all four
// generators (texproject.cpp, TerrainTex.cpp, W3DShaderManager.cpp).
enum D3DTEXCOORDINDEXFLAGS {
	D3DTSS_TCI_PASSTHRU = 0x00000,
	D3DTSS_TCI_CAMERASPACENORMAL = 0x10000,
	D3DTSS_TCI_CAMERASPACEPOSITION = 0x20000,
	D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR = 0x30000,
	D3DTSS_TCI_SELECTMASK = 0x0ffff,
	D3DTSS_TCI_GENERATORMASK = 0x30000,
};

enum D3DTEXTUREOP {
	D3DTOP_DISABLE = 1,
	D3DTOP_SELECTARG1 = 2,
	D3DTOP_SELECTARG2 = 3,
	D3DTOP_MODULATE = 4,
	D3DTOP_MODULATE2X = 5,
	D3DTOP_MODULATE4X = 6,
	D3DTOP_ADD = 7,
	D3DTOP_ADDSIGNED = 8,
	D3DTOP_ADDSIGNED2X = 9,
	D3DTOP_SUBTRACT = 10,
	D3DTOP_ADDSMOOTH = 11,
	D3DTOP_BLENDDIFFUSEALPHA = 12,
	D3DTOP_BLENDTEXTUREALPHA = 13,
	D3DTOP_BLENDFACTORALPHA = 14,
	D3DTOP_BLENDTEXTUREALPHAPM = 15,
	D3DTOP_BLENDCURRENTALPHA = 16,
	D3DTOP_PREMODULATE = 17,
	D3DTOP_MODULATEALPHA_ADDCOLOR = 18,
	D3DTOP_MODULATECOLOR_ADDALPHA = 19,
	D3DTOP_MODULATEINVALPHA_ADDCOLOR = 20,
	D3DTOP_MODULATEINVCOLOR_ADDALPHA = 21,
	D3DTOP_BUMPENVMAP = 22,
	D3DTOP_BUMPENVMAPLUMINANCE = 23,
	D3DTOP_DOTPRODUCT3 = 24,
	D3DTOP_MULTIPLYADD = 25,
	D3DTOP_LERP = 26,
};

enum D3DTEXTUREARG {
	D3DTA_DIFFUSE = 0,
	D3DTA_CURRENT = 1,
	D3DTA_TEXTURE = 2,
	D3DTA_TFACTOR = 3,
	D3DTA_SPECULAR = 4,
	D3DTA_TEMP = 5,
	D3DTA_COMPLEMENT = 0x10,
	D3DTA_ALPHAREPLICATE = 0x20,
	D3DTA_SELECTMASK = 0x0f,
};

// D3DRS_*MATERIALSOURCE. dx8wrapper.cpp and vertmaterial.cpp set all four.
enum D3DMATERIALCOLORSOURCE {
	D3DMCS_MATERIAL = 0,
	D3DMCS_COLOR1 = 1, // vertex diffuse
	D3DMCS_COLOR2 = 2, // vertex specular
};

// D3DLIGHTTYPE. DX8Wrapper::Set_Light maps LightClass::POINT/DIRECTIONAL/SPOT
// onto all three.
enum D3DLIGHTTYPE {
	D3DLIGHT_POINT = 1,
	D3DLIGHT_SPOT = 2,
	D3DLIGHT_DIRECTIONAL = 3,
};

// D3DRS_FOGVERTEXMODE / D3DRS_FOGTABLEMODE. dx8wrapper.cpp:402 sets table mode to
// NONE and vertex mode to LINEAR at device init; nothing changes them afterwards.
enum D3DFOGMODE {
	D3DFOG_NONE = 0,
	D3DFOG_EXP = 1,
	D3DFOG_EXP2 = 2,
	D3DFOG_LINEAR = 3,
};

enum D3DSTENCILOP {
	D3DSTENCILOP_KEEP = 1,
	D3DSTENCILOP_ZERO = 2,
	D3DSTENCILOP_REPLACE = 3,
	D3DSTENCILOP_INCRSAT = 4,
	D3DSTENCILOP_DECRSAT = 5,
	D3DSTENCILOP_INVERT = 6,
	D3DSTENCILOP_INCR = 7,
	D3DSTENCILOP_DECR = 8,
};

enum D3DBLEND {
	D3DBLEND_ZERO = 1,
	D3DBLEND_ONE = 2,
	D3DBLEND_SRCCOLOR = 3,
	D3DBLEND_INVSRCCOLOR = 4,
	D3DBLEND_SRCALPHA = 5,
	D3DBLEND_INVSRCALPHA = 6,
	D3DBLEND_DESTALPHA = 7,
	D3DBLEND_INVDESTALPHA = 8,
	D3DBLEND_DESTCOLOR = 9,
	D3DBLEND_INVDESTCOLOR = 10,
	D3DBLEND_SRCALPHASAT = 11,
};

// D3DRS_BLENDOP. dx8wrapper.cpp sets ADD at device init and W3DStatusCircle flips to
// REVSUBTRACT and back, which is the engine's whole use of it.
enum D3DBLENDOP {
	D3DBLENDOP_ADD = 1,
	D3DBLENDOP_SUBTRACT = 2,
	D3DBLENDOP_REVSUBTRACT = 3,
	D3DBLENDOP_MIN = 4,
	D3DBLENDOP_MAX = 5,
};

enum D3DCMPFUNC {
	D3DCMP_NEVER = 1,
	D3DCMP_LESS = 2,
	D3DCMP_EQUAL = 3,
	D3DCMP_LESSEQUAL = 4,
	D3DCMP_GREATER = 5,
	D3DCMP_NOTEQUAL = 6,
	D3DCMP_GREATEREQUAL = 7,
	D3DCMP_ALWAYS = 8,
};

enum D3DCULL {
	D3DCULL_NONE = 1,
	D3DCULL_CW = 2,
	D3DCULL_CCW = 3,
};

enum D3DFILLMODE {
	D3DFILL_POINT = 1,
	D3DFILL_WIREFRAME = 2,
	D3DFILL_SOLID = 3,
};

enum D3DTEXTUREADDRESS {
	D3DTADDRESS_WRAP = 1,
	D3DTADDRESS_MIRROR = 2,
	D3DTADDRESS_CLAMP = 3,
	D3DTADDRESS_BORDER = 4,
};

enum D3DTEXTUREFILTERTYPE {
	D3DTEXF_NONE = 0,
	D3DTEXF_POINT = 1,
	D3DTEXF_LINEAR = 2,
	D3DTEXF_ANISOTROPIC = 3,
};

enum D3DPRIMITIVETYPE {
	D3DPT_POINTLIST = 1,
	D3DPT_LINELIST = 2,
	D3DPT_LINESTRIP = 3,
	D3DPT_TRIANGLELIST = 4,
	D3DPT_TRIANGLESTRIP = 5,
	D3DPT_TRIANGLEFAN = 6,
};

// Flexible vertex format bits. The engine passes these straight to
// IDirect3DDevice8::SetVertexShader when it wants the fixed-function pipeline,
// which is the single nastiest thing to retarget (see the doc).
enum {
	D3DFVF_RESERVED0 = 0x001,
	D3DFVF_XYZ = 0x002,
	D3DFVF_XYZRHW = 0x004,
	D3DFVF_XYZB1 = 0x006,
	D3DFVF_XYZB2 = 0x008,
	D3DFVF_XYZB3 = 0x00a,
	D3DFVF_XYZB4 = 0x00c,
	D3DFVF_XYZB5 = 0x00e,
	D3DFVF_NORMAL = 0x010,
	D3DFVF_PSIZE = 0x020,
	D3DFVF_DIFFUSE = 0x040,
	D3DFVF_SPECULAR = 0x080,
	D3DFVF_TEX0 = 0x000,
	D3DFVF_TEX1 = 0x100,
	D3DFVF_TEX2 = 0x200,
	D3DFVF_TEX3 = 0x300,
	D3DFVF_TEX4 = 0x400,
	D3DFVF_TEX5 = 0x500,
	D3DFVF_TEX6 = 0x600,
	D3DFVF_TEX7 = 0x700,
	D3DFVF_TEX8 = 0x800,
	D3DFVF_TEXCOUNT_MASK = 0xf00,
	D3DFVF_TEXCOUNT_SHIFT = 8,
	D3DFVF_POSITION_MASK = 0x00e,
	// The last blend weight is a packed ubyte4 of bone indices rather than a float.
	// dx8fvf.cpp:62 relies on exactly this pairing with D3DFVF_XYZB4.
	D3DFVF_LASTBETA_UBYTE4 = 0x1000,
};

// D3DFVF_TEXCOORDSIZEn(i): two bits per coordinate set, packed above the texture
// count, saying how many floats that set has. dx8fvf.cpp decodes exactly this.
constexpr uint32_t D3DFVF_TEXCOORDSIZE1(uint32_t index) { return 3u << (index * 2 + 16); }
constexpr uint32_t D3DFVF_TEXCOORDSIZE2(uint32_t) { return 0u; }
constexpr uint32_t D3DFVF_TEXCOORDSIZE3(uint32_t index) { return 1u << (index * 2 + 16); }
constexpr uint32_t D3DFVF_TEXCOORDSIZE4(uint32_t index) { return 2u << (index * 2 + 16); }

// Number of floats in texture coordinate set `index`, per the D3D8 encoding above.
constexpr uint32_t Fvf_Texcoord_Components(uint32_t fvf, uint32_t index) {
	switch ((fvf >> (index * 2 + 16)) & 3u) {
	case 0: return 2;
	case 1: return 3;
	case 2: return 4;
	default: return 1;
	}
}

// The engine's named layouts (Core/Libraries/Source/WWVegas/WW3D2/dx8fvf.h). Kept
// here so the spike's tests exercise the real bit patterns rather than invented ones.
enum {
	DX8_FVF_XYZ = D3DFVF_XYZ,
	DX8_FVF_XYZN = D3DFVF_XYZ | D3DFVF_NORMAL,
	DX8_FVF_XYZNUV1 = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1,
	DX8_FVF_XYZNUV2 = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX2,
	DX8_FVF_XYZNDUV1 = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1 | D3DFVF_DIFFUSE,
	DX8_FVF_XYZNDUV2 = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX2 | D3DFVF_DIFFUSE,
	DX8_FVF_XYZDUV1 = D3DFVF_XYZ | D3DFVF_TEX1 | D3DFVF_DIFFUSE,
	DX8_FVF_XYZDUV2 = D3DFVF_XYZ | D3DFVF_TEX2 | D3DFVF_DIFFUSE,
	DX8_FVF_XYZUV1 = D3DFVF_XYZ | D3DFVF_TEX1,
	DX8_FVF_XYZUV2 = D3DFVF_XYZ | D3DFVF_TEX2,
	DX8_FVF_XYZNDUV1TG3 = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX4 |
	                      D3DFVF_TEXCOORDSIZE2(0) | D3DFVF_TEXCOORDSIZE3(1) |
	                      D3DFVF_TEXCOORDSIZE3(2) | D3DFVF_TEXCOORDSIZE3(3),
	DX8_FVF_XYZNUV2DMAP = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX3 |
	                      D3DFVF_TEXCOORDSIZE1(0) | D3DFVF_TEXCOORDSIZE4(1) |
	                      D3DFVF_TEXCOORDSIZE2(2),
};

} // namespace spike
