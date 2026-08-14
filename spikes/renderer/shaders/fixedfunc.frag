#version 450
// D3D8 fixed-function texture-stage cascade, interpreted at runtime.
//
// This is the part of D3D8 with no Vulkan equivalent whatsoever. D3D8 configures a
// chain of up to 8 combiner stages through SetTextureStageState; Vulkan has only
// shaders. Two strategies exist: compile one shader permutation per state
// combination, or write one uber-shader that reads the state from a uniform. This
// is the second, because the measured combination count makes the first
// impractical to warm up (see docs/porting/fixed-function-measurements.md).
//
// The measured set of operations the engine can request is 17 of D3D8's 26
// D3DTOP_* values (docs/porting/fixed-function-measurements.md). All 17 are
// implemented. So are 8 more that cost one line each, because a missing op renders
// something plausible-but-wrong rather than failing visibly. The one op that is not
// implemented is D3DTOP_PREMODULATE, which is defined in terms of the *next*
// stage's texture and which no engine call site uses; it falls through to
// SELECTARG1, as does any value outside 1..26.
//
// The uniform block must stay identical to spike::DrawUniforms in
// src/state_translate.h and to the copy in fixedfunc.vert.

#define MAX_STAGES 8
#define MAX_LIGHTS 4
// Must match src/d3d8_subset.h and the copy in fixedfunc.vert.
#define MAX_SHADER_INSTRUCTIONS 32
#define MAX_PS_CONSTANTS 8
#define MAX_VS_CONSTANTS 96
#define MAX_VS_INPUTS 16
#define MAX_CLIP_PLANES 6
#define PS_TEMPS 2
#define PS_TEXTURES 4

layout(location = 0) in vec4 v_diffuse;
layout(location = 1) in vec4 v_specular;
layout(location = 2) in vec4 v_misc;
layout(location = 3) in vec4 v_texcoord[MAX_STAGES];

layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0, std140) uniform Draw {
	mat4 wvp;
	mat4 world;
	mat4 world_view;
	mat4 view;
	mat4 tex_matrix[4];

	ivec4 stage_color[MAX_STAGES];
	ivec4 stage_alpha[MAX_STAGES];
	ivec4 stage_misc[MAX_STAGES];
	vec4 stage_bump[MAX_STAGES];
	vec4 stage_bump_lum[MAX_STAGES];

	vec4 light_diffuse[MAX_LIGHTS];
	vec4 light_ambient[MAX_LIGHTS];
	vec4 light_specular[MAX_LIGHTS];
	vec4 light_position[MAX_LIGHTS];
	vec4 light_direction[MAX_LIGHTS];
	vec4 light_attenuation[MAX_LIGHTS];
	vec4 light_spot[MAX_LIGHTS];

	vec4 material_diffuse;
	vec4 material_ambient;
	vec4 material_specular;
	vec4 material_emissive;
	vec4 material_power;
	vec4 global_ambient;
	vec4 tfactor;
	vec4 fog_color;
	vec4 fog_params;
	vec4 misc;

	ivec4 flags;
	ivec4 flags2;
	ivec4 sources;
	ivec4 flags3;
	vec4 point_size;  // size, min, max, point-sprite active for this draw
	vec4 point_scale;

	ivec4 ps_program[MAX_SHADER_INSTRUCTIONS * 2];
	ivec4 vs_program[MAX_SHADER_INSTRUCTIONS * 2];
	vec4 ps_constants[MAX_PS_CONSTANTS];
	vec4 vs_constants[MAX_VS_CONSTANTS];
	ivec4 vs_inputs[MAX_VS_INPUTS / 4];
	ivec4 shader_counts; // ps instructions, vs instructions, -, -

	vec4 clip_planes[MAX_CLIP_PLANES];
	ivec4 clip_enable;
} u;

