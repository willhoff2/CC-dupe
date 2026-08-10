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

// Turns a parsed W3D model into draw batches, a camera, and -- separately from the GPU -- a
// CPU reference image of the same batches.
//
// The CPU reference exists to answer the question the GPU image cannot answer about itself:
// whether the picture is *right*. It runs the same matrices through independent scalar code,
// rasterises with its own depth buffer, and samples the same decoded texels, so a
// disagreement between the two images means one of the two is wrong. It also reports the
// screen-space winding of the visible surface, which is what decides the D3D cull mode.
#pragma once

#include "texture_load.h"
#include "w3d_model.h"

#include "render_backend.h"

#include <cstdint>
#include <string>
#include <vector>

namespace zh
{

// D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1, in FVF order.
struct ModelVertex
{
	float x = 0.0f, y = 0.0f, z = 0.0f;
	float nx = 0.0f, ny = 0.0f, nz = 0.0f;
	uint32_t diffuse = 0xffffffffu;
	float u = 0.0f, v = 0.0f;
};

struct DrawBatch
{
	std::string mesh_name;
	std::string texture_name;    // as referenced by the .w3d
	int texture_index = -1;      // into the caller's texture array, -1 for untextured
	bool alpha_test = false;
	bool two_sided = false;
	bool alpha_blend = false;
	// The W3D vertex material, handed to the backend as D3D8 material state. The CPU
	// reference evaluates the light itself from these, so the two agree only if both
	// implement D3D8's lighting equation.
	float material_diffuse[3] = {1.0f, 1.0f, 1.0f};
	float material_ambient[3] = {1.0f, 1.0f, 1.0f};
	float opacity = 1.0f;
	std::vector<ModelVertex> vertices;
	std::vector<uint16_t> indices;
	spike::Matrix4x4 world = spike::Matrix4x4::Identity();
};

struct Camera
{
	spike::Matrix4x4 view = spike::Matrix4x4::Identity();
	spike::Matrix4x4 projection = spike::Matrix4x4::Identity();
	float eye[3] = {0.0f, 0.0f, 0.0f};
	float target[3] = {0.0f, 0.0f, 0.0f};
	float radius = 1.0f;
};

// The single directional light. The GPU path passes it to the backend as a D3DLIGHT8; the CPU
// reference evaluates the same equation itself into each vertex's diffuse colour.
struct Light
{
	float direction[3] = {-0.5f, 0.6f, -0.62f}; // pointing *at* the model, world space
	float ambient = 0.35f;
	float intensity = 0.85f;
};

// One batch per (mesh, material pass 0). Vertices keep W3D object space; the pivot transform
// stays in `world` so the backend's transform path is what places the mesh.
std::vector<DrawBatch> build_batches(const Model &model, const std::vector<TextureImage> &textures,
                                     const std::vector<int> &mesh_texture_index,
                                     const Light &light, std::vector<std::string> &notes);

// Frames every batch: a three-quarter view from +X/-Y/+Z, W3D's Z being up.
Camera frame_batches(const std::vector<DrawBatch> &batches, uint32_t width, uint32_t height);

struct ReferenceImage
{
	std::vector<uint8_t> rgba; // width*height*4, top row first
	uint32_t width = 0;
	uint32_t height = 0;
	uint64_t covered_pixels = 0;
	// Screen-space winding of the triangle that won the depth test, per covered pixel.
	uint64_t visible_ccw_pixels = 0;
	uint64_t visible_cw_pixels = 0;
};

// Which screen-space winding the reference discards, in the same sense as D3D8's cull modes.
enum class ReferenceCull
{
	None,
	Clockwise,
	CounterClockwise,
};

// Rasterises `batches` with scalar code: depth-buffered, textured, top-origin V.
ReferenceImage rasterise_reference(const std::vector<DrawBatch> &batches,
                                   const std::vector<TextureImage> &textures,
                                   const std::vector<std::vector<uint8_t>> &decoded_mip0,
                                   const Camera &camera, uint32_t width, uint32_t height,
                                   const float clear_rgb[3], ReferenceCull cull);

struct ImageComparison
{
	uint64_t gpu_covered = 0;
	uint64_t reference_covered = 0;
	uint64_t intersection = 0;
	uint64_t union_count = 0;
	double coverage_iou = 0.0;
	double mean_abs_difference = 0.0; // over the intersection, 0..255 per channel
	uint64_t large_difference_pixels = 0; // channel difference > 48
};

ImageComparison compare_images(const std::string &gpu_rgba, const ReferenceImage &reference,
                               const float clear_rgb[3]);

} // namespace zh
