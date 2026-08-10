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

// zh-model-render: load a retail Zero Hour model and its retail textures out of the .big
// archives and draw it on this machine's GPU through the renderer spike's Vulkan backend.
//
//   zh-model-render --data /path/to/zero-hour [--model avcrusader.W3D] [--out model.png]
//
// The data directory is always an argument: the licensed asset set never appears in this
// repository, in the build, or in the committed evidence beyond the rendered image.

#include "asset_source.h"
#include "scene.h"
#include "texture_load.h"
#include "w3d_model.h"

#include "png_write.h"
#include "render_backend.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

// The D3D8 enums the backend's interface speaks, spelled as the engine spells them.
using namespace spike;

namespace
{

struct Options
{
	std::string data_dir;
	std::string model_name = "avcrusader.W3D";
	std::string out_path = "model.png";
	std::string reference_path;
	std::string texture_dump_prefix;
	bool flat_light = false;
	bool uv_test = false;
	std::string cull_override;
	uint32_t width = 1024;
	uint32_t height = 768;
	bool validation = true;
};

bool parse_options(int argc, char **argv, Options &options)
{
	for (int i = 1; i < argc; ++i) {
		const char *argument = argv[i];
		auto value = [&](const char *name) -> const char * {
			if (std::strcmp(argument, name) == 0 && i + 1 < argc) {
				return argv[++i];
			}
			return nullptr;
		};
		if (const char *v = value("--data")) {
			options.data_dir = v;
		} else if (const char *v = value("--model")) {
			options.model_name = v;
		} else if (const char *v = value("--out")) {
			options.out_path = v;
		} else if (const char *v = value("--reference-out")) {
			options.reference_path = v;
		} else if (const char *v = value("--dump-textures")) {
			options.texture_dump_prefix = v;
		} else if (const char *v = value("--width")) {
			options.width = static_cast<uint32_t>(std::atoi(v));
		} else if (const char *v = value("--height")) {
			options.height = static_cast<uint32_t>(std::atoi(v));
		} else if (const char *v = value("--cull")) {
			options.cull_override = v;
		} else if (std::strcmp(argument, "--uv-test") == 0) {
			options.uv_test = true;
		} else if (std::strcmp(argument, "--flat-light") == 0) {
			options.flat_light = true;
		} else if (std::strcmp(argument, "--no-validation") == 0) {
			options.validation = false;
		} else if (std::strcmp(argument, "--help") == 0) {
			std::printf("usage: %s --data <zero-hour-dir> [--model name.W3D] [--out file.png]\n"
			            "          [--reference-out file.png] [--width N] [--height N]\n"
			            "          [--flat-light] [--uv-test] [--cull none|cw|ccw]\n"
			            "          [--dump-textures prefix] [--no-validation]\n",
			            argv[0]);
			return false;
		} else {
			std::fprintf(stderr, "unrecognised argument %s\n", argument);
			return false;
		}
	}
	if (options.data_dir.empty()) {
		std::fprintf(stderr, "--data <zero-hour-dir> is required\n");
		return false;
	}
	return true;
}

// The engine's chunk reader works through FileClass, so the archived member is spilled to a
// scratch file and removed again. Nothing from the archives outlives the run.
class ScratchFile
{
public:
	bool write(const std::vector<uint8_t> &bytes, std::string &error)
	{
		char pattern[] = "/tmp/zh-model-render-XXXXXX";
		const int fd = ::mkstemp(pattern);
		if (fd < 0) {
			error = "cannot create a scratch file";
			return false;
		}
		Path = pattern;
		const ssize_t written = ::write(fd, bytes.data(), bytes.size());
		::close(fd);
		if (written != static_cast<ssize_t>(bytes.size())) {
			error = "short write to " + Path;
			return false;
		}
		return true;
	}

	~ScratchFile()
	{
		if (!Path.empty()) {
			::unlink(Path.c_str());
		}
	}