layout(set = 0, binding = 1) uniform sampler2D u_texture[MAX_STAGES];

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
const int TOP_BLENDDIFFUSEALPHA = 12;
const int TOP_BLENDTEXTUREALPHA = 13;
const int TOP_BLENDFACTORALPHA = 14;
const int TOP_BLENDTEXTUREALPHAPM = 15;
const int TOP_BLENDCURRENTALPHA = 16;
const int TOP_MODULATEALPHA_ADDCOLOR = 18;
const int TOP_MODULATECOLOR_ADDALPHA = 19;
const int TOP_MODULATEINVALPHA_ADDCOLOR = 20;
const int TOP_MODULATEINVCOLOR_ADDALPHA = 21;
const int TOP_BUMPENVMAP = 22;
const int TOP_BUMPENVMAPLUMINANCE = 23;
const int TOP_DOTPRODUCT3 = 24;
const int TOP_MULTIPLYADD = 25;
const int TOP_LERP = 26;

// D3DTA_*
const int TA_DIFFUSE = 0;
const int TA_CURRENT = 1;
const int TA_TEXTURE = 2;
const int TA_TFACTOR = 3;
const int TA_SPECULAR = 4;
const int TA_TEMP = 5;
const int TA_COMPLEMENT = 0x10;
const int TA_ALPHAREPLICATE = 0x20;
const int TA_SELECTMASK = 0x0f;

// D3DCMP_*
const int CMP_NEVER = 1;
const int CMP_LESS = 2;
const int CMP_EQUAL = 3;
const int CMP_LESSEQUAL = 4;
const int CMP_GREATER = 5;
const int CMP_NOTEQUAL = 6;
const int CMP_GREATEREQUAL = 7;

const int FOG_NONE = 0;
const int FOG_EXP = 1;
const int FOG_EXP2 = 2;
const int FOG_LINEAR = 3;

vec4 sample_stage(int stage, vec2 bump_offset) {
	vec4 coord = v_texcoord[stage];
	// D3DTTFF_PROJECTED: divide by the last used coordinate after interpolation.
	if ((u.stage_misc[stage].y & 256) != 0) {
		int count = u.stage_misc[stage].y & 0xff;
		float divisor = count >= 4 ? coord.w : (count == 3 ? coord.z : coord.y);
		coord.xy /= (abs(divisor) > 1e-6 ? divisor : 1.0);
	}
	// D3DRS_POINTSPRITEENABLE replaces every stage's coordinates with the position
	// within the expanded point, which is exactly gl_PointCoord (both are y-down
	// from the sprite's top-left corner). The uniform is only non-zero when a point
	// primitive is being rasterised, which is when gl_PointCoord is defined.
	vec2 uv = (u.point_size.w != 0.0 ? gl_PointCoord : coord.xy) + bump_offset;
	switch (stage) {
	case 0: return texture(u_texture[0], uv);
	case 1: return texture(u_texture[1], uv);
	case 2: return texture(u_texture[2], uv);
	case 3: return texture(u_texture[3], uv);
	case 4: return texture(u_texture[4], uv);
	case 5: return texture(u_texture[5], uv);
	case 6: return texture(u_texture[6], uv);
	default: return texture(u_texture[7], uv);
	}
}

vec4 select_arg(int arg, vec4 tex, vec4 current, vec4 temp) {
	int which = arg & TA_SELECTMASK;
	vec4 value;
	if (which == TA_TEXTURE) value = tex;
	else if (which == TA_CURRENT) value = current;
	else if (which == TA_TFACTOR) value = u.tfactor;
	else if (which == TA_SPECULAR) value = v_specular;
	else if (which == TA_TEMP) value = temp;
	else value = v_diffuse;

	if ((arg & TA_ALPHAREPLICATE) != 0) value = vec4(value.a);
	if ((arg & TA_COMPLEMENT) != 0) value = vec4(1.0) - value;
	return value;
}

