// postfx-crt - JetExamples' "CRT" (esp32-postfx-crt, MIT, CubeCoders) for
// piegpu's OpenGL: a textured cube before a poster, without and with a
// picture tube's lines (every other row darker: here a striped quad
// multiplied over the picture), seven seconds each.
#include <cmath>
#include <cstdio>
#include "kit.hpp"
#include "../assets/crt.hpp"
#include "../assets/crt_labels.hpp"

namespace {

kit::Camera camera;
kit::Light light;				// none: every colour as it is
kit::Mesh wall, cube;
uint16_t gradient[kit::H];
GLuint backdrop, tile[3], title, state[2], note, detail;
float seconds_in = 0;
int mode = -1, pitch, yaw, roll, poster_first, rails_first;

kit::Material paint (unsigned hex)
{
	return kit::Material (kit::to565 (hex), 255, 255, 0, false);
}

void update (float seconds)
{
	seconds_in = std::fmod (seconds_in + seconds, 14.0f);
	const int next = (int) (seconds_in / 7.0f);
	const float phase = std::fmod (seconds_in, 7.0f);
	if (next != mode)
		std::printf ("CRT: %s; intensity 112/255; no extra image buffer\n", next ? "ON" : "OFF");
	mode = next;
	// one turn in seven seconds: the same path in both modes
	pitch = (int) (20 + 14 * std::sin (phase * 6.2831853f / 7));
	yaw = (int) (phase * 360.0f / 7);
	roll = (int) (9 * std::sin (phase * 12.566371f / 7));
}

void init ()
{
	camera.fov = 58;
	camera.near_plane = 40;
	camera.far_plane = 2500;
	for (int y = 0; y < kit::H; y++)
		gradient[y] = kit::to565 (((18 + y * 12 / 320) << 16) | ((23 + y * 20 / 320) << 8) | (49 + y * 29 / 320));
	wall.plane (1090, 555, 0, 0, 1350, paint (0x6B719A));
	wall.plane (1060, 530, 0, 0, 1335, paint (0x1B213F));
	poster_first = (int) wall.vertices.size ();
	wall.plane (1024, 512, 0, 0, 1320, paint (0xFFFFFF));
	rails_first = (int) wall.vertices.size ();
	wall.plane (14, 490, -543, 0, 1310, paint (0x33F1DE));
	wall.plane (14, 490, 543, 0, 1310, paint (0xFF659D));
	cube.box (0, 0, 0, 240, 240, 240, paint (0xFFFFFF), true);
	wall.hand_over ();
	cube.hand_over ();
	backdrop = kit::texture565 (256, 128, crt::backdrop);
	tile[0] = kit::texture565 (32, 32, crt::cyan);
	tile[1] = kit::texture565 (32, 32, crt::pink);
	tile[2] = kit::texture565 (32, 32, crt::gold);
	title = kit::texture (crt_labels::title);
	state[0] = kit::texture (crt_labels::off);
	state[1] = kit::texture (crt_labels::on);
	note = kit::texture (crt_labels::note);
	detail = kit::texture (crt_labels::detail);
	update (0);
}

void draw ()
{
	kit::background (gradient);
	kit::begin (camera, light);
	kit::Draw how;				// the wall: painted back to front
	how.depth_test = false;
	how.cull = kit::NONE;
	how.count = poster_first;
	kit::draw (wall, how);
	how.first = poster_first;
	how.count = rails_first - poster_first;
	how.texture = backdrop;
	kit::draw (wall, how);
	how.first = rails_first;
	how.count = -1;
	how.texture = 0;
	kit::draw (wall, how);

	kit::Draw box;
	box.model = kit::translation (190, 10, 870) * kit::rotation ((float) pitch, (float) yaw, (float) roll);
	for (int face = 0; face < 6; face++)	// a texture for each pair of faces
	{
		box.texture = tile[face % 3];
		box.first = face * 6;
		box.count = 6;
		kit::draw (cube, box);
	}
	if (mode)
		kit::scanlines (112);
	kit::sprite (title, crt_labels::title.width, crt_labels::title.height, 18, 14);
	kit::sprite (state[mode], crt_labels::off.width, crt_labels::off.height, 18, 45);
	kit::sprite (detail, crt_labels::detail.width, crt_labels::detail.height, 18, 271);
	kit::sprite (note, crt_labels::note.width, crt_labels::note.height, 18, 295);
}

}  // namespace

int main ()
{
	return kit::run ({"CRT", init, update, draw});
}
