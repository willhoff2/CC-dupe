#version 450

// One shader, several modes -- each mode is one thing the real engine needs and the
// two-draw spike does not cover. See src/feature_probe.cpp.

layout(location = 0) in vec2 in_uv;
layout(location = 1) in vec4 in_color;

layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D tex0;
layout(set = 0, binding = 1) uniform sampler2D tex1;

layout(push_constant) uniform Push {
	float z;
	int mode;
} push;

const int MODE_DIFFUSE = 0;
const int MODE_TEXTURE0 = 1;
const int MODE_TWO_STAGE_MODULATE = 2;
const int MODE_TEXTURE0_LOD1 = 3;

void main() {
	if (push.mode == MODE_TEXTURE0) {
		out_color = texture(tex0, in_uv);
	} else if (push.mode == MODE_TWO_STAGE_MODULATE) {
		out_color = texture(tex0, in_uv) * texture(tex1, in_uv);
	} else if (push.mode == MODE_TEXTURE0_LOD1) {
		out_color = textureLod(tex0, in_uv, 1.0);
	} else {
		out_color = in_color;
	}
}