// The colour half of a stage. Runs on vec3s; the alpha half runs the same maths on
// scalars, which is why the two are separate functions rather than one vec4 pass.
vec3 apply_color_op(int op, vec3 a1, vec3 a2, vec3 a0, float a1a, float texa, float curra) {
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
	case TOP_BLENDDIFFUSEALPHA: return mix(a2, a1, v_diffuse.a);
	case TOP_BLENDTEXTUREALPHA: return mix(a2, a1, texa);
	case TOP_BLENDFACTORALPHA: return mix(a2, a1, u.tfactor.a);
	case TOP_BLENDTEXTUREALPHAPM: return clamp(a1 + a2 * (1.0 - texa), 0.0, 1.0);
	case TOP_BLENDCURRENTALPHA: return mix(a2, a1, curra);
	case TOP_MODULATEALPHA_ADDCOLOR: return clamp(a1 + a1a * a2, 0.0, 1.0);
	case TOP_MODULATECOLOR_ADDALPHA: return clamp(a1 * a2 + a1a, 0.0, 1.0);
	case TOP_MODULATEINVALPHA_ADDCOLOR: return clamp((1.0 - a1a) * a2 + a1, 0.0, 1.0);
	case TOP_MODULATEINVCOLOR_ADDALPHA: return clamp((1.0 - a1) * a2 + a1a, 0.0, 1.0);
	// D3D8 defines DOTPRODUCT3 on signed values: each argument is biased from
	// [0,1] to [-1,1], the dot product is taken, saturated, and replicated.
	case TOP_DOTPRODUCT3: {
		float d = dot(a1 * 2.0 - 1.0, a2 * 2.0 - 1.0);
		return vec3(clamp(d, 0.0, 1.0));
	}
	case TOP_MULTIPLYADD: return clamp(a0 + a1 * a2, 0.0, 1.0);
	case TOP_LERP: return mix(a2, a1, a0);
	// BUMPENVMAP consumes the stage's texture as a perturbation for the *next*
	// stage and leaves the colour pipeline alone (handled by the caller).
	case TOP_BUMPENVMAP:
	case TOP_BUMPENVMAPLUMINANCE: return a2;
	default: return a1;
	}
}

float apply_alpha_op(int op, float a1, float a2, float a0, float texa, float curra) {
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
	case TOP_BLENDDIFFUSEALPHA: return mix(a2, a1, v_diffuse.a);
	case TOP_BLENDTEXTUREALPHA: return mix(a2, a1, texa);
	case TOP_BLENDFACTORALPHA: return mix(a2, a1, u.tfactor.a);
	case TOP_BLENDTEXTUREALPHAPM: return clamp(a1 + a2 * (1.0 - texa), 0.0, 1.0);
	case TOP_BLENDCURRENTALPHA: return mix(a2, a1, curra);
	case TOP_DOTPRODUCT3: {
		// DOTPRODUCT3 writes the same replicated scalar to colour and alpha.
		return a1;
	}
	case TOP_MULTIPLYADD: return clamp(a0 + a1 * a2, 0.0, 1.0);
	case TOP_LERP: return mix(a2, a1, a0);
	default: return a1;
	}
}

bool alpha_test_passes(float alpha) {
	float reference = u.misc.x;
	switch (u.flags.y) {
	case CMP_NEVER: return false;
	case CMP_LESS: return alpha < reference;
	case CMP_EQUAL: return alpha == reference;
	case CMP_LESSEQUAL: return alpha <= reference;
	case CMP_GREATER: return alpha > reference;
	case CMP_NOTEQUAL: return alpha != reference;
	case CMP_GREATEREQUAL: return alpha >= reference;
	default: return true;
	}
}

// --- ps.1.1 -----------------------------------------------------------------
// The token decoding is the same as fixedfunc.vert's; see the comment there.
int sh_opcode(int t) { return t & 0xffff; }
int sh_regnum(int t) { return t & 0x1fff; }
int sh_regtype(int t) { return (t >> 28) & 7; }
int sh_writemask(int t) { return (t >> 16) & 0xf; }
bool sh_saturates(int t) { return ((t >> 20) & 0xf) == 1; }
int sh_dstshift(int t) { int s = (t >> 24) & 0xf; return s > 7 ? s - 16 : s; }

