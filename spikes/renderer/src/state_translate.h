// Renderer spike: D3D8 fixed-function state -> Vulkan translation.
//
// This file is the interesting half of the spike. D3D8 lets the engine change one
// piece of pipeline state at a time; Vulkan bakes all of it into an immutable
// VkPipeline. So every retargeting strategy needs exactly what is here:
//
//   1. enum -> enum tables for the states that have a 1:1 Vulkan equivalent,
//   2. a pipeline key + cache for the states that are baked into a VkPipeline,
//   3. a uniform block for the states that have no Vulkan equivalent at all and
//      must be interpreted by a shader at runtime (the texture-stage cascade, the
//      fixed-function transform/lighting/fog pipeline, and alpha test).
//
// Category (3) is the whole risk of Phase 4. The contents of DrawUniforms below are
// measured, not guessed: see tools/texture-stage-scan.py, tools/engine-usage-scan.py
// and docs/porting/fixed-function-measurements.md.

#pragma once

#include "d3d8_subset.h"

#include <cstdint>
#include <vulkan/vulkan.h>

namespace spike {

// --- category 1: direct enum mappings ---------------------------------------
VkBlendFactor To_Vk_Blend_Factor(uint32_t d3d_blend);
VkCompareOp To_Vk_Compare_Op(uint32_t d3d_cmp);
VkCullModeFlags To_Vk_Cull_Mode(uint32_t d3d_cull);
VkPolygonMode To_Vk_Polygon_Mode(uint32_t d3d_fill);
VkSamplerAddressMode To_Vk_Address_Mode(uint32_t d3d_address);
VkFilter To_Vk_Filter(uint32_t d3d_filter);
VkSamplerMipmapMode To_Vk_Mipmap_Mode(uint32_t d3d_mip_filter);
VkPrimitiveTopology To_Vk_Topology(uint32_t d3d_primitive_type);
VkColorComponentFlags To_Vk_Color_Write_Mask(uint32_t d3d_colorwriteenable);
VkStencilOp To_Vk_Stencil_Op(uint32_t d3d_stencil_op);
VkBlendOp To_Vk_Blend_Op(uint32_t d3d_blend_op);

// Number of vertices (or indices) a primitive count covers, per D3DPRIMITIVETYPE.
// D3D8 counts primitives, Vulkan counts vertices, and the two differ per topology.
uint32_t Vertex_Count_For_Primitives(uint32_t d3d_primitive_type, uint32_t primitive_count);

// --- category 2: what a VkPipeline has to bake in ---------------------------
// Every field here is a D3DRS_* the engine sets at draw granularity. Changing any
// one of them means a different VkPipeline, which is why the cache exists.
struct PipelineKey {
	uint32_t fvf = 0;
	uint32_t topology = D3DPT_TRIANGLELIST;

	uint32_t z_enable = 1;
	uint32_t z_write_enable = 1;
	uint32_t z_func = D3DCMP_LESSEQUAL;
	uint32_t cull_mode = D3DCULL_CCW;
	uint32_t fill_mode = D3DFILL_SOLID;
	uint32_t shade_mode = 2; // D3DSHADE_GOURAUD
	uint32_t alpha_blend_enable = 0;
	uint32_t src_blend = D3DBLEND_ONE;
	uint32_t dest_blend = D3DBLEND_ZERO;
	uint32_t blend_op = D3DBLENDOP_ADD;
	uint32_t color_write_enable = 0xf;
	uint32_t stencil_enable = 0;
	uint32_t stencil_func = D3DCMP_ALWAYS;
	uint32_t stencil_fail = D3DSTENCILOP_KEEP;
	uint32_t stencil_zfail = D3DSTENCILOP_KEEP;
	uint32_t stencil_pass = D3DSTENCILOP_KEEP;
	uint32_t stencil_ref = 0;
	uint32_t stencil_mask = 0xffffffff;
	uint32_t stencil_write_mask = 0xffffffff;
	// Whether D3DRS_ZBIAS is non-zero, not its value: the backend uses
	// VK_DYNAMIC_STATE_DEPTH_BIAS, so the amount does not need its own pipeline.
	uint32_t depth_bias_enable = 0;
	// Whether the current render target has a depth/stencil surface. Not a D3D8 state
	// at all: a Vulkan pipeline is only usable with a render pass it is compatible
	// with, and D3D8's SetRenderTarget(colour, nullptr) is a pass with no depth
	// attachment.
	uint32_t has_depth_attachment = 1;

