// tropical-island - JetExamples' "Tropical island" (esp32-tropical-island,
// MIT, CubeCoders) for piegpu's OpenGL: an island of flat-coloured meshes the
// camera circles, in a sea that mirrors it, under a sun with a lens flare.
// The sea is Jet's screen-space reflection: the island and the sky are drawn
// into a texture first; the water's shader (kit_water) takes its picture
// from there, upside down about the waterline and rippled. The flare: bitmaps
// added along the line from the sun through the screen's middle, fading when
// something of the island is before the sun.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "kit.hpp"
#include "../assets/island_labels.hpp"
#include "kit_water_program.h"

namespace {

constexpr float PI = 3.14159265359f;
constexpr float SUN[3] = {0.20f, 0.18f, 0.963f};	// towards it
constexpr unsigned WATER = 0x088C9C;

kit::Camera camera;
kit::Light light;				// none: every colour as it is
kit::Mesh island, sea;
uint16_t sky[kit::H];
GLuint title, caption, glow, ring, disc;
GLuint water_program;
GLint w_pos, w_mvp, w_area, w_water, w_color;
float seconds_in = 0, visibility = 0;
int sun_x, sun_y;
bool sun_ahead;

struct P { float x, y, z; };

kit::Material paint (unsigned hex)
{
	return kit::Material (kit::to565 (hex), 255, 255, 0, false);
}

void quad (kit::Mesh& mesh, P a, P b, P c, P d, const kit::Material& m)
{
	mesh.triangle (&a.x, &b.x, &c.x, m);
	mesh.triangle (&c.x, &d.x, &a.x, m);
}

void tri (kit::Mesh& mesh, P a, P b, P c, const kit::Material& m)
{
	mesh.triangle (&a.x, &b.x, &c.x, m);
}

float coast (float a)
{
	return 1 + 0.09f * std::sin (a * 3 + 0.5f) + 0.05f * std::cos (a * 5);
}

void make_beach ()
{
	const kit::Material sand[4] = {paint (0xE6C587), paint (0xFFE2A0), paint (0xF4D395), paint (0xFADDA0)};
	const kit::Material grass[3] = {paint (0x75AA3E), paint (0x85BC43), paint (0x639B32)};
	constexpr int n = 24;
	static const float scales[4] = {1.14f, 1.0f, 0.82f, 0.56f}, heights[4] = {1, 7, 42, 73};
	P rim[4][n];
	for (int r = 0; r < 4; r++)
		for (int i = 0; i < n; i++)
		{
			const float a = 2 * PI * i / n;
			rim[r][i] = {650 * scales[r] * coast (a) * std::cos (a), heights[r], 470 * scales[r] * coast (a) * std::sin (a)};
		}
	for (int r = 0; r < 3; r++)
		for (int i = 0; i < n; i++)
		{
			const int j = (i + 1) % n;
			quad (island, rim[r][i], rim[r][j], rim[r + 1][j], rim[r + 1][i], sand[(i + r) % 4]);
		}
	for (int i = 0; i < n; i++)
		tri (island, rim[3][i], rim[3][(i + 1) % n], {0, 96, 0}, grass[i % 3]);
	// a narrow, broken pale lip at the shore. (Jet paints it over the sand; with
	// a depth buffer it has to lie above it: the sand is 6 high there)
	const kit::Material foam = paint (0xC6F1CE);
	P lip[n][2];
	for (int i = 0; i < n; i++)
	{
		const float a = 2 * PI * i / n;
		lip[i][0] = {650 * coast (a) * 1.022f * std::cos (a), 7, 470 * coast (a) * 1.022f * std::sin (a)};
		lip[i][1] = {650 * coast (a) * 1.014f * std::cos (a), 8, 470 * coast (a) * 1.014f * std::sin (a)};
	}
	for (int i = 0; i < n; i++)
		if (i % 4 != 0)
		{
			const int j = (i + 1) % n;
			quad (island, lip[i][0], lip[j][0], lip[j][1], lip[i][1], foam);
		}
}

void make_palm (float x, float z, float height, float phase)
{
	const kit::Material bark[5] = {paint (0x94613B), paint (0xBB8852), paint (0xD09D60), paint (0xAA7844), paint (0x785335)};
	const kit::Material leaf[4] = {paint (0x228546), paint (0x55B94E), paint (0x8BCF50), paint (0x389D40)};
	const P at = {(float) (int) x, 72, (float) (int) z};
	auto put = [&] (P p) { return P {std::round (p.x) + at.x, std::round (p.y) + at.y, std::round (p.z) + at.z}; };
	constexpr int sides = 5;
	P trunk[4][sides];
	for (int row = 0; row <= 3; row++)
	{
		const float t = row / 3.0f, bend = t * t * height * 0.17f;
		for (int i = 0; i < sides; i++)
		{
			const float a = 2 * PI * i / sides, radius = 20 - t * 9;
			trunk[row][i] = put ({std::cos (phase) * bend + radius * std::cos (a), height * t,
					      std::sin (phase) * bend + radius * std::sin (a)});
		}
	}
	for (int row = 0; row < 3; row++)
		for (int i = 0; i < sides; i++)
		{
			const int j = (i + 1) % sides;
			quad (island, trunk[row][i], trunk[row][j], trunk[row + 1][j], trunk[row + 1][i], bark[i]);
		}
	const P crown = {std::cos (phase) * height * 0.17f, height, std::sin (phase) * height * 0.17f};
	for (int i = 0; i < 7; i++)
	{
		const float a = phase + i * 2 * PI / 7;
		const P dir = {std::cos (a), 0, std::sin (a)}, side = {-dir.z, 0, dir.x};
		const float length = height * (0.46f + 0.03f * (i % 3)), half = length * 0.15f;
		const P ridge = {crown.x + dir.x * length * 0.43f, crown.y + height * 0.09f, crown.z + dir.z * length * 0.43f};
		const P v[5] = {put (crown),
				put ({ridge.x + side.x * half, ridge.y - 23, ridge.z + side.z * half}),
				put (ridge),
				put ({ridge.x - side.x * half, ridge.y - 23, ridge.z - side.z * half}),
				put ({crown.x + dir.x * length, crown.y - height * 0.15f, crown.z + dir.z * length})};
		tri (island, v[0], v[1], v[2], leaf[i % 4]);
		tri (island, v[0], v[2], v[3], leaf[(i + 1) % 4]);
		tri (island, v[1], v[4], v[2], leaf[i % 4]);
		tri (island, v[2], v[4], v[3], leaf[(i + 1) % 4]);
	}
}

void make_rock (float x, float z, float size, float phase)
{
	const kit::Material grey[4] = {paint (0x879B97), paint (0xAABAB1), paint (0x6A837F), paint (0xC0C5A8)};
	P v[2][5];
	for (int row = 0; row < 2; row++)
		for (int i = 0; i < 5; i++)
		{
			const float a = phase + i * 2 * PI / 5, r = size * (row ? 0.64f : 1.0f);
			v[row][i] = {x + r * std::cos (a), row ? size * 0.8f : 8, z + r * 0.75f * std::sin (a)};
		}
	const P top = {x + size * 0.1f, size * 1.03f, z};
	for (int i = 0; i < 5; i++)
	{
		const int j = (i + 1) % 5;
		quad (island, v[0][i], v[0][j], v[1][j], v[1][i], grey[i % 4]);
		tri (island, v[1][i], v[1][j], top, grey[(i + 1) % 4]);
	}
}

// the three bitmaps of the flare: a quarter of each, mirrored into the whole when drawn
void make_sun ()
{
	uint16_t pixels[3][16 * 16];
	for (int y = 0; y < 16; y++)
		for (int x = 0; x < 16; x++)
		{
			const float dx = x - 15.5f, dy = y - 15.5f, r = std::sqrt (dx * dx + dy * dy) / 16;
			float g = std::max (0.0f, 1 - r);
			g = g * g * 0.33f;
			const float h = std::max (0.0f, 1 - std::fabs (r - 0.65f) / 0.18f) * 0.3f;
			const float d = std::min (std::max ((0.57f - r) * 20, 0.0f), 1.0f);
			auto rgb = [] (unsigned red, unsigned green, unsigned blue) { return (uint16_t) ((red >> 3) << 11 | (green >> 2) << 5 | (blue >> 3)); };
			pixels[0][y * 16 + x] = rgb ((unsigned) (255 * g), (unsigned) (226 * g), (unsigned) (151 * g));
			pixels[1][y * 16 + x] = rgb ((unsigned) (132 * h), (unsigned) (203 * h), (unsigned) (235 * h));
			pixels[2][y * 16 + x] = rgb ((unsigned) (255 * d), (unsigned) (246 * d), (unsigned) (202 * d));
		}
	glow = kit::texture565 (16, 16, pixels[0]);
	ring = kit::texture565 (16, 16, pixels[1]);
	disc = kit::texture565 (16, 16, pixels[2]);
}

// is something of the island on the line from the eye to the sun?
bool sun_hidden ()
{
	const float o[3] = {camera.x, camera.y, camera.z};
	for (size_t i = 0; i + 2 < island.vertices.size (); i += 3)
	{
		const kit::Vertex &a = island.vertices[i], &b = island.vertices[i + 1], &c = island.vertices[i + 2];
		const float e1[3] = {(float) (b.x - a.x), (float) (b.y - a.y), (float) (b.z - a.z)};
		const float e2[3] = {(float) (c.x - a.x), (float) (c.y - a.y), (float) (c.z - a.z)};
		const float p[3] = {SUN[1] * e2[2] - SUN[2] * e2[1], SUN[2] * e2[0] - SUN[0] * e2[2], SUN[0] * e2[1] - SUN[1] * e2[0]};
		const float det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
		if (std::fabs (det) < 1e-6f)
			continue;
		const float t[3] = {o[0] - a.x, o[1] - a.y, o[2] - a.z};
		const float u = (t[0] * p[0] + t[1] * p[1] + t[2] * p[2]) / det;
		if (u < 0 || u > 1)
			continue;
		const float q[3] = {t[1] * e1[2] - t[2] * e1[1], t[2] * e1[0] - t[0] * e1[2], t[0] * e1[1] - t[1] * e1[0]};
		const float v = (SUN[0] * q[0] + SUN[1] * q[1] + SUN[2] * q[2]) / det;
		if (v < 0 || u + v > 1)
			continue;
		if ((e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) / det > 0)
			return true;
	}
	return false;
}

void update (float seconds)
{
	seconds_in = std::fmod (seconds_in + seconds, 3600.0f);
	const float angle = seconds_in * (PI / 7.5f);				// round in 15 seconds
	const float radius = 1800 + 260 * std::sin (seconds_in * PI / 15);	// nearer and further, in 30
	camera.x = (float) (int) (std::sin (angle) * radius);
	camera.y = (float) (int) (180 + 30 * std::sin (angle * 2));
	camera.z = (float) (int) (-std::cos (angle) * radius);
	camera.look_at (0, 160, 0);
	// the flare: where the sun is on the screen; it comes and goes in a sixth of a second
	float x, y;
	sun_ahead = camera.project (camera.x + SUN[0] * 1024, camera.y + SUN[1] * 1024, camera.z + SUN[2] * 1024, &x, &y);
	sun_x = (int) x;
	sun_y = (int) y;
	if (!sun_ahead)
		visibility = 0;
	else
		visibility = std::min (1.0f, std::max (0.0f, visibility + (sun_hidden () ? -6.0f : 6.0f) * seconds));
}

void init ()
{
	camera.fov = 64;
	camera.near_plane = 64;
	camera.far_plane = 8500;
	for (int y = 0; y < kit::H; y++)
	{
		const float t = std::min (1.0f, y / 175.0f);
		sky[y] = (uint16_t) (((unsigned) (30 + 155 * t) >> 3) << 11 | ((unsigned) (130 + 98 * t) >> 2) << 5 | ((unsigned) (210 + 30 * t) >> 3));
	}
	make_beach ();
	make_palm (-235, -40, 505, 0.25f);
	make_palm (105, 110, 600, 2.4f);
	make_palm (280, -70, 395, 1.2f);
	make_palm (-50, -200, 420, 4.5f);
	make_palm (-295, 180, 380, 3.5f);
	make_rock (550, 90, 68, 0.2f);
	make_rock (605, 125, 44, 0.7f);
	make_rock (-470, -240, 80, 0.5f);
	make_rock (-535, -225, 46, 0.3f);
	for (int z = -3; z < 3; z++)			// the sea: tiles, so that none is huge
		for (int x = -3; x < 3; x++)
		{
			constexpr float step = 1600;
			quad (sea, {x * step, 0, z * step}, {(x + 1) * step, 0, z * step}, {(x + 1) * step, 0, (z + 1) * step},
			      {x * step, 0, (z + 1) * step}, paint (WATER));
		}
	make_sun ();
	title = kit::texture (island_labels::title);
	caption = kit::texture (island_labels::caption);

	water_program = glCreateProgram ();
	glProgramBinaryOES (water_program, PGL_PROGRAM_BINARY_PGPU, &kit_water_info, sizeof kit_water_info);
	w_pos = glGetAttribLocation (water_program, "a_pos");
	w_mvp = glGetUniformLocation (water_program, "u_mvp");
	w_area = glGetUniformLocation (water_program, "u_area");
	w_water = glGetUniformLocation (water_program, "u_water");
	w_color = glGetUniformLocation (water_program, "u_color");
	glUseProgram (water_program);
	glUniform1i (glGetUniformLocation (water_program, "u_frame"), 0);
	glUniform1i (glGetUniformLocation (water_program, "u_sky"), 1);
	update (0);
}

void draw_sea (GLuint picture)
{
	// Jet's numbers: the horizon's row from the camera's pitch; the shore's from
	// the island's middle on the water. The mirror is about the horizon, shifted
	// so that the island's reflection starts at its shore
	const float f = (kit::W / 2) / std::tan (camera.fov * PI / 360);
	const int horizon = kit::H / 2 + (int) (std::sin (-camera.pitch * PI / 180) * f);
	float sx, shore;
	camera.project (0, 0, 0, &sx, &shore);
	const int bias = std::min (std::max ((int) std::lround (2 * (shore - horizon)), 0), 255);
	int ax, ay, aw, ah;
	kit::area (&ax, &ay, &aw, &ah);
	const kit::Rgb c = kit::rgb565 (kit::to565 (WATER));
	sea.upload ();
	kit::own_gl_begin ();
	glUseProgram (water_program);
	glUniformMatrix4fv (w_mvp, 1, GL_FALSE, kit::view_projection ().m);
	glUniform4f (w_area, (float) ax, (float) (ay + ah), 1.0f / aw, (float) kit::H / ah);
	glUniform4f (w_water, (float) horizon, (float) bias, std::round (shore), std::fmod ((float) (int) (seconds_in * 240.0f), 360.0f));
	glUniform4f (w_color, c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, 160 / 255.0f);
	glActiveTexture (GL_TEXTURE1);
	glBindTexture (GL_TEXTURE_2D, kit::gradient ());
	glActiveTexture (GL_TEXTURE0);
	glBindTexture (GL_TEXTURE_2D, picture);
	glDisable (GL_BLEND);
	glDisable (GL_CULL_FACE);
	glDisable (GL_DEPTH_TEST);
	glBindBuffer (GL_ARRAY_BUFFER, sea.buffer);
	for (GLint i = 0; i < 8; i++)
	{
		if (i == w_pos)
			glEnableVertexAttribArray (i);
		else
			glDisableVertexAttribArray (i);
	}
	glVertexAttribPointer (w_pos, 4, GL_SHORT, GL_FALSE, sizeof (kit::Vertex), (void*) 0);
	glDrawArrays (GL_TRIANGLES, 0, (GLsizei) sea.vertices.size ());
	kit::own_gl_end ();
}

void draw ()
{
	kit::Draw land;
	land.cull = kit::NONE;
	// what the sea mirrors
	kit::capture_begin (2);
	kit::background (sky);
	kit::begin (camera, light);
	kit::draw (island, land);
	const GLuint picture = kit::capture_end ();
	// the picture
	kit::background (sky);
	draw_sea (picture);
	kit::draw (island, land);

	if (sun_ahead && visibility > 0)
	{
		// (the sun's own bitmap is 128 across: it fades as it leaves the screen)
		const float half = 64;
		float edge = std::min (std::min (sun_x / half, (kit::W - 1 - sun_x) / half), std::min (sun_y / half, (kit::H - 1 - sun_y) / half));
		edge = std::min (std::max (edge, 0.0f), 1.0f);
		const float rx = kit::W * 0.5f - sun_x, ry = kit::H * 0.5f - sun_y, length = std::sqrt (rx * rx + ry * ry);
		float dx = 0, dy = 0, reach = 0;
		if (length > 0.5f)
		{
			dx = rx / length;
			dy = ry / length;
			for (int corner = 0; corner < 4; corner++)	// to the furthest corner
			{
				const float cx = (corner & 1 ? kit::W : 0) - sun_x, cy = (corner & 2 ? kit::H : 0) - sun_y;
				reach = std::max (reach, std::sqrt (cx * cx + cy * cy));
			}
		}
		const struct { GLuint texture; float along, alpha; int scale; } part[7] =
		{
			{glow, 0, 1, 4}, {disc, 0, 1, 1}, {ring, -0.10f, 0.45f, 1}, {glow, 0.19f, 0.45f, 1},
			{ring, 0.36f, 0.8f, 2}, {glow, 0.58f, 0.35f, 1}, {ring, 0.77f, 0.55f, 1},
		};
		for (const auto& e : part)
		{
			const float px = sun_x + dx * e.along * reach, py = sun_y + dy * e.along * reach;
			const int size = 32 * e.scale;
			kit::sprite (e.texture, 16, 16, (int) (px - size * 0.5f), (int) (py - size * 0.5f),
				     (int) (visibility * edge * e.alpha * 255.0f), true, e.scale, kit::MIRROR_X | kit::MIRROR_Y);
		}
	}
	kit::sprite (title, island_labels::title.width, island_labels::title.height, 22, 17);
	kit::sprite (caption, island_labels::caption.width, island_labels::caption.height, 22, 287);
}

}  // namespace

int main ()
{
	return kit::run ({"Tropical island", init, update, draw});
}