	const std::string &path() const { return Path; }

private:
	std::string Path;
};

// The scene and the CPU reference work in Westwood's column-vector convention (v' = M v).
// The backend takes matrices in D3D8's row-vector convention, because that is what
// DX8Wrapper hands it via To_D3DMATRIX, and composes them as world * view * projection.
// Transposing here is exactly that conversion.
spike::Matrix4x4 to_d3d_matrix(const spike::Matrix4x4 &m)
{
	spike::Matrix4x4 out{};
	for (int row = 0; row < 4; ++row) {
		for (int column = 0; column < 4; ++column) {
			out.m[row][column] = m.m[column][row];
		}
	}
	return out;
}

// D3DRS_AMBIENT is a D3DCOLOR; the scene's ambient term is a single grey level.
uint32_t grey_d3dcolor(float level)
{
	const float clamped = level < 0.0f ? 0.0f : (level > 1.0f ? 1.0f : level);
	const uint32_t channel = static_cast<uint32_t>(clamped * 255.0f + 0.5f);
	return 0xff000000u | (channel << 16) | (channel << 8) | channel;
}

struct CheckResults
{
	uint32_t failures = 0;
	uint32_t meshes_checked = 0;
	uint32_t textured_meshes = 0;
	uint32_t unresolved_textures = 0;
	float uv_min = 0.0f;
	float uv_max = 0.0f;
};

// Everything here is checked against a value that came from a different part of the file than
// the data being checked: counts and bounds against W3D_CHUNK_MESH_HEADER3, texture names
// against the archive directory. This is the part that decides whether the picture can be
// trusted.
CheckResults cross_check(const zh::Model &model, const zh::AssetSource &assets)
{
	CheckResults results;
	bool first_uv = true;
	std::printf("\ngeometry cross-checks (parsed arrays vs W3D_CHUNK_MESH_HEADER3)\n");
	std::printf("%-18s %8s %8s %8s %8s  %s\n", "mesh", "verts", "decl", "tris", "decl", "bounds");
	for (const zh::SubMesh &mesh : model.meshes) {
		++results.meshes_checked;
		const uint32_t vertices = static_cast<uint32_t>(mesh.positions.size() / 3);
		const uint32_t triangles = static_cast<uint32_t>(mesh.indices.size() / 3);
		bool bounds_ok = true;
		float min[3] = {0.0f, 0.0f, 0.0f};
		float max[3] = {0.0f, 0.0f, 0.0f};
		for (uint32_t v = 0; v < vertices; ++v) {
			for (int axis = 0; axis < 3; ++axis) {
				const float value = mesh.positions[v * 3 + static_cast<size_t>(axis)];
				if (v == 0) {
					min[axis] = max[axis] = value;
				} else {
					min[axis] = std::min(min[axis], value);
					max[axis] = std::max(max[axis], value);
				}
			}
		}
		// The header's box is the exporter's; it may be marginally larger than the vertices
		// but must never be smaller.
		const float tolerance = 1e-3f;
		for (int axis = 0; axis < 3; ++axis) {
			if (min[axis] < mesh.declared_min[axis] - tolerance ||
			    max[axis] > mesh.declared_max[axis] + tolerance) {
				bounds_ok = false;
			}
		}
		const bool counts_ok =
		    vertices == mesh.declared_vertices && triangles == mesh.declared_triangles;
		uint32_t worst_index = 0;
		for (uint32_t index : mesh.indices) {
			worst_index = std::max(worst_index, index);
		}
		const bool indices_ok = vertices == 0 || worst_index < vertices;
		const bool normals_ok = mesh.normals.size() == mesh.positions.size();

		bool uvs_ok = true;
		if (!mesh.texcoords.empty()) {
			if (mesh.texcoords.size() != static_cast<size_t>(vertices) * 2) {
				uvs_ok = false;
			}
			for (float value : mesh.texcoords) {
				if (!std::isfinite(value)) {
					uvs_ok = false;
					continue;
				}
				if (first_uv) {
					results.uv_min = results.uv_max = value;
					first_uv = false;
				}
				results.uv_min = std::min(results.uv_min, value);
				results.uv_max = std::max(results.uv_max, value);
			}
		}

		const bool ok = counts_ok && bounds_ok && indices_ok && normals_ok && uvs_ok;
		if (!ok) {
			++results.failures;
		}
		std::printf("%-18s %8u %8u %8u %8u  %-6s %s%s%s%s\n", mesh.name.c_str(), vertices,
		            mesh.declared_vertices, triangles, mesh.declared_triangles,
		            bounds_ok ? "inside" : "OUTSIDE", counts_ok ? "" : "COUNT-MISMATCH ",
		            indices_ok ? "" : "INDEX-OUT-OF-RANGE ", normals_ok ? "" : "NORMALS-MISSING ",
		            uvs_ok ? "" : "UV-BROKEN ");
	}
	std::printf("texture coordinate range across all meshes: %.4f .. %.4f\n", results.uv_min,
	            results.uv_max);

	std::printf("\ntexture references (from the .w3d) resolved against the archives\n");
	for (const zh::SubMesh &mesh : model.meshes) {
		for (size_t pass = 0; pass < mesh.passes.size(); ++pass) {
			for (size_t stage = 0; stage < mesh.passes[pass].stages.size(); ++stage) {
				const std::string &name = mesh.passes[pass].stages[stage].texture_name;
				if (name.empty()) {
					continue;
				}
				zh::AssetLocation where;
				const bool found = assets.find(name, where);
				if (!found) {
					++results.unresolved_textures;
				} else if (pass == 0 && stage == 0) {
					++results.textured_meshes;
				}
				std::printf("%-18s pass %zu stage %zu  %-20s -> %s\n", mesh.name.c_str(), pass,
				            stage, name.c_str(),
				            found ? (where.archive + ":" + where.stored_name).c_str()
				                  : "NOT FOUND IN ANY ARCHIVE");
			}
		}
	}
	return results;
}

} // namespace

