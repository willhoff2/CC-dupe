#version 450

// Renderer spike: D3D8's texture-stage cascade, interpreted at runtime.
//
// D3DTSS_COLOROP / COLORARG1 / COLORARG2 / ALPHAOP / ... have no Vulkan equivalent
// whatsoever: they are a tiny interpreted program that D3D8 drivers used to compile
// into combiner state. The engine sets them 865 times across 23 distinct states.
//
// This shader is the "uber-shader" strategy: read the stage state from a uniform
// block and branch. It is the strategy that keeps the engine's call sites unchanged.
// The alternative -- compiling a shader permutation per stage-state combination --
// is faster but needs a shader cache and a compile-on-demand path.

layout(location = 0) in vec4 v_diffuse;
layout(location = 1) in vec2 v_texcoord0;
layout(location = 2) in vec2 v_texcoord1;

layout(location = 0) out vec4 o_color;

// Must match spike::StageUniforms in state_translate.h.
layout(set = 0, binding = 0) uniform StageState {
	ivec4 stage0;   // COLOROP, COLORARG1, COLORARG2, ALPHAOP
	ivec4 stage0b;  // ALPHAARG1, ALPHAARG2, TEXCOORDINDEX, -
	ivec4 stage1;
	ivec4 stage1b;
	vec4 tfactor;   // D3DRS_TEXTUREFACTOR
	ivec4 flags;    // alpha test enable, alpha func, -, -
	vec4 misc;      // alpha ref (normalised), -, -, -
} ss;

layout(set = 0, binding = 1) uniform sampler2D u_tex0;
layout(set = 0, binding = 2) uniform sampler2D u_tex1;

// D3DTOP_*
const int TOP_DISABLE = 1;
const int TOP_SELECTARG1 = 2;
const int TOP_SELECTARG2 = 3;
const int TOP_MODULATE = 4;
const int TOP_MODULATE2X = 5;
const int TOP_MODULATE4X = 6;
const int TOP_ADD = 7;
const int TOP_ADDSIGNED = 8;
const int TOP_ADDSIGNED2X = 9;
const int TOP_SUBTRACT = 10;
const int TOP_ADDSMOOTH = 11;
const int TOP_BLENDTEXTUREALPHA = 13;
const int TOP_BLENDCURRENTALPHA = 16;
const int TOP_DOTPRODUCT3 = 24;
// D3DTOP_MULTIPLYADD and D3DTOP_LERP need COLORARG0, which this spike does not plumb
// through; they fall through to SELECTARG1. The engine uses MULTIPLYADD twice.

// D3DTA_*
const int TA_DIFFUSE = 0;
const int TA_CURRENT = 1;
const int TA_TEXTURE = 2;
const int TA_TFACTOR = 3;
const int TA_SELECTMASK = 0x0f;
const int TA_COMPLEMENT = 0x10;
const int TA_ALPHAREPLICATE = 0x20;

// D3DCMP_*
const int CMP_NEVER = 1;
const int CMP_LESS = 2;
const int CMP_EQUAL = 3;
const int CMP_LESSEQUAL = 4;
const int CMP_GREATER = 5;
const int CMP_NOTEQUAL = 6;
const int CMP_GREATEREQUAL = 7;

vec4 pick_arg(int arg, vec4 tex, vec4 current) {
	vec4 v;
	switch (arg & TA_SELECTMASK) {
	case TA_DIFFUSE: v = v_diffuse; break;
	case TA_CURRENT: v = current; break;
	case TA_TEXTURE: v = tex; break;
	case TA_TFACTOR: v = ss.tfactor; break;
	default: v = current; break;
	}
	if ((arg & TA_ALPHAREPLICATE) != 0) v = vec4(v.a);
	if ((arg & TA_COMPLEMENT) != 0) v = vec4(1.0) - v;
	return v;
}

