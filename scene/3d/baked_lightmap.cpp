/*************************************************************************/
/*  baked_lightmap.cpp                                                   */
/*************************************************************************/
/*                       This file is part of:                           */
/*                           GODOT ENGINE                                */
/*                      https://godotengine.org                          */
/*************************************************************************/
/* Copyright (c) 2007-2020 Juan Linietsky, Ariel Manzur.                 */
/* Copyright (c) 2014-2020 Godot Engine contributors (cf. AUTHORS.md).   */
/*                                                                       */
/* Permission is hereby granted, free of charge, to any person obtaining */
/* a copy of this software and associated documentation files (the       */
/* "Software"), to deal in the Software without restriction, including   */
/* without limitation the rights to use, copy, modify, merge, publish,   */
/* distribute, sublicense, and/or sell copies of the Software, and to    */
/* permit persons to whom the Software is furnished to do so, subject to */
/* the following conditions:                                             */
/*                                                                       */
/* The above copyright notice and this permission notice shall be        */
/* included in all copies or substantial portions of the Software.       */
/*                                                                       */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,       */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF    */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.*/
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY  */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,  */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE     */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                */
/*************************************************************************/

#include "baked_lightmap.h"
#include "core/io/config_file.h"
#include "core/io/resource_saver.h"
#include "core/math/math_defs.h"
#include "core/os/dir_access.h"
#include "core/os/os.h"
#include "core/os/threaded_array_processor.h"
#include "modules/denoise/lightmap_denoiser.h"
#include "voxel_light_baker.h"

#include <random>

void BakedLightmapData::set_bounds(const AABB &p_bounds) {

	bounds = p_bounds;
	VS::get_singleton()->lightmap_capture_set_bounds(baked_light, p_bounds);
}

AABB BakedLightmapData::get_bounds() const {

	return bounds;
}

void BakedLightmapData::set_octree(const PoolVector<uint8_t> &p_octree) {

	VS::get_singleton()->lightmap_capture_set_octree(baked_light, p_octree);
}

PoolVector<uint8_t> BakedLightmapData::get_octree() const {

	return VS::get_singleton()->lightmap_capture_get_octree(baked_light);
}

void BakedLightmapData::set_cell_space_transform(const Transform &p_xform) {

	cell_space_xform = p_xform;
	VS::get_singleton()->lightmap_capture_set_octree_cell_transform(baked_light, p_xform);
}

Transform BakedLightmapData::get_cell_space_transform() const {
	return cell_space_xform;
}

void BakedLightmapData::set_cell_subdiv(int p_cell_subdiv) {
	cell_subdiv = p_cell_subdiv;
	VS::get_singleton()->lightmap_capture_set_octree_cell_subdiv(baked_light, p_cell_subdiv);
}

int BakedLightmapData::get_cell_subdiv() const {
	return cell_subdiv;
}

void BakedLightmapData::set_energy(float p_energy) {

	energy = p_energy;
	VS::get_singleton()->lightmap_capture_set_energy(baked_light, energy);
}

float BakedLightmapData::get_energy() const {

	return energy;
}

void BakedLightmapData::add_user(const NodePath &p_path, const Ref<Resource> &p_lightmap, int p_lightmap_slice, const Rect2 &p_lightmap_uv_rect, int p_instance) {

	ERR_FAIL_COND_MSG(p_lightmap.is_null(), "It's not a reference to a valid Texture object.");
	ERR_FAIL_COND(p_lightmap_slice == -1 && !Object::cast_to<Texture>(p_lightmap.ptr()));
	ERR_FAIL_COND(p_lightmap_slice != -1 && !Object::cast_to<TextureLayered>(p_lightmap.ptr()));

	User user;
	user.path = p_path;
	if (p_lightmap_slice == -1) {
		user.lightmap.single = p_lightmap;
	} else {
		user.lightmap.layered = p_lightmap;
	}
	user.lightmap_slice = p_lightmap_slice;
	user.lightmap_uv_rect = p_lightmap_uv_rect;
	user.instance_index = p_instance;
	users.push_back(user);
}

int BakedLightmapData::get_user_count() const {

	return users.size();
}
NodePath BakedLightmapData::get_user_path(int p_user) const {

	ERR_FAIL_INDEX_V(p_user, users.size(), NodePath());
	return users[p_user].path;
}
Ref<Resource> BakedLightmapData::get_user_lightmap(int p_user) const {

	ERR_FAIL_INDEX_V(p_user, users.size(), Ref<Resource>());
	if (users[p_user].lightmap_slice == -1) {
		return users[p_user].lightmap.single;
	} else {
		return users[p_user].lightmap.layered;
	}
}

int BakedLightmapData::get_user_lightmap_slice(int p_user) const {

	ERR_FAIL_INDEX_V(p_user, users.size(), -1);
	return users[p_user].lightmap_slice;
}

Rect2 BakedLightmapData::get_user_lightmap_uv_rect(int p_user) const {

	ERR_FAIL_INDEX_V(p_user, users.size(), Rect2(0, 0, 1, 1));
	return users[p_user].lightmap_uv_rect;
}

int BakedLightmapData::get_user_instance(int p_user) const {

	ERR_FAIL_INDEX_V(p_user, users.size(), -1);
	return users[p_user].instance_index;
}

void BakedLightmapData::clear_users() {
	users.clear();
}

void BakedLightmapData::_set_user_data(const Array &p_data) {

	// Detect old lightmapper format
	if (p_data.size() % 3 == 0) {
		bool is_old_format = true;
		for (int i = 0; i < p_data.size(); i += 3) {
			is_old_format = is_old_format && p_data[i + 0].get_type() == Variant::NODE_PATH;
			is_old_format = is_old_format && p_data[i + 1].is_ref();
			is_old_format = is_old_format && p_data[i + 2].get_type() == Variant::INT;
			if (!is_old_format) {
				break;
			}
		}
		if (is_old_format) {
#ifdef DEBUG_ENABLED
			WARN_PRINTS("Geometry at path " + String(p_data[0]) + " is using old lightmapper data. Please re-bake.");
#endif
			Array adapted_data;
			adapted_data.resize((p_data.size() / 3) * 5);
			for (int i = 0; i < p_data.size() / 3; i++) {
				adapted_data[i * 5 + 0] = p_data[i * 3 + 0];
				adapted_data[i * 5 + 1] = p_data[i * 3 + 1];
				adapted_data[i * 5 + 2] = -1;
				adapted_data[i * 5 + 3] = Rect2(0, 0, 1, 1);
				adapted_data[i * 5 + 4] = p_data[i * 3 + 2];
			}
			_set_user_data(adapted_data);
			return;
		}
	}

	ERR_FAIL_COND((p_data.size() % 5) != 0);

	for (int i = 0; i < p_data.size(); i += 5) {
		add_user(p_data[i], p_data[i + 1], p_data[i + 2], p_data[i + 3], p_data[i + 4]);
	}
}

Array BakedLightmapData::_get_user_data() const {

	Array ret;
	for (int i = 0; i < users.size(); i++) {
		ret.push_back(users[i].path);
		ret.push_back(users[i].lightmap_slice == -1 ? Ref<Resource>(users[i].lightmap.single) : Ref<Resource>(users[i].lightmap.layered));
		ret.push_back(users[i].lightmap_slice);
		ret.push_back(users[i].lightmap_uv_rect);
		ret.push_back(users[i].instance_index);
	}
	return ret;
}

RID BakedLightmapData::get_rid() const {
	return baked_light;
}
void BakedLightmapData::_bind_methods() {

	ClassDB::bind_method(D_METHOD("_set_user_data", "data"), &BakedLightmapData::_set_user_data);
	ClassDB::bind_method(D_METHOD("_get_user_data"), &BakedLightmapData::_get_user_data);

	ClassDB::bind_method(D_METHOD("set_bounds", "bounds"), &BakedLightmapData::set_bounds);
	ClassDB::bind_method(D_METHOD("get_bounds"), &BakedLightmapData::get_bounds);

	ClassDB::bind_method(D_METHOD("set_cell_space_transform", "xform"), &BakedLightmapData::set_cell_space_transform);
	ClassDB::bind_method(D_METHOD("get_cell_space_transform"), &BakedLightmapData::get_cell_space_transform);

	ClassDB::bind_method(D_METHOD("set_cell_subdiv", "cell_subdiv"), &BakedLightmapData::set_cell_subdiv);
	ClassDB::bind_method(D_METHOD("get_cell_subdiv"), &BakedLightmapData::get_cell_subdiv);

	ClassDB::bind_method(D_METHOD("set_octree", "octree"), &BakedLightmapData::set_octree);
	ClassDB::bind_method(D_METHOD("get_octree"), &BakedLightmapData::get_octree);

	ClassDB::bind_method(D_METHOD("set_energy", "energy"), &BakedLightmapData::set_energy);
	ClassDB::bind_method(D_METHOD("get_energy"), &BakedLightmapData::get_energy);

	ClassDB::bind_method(D_METHOD("add_user", "path", "lightmap", "lightmap_slice", "lightmap_uv_rect", "instance"), &BakedLightmapData::add_user);
	ClassDB::bind_method(D_METHOD("get_user_count"), &BakedLightmapData::get_user_count);
	ClassDB::bind_method(D_METHOD("get_user_path", "user_idx"), &BakedLightmapData::get_user_path);
	ClassDB::bind_method(D_METHOD("get_user_lightmap", "user_idx"), &BakedLightmapData::get_user_lightmap);
	ClassDB::bind_method(D_METHOD("clear_users"), &BakedLightmapData::clear_users);

	ADD_PROPERTY(PropertyInfo(Variant::AABB, "bounds", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NOEDITOR), "set_bounds", "get_bounds");
	ADD_PROPERTY(PropertyInfo(Variant::TRANSFORM, "cell_space_transform", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NOEDITOR), "set_cell_space_transform", "get_cell_space_transform");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "cell_subdiv", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NOEDITOR), "set_cell_subdiv", "get_cell_subdiv");
	ADD_PROPERTY(PropertyInfo(Variant::REAL, "energy", PROPERTY_HINT_RANGE, "0,16,0.01,or_greater"), "set_energy", "get_energy");
	ADD_PROPERTY(PropertyInfo(Variant::POOL_BYTE_ARRAY, "octree", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NOEDITOR), "set_octree", "get_octree");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "user_data", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NOEDITOR | PROPERTY_USAGE_INTERNAL), "_set_user_data", "_get_user_data");
}

BakedLightmapData::BakedLightmapData() {

	baked_light = VS::get_singleton()->lightmap_capture_create();
	energy = 1;
	cell_subdiv = 1;
}

BakedLightmapData::~BakedLightmapData() {

	VS::get_singleton()->free(baked_light);
}

///////////////////////////

BakedLightmap::BakeBeginFunc BakedLightmap::bake_begin_function = NULL;
BakedLightmap::BakeStepFunc BakedLightmap::bake_step_function = NULL;
BakedLightmap::BakeEndFunc BakedLightmap::bake_end_function = NULL;
BakedLightmap::BakeStepFunc BakedLightmap::bake_substep_function = NULL;
BakedLightmap::BakeEndFunc BakedLightmap::bake_end_substep_function = NULL;

#ifdef TOOLS_ENABLED
bool RaytraceLightBaker::_bake_time(float p_secs, float p_progress) {

	static uint64_t last_update = 0;

	if (BakedLightmap::bake_substep_function == NULL) {
		return false;
	}

	uint64_t time = OS::get_singleton()->get_ticks_usec();

	if (time - last_update > 500000) {

		last_update = time;

		String time_left = "-:- s";
		if (p_secs >= 0.0f) {
			int mins_left = p_secs / 60;
			int secs_left = Math::fmod(p_secs, 60.0f);
			time_left = vformat("%d:%02d s", mins_left, secs_left);
		}

		int percent = p_progress * 100;
		return BakedLightmap::bake_substep_function(percent, vformat(RTR("Time Left: %s"), time_left));
	}

	return false;
}

void RaytraceLightBaker::_get_material_images(RaytraceLightBaker::PlotMesh &r_plot_mesh) {

	for (int i = 0; i < r_plot_mesh.surface_materials.size(); ++i) {
		Ref<SpatialMaterial> mat = r_plot_mesh.surface_materials[i];

		r_plot_mesh.albedo_images.push_back(Ref<Image>());
		r_plot_mesh.emission_images.push_back(Ref<Image>());

		if (mat.is_valid()) {
			Ref<Texture> albedo_tex = mat->get_texture(SpatialMaterial::TEXTURE_ALBEDO);

			if (albedo_tex.is_valid()) {
				r_plot_mesh.albedo_images.write[i] = albedo_tex->get_data();
			}

			Ref<Texture> emission_tex = mat->get_texture(SpatialMaterial::TEXTURE_EMISSION);
			if (emission_tex.is_valid()) {
				r_plot_mesh.emission_images.write[i] = emission_tex->get_data();
			}
		}
	}
}