	bool operator==(const PipelineKey& o) const;
};

uint64_t Hash_Pipeline_Key(const PipelineKey& k);

// D3DRS_ZBIAS (0..16) -> VkPipelineRasterizationStateCreateInfo::depthBiasConstantFactor.
//
// D3D8's ZBIAS has no defined unit: the spec says larger values are "closer to the
// eye" and leaves the magnitude to the driver. Vulkan's depthBiasConstantFactor is
// defined: it is multiplied by r, the smallest resolvable depth difference of the
// depth format. So the one mapping that *is* defensible is "one ZBIAS unit == one
// resolvable depth unit", i.e. factor = -zbias, which is the smallest bias that is
// guaranteed to change the comparison result and cannot silently do nothing.
//
// The engine only ever writes 0, 1, 4, 7 and 8 (tools/engine-usage-scan.py), all of
// them to lift decals/tracks/water off the terrain, so the sign matters and the
// exact scale does not, as long as it is monotonic and non-zero.
float Z_Bias_To_Depth_Bias_Constant_Factor(uint32_t z_bias);

// --- FVF -> VkPipelineVertexInputStateCreateInfo ----------------------------
// The engine calls SetVertexShader(fvf) at 15 of its 23 SetVertexShader sites, i.e.
// it uses D3D8's fixed-function vertex declaration. Vulkan has no equivalent, so
// the FVF bitfield has to be decoded into explicit attribute descriptions.
enum VertexAttribLocation {
	VA_POSITION = 0,
	VA_BLENDWEIGHT = 1,
	VA_BLENDINDICES = 2,
	VA_NORMAL = 3,
	VA_DIFFUSE = 4,
	VA_SPECULAR = 5,
	VA_TEXCOORD0 = 6,
	VA_TEXCOORD1 = 7,
	VA_TEXCOORD2 = 8,
	VA_TEXCOORD3 = 9,
	VA_COUNT = 10,
};

// Backing store for shader inputs the FVF does not supply. Vulkan requires an
// attribute for every shader input, so the missing ones are pointed at this,
// with the values D3D8's fixed-function pipeline substitutes when a vertex
// component is absent: white diffuse, black specular, +Z normal, zero texcoords
// with a w of 1 so a PROJECTED texture transform divides by one.
struct DummyVertex {
	float texcoord[4]{0.0f, 0.0f, 0.0f, 1.0f}; // offset 0
	float normal[3]{0.0f, 0.0f, 1.0f};         // offset 16
	uint32_t diffuse = 0xffffffffu;            // offset 28
	uint32_t specular = 0x00000000u;           // offset 32
	float blend_weight[3]{0.0f, 0.0f, 0.0f};   // offset 36
	uint32_t blend_indices = 0;                // offset 48
	uint32_t pad[3]{0, 0, 0};                  // offset 52, stride 64
};
static_assert(sizeof(DummyVertex) == 64, "offsets below are baked into the pipeline");
// Offsets of DummyVertex's members, in the order the enum above needs them.
constexpr uint32_t kDummyOffsets[VA_COUNT] = {
    0,  // VA_POSITION: never dummied, an FVF without a position is rejected
    36, // VA_BLENDWEIGHT
    48, // VA_BLENDINDICES
    16, // VA_NORMAL
    28, // VA_DIFFUSE
    32, // VA_SPECULAR
    0, 0, 0, 0, // VA_TEXCOORD0..3
};

struct VertexLayout {
	VkVertexInputAttributeDescription attributes[VA_COUNT]{};
	uint32_t attribute_count = 0;
	uint32_t stride = 0;
	bool pretransformed = false; // D3DFVF_XYZRHW: position already in screen space
	uint32_t texcoord_sets = 0;
	// Locations the FVF does not supply. Vulkan requires every shader input to have
	// an attribute, so these are pointed at the DummyVertex binding.
	bool supplies[VA_COUNT]{};
};

// Returns false only for FVFs the engine cannot produce: no position, more than
// kMaxTexCoordSets coordinate sets, or a blend-weight count other than the
// D3DFVF_XYZB4|D3DFVF_LASTBETA_UBYTE4 pairing dx8fvf.cpp emits.
bool Decode_Fvf(uint32_t fvf, VertexLayout& out);

// --- category 3: state with no Vulkan equivalent ----------------------------
// D3D8's texture-stage cascade plus its fixed-function transform, lighting and fog.
// Mirrors, field for field, the std140 uniform block shared by
// shaders/fixedfunc.vert and shaders/fixedfunc.frag.
//
// A real backend either does this (one uber-shader, state in a uniform) or compiles
// a shader permutation per state combination. The measured combination count is in
// docs/porting/fixed-function-measurements.md; it is what decides between the two.
struct alignas(16) DrawUniforms {
	// D3D8 lights and fog are defined in camera space, so lighting is done there:
	// world_view puts the vertex in camera space and `view` puts the world-space
	// light positions and directions there too.
	float wvp[16]{};        // world*view*projection, y-flipped for Vulkan clip space
	float world[16]{};      // object -> world; user clip planes are in world space
	float world_view[16]{}; // object -> camera; the vertex shader derives the
	                        // normal matrix from it
	float view[16]{};       // world -> camera
	float tex_matrix[4][16]{};     // D3DTS_TEXTURE0..3

