// textured-boxes - JetExamples' "Textured crate" (esp32-textured-boxes, MIT,
// CubeCoders; the crate's texture: Cpt_Flash, CC0) for piegpu's OpenGL: a
// crate turning, its texture mapped four ways, three seconds each: without
// perspective (the old consoles' swimming texture) and with, the nearest
// texel and filtered.
#include <cmath>
#include <cstdio>
#include "kit.hpp"
#include "../assets/crate.hpp"
#include "../assets/boxes_labels.hpp"

namespace {

kit::Camera camera;
kit::Light light;
kit::Mesh box;
uint16_t gradient[kit::H];
GLuint nearest, filtered, title, caption[4];
float seconds_in = 0;
int mode = -1, pitch, yaw, roll;

void update (float seconds)
{
	static const char* const names[] = {"AFFINE / NEAREST", "PERSPECTIVE / NEAREST", "AFFINE / BILINEAR", "PERSPECTIVE / BILINEAR"};
	seconds_in = std::fmod (seconds_in + seconds, 3600.0f);
	const int next = (int) (seconds_in / 3.0f) % 4;
	if (next != mode)
		std::printf ("Texture mode: %s\n", names[next]);
	mode = next;
	pitch = (int) (22 + 18 * std::sin (seconds_in * 1.8f));
	yaw = (int) std::fmod (35 + seconds_in * 65, 360.0f);
	roll = (int) (8 * std::sin (seconds_in * 1.3f));
}

void init ()
{
	camera.near_plane = 32;
	camera.far_plane = 2000;
	light.on = true;
	light.azimuth = 235;
	light.elevation = 35;
	light.color = {255, 244, 230};
	light.ambient = {80, 90, 104};
	light.has_ambient = true;
	for (int y = 0; y < kit::H; y++)
	{
		const int r = 12 + y * 24 / kit::H, g = 28 + y * 56 / kit::H, b = 54 + y * 72 / kit::H;
		gradient[y] = (uint16_t) ((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
	}
	box.box (0, 0, 0, 200, 200, 200, kit::Material (0xFFFF, 255, 200, 0), true);
	nearest = kit::texture565 (crate::width, crate::height, crate::pixels, false);
	filtered = kit::texture565 (crate::width, crate::height, crate::pixels, true);
	title = kit::texture (boxes_labels::title);
	const kit::Image* const modes[4] = {&boxes_labels::mode0, &boxes_labels::mode1, &boxes_labels::mode2, &boxes_labels::mode3};
	for (int i = 0; i < 4; i++)
		caption[i] = kit::texture (*modes[i]);
	update (0);
}

void draw ()
{
	const kit::Image* const modes[4] = {&boxes_labels::mode0, &boxes_labels::mode1, &boxes_labels::mode2, &boxes_labels::mode3};
	kit::background (gradient);
	kit::begin (camera, light);
	kit::Draw how;
	how.model = kit::translation (0, 0, 520) * kit::rotation ((float) pitch, (float) yaw, (float) roll);
	how.texture = mode >= 2 ? filtered : nearest;
	how.affine = !(mode & 1);
	kit::draw (box, how);
	kit::sprite (title, boxes_labels::title.width, boxes_labels::title.height, 22, 17);
	kit::sprite (caption[mode], modes[mode]->width, modes[mode]->height, 22, 287);
}

}  // namespace

int main ()
{
	return kit::run ({"Textured crate", init, update, draw});
}