void RaytraceLightBaker::_get_material_textures(RaytraceLightBaker::PlotMesh &r_plot_mesh) {

	for (int i = 0; i < r_plot_mesh.surface_materials.size(); ++i) {
		Ref<SpatialMaterial> mat = r_plot_mesh.surface_materials[i];

		Size2i albedo_texture_size;
		Size2i emission_texture_size;
		r_plot_mesh.albedo_textures.push_back(Vector<Color>());
		r_plot_mesh.emission_textures.push_back(Vector<Color>());

		if (mat.is_valid()) {

			Ref<Image> albedo_image = r_plot_mesh.albedo_images[i];

			if (albedo_image.is_valid()) {
				albedo_texture_size = albedo_image->get_size();
				r_plot_mesh.albedo_textures.write[i] = _get_bake_texture(albedo_image, mat->get_albedo(), Color(0, 0, 0)); // albedo texture, color is multiplicative
			} else {
				albedo_texture_size = Size2i(8, 8);
				_get_solid_texture(mat->get_albedo(), albedo_texture_size, r_plot_mesh.albedo_textures.write[i]);
			}

			Ref<Image> emission_image = r_plot_mesh.emission_images[i];

			Color emission_col = mat->get_emission();
			float emission_energy = mat->get_emission_energy();

			if (emission_image.is_valid()) {
				emission_texture_size = emission_image->get_size();
				if (mat->get_emission_operator() == SpatialMaterial::EMISSION_OP_ADD) {
					r_plot_mesh.emission_textures.write[i] = _get_bake_texture(emission_image, Color(1, 1, 1) * emission_energy, emission_col * emission_energy);
				} else {
					r_plot_mesh.emission_textures.write[i] = _get_bake_texture(emission_image, emission_col * emission_energy, Color(0, 0, 0));
				}
			} else {
				emission_texture_size = Size2i(8, 8);
				if (mat->get_emission_operator() == SpatialMaterial::EMISSION_OP_ADD) {
					_get_solid_texture(emission_col * emission_energy, albedo_texture_size, r_plot_mesh.emission_textures.write[i]);
				} else {
					_get_solid_texture(Color(0, 0, 0), albedo_texture_size, r_plot_mesh.emission_textures.write[i]);
				}
			}
		} else {
			albedo_texture_size = Size2i(8, 8);
			_get_solid_texture(Color(1, 1, 1), albedo_texture_size, r_plot_mesh.albedo_textures.write[i]);

			emission_texture_size = Size2i(8, 8);
			_get_solid_texture(Color(0, 0, 0), emission_texture_size, r_plot_mesh.emission_textures.write[i]);
		}

		r_plot_mesh.albedo_texture_sizes.push_back(albedo_texture_size);
		r_plot_mesh.emission_texture_sizes.push_back(emission_texture_size);
	}
}

void RaytraceLightBaker::_clear_material_textures(PlotMesh &r_plot_mesh) {
	r_plot_mesh.albedo_textures.clear();
	r_plot_mesh.albedo_texture_sizes.clear();
	r_plot_mesh.albedo_images.clear();
	r_plot_mesh.emission_textures.clear();
	r_plot_mesh.emission_texture_sizes.clear();
	r_plot_mesh.emission_images.clear();
}

void RaytraceLightBaker::_find_meshes_and_lights(Node *p_from_node) {

	MeshInstance *mi = Object::cast_to<MeshInstance>(p_from_node);
	if (mi && mi->get_flag(GeometryInstance::FLAG_USE_BAKED_LIGHT) && mi->is_visible_in_tree()) {
		Ref<Mesh> mesh = mi->get_mesh();
		if (mesh.is_valid()) {

			bool all_have_uv2 = true;
			for (int i = 0; i < mesh->get_surface_count(); i++) {
				if (!(mesh->surface_get_format(i) & Mesh::ARRAY_FORMAT_TEX_UV2)) {
					all_have_uv2 = false;
					break;
				}
			}

			if (all_have_uv2) {

				AABB aabb = mesh->get_aabb();

				Transform xf = mi->get_global_transform();

				if (global_bounds.intersects(xf.xform(aabb))) {
					PlotMesh pm;
					pm.local_xform = xf;
					pm.size_hint = mi->get_lightmap_size_hint();
					pm.node = mi;
					pm.instance_idx = -1;
					pm.mesh = mesh;
					for (int i = 0; i < mesh->get_surface_count(); i++) {
						pm.surface_primitive_types.push_back(mesh->surface_get_primitive_type(i));
						pm.surface_arrays.push_back(mesh->surface_get_arrays(i));

						Ref<Material> mat;
						if (mi->get_material_override().is_valid()) {
							mat = mi->get_material_override();
						} else if (mi->get_surface_material(i).is_valid()) {
							mat = mi->get_surface_material(i);
						} else if (mesh->surface_get_material(i).is_valid()) {
							mat = mesh->surface_get_material(i);
						}

						pm.surface_materials.push_back(mat);
					}

					pm.cast_shadows = mi->get_bake_cast_shadows();
					pm.save_lightmap = mi->get_generate_lightmap();
					mesh_list.push_back(pm);
				}
			}
		}
	}

	Spatial *s = Object::cast_to<Spatial>(p_from_node);

	if (!mi && s) {
		Array meshes = p_from_node->call("get_bake_meshes");
		if (meshes.size() && (meshes.size() & 1) == 0) {
			Transform xf = s->get_global_transform();
			for (int i = 0; i < meshes.size(); i += 2) {
				PlotMesh pm;
				Transform mesh_xf = meshes[i + 1];
				pm.local_xform = xf * mesh_xf;
				pm.size_hint = Size2i();
				pm.node = s;
				pm.mesh = meshes[i];
				if (!pm.mesh.is_valid())
					continue;

				for (int j = 0; j < pm.mesh->get_surface_count(); j++) {
					pm.surface_primitive_types.push_back(pm.mesh->surface_get_primitive_type(j));
					pm.surface_arrays.push_back(pm.mesh->surface_get_arrays(j));

					Ref<Material> mat;
					if (pm.mesh->surface_get_material(i).is_valid()) {
						mat = pm.mesh->surface_get_material(i);
					}
					pm.surface_materials.push_back(mat);
				}
				pm.instance_idx = i / 2;
				GeometryInstance *gi = Object::cast_to<GeometryInstance>(s);

				if (gi) {
					pm.cast_shadows = gi->get_bake_cast_shadows();
					pm.save_lightmap = gi->get_generate_lightmap();
				}

				mesh_list.push_back(pm);
			}
		}
	}

	Light *light = Object::cast_to<Light>(p_from_node);

	if (light && light->get_bake_mode() != Light::BAKE_DISABLED) {
		PlotLight pl;
		Transform xf = light->get_global_transform();

		pl.global_xform = xf;
		pl.light = light;
		light_list.push_back(pl);
	}

	for (int i = 0; i < p_from_node->get_child_count(); i++) {

		Node *child = p_from_node->get_child(i);
		if (!child->get_owner())
			continue; //maybe a helper

		_find_meshes_and_lights(child);
	}
}

void RaytraceLightBaker::_get_solid_texture(const Color &p_color, const Size2i &p_size, Vector<Color> &r_texture) {
	int size = p_size.x * p_size.y;
	r_texture.resize(size);
	Color *ptr = r_texture.ptrw();

	for (int i = 0; i < size; i++) {
		ptr[i] = p_color;
	}
}

Vector<Color> RaytraceLightBaker::_get_bake_texture(Ref<Image> p_image, const Color &p_color_mul, const Color &p_color_add) {

	Vector<Color> ret;

	int width = p_image->get_width();
	int height = p_image->get_height();
	int size = width * height;
	ret.resize(size);

	p_image = p_image->duplicate();

	if (p_image->is_compressed()) {
		p_image->decompress();
	}

	p_image->convert(Image::FORMAT_RGBA8);

	PoolVector<uint8_t>::Read r = p_image->get_data().read();
	Color *ret_ptr = ret.ptrw();

	for (int i = 0; i < size; i++) {
		Color c;
		c.r = (r[i * 4 + 0] / 255.0) * p_color_mul.r + p_color_add.r;
		c.g = (r[i * 4 + 1] / 255.0) * p_color_mul.g + p_color_add.g;
		c.b = (r[i * 4 + 2] / 255.0) * p_color_mul.b + p_color_add.b;

		c.a = r[i * 4 + 3] / 255.0;

		ret_ptr[i] = c;
	}

	return ret;
}

Size2i RaytraceLightBaker::_compute_lightmap_size(const PlotMesh &p_plot_mesh) {
	double area = 0;
	double uv_area = 0;
	for (int i = 0; i < p_plot_mesh.surface_arrays.size(); i++) {
		Array arrays = p_plot_mesh.surface_arrays[i];
		PoolVector<Vector3> vertices = arrays[Mesh::ARRAY_VERTEX];
		PoolVector<Vector2> uv2 = arrays[Mesh::ARRAY_TEX_UV2];
		PoolVector<int> indices = arrays[Mesh::ARRAY_INDEX];

		ERR_FAIL_COND_V(vertices.size() == 0, Vector2());
		ERR_FAIL_COND_V(uv2.size() == 0, Vector2());

		int vc = vertices.size();
		PoolVector<Vector3>::Read vr = vertices.read();
		PoolVector<Vector2>::Read u2r = uv2.read();
		PoolVector<int>::Read ir;
		int ic = 0;

		if (indices.size()) {
			ic = indices.size();
			ir = indices.read();
		}

		int faces = ic ? ic / 3 : vc / 3;
		for (int j = 0; j < faces; j++) {
			Vector3 vertex[3];
			Vector2 uv[3];

			for (int k = 0; k < 3; k++) {
				int idx = ic ? ir[j * 3 + k] : j * 3 + k;
				vertex[k] = p_plot_mesh.local_xform.xform(vr[idx]);
				uv[k] = u2r[idx];
			}

			Vector3 p1 = vertex[0];
			Vector3 p2 = vertex[1];
			Vector3 p3 = vertex[2];
			double a = p1.distance_to(p2);
			double b = p2.distance_to(p3);
			double c = p3.distance_to(p1);
			double halfPerimeter = (a + b + c) / 2.0;
			area += sqrt(halfPerimeter * (halfPerimeter - a) * (halfPerimeter - b) * (halfPerimeter - c));

			Vector2 uv_p1 = uv[0];
			Vector2 uv_p2 = uv[1];
			Vector2 uv_p3 = uv[2];
			double uv_a = uv_p1.distance_to(uv_p2);
			double uv_b = uv_p2.distance_to(uv_p3);
			double uv_c = uv_p3.distance_to(uv_p1);
			double uv_halfPerimeter = (uv_a + uv_b + uv_c) / 2.0;
			uv_area += sqrt(
					uv_halfPerimeter * (uv_halfPerimeter - uv_a) * (uv_halfPerimeter - uv_b) * (uv_halfPerimeter - uv_c));
		}
	}

	if (uv_area < 0.0001f) {
		uv_area = 1.0;
	}

	int pixels = Math::round(ceil((1.0 / sqrt(uv_area)) * sqrt(area * default_texels_per_unit)));
	int size = CLAMP(pixels, 2, 4096);
	return Size2i(size, size);
}

void RaytraceLightBaker::_make_lightmap(uint32_t p_idx, int p_base_idx) {
	int idx = p_base_idx + p_idx;
	PlotMesh &plot_mesh = mesh_list[idx];

	plot_mesh.invalid_uv2s = false;

	_get_material_textures(plot_mesh);

	const Size2i &size = scene_lightmap_sizes[idx];

	int buffer_size = size.x * size.y;

	LocalVector<LightMapElement> &lightmap = scene_lightmaps[idx];
	LocalVector<int> &lightmap_indices = scene_lightmap_indices[idx];

	lightmap_indices.resize(buffer_size);
	lightmap.resize(buffer_size);

	int lightmap_index = 0;

	for (unsigned int i = 0; i < lightmap_indices.size(); i++) {
		lightmap_indices[i] = -1;
	}

	for (int i = 0; i < plot_mesh.surface_arrays.size(); i++) {

		if (plot_mesh.surface_primitive_types[i] != Mesh::PRIMITIVE_TRIANGLES)
			continue; //only triangles

		Array a = plot_mesh.surface_arrays[i];

		PoolVector<Vector3> vertices = a[Mesh::ARRAY_VERTEX];
		PoolVector<Vector3>::Read vr = vertices.read();
		PoolVector<Vector2> uv = a[Mesh::ARRAY_TEX_UV];
		PoolVector<Vector2>::Read uvr;
		PoolVector<Vector2> uv2 = a[Mesh::ARRAY_TEX_UV2];
		PoolVector<Vector2>::Read uv2r;
		PoolVector<Vector3> normals = a[Mesh::ARRAY_NORMAL];
		PoolVector<Vector3>::Read nr;
		PoolVector<int> index = a[Mesh::ARRAY_INDEX];

		bool read_uv = false;
		bool read_normals = false;

		if (uv.size()) {

			uvr = uv.read();
			read_uv = true;
		}

		uv2r = uv2.read();

		if (normals.size()) {
			read_normals = true;
			nr = normals.read();
		}

		if (index.size()) {

			int facecount = index.size() / 3;
			PoolVector<int>::Read ir = index.read();

			for (int j = 0; j < facecount; j++) {

				Vector3 vtxs[3];
				Vector2 uvs[3];
				Vector2 uv2s[3];
				Vector3 normal[3];

				for (int k = 0; k < 3; k++) {
					vtxs[k] = plot_mesh.local_xform.xform(vr[ir[j * 3 + k]]);
				}

				if (read_uv) {
					for (int k = 0; k < 3; k++) {
						uvs[k] = uvr[ir[j * 3 + k]];
					}
				}

				for (int k = 0; k < 3; k++) {
					uv2s[k] = uv2r[ir[j * 3 + k]];
					if (uv2s[k].x < 0.0f || uv2s[k].x > 1.0f || uv2s[k].y < 0.0f || uv2s[k].y > 1.0f) {
						plot_mesh.invalid_uv2s = true;
						return;
					}
				}

				if (read_normals) {
					for (int k = 0; k < 3; k++) {
						normal[k] = plot_mesh.local_xform.basis.xform(nr[ir[j * 3 + k]]).normalized();
					}
				}

				_plot_triangle(uv2s, vtxs, normal, uvs, plot_mesh.albedo_textures[i], plot_mesh.albedo_texture_sizes[i], plot_mesh.emission_textures[i], plot_mesh.emission_texture_sizes[i], size.x, size.y, lightmap.ptr(), lightmap_index, lightmap_indices.ptr());
			}

		} else {
			// TODO non-indexed
		}
	}
	lightmap.resize(lightmap_index);
}