	// One entry per D3D8 texture stage.
	int32_t stage_color[kMaxTextureStages][4]{};  // COLOROP, COLORARG1, COLORARG2, COLORARG0
	int32_t stage_alpha[kMaxTextureStages][4]{};  // ALPHAOP, ALPHAARG1, ALPHAARG2, ALPHAARG0
	int32_t stage_misc[kMaxTextureStages][4]{};   // TEXCOORDINDEX, TEXTURETRANSFORMFLAGS, has_texture, -
	float stage_bump[kMaxTextureStages][4]{};     // BUMPENVMAT00, 01, 10, 11
	float stage_bump_lum[kMaxTextureStages][4]{}; // BUMPENVLSCALE, BUMPENVLOFFSET, -, -

	float light_diffuse[kMaxLights][4]{};
	float light_ambient[kMaxLights][4]{};
	float light_specular[kMaxLights][4]{};
	float light_position[kMaxLights][4]{};  // world-space xyz, w = D3DLIGHTTYPE (0 = off)
	float light_direction[kMaxLights][4]{}; // world-space xyz (light -> scene), w = range
	float light_attenuation[kMaxLights][4]{}; // a0, a1, a2, falloff
	float light_spot[kMaxLights][4]{};        // cos(theta/2), cos(phi/2), -, -

	float material_diffuse[4]{1.f, 1.f, 1.f, 1.f};
	float material_ambient[4]{1.f, 1.f, 1.f, 1.f};
	float material_specular[4]{0.f, 0.f, 0.f, 0.f};
	float material_emissive[4]{0.f, 0.f, 0.f, 0.f};
	float material_power[4]{0.f, 0.f, 0.f, 0.f}; // .x = specular exponent
	float global_ambient[4]{0.f, 0.f, 0.f, 0.f}; // D3DRS_AMBIENT
	float tfactor[4]{1.f, 1.f, 1.f, 1.f};        // D3DRS_TEXTUREFACTOR
	float fog_color[4]{0.f, 0.f, 0.f, 0.f};
	float fog_params[4]{0.f, 1.f, 1.f, 0.f};     // start, end, density, -
	float misc[4]{0.f, 0.f, 0.f, 0.f};           // alpha ref (0..1), viewport w, viewport h, -

	int32_t flags[4]{0, D3DCMP_ALWAYS, 0, 0};    // alphatest enable, alphafunc, lighting, fogenable
	int32_t flags2[4]{0, D3DFOG_NONE, D3DFOG_NONE, 0}; // pretransformed, fogvertexmode, fogtablemode, specularenable
	int32_t sources[4]{D3DMCS_COLOR1, D3DMCS_COLOR2, D3DMCS_MATERIAL, D3DMCS_MATERIAL};
	int32_t flags3[4]{1, 0, 1, 0};               // colorvertex, normalizenormals, localviewer, rangefog

	// D3DRS_POINTSIZE/_MIN/_MAX and D3DRS_POINTSPRITEENABLE. Vulkan expands a point
	// through gl_PointSize, which is a vertex-shader output rather than pipeline
	// state, so the whole group travels in the uniform block.
	float point_size[4]{1.f, 0.f, 64.f, 0.f}; // size, min, max, sprite enable
	// D3DRS_POINTSCALE_A/B/C and D3DRS_POINTSCALEENABLE.
	float point_scale[4]{1.f, 0.f, 0.f, 0.f}; // a, b, c, scale enable

	// --- ps.1.1 / vs.1.1, interpreted at draw time ---------------------------
	// The engine loads its 16 shaders as compiled D3D8 token streams from .pso/.vso
	// files (W3DShaderManager::LoadAndCreateD3DShader), so a port has to consume D3D8
	// tokens at runtime, not shader source. Rather than translate a token stream into
	// SPIR-V per shader, the tokens travel here and the uber-shader decodes the same
	// bit fields d3d8types.h defines -- the same choice the texture-stage cascade
	// already makes, for the same reason.
	//
	// Two ivec4 per instruction: {opcode token, destination token, source 0, source 1}
	// and {source 2, relative-address token, -, -}.
	int32_t ps_program[kMaxShaderInstructions][8]{};
	int32_t vs_program[kMaxShaderInstructions][8]{};
	float ps_constants[kMaxPixelShaderConstants][4]{};
	float vs_constants[kMaxVertexShaderConstants][4]{};
	// v-register -> vertex element, from the D3DVSD_* declaration. VA_* value, or -1.
	int32_t vs_inputs[kMaxVertexShaderInputs / 4][4]{};
	// ps instruction count, vs instruction count, -, -. Zero means fixed function.
	int32_t shader_counts[4]{};

	// D3D8 user clip planes (SetClipPlane) and the D3DRS_CLIPPLANEENABLE bitmask.
	float clip_planes[kMaxClipPlanes][4]{};
	int32_t clip_enable[4]{};
};

} // namespace spike