int main(int argc, char **argv)
{
	Options options;
	if (!parse_options(argc, argv, options)) {
		return 1;
	}

	zh::AssetSource assets;
	std::string error;
	if (!assets.open_directory(options.data_dir, error)) {
		std::fprintf(stderr, "%s\n", error.c_str());
		return 1;
	}
	std::printf("archives: %zu, entries: %zu\n", assets.archive_count(), assets.entry_count());

	std::vector<uint8_t> model_bytes;
	zh::AssetLocation model_location;
	if (!assets.read_by_name(options.model_name, model_bytes, model_location, error, false)) {
		std::fprintf(stderr, "%s\n", error.c_str());
		return 1;
	}
	std::printf("model: %s:%s (%zu bytes)\n", model_location.archive.c_str(),
	            model_location.stored_name.c_str(), model_bytes.size());

	ScratchFile scratch;
	if (!scratch.write(model_bytes, error)) {
		std::fprintf(stderr, "%s\n", error.c_str());
		return 1;
	}

	zh::Model model;
	model.source_name = model_location.stored_name;
	if (!zh::load_w3d_model(scratch.path().c_str(), model, error)) {
		std::fprintf(stderr, "load_w3d_model: %s\n", error.c_str());
		return 1;
	}
	std::printf("chunks read: %u, meshes: %zu, hierarchy: %s (%u pivots), HLOD: %s, LOD arrays: "
	            "%zu (rendering %d)\n",
	            model.chunks_read, model.meshes.size(),
	            model.has_hierarchy ? model.hierarchy_name.c_str() : "none",
	            model.hierarchy_pivots, model.has_hlod ? model.hlod_name.c_str() : "none",
	            model.lod_arrays.size(), model.rendered_lod);

	const CheckResults checks = cross_check(model, assets);

	// --- textures ----------------------------------------------------------------
	std::vector<zh::TextureImage> textures;
	std::vector<int> mesh_texture_index(model.meshes.size(), -1);
	std::printf("\ntextures loaded from the archives\n");
	for (size_t mesh_index = 0; mesh_index < model.meshes.size(); ++mesh_index) {
		const zh::SubMesh &mesh = model.meshes[mesh_index];
		if (mesh.passes.empty() || mesh.passes.front().stages.empty()) {
			continue;
		}
		const std::string &name = mesh.passes.front().stages.front().texture_name;
		if (name.empty()) {
			continue;
		}
		int existing = -1;
		for (size_t t = 0; t < textures.size(); ++t) {
			if (textures[t].requested_name == name) {
				existing = static_cast<int>(t);
				break;
			}
		}
		if (existing >= 0) {
			mesh_texture_index[mesh_index] = existing;
			continue;
		}

		std::vector<uint8_t> bytes;
		zh::AssetLocation where;
		if (!assets.read_by_name(name, bytes, where, error)) {
			std::fprintf(stderr, "  %-22s %s\n", name.c_str(), error.c_str());
			continue;
		}
		zh::TextureImage image;
		if (!zh::load_texture(bytes, image, error)) {
			std::fprintf(stderr, "  %-22s %s\n", name.c_str(), error.c_str());
			continue;
		}
		image.requested_name = name;
		image.archive = where.archive;
		image.stored_name = where.stored_name;
		image.file_bytes = bytes.size();
		std::printf("  %-22s %s:%-34s %4ux%-4u %-6s %-28s %zu mip(s)%s\n", name.c_str(),
		            where.archive.c_str(), where.stored_name.c_str(), image.width, image.height,
		            image.format_name.c_str(), image.vulkan_format.c_str(), image.mips.size(),
		            image.has_alpha ? " alpha" : "");
		mesh_texture_index[mesh_index] = static_cast<int>(textures.size());
		textures.push_back(std::move(image));
	}

	if (options.uv_test) {
		// Diagnostic: replaces every texture with the same synthetic gradient, so the GPU
		// image and the CPU reference can be compared on UV addressing alone. Red follows u,
		// green follows v, and the checker makes any half-texel or flip obvious.
		for (zh::TextureImage &image : textures) {
			image.format = zh::TexFormat::Bgra8;
			image.format_name = "uv-test";
			image.vulkan_format = "VK_FORMAT_B8G8R8A8_UNORM";
			image.width = image.height = 256;
			image.has_alpha = false;
			image.data.assign(256u * 256u * 4u, 0);
			for (uint32_t y = 0; y < 256; ++y) {
				for (uint32_t x = 0; x < 256; ++x) {
					uint8_t *pixel = image.data.data() + (static_cast<size_t>(y) * 256 + x) * 4;
					pixel[0] = ((x / 32 + y / 32) % 2) ? 0u : 255u; // blue: checker
					pixel[1] = static_cast<uint8_t>(y);             // green follows v
					pixel[2] = static_cast<uint8_t>(x);             // red follows u
					pixel[3] = 255;
				}
			}
			image.mips.clear();
			zh::MipLevel level;
			level.width = level.height = 256;
			level.offset = 0;
			level.bytes = image.data.size();
			image.mips.push_back(level);
		}
	}

	std::vector<std::string> notes;
	zh::Light light;
	if (options.flat_light) {
		// Diagnostic: kills the lighting term so the texture path can be compared on its own.
		light.ambient = 1.0f;
		light.intensity = 0.0f;
	}
	std::vector<zh::DrawBatch> batches =
	    zh::build_batches(model, textures, mesh_texture_index, light, notes);
	if (batches.empty()) {
		std::fprintf(stderr, "nothing to draw\n");
		return 1;
	}
	const zh::Camera camera = zh::frame_batches(batches, options.width, options.height);

	// --- CPU reference, before the GPU ever runs ---------------------------------
	std::vector<std::vector<uint8_t>> decoded_mip0(textures.size());
	for (size_t t = 0; t < textures.size(); ++t) {
		std::string decode_error;
		if (!zh::decode_mip_to_rgba(textures[t], 0, decoded_mip0[t], decode_error)) {
			std::fprintf(stderr, "decode %s: %s\n", textures[t].requested_name.c_str(),
			             decode_error.c_str());
		}
		if (!options.texture_dump_prefix.empty() && !decoded_mip0[t].empty()) {
			// Diagnostic only: lets the CPU-side decode be looked at on its own, so a
			// disagreement with the GPU can be pinned on the decoder or on the upload.
			const std::string path = options.texture_dump_prefix + std::to_string(t) + ".png";
			const std::string pixels(reinterpret_cast<const char *>(decoded_mip0[t].data()),
			                         decoded_mip0[t].size());
			spike::Write_Png(path, pixels, textures[t].width, textures[t].height);
			std::printf("  wrote decoded %s to %s\n", textures[t].requested_name.c_str(),
			            path.c_str());
		}
	}
	const float clear_rgb[3] = {0.06f, 0.07f, 0.10f};
	// First pass draws every triangle, so the winding of the visible surface is measured
	// without assuming which side is front.
	const zh::ReferenceImage unculled_reference =
	    zh::rasterise_reference(batches, textures, decoded_mip0, camera, options.width,
	                            options.height, clear_rgb, zh::ReferenceCull::None);
	const zh::ReferenceImage &reference_for_winding = unculled_reference;
	const double cw_share =
	    reference_for_winding.covered_pixels == 0
	        ? 0.0
	        : static_cast<double>(reference_for_winding.visible_cw_pixels) /
	              static_cast<double>(reference_for_winding.covered_pixels);
	std::printf("\nCPU reference: %llu covered pixels, visible surface %.1f%% clockwise on "
	            "screen\n",
	            static_cast<unsigned long long>(reference_for_winding.covered_pixels),
	            cw_share * 100.0);

	// A closed mesh drawn with a consistent winding shows one orientation on the visible side.
	// Which one it is decides the cull mode, so it is measured rather than guessed. D3D8 names
	// its cull modes after the winding to *discard*, in a y-down screen space, so the visible
	// orientation picks the opposite mode.
	const bool front_faces_are_clockwise = cw_share >= 0.5;
	uint32_t cull_mode = front_faces_are_clockwise ? D3DCULL_CCW : D3DCULL_CW;
	if (options.cull_override == "none") {
		cull_mode = D3DCULL_NONE;
	} else if (options.cull_override == "cw") {
		cull_mode = D3DCULL_CW;
	} else if (options.cull_override == "ccw") {
		cull_mode = D3DCULL_CCW;
	}
	// Second pass culls the same faces the GPU will, so the two images are compared under the
	// same rules. The decision above came from the pass that culled nothing.
	const zh::ReferenceCull reference_cull =
	    cull_mode == D3DCULL_NONE
	        ? zh::ReferenceCull::None
	        : (cull_mode == D3DCULL_CW ? zh::ReferenceCull::Clockwise
	                                   : zh::ReferenceCull::CounterClockwise);
	const zh::ReferenceImage reference =
	    zh::rasterise_reference(batches, textures, decoded_mip0, camera, options.width,
	                            options.height, clear_rgb, reference_cull);
	std::printf("visible side is %s on screen -> culling %s faces\n",
	            front_faces_are_clockwise ? "clockwise" : "counter-clockwise",
	            front_faces_are_clockwise ? "counter-clockwise" : "clockwise");

	// --- GPU ---------------------------------------------------------------------
	spike::RenderBackend *gfx = spike::Create_Vulkan_Backend(options.validation, true);
	if (!gfx->Init(nullptr, options.width, options.height)) {
		std::fprintf(stderr, "Vulkan backend Init failed\n");
		return 1;
	}
	std::printf("\ndevice: %s\n", gfx->Device_Description());
	std::printf("sampled-image support: DXT1 %s, DXT3 %s, DXT5 %s, A8R8G8B8 %s\n",
	            gfx->Supports_Texture_Format(spike::TextureFormat::DXT1) ? "yes" : "no",
	            gfx->Supports_Texture_Format(spike::TextureFormat::DXT3) ? "yes" : "no",
	            gfx->Supports_Texture_Format(spike::TextureFormat::DXT5) ? "yes" : "no",
	            gfx->Supports_Texture_Format(spike::TextureFormat::A8R8G8B8) ? "yes" : "no");

	std::vector<spike::TextureHandle *> texture_handles(textures.size(), nullptr);
	for (size_t t = 0; t < textures.size(); ++t) {
		const zh::TextureImage &image = textures[t];
		spike::TextureFormat format = spike::TextureFormat::A8R8G8B8;
		switch (image.format) {
		case zh::TexFormat::Bc1: format = spike::TextureFormat::DXT1; break;
		case zh::TexFormat::Bc2: format = spike::TextureFormat::DXT3; break;
		case zh::TexFormat::Bc3: format = spike::TextureFormat::DXT5; break;
		default: break;
		}
		if (!gfx->Supports_Texture_Format(format)) {
			std::fprintf(stderr, "device cannot sample %s (%s)\n", image.format_name.c_str(),
			             image.requested_name.c_str());
			return 1;
		}
		std::vector<spike::TextureMip> levels(image.mips.size());
		for (size_t mip = 0; mip < image.mips.size(); ++mip) {
			levels[mip].data = image.data.data() + image.mips[mip].offset;
			levels[mip].bytes = image.mips[mip].bytes;
			levels[mip].width = image.mips[mip].width;
			levels[mip].height = image.mips[mip].height;
		}
		spike::TextureDesc desc;
		desc.format = format;
		desc.mips = levels.data();
		desc.mip_count = static_cast<uint32_t>(levels.size());
		texture_handles[t] = gfx->Create_Texture(desc);
		if (texture_handles[t] == nullptr) {
			std::fprintf(stderr, "Create_Texture failed for %s\n", image.requested_name.c_str());
			return 1;
		}
	}

	struct GpuBatch
	{
		spike::VertexBufferHandle *vb = nullptr;
		spike::IndexBufferHandle *ib = nullptr;
	};
	std::vector<GpuBatch> gpu_batches(batches.size());
	for (size_t b = 0; b < batches.size(); ++b) {
		gpu_batches[b].vb = gfx->Create_Vertex_Buffer(
		    batches[b].vertices.data(),
		    batches[b].vertices.size() * sizeof(zh::ModelVertex),
		    D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1);
		gpu_batches[b].ib =
		    gfx->Create_Index_Buffer(batches[b].indices.data(), batches[b].indices.size());
		if (gpu_batches[b].vb == nullptr || gpu_batches[b].ib == nullptr) {
			std::fprintf(stderr, "buffer creation failed for %s\n",
			             batches[b].mesh_name.c_str());
			return 1;
		}
	}

	// The frame is drawn twice: once with mip filtering off, which is the configuration the CPU
	// reference can be held to, and once with the full mip chain, which is what the engine asks
	// for. The difference between the two is reported rather than hidden.
	float light_direction[3] = {light.direction[0], light.direction[1], light.direction[2]};
	{
		const float length = std::sqrt(light_direction[0] * light_direction[0] +
		                               light_direction[1] * light_direction[1] +
		                               light_direction[2] * light_direction[2]);
		if (length > 0.0f) {
			for (float &component : light_direction) {
				component /= length;
			}
		}
	}

	spike::SurfaceFormat format;
	std::string rgba;
	auto draw_frame = [&](uint32_t mip_filter, std::string &out) -> bool {
		gfx->Begin_Scene();
		gfx->Clear(true, true, clear_rgb[0], clear_rgb[1], clear_rgb[2], 1.0f);
		gfx->Set_Transform(D3DTS_VIEW, to_d3d_matrix(camera.view));
		gfx->Set_Transform(D3DTS_PROJECTION, to_d3d_matrix(camera.projection));
		gfx->Set_DX8_Render_State(D3DRS_ZENABLE, 1);
		gfx->Set_DX8_Render_State(D3DRS_ZWRITEENABLE, 1);
		gfx->Set_DX8_Render_State(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
		gfx->Set_DX8_Render_State(D3DRS_ALPHABLENDENABLE, 0);
		gfx->Set_DX8_Render_State(D3DRS_ALPHAREF, 128);
		gfx->Set_DX8_Render_State(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
		// The backend's own fixed-function lighting, not a colour baked into the vertices:
		// one directional light plus a global ambient, with the W3D vertex material handed
		// over per batch as D3D8 material state.
		gfx->Set_DX8_Render_State(D3DRS_LIGHTING, 1);
		gfx->Set_DX8_Render_State(D3DRS_SPECULARENABLE, 0);
		// The reference normalises the transformed normal, and the pivot transforms are
		// rigid, so asking D3D8 to renormalise keeps the two identical.
		gfx->Set_DX8_Render_State(D3DRS_NORMALIZENORMALS, 1);
		gfx->Set_DX8_Render_State(D3DRS_AMBIENT, grey_d3dcolor(light.ambient));
		gfx->Set_DX8_Render_State(D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_MATERIAL);
		gfx->Set_DX8_Render_State(D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_MATERIAL);
		gfx->Set_DX8_Render_State(D3DRS_EMISSIVEMATERIALSOURCE, D3DMCS_MATERIAL);
		spike::LightState directional;
		directional.type = D3DLIGHT_DIRECTIONAL;
		for (int i = 0; i < 3; ++i) {
			directional.diffuse[i] = light.intensity;
			directional.direction[i] = light_direction[i];
		}
		directional.diffuse[3] = 1.0f;
		gfx->Set_Light(0, &directional);
		// texture MODULATE diffuse: what W3D's default shader does with a vertex material.
		gfx->Set_DX8_Texture_Stage_State(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
		gfx->Set_DX8_Texture_Stage_State(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
		gfx->Set_DX8_Texture_Stage_State(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
		gfx->Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
		gfx->Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
		gfx->Set_DX8_Texture_Stage_State(0, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
		gfx->Set_DX8_Texture_Stage_State(0, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
		gfx->Set_DX8_Texture_Stage_State(0, D3DTSS_MIPFILTER, mip_filter);
		gfx->Set_DX8_Texture_Stage_State(0, D3DTSS_ADDRESSU, D3DTADDRESS_WRAP);
		gfx->Set_DX8_Texture_Stage_State(0, D3DTSS_ADDRESSV, D3DTADDRESS_WRAP);
		gfx->Set_DX8_Texture_Stage_State(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
		gfx->Set_DX8_Texture_Stage_State(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

		for (size_t b = 0; b < batches.size(); ++b) {
			const zh::DrawBatch &batch = batches[b];
			gfx->Set_Transform(D3DTS_WORLD, to_d3d_matrix(batch.world));
			spike::MaterialState material;
			for (int i = 0; i < 3; ++i) {
				material.diffuse[i] = batch.material_diffuse[i];
				material.ambient[i] = batch.material_ambient[i];
			}
			material.diffuse[3] = batch.opacity;
			gfx->Set_Material(material);
			gfx->Set_DX8_Render_State(D3DRS_CULLMODE,
			                          batch.two_sided ? D3DCULL_NONE : cull_mode);
			gfx->Set_DX8_Render_State(D3DRS_ALPHATESTENABLE, batch.alpha_test ? 1 : 0);
			gfx->Set_Texture(0, batch.texture_index >= 0
			                        ? texture_handles[static_cast<size_t>(batch.texture_index)]
			                        : nullptr);
			gfx->Set_Vertex_Buffer(gpu_batches[b].vb, 0);
			gfx->Set_Index_Buffer(gpu_batches[b].ib, 0);
			gfx->Draw_Triangles(0, static_cast<uint32_t>(batch.indices.size() / 3), 0,
			                    static_cast<uint32_t>(batch.vertices.size()));
		}
		gfx->End_Scene(true);
		return gfx->Read_Back_Color_Target(out, format);
	};

	std::string unmipped;
	if (!draw_frame(D3DTEXF_NONE, unmipped) || !draw_frame(D3DTEXF_LINEAR, rgba)) {
		std::fprintf(stderr, "Read_Back_Color_Target failed\n");
		return 1;
	}
	if (!spike::Write_Png(options.out_path, rgba, format.width, format.height)) {
		std::fprintf(stderr, "failed to write %s\n", options.out_path.c_str());
		return 1;
	}
	if (!options.reference_path.empty()) {
		const std::string reference_rgba(reinterpret_cast<const char *>(reference.rgba.data()),
		                                reference.rgba.size());
		if (!spike::Write_Png(options.reference_path, reference_rgba, reference.width,
		                      reference.height)) {
			std::fprintf(stderr, "failed to write %s\n", options.reference_path.c_str());
			return 1;
		}
	}

	const zh::ImageComparison comparison = zh::compare_images(unmipped, reference, clear_rgb);
	zh::ReferenceImage unmipped_as_reference;
	unmipped_as_reference.width = format.width;
	unmipped_as_reference.height = format.height;
	unmipped_as_reference.rgba.assign(unmipped.begin(), unmipped.end());
	const zh::ImageComparison mip_effect =
	    zh::compare_images(rgba, unmipped_as_reference, clear_rgb);
	std::printf("\ndraws: %zu, pipelines: %u, validation messages: %u\n", batches.size(),
	            gfx->Pipeline_Count(), gfx->Validation_Message_Count());
	std::printf("wrote %s (%ux%u)%s%s\n", options.out_path.c_str(), format.width, format.height,
	            options.reference_path.empty() ? "" : " and ", options.reference_path.c_str());
	std::printf("\nGPU vs CPU reference (both with mip filtering off)\n");
	std::printf("  covered pixels        GPU %llu, reference %llu\n",
	            static_cast<unsigned long long>(comparison.gpu_covered),
	            static_cast<unsigned long long>(comparison.reference_covered));
	std::printf("  coverage IoU          %.4f\n", comparison.coverage_iou);
	std::printf("  mean |difference|     %.2f / 255 per channel over the overlap\n",
	            comparison.mean_abs_difference);
	std::printf("  pixels differing > 48 %llu (%.2f%% of the overlap)\n",
	            static_cast<unsigned long long>(comparison.large_difference_pixels),
	            comparison.intersection == 0
	                ? 0.0
	                : 100.0 * static_cast<double>(comparison.large_difference_pixels) /
	                      static_cast<double>(comparison.intersection));
	std::printf("effect of the mip chain (trilinear vs mip 0): coverage IoU %.4f, mean "
	            "|difference| %.2f\n",
	            mip_effect.coverage_iou, mip_effect.mean_abs_difference);

	if (!notes.empty() || !model.notes.empty()) {
		std::printf("\nnotes\n");
		for (const std::string &text : model.notes) {
			std::printf("  %s\n", text.c_str());
		}
		for (const std::string &text : notes) {
			std::printf("  %s\n", text.c_str());
		}
	}

	const uint32_t validation_messages = gfx->Validation_Message_Count();
	gfx->Shutdown();
	delete gfx;

	// The verdict is mechanical: the geometry must agree with the file's own headers, the two
	// independent rasterisations must agree with each other, and Vulkan must not have
	// complained. Unresolved textures are reported separately: retail archives can refer to
	// generated/runtime textures, and those meshes are omitted rather than drawn incorrectly.
	uint32_t failures = checks.failures;
	if (comparison.gpu_covered == 0) {
		std::fprintf(stderr, "FAIL: the GPU drew nothing\n");
		++failures;
	}
	if (comparison.coverage_iou < 0.97) {
		std::fprintf(stderr, "FAIL: GPU and CPU reference silhouettes disagree (IoU %.4f)\n",
		             comparison.coverage_iou);
		++failures;
	}
	if (comparison.mean_abs_difference > 6.0) {
		std::fprintf(stderr, "FAIL: GPU and CPU reference shading disagree (mean %.2f)\n",
		             comparison.mean_abs_difference);
		++failures;
	}
	if (options.validation && validation_messages != 0) {
		++failures;
	}
	if (checks.unresolved_textures != 0) {
		std::printf("PARTIAL: %u texture reference(s) unresolved; affected meshes were omitted\n",
		            checks.unresolved_textures);
	}
	std::printf("\n%s: %u mechanical check(s) failed\n", failures == 0 ? "PASS" : "FAIL",
	            failures);
	return failures == 0 ? 0 : 1;
}