bool RaytraceLightBaker::_cast_shadow_ray(RaytraceEngine::Ray &r_ray) {
	return RaytraceEngine::get_singleton()->intersect(r_ray);
}

void RaytraceLightBaker::_compute_direct_light(const RaytraceLightBaker::PlotLight &p_plot_light, RaytraceLightBaker::LightMapElement *r_lightmap, int p_size) {
	Light *light = p_plot_light.light;

	OmniLight *omni = Object::cast_to<OmniLight>(light);
	SpotLight *spot = Object::cast_to<SpotLight>(light);
	DirectionalLight *directional = Object::cast_to<DirectionalLight>(light);
	Vector3 light_position = p_plot_light.global_xform.origin;

	float light_range = light->get_param(Light::Param::PARAM_RANGE);
	float light_attenuation = light->get_param(Light::Param::PARAM_ATTENUATION);

	Color c = light->get_color();
	Vector3 light_energy = Vector3(c.r, c.g, c.b) * light->get_param(Light::Param::PARAM_ENERGY);

	for (int i = 0; i < p_size; i++) {

		Vector3 normal = r_lightmap[i].normal;
		Vector3 position = r_lightmap[i].pos;
		Vector3 final_energy;

		if (omni) {
			Vector3 light_direction = (position - light_position).normalized();
			if (normal.dot(light_direction) >= 0.0) continue;
			float dist = position.distance_to(light_position);

			if (dist <= light_range) {
				RaytraceEngine::Ray ray = RaytraceEngine::Ray(position, -light_direction, bias, dist - bias);
				if (_cast_shadow_ray(ray)) continue;
				float att = powf(1.0 - dist / light_range, light_attenuation);
				final_energy = light_energy * att * MAX(0, normal.dot(-light_direction));
			}
		}

		if (spot) {

			Vector3 light_direction = (position - light_position).normalized();
			if (normal.dot(light_direction) >= 0.0) continue;

			Vector3 spot_direction = -p_plot_light.global_xform.basis.get_axis(Vector3::AXIS_Z).normalized();
			float angle = Math::acos(spot_direction.dot(light_direction));
			float spot_angle = spot->get_param(Light::Param::PARAM_SPOT_ANGLE);

			if (Math::rad2deg(angle) > spot_angle) continue;

			float dist = position.distance_to(light_position);
			if (dist > light_range) continue;

			RaytraceEngine::Ray ray = RaytraceEngine::Ray(position, -light_direction, bias, dist);
			if (_cast_shadow_ray(ray)) continue;

			float normalized_dist = dist * (1.0f / MAX(0.001f, spot->get_param(Light::Param::PARAM_RANGE)));
			float norm_light_attenuation = Math::pow(MAX(1.0f - normalized_dist, 0.001f), spot->get_param(Light::Param::PARAM_ATTENUATION));

			float spot_cutoff = Math::cos(Math::deg2rad(spot_angle));
			float scos = MAX(light_direction.dot(spot_direction), spot_cutoff);
			float spot_rim = (1.0f - scos) / (1.0f - spot_cutoff);
			norm_light_attenuation *= 1.0f - pow(MAX(spot_rim, 0.001f), spot->get_param(Light::Param::PARAM_SPOT_ATTENUATION));
			final_energy = light_energy * norm_light_attenuation * MAX(0, normal.dot(-light_direction));
		}

		if (directional) {
			Vector3 light_direction = -p_plot_light.global_xform.basis.get_axis(Vector3::AXIS_Z).normalized();
			if (normal.dot(light_direction) >= 0.0) continue;
			RaytraceEngine::Ray ray = RaytraceEngine::Ray(position, -light_direction, bias);
			if (_cast_shadow_ray(ray)) continue;

			final_energy = light_energy * MAX(0, normal.dot(-light_direction));
		}

		r_lightmap[i].direct_light += final_energy * light->get_param(Light::PARAM_INDIRECT_ENERGY);
		if (light->get_bake_mode() == Light::BAKE_ALL) {
			r_lightmap[i].output += final_energy;
		}
	}
}

static Color _bilinear_sample(Vector<Color> texture, Vector2i size, Vector2 uv, bool wrap_y_axis = true) {

	uv.x = Math::fposmod(uv.x, 1.0f);
	uv.y = wrap_y_axis ? Math::fposmod(uv.y, 1.0f) : uv.y;

	float xf = uv.x * size.x;
	float yf = uv.y * size.y;

	int xi = (int)xf;
	int yi = (int)yf;

	Color texels[4];
	for (int i = 0; i < 4; i++) {
		int sample_x = (xi + i % 2) % size.x;

		int sample_y;
		if (wrap_y_axis) {
			sample_y = (yi + i / 2) % size.y;
		} else {
			sample_y = CLAMP(yi + i / 2, 0, size.y - 1);
		}
		texels[i] = texture[sample_y * size.x + sample_x];
	}

	float tx = xf - xi;
	float ty = yf - yi;

	Color c;
	for (int i = 0; i < 4; i++) {
		c[i] = Math::lerp(Math::lerp(texels[0][i], texels[1][i], tx), Math::lerp(texels[2][i], texels[3][i], tx), ty);
	}
	return c;
}

_ALWAYS_INLINE_ float uniform_rand() {
	/* Algorithm "xor" from p. 4 of Marsaglia, "Xorshift RNGs" */
	static thread_local uint32_t state = rand();
	state ^= state << 13;
	state ^= state >> 17;
	state ^= state << 5;
	return float(state) / UINT32_MAX;
}

void RaytraceLightBaker::_compute_ray_trace(uint32_t p_idx, LightMapElement *r_texels) {

	LightMapElement &texel = r_texels[p_idx];

	const static int samples_per_quality[4] = { 64, 256, 512, 1024 };

	int samples = samples_per_quality[bake_quality];

	Vector3 accum;

	const Vector3 const_forward = Vector3(0, 0, 1);
	const Vector3 const_up = Vector3(0, 1, 0);

	for (int i = 0; i < samples; i++) {

		Vector3 color;
		Vector3 throughput = Vector3(1.0f, 1.0f, 1.0f);

		Vector3 position = texel.pos;
		Vector3 normal = texel.normal;
		Vector3 direction;

		for (int depth = 0; depth < bounces; depth++) {

			Vector3 tangent = const_forward.cross(normal);
			if (unlikely(tangent.length_squared() < 0.005f)) {
				tangent = const_up.cross(normal);
			}
			tangent.normalize();
			Vector3 bitangent = tangent.cross(normal);
			bitangent.normalize();

			Basis normal_xform = Basis(tangent, bitangent, normal);
			normal_xform.transpose();

			float u1 = uniform_rand();
			float u2 = uniform_rand();

			float radius = Math::sqrt(u1);
			float theta = Math_TAU * u2;

			Vector3 axis = Vector3(radius * Math::cos(theta), radius * Math::sin(theta), Math::sqrt(MAX(0.0f, 1.0f - u1)));

			direction = normal_xform.xform(axis);

			// We can skip multiplying throughput by cos(theta) because de sampling PDF is also cos(theta) and they cancel each other
			//float pdf = normal.dot(direction);
			//throughput *= normal.dot(direction)/pdf;

			RaytraceEngine::Ray ray(position + normal * bias * 2.0f, direction, bias);
			bool hit = RaytraceEngine::get_singleton()->intersect(ray);

			if (!hit) {
				Color c = sky_color;
				if (!sky_data.empty()) {

					direction = sky_orientation.xform_inv(direction);
					Vector2 st = Vector2(Math::atan2(direction.z, direction.x), Math::acos(direction.y));

					if (Math::is_nan(st.y)) {
						st.y = direction.y > 0.0 ? 0.0 : Math_PI;
					}

					st.x += Math_PI;
					st /= Vector2(Math_TAU, Math_PI);
					st.x = Math::fmod(st.x + 0.75, 1.0);

					c = _bilinear_sample(sky_data, sky_size, st, false);
				}
				color += throughput * Vector3(c.r, c.g, c.b) * c.a;
				break;
			}

			unsigned int hit_mesh_id = ray.geomID;
			const Size2i &size = scene_lightmap_sizes[hit_mesh_id];

			int x = ray.u * size.x;
			int y = ray.v * size.y;

			const int &idx = scene_lightmap_indices[hit_mesh_id][y * size.x + x];

			if (idx < 0) {
				break;
			}

			const LightMapElement &sample = scene_lightmaps[hit_mesh_id][idx];

			if (sample.normal.dot(ray.dir) > 0.0 && !no_shadow_meshes.has(hit_mesh_id)) {
				// We hit a back-face
				break;
			}

			color += throughput * sample.emission;
			throughput *= sample.albedo;
			color += throughput * sample.direct_light;

			// Russian Roulette
			// https://computergraphics.stackexchange.com/questions/2316/is-russian-roulette-really-the-answer
			const float p = throughput[throughput.max_axis()];
			if (uniform_rand() > p) {
				break;
			}
			throughput *= 1.0f / p;

			position = sample.pos;
			normal = sample.normal;
		}
		accum += color;
	}

	texel.output += accum / samples;
}

Error RaytraceLightBaker::_compute_indirect_light(unsigned int mesh_id) {

	int total_size = scene_lightmaps[mesh_id].size();
	LightMapElement *lightmap_ptr = scene_lightmaps[mesh_id].ptr();

#if 0
		// Disable threading, for debugging
		for (int i = 0; i < total_size; i++) {
			_compute_ray_trace(i,lightmap_ptr);
	 	}
		return OK;
#endif

	int slice_size = 8192;
	int n_slices = total_size / slice_size;
	int leftover_slice = total_size % slice_size;

	uint64_t begin_time = OS::get_singleton()->get_ticks_usec();

	for (int i = 0; i < n_slices; i++) {
		thread_process_array(slice_size, this, &RaytraceLightBaker::_compute_ray_trace, &lightmap_ptr[i * slice_size]);

		uint64_t elapsed = OS::get_singleton()->get_ticks_usec() - begin_time;
		float elapsed_sec = double(elapsed) / 1000000.0;
		float remaining = i < 1 ? -1.0 : (elapsed_sec / i) * (n_slices - i);
		if (_bake_time(remaining, i / float(n_slices + 1))) {
			return ERR_SKIP;
		}
	}

	thread_process_array(leftover_slice, this, &RaytraceLightBaker::_compute_ray_trace, &lightmap_ptr[n_slices * slice_size]);

	if (_bake_time(0.0, 1.0)) {
		return ERR_SKIP;
	}

	return OK;
}

Error RaytraceLightBaker::_generate_buffers(int *r_progress_step) {

	int total_size = scene_lightmaps.size();

	int slice_size = OS::get_singleton()->get_processor_count();
	int n_slices = total_size / slice_size;
	int leftover_slice = total_size % slice_size;

	for (int i = 0; i < n_slices; i++) {
		*r_progress_step += slice_size;

		if (BakedLightmap::bake_step_function) {
			bool cancel = BakedLightmap::bake_step_function(*r_progress_step, RTR("Build buffers ") + " (" + itos(i * slice_size + 1) + "/" + itos(total_size) + ")");
			if (cancel) {
				return ERR_SKIP;
			}
		}

		for (int j = 0; j < slice_size; ++j) {
			_get_material_images(mesh_list[i * slice_size + j]);
		}

		thread_process_array(slice_size, this, &RaytraceLightBaker::_make_lightmap, i * slice_size);

		for (int j = 0; j < slice_size; ++j) {
			_clear_material_textures(mesh_list[i * slice_size + j]);
			if (mesh_list[i * slice_size + j].invalid_uv2s) {
				return ERR_INVALID_DATA;
			}
		}
	}

	if (leftover_slice > 0) {
		*r_progress_step += leftover_slice;
		if (BakedLightmap::bake_step_function) {
			bool cancel = BakedLightmap::bake_step_function(*r_progress_step, RTR("Build buffers ") + " (" + itos(total_size) + "/" + itos(total_size) + ")");
			if (cancel) {
				return ERR_SKIP;
			}
		}

		for (int j = 0; j < leftover_slice; ++j) {
			_get_material_images(mesh_list[n_slices * slice_size + j]);
		}

		thread_process_array(leftover_slice, this, &RaytraceLightBaker::_make_lightmap, n_slices * slice_size);

		for (int j = 0; j < leftover_slice; ++j) {
			_clear_material_textures(mesh_list[n_slices * slice_size + j]);
			if (mesh_list[n_slices * slice_size + j].invalid_uv2s) {
				return ERR_INVALID_DATA;
			}
		}
	}

	for (uint32_t i = 0; i < mesh_list.size(); i++) {
		const Size2i &size = scene_lightmap_sizes[i];
		bool has_alpha = false;
		PoolVector<uint8_t> alpha_data;
		alpha_data.resize(size.x * size.y);
		{
			PoolVector<uint8_t>::Write w = alpha_data.write();
			for (unsigned int j = 0; j < scene_lightmap_indices[i].size(); ++j) {
				int idx = scene_lightmap_indices[i][j];
				uint8_t alpha = 0;
				if (idx >= 0) {
					alpha = CLAMP(scene_lightmaps[i][idx].alpha * 255, 0, 255);
					if (alpha < 255) {
						has_alpha = true;
					}
				}
				w[j] = alpha;
			}
		}

		if (has_alpha) {
			Ref<Image> alpha_texture;
			alpha_texture.instance();
			alpha_texture->create(size.x, size.y, false, Image::FORMAT_L8, alpha_data);

			RaytraceEngine::get_singleton()->set_mesh_alpha_texture(alpha_texture, i);
		}
	}

	return OK;
}