const int REG_TEMP = 0;
const int REG_INPUT = 1;
const int REG_CONST = 2;
const int REG_TEXTURE = 3;

const int SIO_MOV = 1;
const int SIO_ADD = 2;
const int SIO_SUB = 3;
const int SIO_MAD = 4;
const int SIO_MUL = 5;
const int SIO_DP3 = 8;
const int SIO_DP4 = 9;
const int SIO_MIN = 10;
const int SIO_MAX = 11;
const int SIO_LRP = 18;
const int SIO_TEXCOORD = 64;
const int SIO_TEXKILL = 65;
const int SIO_TEX = 66;
const int SIO_CND = 80;

vec4 sh_swizzled(vec4 v, int token) {
	int s = (token >> 16) & 0xff;
	float c[4] = float[4](v.x, v.y, v.z, v.w);
	return vec4(c[s & 3], c[(s >> 2) & 3], c[(s >> 4) & 3], c[(s >> 6) & 3]);
}

// D3DSPSM_*, the source-register modifiers.
vec4 sh_modified(vec4 v, int token) {
	int m = (token >> 24) & 0xf;
	if (m == 1) return -v;
	if (m == 2) return v - 0.5;
	if (m == 3) return -(v - 0.5);
	if (m == 4) return 2.0 * v - 1.0;
	if (m == 5) return -(2.0 * v - 1.0);
	if (m == 6) return 1.0 - v;
	if (m == 7) return 2.0 * v;
	if (m == 8) return -2.0 * v;
	return v;
}

vec4 sh_shifted(vec4 v, int token) {
	int s = sh_dstshift(token);
	if (s > 0) return v * float(1 << s);
	if (s < 0) return v / float(1 << (-s));
	return v;
}

vec4 ps_source(int token, vec4 r[PS_TEMPS], vec4 t[PS_TEXTURES]) {
	int reg = sh_regnum(token);
	int type = sh_regtype(token);
	vec4 value = vec4(0.0);
	// v0/v1 are the interpolated diffuse and specular, i.e. what the cascade calls
	// D3DTA_DIFFUSE and D3DTA_SPECULAR.
	if (type == REG_TEMP) value = r[clamp(reg, 0, PS_TEMPS - 1)];
	else if (type == REG_INPUT) value = reg == 1 ? v_specular : v_diffuse;
	else if (type == REG_CONST) value = u.ps_constants[clamp(reg, 0, MAX_PS_CONSTANTS - 1)];
	else if (type == REG_TEXTURE) value = t[clamp(reg, 0, PS_TEXTURES - 1)];
	return sh_modified(sh_swizzled(value, token), token);
}

