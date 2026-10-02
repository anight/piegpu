// sprite-controls - JetExamples' "Air Mail" (esp32-sprite-controls, MIT,
// CubeCoders) for piegpu's OpenGL: a courier plane, a bitmap over fields that
// pass below: mirrored as it turns back, flown upside down, followed by
// fainter echoes of itself, and under a film's bars and a fade to black;
// seven seconds each.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "kit.hpp"
#include "../assets/airmail.hpp"
#include "../assets/airmail_labels.hpp"

namespace {

constexpr float PI = 3.14159265359f;

kit::Camera camera;
kit::Light light;				// none: every colour as it is
kit::Mesh far_land, fields, rails, stripe;
uint16_t sky[kit::H];
GLuint ship, title, mode_label[4], desc_label[4];
float seconds_in = 0;
int stage = -1;

kit::Material paint (unsigned hex)
{
	return kit::Material (kit::to565 (hex), 255, 255, 0, false);
}

void quad (kit::Mesh& mesh, float ax, float ay, float az, float bx, float by, float bz, float cx, float cy, float cz,
	   float dx, float dy, float dz, const kit::Material& m)
{
	const float a[3] = {ax, ay, az}, b[3] = {bx, by, bz}, c[3] = {cx, cy, cz}, d[3] = {dx, dy, dz};
	mesh.triangle (a, b, c, m);
	mesh.triangle (c, d, a, m);
}

// where the plane is at a time, and which way it looks
void place (float t, int* x, int* y, unsigned* flags)
{
	const float a = t * 2 * PI / 7;
	*x = 176 + (int) (145 * std::sin (a));
	*y = 143 + (int) (22 * std::sin (2 * a));
	*flags = std::cos (a) < 0 ? kit::FLIP_X : 0;
}

void update (float seconds)
{
	static const char* const names[] = {"DIRECTION / FLIP X", "INVERT / FLIP Y", "ECHO / COMBINED ALPHA", "CINEMA / BARS + FADE"};
	seconds_in = std::fmod (seconds_in + seconds, 28.0f);
	const int next = (int) (seconds_in / 7);
	if (next != stage)
		std::printf ("Courier: %s\n", names[next]);
	stage = next;
	camera.x = (float) (int) (35 * std::sin (seconds_in * 2 * PI / 28));
	camera.y = 190;
	camera.z = -600;
	camera.look_at (0, 120, 900);
}

void init ()
{
	camera.fov = 58;
	camera.near_plane = 40;
	camera.far_plane = 6000;
	for (int y = 0; y < kit::H; y++)
		sky[y] = kit::to565 (((91 + y * 83 / 320) << 16) | ((155 + y * 57 / 320) << 8) | (200 + y * 30 / 320));
	// a warm sun behind the hills, the hills, the ground
	const kit::Material sun = paint (0xFFF1C3), dark = paint (0x567E75), bright = paint (0x84A08A);
	for (int y = -210; y < 210; y += 20)
	{
		const int x = (int) std::sqrt ((float) (210 * 210 - y * y)), yy = std::min (210, y + 20);
		const int xx = (int) std::sqrt ((float) (210 * 210 - yy * yy));
		quad (far_land, (float) -x, (float) (560 + y), 2600, (float) x, (float) (560 + y), 2600, (float) xx, (float) (560 + yy), 2600,
		      (float) -xx, (float) (560 + yy), 2600, sun);
	}
	for (int i = 0; i < 12; i++)
	{
		const float x = (float) (-2400 + i * 400), h = (float) (230 + (i * 137) % 420);
		const float p[4][3] = {{x, 0, 1950}, {x + 260, h, 1950}, {x + 520, 0, 1950}, {x + 260, 0, 1870}};
		far_land.triangle (p[0], p[1], p[3], dark);
		far_land.triangle (p[1], p[2], p[3], bright);
	}
	quad (far_land, -4000, 0, -500, 4000, 0, -500, 4000, 0, 4000, -4000, 0, 4000, paint (0x73934D));
	// fourteen rows of crop plots (96 vertices each), earthen tracks between them
	const kit::Material crops[4] = {paint (0x8EA35B), paint (0xA7AA61), paint (0x628547), paint (0xC0AF71)};
	for (int i = 0; i < 14; i++)
		for (int x = -1600, j = 0; x < 1600; x += 200, j++)
			quad (fields, (float) (x + 9), 1, 8, (float) (x + 191), 1, 8, (float) (x + 191), 1, 172, (float) (x + 9), 1, 172,
			      crops[(i * 3 + j) % 4]);
	const kit::Material track = paint (0xB4B56E);
	for (int x = -1600; x <= 1600; x += 200)
		quad (rails, (float) (x - 6), 1, -500, (float) (x + 6), 1, -500, (float) (x + 6), 1, 2500, (float) (x - 6), 1, 2500, track);
	quad (stripe, -1800, 2, -3, 1800, 2, -3, 1800, 2, 3, -1800, 2, 3, track);
	for (kit::Mesh* mesh : {&far_land, &fields, &rails, &stripe})	// (they stay as they are: the RPi has them)
		mesh->hand_over ();

	const kit::Image keyed = {64, 32, airmail::ship, true, 0};
	ship = kit::texture (keyed);
	using namespace airmail_labels;
	title = kit::texture (airmail_labels::title);
	const kit::Image* const modes[4] = {&direction, &invert, &echo, &cinema};
	const kit::Image* const descriptions[4] = {&directionDesc, &invertDesc, &echoDesc, &cinemaDesc};
	for (int i = 0; i < 4; i++)
	{
		mode_label[i] = kit::texture (*modes[i]);
		desc_label[i] = kit::texture (*descriptions[i]);
	}
	update (0);
}

void draw ()
{
	const float phase = std::fmod (seconds_in, 7.0f);
	kit::background (sky);
	kit::begin (camera, light);
	kit::Draw how;					// the land is painted far to near, in this order
	how.depth_test = false;
	how.cull = kit::NONE;
	kit::draw (far_land, how);
	int row_z[14];
	for (int i = 0; i < 14; i++)			// the rows come towards the camera, and start again far off
		row_z[i] = (int) std::fmod (i * 180.0f - seconds_in * 180.0f + 7560.0f, 2520.0f) - 500;
	for (int i = 0; i < 14; i++)
	{
		how.model = kit::translation (0, 0, (float) row_z[i]);
		how.first = i * 96;
		how.count = 96;
		kit::draw (fields, how);
	}
	how.first = 0;
	how.count = -1;
	how.model = kit::identity ();
	kit::draw (rails, how);
	for (int i = 0; i < 14; i++)
	{
		how.model = kit::translation (0, 0, (float) row_z[i]);
		kit::draw (stripe, how);
	}

	int x, y;
	unsigned flags;
	if (stage == 2)
	{
		// the echoes: the group's opacity times each one's own
		const int group = (uint8_t) (140 + 115 * (0.5f + 0.5f * std::sin (phase * 2 * PI / 7)));
		for (int i = 0; i < 4; i++)
		{
			place (seconds_in - (4 - i) * 0.23f, &x, &y, &flags);
			kit::sprite (ship, 64, 32, x, y, (35 + i * 42) * group / 255, false, 2, flags);
		}
	}
	place (seconds_in, &x, &y, &flags);
	if (stage == 1 && ((int) (phase / 1.75f) & 1))
		flags |= kit::FLIP_Y;
	kit::sprite (ship, 64, 32, x, y, 255, false, 2, flags);
	if (stage == 3)
	{
		const float bars = std::min (1.0f, std::min (phase, 7.0f - phase));
		const int top = (int) (70 * bars), bottom = (int) (43 * bars);
		kit::rect (0, 0, 480, top, 0);
		kit::rect (0, 320 - bottom, 480, bottom, 0);
		const float f = phase > 4 ? std::sin ((phase - 4) * PI / 3) : 0;
		const int fade = (uint8_t) (255 * f * f);
		if (fade > 0)
			kit::rect (0, 0, 480, 320, 0, fade);
	}
	kit::rect (0, 0, 480, 65, 0x0842, 110);
	kit::rect (0, 289, 480, 31, 0x0842, 110);
	kit::sprite (title, 310, 27, 18, 13);
	kit::sprite (mode_label[stage], 335, 19, 18, 44);
	kit::sprite (desc_label[stage], stage < 2 ? 450 : 455, 19, 18, 299);
}

}  // namespace

int main ()
{
	return kit::run ({"Air Mail", init, update, draw});
}