Vector3 RaytraceLightBaker::_fix_sample_position(const Vector3 &p_position, const Vector3 &p_normal, const Vector3 &p_tangent, const Vector3 &p_bitangent, const Vector2 &p_texel_size) {

	RaytraceEngine *rt = RaytraceEngine::get_singleton();

	Basis tangent_basis(p_tangent, p_bitangent, p_normal);
	tangent_basis.orthonormalize();
	Vector2 half_size = p_texel_size / 2.0f;
	Vector3 corrected = p_position;

	for (int i = -1; i <= 1; i += 1) {
		for (int j = -1; j <= 1; j += 1) {
			if (i == 0 && j == 0) continue;
			Vector3 offset = Vector3(half_size.x * i, half_size.y * j, 0.0);
			Vector3 rotated_offset = tangent_basis.xform_inv(offset);
			Vector3 target = p_position + rotated_offset;
			Vector3 ray_vector = target - corrected;

			float ray_length = ray_vector.length();
			Vector3 ray_direction = ray_vector.normalized();
			RaytraceEngine::Ray ray(corrected + p_normal * bias - ray_direction * bias, ray_direction, 0.0f, ray_length + (bias * 2.0));
			bool hit = rt->intersect(ray);
			if (hit) {
				ray.normal.normalize();
				if (ray.normal.dot(ray_direction) > 0.0f) {
					corrected = corrected + ray.dir * ray.tfar + ray.normal * (bias * 2.0f);
				}
			}
		}
	}

	return corrected;
}

bool _segment_intersect(Vector2 x0, Vector2 x1, Vector2 y0, Vector2 y1) {
#define edgeFunction(a, b, c) (c[0] - a[0]) * (b[1] - a[1]) - (c[1] - a[1]) * (b[0] - a[0])
#define onSegment(a, b, c) (b[0] <= MAX(a[0], c[0]) && b[0] >= MIN(a[0], c[0]) && b[1] <= MAX(a[1], c[1]) && b[1] >= MIN(a[1], c[1]))

	float orient1 = edgeFunction(x0, x1, y0);
	float orient2 = edgeFunction(x0, x1, y1);
	float orient3 = edgeFunction(y0, y1, x0);
	float orient4 = edgeFunction(y0, y1, x1);

	int o1 = orient1 == 0.0f ? 0 : (orient1 > 0.0f ? 1 : -1);
	int o2 = orient2 == 0.0f ? 0 : (orient2 > 0.0f ? 1 : -1);
	int o3 = orient3 == 0.0f ? 0 : (orient3 > 0.0f ? 1 : -1);
	int o4 = orient4 == 0.0f ? 0 : (orient4 > 0.0f ? 1 : -1);

	// General case
	if (o1 != o2 && o3 != o4)
		return true;

	// Special Cases
	// x0, x1 and y0 are colinear and y0 lies on segment x0x1
	if (o1 == 0 && onSegment(x0, y0, x1)) return true;

	// x0, x1 and y1 are colinear and y1 lies on segment x0x1
	if (o2 == 0 && onSegment(x0, y1, x1)) return true;

	// y0, y1 and x0 are colinear and x0 lies on segment y0y1
	if (o3 == 0 && onSegment(y0, x0, y1)) return true;

	// y0, y1 and x1 are colinear and x1 lies on segment y0y1
	if (o4 == 0 && onSegment(y0, x1, y1)) return true;

	return false; // Doesn't fall in any of the above cases

#undef edgeFunction
#undef onSegment
}

void RaytraceLightBaker::_plot_triangle(Vector2 *p_vertices, Vector3 *p_positions, Vector3 *p_normals, Vector2 *p_uvs, const Vector<Color> &p_albedo_texture, const Size2i &p_albedo_size, const Vector<Color> &p_emission_texture, const Size2i &p_emission_size, int p_width, int p_height, LightMapElement *r_texels, int &r_lightmap_index, int *r_lightmap_indices) {
	Vector2 pv0 = p_vertices[0];
	Vector2 pv1 = p_vertices[1];
	Vector2 pv2 = p_vertices[2];

	Vector2 v0(pv0.x * p_width, pv0.y * p_height);
	Vector2 v1(pv1.x * p_width, pv1.y * p_height);
	Vector2 v2(pv2.x * p_width, pv2.y * p_height);

	Vector3 p0 = p_positions[0];
	Vector3 p1 = p_positions[1];
	Vector3 p2 = p_positions[2];

	Vector3 n0 = p_normals[0];
	Vector3 n1 = p_normals[1];
	Vector3 n2 = p_normals[2];

	Vector2 uv0 = p_uvs[0];
	Vector2 uv1 = p_uvs[1];
	Vector2 uv2 = p_uvs[2];

#define edgeFunction(a, b, c) (c[0] - a[0]) * (b[1] - a[1]) - (c[1] - a[1]) * (b[0] - a[0])

	float area = edgeFunction(v0, v1, v2);

	if (area < 0.0) {
		SWAP(pv1, pv2);
		SWAP(v1, v2);
		SWAP(p1, p2);
		SWAP(n1, n2);
		SWAP(uv1, uv2);

		area *= -1.0f;
	}

	Vector3 edge1 = p1 - p0;
	Vector3 edge2 = p2 - p0;

	Vector2 uv_edge1 = pv1 - pv0;
	Vector2 uv_edge2 = pv2 - pv0;

	float r = 1.0f / (uv_edge1.x * uv_edge2.y - uv_edge1.y * uv_edge2.x);

	Vector3 tangent = (edge1 * uv_edge2.y - edge2 * uv_edge1.y) * r;
	Vector3 bitangent = (edge2 * uv_edge1.x - edge1 * uv_edge2.x) * r;

	tangent.normalize();
	bitangent.normalize();

	// Compute triangle bounding box
	Vector2 bbox_min = Vector2(MIN(v0.x, MIN(v1.x, v2.x)), MIN(v0.y, MIN(v1.y, v2.y)));
	Vector2 bbox_max = Vector2(MAX(v0.x, MAX(v1.x, v2.x)), MAX(v0.y, MAX(v1.y, v2.y)));

	bbox_min = bbox_min.floor();
	bbox_max = bbox_max.ceil();

	uint32_t min_x = MAX(bbox_min.x - 2, 0);
	uint32_t min_y = MAX(bbox_min.y - 2, 0);
	uint32_t max_x = MIN(bbox_max.x, p_width - 1);
	uint32_t max_y = MIN(bbox_max.y, p_height - 1);

	Vector2 texel_size;

	for (int i = 0; i < 2; i++) {
		Vector2i p = v0;
		p[i] += 1;

		Vector2 vv0 = v2 - v0;
		Vector2 vv1 = v1 - v0;
		Vector2 vv2 = p + Vector2(0.5, 0.5) - v0;

		// Compute dot products
		float dot00 = vv0.dot(vv0);
		float dot01 = vv0.dot(vv1);
		float dot02 = vv0.dot(vv2);
		float dot11 = vv1.dot(vv1);
		float dot12 = vv1.dot(vv2);

		// Compute barycentric coordinates
		float invDenom = 1.0f / (dot00 * dot11 - dot01 * dot01);
		float u2 = (dot11 * dot02 - dot01 * dot12) * invDenom;
		float u1 = (dot00 * dot12 - dot01 * dot02) * invDenom;
		float u0 = 1.0f - u2 - u1;

		Vector3 pos = p0 * u0 + p1 * u1 + p2 * u2;

		texel_size[i] = p0.distance_to(pos);
	}

	for (uint32_t j = min_y; j <= max_y; ++j) {
		for (uint32_t i = min_x; i <= max_x; i++) {

			bool inside = false;
			const Vector2 pixel = Vector2(i, j);

			if (v0.floor() == pixel || v1.floor() == pixel || v2.floor() == pixel) {
				inside = true;
			}

			if (!inside) {
				Vector2 p = pixel + Vector2(0.5f, 0.5f);
				float w0 = edgeFunction(v1, v2, p);
				float w1 = edgeFunction(v2, v0, p);
				float w2 = edgeFunction(v0, v1, p);
				if (w0 >= 0 && w1 >= 0 && w2 >= 0) {
					inside = true;
				}
			}

			if (!inside) {
				const float neg_offset = -0.1f;
				const float pos_offset = 1.1f;
				const Vector2 corners[4] = { Vector2(neg_offset, neg_offset), Vector2(neg_offset, pos_offset), Vector2(pos_offset, pos_offset), Vector2(pos_offset, neg_offset) };
				for (int k = 0; k < 4; k++) {
					Vector2 p = pixel + corners[k];
					Vector2 q = pixel + corners[(k + 1) % 4];

					if (_segment_intersect(p, q, v0, v1) || _segment_intersect(p, q, v1, v2) || _segment_intersect(p, q, v2, v0)) {
						inside = true;
						break;
					}
				}
			}

			if (!inside) continue;

			Vector2 p = Vector2(i + 0.5f, j + 0.5f);
			float w0 = edgeFunction(v1, v2, p);
			float w1 = edgeFunction(v2, v0, p);
			float w2 = edgeFunction(v0, v1, p);

			if (Math::is_zero_approx(area)) {
				w0 = w1 = w2 = 1.0f / 3.0f;
			} else {
				w0 /= area;
				w1 /= area;
				w2 /= area;
			}
			int ofs = j * p_width + i;
			if (r_lightmap_indices[ofs] >= 0) {
				continue;
			}

			Vector3 pos = p0 * w0 + p1 * w1 + p2 * w2;
			Vector3 normal = n0 * w0 + n1 * w1 + n2 * w2;
			Vector2 uv = uv0 * w0 + uv1 * w1 + uv2 * w2;
			Color c = _bilinear_sample(p_albedo_texture, p_albedo_size, uv);
			Color e = _bilinear_sample(p_emission_texture, p_emission_size, uv);

			pos = _fix_sample_position(pos, normal, tangent, bitangent, texel_size);

			if (c.a > 0.0f) {
				r_lightmap_indices[ofs] = r_lightmap_index;

				LightMapElement &texel = r_texels[r_lightmap_index];
				texel.normal = normal;
				texel.pos = pos;
				texel.albedo = Vector3(c.r, c.g, c.b);
				texel.alpha = c.a;
				texel.emission = Vector3(e.r, e.g, e.b);
				texel.direct_light = Vector3();
				texel.output = Vector3();

				++r_lightmap_index;
			}
		}
	}
}

struct SeamEdge {
	Vector3 pos[2];
	Vector3 normal[2];
	Vector2 uv[2];

	_FORCE_INLINE_ bool operator<(const SeamEdge &p_edge) const {
		return pos[0].x < p_edge.pos[0].x;
	}
};

void RaytraceLightBaker::_fix_seams(const PlotMesh &p_plot_mesh, Vector3 *r_lightmap, const Size2i &p_size) {

	float max_uv_distance = 1.5f / MAX(p_size.x, p_size.y);

	for (int i = 0; i < p_plot_mesh.surface_arrays.size(); i++) {

		if (p_plot_mesh.surface_primitive_types[i] != Mesh::PRIMITIVE_TRIANGLES)
			continue; //only triangles

		Array arr = p_plot_mesh.surface_arrays[i];

		PoolVector<Vector3> vertices = arr[Mesh::ARRAY_VERTEX];
		PoolVector<Vector3>::Read vr = vertices.read();
		PoolVector<Vector2> uv2 = arr[Mesh::ARRAY_TEX_UV2];
		PoolVector<Vector2>::Read uv2r = uv2.read();
		PoolVector<Vector3> normals = arr[Mesh::ARRAY_NORMAL];
		PoolVector<Vector3>::Read nr;
		PoolVector<int> index = arr[Mesh::ARRAY_INDEX];

		bool read_normals = false;

		if (normals.size()) {
			read_normals = true;
			nr = normals.read();
		}

		if (index.size()) {

			int facecount = index.size() / 3;
			PoolVector<int>::Read ir = index.read();
			Vector<SeamEdge> edges;
			edges.resize(facecount * 3);
			SeamEdge *edges_ptr = edges.ptrw();
			for (int j = 0; j < facecount; j++) {

				for (int k = 0; k < 3; k++) {
					int idx[2];
					idx[0] = ir[j * 3 + k];
					idx[1] = ir[j * 3 + (k + 1) % 3];

					if (vr[idx[1]] < vr[idx[0]]) {
						SWAP(idx[0], idx[1]);
					}

					SeamEdge e;
					for (int l = 0; l < 2; ++l) {
						e.pos[l] = vr[idx[l]];
						e.uv[l] = uv2r[idx[l]];
						if (read_normals) {
							e.normal[l] = nr[idx[l]];
						}
					}
					edges_ptr[j * 3 + k] = e;
				}
			}

			edges.sort();

			for (int j = 0; j < edges.size(); j++) {
				const SeamEdge &edge0 = edges[j];
				for (int k = j + 1; k < edges.size() && edges[k].pos[0].x < edge0.pos[0].x + 0.06; ++k) {
					const SeamEdge &edge1 = edges[k];

					bool is_seam = true;
					for (int l = 0; l < 2; ++l) {
						if (edge0.pos[l].distance_to(edge1.pos[l]) > 0.0005) {
							is_seam = false;
							break;
						}

						if (edge0.normal[l].distance_to(edge1.normal[l]) > 0.005) {
							is_seam = false;
							break;
						}
					}

					if (edge0.uv[0].distance_to(edge1.uv[0]) < max_uv_distance && edge0.uv[1].distance_to(edge1.uv[1]) < max_uv_distance) {
						is_seam = false;
					}

					if (!is_seam) continue;

					_fix_seam(edge0.uv[0], edge0.uv[1], edge1.uv[0], edge1.uv[1], r_lightmap, p_size);
					_fix_seam(edge1.uv[0], edge1.uv[1], edge0.uv[0], edge0.uv[1], r_lightmap, p_size);
				}
			}

		} else {
			// TODO non-indexed
		}
	}
}

