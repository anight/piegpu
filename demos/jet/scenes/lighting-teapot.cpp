// lighting-teapot - JetExamples' "Utah teapot lighting"
// (esp32-lighting-teapot, MIT, CubeCoders) for piegpu's OpenGL: one mesh, one
// motion, lit flat, then per vertex (Gouraud), then per pixel with a glossy
// highlight (Phong), three seconds each.
#include <cmath>
#include <cstdio>
#include "teapot.hpp"
#include "../assets/teapot_labels.hpp"

namespace {

using namespace teapot_scene;
float seconds_in = 0, pitch, yaw, roll;
int mode = -1;
GLuint title, caption[3];

void select (int next)
{
	static const char* const names[] = {"FLAT", "GOURAUD", "PHONG"};
	if (mode == next)
		return;
	mode = next;
	std::printf ("Shading mode: %s\n", names[mode]);
}

void update (float seconds)
{
	seconds_in = std::fmod (seconds_in + seconds, 3600.0f);
	select ((int) (seconds_in / 3.0f) % 3);
	pitch = -14.0f + 20.0f * std::sin (seconds_in * 1.8f);
	roll = 10.0f * std::sin (seconds_in * 1.4f);
	yaw = std::fmod (25.0f + seconds_in * 25.0f, 360.0f);
}

void init ()
{
	setup (true);
	std::printf ("Utah teapot: %u vertices, %u triangles, Flat / Gouraud / Phong cycle\n",
		     (unsigned) smooth.vertices.size (), (unsigned) smooth.indices.size () / 3);
	smooth.hand_over ();				// (the RPi has them: no copy kept here)
	faceted.hand_over ();
	title = kit::texture (teapot_labels::title);
	caption[0] = kit::texture (teapot_labels::mode0);
	caption[1] = kit::texture (teapot_labels::mode1);
	caption[2] = kit::texture (teapot_labels::mode2);
	update (0);
}

void draw ()
{
	kit::background (gradient);
	kit::begin (camera, light);
	kit::Draw how;
	how.model = kit::rotation ((float) (int) pitch, (float) (int) yaw, (float) (int) roll);
	how.shading = mode == 2 ? kit::PHONG : kit::VERTEX;
	kit::draw (mode == 0 ? faceted : smooth, how);
	kit::sprite (title, teapot_labels::title.width, teapot_labels::title.height, 22, 17);
	kit::sprite (caption[mode], teapot_labels::mode0.width, teapot_labels::mode0.height, 22, 287);
}

}  // namespace

int main ()
{
	return kit::run ({"Utah teapot lighting", init, update, draw});
}
