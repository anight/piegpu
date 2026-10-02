// depth-teapot - JetExamples' "Depth comparison" (esp32-depth-teapot, MIT,
// CubeCoders) for piegpu's OpenGL: the same teapot and motion drawn in
// painter's order (the triangles sorted by how far they are, the farthest
// first: where the handle or the spout passes the body the order is wrong)
// and with the depth buffer, seven seconds each.
//
// Painter's order here is Jet's: a triangle's distance is the mean of its
// three vertices', sorted into 64 bands between the near and the far plane;
// within a band the triangles keep the mesh's order.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "teapot.hpp"
#include "../assets/depth_labels.hpp"

namespace {

using namespace teapot_scene;
float seconds_in = 0, pitch, yaw, roll;
int mode = -1;
GLuint title, caption[2];

void update (float seconds)
{
	seconds_in = std::fmod (seconds_in + seconds, 3584.0f);
	const int next = (int) (seconds_in / 7.0f) % 2;
	if (next != mode)
		std::printf ("Depth mode: %s\n", next ? "DEPTH TESTING" : "PAINTER SORTING");
	mode = next;
	const float phase = std::fmod (seconds_in, 7.0f) / 7.0f;
	pitch = -14.0f + 20.0f * std::sin (phase * 6.283185307f);
	roll = 10.0f * std::sin (phase * 12.566370614f);
	yaw = (float) ((int) (25.0f + 360.0f * phase) % 360);
}

void init ()
{
	setup ();
	title = kit::texture (depth_labels::title);
	caption[0] = kit::texture (depth_labels::mode0);
	caption[1] = kit::texture (depth_labels::mode1);
	update (0);
	std::printf ("Utah teapot: %u vertices, %u triangles, fixed Phong, painter / depth cycle\n",
		     (unsigned) smooth.vertices.size (), (unsigned) smooth.indices.size () / 3);
}

void painters_order (const kit::Mat4& model)
{
	const kit::Mat4 mv = camera.view () * model;
	const size_t n = smooth.indices.size () / 3;
	static std::vector<int> depth;			// a vertex's distance
	static std::vector<std::pair<int, int>> order;	// a triangle's band, its place in the mesh
	depth.resize (smooth.vertices.size ());
	for (size_t i = 0; i < smooth.vertices.size (); i++)
	{
		const kit::Vertex& v = smooth.vertices[i];
		depth[i] = (int) (mv.m[2] * v.x + mv.m[6] * v.y + mv.m[10] * v.z + mv.m[14]);
	}
	order.resize (n);
	const int range = (int) (camera.far_plane - camera.near_plane);
	for (size_t t = 0; t < n; t++)
	{
		const uint16_t* corner = teapot::triangles[t];
		const int z = (depth[corner[0]] + depth[corner[1]] + depth[corner[2]]) / 3;
		const int band = std::max (0, std::min ((int) ((long long) (z - (int) camera.near_plane) * 64 / range), 63));
		order[t] = {-band, (int) t};		// the farthest band first
	}
	std::stable_sort (order.begin (), order.end (),
			  [] (const std::pair<int, int>& a, const std::pair<int, int>& b) { return a.first < b.first; });
	for (size_t t = 0; t < n; t++)
		for (int k = 0; k < 3; k++)
			smooth.indices[3 * t + k] = teapot::triangles[order[t].second][k];
	smooth.uploaded_indices = -1;			// (sent again)
}

void draw ()
{
	kit::background (gradient);
	kit::begin (camera, light);
	kit::Draw how;
	how.model = kit::rotation ((float) (int) pitch, (float) (int) yaw, (float) (int) roll);
	how.shading = kit::PHONG;
	if (mode == 0)				// its triangles in the painter's order, far to near; or as they come, with the depth buffer
	{
		painters_order (how.model);
		how.depth_test = false;
	}
	kit::draw (smooth, how);
	kit::sprite (title, depth_labels::title.width, depth_labels::title.height, 22, 17);
	kit::sprite (caption[mode], depth_labels::mode0.width, depth_labels::mode0.height, 22, 287);
}

}  // namespace

int main ()
{
	return kit::run ({"Depth comparison", init, update, draw});
}