vec3 apply_color_op(int op, vec3 a1, vec3 a2, vec4 tex, vec4 current) {
	switch (op) {
	case TOP_SELECTARG1: return a1;
	case TOP_SELECTARG2: return a2;
	case TOP_MODULATE: return a1 * a2;
	case TOP_MODULATE2X: return clamp(a1 * a2 * 2.0, 0.0, 1.0);
	case TOP_MODULATE4X: return clamp(a1 * a2 * 4.0, 0.0, 1.0);
	case TOP_ADD: return clamp(a1 + a2, 0.0, 1.0);
	case TOP_ADDSIGNED: return clamp(a1 + a2 - 0.5, 0.0, 1.0);
	case TOP_ADDSIGNED2X: return clamp((a1 + a2 - 0.5) * 2.0, 0.0, 1.0);
	case TOP_SUBTRACT: return clamp(a1 - a2, 0.0, 1.0);
	case TOP_ADDSMOOTH: return clamp(a1 + a2 - a1 * a2, 0.0, 1.0);
	case TOP_BLENDTEXTUREALPHA: return mix(a2, a1, tex.a);
	case TOP_BLENDCURRENTALPHA: return mix(a2, a1, current.a);
	case TOP_DOTPRODUCT3: return vec3(clamp(dot(a1 * 2.0 - 1.0, a2 * 2.0 - 1.0), 0.0, 1.0));
	default: return a1;
	}
}

float apply_alpha_op(int op, float a1, float a2) {
	switch (op) {
	case TOP_SELECTARG1: return a1;
	case TOP_SELECTARG2: return a2;
	case TOP_MODULATE: return a1 * a2;
	case TOP_MODULATE2X: return clamp(a1 * a2 * 2.0, 0.0, 1.0);
	case TOP_MODULATE4X: return clamp(a1 * a2 * 4.0, 0.0, 1.0);
	case TOP_ADD: return clamp(a1 + a2, 0.0, 1.0);
	case TOP_ADDSIGNED: return clamp(a1 + a2 - 0.5, 0.0, 1.0);
	case TOP_SUBTRACT: return clamp(a1 - a2, 0.0, 1.0);
	default: return a1;
	}
}

vec4 run_stage(ivec4 s, ivec4 sb, vec4 tex, vec4 current) {
	if (s.x == TOP_DISABLE) return current;
	vec4 c1 = pick_arg(s.y, tex, current);
	vec4 c2 = pick_arg(s.z, tex, current);
	vec4 a1 = pick_arg(sb.x, tex, current);
	vec4 a2 = pick_arg(sb.y, tex, current);
	vec3 rgb = apply_color_op(s.x, c1.rgb, c2.rgb, tex, current);
	// D3D8 rule: an ALPHAOP of DISABLE with an enabled COLOROP leaves alpha at the
	// previous stage's value rather than forcing it to 1.
	float a = (s.w == TOP_DISABLE) ? current.a : apply_alpha_op(s.w, a1.a, a2.a);
	return vec4(rgb, a);
}

void main() {
	vec4 current = v_diffuse;

	current = run_stage(ss.stage0, ss.stage0b, texture(u_tex0, v_texcoord0), current);
	current = run_stage(ss.stage1, ss.stage1b, texture(u_tex1, v_texcoord1), current);

	// D3DRS_ALPHATESTENABLE / ALPHAFUNC / ALPHAREF. Vulkan dropped fixed-function
	// alpha test entirely, so it becomes a discard in the shader -- which also means
	// it is pipeline-invariant here but changes early-z behaviour on some GPUs.
	if (ss.flags.x != 0) {
		float ref = ss.misc.x;
		bool pass;
		switch (ss.flags.y) {
		case CMP_NEVER: pass = false; break;
		case CMP_LESS: pass = current.a < ref; break;
		case CMP_EQUAL: pass = current.a == ref; break;
		case CMP_LESSEQUAL: pass = current.a <= ref; break;
		case CMP_GREATER: pass = current.a > ref; break;
		case CMP_NOTEQUAL: pass = current.a != ref; break;
		case CMP_GREATEREQUAL: pass = current.a >= ref; break;
		default: pass = true; break;
		}
		if (!pass) discard;
	}

	o_color = current;
}
