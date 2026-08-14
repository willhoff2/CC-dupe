#version 450
// D3D8 fixed-function vertex pipeline, interpreted at runtime.
//
// Vulkan has no fixed-function T&L, so everything D3D8's SetTransform/SetLight/
// SetMaterial/D3DRS_FOG* would have configured has to be re-implemented here and
// driven from a uniform block. The layout of that block must stay identical to
// spike::DrawUniforms in src/state_translate.h and to the copy in fixedfunc.frag.
//
// What is modelled, and why (measurements in
// docs/porting/fixed-function-measurements.md):
//   - D3DFVF_XYZRHW passthrough: 6 engine FVFs are pretransformed (UI, shadows)
//   - world/view/projection: DX8Wrapper::Set_Transform, 3 transform types
//   - directional/point/spot lighting: DX8Wrapper::Set_Light maps all three
//   - D3DRS_AMBIENT, material colour sources, D3DRS_COLORVERTEX, specular
//   - vertex fog: the engine sets FOGTABLEMODE=NONE, FOGVERTEXMODE=LINEAR once
//   - texture coordinate selection, D3DTSS_TEXCOORDINDEX generators, D3DTS_TEXTUREn

#define MAX_STAGES 8
#define MAX_LIGHTS 4
// Must match kMaxShaderInstructions/kMaxVertexShaderConstants/kMaxClipPlanes and
// friends in src/d3d8_subset.h.
#define MAX_SHADER_INSTRUCTIONS 32
#define MAX_PS_CONSTANTS 8
#define MAX_VS_CONSTANTS 96
#define MAX_VS_INPUTS 16
#define MAX_CLIP_PLANES 6
#define VS_TEMPS 12

layout(location = 0) in vec4 a_position;
layout(location = 1) in vec3 a_blendweight;
layout(location = 2) in uvec4 a_blendindices;
layout(location = 3) in vec3 a_normal;
layout(location = 4) in vec4 a_diffuse;
layout(location = 5) in vec4 a_specular;
layout(location = 6) in vec4 a_texcoord0;
layout(location = 7) in vec4 a_texcoord1;
layout(location = 8) in vec4 a_texcoord2;
layout(location = 9) in vec4 a_texcoord3;

layout(location = 0) out vec4 v_diffuse;
layout(location = 1) out vec4 v_specular;
layout(location = 2) out vec4 v_misc; // x: fog factor, y: camera-space fog distance
layout(location = 3) out vec4 v_texcoord[MAX_STAGES];

// D3D8's user clip planes, which are a device feature in Vulkan rather than a
// state: the array is always declared and a disabled plane is written as "inside".
out float gl_ClipDistance[MAX_CLIP_PLANES];

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
	vec4 point_size;  // size, min, max, D3DRS_POINTSPRITEENABLE
	vec4 point_scale; // D3DRS_POINTSCALE_A/B/C, D3DRS_POINTSCALEENABLE

	// ps.1.1/vs.1.1 token streams, two ivec4 per instruction.
	ivec4 ps_program[MAX_SHADER_INSTRUCTIONS * 2];
	ivec4 vs_program[MAX_SHADER_INSTRUCTIONS * 2];
	vec4 ps_constants[MAX_PS_CONSTANTS];
	vec4 vs_constants[MAX_VS_CONSTANTS];
	ivec4 vs_inputs[MAX_VS_INPUTS / 4];
	ivec4 shader_counts; // ps instructions, vs instructions, -, -

	vec4 clip_planes[MAX_CLIP_PLANES];
	ivec4 clip_enable; // x: D3DRS_CLIPPLANEENABLE
} u;

// --- D3D8 shader token decoding ---------------------------------------------
// The same bit fields d3d8types.h defines and src/d3d8_subset.h repeats. Shared
// verbatim with fixedfunc.frag, which decodes the pixel half.
int sh_opcode(int t) { return t & 0xffff; }
int sh_regnum(int t) { return t & 0x1fff; }
int sh_regtype(int t) { return (t >> 28) & 7; }
int sh_writemask(int t) { return (t >> 16) & 0xf; }
bool sh_saturates(int t) { return ((t >> 20) & 0xf) == 1; }
int sh_dstshift(int t) { int s = (t >> 24) & 0xf; return s > 7 ? s - 16 : s; }
bool sh_relative(int t) { return (t & 0x2000) != 0; }

