/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// Loads a retail .w3d hierarchical model into plain structs.
//
// The chunk container is read by the engine's own ChunkLoadClass
// (Core/Libraries/Source/WWVegas/WWLib/chunkio.cpp) and every on-disk record is read into the
// engine's own struct from w3d_file.h, so the field order and widths are the engine's, not a
// transcription. The hierarchy is loaded by the engine's own HTreeClass::Load_W3D and posed
// by HTreeClass::Base_Update.
//
// What is *not* the engine's: the per-mesh chunk traversal. MeshModelClass::Load_W3D --
// the function that would normally do this -- cannot be compiled natively because
// meshmdl.h -> dx8wrapper.h -> <d3d8.h>. See docs/porting/native-model-render.md.
//
// This header stays free of engine types so the renderer translation unit, which needs the
// Vulkan headers and must not be compiled with the engine's Win32 shims, can include it.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace zh
{

// One texture stage of one material pass.
struct PassStage
{
	std::string texture_name;   // as referenced by the .w3d, usually "*.tga"
	bool has_own_texcoords = false;
};

struct MaterialPass
{
	std::vector<PassStage> stages;
	std::string vertex_material_name;
	float ambient[3] = {0.0f, 0.0f, 0.0f};
	float diffuse[3] = {1.0f, 1.0f, 1.0f};
	float specular[3] = {0.0f, 0.0f, 0.0f};
	float emissive[3] = {0.0f, 0.0f, 0.0f};
	float opacity = 1.0f;
	float shininess = 0.0f;
	uint32_t vertex_material_attributes = 0;

	// W3dShaderStruct fields, kept in W3D enum space (see w3d_file.h).
	uint8_t depth_compare = 3; // W3DSHADER_DEPTHCOMPARE_PASS_LEQUAL
	uint8_t depth_mask = 1;    // write enable
	uint8_t src_blend = 1;     // ONE
	uint8_t dest_blend = 0;    // ZERO
	uint8_t alpha_test = 0;
	uint8_t pri_gradient = 1;
	uint8_t texturing = 1;
	bool shader_present = false;

	// Vertex-material / shader / texture id arrays as stored: either one entry for the whole
	// mesh or one per vertex (materials) / per triangle (shaders, textures). Recorded so the
	// cross-check can say whether the mesh really is single-material.
	uint32_t vertex_material_id_count = 0;
	uint32_t shader_id_count = 0;
	uint32_t texture_id_count = 0;
	bool per_vertex_dcg = false;
};

struct SubMesh
{
	std::string name;
	std::string container_name;
	uint32_t attributes = 0;
	uint32_t version = 0;

	// Straight out of W3D_CHUNK_MESH_HEADER3, before any geometry is read: the independent
	// yardstick the loaded arrays are checked against.
	uint32_t declared_triangles = 0;
	uint32_t declared_vertices = 0;
	uint32_t declared_materials = 0;
	uint32_t vertex_channels = 0;
	uint32_t face_channels = 0;
	int32_t sort_level = 0;
	float declared_min[3] = {0.0f, 0.0f, 0.0f};
	float declared_max[3] = {0.0f, 0.0f, 0.0f};
	float declared_sphere_center[3] = {0.0f, 0.0f, 0.0f};
	float declared_sphere_radius = 0.0f;

	std::vector<float> positions; // 3 per vertex, object space
	std::vector<float> normals;   // 3 per vertex
	std::vector<float> texcoords; // 2 per vertex, from pass 0 stage 0; empty when absent
	std::vector<uint32_t> indices; // 3 per triangle
	std::vector<float> face_normals; // 3 per triangle, as stored in W3dTriStruct
	std::vector<uint32_t> shade_indices;
	std::vector<uint8_t> vertex_influences; // bone index per vertex for skins (uint16 stored)

	std::vector<MaterialPass> passes;

	// Placement, from the HLOD sub-object table plus the hierarchy pose.
	int bone_index = 0;
	bool in_rendered_lod = true;
	float world_transform[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}; // Matrix3D rows

	bool hidden() const;
	bool two_sided() const;
	bool is_skin() const;
	bool is_camera_aligned() const;
	uint32_t geometry_type() const;
};

struct LodArray
{
	float max_screen_size = 0.0f;
	std::vector<std::pair<std::string, uint32_t>> sub_objects; // name, bone index
};

struct Model
{
	std::string source_name;

	// Hierarchy, via the engine's HTreeClass.
	bool has_hierarchy = false;
	std::string hierarchy_name;
	uint32_t hierarchy_pivots = 0;
	std::vector<std::string> pivot_names;
	std::vector<int> pivot_parents;

	bool has_hlod = false;
	std::string hlod_name;
	std::string hlod_hierarchy_name;
	std::vector<LodArray> lod_arrays;
	int rendered_lod = -1;

	std::vector<SubMesh> meshes;

	// Chunk ids seen at any depth that this loader does not consume, and other observations
	// worth reporting rather than swallowing.
	std::vector<std::string> notes;
	uint32_t chunks_read = 0;
};

// Loads `path` (a .w3d extracted from an archive). Returns false and fills `error` on a
// structural problem. Anything unusual but survivable lands in out.notes.
bool load_w3d_model(const char *path, Model &out, std::string &error);

// Human-readable name for a chunk id, for the notes.
const char *w3d_chunk_name(uint32_t id);

} // namespace zh
