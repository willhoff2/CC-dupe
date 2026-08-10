#version 450

// Probe geometry: NDC position, uv, colour. Depth comes from a push constant so a
// single vertex buffer can be drawn at several depths (the depth-test case).

layout(location = 0) in vec2 in_position;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;

layout(location = 0) out vec2 out_uv;
layout(location = 1) out vec4 out_color;

layout(push_constant) uniform Push {
	float z;
	int mode;
} push;

void main() {
	out_uv = in_uv;
	out_color = in_color;
	gl_Position = vec4(in_position, push.z, 1.0);
}
