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

#include "w3d_model.h"

#include "posix_file.h"

// Engine headers. Everything read off disk below is read into one of the engine's own
// structs; nothing here re-declares a file format record.
#include "WWLib/chunkio.h"
#include "w3d_file.h"
#include "htree.h"
#include "wwmath.h"

#include <cctype>
#include <cstdio>
#include <cstring>

namespace zh
{

bool SubMesh::hidden() const { return (attributes & W3D_MESH_FLAG_HIDDEN) != 0; }
bool SubMesh::two_sided() const { return (attributes & W3D_MESH_FLAG_TWO_SIDED) != 0; }
uint32_t SubMesh::geometry_type() const { return attributes & W3D_MESH_FLAG_GEOMETRY_TYPE_MASK; }
bool SubMesh::is_skin() const
{
	return geometry_type() == W3D_MESH_FLAG_GEOMETRY_TYPE_SKIN ||
	       (attributes & W3D_MESH_FLAG_SKIN) != 0;
}
bool SubMesh::is_camera_aligned() const
{
	return geometry_type() == W3D_MESH_FLAG_GEOMETRY_TYPE_CAMERA_ALIGNED ||
	       geometry_type() == W3D_MESH_FLAG_GEOMETRY_TYPE_CAMERA_ORIENTED;
}

const char *w3d_chunk_name(uint32_t id)
{
	switch (id) {
	case W3D_CHUNK_MESH: return "W3D_CHUNK_MESH";
	case W3D_CHUNK_VERTICES: return "W3D_CHUNK_VERTICES";
	case W3D_CHUNK_VERTEX_NORMALS: return "W3D_CHUNK_VERTEX_NORMALS";
	case W3D_CHUNK_MESH_USER_TEXT: return "W3D_CHUNK_MESH_USER_TEXT";
	case W3D_CHUNK_VERTEX_INFLUENCES: return "W3D_CHUNK_VERTEX_INFLUENCES";
	case W3D_CHUNK_MESH_HEADER3: return "W3D_CHUNK_MESH_HEADER3";
	case W3D_CHUNK_TRIANGLES: return "W3D_CHUNK_TRIANGLES";
	case W3D_CHUNK_VERTEX_SHADE_INDICES: return "W3D_CHUNK_VERTEX_SHADE_INDICES";
	case W3D_CHUNK_PRELIT_UNLIT: return "W3D_CHUNK_PRELIT_UNLIT";
	case W3D_CHUNK_PRELIT_VERTEX: return "W3D_CHUNK_PRELIT_VERTEX";
	case W3D_CHUNK_PRELIT_LIGHTMAP_MULTI_PASS: return "W3D_CHUNK_PRELIT_LIGHTMAP_MULTI_PASS";
	case W3D_CHUNK_PRELIT_LIGHTMAP_MULTI_TEXTURE: return "W3D_CHUNK_PRELIT_LIGHTMAP_MULTI_TEXTURE";
	case W3D_CHUNK_MATERIAL_INFO: return "W3D_CHUNK_MATERIAL_INFO";
	case W3D_CHUNK_SHADERS: return "W3D_CHUNK_SHADERS";
	case W3D_CHUNK_VERTEX_MATERIALS: return "W3D_CHUNK_VERTEX_MATERIALS";
	case W3D_CHUNK_VERTEX_MATERIAL: return "W3D_CHUNK_VERTEX_MATERIAL";
	case W3D_CHUNK_VERTEX_MATERIAL_NAME: return "W3D_CHUNK_VERTEX_MATERIAL_NAME";
	case W3D_CHUNK_VERTEX_MATERIAL_INFO: return "W3D_CHUNK_VERTEX_MATERIAL_INFO";
	case W3D_CHUNK_VERTEX_MAPPER_ARGS0: return "W3D_CHUNK_VERTEX_MAPPER_ARGS0";
	case W3D_CHUNK_VERTEX_MAPPER_ARGS1: return "W3D_CHUNK_VERTEX_MAPPER_ARGS1";
	case W3D_CHUNK_TEXTURES: return "W3D_CHUNK_TEXTURES";
	case W3D_CHUNK_TEXTURE: return "W3D_CHUNK_TEXTURE";
	case W3D_CHUNK_TEXTURE_NAME: return "W3D_CHUNK_TEXTURE_NAME";
	case W3D_CHUNK_TEXTURE_INFO: return "W3D_CHUNK_TEXTURE_INFO";
	case W3D_CHUNK_MATERIAL_PASS: return "W3D_CHUNK_MATERIAL_PASS";
	case W3D_CHUNK_VERTEX_MATERIAL_IDS: return "W3D_CHUNK_VERTEX_MATERIAL_IDS";
	case W3D_CHUNK_SHADER_IDS: return "W3D_CHUNK_SHADER_IDS";
	case W3D_CHUNK_DCG: return "W3D_CHUNK_DCG";
	case W3D_CHUNK_DIG: return "W3D_CHUNK_DIG";
	case W3D_CHUNK_SCG: return "W3D_CHUNK_SCG";
	case W3D_CHUNK_TEXTURE_STAGE: return "W3D_CHUNK_TEXTURE_STAGE";
	case W3D_CHUNK_TEXTURE_IDS: return "W3D_CHUNK_TEXTURE_IDS";
	case W3D_CHUNK_STAGE_TEXCOORDS: return "W3D_CHUNK_STAGE_TEXCOORDS";
	case W3D_CHUNK_PER_FACE_TEXCOORD_IDS: return "W3D_CHUNK_PER_FACE_TEXCOORD_IDS";
	case W3D_CHUNK_HIERARCHY: return "W3D_CHUNK_HIERARCHY";
	case W3D_CHUNK_HLOD: return "W3D_CHUNK_HLOD";
	case W3D_CHUNK_HLOD_HEADER: return "W3D_CHUNK_HLOD_HEADER";
	case W3D_CHUNK_HLOD_LOD_ARRAY: return "W3D_CHUNK_HLOD_LOD_ARRAY";
	case W3D_CHUNK_HLOD_SUB_OBJECT_ARRAY_HEADER: return "W3D_CHUNK_HLOD_SUB_OBJECT_ARRAY_HEADER";
	case W3D_CHUNK_HLOD_SUB_OBJECT: return "W3D_CHUNK_HLOD_SUB_OBJECT";
	case W3D_CHUNK_HLOD_AGGREGATE_ARRAY: return "W3D_CHUNK_HLOD_AGGREGATE_ARRAY";
	case W3D_CHUNK_HLOD_PROXY_ARRAY: return "W3D_CHUNK_HLOD_PROXY_ARRAY";
	case W3D_CHUNK_AABTREE: return "W3D_CHUNK_AABTREE";
	case W3D_CHUNK_EMITTER: return "W3D_CHUNK_EMITTER";
	case W3D_CHUNK_AGGREGATE: return "W3D_CHUNK_AGGREGATE";
	case W3D_CHUNK_COLLECTION: return "W3D_CHUNK_COLLECTION";
	case W3D_CHUNK_BOX: return "W3D_CHUNK_BOX";
	case W3D_CHUNK_ANIMATION: return "W3D_CHUNK_ANIMATION";
	case W3D_CHUNK_COMPRESSED_ANIMATION: return "W3D_CHUNK_COMPRESSED_ANIMATION";
	default: return "unrecognised";
	}
}

namespace
{

std::string fixed_name(const char *raw, size_t capacity)
{
	size_t length = 0;
	while (length < capacity && raw[length] != '\0') {
		++length;
	}
	return std::string(raw, length);
}

void copy_rgb(float *out, const W3dRGBStruct &in)
{
	out[0] = static_cast<float>(in.R) / 255.0f;
	out[1] = static_cast<float>(in.G) / 255.0f;
	out[2] = static_cast<float>(in.B) / 255.0f;
}

void note(Model &model, const std::string &text)
{
	for (const std::string &existing : model.notes) {
		if (existing == text) {
			return;
		}
	}
	model.notes.push_back(text);
}

// Reads a chunk that holds a packed array of `T`. Returns the element count.
template <typename T>
uint32_t read_array(ChunkLoadClass &cload, std::vector<T> &out)
{
	const uint32_t bytes = cload.Cur_Chunk_Length();
	const uint32_t count = bytes / static_cast<uint32_t>(sizeof(T));
	out.resize(count);
	if (count > 0) {
		cload.Read(out.data(), count * static_cast<uint32_t>(sizeof(T)));
	}
	return count;
}

std::string read_string_chunk(ChunkLoadClass &cload)
{
	std::string text(cload.Cur_Chunk_Length(), '\0');
	if (!text.empty()) {
		cload.Read(text.data(), static_cast<uint32>(text.size()));
	}
	const size_t nul = text.find('\0');
	if (nul != std::string::npos) {
		text.resize(nul);
	}
	return text;
}

struct MeshMaterials
{
	std::vector<std::string> texture_names;
	std::vector<W3dShaderStruct> shaders;
	std::vector<std::string> vertex_material_names;
	std::vector<W3dVertexMaterialStruct> vertex_materials;
	std::vector<bool> vertex_material_info_present;
};

void load_vertex_materials(ChunkLoadClass &cload, MeshMaterials &materials)
{
	while (cload.Open_Chunk()) {
		if (cload.Cur_Chunk_ID() == W3D_CHUNK_VERTEX_MATERIAL) {
			materials.vertex_material_names.emplace_back();
			materials.vertex_materials.emplace_back();
			materials.vertex_material_info_present.push_back(false);
			std::memset(&materials.vertex_materials.back(), 0, sizeof(W3dVertexMaterialStruct));
			while (cload.Open_Chunk()) {
				switch (cload.Cur_Chunk_ID()) {
				case W3D_CHUNK_VERTEX_MATERIAL_NAME:
					materials.vertex_material_names.back() = read_string_chunk(cload);
					break;
				case W3D_CHUNK_VERTEX_MATERIAL_INFO:
					cload.Read(&materials.vertex_materials.back(),
					           sizeof(W3dVertexMaterialStruct));
					materials.vertex_material_info_present.back() = true;
					break;
				default:
					break;
				}
				cload.Close_Chunk();
			}
		}
		cload.Close_Chunk();
	}
}

void load_textures(ChunkLoadClass &cload, MeshMaterials &materials)
{
	while (cload.Open_Chunk()) {
		if (cload.Cur_Chunk_ID() == W3D_CHUNK_TEXTURE) {
			materials.texture_names.emplace_back();
			while (cload.Open_Chunk()) {
				if (cload.Cur_Chunk_ID() == W3D_CHUNK_TEXTURE_NAME) {
					materials.texture_names.back() = read_string_chunk(cload);
				}
				cload.Close_Chunk();
			}
		}
		cload.Close_Chunk();
	}
}

// One W3D_CHUNK_MATERIAL_PASS. `pass_texcoords` receives stage 0's texture coordinates when
// the pass carries them.
void load_material_pass(ChunkLoadClass &cload, const MeshMaterials &materials, Model &model,
                        MaterialPass &pass, std::vector<float> &pass_texcoords)
{
	std::vector<uint32> vertex_material_ids;
	std::vector<uint32> shader_ids;
	uint32_t stage_index = 0;

	while (cload.Open_Chunk()) {
		switch (cload.Cur_Chunk_ID()) {
		case W3D_CHUNK_VERTEX_MATERIAL_IDS:
			pass.vertex_material_id_count = read_array(cload, vertex_material_ids);
			break;
		case W3D_CHUNK_SHADER_IDS:
			pass.shader_id_count = read_array(cload, shader_ids);
			break;
		case W3D_CHUNK_DCG:
			pass.per_vertex_dcg = true;
			break;
		case W3D_CHUNK_TEXTURE_STAGE: {
			PassStage stage;
			std::vector<uint32> texture_ids;
			std::vector<W3dTexCoordStruct> texcoords;
			while (cload.Open_Chunk()) {
				switch (cload.Cur_Chunk_ID()) {
				case W3D_CHUNK_TEXTURE_IDS:
					pass.texture_id_count = read_array(cload, texture_ids);
					break;
				case W3D_CHUNK_STAGE_TEXCOORDS:
					read_array(cload, texcoords);
					stage.has_own_texcoords = !texcoords.empty();
					break;
				default:
					break;
				}
				cload.Close_Chunk();
			}
			if (!texture_ids.empty() && texture_ids[0] < materials.texture_names.size()) {
				stage.texture_name = materials.texture_names[texture_ids[0]];
			} else if (!texture_ids.empty() && texture_ids[0] != 0xffffffffu) {
				note(model, "texture id out of range in a material pass");
			}
			if (stage_index == 0 && !texcoords.empty()) {
				pass_texcoords.resize(texcoords.size() * 2);
				for (size_t i = 0; i < texcoords.size(); ++i) {
					pass_texcoords[i * 2 + 0] = texcoords[i].U;
					pass_texcoords[i * 2 + 1] = texcoords[i].V;
				}
			}
			pass.stages.push_back(stage);
			++stage_index;
			break;
		}
		default:
			break;
		}
		cload.Close_Chunk();
	}

	if (!vertex_material_ids.empty() &&
	    vertex_material_ids[0] < materials.vertex_materials.size()) {
		const size_t id = vertex_material_ids[0];
		const W3dVertexMaterialStruct &vm = materials.vertex_materials[id];
		pass.vertex_material_name = materials.vertex_material_names[id];
		pass.vertex_material_attributes = vm.Attributes;
		copy_rgb(pass.ambient, vm.Ambient);
		copy_rgb(pass.diffuse, vm.Diffuse);
		copy_rgb(pass.specular, vm.Specular);
		copy_rgb(pass.emissive, vm.Emissive);
		pass.opacity = vm.Opacity;
		pass.shininess = vm.Shininess;
	}
	if (!shader_ids.empty() && shader_ids[0] < materials.shaders.size()) {
		const W3dShaderStruct &shader = materials.shaders[shader_ids[0]];
		pass.depth_compare = shader.DepthCompare;
		pass.depth_mask = shader.DepthMask;
		pass.src_blend = shader.SrcBlend;
		pass.dest_blend = shader.DestBlend;
		pass.alpha_test = shader.AlphaTest;
		pass.pri_gradient = shader.PriGradient;
		pass.texturing = shader.Texturing;
		pass.shader_present = true;
	}
}

bool load_mesh(ChunkLoadClass &cload, Model &model, SubMesh &mesh)
{
	MeshMaterials materials;
	bool have_header = false;
	std::vector<std::vector<float>> pass_texcoords;

	while (cload.Open_Chunk()) {
		switch (cload.Cur_Chunk_ID()) {
		case W3D_CHUNK_MESH_HEADER3: {
			W3dMeshHeader3Struct header;
			if (cload.Read(&header, sizeof(header)) != sizeof(header)) {
				return false;
			}
			mesh.name = fixed_name(header.MeshName, W3D_NAME_LEN);
			mesh.container_name = fixed_name(header.ContainerName, W3D_NAME_LEN);
			mesh.attributes = header.Attributes;
			mesh.version = header.Version;
			mesh.declared_triangles = header.NumTris;
			mesh.declared_vertices = header.NumVertices;
			mesh.declared_materials = header.NumMaterials;
			mesh.vertex_channels = header.VertexChannels;
			mesh.face_channels = header.FaceChannels;
			mesh.sort_level = header.SortLevel;
			mesh.declared_min[0] = header.Min.X;
			mesh.declared_min[1] = header.Min.Y;
			mesh.declared_min[2] = header.Min.Z;
			mesh.declared_max[0] = header.Max.X;
			mesh.declared_max[1] = header.Max.Y;
			mesh.declared_max[2] = header.Max.Z;
			mesh.declared_sphere_center[0] = header.SphCenter.X;
			mesh.declared_sphere_center[1] = header.SphCenter.Y;
			mesh.declared_sphere_center[2] = header.SphCenter.Z;
			mesh.declared_sphere_radius = header.SphRadius;
			have_header = true;
			break;
		}
		case W3D_CHUNK_VERTICES: {
			std::vector<W3dVectorStruct> vertices;
			read_array(cload, vertices);
			mesh.positions.resize(vertices.size() * 3);
			for (size_t i = 0; i < vertices.size(); ++i) {
				mesh.positions[i * 3 + 0] = vertices[i].X;
				mesh.positions[i * 3 + 1] = vertices[i].Y;
				mesh.positions[i * 3 + 2] = vertices[i].Z;
			}
			break;
		}
		case W3D_CHUNK_VERTEX_NORMALS: {
			std::vector<W3dVectorStruct> normals;
			read_array(cload, normals);
			mesh.normals.resize(normals.size() * 3);
			for (size_t i = 0; i < normals.size(); ++i) {
				mesh.normals[i * 3 + 0] = normals[i].X;
				mesh.normals[i * 3 + 1] = normals[i].Y;
				mesh.normals[i * 3 + 2] = normals[i].Z;
			}
			break;
		}
		case W3D_CHUNK_TRIANGLES: {
			std::vector<W3dTriStruct> triangles;
			read_array(cload, triangles);
			mesh.indices.resize(triangles.size() * 3);
			mesh.face_normals.resize(triangles.size() * 3);
			for (size_t i = 0; i < triangles.size(); ++i) {
				mesh.indices[i * 3 + 0] = triangles[i].Vindex[0];
				mesh.indices[i * 3 + 1] = triangles[i].Vindex[1];
				mesh.indices[i * 3 + 2] = triangles[i].Vindex[2];
				mesh.face_normals[i * 3 + 0] = triangles[i].Normal.X;
				mesh.face_normals[i * 3 + 1] = triangles[i].Normal.Y;
				mesh.face_normals[i * 3 + 2] = triangles[i].Normal.Z;
			}
			break;
		}
		case W3D_CHUNK_VERTEX_SHADE_INDICES:
			read_array(cload, mesh.shade_indices);
			break;
		case W3D_CHUNK_VERTEX_INFLUENCES: {
			std::vector<W3dVertInfStruct> influences;
			read_array(cload, influences);
			mesh.vertex_influences.resize(influences.size());
			for (size_t i = 0; i < influences.size(); ++i) {
				mesh.vertex_influences[i] = static_cast<uint8_t>(influences[i].BoneIdx);
			}
			break;
		}
		case W3D_CHUNK_MATERIAL_INFO: {
			W3dMaterialInfoStruct info;
			cload.Read(&info, sizeof(info));
			break;
		}
		case W3D_CHUNK_SHADERS:
			read_array(cload, materials.shaders);
			break;
		case W3D_CHUNK_VERTEX_MATERIALS:
			load_vertex_materials(cload, materials);
			break;
		case W3D_CHUNK_TEXTURES:
			load_textures(cload, materials);
			break;
		case W3D_CHUNK_MATERIAL_PASS: {
			MaterialPass pass;
			pass_texcoords.emplace_back();
			load_material_pass(cload, materials, model, pass, pass_texcoords.back());
			mesh.passes.push_back(pass);
			break;
		}
		case W3D_CHUNK_AABTREE:
		case W3D_CHUNK_MESH_USER_TEXT:
			break;
		case W3D_CHUNK_PRELIT_UNLIT:
		case W3D_CHUNK_PRELIT_VERTEX:
		case W3D_CHUNK_PRELIT_LIGHTMAP_MULTI_PASS:
		case W3D_CHUNK_PRELIT_LIGHTMAP_MULTI_TEXTURE:
			note(model, std::string("mesh carries a prelit material wrapper (") +
			                w3d_chunk_name(cload.Cur_Chunk_ID()) +
			                "); the unlit pass is the one rendered");
			break;
		default:
			note(model, std::string("mesh sub-chunk not consumed: ") +
			                w3d_chunk_name(cload.Cur_Chunk_ID()));
			break;
		}
		++model.chunks_read;
		cload.Close_Chunk();
	}

	for (size_t i = 0; i < pass_texcoords.size(); ++i) {
		if (!pass_texcoords[i].empty() && mesh.texcoords.empty()) {
			mesh.texcoords = pass_texcoords[i];
		}
	}
	return have_header;
}

void load_hlod(ChunkLoadClass &cload, Model &model)
{
	while (cload.Open_Chunk()) {
		switch (cload.Cur_Chunk_ID()) {
		case W3D_CHUNK_HLOD_HEADER: {
			W3dHLodHeaderStruct header;
			cload.Read(&header, sizeof(header));
			model.hlod_name = fixed_name(header.Name, W3D_NAME_LEN);
			model.hlod_hierarchy_name = fixed_name(header.HierarchyName, W3D_NAME_LEN);
			break;
		}
		case W3D_CHUNK_HLOD_LOD_ARRAY: {
			LodArray array;
			while (cload.Open_Chunk()) {
				switch (cload.Cur_Chunk_ID()) {
				case W3D_CHUNK_HLOD_SUB_OBJECT_ARRAY_HEADER: {
					W3dHLodArrayHeaderStruct header;
					cload.Read(&header, sizeof(header));
					array.max_screen_size = header.MaxScreenSize;
					break;
				}
				case W3D_CHUNK_HLOD_SUB_OBJECT: {
					W3dHLodSubObjectStruct sub;
					cload.Read(&sub, sizeof(sub));
					array.sub_objects.emplace_back(fixed_name(sub.Name, W3D_NAME_LEN * 2),
					                               sub.BoneIndex);
					break;
				}
				default:
					break;
				}
				cload.Close_Chunk();
			}
			model.lod_arrays.push_back(array);
			break;
		}
		case W3D_CHUNK_HLOD_AGGREGATE_ARRAY:
		case W3D_CHUNK_HLOD_PROXY_ARRAY:
			note(model, std::string("HLOD carries a ") + w3d_chunk_name(cload.Cur_Chunk_ID()) +
			                "; not resolved by this tool");
			break;
		default:
			break;
		}
		cload.Close_Chunk();
	}
}

// The sub-object name in the HLOD table is "<container>.<mesh>"; match on the mesh part.
bool sub_object_matches(const std::string &sub_object_name, const SubMesh &mesh)
{
	const size_t dot = sub_object_name.rfind('.');
	const std::string leaf =
	    dot == std::string::npos ? sub_object_name : sub_object_name.substr(dot + 1);
	if (leaf.size() != mesh.name.size()) {
		return false;
	}
	for (size_t i = 0; i < leaf.size(); ++i) {
		if (std::tolower(static_cast<unsigned char>(leaf[i])) !=
		    std::tolower(static_cast<unsigned char>(mesh.name[i]))) {
			return false;
		}
	}
	return true;
}

} // namespace

bool load_w3d_model(const char *path, Model &out, std::string &error)
{
	// Fills the engine's fast trig tables. Cheap, idempotent enough to call per load.
	WWMath::Init();

	PosixFileClass file(path);
	if (!file.Is_Available()) {
		error = std::string("cannot open ") + path;
		return false;
	}
	if (!file.Open(FileClass::READ)) {
		error = std::string("cannot read ") + path;
		return false;
	}

	ChunkLoadClass cload(&file);
	HTreeClass htree;
	bool htree_loaded = false;

	while (cload.Open_Chunk()) {
		const uint32_t id = cload.Cur_Chunk_ID();
		++out.chunks_read;
		switch (id) {
		case W3D_CHUNK_HIERARCHY:
			// The engine's own hierarchy loader, unmodified.
			if (htree.Load_W3D(cload) != HTreeClass::OK) {
				error = "HTreeClass::Load_W3D failed";
				file.Close();
				return false;
			}
			htree_loaded = true;
			out.has_hierarchy = true;
			out.hierarchy_name = htree.Get_Name();
			out.hierarchy_pivots = static_cast<uint32_t>(htree.Num_Pivots());
			break;
		case W3D_CHUNK_MESH: {
			SubMesh mesh;
			if (!load_mesh(cload, out, mesh)) {
				error = "mesh without a W3D_CHUNK_MESH_HEADER3";
				file.Close();
				return false;
			}
			out.meshes.push_back(std::move(mesh));
			break;
		}
		case W3D_CHUNK_HLOD:
			out.has_hlod = true;
			load_hlod(cload, out);
			break;
		default:
			note(out, std::string("top-level chunk not consumed: ") + w3d_chunk_name(id));
			break;
		}
		cload.Close_Chunk();
	}
	file.Close();

	if (out.meshes.empty()) {
		error = "no W3D_CHUNK_MESH in the file";
		return false;
	}

	// Pose the hierarchy with the engine's own code, at the identity root transform.
	if (htree_loaded) {
		htree.Base_Update(Matrix3D(1));
		for (int i = 0; i < htree.Num_Pivots(); ++i) {
			out.pivot_names.push_back(htree.Get_Bone_Name(i));
			out.pivot_parents.push_back(htree.Get_Parent_Index(i));
		}
	}

	// Pick the most detailed LOD array: W3D stores them lowest-detail first.
	if (!out.lod_arrays.empty()) {
		out.rendered_lod = static_cast<int>(out.lod_arrays.size()) - 1;
	}

	for (SubMesh &mesh : out.meshes) {
		mesh.bone_index = 0;
		if (out.rendered_lod >= 0) {
			mesh.in_rendered_lod = false;
			for (const auto &sub : out.lod_arrays[out.rendered_lod].sub_objects) {
				if (sub_object_matches(sub.first, mesh)) {
					mesh.bone_index = static_cast<int>(sub.second);
					mesh.in_rendered_lod = true;
					break;
				}
			}
		}
		if (htree_loaded && mesh.bone_index < htree.Num_Pivots()) {
			const Matrix3D &tm = htree.Get_Transform(mesh.bone_index);
			for (int row = 0; row < 3; ++row) {
				for (int col = 0; col < 4; ++col) {
					mesh.world_transform[row * 4 + col] = tm[row][col];
				}
			}
		}
	}

	return true;
}

} // namespace zh