// D3DSHADER_PARAM_REGISTER_TYPE
const int REG_TEMP = 0;
const int REG_INPUT = 1;
const int REG_CONST = 2;
const int REG_ADDR = 3;
const int REG_RASTOUT = 4;
const int REG_ATTROUT = 5;
const int REG_TEXCRDOUT = 6;

// D3DSHADER_INSTRUCTION_OPCODE_TYPE
const int SIO_MOV = 1;
const int SIO_ADD = 2;
const int SIO_SUB = 3;
const int SIO_MAD = 4;
const int SIO_MUL = 5;
const int SIO_RCP = 6;
const int SIO_RSQ = 7;
const int SIO_DP3 = 8;
const int SIO_DP4 = 9;
const int SIO_MIN = 10;
const int SIO_MAX = 11;
const int SIO_SLT = 12;
const int SIO_SGE = 13;
const int SIO_EXP = 14;
const int SIO_LOG = 15;
const int SIO_LIT = 16;
const int SIO_DST = 17;
const int SIO_LRP = 18;
const int SIO_FRC = 19;
const int SIO_M4x4 = 20;
const int SIO_M4x3 = 21;
const int SIO_M3x4 = 22;
const int SIO_M3x3 = 23;
const int SIO_M3x2 = 24;
const int SIO_EXPP = 78;
const int SIO_LOGP = 79;

vec4 sh_swizzled(vec4 v, int token) {
	int s = (token >> 16) & 0xff;
	float c[4] = float[4](v.x, v.y, v.z, v.w);
	return vec4(c[s & 3], c[(s >> 2) & 3], c[(s >> 4) & 3], c[(s >> 6) & 3]);
}

// D3DSPSM_*, the source-register modifiers. DZ and DW are projective divides that
// only appear in ps.1.1 texture addressing; they pass through unmodified.
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

// --- vs.1.1 -----------------------------------------------------------------
// The v-register the declaration mapped onto a vertex element, resolved to the
// element by the backend (DrawUniforms::vs_inputs).
vec4 vs_input(int reg) {
	int element = u.vs_inputs[reg >> 2][reg & 3];
	switch (element) {
	case 0: return vec4(a_position.xyz, 1.0);
	case 1: return vec4(a_blendweight, 0.0);
	case 2: return vec4(a_blendindices);
	case 3: return vec4(a_normal, 0.0);
	case 4: return a_diffuse;
	case 5: return a_specular;
	case 6: return a_texcoord0;
	case 7: return a_texcoord1;
	case 8: return a_texcoord2;
	case 9: return a_texcoord3;
	default: return vec4(0.0);
	}
}

vec4 vs_source(int token, vec4 r[VS_TEMPS], int a0) {
	int reg = sh_regnum(token);
	int type = sh_regtype(token);
	vec4 value = vec4(0.0);
	if (type == REG_TEMP) {
		value = r[clamp(reg, 0, VS_TEMPS - 1)];
	} else if (type == REG_INPUT) {
		value = vs_input(clamp(reg, 0, MAX_VS_INPUTS - 1));
	} else if (type == REG_CONST) {
		// vs.1.1's c[a0.x + n]: the address register is implicit, so the relative bit
		// is all there is to read.
		int index = sh_relative(token) ? reg + a0 : reg;
		value = u.vs_constants[clamp(index, 0, MAX_VS_CONSTANTS - 1)];
	}
	return sh_modified(sh_swizzled(value, token), token);
}

