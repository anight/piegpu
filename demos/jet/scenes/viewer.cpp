// viewer - picojet's model viewer (~/picojet/app/main.cpp, BSD-2-Clause) for
// piegpu's OpenGL: its nine textured models, one at a time, changing every
// six seconds, tumbling at 43 degrees a second about x and 61 about y under
// picojet's sun and ambient, the camera drifting round. The model is a vertex
// buffer and a texture on the RPi; the RPi's vertex shader turns and lights it.
#include <cmath>
#include <cstdio>
#include "kit.hpp"
#include "assets/mesh_cube.h"
#include "assets/mesh_f117.h"
#include "assets/mesh_f22.h"
#include "assets/mesh_efa.h"
#include "assets/mesh_sphere.h"
#include "assets/mesh_crab.h"
#include "assets/mesh_radio.h"
#include "assets/mesh_column.h"
#include "assets/mesh_biplane.h"
#include "assets/tex_pikuma.h"
#include "assets/tex_f117.h"
#include "assets/tex_f22.h"
#include "assets/tex_efa.h"
#include "assets/tex_crab.h"
#include "assets/tex_radio.h"
#include "assets/tex_column.h"
#include "assets/tex_biplane.h"

namespace {

struct Model
{
	const char* name;
	const int16_t (*verts)[8];		// x, y, z, u, v, nx, ny, nz (picojet's converter)
	int vert_count;
	const uint16_t (*tris)[3];
	int tri_count;
	const uint16_t* texels;			// 128 x 128 RGB565
};

#define MODEL(n, N, tex, name) {name, mesh_##n##_verts, MESH_##N##_VERTS, mesh_##n##_tris, MESH_##N##_TRIS, tex_##tex##_data}
// picojet's order: the cube with the pikuma texture, the sphere with the crab's
const Model MODELS[] =
{
	MODEL (cube, CUBE, pikuma, "CUBE"), MODEL (f117, F117, f117, "F-117"), MODEL (f22, F22, f22, "F-22"),
	MODEL (efa, EFA, efa, "EF-2000"), MODEL (sphere, SPHERE, crab, "SPHERE"), MODEL (crab, CRAB, crab, "CRAB"),
	MODEL (radio, RADIO, radio, "RADIO"), MODEL (column, COLUMN, column, "COLUMN"), MODEL (biplane, BIPLANE, biplane, "BIPLANE"),
};
#undef MODEL
constexpr int MODEL_COUNT = (int) (sizeof MODELS / sizeof MODELS[0]);
constexpr float SCALE = 4;			// picojet's world: four units to a model's one

kit::Camera camera;
kit::Light light;
kit::Mesh mesh;
GLuint texture;
int shown_model = -1;
float spin_x = 0, spin_y = 0, angle = 0, shown = 0;
char caption[48];

void load (int next)
{
	shown_model = next;
	const Model& m = MODELS[next];
	const kit::Material material (0xFFFF, 255, 255, 48);
	mesh.free ();
	for (int i = 0; i < m.vert_count; i++)
	{
		const int16_t* v = m.verts[i];
		mesh.add (v[0], v[1], v[2], v[5], v[6], v[7], material, v[3], v[4]);
	}
	for (int i = 0; i < m.tri_count; i++)
		for (int k = 0; k < 3; k++)
			mesh.indices.push_back (m.tris[i][k]);
	mesh.hand_over ();
	if (texture)
		glDeleteTextures (1, &texture);
	texture = kit::texture565 (128, 128, m.texels);
	std::snprintf (caption, sizeof caption, "%s  %d TRIANGLES", m.name, m.tri_count);
	std::printf ("viewer: %s, %d vertices, %d triangles\n", m.name, m.vert_count, m.tri_count);
}

void update (float seconds)
{
	seconds = seconds > 0.1f ? 0.1f : seconds;
	spin_x = std::fmod (spin_x + seconds * 43.0f, 360.0f);
	spin_y = std::fmod (spin_y + seconds * 61.0f, 360.0f);
	shown += seconds;
	if (shown >= 6.0f)
	{
		shown = 0;
		load ((shown_model + 1) % MODEL_COUNT);
	}
	angle += seconds * 0.35f;
	const float radius = 760.0f * SCALE;
	camera.x = (float) (int) (std::cos (angle) * radius);
	camera.y = 210 * SCALE;
	camera.z = (float) (int) (std::sin (angle) * radius);
	camera.look_at (0, 0, 0);
}

void init ()
{
	camera.fov = 70;
	camera.near_plane = 128;
	camera.far_plane = 4096 * SCALE;
	// (for textured models: the ambient carries the exposure, the sun shapes)
	light.on = light.has_ambient = true;
	light.azimuth = 45;
	light.elevation = 35;
	light.color = {255, 250, 240};
	light.intensity = 255;
	light.ambient = {205, 208, 215};
	load (0);
	update (0);
}

void draw ()
{
	kit::background ((uint16_t) 0x632C);		// picojet's 40% grey
	kit::begin (camera, light);
	kit::Draw how;
	how.model = kit::rotation ((float) (int) spin_x, (float) (int) spin_y, 0) * kit::scaling (SCALE, SCALE, SCALE);
	how.texture = texture;
	kit::draw (mesh, how);
}

const char* line ()
{
	return caption;
}

}  // namespace

int main ()
{
	return kit::run ({"Model viewer", init, update, draw, line});
}
