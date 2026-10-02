// teapot.hpp - what the three teapot scenes share (JetExamples'
// esp32-lighting-teapot, esp32-depth-teapot, esp32-postfx-cel; MIT,
// CubeCoders): the Utah teapot (FreeGLUT's data, ../assets/teapot.hpp), its
// glaze, the light, the camera, the background.
#pragma once
#include "kit.hpp"
#include "../assets/teapot.hpp"

namespace teapot_scene {

inline kit::Camera camera;
inline kit::Light light;
inline kit::Material glaze (0xD8C3, 255, 210, 255);
inline kit::Mesh smooth;			// the vertices with their own normals, indexed
inline kit::Mesh faceted;			// a triangle's three vertices with its first one's normal (Jet's FLAT)
inline uint16_t gradient[kit::H];

/// \param facets also the faceted mesh
inline void setup (bool facets = false)
{
	camera.z = -1250;
	camera.near_plane = 32;
	camera.far_plane = 3000;
	light.on = true;
	light.azimuth = 235;
	light.elevation = 35;
	light.color = {255, 244, 224};
	light.intensity = 255;
	light.ambient = {30, 40, 56};
	light.has_ambient = true;
	for (int y = 0; y < kit::H; y++)
	{
		const int r = 12 + y * 24 / kit::H, g = 28 + y * 56 / kit::H, b = 54 + y * 72 / kit::H;
		gradient[y] = (uint16_t) ((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
	}
	glaze.gloss = 32;				// (Phong's highlight)
	// (as much memory as they take, no more: a Pico has little; and none once the RPi has them)
	const size_t corners = 3 * sizeof teapot::triangles / sizeof teapot::triangles[0];
	smooth.vertices.reserve (sizeof teapot::vertices / sizeof teapot::vertices[0]);
	smooth.indices.reserve (corners);
	if (facets)
		faceted.vertices.reserve (corners);
	for (const auto& v : teapot::vertices)
		smooth.add (v[0], v[1], v[2], v[3], v[4], v[5], glaze);
	for (const auto& t : teapot::triangles)
	{
		for (int k = 0; k < 3; k++)
		{
			smooth.indices.push_back (t[k]);
			const auto& v = teapot::vertices[t[k]];
			const auto& first = teapot::vertices[t[0]];
			if (facets)
				faceted.add (v[0], v[1], v[2], first[3], first[4], first[5], glaze);
		}
	}
}

}  // namespace teapot_scene