void run_vertex_shader() {
	vec4 r[VS_TEMPS];
	for (int i = 0; i < VS_TEMPS; ++i) r[i] = vec4(0.0);
	vec4 out_position = vec4(0.0, 0.0, 0.0, 1.0);
	vec4 out_diffuse = vec4(1.0);
	vec4 out_specular = vec4(0.0);
	vec4 out_texcoord[4] = vec4[4](vec4(0.0), vec4(0.0), vec4(0.0), vec4(0.0));
	int a0 = 0;

	int count = min(u.shader_counts.y, MAX_SHADER_INSTRUCTIONS);
	for (int i = 0; i < count; ++i) {
		ivec4 low = u.vs_program[i * 2];
		ivec4 high = u.vs_program[i * 2 + 1];
		int op = sh_opcode(low.x);
		int dst = low.y;
		vec4 s0 = vs_source(low.z, r, a0);
		vec4 s1 = vs_source(low.w, r, a0);
		vec4 s2 = vs_source(high.x, r, a0);
		vec4 value = vec4(0.0);
		switch (op) {
		case SIO_MOV: value = s0; break;
		case SIO_ADD: value = s0 + s1; break;
		case SIO_SUB: value = s0 - s1; break;
		case SIO_MUL: value = s0 * s1; break;
		case SIO_MAD: value = s0 * s1 + s2; break;
		case SIO_RCP: value = vec4(s0.x != 0.0 ? 1.0 / s0.x : 0.0); break;
		case SIO_RSQ: value = vec4(s0.x != 0.0 ? inversesqrt(abs(s0.x)) : 0.0); break;
		case SIO_DP3: value = vec4(dot(s0.xyz, s1.xyz)); break;
		case SIO_DP4: value = vec4(dot(s0, s1)); break;
		case SIO_MIN: value = min(s0, s1); break;
		case SIO_MAX: value = max(s0, s1); break;
		case SIO_SLT: value = vec4(lessThan(s0, s1)); break;
		case SIO_SGE: value = vec4(greaterThanEqual(s0, s1)); break;
		case SIO_EXP: case SIO_EXPP: value = vec4(exp2(s0.x)); break;
		case SIO_LOG: case SIO_LOGP:
			value = vec4(abs(s0.x) > 0.0 ? log2(abs(s0.x)) : -3.4e38);
			break;
		case SIO_FRC: value = fract(s0); break;
		case SIO_LRP: value = mix(s2, s1, s0); break;
		// D3D8's lighting helpers, defined exactly as the spec writes them.
		case SIO_LIT: {
			float power = clamp(s0.w, -128.0 + 1e-6, 128.0 - 1e-6);
			value = vec4(1.0, max(s0.x, 0.0),
			             (s0.x > 0.0 && s0.y > 0.0) ? pow(s0.y, power) : 0.0, 1.0);
			break;
		}
		case SIO_DST:
			value = vec4(1.0, s0.y * s1.y, s0.z, s1.w);
			break;
		case SIO_M4x4: case SIO_M4x3: case SIO_M3x4: case SIO_M3x3: case SIO_M3x2: {
			// The matrix is consecutive registers starting at source 1, and the
			// operand count says how many rows and how wide each dot product is.
			int rows = (op == SIO_M4x4 || op == SIO_M3x4) ? 4
			          : (op == SIO_M3x2 ? 2 : 3);
			bool three = (op == SIO_M3x4 || op == SIO_M3x3 || op == SIO_M3x2);
			value = vec4(0.0, 0.0, 0.0, 1.0);
			for (int row = 0; row < rows; ++row) {
				vec4 m = vs_source(low.w + row, r, a0);
				float d = three ? dot(s0.xyz, m.xyz) : dot(s0, m);
				if (row == 0) value.x = d;
				else if (row == 1) value.y = d;
				else if (row == 2) value.z = d;
				else value.w = d;
			}
			break;
		}
		default: break;
		}

		value = sh_shifted(value, dst);
		if (sh_saturates(dst)) value = clamp(value, 0.0, 1.0);
		int mask = sh_writemask(dst);
		int reg = sh_regnum(dst);
		int type = sh_regtype(dst);
		if (type == REG_ADDR) {
			// `mov a0.x, v1`: D3D8 rounds the value to the nearest integer.
			a0 = int(round(value.x));
			continue;
		}
		vec4 target;
		if (type == REG_TEMP) target = r[clamp(reg, 0, VS_TEMPS - 1)];
		else if (type == REG_RASTOUT) target = out_position;
		else if (type == REG_ATTROUT) target = reg == 1 ? out_specular : out_diffuse;
		else if (type == REG_TEXCRDOUT) target = out_texcoord[clamp(reg, 0, 3)];
		else target = vec4(0.0);
		if ((mask & 1) != 0) target.x = value.x;
		if ((mask & 2) != 0) target.y = value.y;
		if ((mask & 4) != 0) target.z = value.z;
		if ((mask & 8) != 0) target.w = value.w;
		if (type == REG_TEMP) r[clamp(reg, 0, VS_TEMPS - 1)] = target;
		else if (type == REG_RASTOUT) out_position = target;
		else if (type == REG_ATTROUT) { if (reg == 1) out_specular = target; else out_diffuse = target; }
		else if (type == REG_TEXCRDOUT) out_texcoord[clamp(reg, 0, 3)] = target;
	}

	// oPos is in D3D clip space, where +y is up; Vulkan's is +y down. The
	// fixed-function path folds this flip into wvp, but a vertex shader builds its own
	// projection out of constants, so the flip has to happen here instead.
	gl_Position = vec4(out_position.x, -out_position.y, out_position.z, out_position.w);
	v_diffuse = out_diffuse;
	v_specular = out_specular;
	for (int i = 0; i < 4; ++i) v_texcoord[i] = out_texcoord[i];
	for (int i = 4; i < MAX_STAGES; ++i) v_texcoord[i] = vec4(0.0);
	// oFog is not written by any shader the engine ships, so fog stays off for the
	// shader path rather than reusing the fixed-function factor.
	v_misc = vec4(1.0, 0.0, 0.0, 0.0);
	gl_PointSize = 1.0;
}

