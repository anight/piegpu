// matter - JetExamples' "Matter" (esp32-matter, MIT, CubeCoders) for piegpu's
// OpenGL: an exhibition in motion, ten rooms of eighteen seconds: gears,
// rising slabs, a paper landscape with birds, a quicksilver drop, coloured
// rings and slats, a plant of leaves about a seed, ribbons, crystals, a
// tunnel about a knot, and all of them together.
//
// The meshes are built as the original builds them and stay on the RPi: a
// room's things are moved by their matrices. What changes its shape is
// either worked out here and sent anew (the drop, the birds: a few hundred
// vertices), or shaped by its vertex shader (the landscape: kit_paper, the
// ribbons: kit_ribbon). Chrome is Jet's environment mapping: a picture of a
// room, looked up where the eye's reflection goes, per vertex.
// Jet's ground grids are left out: their faces look down, they aren't seen.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>
#include "kit.hpp"
#include "../assets/matter_type.hpp"
#include "kit_paper_program.h"
#include "kit_ribbon_program.h"

namespace {

constexpr float PI = 3.14159265358979323846f, TAU = 2 * PI;
constexpr float CHAPTER_SECONDS = 18, DURATION = 180;
constexpr int CHAPTERS = 10;
const char* const NAMES[CHAPTERS] = {"PRESSURE", "COUNTERWEIGHT", "PAPER WEATHER", "QUICKSILVER", "COLOUR IN SPACE",
				     "BOTANICA", "INTERFERENCE", "RELIQUARY", "THE GYRE", "MATTER"};

// ---- numbers ---------------------------------------------------------------------------------------

struct V
{
	float x = 0, y = 0, z = 0;
	V operator+ (V b) const		{ return {x + b.x, y + b.y, z + b.z}; }
	V operator- (V b) const		{ return {x - b.x, y - b.y, z - b.z}; }
	V operator* (float k) const	{ return {x * k, y * k, z * k}; }
	V cross (V b) const		{ return {y * b.z - z * b.y, z * b.x - x * b.z, x * b.y - y * b.x}; }
	float dot (V b) const		{ return x * b.x + y * b.y + z * b.z; }
	V unit () const
	{
		const float l = std::sqrt (x * x + y * y + z * z);
		return *this * (1 / std::max (l, 0.0001f));
	}
	V whole () const		{ return {std::round (x), std::round (y), std::round (z)}; }
};

float clamp01 (float a)			{ return std::min (std::max (a, 0.0f), 1.0f); }
float mix (float a, float b, float t)	{ return a + (b - a) * t; }

unsigned colour (unsigned a, unsigned b, float t)
{
	unsigned c = 0;
	for (int s : {0, 8, 16})
		c |= (unsigned) mix ((float) ((a >> s) & 255), (float) ((b >> s) & 255), clamp01 (t)) << s;
	return c;
}

float randf (unsigned i)
{
	uint32_t a = i;
	a ^= a >> 16;
	a *= 0x7feb352du;
	a ^= a >> 15;
	a *= 0x846ca68bu;
	a ^= a >> 16;
	return (a & 65535) / 65535.0f;
}

// a thing's turn (degrees: about x, then y, then z), of a vector
V turned (V v, const int r[3])
{
	const kit::Mat4 m = kit::rotation ((float) r[0], (float) r[1], (float) r[2]);
	return {m.m[0] * v.x + m.m[4] * v.y + m.m[8] * v.z, m.m[1] * v.x + m.m[5] * v.y + m.m[9] * v.z,
		m.m[2] * v.x + m.m[6] * v.y + m.m[10] * v.z};
}

// ---- the things of a room --------------------------------------------------------------------------

struct Paint
{
	kit::Material material;
	bool chrome = false;			// the room's picture, where it's mirrored
};

Paint paint (unsigned c, bool lit = true, int alpha = 255)
{
	return {kit::Material (kit::to565 (c), (uint8_t) alpha, 255, 0, lit)};
}

Paint glossy (unsigned c, int specular = 175, int exponent = 28, int diffuse = 200)
{
	Paint p = {kit::Material (kit::to565 (c), 255, (uint8_t) diffuse, (uint8_t) specular)};
	p.material.gloss = (uint8_t) exponent;
	return p;
}

Paint chrome ()
{
	return {kit::Material (0xFFFF, 255, 255, 0, false), true};
}

struct Thing
{
	kit::Mesh mesh, mirror;			// its faces; those of chrome
	V position, base;
	int rotation[3] = {0, 0, 0};
	int kind = 0;
	float phase = 0;
	kit::Cull cull = kit::BACK;
	bool gloss = false;			// lit per pixel (a glossy paint)
	bool reflects = false;			// its chrome's picture follows the eye
	bool ghost = false;			// see-through: drawn after the others
	bool changes = false;			// its vertices do: they are kept here
	~Thing ()
	{
		mesh.free ();
		mirror.free ();
	}
};

std::vector<std::unique_ptr<Thing>> things;
std::vector<Thing*> items;			// those that move
std::vector<V> glows;
Thing *blob = nullptr, *birds = nullptr;
std::vector<V> blob_directions, blob_sines, blob_cosines;
int ribbons = 0;
kit::Mesh landscape, ribbon_mesh[3];
kit::Program paper_program, ribbon_program;
GLint paper_wave, ribbon_wave, ribbon_wave2;

kit::Camera camera;
kit::Light light;
uint16_t sky[kit::H];
GLuint room_texture, halo_texture, type_texture[matter_type::count];
int chapter = -1;
float seconds_in = 0, local = 0;

kit::Mesh& part (Thing& t, const Paint& p)
{
	if (p.material.gloss)
		t.gloss = true;
	return p.chrome ? t.mirror : t.mesh;
}

Thing* thing ()
{
	things.emplace_back (new Thing);
	return things.back ().get ();
}

Thing* item (Thing* t, int kind = 0, float phase = 0)
{
	t->base = t->position;
	t->kind = kind;
	t->phase = phase;
	items.push_back (t);
	return t;
}

void tri (Thing& t, V a, V b, V c, const Paint& p)
{
	kit::Mesh& mesh = part (t, p);
	const V n = (b - a).cross (c - a);
	const int k = mesh.add (a.x, a.y, a.z, n.x, n.y, n.z, p.material, 0, 0);
	mesh.add (b.x, b.y, b.z, n.x, n.y, n.z, p.material, 1024, 0);
	mesh.add (c.x, c.y, c.z, n.x, n.y, n.z, p.material, 512, 1024);
	for (int i = 0; i < 3; i++)
		mesh.indices.push_back ((uint16_t) (k + i));
}

int quad (Thing& t, V a, V b, V c, V d, const Paint& p)
{
	kit::Mesh& mesh = part (t, p);
	const V n = (b - a).cross (c - a);
	const int k = mesh.add (a.x, a.y, a.z, n.x, n.y, n.z, p.material, 0, 0);
	mesh.add (b.x, b.y, b.z, n.x, n.y, n.z, p.material, 1024, 0);
	mesh.add (c.x, c.y, c.z, n.x, n.y, n.z, p.material, 1024, 1024);
	mesh.add (d.x, d.y, d.z, n.x, n.y, n.z, p.material, 0, 1024);
	for (int i : {0, 1, 2, 2, 3, 0})
		mesh.indices.push_back ((uint16_t) (k + i));
	return k;
}

void set_normal (kit::Vertex& v, V n)
{
	n = n.unit ();
	v.nx = (int16_t) std::lround (n.x * 32767);
	v.ny = (int16_t) std::lround (n.y * 32767);
	v.nz = (int16_t) std::lround (n.z * 32767);
}

// points in rows of `across + 1`, with their normals: the faces between them
void faces (kit::Mesh& mesh, int first, int along, int across)
{
	for (int i = 0; i < along; i++)
		for (int j = 0; j < across; j++)
		{
			const int a = first + i * (across + 1) + j, b = a + across + 1;
			for (int k : {a, a + 1, b + 1, b + 1, b, a})
				mesh.indices.push_back ((uint16_t) k);
		}
}

Thing* box (V p, V size, const Paint& m)
{
	Thing* t = thing ();
	part (*t, m).box (0, 0, 0, size.x, size.y, size.z, m.material, m.chrome);
	t->position = p;
	return t;
}

// Jet's createSphere: rows from the top down, a point every 360 / segments degrees
Thing* sphere (int r, int segments, const Paint& m, V p = {})
{
	Thing* t = thing ();
	kit::Mesh& mesh = part (*t, m);
	const int step = 360 / segments, rows = 180 / step, columns = 360 / step;
	for (int lat = 0; lat <= 180; lat += step)
		for (int lon = 0; lon <= 360; lon += step)
		{
			const float a = lat * PI / 180, b = lon * PI / 180;
			const V n = {std::sin (a) * std::cos (b), std::cos (a), std::sin (a) * std::sin (b)};
			mesh.add (std::trunc (r * n.x), std::trunc (r * n.y), std::trunc (r * n.z), n.x, n.y, n.z, m.material,
				  lon * 1024 / 360, lat * 1024 / 180);
		}
	for (int lat = 0; lat < rows; lat++)
		for (int lon = 0; lon < columns; lon++)
		{
			const int current = lat * (columns + 1) + lon, next = current + columns + 1;
			const int v1 = lon == columns - 1 ? current - columns + 1 : current + 1;
			const int v2 = lon == columns - 1 ? next - columns + 1 : next + 1;
			for (int k : {current, v1, v2, v2, next, current})
				mesh.indices.push_back ((uint16_t) k);
		}
	t->position = p;
	return t;
}

Thing* cylinder (int r, int h, int segments, const Paint& m, V p = {})
{
	Thing* t = thing ();
	for (int i = 0; i < segments; i++)
	{
		const float a = TAU * i / segments, b = TAU * (i + 1) / segments;
		const V v = {r * std::cos (a), h * 0.5f, r * std::sin (a)}, w = {r * std::cos (b), h * 0.5f, r * std::sin (b)};
		const V vv = {v.x, -h * 0.5f, v.z}, ww = {w.x, -h * 0.5f, w.z};
		quad (*t, v, w, ww, vv, m);
		tri (*t, {0, h * 0.5f, 0}, w, v, m);
		tri (*t, {0, -h * 0.5f, 0}, vv, ww, m);
	}
	t->position = p;
	return t;
}

Thing* ring (float major, float minor, int around, int sides, const Paint& m, V p = {})
{
	Thing* t = thing ();
	kit::Mesh& mesh = part (*t, m);
	for (int i = 0; i <= around; i++)
	{
		const float u = TAU * i / around;
		for (int j = 0; j <= sides; j++)
		{
			const float v = TAU * j / sides;
			const V n = {std::cos (u) * std::cos (v), std::sin (v), std::sin (u) * std::cos (v)};
			const V q = {std::cos (u) * (major + minor * std::cos (v)), minor * std::sin (v),
				     std::sin (u) * (major + minor * std::cos (v))};
			mesh.add (q.x, q.y, q.z, n.x, n.y, n.z, m.material, i * 1024 / around, j * 1024 / sides);
		}
	}
	faces (mesh, 0, around, sides);
	t->position = p;
	return t;
}

Thing* knot (float radius, float tube, int segments, int sides, const Paint& m)
{
	Thing* t = thing ();
	kit::Mesh& mesh = part (*t, m);
	auto path = [radius] (float s)
	{
		return V {radius * (2 + std::cos (3 * s)) * 0.5f * std::cos (2 * s), radius * 0.5f * std::sin (3 * s),
			  radius * (2 + std::cos (3 * s)) * 0.5f * std::sin (2 * s)};
	};
	for (int i = 0; i <= segments; i++)
	{
		const float u = TAU * i / segments;
		const V c = path (u), along = (path (u + 0.001f) - path (u - 0.001f)).unit ();
		const V n = along.cross ({0, 1, 0}).unit (), b = along.cross (n).unit ();
		for (int j = 0; j <= sides; j++)
		{
			const float v = TAU * j / sides;
			const V normal = n * std::cos (v) + b * std::sin (v), q = c + normal * tube;
			mesh.add (q.x, q.y, q.z, normal.x, normal.y, normal.z, m.material, i * 4096 / segments, j * 1024 / sides);
		}
	}
	faces (mesh, 0, segments, sides);
	return t;
}

// a gear wheel: flat faces and tooth walls, two bands of rounded bevel between them
Thing* gear (int radius, int teeth, const Paint& front, const Paint& edge)
{
	Thing* t = thing ();
	const int n = teeth * 4;
	const float inner = radius * 0.43f;
	auto pt = [n] (int i, float r, float z)
	{
		const float a = TAU * i / n;
		return V {r * std::cos (a), r * std::sin (a), z};
	};
	auto outer = [n, radius] (int i) { return radius * (((i % n + n) % n % 4 < 2) ? 1.0f : 0.89f); };
	auto radial = [&] (int i)
	{
		const V previous = pt (i, outer (i), 0) - pt (i - 1, outer (i - 1), 0);
		const V next = pt (i + 1, outer (i + 1), 0) - pt (i, outer (i), 0);
		return (V {previous.y, -previous.x, 0}.unit () + V {next.y, -next.x, 0}.unit ()).unit ();
	};
	for (int i = 0; i < n; i++)
	{
		const float a = outer (i), b = outer (i + 1);
		quad (*t, pt (i, inner, 26), pt (i, a - 14, 26), pt (i + 1, b - 14, 26), pt (i + 1, inner, 26), front);
		for (int band = 0; band < 2; band++)
		{
			const float u = band * PI / 4, v = (band + 1) * PI / 4;
			const int k = quad (*t, pt (i, a - 14 + 14 * std::sin (u), 12 + 14 * std::cos (u)),
					    pt (i, a - 14 + 14 * std::sin (v), 12 + 14 * std::cos (v)),
					    pt (i + 1, b - 14 + 14 * std::sin (v), 12 + 14 * std::cos (v)),
					    pt (i + 1, b - 14 + 14 * std::sin (u), 12 + 14 * std::cos (u)), edge);
			kit::Mesh& mesh = part (*t, edge);
			set_normal (mesh.vertices[k], radial (i) * std::sin (u) + V {0, 0, std::cos (u)});
			set_normal (mesh.vertices[k + 1], radial (i) * std::sin (v) + V {0, 0, std::cos (v)});
			set_normal (mesh.vertices[k + 2], radial (i + 1) * std::sin (v) + V {0, 0, std::cos (v)});
			set_normal (mesh.vertices[k + 3], radial (i + 1) * std::sin (u) + V {0, 0, std::cos (u)});
		}
		quad (*t, pt (i, a, 12), pt (i, a, -26), pt (i + 1, b, -26), pt (i + 1, b, 12), edge);
		quad (*t, pt (i + 1, inner, -26), pt (i + 1, b, -26), pt (i, a, -26), pt (i, inner, -26), front);
		quad (*t, pt (i + 1, inner, 26), pt (i + 1, inner, -26), pt (i, inner, -26), pt (i, inner, 26), edge);
	}
	return t;
}

Thing* crystal (float r, float length, int sides, const Paint& a, const Paint& b)
{
	Thing* t = thing ();
	for (int i = 0; i < sides; i++)
	{
		const float u = TAU * i / sides, v = TAU * (i + 1) / sides;
		const V p = {r * std::cos (u), 0, r * std::sin (u)}, q = {r * std::cos (v), 0, r * std::sin (v)};
		tri (*t, p, {0, length, 0}, q, i % 2 ? a : b);
		tri (*t, q, {0, -length * 0.35f, 0}, p, b);
	}
	return t;
}

// a soft dark patch under a room's things
void shadow (float radius, int y = -417)
{
	Thing* t = thing ();
	const Paint dark = paint (0x141915, false, 55);
	for (int i = 0; i < 24; i++)
	{
		const float a = TAU * i / 24, b = TAU * (i + 1) / 24;
		tri (*t, {0, (float) y, 0}, {radius * std::cos (a), (float) y, radius * 0.65f * std::sin (a)},
		     {radius * std::cos (b), (float) y, radius * 0.65f * std::sin (b)}, dark);
	}
	t->cull = kit::NONE;
	t->ghost = true;
}

void gradient (unsigned top, unsigned bottom)
{
	for (int y = 0; y < kit::H; y++)
		sky[y] = kit::to565 (colour (top, bottom, (float) y / (kit::H - 1)));
}

void sun (float azimuth, float elevation)
{
	light.azimuth = azimuth;
	light.elevation = elevation;
}

void orbit (float a, float radius, float y, V target = {})
{
	camera.x = target.x + (int) (std::sin (a) * radius);
	camera.y = target.y + (int) y;
	camera.z = target.z + (int) (std::cos (a) * radius);
	camera.look_at (target.x, target.y, target.z);
}

// ---- the rooms -------------------------------------------------------------------------------------

void pressure ()
{
	gradient (0x111B1D, 0x657169);
	light.ambient = {91, 99, 97};
	sun (50, 40);
	const Paint steel = glossy (0xC9AA72, 210, 42), edge = glossy (0xC6D1CB, 210, 42);
	const Paint rim = paint (0xB6C4BC), red = paint (0xC75227);
	shadow (685, -467);
	for (int i = 0; i < 3; i++)
	{
		Thing* o = gear (i == 0 ? 210 : 130, i == 0 ? 10 : 8, steel, edge);
		o->position = {(float) (i == 0 ? 0 : i == 1 ? 270 : -270), (float) (i == 0 ? 235 : -5), (float) (i == 0 ? 0 : 20)};
		item (o, i == 0 ? 1 : -1, i * 31.0f);
	}
	for (int i = 0; i < 8; i++)			// pistons, up and down
	{
		const float a = TAU * i / 8;
		Thing* o = cylinder (32, 100, 8, red);
		o->position = V {std::cos (a) * 620, -215, std::sin (a) * 620}.whole ();
		item (o, 2, (float) i);
	}
	for (int i = 0; i < 3; i++)			// hoops about the gears, one in another
	{
		Thing* o = ring (485.0f + i * 34, 12, 32, 4, i % 2 ? red : rim, {0, 180, 0});
		o->rotation[0] = 65;
		o->rotation[1] = i * 25;
		item (o, 3, (float) i);
	}
	cylinder (680, 36, 40, paint (0x53645B), {0, -450, 0});
	glows.push_back ({-300, 280, 200});
	glows.push_back ({310, -150, -100});
}

void counterweight ()
{
	gradient (0x233349, 0xB3C5C8);
	light.ambient = {119, 126, 143};
	const Paint chalk = paint (0xCDC6B3), dark = paint (0x48586B), ochre = paint (0xD59C34), clay = paint (0xB45436);
	// a nave of slabs that rise and sink, each for itself, the camera between them
	for (int i = 0; i < 18; i++)
		for (int side : {-1, 1})
		{
			const float z = (float) (i * 410);
			item (box ({(float) (side * (700 + (i % 3) * 70)), -90, z}, {180, (float) (780 + (i % 4) * 100), 250}, i % 4 ? chalk : ochre),
			      0, i * 0.49f + side);
			item (box ({(float) (side * 490), 460, z}, {340, 90, 250}, dark), 1, i * 0.49f + side);
		}
	for (int i = 0; i < 10; i++)			// tumbling cubes, below the lintels, beside the aisle
		item (box ({(float) (i % 2 ? 400 : -400), (float) (-100 + i % 2 * 30), (float) (700 + i * 650)}, {200, 200, 200},
			   i % 2 ? clay : ochre), 2, (float) i);
}

void paper_weather ()
{
	gradient (0x849FA0, 0xE0DAC6);
	light.ambient = {145, 135, 110};
	const Paint cream = paint (0xECE0BA), coral = paint (0xCF654B);
	// the sheet, flat: kit_paper lifts it. A vertex carries its triangle's three corners
	landscape.free ();
	auto corner = [] (const V t[3], int which, const Paint& m)
	{
		const int k = landscape.add (t[which].x, 0, t[which].z, 0, 1, 0, m.material, (int) t[2].x, (int) t[2].z);
		kit::Vertex& v = landscape.vertices[k];
		v.nx = (int16_t) t[0].x;
		v.ny = (int16_t) t[0].z;
		v.nz = (int16_t) t[1].x;
		v.pad2 = (int16_t) t[1].z;
	};
	for (int z = 0; z < 15; z++)
		for (int x = 0; x < 24; x++)
		{
			const float xx = (float) ((x - 12) * 100), zz = (float) ((z - 7) * 100);
			const Paint& m = (x + z) % 5 == 0 ? coral : cream;
			const V first[3] = {{xx, 0, zz + 100}, {xx + 100, 0, zz + 100}, {xx + 100, 0, zz}};
			const V second[3] = {{xx + 100, 0, zz}, {xx, 0, zz}, {xx, 0, zz + 100}};
			for (int k = 0; k < 3; k++)
				corner (first, k, m);
			for (int k = 0; k < 3; k++)
				corner (second, k, m);
		}
	landscape.hand_over ();
	birds = thing ();				// twenty-six folded ones: their vertices are worked out each frame
	birds->cull = kit::NONE;
	birds->changes = true;
}

void quicksilver ()
{
	gradient (0x182724, 0x879B8B);
	light.ambient = {96, 117, 111};
	shadow (500);
	blob = sphere (240, 20, chrome ());
	blob->position.y = 160;
	blob->reflects = blob->changes = true;
	for (const kit::Vertex& v : blob->mirror.vertices)
	{
		const V n = V {(float) v.x, (float) v.y, (float) v.z}.unit ();
		blob_directions.push_back (n);
		blob_sines.push_back ({std::sin (n.x * 4), std::sin (n.y * 5), std::sin (n.z * 4)});
		blob_cosines.push_back ({std::cos (n.x * 4), std::cos (n.y * 5), std::cos (n.z * 4)});
	}
	const Paint porcelain = glossy (0xE5D6AB);
	for (int i = 0; i < 2; i++)
	{
		Thing* o = ring (415.0f + i * 75, 16, 32, 5, porcelain, {0, 160, 0});
		o->rotation[0] = i * 56 + 25;
		o->rotation[1] = i * 43;
		item (o, 0, (float) i);
	}
	for (int i = 0; i < 3; i++)
	{
		Thing* o = sphere (42 + i % 2 * 5, 10, chrome ());
		o->reflects = true;
		item (o, 1, (float) i);
	}
	for (int i = 0; i < 7; i++)
		box ({(float) ((i - 3) * 205), 110, -720}, {110, (float) (1060 - (i % 2) * 200), 180}, paint (i % 2 ? 0x65796B : 0xB2B49A));
	cylinder (390, 35, 36, paint (0xC5BA97), {0, -398, 0});
}

void colour_space ()
{
	gradient (0xDCB7A1, 0xF1D6B3);
	light.ambient = {142, 129, 105};
	const Paint blue = glossy (0x174994), red = paint (0xDA4934), yellow = paint (0xEBC339);
	const Paint black = paint (0x282C36), ivory = paint (0xF4EACD);
	shadow (500);
	for (int i = 0; i < 3; i++)
	{
		Thing* o = ring (170, 45, 32, 6, i == 0 ? blue : i == 1 ? yellow : red, {(float) (-420 + i * 420), (float) (i == 1 ? 240 : -30), -100});
		o->rotation[0] = 90;
		o->rotation[2] = i * 40;
		item (o, 0, (float) i);
	}
	for (int i = 0; i < 15; i++)
		item (box ({(float) ((i - 7) * 77), -250, 310}, {51, 290, 65}, i % 3 == 0 ? blue : i % 3 == 1 ? red : yellow), 1, (float) i);
	for (int i = 0; i < 5; i++)
		item (sphere (65, 12, ivory), 2, (float) i);
	box ({-650, -200, 200}, {60, 430, 90}, black);
	box ({650, -200, 200}, {60, 430, 90}, black);
	for (int i = 0; i < 4; i++)
		item (box ({0, 0, 0}, {540, 20, 30}, black), 3, (float) i);
}

void botanica ()
{
	gradient (0x243831, 0xABB28A);
	light.ambient = {110, 123, 90};
	const Paint green = glossy (0x577A4C), copper = paint (0xB77C3C), ivory = paint (0xDACD9C);
	shadow (460, -487);
	for (int i = 0; i < 72; i++)
	{
		Thing* o = thing ();
		const float size = 53.0f + (i % 5) * 5;
		tri (*o, {0, -size, 0}, {-size * 0.46f, 0, 0}, {0, size * 1.2f, 14}, i % 5 == 0 ? copper : green);
		tri (*o, {0, -size, 0}, {0, size * 1.2f, 14}, {size * 0.46f, 0, 0}, i % 3 == 0 ? ivory : green);
		tri (*o, {size * 0.46f, 0, 0}, {0, size * 1.2f, 14}, {-size * 0.46f, 0, 0}, green);
		o->cull = kit::NONE;
		item (o, 0, (float) i);
	}
	item (sphere (105, 18, glossy (0xD2A348)), 1);
	cylinder (190, 48, 28, paint (0xB0A184), {0, -450, 0});
	glows.assign (16, V ());
}

void interference ()
{
	gradient (0x22252B, 0x807A71);
	light.ambient = {150, 145, 126};
	const Paint ivory = paint (0xE3DFCA), ink = paint (0x161E25), red = paint (0xD4452F);
	// three ribbons: strips of three samples, in turn ink and a colour; kit_ribbon winds them
	constexpr int samples = 72;
	for (int r = 0; r < 3; r++)
	{
		kit::Mesh& mesh = ribbon_mesh[r];
		mesh.free ();
		for (int i = 0; i < samples; i++)
			for (int j = 0; j < 2; j++)
			{
				const kit::Material& m = ((i / 3) % 2 ? (r == 1 ? red : ivory) : ink).material;
				const int k = mesh.add ((float) i, (float) (j - 1), 0, 0, 0, 1, m);
				mesh.add ((float) (i + 1), (float) (j - 1), 0, 0, 0, 1, m);
				mesh.add ((float) (i + 1), (float) j, 0, 0, 0, 1, m);
				mesh.add ((float) i, (float) j, 0, 0, 0, 1, m);
				for (int n : {0, 1, 2, 2, 3, 0})
					mesh.indices.push_back ((uint16_t) (k + n));
			}
		mesh.hand_over ();
	}
	ribbons = 3;
	for (int i = 0; i < 9; i++)
		item (sphere (55, 10, i % 3 ? ivory : red), 0, (float) i);
}

void reliquary ()
{
	gradient (0x263747, 0xA1B7B6);
	light.ambient = {95, 118, 130};
	const Paint silver = chrome (), blue = paint (0x7AABC0), gold = paint (0xBE953E);
	for (int i = 0; i < 40; i++)
		item (crystal (45.0f + (i % 4) * 12, 135 + randf (i * 7) * 160, 5, i % 4 ? blue : gold,
			       i % 4 ? paint (colour (0x283A52, 0xD3E6D4, randf (i * 17))) : silver), 0, (float) i);
	Thing* core = crystal (205, 310, 8, silver, gold);
	core->reflects = true;
	item (core, 1);
	for (int i = 0; i < 3; i++)
		item (ring (360.0f + i * 70, 7, 40, 4, gold), 2, (float) i);
	glows.push_back ({-290, 100, 130});
	glows.push_back ({270, -120, -180});
}

void gyre ()
{
	gradient (0x2B333A, 0x948572);
	// the tunnel is not lit; a low fill and a broad key from the side shape the knot
	light.ambient = {48, 43, 38};
	sun (235, 32);
	light.intensity = 255;
	const Paint clay = paint (0xB56848, false), blue = paint (0x507F8A, false), cream = paint (0xCDBB95, false), dark = paint (0x26363B, false);
	for (int i = 0; i < 12; i++)
	{
		Thing* o = thing ();
		o->cull = kit::NONE;
		const Paint& m = i % 3 == 0 ? clay : i % 3 == 1 ? blue : cream;
		auto point = [] (float a, float radius, float z)
		{
			const float r = radius * (1 + 0.1f * std::cos (a * 4));
			return V {std::cos (a) * r, std::sin (a) * r, z};
		};
		for (int j = 0; j < 16; j++)
		{
			const float a = TAU * j / 16, b = TAU * (j + 1) / 16;
			quad (*o, point (a, 500, 0), point (b, 500, 0), point (b, 340, 0), point (a, 340, 0), m);
			quad (*o, point (a, 340, 0), point (b, 340, 0), point (b, 322, 40), point (a, 322, 40), dark);
		}
		o->position.z = (float) (i * 220);
		item (o, 0, (float) i);
	}
	item (knot (125, 32, 40, 6, glossy (0xF4D28B, 235, 20, 225)), 1);
}

void assembly ()
{
	gradient (0x25342F, 0x8B9781);
	light.ambient = {100, 107, 93};
	sun (60, 35);
	light.intensity = 235;
	const Paint porcelain = glossy (0xE6DBC1), orange = paint (0xC26635), blue = paint (0x456A85);
	shadow (500, -457);
	item (knot (155, 45, 48, 6, porcelain), 0);
	for (int i = 0; i < 12; i++)
	{
		Thing* o;
		if (i % 4 == 0)
		{
			o = crystal (47, 95, 5, chrome (), orange);
			o->reflects = true;
		}
		else if (i % 4 == 1)
			o = ring (58, 18, 12, 4, blue);
		else if (i % 4 == 2)
			o = box ({0, 0, 0}, {100, 100, 100}, orange);
		else
			o = sphere (48, 10, porcelain);
		item (o, 1, (float) i);
	}
	for (int i = 0; i < 3; i++)
		item (ring (340.0f + i * 45, 11, 32, 4, porcelain), 2, (float) i);
}

void load (int next)
{
	glows.clear ();
	items.clear ();
	things.clear ();
	blob = birds = nullptr;
	blob_directions.clear ();
	blob_sines.clear ();
	blob_cosines.clear ();
	ribbons = 0;
	chapter = next;
	camera.fov = 62;
	camera.near_plane = 40;
	camera.far_plane = 12000;
	sun (225, 40);
	light.color = {255, 240, 214};
	light.intensity = 225;
	switch (chapter)
	{
	case 0: pressure (); break;
	case 1: counterweight (); break;
	case 2: paper_weather (); break;
	case 3: quicksilver (); break;
	case 4: colour_space (); break;
	case 5: botanica (); break;
	case 6: interference (); break;
	case 7: reliquary (); break;
	case 8: gyre (); break;
	default: assembly (); break;
	}
	// what won't change its shape goes to the RPi for good
	unsigned triangles = 0;
	for (auto& t : things)
	{
		for (const kit::Mesh* m : {&t->mesh, &t->mirror})
			triangles += (unsigned) (m->indices.empty () ? m->vertices.size () : m->indices.size ()) / 3;
		if (t->changes)
			continue;
		t->mesh.hand_over ();
		if (!t->reflects)
			t->mirror.hand_over ();
	}
	std::printf ("MATTER %02d %s / %u things / %u triangles\n", chapter + 1, NAMES[chapter], (unsigned) things.size (), triangles);
}

// ---- a room's motion -------------------------------------------------------------------------------

void set_rotation (Thing* t, float x, float y, float z)	// (a mesh turns by whole degrees)
{
	t->rotation[0] = (int) x;
	t->rotation[1] = (int) y;
	t->rotation[2] = (int) z;
}

// the drop: a ball whose surface swells and sinks, its normals with it
void pose_blob (float t)
{
	const V st = {std::sin (t * 1.1f), std::sin (-t * 0.8f), std::sin (t * 0.9f)};
	const V ct = {std::cos (t * 1.1f), std::cos (-t * 0.8f), std::cos (t * 0.9f)};
	for (size_t i = 0; i < blob_directions.size (); i++)
	{
		const V n = blob_directions[i], s = blob_sines[i], c = blob_cosines[i];
		const float sx = s.x * ct.x + c.x * st.x, sy = s.y * ct.y + c.y * st.y, sz = s.z * ct.z + c.z * st.z;
		const float cx = c.x * ct.x - s.x * st.x, cy = c.y * ct.y - s.y * st.y, cz = c.z * ct.z - s.z * st.z;
		const float r = 240 + 35 * sx * sy * sz;
		const V grad = {140 * cx * sy * sz, 175 * sx * cy * sz, 140 * sx * sy * cz};
		kit::Vertex& v = blob->mirror.vertices[i];
		const V p = (n * r).whole ();
		v.x = (int16_t) p.x;
		v.y = (int16_t) p.y;
		v.z = (int16_t) p.z;
		set_normal (v, n * r - grad + n * grad.dot (n));
	}
}

// the birds: two triangles each, their wing tips beating, about the landscape's middle
void pose_birds (float t)
{
	const Paint cream = paint (0xECE0BA), coral = paint (0xCF654B), teal = paint (0x487C79);
	birds->mesh.clear ();
	for (int i = 0; i < 26; i++)
	{
		const float a = t * 0.85f + i * 2.39996f, rad = 180.0f + i * 20;
		const V at = {(float) (int) (std::sin (a) * rad), (float) (int) (40 + 140 * std::sin (t * 0.8f + i)), (float) (int) (std::cos (a) * rad)};
		const int turn[3] = {12, (int) (-a * 180 / PI), (int) (20 * std::sin (t * 2.6f + i))};
		const float flap = (float) (int) (40 * std::sin (t * 3.7f + i));
		auto place = [&] (V v) { return turned (v, turn).whole () + at; };
		tri (*birds, place ({-72, flap, 0}), place ({0, 14, 30}), place ({0, 0, -52}), i % 3 ? cream : teal);
		tri (*birds, place ({0, 0, -52}), place ({0, 14, 30}), place ({72, flap, 0}), i % 4 ? coral : cream);
	}
}

// chrome: where the room's picture is seen in each vertex, from the eye (Jet's environmentReflectionUV)
void reflect (Thing* t)
{
	for (kit::Vertex& v : t->mirror.vertices)
	{
		const V p = (turned ({(float) v.x, (float) v.y, (float) v.z}, t->rotation) + t->position).whole ();
		const V n = turned ({(float) v.nx, (float) v.ny, (float) v.nz}, t->rotation).unit ();
		const V eye = (V {camera.x, camera.y, camera.z} - p).unit ();
		const float twice = 2 * n.dot (eye);
		const V r = n * twice - eye;
		v.u = (int16_t) std::lround ((0.5f + std::atan2 (r.x, r.z) / TAU) * 1024);
		v.v = (int16_t) std::min (std::max ((int) std::lround ((0.5f - std::asin (std::min (std::max (r.y, -1.0f), 1.0f)) / PI) * 1024), 0), 1023);
	}
}

void pose (float t)
{
	const float p = t / CHAPTER_SECONDS;
	switch (chapter)
	{
	case 0:
		orbit (0.12f + t * 0.065f, 1460 - 130 * std::sin (t * 0.18f), 250 + 90 * std::sin (t * 0.23f), {0, 150, 0});
		for (Thing* i : items)
		{
			if (i->kind == 1 || i->kind == -1)
			{
				i->rotation[2] = (int) (t * 48 * i->kind + i->phase);
			}
			else if (i->kind == 2)
			{
				// (the original gives them a length too, which Jet doesn't apply: they only rise and sink)
				const int scale = (int) (2800 + 1700 * std::sin (t * 1.8f + i->phase));
				i->position.y = (float) (-430 + scale * 50 / 1024);
			}
			else
			{
				i->rotation[0] = (int) (65 + i->phase * 8);
				i->rotation[2] = (int) (t * 42 * ((int) i->phase % 2 ? -1 : 1));
			}
		}
		break;
	case 1:
	{
		const float travel = 900 + t * 260;
		camera.x = (float) (int) (140 * std::sin (t * 0.2f));
		camera.y = (float) (50 + (int) (150 * p));
		camera.z = (float) (int) travel;
		camera.look_at ((float) (int) (-80 * std::sin (t * 0.3f)), 180, (float) (int) (travel + 1400));
		for (Thing* i : items)
		{
			if (i->kind < 2)
			{
				i->position.y = i->base.y + (int) (160 * std::sin (t * 1.2f + i->phase));
			}
			else
			{
				set_rotation (i, t * 42 + i->phase * 17, t * 53 + i->phase * 29, t * 37);
				i->position.y = i->base.y + (int) (60 * std::sin (t * 1.5f + i->phase));
			}
		}
		break;
	}
	case 2:
		orbit (-0.9f + t * 0.06f, 1220, 590 - 150 * p, {0, -90, 0});
		pose_birds (t);
		break;
	case 3:
		orbit (-0.55f + t * 0.075f, 1370 + 80 * std::sin (t * 0.32f), 185 + 100 * std::sin (t * 0.27f), {0, 100, 0});
		pose_blob (t);
		set_rotation (blob, t * 38, t * 47, 0);
		for (Thing* i : items)
		{
			if (i->kind == 0)
			{
				set_rotation (i, 25 + i->phase * 56 + t * 41, i->phase * 43 - t * 47, t * 37);
			}
			else
			{
				const float a = t * 0.75f + i->phase * TAU / 3;	// beads between the drop and the inner hoop
				i->position = {(float) (int) (330 * std::cos (a)), (float) (int) (160 + 65 * std::sin (a * 1.7f)), (float) (int) (330 * std::sin (a))};
			}
		}
		break;
	case 4:
		orbit (-0.5f + t * 0.075f, 1470, 430 - 150 * p, {0, 25, 0});
		for (Thing* i : items)
		{
			if (i->kind == 0)
			{
				set_rotation (i, 90, t * 47 + i->phase * 35, i->phase * 35);
				i->position.y = i->base.y + (int) (35 * std::sin (t * 1.3f + i->phase));
			}
			else if (i->kind == 1)
			{
				const int scale = (int) (1024 + 600 * std::sin (t * 1.6f + i->phase * 0.6f));
				i->position.y = (float) (-400 + scale * 145 / 1024);
			}
			else if (i->kind == 2)
			{
				const float a = t * 0.75f + i->phase * TAU / 5;	// pendulums above the slats
				i->position = {(float) (int) (-400 + 200 * i->phase), (float) (int) (240 + 50 * std::sin (a)), (float) (int) (300 + 25 * std::cos (a))};
			}
			else
			{
				i->position = {0, (float) (int) (250 + i->phase * 50), -670};	// bars sweeping behind the rings
				set_rotation (i, 0, t * 46 + i->phase * 40, 0);
			}
		}
		break;
	case 5:
		orbit (-0.4f + t * 0.11f, 1030 + 100 * std::sin (t * 0.3f), 110 + 120 * std::sin (t * 0.23f), {0, -50, 0});
		for (Thing* i : items)
		{
			if (i->kind == 0)
			{
				const float u = i->phase / 72, a = i->phase * 2.39996f + t * 0.8f;
				const float r = 55 + 260 * std::sin (PI * u) + 45 * std::sin (t * 0.7f + i->phase * 0.2f);
				i->position = {(float) (int) (r * std::cos (a)), (float) (int) (-390 + 700 * u + 30 * std::sin (t + i->phase * 0.25f)),
					       (float) (int) (r * std::sin (a))};
				set_rotation (i, 25 + 12 * std::sin (t * 3.2f + i->phase * 0.2f), -a * 180 / PI, 20 * std::sin (i->phase * 2.39996f));
			}
			else
			{
				i->position.y = (float) (int) (-20 + 35 * std::sin (t));
				set_rotation (i, t * 38, t * 51, 0);
			}
		}
		for (size_t i = 0; i < glows.size (); i++)		// lights rising about the plant
		{
			const float a = t * 0.5f + i * 2.39996f;
			glows[i] = {std::cos (a) * (210 + i * 12), -190 + std::fmod (t * 65 + i * 51, 600.0f), std::sin (a) * (210 + i * 12)};
		}
		break;
	case 6:
		camera.x = (float) (int) (85 * std::sin (t * 0.29f));
		camera.y = (float) (int) (65 * std::cos (t * 0.24f));
		camera.z = (float) (-2420 + (int) (200 * p));
		camera.look_at (0, 0, 400);
		for (Thing* i : items)
		{
			const float a = t * 0.65f + i->phase;
			i->position = {(float) (int) (110 * std::sin (a)), (float) (int) (100 * std::cos (a)), (float) (int) (-1250 + i->phase * 300)};
		}
		break;
	case 7:
		orbit (0.1f + t * 0.11f, 1100, 420 + 100 * std::sin (t * 0.27f), {0, -60, 0});
		for (Thing* i : items)
		{
			if (i->kind == 0)
			{
				const float a = i->phase * 2.39996f, r = 240 + 290 * std::sqrt (i->phase / 40.0f);
				i->position = {(float) (int) (r * std::cos (a)), (float) (-350 + (int) (90 * std::sin (t * 1.2f + i->phase))), (float) (int) (r * std::sin (a))};
				set_rotation (i, 18 * std::sin (a), i->phase * 35 + t * 42, 20 * std::cos (a));
			}
			else if (i->kind == 1)
			{
				set_rotation (i, 17, t * 44, 12);
				i->position.y = (float) (int) (60 + 50 * std::sin (t));
			}
			else
			{
				set_rotation (i, i->phase * 50 + t * 34, t * 43, i->phase * 28);
			}
		}
		break;
	case 8:
		camera.fov = 76;
		camera.x = (float) (int) (55 * std::sin (t * 0.5f));
		camera.y = (float) (int) (45 * std::cos (t * 0.4f));
		camera.z = -520;
		camera.look_at (0, 0, 2500);
		for (Thing* i : items)
		{
			if (i->kind == 0)
			{
				float z = std::fmod (i->phase * 260 - t * 360 + 3120, 3120.0f);
				if (z < 0)
					z += 3120;
				i->position = {(float) (int) (40 * std::sin (z * 0.0025f + t * 0.5f)), (float) (int) (40 * std::cos (z * 0.0025f + t * 0.5f)),
					       (float) ((int) z - 300)};
				i->rotation[2] = (int) (i->phase * 13 + t * 42 + 8 * std::sin (t * 1.3f + i->phase));
			}
			else
			{
				i->position.z = 500;			// the knot, in the tunnel's bore
				set_rotation (i, t * 38, t * 51, 0);
			}
		}
		break;
	default:
		orbit (-0.4f + t * 0.055f, 1100 + 12 * t, 250 + 8 * t, {0, -10, 0});
		for (Thing* i : items)
		{
			if (i->kind == 0)
			{
				set_rotation (i, t * 38, t * 47, 10);
			}
			else if (i->kind == 1)
			{
				const float a = t * 0.36f + i->phase * TAU / 12, r = 400 + 75 * std::sin (t * 0.45f + i->phase);
				i->position = {(float) (int) (r * std::cos (a)), (float) (int) (160 * std::sin (a * 2 + t * 0.25f)), (float) (int) (r * std::sin (a))};
				set_rotation (i, t * 46 + i->phase * 21, t * 52 + i->phase * 30, t * 39);
			}
			else
			{
				set_rotation (i, i->phase * 50 + t * 34, i->phase * 40 - t * 41, t * 37);
			}
		}
		break;
	}
}

void seek (float absolute)
{
	seconds_in = std::min (std::max (absolute, 0.0f), DURATION);
	const int next = std::min (CHAPTERS - 1, (int) (seconds_in / CHAPTER_SECONDS));
	local = seconds_in - next * CHAPTER_SECONDS;
	if (next != chapter)
		load (next);
	pose (local);
}

void update (float seconds)
{
	const float next = seconds_in + seconds;
	seek (next >= DURATION + 1 ? 0 : next);		// (the original restarts the board here)
	if (next > DURATION && next < DURATION + 1)
		seconds_in = next;
}

void init ()
{
	light.on = light.has_ambient = true;
	// the room that chrome mirrors, the lights' halo: small pictures, made once
	std::vector<uint16_t> pixels (128 * 64);
	for (int y = 0; y < 64; y++)
		for (int x = 0; x < 128; x++)
		{
			const float v = y / 64.0f;
			unsigned c = colour (0x22312F, 0xDED8C3, 0.5f + 0.5f * std::cos (v * PI));
			if ((x > 13 && x < 28) || (x > 73 && x < 93))
				c = colour (0xBACCCC, 0xFFFCDF, clamp01 (1 - std::fabs (v - 0.32f) * 2));
			if (x > 34 && x < 47)
				c = colour (0x713D24, 0xDC7E36, 1 - v);
			if (y > 36 && y < 41)
				c = 0x151B22;
			if ((x % 32) < 3)
				c = 0x3B4543;
			pixels[y * 128 + x] = kit::to565 (c);
		}
	room_texture = kit::texture565 (128, 64, pixels.data (), false, true);
	for (int y = 0; y < 32; y++)
		for (int x = 0; x < 32; x++)
		{
			const float dx = (x - 15.5f) / 15.5f, dy = (y - 15.5f) / 15.5f;
			float v = std::max (0.0f, 1 - std::sqrt (dx * dx + dy * dy));
			v = v * v * v;
			pixels[y * 32 + x] = kit::to565 (((unsigned) (255 * v) << 16) | ((unsigned) (207 * v) << 8) | (unsigned) (135 * v));
		}
	const kit::Image halo = {32, 32, pixels.data (), true, 0};
	halo_texture = kit::texture (halo);
	static const uint8_t* const masks[matter_type::count] = {matter_type::mask0, matter_type::mask1, matter_type::mask2, matter_type::mask3,
		matter_type::mask4, matter_type::mask5, matter_type::mask6, matter_type::mask7, matter_type::mask8, matter_type::mask9,
		matter_type::mask10, matter_type::mask11};
	for (int i = 0; i < matter_type::count; i++)
		type_texture[i] = kit::mask (matter_type::widths[i], matter_type::heights[i], masks[i]);
	paper_program = kit::program (&kit_paper_info, sizeof kit_paper_info);
	paper_wave = glGetUniformLocation (paper_program.name, "u_wave");
	ribbon_program = kit::program (&kit_ribbon_info, sizeof kit_ribbon_info);
	ribbon_wave = glGetUniformLocation (ribbon_program.name, "u_wave");
	ribbon_wave2 = glGetUniformLocation (ribbon_program.name, "u_wave2");
	seek (0);
}

void draw_thing (Thing* t)
{
	kit::Draw how;
	how.model = kit::translation (t->position.x, t->position.y, t->position.z)
		    * kit::rotation ((float) t->rotation[0], (float) t->rotation[1], (float) t->rotation[2]);
	how.cull = t->cull;
	if (t->ghost)
	{
		how.blend = kit::ALPHA;
		how.depth_write = false;
	}
	if (t->changes && !t->mesh.vertices.empty ())
		t->mesh.upload (GL_DYNAMIC_DRAW);
	if (t->mesh.uploaded)
	{
		how.shading = t->gloss ? kit::PHONG : kit::VERTEX;
		kit::draw (t->mesh, how);
	}
	if (t->reflects)
	{
		reflect (t);
		t->mirror.upload (GL_DYNAMIC_DRAW);
	}
	if (t->mirror.uploaded)
	{
		how.shading = kit::VERTEX;
		how.texture = room_texture;
		how.affine = true;
		kit::draw (t->mirror, how);
	}
}

void letters (int which, int x, int y, int alpha)
{
	// (the two rooms with a light ground have theirs dark)
	kit::sprite_mask (type_texture[which], matter_type::widths[which], matter_type::heights[which], x, y,
			  kit::to565 (which == 3 || which == 5 ? 0x142820 : 0xEFE6D3), std::min (std::max (alpha, 0), 255));
}

void draw ()
{
	kit::background (sky);
	kit::begin (camera, light);
	if (chapter == 2)
	{
		kit::use (paper_program);
		glUniform4f (paper_wave, local, 0, 0, 0);
		kit::Draw sheet;
		sheet.program = &paper_program;
		sheet.cull = kit::NONE;
		kit::draw (landscape, sheet);
	}
	for (int r = 0; r < ribbons; r++)
	{
		kit::use (ribbon_program);
		glUniform4f (ribbon_wave, local, r * TAU / 3, 3000, 65);
		glUniform4f (ribbon_wave2, 72, 0, 0, 0);
		kit::Draw band;
		band.program = &ribbon_program;
		band.cull = kit::NONE;
		kit::draw (ribbon_mesh[r], band);
	}
	for (auto& t : things)
		if (!t->ghost)
			draw_thing (t.get ());
	for (auto& t : things)
		if (t->ghost)
			draw_thing (t.get ());

	const float f = (kit::W / 2) / std::tan (camera.fov * PI / 360);
	for (const V& g : glows)				// lights: a halo each, larger when near
	{
		float x, y, depth;
		const V p = g.whole ();
		if (!camera.project (p.x, p.y, p.z, &x, &y, &depth) || depth <= 120)
			continue;
		const int scale = std::min (std::max ((int) (90 * f / (depth * 32)), 1), 3);
		kit::sprite (halo_texture, 32, 32, kit::W / 2 + (int) (x - kit::W / 2) - 16 * scale, kit::H / 2 - (int) (kit::H / 2 - y) - 16 * scale,
			     255, true, scale);
	}
	if (chapter == 0)
		letters (0, 28, 34, (int) (255 * std::min (clamp01 (local - 1.5f), 1 - clamp01 ((local - 6) / 1.5f))));
	else if (chapter == 9)
		letters (11, 26, 201, (int) (255 * clamp01 ((local - 6) / 1.5f)));
	else
		letters (chapter + 1, 22, 278, (int) (230 * std::min (clamp01 ((local - 0.65f) / 0.6f), 1 - clamp01 ((local - 3.5f) / 0.8f))));
	// a short dark between the rooms
	float dark = chapter == 0 ? 1 - clamp01 (local / 2.2f) : 1 - clamp01 (local / 0.55f);
	dark = std::max (dark, chapter == 9 ? clamp01 ((local - 15) / 3) : clamp01 ((local - 17.5f) / 0.5f));
	if ((int) (255 * dark) > 0)
		kit::rect (0, 0, kit::W, kit::H, 0, (int) (255 * dark));
}

}  // namespace

int main ()
{
	return kit::run ({"Matter", init, update, draw});
}
