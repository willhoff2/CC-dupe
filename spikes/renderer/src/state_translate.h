// Renderer spike: D3D8 fixed-function state -> Vulkan translation.
//
// This file is the interesting half of the spike. D3D8 lets the engine change one
// piece of pipeline state at a time; Vulkan bakes all of it into an immutable
// VkPipeline. So every retargeting strategy needs exactly what is here:
//
//   1. enum -> enum tables for the states that have a 1:1 Vulkan equivalent,
//   2. a pipeline key + cache for the states that are baked into a VkPipeline,
//   3. a uniform block for the states that have no Vulkan equivalent at all and
//      must be interpreted by a shader at runtime (the texture-stage cascade).
//
// Category (3) is the whole risk of Phase 4.

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
	uint32_t color_write_enable = 0xf;
	uint32_t stencil_enable = 0;
	// D3DRS_ZBIAS is a 0..16 integer in D3D8 and a float pair in Vulkan; kept in
	// the key because depthBias is pipeline state unless dynamic state is used.
	uint32_t z_bias = 0;

	bool operator==(const PipelineKey& o) const;
};

uint64_t Hash_Pipeline_Key(const PipelineKey& k);

// --- FVF -> VkPipelineVertexInputStateCreateInfo ----------------------------
// The engine calls SetVertexShader(fvf) at 15 of its 23 SetVertexShader sites, i.e.
// it uses D3D8's fixed-function vertex declaration. Vulkan has no equivalent, so
// the FVF bitfield has to be decoded into explicit attribute descriptions.
enum VertexAttribLocation {
	VA_POSITION = 0,
	VA_NORMAL = 1,
	VA_DIFFUSE = 2,
	VA_TEXCOORD0 = 3,
	VA_TEXCOORD1 = 4,
	VA_COUNT = 5,
};

struct VertexLayout {
	VkVertexInputAttributeDescription attributes[VA_COUNT]{};
	uint32_t attribute_count = 0;
	uint32_t stride = 0;
	bool pretransformed = false; // D3DFVF_XYZRHW: position already in screen space
	// Locations the FVF does not supply. Vulkan requires every shader input to have
	// an attribute, so these are pointed at a constant dummy binding.
	bool supplies[VA_COUNT]{};
};

// Returns false for FVF bits the spike does not decode (blend weights, >2 texcoord
// sets, non-2D texcoord sizes). Those are enumerated in the doc as real work.
bool Decode_Fvf(uint32_t fvf, VertexLayout& out);

// --- category 3: state with no Vulkan equivalent ----------------------------
// D3D8's texture-stage cascade. Mirrors the std140 uniform block in
// shaders/fixedfunc.frag; a real backend either does this or compiles a shader
// permutation per stage-state combination.
struct alignas(16) StageUniforms {
	int32_t stage0[4]{D3DTOP_MODULATE, D3DTA_TEXTURE, D3DTA_DIFFUSE, D3DTOP_MODULATE};
	int32_t stage0b[4]{D3DTA_TEXTURE, D3DTA_DIFFUSE, 0, 0};
	int32_t stage1[4]{D3DTOP_DISABLE, D3DTA_TEXTURE, D3DTA_CURRENT, D3DTOP_DISABLE};
	int32_t stage1b[4]{D3DTA_TEXTURE, D3DTA_CURRENT, 0, 0};
	float tfactor[4]{1.f, 1.f, 1.f, 1.f};
	int32_t flags[4]{0, D3DCMP_ALWAYS, 0, 0}; // alphatest enable, alphafunc, -, -
	float misc[4]{0.f, 0.f, 0.f, 0.f};        // alpharef (0..1), -, -, -
};

} // namespace spike