void RaytraceLightBaker::_fix_seam(const Vector2 &p_uv0, const Vector2 &p_uv1, const Vector2 &p_uv3, const Vector2 &p_uv4, Vector3 *r_lightmap, const Size2i &p_size) {

	Vector2 p0 = p_uv0 * p_size;
	Vector2 p1 = p_uv1 * p_size;

	const Vector2i start_pixel = p0.floor();
	const Vector2i end_pixel = p1.floor();

	Vector2 seam_dir = (p1 - p0).normalized();
	Vector2 t_delta = Vector2(1.0f / Math::abs(seam_dir.x), 1.0f / Math::abs(seam_dir.y));
	Vector2i step = Vector2(seam_dir.x > 0 ? 1 : (seam_dir.x < 0 ? -1 : 0), seam_dir.y > 0 ? 1 : (seam_dir.y < 0 ? -1 : 0));

	Vector2 t_next = Vector2(Math::fmod(p0.x, 1.0f), Math::fmod(p0.y, 1.0f));

	if (step.x == 1) {
		t_next.x = 1.0f - t_next.x;
	}

	if (step.y == 1) {
		t_next.y = 1.0f - t_next.y;
	}

	t_next.x /= Math::abs(seam_dir.x);
	t_next.y /= Math::abs(seam_dir.y);

	if (Math::is_nan(t_next.x)) {
		t_next.x = 1e20f;
	}

	if (Math::is_nan(t_next.y)) {
		t_next.y = 1e20f;
	}

	Vector2i pixel = start_pixel;
	Vector2 start_p = start_pixel;
	float max_dist = p0.distance_to(p1) + 1.0f;

	while (pixel != end_pixel && Vector2(pixel).distance_to(start_p) < max_dist) {

		Vector2 p_uv = Vector2(pixel) + Vector2(0.5f, 0.5f);
		float t = (p0 + Vector2(0.5f, 0.5f)).distance_to(p_uv) / (p0 + Vector2(0.5f, 0.5f)).distance_to(p1 + Vector2(0.5f, 0.5f));
		t = CLAMP(t, 0.0, 1.0);

		Vector2 q_uv = p_uv3 + (p_uv4 - p_uv3) * t;
		Vector2i q = (q_uv * p_size).floor();

		Vector3 p_color = r_lightmap[pixel.y * p_size.x + pixel.x];
		Vector3 q_color = r_lightmap[q.y * p_size.x + q.x];

		r_lightmap[pixel.y * p_size.x + pixel.x] = (p_color + q_color) / 2.0;

		if (t_next.x < t_next.y) {
			pixel.x += step.x;
			t_next.x += t_delta.x;
		} else {
			pixel.y += step.y;
			t_next.y += t_delta.y;
		}
	}
}