// D3DTSS_TEXTURETRANSFORMFLAGS
const int TTFF_DISABLE = 0;
const int TTFF_PROJECTED = 256;
// D3DTSS_TEXCOORDINDEX generators
const int TCI_CAMERASPACENORMAL = 0x10000;
const int TCI_CAMERASPACEPOSITION = 0x20000;
const int TCI_CAMERASPACEREFLECTIONVECTOR = 0x30000;
// D3DMATERIALCOLORSOURCE
const int MCS_MATERIAL = 0;
const int MCS_COLOR1 = 1;
const int MCS_COLOR2 = 2;
// D3DLIGHTTYPE
const int LIGHT_POINT = 1;
const int LIGHT_SPOT = 2;
const int LIGHT_DIRECTIONAL = 3;
// D3DFOGMODE
const int FOG_NONE = 0;
const int FOG_EXP = 1;
const int FOG_EXP2 = 2;
const int FOG_LINEAR = 3;

vec4 material_source(int source, vec4 material) {
	// D3DRS_COLORVERTEX gates the vertex-colour sources entirely.
	if (u.flags3.x == 0) return material;
	if (source == MCS_COLOR1) return a_diffuse;
	if (source == MCS_COLOR2) return a_specular;
	return material;
}

vec4 fetch_texcoord(int set_index) {
	if (set_index == 1) return a_texcoord1;
	if (set_index == 2) return a_texcoord2;
	if (set_index == 3) return a_texcoord3;
	return a_texcoord0;
}

float fog_factor(float d) {
	int mode = u.flags2.y;
	float start = u.fog_params.x;
	float end = u.fog_params.y;
	float density = u.fog_params.z;
	if (mode == FOG_LINEAR) {
		// D3D8: f = (end - d) / (end - start), saturated. end == start is degenerate.
		float range = end - start;
		if (abs(range) < 1e-6) return d >= end ? 0.0 : 1.0;
		return clamp((end - d) / range, 0.0, 1.0);
	}
	if (mode == FOG_EXP) return clamp(exp(-d * density), 0.0, 1.0);
	if (mode == FOG_EXP2) return clamp(exp(-(d * density) * (d * density)), 0.0, 1.0);
	return 1.0;
}