// Returns r0, which is ps.1.1's output register.
vec4 run_pixel_shader() {
	vec4 r[PS_TEMPS];
	for (int i = 0; i < PS_TEMPS; ++i) r[i] = vec4(0.0);
	vec4 t[PS_TEXTURES];
	for (int i = 0; i < PS_TEXTURES; ++i) t[i] = vec4(0.0);

	int count = min(u.shader_counts.x, MAX_SHADER_INSTRUCTIONS);
	for (int i = 0; i < count; ++i) {
		ivec4 low = u.ps_program[i * 2];
		ivec4 high = u.ps_program[i * 2 + 1];
		int op = sh_opcode(low.x);
		int dst = low.y;
		int reg = sh_regnum(dst);
		int type = sh_regtype(dst);

		if (op == SIO_TEX || op == SIO_TEXCOORD) {
			// `tex tN` samples stage N with the interpolated coordinate set N;
			// `texcoord tN` takes the coordinate itself without sampling.
			int stage = clamp(reg, 0, PS_TEXTURES - 1);
			t[stage] = op == SIO_TEX ? sample_stage(stage, vec2(0.0))
			                         : vec4(v_texcoord[stage].xyz, 1.0);
			continue;
		}
		if (op == SIO_TEXKILL) {
			// D3D8 kills the pixel when any of the first three coordinate
			// components is negative.
			if (any(lessThan(v_texcoord[clamp(reg, 0, PS_TEXTURES - 1)].xyz, vec3(0.0))))
				discard;
			continue;
		}

		vec4 s0 = ps_source(low.z, r, t);
		vec4 s1 = ps_source(low.w, r, t);
		vec4 s2 = ps_source(high.x, r, t);
		vec4 value = vec4(0.0);
		switch (op) {
		case SIO_MOV: value = s0; break;
		case SIO_ADD: value = s0 + s1; break;
		case SIO_SUB: value = s0 - s1; break;
		case SIO_MUL: value = s0 * s1; break;
		case SIO_MAD: value = s0 * s1 + s2; break;
		case SIO_DP3: value = vec4(dot(s0.xyz, s1.xyz)); break;
		case SIO_DP4: value = vec4(dot(s0, s1)); break;
		case SIO_MIN: value = min(s0, s1); break;
		case SIO_MAX: value = max(s0, s1); break;
		case SIO_LRP: value = mix(s2, s1, s0); break;
		// ps.1.1's cnd compares src0.a against 0.5 and takes src1 when above.
		case SIO_CND: value = s0.a > 0.5 ? s1 : s2; break;
		default: break;
		}

		value = sh_shifted(value, dst);
		// ps.1.1 registers are fixed-point: the result clamps to [-1,1], and to
		// [0,1] when the instruction carries _sat.
		value = sh_saturates(dst) ? clamp(value, 0.0, 1.0) : clamp(value, -1.0, 1.0);
		int mask = sh_writemask(dst);
		vec4 target = type == REG_TEXTURE ? t[clamp(reg, 0, PS_TEXTURES - 1)]
		                                  : r[clamp(reg, 0, PS_TEMPS - 1)];
		if ((mask & 1) != 0) target.x = value.x;
		if ((mask & 2) != 0) target.y = value.y;
		if ((mask & 4) != 0) target.z = value.z;
		if ((mask & 8) != 0) target.w = value.w;
		if (type == REG_TEXTURE) t[clamp(reg, 0, PS_TEXTURES - 1)] = target;
		else r[clamp(reg, 0, PS_TEMPS - 1)] = target;
	}
	return r[0];
}

float pixel_fog_factor(float d) {
	float start = u.fog_params.x;
	float end = u.fog_params.y;
	float density = u.fog_params.z;
	switch (u.flags2.z) {
	case FOG_LINEAR: {
		float range = end - start;
		if (abs(range) < 1e-6) return d >= end ? 0.0 : 1.0;
		return clamp((end - d) / range, 0.0, 1.0);
	}
	case FOG_EXP: return clamp(exp(-d * density), 0.0, 1.0);
	case FOG_EXP2: return clamp(exp(-(d * density) * (d * density)), 0.0, 1.0);
	default: return 1.0;
	}
}

