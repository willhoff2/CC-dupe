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

layout(set = 0, binding = 0, std140) uniform Draw {
	mat4 wvp;
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
} u;

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
}
