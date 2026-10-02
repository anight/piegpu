// sprites-blending - JetExamples' "Courtyard" (esp32-sprites-blending, MIT,
// CubeCoders) for piegpu's OpenGL: a courtyard at night, built of layers
// shown one after another: the plain geometry; its reflection (the scene
// drawn again upside down, under a floor that lets some of it through);
// pools and cones of light (meshes added to the picture); and the lamps'
// halos (bitmaps added over it). The first fourteen seconds: all of them.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "kit.hpp"
#include "../assets/courtyard.hpp"
#include "../assets/courtyard_labels.hpp"

namespace {

constexpr float PI = 3.14159265359f;
const float LAMP[2][3] = {{-330, 432, 110}, {330, 432, 180}};

kit::Camera camera;
kit::Light light;				// none: every colour as it is
kit::Mesh backdrop, solid, leaves, sign, jewel, floor_mesh, seams, pools, cones;
uint16_t sky[kit::H];
GLuint sign_texture, glow[2], title, mode_label[5], desc_label[5];
float seconds_in = 0;
int stage = -1, jewel_pitch = 0, jewel_yaw = 0;

kit::Material paint (unsigned hex, int alpha = 255)
{
	return kit::Material (kit::to565 (hex), (uint8_t) alpha, 255, 0, false);
}

// added to the picture: Jet adds (alpha + 1) / 256 of the colour
kit::Material glowing (unsigned hex, int alpha)
{
	return paint (hex, alpha + 1);
}

struct P { float x, y, z; };

void quad (kit::Mesh& mesh, P a, P b, P c, P d, const kit::Material& m, bool textured = false)
{
	static const int uv[4][2] = {{0, 1024}, {1024, 1024}, {1024, 0}, {0, 0}};
	mesh.quad (&a.x, &b.x, &c.x, &d.x, m, textured ? uv : nullptr);
}

void fan (kit::Mesh& mesh, P centre, const P* rim, int n, const kit::Material& m)
{
	for (int i = 0; i < n; i++)
		mesh.triangle (&centre.x, &rim[i].x, &rim[(i + 1) % n].x, m);
}

void plant (int x, int z)
{
	solid.box ((float) x, 30, (float) z, 72, 60, 72, paint (0x7F5578));
	solid.box ((float) x, 63, (float) z, 80, 9, 80, paint (0xD3939F));
	for (int i = 0; i < 7; i++)
	{
		const float a = i * 2 * PI / 7;
		const P base = {(float) x, 65, (float) z};
		const P tip = {(float) (x + (int) (85 * std::cos (a))), (float) (110 + (i % 3) * 45), (float) (z + (int) (85 * std::sin (a)))};
		const P left = {(float) (x + (int) (28 * std::cos (a - 0.5f))), 120, (float) (z + (int) (28 * std::sin (a - 0.5f)))};
		const P right = {(float) (x + (int) (28 * std::cos (a + 0.5f))), 120, (float) (z + (int) (28 * std::sin (a + 0.5f)))};
		quad (leaves, base, left, tip, right, paint (i % 2 ? 0x438F89 : 0x68BB9E));
	}
}

// a lamp's light: discs one in another on the floor (a soft pool, no texture,
// no real light), and thin cone shells: dust in the beam
void lamp_light (const float head[3], unsigned colour)
{
	for (int ring = 0; ring < 5; ring++)
	{
		const int radius = 185 - ring * 30;
		P rim[16];
		for (int i = 0; i < 16; i++)
		{
			const float a = i * 2 * PI / 16;
			rim[i] = {head[0] + (int) (radius * std::cos (a)), 2, head[2] + (int) (radius * std::sin (a))};
		}
		fan (pools, {head[0], 2, head[2]}, rim, 16, glowing (colour, 7 + ring * 3));
	}
	for (int ring = 0; ring < 2; ring++)
	{
		const int radius = 160 - ring * 45;
		P rim[12];
		for (int i = 0; i < 12; i++)
		{
			const float a = i * 2 * PI / 12;
			rim[i] = {head[0] + (int) (radius * std::cos (a)), 4, head[2] + (int) (radius * std::sin (a))};
		}
		fan (cones, {head[0], head[1] - 8, head[2]}, rim, 12, glowing (colour, ring ? 4 : 5));
	}
}

void update (float seconds)
{
	static const char* const names[] = {"AFTER HOURS / ALL LAYERS", "UNLIT GEOMETRY", "MIRRORS + ALPHA FLOOR", "ADDITIVE LIGHT MESHES", "SPRITE HALOS"};
	seconds_in = std::fmod (seconds_in + seconds, 42.0f);
	const int next = seconds_in < 14 ? 0 : 1 + (int) ((seconds_in - 14) / 7);
	if (next != stage)
		std::printf ("Courtyard: %s\n", names[next]);
	stage = next;
	const float a = seconds_in * 2 * PI / 14;
	// a sweep that rises and falls: it shows the floor's trick
	camera.x = (float) (int) (360 * std::sin (a));
	camera.y = (float) (480 + (int) (65 * std::sin (a + 0.6f)));
	camera.z = (float) (-1100 + (int) (90 * std::cos (a)));
	camera.look_at (0, 135, 90);
	jewel_yaw = (int) (seconds_in * 360 / 14) % 360;
	jewel_pitch = (int) (10 * std::sin (a));
}

void init ()
{
	camera.fov = 58;
	camera.near_plane = 40;
	camera.far_plane = 6000;
	for (int y = 0; y < kit::H; y++)
		sky[y] = kit::to565 (((17 + y * 10 / 320) << 16) | ((25 + y * 25 / 320) << 8) | (48 + y * 26 / 320));
	// behind the courtyard, and not reflected: the moon, a skyline
	{
		P rim[24];
		for (int i = 0; i < 24; i++)
		{
			const float a = i * 2 * PI / 24;
			rim[i] = {(float) (-950 + (int) (92 * std::cos (a))), (float) (200 + (int) (92 * std::sin (a))), 1300};
		}
		fan (backdrop, {-950, 200, 1300}, rim, 24, paint (0xC3C6CB));
	}
	for (int i = 0; i < 9; i++)
	{
		const float x = (float) (-1050 + i * 250), top = (float) (260 + (i % 3) * 110);
		quad (backdrop, {x, 0, 1100}, {x + 200, 0, 1100}, {x + 200, top, 1100}, {x, top, 1100}, paint (i % 2 ? 0x24324E : 0x2C3B57));
	}
	// the cafe's low wall, its sign
	solid.box (0, 135, 490, 680, 270, 45, paint (0x3A405A));
	solid.box (0, 277, 480, 710, 18, 65, paint (0x64868D));
	for (int x : {-260, 260})
	{
		solid.box ((float) x, 145, 461, 88, 190, 8, paint (0x243348));
		solid.box ((float) x, 145, 454, 7, 180, 5, paint (0x6BBDB9));
	}
	quad (sign, {-190, 82, 460}, {190, 82, 460}, {190, 224, 460}, {-190, 224, 460}, paint (0xFFFFFF), true);
	// the plinth and its jewel
	solid.box (0, 26, 40, 190, 52, 160, paint (0x657E8F));
	solid.box (0, 57, 40, 160, 10, 134, paint (0xB8A4B6));
	{
		const float size = 110, w = 110 * 2 / 3;
		const P points[6] = {{0, size, 0}, {w, 0, 0}, {0, 0, w}, {-w, 0, 0}, {0, 0, -w}, {0, -size, 0}};
		static const unsigned colours[8] = {0xFC92BE, 0xA54C98, 0x61E5CF, 0x36758B, 0xF3BF98, 0x824E88, 0x39A5AE, 0xB86FA9};
		for (int i = 0; i < 4; i++)
		{
			const int a = 1 + i, b = 1 + (i + 1) % 4;
			jewel.triangle (&points[0].x, &points[b].x, &points[a].x, paint (colours[i]));
			jewel.triangle (&points[5].x, &points[a].x, &points[b].x, paint (colours[i + 4]));
		}
	}
	for (int i = 0; i < 2; i++)
	{
		const float* p = LAMP[i];
		const float post = p[0] + (i ? 40 : -40);
		solid.box (post, 205, p[2], 15, 410, 15, paint (0x7598A6));
		solid.box (p[0], 425, p[2], 96, 15, 47, paint (0x7392A0));
		solid.box (p[0], 415, p[2], 77, 5, 35, paint (i ? 0x92F5FF : 0xFFD6A2));
		solid.box (post, 12, p[2], 60, 24, 60, paint (0x435B74));
	}
	plant (-480, -65);
	plant (470, 35);
	// the floor, the seams of its slabs
	quad (floor_mesh, {-1500, 0, -850}, {1500, 0, -850}, {1500, 0, 1500}, {-1500, 0, 1500}, paint (0x25424E));
	const kit::Material seam = paint (0x71939F, 48);
	for (int z = -600; z <= 600; z += 200)
		quad (seams, {-720, 1, (float) (z - 3)}, {720, 1, (float) (z - 3)}, {720, 1, (float) (z + 3)}, {-720, 1, (float) (z + 3)}, seam);
	for (int x = -600; x <= 600; x += 200)
		quad (seams, {(float) (x - 3), 1, -750}, {(float) (x + 3), 1, -750}, {(float) (x + 3), 1, 650}, {(float) (x - 3), 1, 650}, seam);
	lamp_light (LAMP[0], 0xFFD198);
	lamp_light (LAMP[1], 0x7DE9FA);
	for (kit::Mesh* mesh : {&backdrop, &solid, &leaves, &sign, &jewel, &floor_mesh, &seams, &pools, &cones})
		mesh->hand_over ();			// (they stay as they are: the RPi has them)

	sign_texture = kit::texture565 (128, 48, courtyard::sign);
	const kit::Image warm = {16, 16, courtyard::warmGlow, true, 0}, cool = {16, 16, courtyard::coolGlow, true, 0};
	glow[0] = kit::texture (warm);
	glow[1] = kit::texture (cool);
	using namespace courtyard_labels;
	title = kit::texture (courtyard_labels::title);
	const kit::Image* const modes[5] = {&full, &bare, &mirror, &courtyard_labels::light, &halo};
	const kit::Image* const descriptions[5] = {&fullDesc, &bareDesc, &mirrorDesc, &lightDesc, &haloDesc};
	for (int i = 0; i < 5; i++)
	{
		mode_label[i] = kit::texture (*modes[i]);
		desc_label[i] = kit::texture (*descriptions[i]);
	}
	update (0);
}

// the courtyard's things, as they are or upside down (their reflection)
void subjects (bool mirrored)
{
	const kit::Mat4 world = mirrored ? kit::scaling (1, -1, 1) : kit::identity ();
	kit::Draw how;
	how.model = world;
	how.cull = mirrored ? kit::FRONT : kit::BACK;	// (upside down a face runs the other way round)
	kit::draw (solid, how);
	how.model = world * kit::translation (0, 202, 40) * kit::rotation ((float) jewel_pitch, (float) jewel_yaw, 0);
	kit::draw (jewel, how);
	how.model = world;
	how.cull = kit::NONE;
	kit::draw (leaves, how);
	how.texture = sign_texture;
	kit::draw (sign, how);
}

void draw ()
{
	const bool reflection = stage != 1, lights = stage == 0 || stage >= 3, halos = stage == 0 || stage == 4;
	kit::background (sky);
	kit::begin (camera, light);
	kit::Draw flat;					// painted in this order, over what's there
	flat.depth_test = false;
	flat.cull = kit::NONE;
	kit::draw (backdrop, flat);
	if (reflection)
		subjects (true);
	flat.blend = kit::ALPHA;
	flat.alpha = reflection ? 158 / 255.0f : 1.0f;	// the floor lets the reflection through
	kit::draw (floor_mesh, flat);
	flat.alpha = 1;
	kit::draw (seams, flat);
	if (lights)
	{
		flat.blend = kit::ADD;
		kit::draw (pools, flat);
	}
	subjects (false);
	if (lights)
	{
		kit::Draw beam;				// over what's behind it, hidden by what's before it
		beam.cull = kit::NONE;
		beam.blend = kit::ADD;
		beam.depth_write = false;
		kit::draw (cones, beam);
	}

	if (halos)
		for (int i = 0; i < 2; i++)
		{
			float x, y;
			if (camera.project (LAMP[i][0], LAMP[i][1], LAMP[i][2], &x, &y))
				kit::sprite (glow[i], 16, 16, 240 + (int) (x - 240) - 32, 160 - (int) (160 - y) - 32, 255, true, 2,
					     kit::MIRROR_X | kit::MIRROR_Y);
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
	return kit::run ({"Courtyard", init, update, draw});
}
