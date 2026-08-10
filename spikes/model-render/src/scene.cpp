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

#include "scene.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace zh
{

namespace
{

using spike::Matrix4x4;

// Westwood convention throughout: v' = M * v, so a composite is projection * view * world.
// This is the same convention DX8Wrapper hands to To_D3DMATRIX, which transposes on the way
// into D3D.
Matrix4x4 multiply(const Matrix4x4 &a, const Matrix4x4 &b)
{
	Matrix4x4 r{};
	for (int i = 0; i < 4; ++i) {
		for (int j = 0; j < 4; ++j) {
			float s = 0.0f;
			for (int k = 0; k < 4; ++k) {
				s += a.m[i][k] * b.m[k][j];
			}
			r.m[i][j] = s;
		}
	}
	return r;
}

void transform_point(const Matrix4x4 &m, const float in[3], float out[4])
{
	for (int i = 0; i < 4; ++i) {
		out[i] = m.m[i][0] * in[0] + m.m[i][1] * in[1] + m.m[i][2] * in[2] + m.m[i][3];
	}
}

void transform_direction(const Matrix4x4 &m, const float in[3], float out[3])
{
	for (int i = 0; i < 3; ++i) {
		out[i] = m.m[i][0] * in[0] + m.m[i][1] * in[1] + m.m[i][2] * in[2];
	}
}

void normalise(float v[3])
{
	const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	if (length > 1e-8f) {
		v[0] /= length;
		v[1] /= length;
		v[2] /= length;
	}
}

void cross(const float a[3], const float b[3], float out[3])
{
	out[0] = a[1] * b[2] - a[2] * b[1];
	out[1] = a[2] * b[0] - a[0] * b[2];
	out[2] = a[0] * b[1] - a[1] * b[0];
}

float dot(const float a[3], const float b[3])
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

uint32_t pack_argb(float r, float g, float b, float a)
{
	auto to_byte = [](float value) -> uint32_t {
		const float clamped = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
		return static_cast<uint32_t>(clamped * 255.0f + 0.5f);
	};
	return (to_byte(a) << 24) | (to_byte(r) << 16) | (to_byte(g) << 8) | to_byte(b);
}

Matrix4x4 world_from_mesh(const SubMesh &mesh)
{
	Matrix4x4 m = Matrix4x4::Identity();
	for (int row = 0; row < 3; ++row) {
		for (int col = 0; col < 4; ++col) {
			m.m[row][col] = mesh.world_transform[row * 4 + col];
		}
	}
	return m;
}

} // namespace

std::vector<DrawBatch> build_batches(const Model &model, const std::vector<TextureImage> &textures,
                                     const std::vector<int> &mesh_texture_index,
                                     const Light &light, std::vector<std::string> &notes)
{
	std::vector<DrawBatch> batches;
	float light_dir[3] = {light.direction[0], light.direction[1], light.direction[2]};
	normalise(light_dir);

	for (size_t mesh_index = 0; mesh_index < model.meshes.size(); ++mesh_index) {
		const SubMesh &mesh = model.meshes[mesh_index];
		if (mesh.hidden()) {
			notes.push_back("skipped hidden mesh " + mesh.name);
			continue;
		}
		if (model.rendered_lod >= 0 && !mesh.in_rendered_lod) {
			continue; // a lower-detail LOD's mesh
		}
		if (mesh.positions.empty() || mesh.indices.empty()) {
			notes.push_back("skipped mesh with no geometry: " + mesh.name);
			continue;
		}
		if (mesh.positions.size() / 3 > 65535) {
			notes.push_back("skipped mesh " + mesh.name +
			                ": more than 65535 vertices and the backend's index buffers are "
			                "16-bit");
			continue;
		}

		DrawBatch batch;
		batch.mesh_name = mesh.name;
		if (!mesh.passes.empty() && !mesh.passes.front().stages.empty() &&
		    !mesh.passes.front().stages.front().texture_name.empty() &&
		    (mesh_index >= mesh_texture_index.size() || mesh_texture_index[mesh_index] < 0)) {
			// Drawing it untextured would put a white surface where the artist put a texture,
			// which is exactly the kind of plausible-but-wrong pixel to avoid.
			notes.push_back("skipped mesh " + mesh.name + ": its texture " +
			                mesh.passes.front().stages.front().texture_name +
			                " did not load");
			continue;
		}
		batch.world = world_from_mesh(mesh);
		batch.two_sided = mesh.two_sided();
		batch.texture_index =
		    mesh_index < mesh_texture_index.size() ? mesh_texture_index[mesh_index] : -1;

		if (!mesh.passes.empty()) {
			const MaterialPass &pass = mesh.passes.front();
			for (int i = 0; i < 3; ++i) {
				batch.material_diffuse[i] = pass.diffuse[i];
				batch.material_ambient[i] = pass.ambient[i];
			}
			batch.opacity = pass.opacity;
			batch.alpha_test = pass.alpha_test != 0;
			// W3D shader blend: SrcBlend 1 / DestBlend 0 is opaque (SRC_ONE / DEST_ZERO).
			batch.alpha_blend = pass.dest_blend != 0;
			if (!pass.stages.empty()) {
				batch.texture_name = pass.stages.front().texture_name;
			}
			if (mesh.passes.size() > 1) {
				notes.push_back("mesh " + mesh.name + " has " +
				                std::to_string(mesh.passes.size()) +
				                " material passes; only pass 0 is drawn");
			}
		}

		const size_t vertex_count = mesh.positions.size() / 3;
		const bool have_normals = mesh.normals.size() == mesh.positions.size();
		const bool have_uvs = mesh.texcoords.size() == vertex_count * 2;
		if (!have_uvs && batch.texture_index >= 0) {
			notes.push_back("mesh " + mesh.name +
			                " has a texture but no stage-0 texture coordinates");
		}

		batch.vertices.resize(vertex_count);
		for (size_t v = 0; v < vertex_count; ++v) {
			ModelVertex &out = batch.vertices[v];
			out.x = mesh.positions[v * 3 + 0];
			out.y = mesh.positions[v * 3 + 1];
			out.z = mesh.positions[v * 3 + 2];
			float normal[3] = {0.0f, 0.0f, 1.0f};
			if (have_normals) {
				normal[0] = mesh.normals[v * 3 + 0];
				normal[1] = mesh.normals[v * 3 + 1];
				normal[2] = mesh.normals[v * 3 + 2];
			}
			out.nx = normal[0];
			out.ny = normal[1];
			out.nz = normal[2];
			if (have_uvs) {
				out.u = mesh.texcoords[v * 2 + 0];
				out.v = mesh.texcoords[v * 2 + 1];
			}

			// D3D8's lighting equation, evaluated here for the CPU reference only: the GPU
			// path sets D3DRS_LIGHTING and the same light and material through the backend,
			// which ignores the vertex colour under D3DMCS_MATERIAL. Two implementations of
			// one equation is the point -- it is what makes the comparison a check.
			float world_normal[3] = {normal[0], normal[1], normal[2]};
			transform_direction(batch.world, normal, world_normal);
			normalise(world_normal);
			float lambert = -dot(world_normal, light_dir);
			if (lambert < 0.0f) {
				lambert = 0.0f;
			}
			out.diffuse = pack_argb(batch.material_ambient[0] * light.ambient +
			                            batch.material_diffuse[0] * light.intensity * lambert,
			                        batch.material_ambient[1] * light.ambient +
			                            batch.material_diffuse[1] * light.intensity * lambert,
			                        batch.material_ambient[2] * light.ambient +
			                            batch.material_diffuse[2] * light.intensity * lambert,
			                        batch.opacity);
		}

		batch.indices.resize(mesh.indices.size());
		for (size_t i = 0; i < mesh.indices.size(); ++i) {
			batch.indices[i] = static_cast<uint16_t>(mesh.indices[i]);
		}
		if (batch.texture_index >= 0 &&
		    static_cast<size_t>(batch.texture_index) >= textures.size()) {
			batch.texture_index = -1;
		}
		batches.push_back(std::move(batch));
	}
	return batches;
}

Camera frame_batches(const std::vector<DrawBatch> &batches, uint32_t width, uint32_t height)
{
	float min[3] = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
	                std::numeric_limits<float>::max()};
	float max[3] = {-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(),
	                -std::numeric_limits<float>::max()};
	for (const DrawBatch &batch : batches) {
		for (const ModelVertex &vertex : batch.vertices) {
			const float local[3] = {vertex.x, vertex.y, vertex.z};
			float world[4];
			transform_point(batch.world, local, world);
			for (int i = 0; i < 3; ++i) {
				min[i] = std::min(min[i], world[i]);
				max[i] = std::max(max[i], world[i]);
			}
		}
	}

	Camera camera;
	for (int i = 0; i < 3; ++i) {
		camera.target[i] = (min[i] + max[i]) * 0.5f;
	}
	float extent[3] = {max[0] - min[0], max[1] - min[1], max[2] - min[2]};
	camera.radius = 0.5f * std::sqrt(extent[0] * extent[0] + extent[1] * extent[1] +
	                                 extent[2] * extent[2]);
	if (!(camera.radius > 0.0f)) {
		camera.radius = 1.0f;
	}

	// Three-quarter view. W3D is Z-up, so the elevation goes into +Z and the world up vector
	// is +Z -- get this wrong and the tank is drawn lying on its side.
	float direction[3] = {0.78f, -1.0f, 0.52f};
	normalise(direction);
	const float distance = camera.radius * 2.9f;
	for (int i = 0; i < 3; ++i) {
		camera.eye[i] = camera.target[i] + direction[i] * distance;
	}

	float forward[3] = {camera.target[0] - camera.eye[0], camera.target[1] - camera.eye[1],
	                    camera.target[2] - camera.eye[2]};
	normalise(forward);
	const float world_up[3] = {0.0f, 0.0f, 1.0f};
	float right[3];
	cross(forward, world_up, right);
	normalise(right);
	float up[3];
	cross(right, forward, up);
	normalise(up);

	Matrix4x4 view = Matrix4x4::Identity();
	for (int i = 0; i < 3; ++i) {
		view.m[0][i] = right[i];
		view.m[1][i] = up[i];
		view.m[2][i] = -forward[i];
	}
	view.m[0][3] = -dot(right, camera.eye);
	view.m[1][3] = -dot(up, camera.eye);
	view.m[2][3] = dot(forward, camera.eye);
	camera.view = view;

	// Right-handed perspective with clip z in [0,1]: D3D's depth range, which is also
	// Vulkan's. The view direction is -Z.
	const float near_plane = std::max(0.05f, camera.radius * 0.05f);
	const float far_plane = distance + camera.radius * 4.0f;
	const float fov_y = 32.0f * 3.14159265358979f / 180.0f;
	const float focal = 1.0f / std::tan(fov_y * 0.5f);
	const float aspect = static_cast<float>(width) / static_cast<float>(height);
	Matrix4x4 projection{};
	projection.m[0][0] = focal / aspect;
	projection.m[1][1] = focal;
	projection.m[2][2] = far_plane / (near_plane - far_plane);
	projection.m[2][3] = near_plane * far_plane / (near_plane - far_plane);
	projection.m[3][2] = -1.0f;
	camera.projection = projection;
	return camera;
}