void main() {
	// D3DFVF_XYZBn: the engine's skinned meshes carry weights and packed bone
	// indices in the vertex. They are declared so the attribute list and the stride
	// match the FVF, but no bone palette is applied: the engine sets no bone
	// matrices through the D3D8 surface, so there is nothing to apply.
	vec4 position = vec4(a_position.xyz, 1.0) +
	                0.0 * vec4(a_blendweight, float(a_blendindices.x));

	vec4 camera_position = u.world_view * position;

	if (u.flags2.x != 0) {
		// D3DFVF_XYZRHW: x,y are already in screen pixels, z is a depth value in
		// [0,1] and w holds 1/w. Map straight to Vulkan clip space, keeping the
		// perspective divide a no-op.
		vec2 ndc = vec2(a_position.x / u.misc.y * 2.0 - 1.0,
		                a_position.y / u.misc.z * 2.0 - 1.0);
		gl_Position = vec4(ndc, a_position.z, 1.0);
		camera_position = vec4(a_position.xy, a_position.z, 1.0);
	} else {
		gl_Position = u.wvp * position;
	}

	// Normals go to camera space through the inverse transpose, so non-uniform
	// scale in the world matrix does not tilt them. D3DRS_NORMALIZENORMALS decides
	// whether D3D8 renormalises afterwards; when it is off, a scaled normal scales
	// the lighting, which some engine content relies on.
	vec3 normal = transpose(inverse(mat3(u.world_view))) * a_normal;
	if (u.flags3.y != 0) normal = normalize(normal);

	// --- lighting -----------------------------------------------------------
	vec4 diffuse_material = material_source(u.sources.x, u.material_diffuse);
	vec4 specular_material = material_source(u.sources.y, u.material_specular);
	vec4 ambient_material = material_source(u.sources.z, u.material_ambient);
	vec4 emissive_material = material_source(u.sources.w, u.material_emissive);

	if (u.flags.z == 0) {
		// D3DRS_LIGHTING off: the vertex colours pass straight through. With no
		// diffuse in the FVF the dummy attribute supplies opaque white, which is
		// what D3D8 substitutes.
		v_diffuse = a_diffuse;
		v_specular = u.flags2.w != 0 ? a_specular : vec4(0.0);
	} else {
		vec3 ambient_sum = u.global_ambient.rgb;
		vec3 diffuse_sum = vec3(0.0);
		vec3 specular_sum = vec3(0.0);
		// D3DRS_LOCALVIEWER off means D3D8 uses a fixed camera-space view direction
		// of (0,0,1) for the halfway vector instead of the per-vertex one.
		vec3 to_eye = u.flags3.z != 0 ? normalize(-camera_position.xyz) : vec3(0.0, 0.0, 1.0);

		for (int i = 0; i < MAX_LIGHTS; ++i) {
			int type = int(u.light_position[i].w);
			if (type == 0) continue;

			vec3 to_light;
			float attenuation = 1.0;
			float spot = 1.0;
			if (type == LIGHT_DIRECTIONAL) {
				to_light = normalize(-(mat3(u.view) * u.light_direction[i].xyz));
			} else {
				vec3 light_position = (u.view * vec4(u.light_position[i].xyz, 1.0)).xyz;
				vec3 delta = light_position - camera_position.xyz;
				float distance = length(delta);
				if (distance > u.light_direction[i].w) continue; // outside D3DLIGHT8::Range
				to_light = distance > 0.0 ? delta / distance : vec3(0.0, 0.0, 1.0);
				float a0 = u.light_attenuation[i].x;
				float a1 = u.light_attenuation[i].y;
				float a2 = u.light_attenuation[i].z;
				float denominator = a0 + a1 * distance + a2 * distance * distance;
				attenuation = denominator > 0.0 ? min(1.0, 1.0 / denominator) : 1.0;
				if (type == LIGHT_SPOT) {
					// D3D8: rho is the cosine between the light direction and the
					// direction to the vertex; the falloff is applied between the
					// inner (theta) and outer (phi) cone cosines.
					vec3 spot_direction = normalize(mat3(u.view) * u.light_direction[i].xyz);
					float rho = dot(spot_direction, -to_light);
					float cos_theta = u.light_spot[i].x;
					float cos_phi = u.light_spot[i].y;
					if (rho <= cos_phi) {
						spot = 0.0;
					} else if (rho > cos_theta) {
						spot = 1.0;
					} else {
						spot = pow((rho - cos_phi) / max(cos_theta - cos_phi, 1e-6),
						           u.light_attenuation[i].w);
					}
				}
			}

			float scale = attenuation * spot;
			ambient_sum += u.light_ambient[i].rgb * scale;
			float n_dot_l = max(dot(normal, to_light), 0.0);
			diffuse_sum += u.light_diffuse[i].rgb * n_dot_l * scale;
			if (u.flags2.w != 0 && u.material_power.x > 0.0 && n_dot_l > 0.0) {
				vec3 halfway = normalize(to_light + to_eye);
				specular_sum += u.light_specular[i].rgb *
				                pow(max(dot(normal, halfway), 0.0), u.material_power.x) * scale;
			}
		}

		vec3 lit = ambient_material.rgb * ambient_sum + diffuse_material.rgb * diffuse_sum +
		           emissive_material.rgb;
		v_diffuse = vec4(clamp(lit, 0.0, 1.0), diffuse_material.a);
		v_specular = vec4(clamp(specular_material.rgb * specular_sum, 0.0, 1.0), 0.0);
	}

	// --- point size ---------------------------------------------------------
	// D3D8 expands a D3DPT_POINTLIST vertex into a screen-space square; Vulkan does
	// the same from gl_PointSize, so the whole of D3DRS_POINTSIZE* is this:
	//   scaling off: the size is already in pixels
	//   scaling on:  size * viewport_height * sqrt(1 / (A + B*d + C*d*d)), d being the
	//                camera-space distance, which is D3D8's documented formula
	// The clamp order is D3D8's: the scaled size is clamped to [MIN, MAX].
	{
		float size = u.point_size.x;
		if (u.point_scale.w != 0.0) {
			float d = length(camera_position.xyz);
			float denominator = u.point_scale.x + u.point_scale.y * d +
			                    u.point_scale.z * d * d;
			size *= u.misc.z * sqrt(1.0 / max(denominator, 1e-6));
		}
		gl_PointSize = clamp(size, max(u.point_size.y, 1.0), u.point_size.z);
	}

	// --- fog ----------------------------------------------------------------
	// D3DRS_RANGEFOGENABLE swaps the camera-space z for the true distance, which
	// removes the "fog changes when you turn the camera" artefact.
	float fog_distance = u.flags3.w != 0 ? length(camera_position.xyz) : abs(camera_position.z);
	v_misc = vec4(u.flags.w != 0 ? fog_factor(fog_distance) : 1.0, fog_distance, 0.0, 0.0);

	// --- texture coordinates -------------------------------------------------
	for (int stage = 0; stage < MAX_STAGES; ++stage) {
		int index = u.stage_misc[stage].x;
		int generator = index & 0x30000;
		vec4 coord;
		if (generator == TCI_CAMERASPACENORMAL) {
			coord = vec4(normal, 1.0);
		} else if (generator == TCI_CAMERASPACEPOSITION) {
			coord = vec4(camera_position.xyz, 1.0);
		} else if (generator == TCI_CAMERASPACEREFLECTIONVECTOR) {
			coord = vec4(reflect(normalize(camera_position.xyz), normal), 1.0);
		} else {
			coord = fetch_texcoord(index & 0xffff);
		}

		int transform_flags = u.stage_misc[stage].y;
		if ((transform_flags & 0xff) != TTFF_DISABLE && stage < 4) {
			coord = u.tex_matrix[stage] * coord;
		}
		// The projective divide is deliberately left to the fragment shader: D3D8
		// divides after interpolation, and doing it here would make a projected
		// texture affine across the triangle.
		v_texcoord[stage] = coord;
	}

	// A vertex shader replaces all of the above, which is what D3D8 does too: with
	// one set, none of SetTransform/SetLight/SetMaterial affects the vertex.
	if (u.shader_counts.y != 0) run_vertex_shader();

	// --- user clip planes ---------------------------------------------------
	// D3D8's planes are in world space for fixed-function vertices, and the plane
	// equation is the same one Vulkan's clip distance uses: keep where dot >= 0.
	vec4 world_position = u.world * position;
	for (int i = 0; i < MAX_CLIP_PLANES; ++i) {
		bool enabled = (u.clip_enable.x & (1 << i)) != 0;
		gl_ClipDistance[i] = enabled ? dot(u.clip_planes[i], world_position) : 1.0;
	}
}