BakedLightmap::BakeError RaytraceLightBaker::bake(Node *p_base_node, Node *p_from_node, bool p_generate_atlas, int p_max_atlas_size, String p_save_path, Ref<BakedLightmapData> r_lightmap_data) {

#if DEBUG_ENABLED
	uint64_t start_msec, current_msec;
#define time_split(msg)                                                                 \
	current_msec = OS::get_singleton()->get_system_time_msecs();                        \
	print_line(vformat("%-15s -> \t%.2f", msg, (current_msec - start_msec) / 1000.0f)); \
	start_msec = current_msec

	start_msec = OS::get_singleton()->get_system_time_msecs();
#else
#define time_split(msg)
#endif

	_find_meshes_and_lights(p_from_node);

	time_split("Find meshes");

	RaytraceEngine *rt = RaytraceEngine::get_singleton();
	rt->init_scene();

	for (uint32_t mesh_id = 0; mesh_id < mesh_list.size(); mesh_id++) {
		rt->add_mesh(mesh_list[mesh_id].mesh, mesh_list[mesh_id].local_xform, mesh_id);
	}
	rt->commit_scene();

	time_split("Build BVH");

	scene_lightmaps.resize(mesh_list.size());
	scene_lightmap_sizes.resize(mesh_list.size());
	scene_lightmap_indices.resize(mesh_list.size());

	Set<int> no_lightmap_meshes;

	for (uint32_t mesh_id = 0; mesh_id < mesh_list.size(); mesh_id++) {
		if (!mesh_list[mesh_id].cast_shadows) {
			no_shadow_meshes.insert(mesh_id);
		}
		if (!mesh_list[mesh_id].save_lightmap) {
			no_lightmap_meshes.insert(mesh_id);
		}
	}

	int n_lit_meshes = mesh_list.size() - no_lightmap_meshes.size();
	if (BakedLightmap::bake_begin_function) {
		int step_count =
				(p_generate_atlas && n_lit_meshes ? 1 : 0) + // Optimize atlas
				mesh_list.size() + // Generate buffers
				n_lit_meshes + // Direct
				n_lit_meshes + // Indirect
				n_lit_meshes + // Denoise & fix seams
				n_lit_meshes + // Plot lightmap
				(p_generate_atlas && n_lit_meshes ? 1 : n_lit_meshes) + // Save images
				(capture_enabled ? 1 : 0);
		BakedLightmap::bake_begin_function(step_count);
	}

	int step = 0;

	time_split("Build sets");

	Size2i atlas_size;

	for (uint32_t mesh_id = 0; mesh_id < mesh_list.size(); mesh_id++) {

		Size2i size = mesh_list[mesh_id].size_hint;

		if (size == Size2()) {
			size = _compute_lightmap_size(mesh_list[mesh_id]);
			ERR_FAIL_COND_V(size == Size2i(), BakedLightmap::BAKE_ERROR_LIGHTMAP_SIZE);
		}

		scene_lightmap_sizes[mesh_id] = size;

		if (mesh_list[mesh_id].save_lightmap) {
			atlas_size.width = MAX(atlas_size.width, size.width);
			atlas_size.height = MAX(atlas_size.height, size.height);
		}
	}

	// Determine best atlas layout by bruteforce fitting

	struct AtlasOffset {
		int slice;
		int x;
		int y;
	};
	Vector<AtlasOffset> atlas_offsets;
	int atlas_slices = 0;
	if (p_generate_atlas && n_lit_meshes) {
		int max = nearest_power_of_2_templated(atlas_size.width);
		max = MAX(max, nearest_power_of_2_templated(atlas_size.height));

		if (max > p_max_atlas_size) {
			return BakedLightmap::BAKE_ERROR_LIGHTMAP_SIZE;
		}

		Size2i best_atlas_size;
		int best_atlas_slices = 0;
		int best_atlas_memory = 0x7FFFFFFF;
		float best_atlas_mem_utilization = 0;
		Vector<AtlasOffset> best_atlas_offsets;
		Vector<Size2i> best_scaled_sizes;

		int first_try_mem_occupied = 0;
		int first_try_mem_used = 0;
		for (int recovery_percent = 0; recovery_percent <= 100; recovery_percent += 10) {
			// These only make sense from the second round of the loop
			float recovery_scale = 1;
			int target_mem_occupied = 0;
			if (recovery_percent != 0) {
				target_mem_occupied = first_try_mem_occupied + (first_try_mem_used - first_try_mem_occupied) * recovery_percent * 0.01f;
				recovery_scale = recovery_percent == 0 ? 1.0f : Math::sqrt(static_cast<float>(target_mem_occupied) / first_try_mem_occupied);
			}

			atlas_size = Size2i(max, max);
			while (atlas_size.x <= p_max_atlas_size && atlas_size.y <= p_max_atlas_size) {

				if (recovery_percent != 0) {
					// Find out how much memory is not recoverable (because of lightmaps that can't grow),
					// to compute a greater recovery scale for those that can.
					int mem_unrecoverable = 0;

					for (unsigned int i = 0; i < scene_lightmap_sizes.size(); i++) {
						if (mesh_list[i].save_lightmap) {
							Vector2i scaled_size = Vector2i(
									static_cast<int>(recovery_scale * scene_lightmap_sizes[i].x),
									static_cast<int>(recovery_scale * scene_lightmap_sizes[i].y));
							if (scaled_size.x > atlas_size.x || scaled_size.y > atlas_size.y) {
								mem_unrecoverable += scaled_size.x * scaled_size.y - scene_lightmap_sizes[i].x * scene_lightmap_sizes[i].y;
							}
						}
					}
					recovery_scale = Math::sqrt(static_cast<float>(target_mem_occupied - mem_unrecoverable) / (first_try_mem_occupied - mem_unrecoverable));
				}

				Vector<Size2i> scaled_sizes;
				scaled_sizes.resize(scene_lightmap_sizes.size());
				{
					for (unsigned int i = 0; i < scene_lightmap_sizes.size(); i++) {
						if (mesh_list[i].save_lightmap) {
							if (recovery_percent == 0) {
								scaled_sizes.write[i] = scene_lightmap_sizes[i];
							} else {
								Vector2i scaled_size = Vector2i(
										static_cast<int>(recovery_scale * scene_lightmap_sizes[i].x),
										static_cast<int>(recovery_scale * scene_lightmap_sizes[i].y));
								if (scaled_size.x <= atlas_size.x && scaled_size.y <= atlas_size.y) {
									scaled_sizes.write[i] = scaled_size;
								} else {
									scaled_sizes.write[i] = scene_lightmap_sizes[i];
								}
							}
						} else {
							// Don't consider meshes with no generated lightmap here; will compensate later
							scaled_sizes.write[i] = Vector2i();
						}
					}
				}

				Vector<Size2i> source_sizes = scaled_sizes;
				Vector<int> source_indices;
				source_indices.resize(source_sizes.size());
				for (int i = 0; i < source_indices.size(); i++) {
					source_indices.write[i] = i;
				}

				Vector<AtlasOffset> curr_atlas_offsets;
				curr_atlas_offsets.resize(source_sizes.size());

				int slices = 0;

				while (source_sizes.size() > 0) {

					Vector<Geometry::PackRectsResult> offsets = Geometry::partial_pack_rects(source_sizes, atlas_size);
					Vector<int> new_indices;
					Vector<Vector2i> new_sources;
					for (int i = 0; i < offsets.size(); i++) {
						Geometry::PackRectsResult ofs = offsets[i];
						int sidx = source_indices[i];
						if (ofs.packed) {
							curr_atlas_offsets.write[sidx] = { slices, ofs.x, ofs.y };
						} else {
							new_indices.push_back(sidx);
							new_sources.push_back(source_sizes[i]);
						}
					}

					source_sizes = new_sources;
					source_indices = new_indices;
					slices++;
				}

				int mem_used = atlas_size.x * atlas_size.y * slices;
				int mem_occupied = 0;
				for (int i = 0; i < curr_atlas_offsets.size(); i++) {
					mem_occupied += scaled_sizes[i].x * scaled_sizes[i].y;
				}

				float mem_utilization = static_cast<float>(mem_occupied) / mem_used;
				if (mem_used < best_atlas_memory || (mem_used == best_atlas_memory && mem_utilization > best_atlas_mem_utilization)) {
					best_atlas_size = atlas_size;
					best_atlas_offsets = curr_atlas_offsets;
					best_atlas_slices = slices;
					best_atlas_memory = mem_used;
					best_atlas_mem_utilization = mem_utilization;
					best_scaled_sizes = scaled_sizes;
					if (recovery_percent == 0) {
						first_try_mem_occupied = mem_occupied;
						first_try_mem_used = mem_used;
					}
				}

				if (atlas_size.width == atlas_size.height) {
					atlas_size.width *= 2;
				} else {
					atlas_size.height *= 2;
				}
			}
		}
		atlas_size = best_atlas_size;
		atlas_slices = best_atlas_slices;
		atlas_offsets = best_atlas_offsets;

		// Set new lightmap sizes with possible texture space recovery
		for (unsigned int i = 0; i < scene_lightmap_sizes.size(); i++) {
			if (best_scaled_sizes[i] != Size2i()) {
				scene_lightmap_sizes[i] = best_scaled_sizes[i];
			}
		}

		print_line(vformat("Texture space utilization: %.2f%% (improved from: %.2f%%)", best_atlas_mem_utilization * 100.0f, 100.0f * first_try_mem_occupied / first_try_mem_used));

		time_split("Optimize atlas");
	}

	Error err = _generate_buffers(&step);

	if (err == ERR_SKIP) {
		return BakedLightmap::BAKE_ERROR_USER_ABORTED;
	} else if (err == ERR_INVALID_DATA) {
		return BakedLightmap::BAKE_ERROR_INVALID_MESH;
	}

	time_split("Build buffers");

	rt->set_mesh_filter(no_shadow_meshes);

	int mesh_step = 0;
	for (uint32_t mesh_id = 0; mesh_id < mesh_list.size(); mesh_id++) {

		if (!mesh_list[mesh_id].save_lightmap) {
			continue;
		}

		if (BakedLightmap::bake_step_function) {
			bool cancel = BakedLightmap::bake_step_function(step++, RTR("Direct light ") + " (" + itos(mesh_step + 1) + "/" + itos(n_lit_meshes) + ")");
			if (cancel) {
				return BakedLightmap::BAKE_ERROR_USER_ABORTED;
			}
		}

		for (uint32_t i = 0; i < light_list.size(); i++) {
			_compute_direct_light(light_list[i], scene_lightmaps[mesh_id].ptr(), scene_lightmaps[mesh_id].size());
		}
		mesh_step++;
	}

	rt->clear_mesh_filter();

	time_split("Direct light");

	mesh_step = 0;
	for (uint32_t mesh_id = 0; mesh_id < mesh_list.size(); mesh_id++) {

		if (!mesh_list[mesh_id].save_lightmap) {
			continue;
		}

		if (BakedLightmap::bake_step_function) {
			bool cancel = BakedLightmap::bake_step_function(step++, RTR("Indirect light ") + " (" + itos(mesh_step + 1) + "/" + itos(n_lit_meshes) + ")");
			if (cancel) {
				return BakedLightmap::BAKE_ERROR_USER_ABORTED;
			}
		}

		_bake_time(-1.0f, 0.0f);

		if (!scene_lightmaps[mesh_id].empty()) {
			_compute_indirect_light(mesh_id);
		}

		mesh_step++;
	}

	if (BakedLightmap::bake_end_substep_function) {
		BakedLightmap::bake_end_substep_function();
	}

	time_split("Indirect light");

	Vector<Vector<Vector3> > lightmaps_data;
	lightmaps_data.resize(mesh_list.size());

	if (use_denoiser) {
		LightmapDenoiser::get_singleton()->init();
	}

	mesh_step = 0;
	for (uint32_t mesh_id = 0; mesh_id < mesh_list.size(); mesh_id++) {

		if (!mesh_list[mesh_id].save_lightmap) {
			continue;
		}

		if (BakedLightmap::bake_step_function) {
			bool cancel = BakedLightmap::bake_step_function(step++, RTR("Denoise & fix seams ") + " (" + itos(mesh_step + 1) + "/" + itos(n_lit_meshes) + ")");
			if (cancel) {
				return BakedLightmap::BAKE_ERROR_USER_ABORTED;
			}
		}

		const Size2i &size = scene_lightmap_sizes[mesh_id];
		LightMapElement *lightmap_ptr = scene_lightmaps[mesh_id].ptr();
		int *lightmap_indices_ptr = scene_lightmap_indices[mesh_id].ptr();

		Vector<Vector3> *lightmap_data = &lightmaps_data.ptrw()[mesh_id];
		lightmap_data->resize(size.x * size.y);

		Vector3 *lightmap_data_ptr = lightmap_data->ptrw();
		for (int i = 0; i < size.y; i++) {
			for (int j = 0; j < size.x; j++) {
				int idx = lightmap_indices_ptr[i * size.x + j];
				if (idx >= 0) {
					lightmap_data_ptr[i * size.x + j] = lightmap_ptr[idx].output;
					continue; //filled, skip
				}

				//this can't be made separatable..

				int closest_idx = -1;
				float closest_dist = 1e20;

				const int margin = 2;
				for (int y = i - margin; y <= i + margin; y++) {
					for (int x = j - margin; x <= j + margin; x++) {

						if (x == j && y == i)
							continue;
						if (x < 0 || x >= size.x)
							continue;
						if (y < 0 || y >= size.y)
							continue;
						int cell_idx = lightmap_indices_ptr[y * size.x + x];
						if (cell_idx < 0) {
							continue; //also ensures that blitted stuff is not reused
						}

						float dist = Vector2(i - y, j - x).length_squared();
						if (dist < closest_dist) {
							closest_dist = dist;
							closest_idx = cell_idx;
						}
					}
				}

				if (closest_idx != -1) {
					lightmap_data_ptr[i * size.x + j] = lightmap_ptr[closest_idx].output;
				}
			}
		}

		scene_lightmaps[mesh_id].clear(); // Free now to save memory

		if (use_denoiser) {
			LightmapDenoiser::get_singleton()->denoise_lightmap((float *)lightmap_data->ptrw(), size);
		}

		_fix_seams(mesh_list[mesh_id], lightmap_data->ptrw(), size);

		mesh_step++;
	}

	if (use_denoiser) {
		LightmapDenoiser::get_singleton()->free();
	}

	time_split("Noise & seams");

	// 1. Create all the images (either per mesh or per atlas slice)

	Vector<PlotMesh *> lit_meshes;
	for (uint32_t mesh_id = 0; mesh_id < mesh_list.size(); mesh_id++) {
		if (mesh_list[mesh_id].save_lightmap) {
			lit_meshes.push_back(&mesh_list[mesh_id]);
		}
	}

	Vector<Ref<Image> > images;
	uint32_t tex_flags = Texture::FLAGS_DEFAULT;

	if (p_generate_atlas) {
		images.resize(atlas_slices);

		for (int i = 0; i < atlas_slices; i++) {
			Ref<Image> image;
			image.instance();
			image->create(atlas_size.x, atlas_size.y, false, Image::FORMAT_RGBF);
			images.set(i, image);
		}
	} else {
		images.resize(n_lit_meshes);

		mesh_step = 0;
		for (uint32_t mesh_id = 0; mesh_id < mesh_list.size(); mesh_id++) {
			if (!mesh_list[mesh_id].save_lightmap) {
				continue;
			}

			String mesh_name = mesh_list[mesh_id].node->get_name();
			if (mesh_name == "" || mesh_name.find(":") != -1 || mesh_name.find("/") != -1) {
				mesh_name = "LightMap";
			}

			if (used_mesh_names.has(mesh_name)) {
				int idx = 2;
				String base = mesh_name;
				while (true) {
					mesh_name = base + itos(idx);
					if (!used_mesh_names.has(mesh_name))
						break;
					idx++;
				}
			}
			used_mesh_names.insert(mesh_name);

			Ref<Image> image;
			image.instance();
			image->create(scene_lightmap_sizes[mesh_id].x, scene_lightmap_sizes[mesh_id].y, false, Image::FORMAT_RGBF);
			image->set_name(mesh_name);
			images.set(mesh_step, image);

			mesh_step++;
		}
	}

	// 2. Plot each lightmap onto its corresponding image

	auto _blit_lightmap = [](const Vector<Vector3> &p_src, const Size2i &p_size, Image *p_dst, int p_x, int p_y) -> void {
		ERR_FAIL_COND(p_x < 0 || p_y < 0);
		ERR_FAIL_COND(p_x + p_size.x > p_dst->get_width());
		ERR_FAIL_COND(p_y + p_size.y > p_dst->get_height());

		p_dst->lock();
		for (int y = 0; y < p_size.y; y++) {
			const Vector3 *__restrict src = p_src.ptr() + y * p_size.x;
			for (int x = 0; x < p_size.x; x++) {
				p_dst->set_pixel(p_x + x, p_y + y, Color(src->x, src->y, src->z));
				src++;
			}
		}
		p_dst->unlock();
	};

	mesh_step = 0;
	for (uint32_t mesh_id = 0; mesh_id < mesh_list.size(); mesh_id++) {
		if (!mesh_list[mesh_id].save_lightmap) {
			continue;
		}

		if (BakedLightmap::bake_step_function) {
			bool cancel = BakedLightmap::bake_step_function(step++, RTR("Plot light map ") + " (" + itos(mesh_step + 1) + "/" + itos(n_lit_meshes) + ")");
			if (cancel) {
				return BakedLightmap::BAKE_ERROR_USER_ABORTED;
			}
		}

		if (p_generate_atlas) {
			_blit_lightmap(lightmaps_data[mesh_id], scene_lightmap_sizes[mesh_id], images.get(atlas_offsets[mesh_id].slice).ptr(), atlas_offsets[mesh_id].x, atlas_offsets[mesh_id].y);
		} else {
			_blit_lightmap(lightmaps_data[mesh_id], scene_lightmap_sizes[mesh_id], images.get(mesh_step).ptr(), 0, 0);
		}

		mesh_step++;
	}

	time_split("Plotting");

	// 3. Save images to disk

	if (p_generate_atlas) {
		if (n_lit_meshes) {
			if (BakedLightmap::bake_step_function) {
				bool cancel = BakedLightmap::bake_step_function(step++, RTR("Save light map atlas"));
				if (cancel) {
					return BakedLightmap::BAKE_ERROR_USER_ABORTED;
				}
			}

			Ref<Image> large_image;
			large_image.instance();
			large_image->create(atlas_size.x, atlas_size.y * images.size(), false, images[0]->get_format());
			for (int i = 0; i < images.size(); i++) {
				large_image->blit_rect(images[i], Rect2(0, 0, atlas_size.x, atlas_size.y), Point2(0, atlas_size.y * i));
			}

			Ref<TextureLayered> texture;
			String image_path = p_save_path.plus_file(p_base_node->get_name());

			if (ResourceLoader::import) {

				image_path += ".exr";
				String relative_path = image_path;
				if (relative_path.begins_with("res://")) {
					relative_path = relative_path.substr(6, relative_path.length());
				}
				large_image->save_exr(relative_path, false);

				Ref<ConfigFile> config;
				config.instance();
				if (FileAccess::exists(image_path + ".import")) {
					config->load(image_path + ".import");
				} else {
					// Set only if settings don't exist, to keep user choice
					config->set_value("params", "compress/mode", 0);
				}
				config->set_value("remap", "importer", "texture_array");
				config->set_value("remap", "type", "TextureArray");
				config->set_value("params", "detect_3d", false);
				config->set_value("params", "flags/repeat", false);
				config->set_value("params", "flags/filter", true);
				config->set_value("params", "flags/mipmaps", false);
				config->set_value("params", "flags/srgb", false);
				config->set_value("params", "slices/horizontal", 1);
				config->set_value("params", "slices/vertical", images.size());
				config->save(image_path + ".import");

				ResourceLoader::import(image_path);
				texture = ResourceLoader::load(image_path); //if already loaded, it will be updated on refocus?
			} else {

				image_path += ".texarr";
				Ref<TextureLayered> tex;
				bool set_path = true;
				if (ResourceCache::has(image_path)) {
					tex = Ref<Resource>((Resource *)ResourceCache::get(image_path));
					set_path = false;
				}

				if (!tex.is_valid()) {
					tex.instance();
				}

				tex->create(atlas_size.x, atlas_size.y, images.size(), images[0]->get_format(), tex_flags);
				for (int i = 0; i < images.size(); i++) {
					tex->set_layer_data(images[i], i);
				}

				ResourceSaver::save(image_path, tex, ResourceSaver::FLAG_CHANGE_PATH);
				if (set_path) {
					tex->set_path(image_path);
				}
				texture = tex;
			}

			mesh_step = 0;
			for (uint32_t mesh_id = 0; mesh_id < mesh_list.size(); mesh_id++) {
				if (mesh_list[mesh_id].save_lightmap) {
					Rect2 uv_rect = Rect2(
							static_cast<real_t>(atlas_offsets[mesh_id].x) / atlas_size.x,
							static_cast<real_t>(atlas_offsets[mesh_id].y) / atlas_size.y,
							static_cast<real_t>(scene_lightmap_sizes[mesh_id].x) / atlas_size.x,
							static_cast<real_t>(scene_lightmap_sizes[mesh_id].y) / atlas_size.y);
					r_lightmap_data->add_user(p_base_node->get_path_to(mesh_list[mesh_id].node), texture, atlas_offsets[mesh_id].slice, uv_rect, mesh_list[mesh_id].instance_idx);
					mesh_step++;
				}
			}
		}
	} else {
		for (int i = 0; i < images.size(); i++) {

			if (BakedLightmap::bake_step_function) {
				bool cancel = BakedLightmap::bake_step_function(step++, RTR("Save light map ") + " (" + itos(i + 1) + "/" + itos(images.size()) + ")");
				if (cancel) {
					if (BakedLightmap::bake_end_function) {
						BakedLightmap::bake_end_function();
					}
					return BakedLightmap::BAKE_ERROR_USER_ABORTED;
				}
			}

			Ref<Texture> texture;
			String image_path = p_save_path.plus_file(images[i]->get_name());

			if (ResourceLoader::import) {

				image_path += ".exr";
				String relative_path = image_path;
				if (relative_path.begins_with("res://")) {
					relative_path = relative_path.substr(6, relative_path.length());
				}
				images[i]->save_exr(relative_path, false);

				Ref<ConfigFile> config;
				config.instance();
				if (FileAccess::exists(image_path + ".import")) {
					config->load(image_path + ".import");
				} else {
					// Set only if settings don't exist, to keep user choice
					config->set_value("params", "compress/mode", 0);
				}
				config->set_value("remap", "importer", "texture");
				config->set_value("remap", "type", "StreamTexture");
				config->set_value("params", "detect_3d", false);
				config->set_value("params", "flags/repeat", false);
				config->set_value("params", "flags/filter", true);
				config->set_value("params", "flags/mipmaps", false);
				config->set_value("params", "flags/srgb", false);

				config->save(image_path + ".import");

				ResourceLoader::import(image_path);
				texture = ResourceLoader::load(image_path); //if already loaded, it will be updated on refocus?
			} else {

				image_path += ".tex";
				Ref<ImageTexture> tex;
				bool set_path = true;
				if (ResourceCache::has(image_path)) {
					tex = Ref<Resource>((Resource *)ResourceCache::get(image_path));
					set_path = false;
				}

				if (!tex.is_valid()) {
					tex.instance();
				}

				tex->create_from_image(images[i], tex_flags);

				ResourceSaver::save(image_path, tex, ResourceSaver::FLAG_CHANGE_PATH);
				if (set_path) {
					tex->set_path(image_path);
				}
				texture = tex;
			}

			r_lightmap_data->add_user(p_base_node->get_path_to(lit_meshes[i]->node), texture, -1, Rect2(0, 0, 1, 1), lit_meshes[i]->instance_idx);
		}
	}

	time_split("Saving");

	if (BakedLightmap::bake_step_function) {
		BakedLightmap::bake_step_function(step++, RTR("Generating capture... "));
	}

	if (capture_enabled) {
		VoxelLightBaker voxel_baker;

		voxel_baker.begin_bake(capture_subdiv + 1, bake_bounds);

		for (uint32_t mesh_id = 0; mesh_id < mesh_list.size(); mesh_id++) {
			PlotMesh &pm = mesh_list[mesh_id];
			voxel_baker.plot_mesh(pm.local_xform, pm.mesh, pm.surface_materials, Ref<Material>());
		}

		voxel_baker.begin_bake_light(VoxelLightBaker::BakeQuality(capture_quality), capture_propagation);

		for (uint32_t i = 0; i < light_list.size(); i++) {
			PlotLight &pl = light_list[i];
			switch (pl.light->get_light_type()) {
				case VS::LIGHT_DIRECTIONAL: {
					voxel_baker.plot_light_directional(-pl.global_xform.basis.get_axis(2), pl.light->get_color(), pl.light->get_param(Light::PARAM_ENERGY), pl.light->get_param(Light::PARAM_INDIRECT_ENERGY), pl.light->get_bake_mode() == Light::BAKE_ALL);
				} break;
				case VS::LIGHT_OMNI: {
					voxel_baker.plot_light_omni(pl.global_xform.origin, pl.light->get_color(), pl.light->get_param(Light::PARAM_ENERGY), pl.light->get_param(Light::PARAM_INDIRECT_ENERGY), pl.light->get_param(Light::PARAM_RANGE), pl.light->get_param(Light::PARAM_ATTENUATION), pl.light->get_bake_mode() == Light::BAKE_ALL);
				} break;
				case VS::LIGHT_SPOT: {
					voxel_baker.plot_light_spot(pl.global_xform.origin, pl.global_xform.basis.get_axis(2), pl.light->get_color(), pl.light->get_param(Light::PARAM_ENERGY), pl.light->get_param(Light::PARAM_INDIRECT_ENERGY), pl.light->get_param(Light::PARAM_RANGE), pl.light->get_param(Light::PARAM_ATTENUATION), pl.light->get_param(Light::PARAM_SPOT_ANGLE), pl.light->get_param(Light::PARAM_SPOT_ATTENUATION), pl.light->get_bake_mode() == Light::BAKE_ALL);

				} break;
			}
		}

		voxel_baker.end_bake();

		r_lightmap_data->set_cell_subdiv(capture_subdiv);
		r_lightmap_data->set_bounds(local_bounds);
		r_lightmap_data->set_octree(voxel_baker.create_capture_octree(capture_subdiv));

		{
			float bake_bound_size = bake_bounds.get_longest_axis_size();
			Transform to_bounds;
			to_bounds.basis.scale(Vector3(bake_bound_size, bake_bound_size, bake_bound_size));
			to_bounds.origin = local_bounds.position;

			Transform to_grid;
			to_grid.basis.scale(Vector3(1 << (capture_subdiv - 1), 1 << (capture_subdiv - 1), 1 << (capture_subdiv - 1)));

			Transform to_cell_space = to_grid * to_bounds.affine_inverse();
			r_lightmap_data->set_cell_space_transform(to_cell_space);
		}
	}

	if (BakedLightmap::bake_end_function) {
		BakedLightmap::bake_end_function();
	}

	time_split("Capture data");

	return BakedLightmap::BAKE_ERROR_OK;
}