namespace
{

struct Sample
{
	float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;
};

// Bilinear, wrapping, top-origin V -- the D3D8 texel addressing the engine relies on, which
// Vulkan shares. TGA files are flipped once at load time so this holds for them too.
Sample sample_texture(const std::vector<uint8_t> &rgba, uint32_t width, uint32_t height, float u,
                      float v)
{
	Sample out;
	if (rgba.empty() || width == 0 || height == 0) {
		return out;
	}
	const float x = u * static_cast<float>(width) - 0.5f;
	const float y = v * static_cast<float>(height) - 0.5f;
	const int x0 = static_cast<int>(std::floor(x));
	const int y0 = static_cast<int>(std::floor(y));
	const float fx = x - static_cast<float>(x0);
	const float fy = y - static_cast<float>(y0);
	auto wrap = [](int value, uint32_t size) -> uint32_t {
		const int n = static_cast<int>(size);
		int m = value % n;
		if (m < 0) {
			m += n;
		}
		return static_cast<uint32_t>(m);
	};
	float accum[4] = {0.0f, 0.0f, 0.0f, 0.0f};
	const float weights[4] = {(1.0f - fx) * (1.0f - fy), fx * (1.0f - fy), (1.0f - fx) * fy,
	                          fx * fy};
	const int offsets[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
	for (int i = 0; i < 4; ++i) {
		const uint32_t tx = wrap(x0 + offsets[i][0], width);
		const uint32_t ty = wrap(y0 + offsets[i][1], height);
		const size_t base = (static_cast<size_t>(ty) * width + tx) * 4;
		for (int c = 0; c < 4; ++c) {
			accum[c] += weights[i] * static_cast<float>(rgba[base + c]);
		}
	}
	out.r = accum[0] / 255.0f;
	out.g = accum[1] / 255.0f;
	out.b = accum[2] / 255.0f;
	out.a = accum[3] / 255.0f;
	return out;
}

} // namespace

ReferenceImage rasterise_reference(const std::vector<DrawBatch> &batches,
                                   const std::vector<TextureImage> &textures,
                                   const std::vector<std::vector<uint8_t>> &decoded_mip0,
                                   const Camera &camera, uint32_t width, uint32_t height,
                                   const float clear_rgb[3], ReferenceCull cull)
{
	ReferenceImage image;
	image.width = width;
	image.height = height;
	image.rgba.assign(static_cast<size_t>(width) * height * 4, 0);
	for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i) {
		image.rgba[i * 4 + 0] = static_cast<uint8_t>(clear_rgb[0] * 255.0f + 0.5f);
		image.rgba[i * 4 + 1] = static_cast<uint8_t>(clear_rgb[1] * 255.0f + 0.5f);
		image.rgba[i * 4 + 2] = static_cast<uint8_t>(clear_rgb[2] * 255.0f + 0.5f);
		image.rgba[i * 4 + 3] = 255;
	}
	std::vector<float> depth(static_cast<size_t>(width) * height, 1.0f);
	std::vector<int8_t> winding(static_cast<size_t>(width) * height, 0);

