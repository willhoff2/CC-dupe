#include "state_translate.h"

#include <cstring>

namespace spike {

VkBlendFactor To_Vk_Blend_Factor(uint32_t b) {
	switch (b) {
	case D3DBLEND_ZERO: return VK_BLEND_FACTOR_ZERO;
	case D3DBLEND_ONE: return VK_BLEND_FACTOR_ONE;
	case D3DBLEND_SRCCOLOR: return VK_BLEND_FACTOR_SRC_COLOR;
	case D3DBLEND_INVSRCCOLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
	case D3DBLEND_SRCALPHA: return VK_BLEND_FACTOR_SRC_ALPHA;
	case D3DBLEND_INVSRCALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	case D3DBLEND_DESTALPHA: return VK_BLEND_FACTOR_DST_ALPHA;
	case D3DBLEND_INVDESTALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
	case D3DBLEND_DESTCOLOR: return VK_BLEND_FACTOR_DST_COLOR;
	case D3DBLEND_INVDESTCOLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
	case D3DBLEND_SRCALPHASAT: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
	default: return VK_BLEND_FACTOR_ONE;
	}
}

VkCompareOp To_Vk_Compare_Op(uint32_t c) {
	switch (c) {
	case D3DCMP_NEVER: return VK_COMPARE_OP_NEVER;
	case D3DCMP_LESS: return VK_COMPARE_OP_LESS;
	case D3DCMP_EQUAL: return VK_COMPARE_OP_EQUAL;
	case D3DCMP_LESSEQUAL: return VK_COMPARE_OP_LESS_OR_EQUAL;
	case D3DCMP_GREATER: return VK_COMPARE_OP_GREATER;
	case D3DCMP_NOTEQUAL: return VK_COMPARE_OP_NOT_EQUAL;
	case D3DCMP_GREATEREQUAL: return VK_COMPARE_OP_GREATER_OR_EQUAL;
	case D3DCMP_ALWAYS: return VK_COMPARE_OP_ALWAYS;
	default: return VK_COMPARE_OP_ALWAYS;
	}
}

// Note the handedness flip. D3D8 defines cull order against a left-handed,
// y-down-in-screen-space convention; Vulkan clip space is y-down with a
// counter-clockwise front face by default. The engine's projection matrices are
// Westwood/D3D convention, so the mapping below pairs with the y-flip applied to
// the projection matrix in vulkan_backend.cpp. Getting this pair wrong is the
// classic "everything is inside out" port bug.
VkCullModeFlags To_Vk_Cull_Mode(uint32_t c) {
	switch (c) {
	case D3DCULL_NONE: return VK_CULL_MODE_NONE;
	case D3DCULL_CW: return VK_CULL_MODE_FRONT_BIT;
	case D3DCULL_CCW: return VK_CULL_MODE_BACK_BIT;
	default: return VK_CULL_MODE_NONE;
	}
}

VkPolygonMode To_Vk_Polygon_Mode(uint32_t f) {
	switch (f) {
	case D3DFILL_POINT: return VK_POLYGON_MODE_POINT;
	case D3DFILL_WIREFRAME: return VK_POLYGON_MODE_LINE;
	default: return VK_POLYGON_MODE_FILL;
	}
}

VkSamplerAddressMode To_Vk_Address_Mode(uint32_t a) {
	switch (a) {
	case D3DTADDRESS_WRAP: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
	case D3DTADDRESS_MIRROR: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
	case D3DTADDRESS_CLAMP: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	case D3DTADDRESS_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
	default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
	}
}

VkFilter To_Vk_Filter(uint32_t f) {
	// D3DTEXF_ANISOTROPIC folds into VK_FILTER_LINEAR plus anisotropyEnable, which
	// lives on the sampler, not the filter field.
	return (f == D3DTEXF_POINT || f == D3DTEXF_NONE) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
}

VkSamplerMipmapMode To_Vk_Mipmap_Mode(uint32_t f) {
	return (f == D3DTEXF_LINEAR) ? VK_SAMPLER_MIPMAP_MODE_LINEAR
	                             : VK_SAMPLER_MIPMAP_MODE_NEAREST;
}

VkPrimitiveTopology To_Vk_Topology(uint32_t p) {
	switch (p) {
	case D3DPT_POINTLIST: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
	case D3DPT_LINELIST: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
	case D3DPT_LINESTRIP: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
	case D3DPT_TRIANGLELIST: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	case D3DPT_TRIANGLESTRIP: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
	case D3DPT_TRIANGLEFAN: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
	default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	}
}

VkColorComponentFlags To_Vk_Color_Write_Mask(uint32_t m) {
	// D3DCOLORWRITEENABLE_RED == 1<<0, GREEN 1<<1, BLUE 1<<2, ALPHA 1<<3.
	VkColorComponentFlags out = 0;
	if (m & 1) out |= VK_COLOR_COMPONENT_R_BIT;
	if (m & 2) out |= VK_COLOR_COMPONENT_G_BIT;
	if (m & 4) out |= VK_COLOR_COMPONENT_B_BIT;
	if (m & 8) out |= VK_COLOR_COMPONENT_A_BIT;
	return out;
}

VkStencilOp To_Vk_Stencil_Op(uint32_t op) {
	switch (op) {
	case D3DSTENCILOP_KEEP: return VK_STENCIL_OP_KEEP;
	case D3DSTENCILOP_ZERO: return VK_STENCIL_OP_ZERO;
	case D3DSTENCILOP_REPLACE: return VK_STENCIL_OP_REPLACE;
	case D3DSTENCILOP_INCRSAT: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
	case D3DSTENCILOP_DECRSAT: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
	case D3DSTENCILOP_INVERT: return VK_STENCIL_OP_INVERT;
	case D3DSTENCILOP_INCR: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
	case D3DSTENCILOP_DECR: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
	default: return VK_STENCIL_OP_KEEP;
	}
}

float Z_Bias_To_Depth_Bias_Constant_Factor(uint32_t z_bias) {
	// Negative: D3D8 pulls larger ZBIAS values towards the eye, and the engine's
	// depth comparison is LESSEQUAL, so "closer" means a smaller depth value.
	// Magnitude: one unit of Vulkan's constant factor is one r, the smallest
	// resolvable depth difference for the attachment format, so ZBIAS n maps to n
	// resolvable units. See the header for why no better-defined mapping exists.
	return -static_cast<float>(z_bias);
}

bool PipelineKey::operator==(const PipelineKey& o) const {
	return std::memcmp(this, &o, sizeof(PipelineKey)) == 0;
}

uint64_t Hash_Pipeline_Key(const PipelineKey& k) {
	const auto* bytes = reinterpret_cast<const unsigned char*>(&k);
	uint64_t h = 1469598103934665603ull; // FNV-1a
	for (size_t i = 0; i < sizeof(PipelineKey); ++i) {
		h ^= bytes[i];
		h *= 1099511628211ull;
	}
	return h;
}

namespace {

VkFormat Texcoord_Format(uint32_t components) {
	switch (components) {
	case 1: return VK_FORMAT_R32_SFLOAT;
	case 3: return VK_FORMAT_R32G32B32_SFLOAT;
	case 4: return VK_FORMAT_R32G32B32A32_SFLOAT;
	default: return VK_FORMAT_R32G32_SFLOAT;
	}
}

} // namespace

// The field order below is D3D8's fixed FVF ordering, which is also the order
// dx8fvf.cpp's FVFInfoClass accumulates offsets in. Any disagreement here shows up
// as garbled geometry, so the two must be read side by side.
bool Decode_Fvf(uint32_t fvf, VertexLayout& out) {
	out = VertexLayout{};
	uint32_t offset = 0;

	const uint32_t position_bits = fvf & D3DFVF_POSITION_MASK;
	if (position_bits == D3DFVF_XYZRHW) {
		out.attributes[out.attribute_count++] = {VA_POSITION, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offset};
		out.supplies[VA_POSITION] = true;
		out.pretransformed = true;
		offset += 16;
	} else if (position_bits >= D3DFVF_XYZ && position_bits <= D3DFVF_XYZB5) {
		// 3 floats; Vulkan fills the missing w with 1.0, which is what D3D does too.
		out.attributes[out.attribute_count++] = {VA_POSITION, 0, VK_FORMAT_R32G32B32_SFLOAT, offset};
		out.supplies[VA_POSITION] = true;
		offset += 12;

		// D3DFVF_XYZBn: n DWORDs of blend data after the position. With
		// D3DFVF_LASTBETA_UBYTE4 the final DWORD is four packed bone indices
		// rather than a weight -- that is the pairing dx8fvf.cpp:62 emits for
		// the engine's skinned meshes.
		// The position bits are not evenly spaced: XYZ is 0x002 but XYZB1..XYZB5 run
		// 0x006, 0x008, 0x00a, 0x00c, 0x00e, so n = (bits - 4) / 2 and XYZ is its own
		// case. Treating XYZ as part of the run gives every skinned FVF one blend
		// DWORD too many.
		const uint32_t blend_dwords =
		    position_bits == D3DFVF_XYZ ? 0u : (position_bits - 4u) / 2u;
		if (blend_dwords > 0) {
			const bool last_is_indices = (fvf & D3DFVF_LASTBETA_UBYTE4) != 0;
			const uint32_t weights = last_is_indices ? blend_dwords - 1 : blend_dwords;
			if (weights > 3) return false; // needs a 4th weight attribute; nothing emits it
			if (weights > 0) {
				out.attributes[out.attribute_count++] = {
				    VA_BLENDWEIGHT, 0, Texcoord_Format(weights), offset};
				out.supplies[VA_BLENDWEIGHT] = true;
				offset += weights * 4;
			}
			if (last_is_indices) {
				out.attributes[out.attribute_count++] = {
				    VA_BLENDINDICES, 0, VK_FORMAT_R8G8B8A8_UINT, offset};
				out.supplies[VA_BLENDINDICES] = true;
				offset += 4;
			}
		}
	} else {
		return false; // no position: not a fixed-function FVF
	}

	if (fvf & D3DFVF_NORMAL) {
		out.attributes[out.attribute_count++] = {VA_NORMAL, 0, VK_FORMAT_R32G32B32_SFLOAT, offset};
		out.supplies[VA_NORMAL] = true;
		offset += 12;
	}
	if (fvf & D3DFVF_PSIZE) {
		offset += 4; // point size: stride only, the spike does not draw point sprites
	}
	if (fvf & D3DFVF_DIFFUSE) {
		// D3DCOLOR is 0xAARRGGBB, i.e. B,G,R,A in memory on a little-endian host.
		out.attributes[out.attribute_count++] = {VA_DIFFUSE, 0, VK_FORMAT_B8G8R8A8_UNORM, offset};
		out.supplies[VA_DIFFUSE] = true;
		offset += 4;
	}
	if (fvf & D3DFVF_SPECULAR) {
		out.attributes[out.attribute_count++] = {VA_SPECULAR, 0, VK_FORMAT_B8G8R8A8_UNORM, offset};
		out.supplies[VA_SPECULAR] = true;
		offset += 4;
	}

	const uint32_t texcoord_sets = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
	if (texcoord_sets > kMaxTexCoordSets) return false;
	out.texcoord_sets = texcoord_sets;
	for (uint32_t i = 0; i < texcoord_sets; ++i) {
		const uint32_t components = Fvf_Texcoord_Components(fvf, i);
		const uint32_t loc = VA_TEXCOORD0 + i;
		out.attributes[out.attribute_count++] = {loc, 0, Texcoord_Format(components), offset};
		out.supplies[loc] = true;
		offset += components * 4;
	}

	out.stride = offset;
	return true;
}

} // namespace spike