RaytraceLightBaker::RaytraceLightBaker() {
	default_texels_per_unit = 0.0f;
	bake_quality = BakedLightmap::BAKE_QUALITY_MEDIUM;
	capture_quality = BakedLightmap::BAKE_QUALITY_MEDIUM;
	capture_propagation = 0.85;
	capture_subdiv = 0;
}
#endif

void BakedLightmap::set_capture_cell_size(float p_cell_size) {
	capture_cell_size = MAX(0.1, p_cell_size);
}

float BakedLightmap::get_capture_cell_size() const {
	return capture_cell_size;
}

void BakedLightmap::set_extents(const Vector3 &p_extents) {
	extents = p_extents;
	update_gizmo();
	_change_notify("bake_extents");
}

Vector3 BakedLightmap::get_extents() const {
	return extents;
}

void BakedLightmap::set_default_texels_per_unit(const float &p_bake_texels_per_unit) {
	default_texels_per_unit = MAX(0.0, p_bake_texels_per_unit);
}

float BakedLightmap::get_default_texels_per_unit() const {
	return default_texels_per_unit;
}

void BakedLightmap::set_capture_enabled(bool p_enable) {
	capture_enabled = p_enable;
	_change_notify();
}

bool BakedLightmap::get_capture_enabled() const {
	return capture_enabled;
}

BakedLightmap::BakeError BakedLightmap::bake(Node *p_from_node, bool p_create_visual_debug) {
#ifdef TOOLS_ENABLED
	String save_path;

	if (image_path.begins_with("res://")) {
		save_path = image_path;
	} else {
		if (get_filename() != "") {
			save_path = get_filename().get_base_dir();
		} else if (get_owner() && get_owner()->get_filename() != "") {
			save_path = get_owner()->get_filename().get_base_dir();
		}

		if (save_path == "") {
			return BAKE_ERROR_NO_SAVE_PATH;
		}
		if (image_path != "") {
			save_path.plus_file(image_path);
		}
	}
	{
		//check for valid save path
		DirAccessRef d = DirAccess::open(save_path);
		if (!d) {
			ERR_PRINTS("Invalid Save Path '" + save_path + "'.");
			return BAKE_ERROR_NO_SAVE_PATH;
		}
	}

	Ref<BakedLightmapData> new_light_data;
	new_light_data.instance();

	Node *from_node = p_from_node ? p_from_node : get_owner();

	Vector<Color> environment_data;
	Basis environment_transform;
	Size2i environment_size;

	if (environment_mode != ENVIRONMENT_MODE_DISABLED) {

		environment_transform = get_global_transform().basis;

		switch (environment_mode) {
			case ENVIRONMENT_MODE_DISABLED: {
				//nothing
			} break;
			case ENVIRONMENT_MODE_SCENE: {
				Ref<World> world = get_world();
				if (world.is_valid()) {
					Ref<Environment> env = world->get_environment();
					if (env.is_null()) {
						env = world->get_fallback_environment();
					}

					if (env.is_valid()) {
						environment_data = _get_irradiance_map(env, environment_size);
						environment_transform = env->get_sky_orientation();
					}
				}
			} break;
			case ENVIRONMENT_MODE_CUSTOM_SKY: {
				if (environment_custom_sky.is_valid()) {
					Ref<Environment> env;
					env.instance();
					env->set_sky(environment_custom_sky);
					env->set_background(Environment::BG_SKY);
					environment_data = _get_irradiance_map(env, environment_size);
				}

			} break;
			case ENVIRONMENT_MODE_CUSTOM_COLOR: {
				//nothing
			} break;
		}
	}

	RaytraceLightBaker baker;
	baker.bias = bias;
	baker.default_texels_per_unit = default_texels_per_unit;
	baker.bake_quality = bake_quality;
	baker.capture_quality = capture_quality;
	baker.capture_propagation = capture_propagation;
	baker.bounces = bounces;
	baker.use_denoiser = use_denoiser;
	baker.capture_enabled = capture_enabled;
	if (environment_data.empty()) {
		baker.sky_color = environment_mode == ENVIRONMENT_MODE_CUSTOM_COLOR ? environment_custom_color * environment_custom_energy : Color(0, 0, 0);
	} else {
		baker.sky_data = environment_data;
		baker.sky_size = environment_size;
		baker.sky_orientation = environment_transform;
	}

	baker.local_bounds = AABB(-extents, extents * 2);
	baker.global_bounds = get_global_transform().xform(baker.local_bounds);
	baker.bake_bounds = AABB(-extents, extents * 2);

	float bake_cell_size = capture_cell_size / 2.0;
	int subdiv = nearest_power_of_2_templated(int(baker.bake_bounds.get_longest_axis_size() / bake_cell_size));
	baker.bake_bounds.size[baker.bake_bounds.get_longest_axis_index()] = subdiv * bake_cell_size;
	int bake_subdiv = nearest_shift(subdiv) + 1;
	baker.capture_subdiv = bake_subdiv - 1;

	BakeError err = baker.bake(this, from_node, generate_atlas, max_atlas_size, save_path, new_light_data);

	if (err == BAKE_ERROR_OK) {
		set_light_data(new_light_data);
	} else {
		if (bake_end_substep_function) {
			bake_end_substep_function();
		}

		if (bake_end_function) {
			bake_end_function();
		}
	}

	return err;
#else
	return BAKE_ERROR_OK;
#endif
}

void BakedLightmap::_notification(int p_what) {
	if (p_what == NOTIFICATION_READY) {

		if (light_data.is_valid()) {
			_assign_lightmaps();
		}
		request_ready(); //will need ready again if re-enters tree
	}

	if (p_what == NOTIFICATION_EXIT_TREE) {

		if (light_data.is_valid()) {
			_clear_lightmaps();
		}
	}
}

void BakedLightmap::_assign_lightmaps() {

	ERR_FAIL_COND(!light_data.is_valid());

	for (int i = 0; i < light_data->get_user_count(); i++) {
		Ref<Resource> lightmap = light_data->get_user_lightmap(i);
		ERR_CONTINUE(!lightmap.is_valid());
		ERR_CONTINUE(!Object::cast_to<Texture>(lightmap.ptr()) && !Object::cast_to<TextureLayered>(lightmap.ptr()));

		Node *node = get_node(light_data->get_user_path(i));
		int instance_idx = light_data->get_user_instance(i);
		if (instance_idx >= 0) {
			RID instance = node->call("get_bake_mesh_instance", instance_idx);
			if (instance.is_valid()) {
				VS::get_singleton()->instance_set_use_lightmap(instance, get_instance(), lightmap->get_rid(), light_data->get_user_lightmap_slice(i), light_data->get_user_lightmap_uv_rect(i));
			}
		} else {
			VisualInstance *vi = Object::cast_to<VisualInstance>(node);
			ERR_CONTINUE(!vi);
			VS::get_singleton()->instance_set_use_lightmap(vi->get_instance(), get_instance(), lightmap->get_rid(), light_data->get_user_lightmap_slice(i), light_data->get_user_lightmap_uv_rect(i));
		}
	}
}

void BakedLightmap::_clear_lightmaps() {
	ERR_FAIL_COND(!light_data.is_valid());
	for (int i = 0; i < light_data->get_user_count(); i++) {
		Node *node = get_node(light_data->get_user_path(i));
		int instance_idx = light_data->get_user_instance(i);
		if (instance_idx >= 0) {
			RID instance = node->call("get_bake_mesh_instance", instance_idx);
			if (instance.is_valid()) {
				VS::get_singleton()->instance_set_use_lightmap(instance, get_instance(), RID(), -1, Rect2(0, 0, 1, 1));
			}
		} else {
			VisualInstance *vi = Object::cast_to<VisualInstance>(node);
			ERR_CONTINUE(!vi);
			VS::get_singleton()->instance_set_use_lightmap(vi->get_instance(), get_instance(), RID(), -1, Rect2(0, 0, 1, 1));
		}
	}
}

Vector<Color> BakedLightmap::_get_irradiance_map(Ref<Environment> p_env, Size2i &r_size) {
	Vector<Color> ret;

	switch (p_env->get_background()) {
		case Environment::BG_SKY: {
			Ref<Sky> sky = p_env->get_sky();

			if (sky.is_null()) {
				return ret;
			}

			Ref<Image> sky_image;
			Ref<PanoramaSky> panorama = sky;
			if (panorama.is_valid()) {
				sky_image = panorama->get_panorama()->get_data();
			}

			Ref<ProceduralSky> procedural = sky;
			if (procedural.is_valid()) {
				sky_image = procedural->get_panorama();
			}

			if (sky_image.is_null()) {
				return ret;
			}

			sky_image->convert(Image::FORMAT_RGBAF);
			sky_image->generate_mipmaps();

			Vector2 sky_size = sky_image->get_size();

			int mip_level = 0;

			while (sky_size.x > 32) {
				sky_size /= 2.0;
				mip_level++;
			}

			ret.resize(sky_size.x * sky_size.y);

			int mip_ofs, mip_size;
			sky_image->get_mipmap_offset_and_size(mip_level, mip_ofs, mip_size);

			PoolByteArray sky_raw_data = sky_image->get_data();
			{
				PoolByteArray::Read r = sky_raw_data.read();
				const unsigned char *b = r.ptr();
				const float *r_ptr = (float *)(b + mip_ofs);
				Color *w_ptr = ret.ptrw();
				for (int i = 0; i < sky_size.x * sky_size.y; i++) {
					int idx = i * 4;
					Color c = Color(r_ptr[idx + 0] * p_env->get_bg_energy(), r_ptr[idx + 1] * p_env->get_bg_energy(), r_ptr[idx + 2] * p_env->get_bg_energy(), r_ptr[idx + 3] * p_env->get_bg_energy());
					w_ptr[i] = c;
				}
			}
			r_size = sky_size;
			break;
		}
		case Environment::BG_COLOR: {
			ret.resize(8 * 8);
			Color c = p_env->get_bg_color();
			c.r *= p_env->get_bg_energy();
			c.g *= p_env->get_bg_energy();
			c.b *= p_env->get_bg_energy();
			for (int i = 0; i < 8; i++) {
				for (int j = 0; j < 8; j++) {
					ret.write[j * 8 + i] = c;
				}
			}
			r_size = Vector2i(8, 8);
			break;
		}
		default: {
		}
	}
	return ret;
}