	const Matrix4x4 view_projection = multiply(camera.projection, camera.view);

	for (const DrawBatch &batch : batches) {
		const Matrix4x4 clip_from_object = multiply(view_projection, batch.world);
		const std::vector<uint8_t> *texels = nullptr;
		uint32_t texture_width = 0;
		uint32_t texture_height = 0;
		if (batch.texture_index >= 0) {
			texels = &decoded_mip0[static_cast<size_t>(batch.texture_index)];
			texture_width = textures[static_cast<size_t>(batch.texture_index)].width;
			texture_height = textures[static_cast<size_t>(batch.texture_index)].height;
		}

		for (size_t tri = 0; tri + 2 < batch.indices.size(); tri += 3) {
			const ModelVertex *vertices[3] = {&batch.vertices[batch.indices[tri + 0]],
			                                  &batch.vertices[batch.indices[tri + 1]],
			                                  &batch.vertices[batch.indices[tri + 2]]};
			float clip[3][4];
			bool behind = false;
			for (int i = 0; i < 3; ++i) {
				const float object[3] = {vertices[i]->x, vertices[i]->y, vertices[i]->z};
				transform_point(clip_from_object, object, clip[i]);
				if (clip[i][3] <= 1e-6f) {
					behind = true;
				}
			}
			if (behind) {
				continue; // no near-plane clipping in the reference; whole triangle dropped
			}

			// Screen space with y growing downward, matching the backend's Vulkan y-flip.
			float sx[3], sy[3], sz[3], inv_w[3];
			for (int i = 0; i < 3; ++i) {
				inv_w[i] = 1.0f / clip[i][3];
				const float ndc_x = clip[i][0] * inv_w[i];
				const float ndc_y = clip[i][1] * inv_w[i];
				sz[i] = clip[i][2] * inv_w[i];
				sx[i] = (ndc_x * 0.5f + 0.5f) * static_cast<float>(width);
				sy[i] = (0.5f - ndc_y * 0.5f) * static_cast<float>(height);
			}

			// Signed area in screen space, where y grows downward as it does in both D3D's
			// viewport and Vulkan's framebuffer: positive means the vertices run clockwise as
			// seen on screen. This is the sense D3D8's cull modes are named in.
			const float screen_area = 0.5f * ((sx[1] - sx[0]) * (sy[2] - sy[0]) -
			                                  (sx[2] - sx[0]) * (sy[1] - sy[0]));
			if (std::fabs(screen_area) < 1e-7f) {
				continue;
			}
			const int8_t orientation = screen_area > 0.0f ? 1 : -1;
			if (!batch.two_sided) {
				if ((cull == ReferenceCull::Clockwise && orientation > 0) ||
				    (cull == ReferenceCull::CounterClockwise && orientation < 0)) {
					continue;
				}
			}

			const int min_x = std::max(0, static_cast<int>(std::floor(std::min(
			                                  sx[0], std::min(sx[1], sx[2])))));
			const int max_x = std::min(static_cast<int>(width) - 1,
			                           static_cast<int>(std::ceil(std::max(
			                               sx[0], std::max(sx[1], sx[2])))));
			const int min_y = std::max(0, static_cast<int>(std::floor(std::min(
			                                  sy[0], std::min(sy[1], sy[2])))));
			const int max_y = std::min(static_cast<int>(height) - 1,
			                           static_cast<int>(std::ceil(std::max(
			                               sy[0], std::max(sy[1], sy[2])))));

			const float double_area = (sx[1] - sx[0]) * (sy[2] - sy[0]) -
			                          (sx[2] - sx[0]) * (sy[1] - sy[0]);
			for (int y = min_y; y <= max_y; ++y) {
				for (int x = min_x; x <= max_x; ++x) {
					const float px = static_cast<float>(x) + 0.5f;
					const float py = static_cast<float>(y) + 0.5f;
					float bary[3];
					bary[1] = ((px - sx[0]) * (sy[2] - sy[0]) - (py - sy[0]) * (sx[2] - sx[0])) /
					          double_area;
					bary[2] = ((py - sy[0]) * (sx[1] - sx[0]) - (px - sx[0]) * (sy[1] - sy[0])) /
					          double_area;
					bary[0] = 1.0f - bary[1] - bary[2];
					if (bary[0] < 0.0f || bary[1] < 0.0f || bary[2] < 0.0f) {
						continue;
					}

					const float z = bary[0] * sz[0] + bary[1] * sz[1] + bary[2] * sz[2];
					const size_t pixel = static_cast<size_t>(y) * width + static_cast<size_t>(x);
					if (z < 0.0f || z > 1.0f || z > depth[pixel]) {
						continue;
					}

					const float w_reciprocal =
					    bary[0] * inv_w[0] + bary[1] * inv_w[1] + bary[2] * inv_w[2];
					float perspective[3];
					for (int i = 0; i < 3; ++i) {
						perspective[i] = bary[i] * inv_w[i] / w_reciprocal;
					}

					Sample texel;
					if (texels != nullptr) {
						const float u = perspective[0] * vertices[0]->u +
						                perspective[1] * vertices[1]->u +
						                perspective[2] * vertices[2]->u;
						const float v = perspective[0] * vertices[0]->v +
						                perspective[1] * vertices[1]->v +
						                perspective[2] * vertices[2]->v;
						texel = sample_texture(*texels, texture_width, texture_height, u, v);
					}
					if (batch.alpha_test && texel.a < 0.5f) {
						continue;
					}

					float diffuse[4] = {0.0f, 0.0f, 0.0f, 0.0f};
					for (int i = 0; i < 3; ++i) {
						const uint32_t packed = vertices[i]->diffuse;
						diffuse[0] += perspective[i] * static_cast<float>((packed >> 16) & 0xff);
						diffuse[1] += perspective[i] * static_cast<float>((packed >> 8) & 0xff);
						diffuse[2] += perspective[i] * static_cast<float>(packed & 0xff);
						diffuse[3] += perspective[i] * static_cast<float>((packed >> 24) & 0xff);
					}

					const float colour[3] = {texel.r * diffuse[0], texel.g * diffuse[1],
					                         texel.b * diffuse[2]};
					for (int c = 0; c < 3; ++c) {
						const float clamped = colour[c] < 0.0f ? 0.0f
						                                      : (colour[c] > 255.0f ? 255.0f
						                                                            : colour[c]);
						image.rgba[pixel * 4 + static_cast<size_t>(c)] =
						    static_cast<uint8_t>(clamped + 0.5f);
					}
					image.rgba[pixel * 4 + 3] = 255;
					depth[pixel] = z;
					winding[pixel] = orientation;
				}
			}
		}
	}