void main() {
	// A pixel shader replaces the whole texture-stage cascade, exactly as it does in
	// D3D8: SetTextureStageState is ignored while one is bound, but the alpha test
	// and fog are still fixed-function state and still apply. D3DRS_SPECULARENABLE
	// does not, because the shader reads v1 itself.
	if (u.shader_counts.x != 0) {
		vec4 shaded = run_pixel_shader();
		if (u.flags.x != 0 && !alpha_test_passes(shaded.a)) discard;
		if (u.flags.w != 0) {
			float factor = u.flags2.z != FOG_NONE ? pixel_fog_factor(v_misc.y) : v_misc.x;
			shaded.rgb = mix(u.fog_color.rgb, shaded.rgb, factor);
		}
		out_color = shaded;
		return;
	}

	// D3D8 seeds CURRENT with the (lit) vertex diffuse colour before stage 0.
	vec4 current = v_diffuse;
	vec4 temp = vec4(0.0);
	vec2 bump_offset = vec2(0.0);
	float bump_luminance = 1.0;

	for (int stage = 0; stage < MAX_STAGES; ++stage) {
		int color_op = u.stage_color[stage].x;
		// D3D8: the first stage with COLOROP == DISABLE ends the cascade; later
		// stages are ignored even if they are configured.
		if (color_op == TOP_DISABLE) break;

		vec4 tex = vec4(1.0);
		if (u.stage_misc[stage].z != 0) {
			tex = sample_stage(stage, bump_offset);
			tex.rgb *= bump_luminance;
		}
		bump_offset = vec2(0.0);
		bump_luminance = 1.0;

		if (color_op == TOP_BUMPENVMAP || color_op == TOP_BUMPENVMAPLUMINANCE) {
			// du,dv come from the stage's texture, which D3D8 requires to be one of
			// the signed formats (V8U8/L6V5U5/X8L8V8U8). The backend uploads those as
			// SNORM, so the sample is already in [-1,1]: biasing it here would halve
			// the perturbation and offset it.
			vec2 delta = tex.rg;
			bump_offset = vec2(u.stage_bump[stage].x * delta.x + u.stage_bump[stage].z * delta.y,
			                   u.stage_bump[stage].y * delta.x + u.stage_bump[stage].w * delta.y);
			if (color_op == TOP_BUMPENVMAPLUMINANCE) {
				bump_luminance = clamp(tex.b * u.stage_bump_lum[stage].x +
				                           u.stage_bump_lum[stage].y,
				                       0.0, 1.0);
			}
			continue; // CURRENT is untouched by a bump stage
		}

		vec4 c1 = select_arg(u.stage_color[stage].y, tex, current, temp);
		vec4 c2 = select_arg(u.stage_color[stage].z, tex, current, temp);
		vec4 c0 = select_arg(u.stage_color[stage].w, tex, current, temp);

		int alpha_op = u.stage_alpha[stage].x;
		vec4 a1 = select_arg(u.stage_alpha[stage].y, tex, current, temp);
		vec4 a2 = select_arg(u.stage_alpha[stage].z, tex, current, temp);
		vec4 a0 = select_arg(u.stage_alpha[stage].w, tex, current, temp);

		// The alpha the previous stage produced: D3DTOP_BLENDCURRENTALPHA blends
		// against it, and it survives a stage whose ALPHAOP is DISABLE.
		float previous_alpha = current.a;
		vec3 color = apply_color_op(color_op, c1.rgb, c2.rgb, c0.rgb, c1.a, tex.a, previous_alpha);
		// D3DTOP_DISABLE on the alpha channel alone means the alpha of the previous
		// stage survives, which is how the engine's TEXTURING_DISABLE presets keep
		// the vertex alpha alive through an otherwise colour-only cascade.
		float alpha = (alpha_op == TOP_DISABLE)
		                  ? previous_alpha
		                  : apply_alpha_op(alpha_op, a1.a, a2.a, a0.a, tex.a, previous_alpha);
		if (color_op == TOP_DOTPRODUCT3) alpha = color.r;

		// D3DTSS_RESULTARG: a stage can write to TEMP instead of CURRENT. The engine
		// never sets it (the only call site is commented out in dx8wrapper.cpp:3838),
		// so this is here to keep the cascade faithful, not because it is exercised.
		if ((u.stage_misc[stage].w & TA_SELECTMASK) == TA_TEMP) {
			temp = vec4(color, alpha);
		} else {
			current = vec4(color, alpha);
		}
	}

	// D3DRS_SPECULARENABLE adds the specular colour after the cascade, not before.
	if (u.flags2.w != 0) current.rgb = clamp(current.rgb + v_specular.rgb, 0.0, 1.0);

	if (u.flags.x != 0 && !alpha_test_passes(current.a)) discard;

	if (u.flags.w != 0) {
		// Table (pixel) fog wins over vertex fog when both are set, which is D3D8's
		// documented precedence. The engine sets FOGTABLEMODE=NONE at device init,
		// so in practice the vertex factor computed in the vertex shader is used.
		float factor = u.flags2.z != FOG_NONE ? pixel_fog_factor(v_misc.y) : v_misc.x;
		current.rgb = mix(u.fog_color.rgb, current.rgb, factor);
	}

	out_color = current;
}