void BakedLightmap::set_light_data(const Ref<BakedLightmapData> &p_data) {

	if (light_data.is_valid()) {
		if (is_inside_tree()) {
			_clear_lightmaps();
		}
		set_base(RID());
	}

	light_data = p_data;
	_change_notify("light_data");

	if (light_data.is_valid()) {
		set_base(light_data->get_rid());
		if (is_inside_tree()) {
			_assign_lightmaps();
		}
	}
}

Ref<BakedLightmapData> BakedLightmap::get_light_data() const {

	return light_data;
}

void BakedLightmap::set_capture_propagation(float p_propagation) {
	capture_propagation = p_propagation;
}

float BakedLightmap::get_capture_propagation() const {

	return capture_propagation;
}

void BakedLightmap::set_capture_quality(BakeQuality p_quality) {
	capture_quality = p_quality;
}

BakedLightmap::BakeQuality BakedLightmap::get_capture_quality() const {
	return capture_quality;
}

void BakedLightmap::set_generate_atlas(bool p_enabled) {
	generate_atlas = p_enabled;
}

bool BakedLightmap::is_generate_atlas_enabled() const {
	return generate_atlas;
}

void BakedLightmap::set_max_atlas_size(int p_size) {
	ERR_FAIL_COND(p_size < 2048);
	max_atlas_size = p_size;
}

int BakedLightmap::get_max_atlas_size() const {
	return max_atlas_size;
}

void BakedLightmap::set_bake_quality(BakeQuality p_quality) {
	bake_quality = p_quality;
}

BakedLightmap::BakeQuality BakedLightmap::get_bake_quality() const {
	return bake_quality;
}

void BakedLightmap::set_image_path(const String &p_path) {
	image_path = p_path;
}

String BakedLightmap::get_image_path() const {
	return image_path;
}

void BakedLightmap::set_use_denoiser(bool p_enable) {

	use_denoiser = p_enable;
}

bool BakedLightmap::is_using_denoiser() const {

	return use_denoiser;
}

void BakedLightmap::set_environment_mode(EnvironmentMode p_mode) {
	environment_mode = p_mode;
	_change_notify();
}

BakedLightmap::EnvironmentMode BakedLightmap::get_environment_mode() const {
	return environment_mode;
}

void BakedLightmap::set_environment_custom_sky(const Ref<Sky> &p_sky) {
	environment_custom_sky = p_sky;
}

Ref<Sky> BakedLightmap::get_environment_custom_sky() const {
	return environment_custom_sky;
}

void BakedLightmap::set_environment_custom_color(const Color &p_color) {
	environment_custom_color = p_color;
}
Color BakedLightmap::get_environment_custom_color() const {
	return environment_custom_color;
}

void BakedLightmap::set_environment_custom_energy(float p_energy) {
	environment_custom_energy = p_energy;
}
float BakedLightmap::get_environment_custom_energy() const {
	return environment_custom_energy;
}

void BakedLightmap::set_bounces(int p_bounces) {
	ERR_FAIL_COND(p_bounces < 0 || p_bounces > 16);
	bounces = p_bounces;
}

int BakedLightmap::get_bounces() const {
	return bounces;
}

void BakedLightmap::set_bias(float p_bias) {
	ERR_FAIL_COND(p_bias < 0.00001);
	bias = p_bias;
}

float BakedLightmap::get_bias() const {
	return bias;
}

AABB BakedLightmap::get_aabb() const {
	return AABB(-extents, extents * 2);
}
PoolVector<Face3> BakedLightmap::get_faces(uint32_t p_usage_flags) const {
	return PoolVector<Face3>();
}

void BakedLightmap::_validate_property(PropertyInfo &property) const {
	if (property.name.begins_with("capture") && property.name != "capture_enabled" && !capture_enabled) {
		property.usage = 0;
	}

	if (property.name == "environment_custom_sky" && environment_mode != ENVIRONMENT_MODE_CUSTOM_SKY) {
		property.usage = 0;
	}

	if (property.name == "environment_custom_color" && environment_mode != ENVIRONMENT_MODE_CUSTOM_COLOR) {
		property.usage = 0;
	}

	if (property.name == "environment_custom_energy" && environment_mode != ENVIRONMENT_MODE_CUSTOM_COLOR && environment_mode != ENVIRONMENT_MODE_CUSTOM_SKY) {
		property.usage = 0;
	}
}

#ifdef TOOLS_ENABLED
String BakedLightmap::get_configuration_warning() const {

	if (generate_atlas && OS::get_singleton()->get_current_video_driver() == OS::VIDEO_DRIVER_GLES2) {
		return TTR("Lightmap atlassing is not supported by the GLES2 driver. If you need to support GLES2, please uncheck bake/generate_atlas.");
	} else {
		return "";
	}
}
#endif

void BakedLightmap::_bind_methods() {

	ClassDB::bind_method(D_METHOD("set_light_data", "data"), &BakedLightmap::set_light_data);
	ClassDB::bind_method(D_METHOD("get_light_data"), &BakedLightmap::get_light_data);

	ClassDB::bind_method(D_METHOD("set_bake_quality", "quality"), &BakedLightmap::set_bake_quality);
	ClassDB::bind_method(D_METHOD("get_bake_quality"), &BakedLightmap::get_bake_quality);

	ClassDB::bind_method(D_METHOD("set_bounces", "bounces"), &BakedLightmap::set_bounces);
	ClassDB::bind_method(D_METHOD("get_bounces"), &BakedLightmap::get_bounces);

	ClassDB::bind_method(D_METHOD("set_bias", "bias"), &BakedLightmap::set_bias);
	ClassDB::bind_method(D_METHOD("get_bias"), &BakedLightmap::get_bias);

	ClassDB::bind_method(D_METHOD("set_environment_mode", "mode"), &BakedLightmap::set_environment_mode);
	ClassDB::bind_method(D_METHOD("get_environment_mode"), &BakedLightmap::get_environment_mode);

	ClassDB::bind_method(D_METHOD("set_environment_custom_sky", "sky"), &BakedLightmap::set_environment_custom_sky);
	ClassDB::bind_method(D_METHOD("get_environment_custom_sky"), &BakedLightmap::get_environment_custom_sky);

	ClassDB::bind_method(D_METHOD("set_environment_custom_color", "color"), &BakedLightmap::set_environment_custom_color);
	ClassDB::bind_method(D_METHOD("get_environment_custom_color"), &BakedLightmap::get_environment_custom_color);

	ClassDB::bind_method(D_METHOD("set_environment_custom_energy", "energy"), &BakedLightmap::set_environment_custom_energy);
	ClassDB::bind_method(D_METHOD("get_environment_custom_energy"), &BakedLightmap::get_environment_custom_energy);

	ClassDB::bind_method(D_METHOD("set_use_denoiser", "use_denoiser"), &BakedLightmap::set_use_denoiser);
	ClassDB::bind_method(D_METHOD("is_using_denoiser"), &BakedLightmap::is_using_denoiser);

	ClassDB::bind_method(D_METHOD("set_generate_atlas", "enabled"), &BakedLightmap::set_generate_atlas);
	ClassDB::bind_method(D_METHOD("is_generate_atlas_enabled"), &BakedLightmap::is_generate_atlas_enabled);

	ClassDB::bind_method(D_METHOD("set_max_atlas_size", "max_atlas_size"), &BakedLightmap::set_max_atlas_size);
	ClassDB::bind_method(D_METHOD("get_max_atlas_size"), &BakedLightmap::get_max_atlas_size);

	ClassDB::bind_method(D_METHOD("set_capture_quality", "capture_quality"), &BakedLightmap::set_capture_quality);
	ClassDB::bind_method(D_METHOD("get_capture_quality"), &BakedLightmap::get_capture_quality);

	ClassDB::bind_method(D_METHOD("set_extents", "extents"), &BakedLightmap::set_extents);
	ClassDB::bind_method(D_METHOD("get_extents"), &BakedLightmap::get_extents);

	ClassDB::bind_method(D_METHOD("set_default_texels_per_unit", "texels"), &BakedLightmap::set_default_texels_per_unit);
	ClassDB::bind_method(D_METHOD("get_default_texels_per_unit"), &BakedLightmap::get_default_texels_per_unit);

	ClassDB::bind_method(D_METHOD("set_capture_propagation", "propagation"), &BakedLightmap::set_capture_propagation);
	ClassDB::bind_method(D_METHOD("get_capture_propagation"), &BakedLightmap::get_capture_propagation);

	ClassDB::bind_method(D_METHOD("set_capture_enabled", "enabled"), &BakedLightmap::set_capture_enabled);
	ClassDB::bind_method(D_METHOD("get_capture_enabled"), &BakedLightmap::get_capture_enabled);

	ClassDB::bind_method(D_METHOD("set_capture_cell_size", "capture_cell_size"), &BakedLightmap::set_capture_cell_size);
	ClassDB::bind_method(D_METHOD("get_capture_cell_size"), &BakedLightmap::get_capture_cell_size);

	ClassDB::bind_method(D_METHOD("set_image_path", "image_path"), &BakedLightmap::set_image_path);
	ClassDB::bind_method(D_METHOD("get_image_path"), &BakedLightmap::get_image_path);

	ClassDB::bind_method(D_METHOD("bake", "from_node", "create_visual_debug"), &BakedLightmap::bake, DEFVAL(Variant()), DEFVAL(false));

	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "extents"), "set_extents", "get_extents");

	ADD_GROUP("Tweaks", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "quality", PROPERTY_HINT_ENUM, "Low,Medium,High,Ultra"), "set_bake_quality", "get_bake_quality");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "bounces", PROPERTY_HINT_RANGE, "0,16,1"), "set_bounces", "get_bounces");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "use_denoiser"), "set_use_denoiser", "is_using_denoiser");
	ADD_PROPERTY(PropertyInfo(Variant::REAL, "bias", PROPERTY_HINT_RANGE, "0.00001,0.1,0.00001,or_greater"), "set_bias", "get_bias");
	ADD_PROPERTY(PropertyInfo(Variant::REAL, "default_texels_per_unit", PROPERTY_HINT_RANGE, "0.0,64.0,0.01,or_greater"), "set_default_texels_per_unit", "get_default_texels_per_unit");

	ADD_GROUP("Atlas", "atlas_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "atlas_generate"), "set_generate_atlas", "is_generate_atlas_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "atlas_max_size"), "set_max_atlas_size", "get_max_atlas_size");

	ADD_GROUP("Environment", "environment_");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "environment_mode", PROPERTY_HINT_ENUM, "Disabled,Scene,Custom Sky,Custom Color"), "set_environment_mode", "get_environment_mode");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "environment_custom_sky", PROPERTY_HINT_RESOURCE_TYPE, "Sky"), "set_environment_custom_sky", "get_environment_custom_sky");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "environment_custom_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_environment_custom_color", "get_environment_custom_color");
	ADD_PROPERTY(PropertyInfo(Variant::REAL, "environment_custom_energy", PROPERTY_HINT_RANGE, "0,64,0.01"), "set_environment_custom_energy", "get_environment_custom_energy");

	ADD_GROUP("Capture", "capture_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "capture_enabled"), "set_capture_enabled", "get_capture_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::REAL, "capture_cell_size", PROPERTY_HINT_RANGE, "0.5,2.0,0.05,or_greater"), "set_capture_cell_size", "get_capture_cell_size");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "capture_quality", PROPERTY_HINT_ENUM, "Low,Medium,High"), "set_capture_quality", "get_capture_quality");
	ADD_PROPERTY(PropertyInfo(Variant::REAL, "capture_propagation", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_capture_propagation", "get_capture_propagation");

	ADD_GROUP("Data", "");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "image_path", PROPERTY_HINT_DIR), "set_image_path", "get_image_path");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "light_data", PROPERTY_HINT_RESOURCE_TYPE, "BakedLightmapData"), "set_light_data", "get_light_data");

	BIND_ENUM_CONSTANT(BAKE_QUALITY_LOW);
	BIND_ENUM_CONSTANT(BAKE_QUALITY_MEDIUM);
	BIND_ENUM_CONSTANT(BAKE_QUALITY_HIGH);
	BIND_ENUM_CONSTANT(BAKE_QUALITY_ULTRA);

	BIND_ENUM_CONSTANT(BAKE_ERROR_OK);
	BIND_ENUM_CONSTANT(BAKE_ERROR_NO_SAVE_PATH);
	BIND_ENUM_CONSTANT(BAKE_ERROR_NO_MESHES);
	BIND_ENUM_CONSTANT(BAKE_ERROR_CANT_CREATE_IMAGE);
	BIND_ENUM_CONSTANT(BAKE_ERROR_USER_ABORTED);

	BIND_ENUM_CONSTANT(ENVIRONMENT_MODE_DISABLED);
	BIND_ENUM_CONSTANT(ENVIRONMENT_MODE_SCENE);
	BIND_ENUM_CONSTANT(ENVIRONMENT_MODE_CUSTOM_SKY);
	BIND_ENUM_CONSTANT(ENVIRONMENT_MODE_CUSTOM_COLOR);
}

BakedLightmap::BakedLightmap() {

	extents = Vector3(10, 10, 10);

	default_texels_per_unit = 16.0f;
	bake_quality = BAKE_QUALITY_MEDIUM;
	capture_quality = BAKE_QUALITY_MEDIUM;
	capture_propagation = 1;
	capture_enabled = false;
	bounces = 3;
	image_path = ".";
	set_disable_scale(true);
	capture_cell_size = 0.5;

	environment_mode = ENVIRONMENT_MODE_DISABLED;
	environment_custom_color = Color(0.2, 0.7, 1.0);
	environment_custom_energy = 1.0;

	use_denoiser = true;
	bias = 0.005;

	generate_atlas = true;
	max_atlas_size = 4096;
}