	for (size_t pixel = 0; pixel < winding.size(); ++pixel) {
		if (winding[pixel] == 0) {
			continue;
		}
		++image.covered_pixels;
		if (winding[pixel] > 0) {
			++image.visible_cw_pixels;
		} else {
			++image.visible_ccw_pixels;
		}
	}
	return image;
}

ImageComparison compare_images(const std::string &gpu_rgba, const ReferenceImage &reference,
                               const float clear_rgb[3])
{
	ImageComparison result;
	const uint8_t clear[3] = {static_cast<uint8_t>(clear_rgb[0] * 255.0f + 0.5f),
	                          static_cast<uint8_t>(clear_rgb[1] * 255.0f + 0.5f),
	                          static_cast<uint8_t>(clear_rgb[2] * 255.0f + 0.5f)};
	auto covered = [&clear](const uint8_t *pixel) {
		for (int c = 0; c < 3; ++c) {
			const int difference = static_cast<int>(pixel[c]) - static_cast<int>(clear[c]);
			if (difference > 8 || difference < -8) {
				return true;
			}
		}
		return false;
	};

	const size_t pixels = static_cast<size_t>(reference.width) * reference.height;
	if (gpu_rgba.size() < pixels * 4) {
		return result;
	}
	const uint8_t *gpu = reinterpret_cast<const uint8_t *>(gpu_rgba.data());
	double difference_sum = 0.0;
	for (size_t pixel = 0; pixel < pixels; ++pixel) {
		const uint8_t *gpu_pixel = gpu + pixel * 4;
		const uint8_t *reference_pixel = reference.rgba.data() + pixel * 4;
		const bool gpu_covered = covered(gpu_pixel);
		const bool reference_covered = covered(reference_pixel);
		result.gpu_covered += gpu_covered ? 1 : 0;
		result.reference_covered += reference_covered ? 1 : 0;
		if (gpu_covered && reference_covered) {
			++result.intersection;
			int worst = 0;
			for (int c = 0; c < 3; ++c) {
				const int difference = std::abs(static_cast<int>(gpu_pixel[c]) -
				                                static_cast<int>(reference_pixel[c]));
				difference_sum += static_cast<double>(difference);
				worst = std::max(worst, difference);
			}
			if (worst > 48) {
				++result.large_difference_pixels;
			}
		}
		if (gpu_covered || reference_covered) {
			++result.union_count;
		}
	}
	result.coverage_iou = result.union_count == 0
	                          ? 0.0
	                          : static_cast<double>(result.intersection) /
	                                static_cast<double>(result.union_count);
	result.mean_abs_difference = result.intersection == 0
	                                 ? 0.0
	                                 : difference_sum / static_cast<double>(result.intersection * 3);
	return result;
}

} // namespace zh
