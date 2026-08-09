#version 450

// Renderer spike: stand-in for D3D8's fixed-function vertex pipeline.
//
// Every location here must have a matching VkVertexInputAttributeDescription, even
// when the engine's FVF does not supply it -- Vulkan has no equivalent of D3D8's
// "the FVF says what exists". state_translate.cpp points the missing ones at a
// constant dummy buffer.

layout(location = 0) in vec4 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec4 a_diffuse;
layout(location = 3) in vec2 a_texcoord0;
layout(location = 4) in vec2 a_texcoord1;

layout(push_constant) uniform Push {
	mat4 wvp;            // world * view * projection, already y-flipped for Vulkan
	ivec4 flags;         // .x = 1 when the position is D3DFVF_XYZRHW (pretransformed)
	vec4 viewport;       // .xy = render target size in pixels
} push;

layout(location = 0) out vec4 v_diffuse;
layout(location = 1) out vec2 v_texcoord0;
layout(location = 2) out vec2 v_texcoord1;

void main() {
	if (push.flags.x != 0) {
		// D3DFVF_XYZRHW: x,y are screen pixels, z is 0..1 depth, w is 1/w.
		// D3D consumes these post-viewport-transform; Vulkan has no such path, so
		// the transform has to be undone by hand into clip space.
		float rhw = a_position.w == 0.0 ? 1.0 : a_position.w;
		float w = 1.0 / rhw;
		vec2 ndc = vec2(a_position.x / push.viewport.x, a_position.y / push.viewport.y) * 2.0 - 1.0;
		gl_Position = vec4(ndc * w, a_position.z * w, w);
	} else {
		gl_Position = push.wvp * vec4(a_position.xyz, 1.0);
	}

	v_diffuse = a_diffuse;
	v_texcoord0 = a_texcoord0;
	v_texcoord1 = a_texcoord1;
}
