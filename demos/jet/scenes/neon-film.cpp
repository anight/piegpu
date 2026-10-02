// neon-film - JetExamples' "ESP 88" (esp32-neon-film, MIT, CubeCoders) for
// piegpu's OpenGL: a short film in twelve cuts, 116 seconds: a city over a
// river, a street in the rain, a coupe followed through it, from behind,
// from its seat; a junction taken sideways, a pursuit on the boulevard, the
// wheels folding down, the car leaving the street, the city from above, the
// credits.
//
// The city is built as the original builds it, block by block, and stays on
// the RPi: a block's shop fronts, windows, signs and awnings are one mesh
// with one texture (the film's pictures, put together into an atlas), drawn
// where the street's recycling puts it. Neon tubes are quads their vertex
// shader turns to the camera (kit_neon); the river mirrors the picture
// (kit_water, as the island's sea); a wet street mirrors the fronts (drawn
// again, upside down, under a floor that lets them through). The cars are
// parts placed by their matrices; rain and spray are a few hundred
// triangles sent each frame.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>
#include "kit.hpp"
#include "../assets/film.hpp"
#include "../assets/film_credits.hpp"
#include "kit_neon_program.h"
#include "kit_water_program.h"

namespace {

constexpr float PI = 3.14159265359f;
constexpr float DURATIONS[] = {12, 10, 7, 9, 9, 6, 11, 14, 10, 5, 8, 15};
constexpr float DURATION = 116;
constexpr int CUTS = 12;
constexpr float CREDITS_START = 2, CREDITS_FADE_IN = 2, CREDITS_HOLD = 8, CREDITS_FADE_OUT = 3;
const char* const NAMES[CUTS] = {"THE RIVER", "RAIN DISTRICT", "THE COURIER", "REAR VIEW", "NO DRIVER", "EIGHTY EIGHT",
				 "BOULEVARD", "PURSUIT", "FLIGHT MODE", "IGNITION", "ABOVE IT ALL", "ESP 88"};

// ---- numbers: the original's are whole units -------------------------------------------------------

struct P
{
	int x = 0, y = 0, z = 0;
	P operator+ (P b) const		{ return {x + b.x, y + b.y, z + b.z}; }
	P operator- (P b) const		{ return {x - b.x, y - b.y, z - b.z}; }
	P operator* (int k) const	{ return {x * k, y * k, z * k}; }
	P divide (int k) const		{ return {x / k, y / k, z / k}; }
};

float clamp01 (float t)		{ return std::min (std::max (t, 0.0f), 1.0f); }

P lerp (P a, P b, float t)
{
	return {(int) (a.x + (b.x - a.x) * t), (int) (a.y + (b.y - a.y) * t), (int) (a.z + (b.z - a.z) * t)};
}

P yawed (P p, float degrees)
{
	const float a = degrees * PI / 180;
	return {(int) (p.x * std::cos (a) + p.z * std::sin (a)), p.y, (int) (-p.x * std::sin (a) + p.z * std::cos (a))};
}

// the film's colours are graded once, as they are written down: more contrast
int contrast (int value)
{
	return std::min (std::max ((value * 5 - 80 + 2) / 4, 0), 255);
}

uint16_t rgb (unsigned c)
{
	const int r = contrast ((c >> 16) & 255), g = contrast ((c >> 8) & 255), b = contrast (c & 255);
	return (uint16_t) ((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
}

uint32_t weather_hash (uint32_t v)
{
	v ^= v >> 16;
	v *= 0x7feb352du;
	v ^= v >> 15;
	v *= 0x846ca68bu;
	return v ^ (v >> 16);
}

// ---- the pictures: one texture of them all ---------------------------------------------------------

enum Tile { PLAIN = -1, FACADE0, FACADE1, FACADE2, FACADE3, SHOP0, SHOP1, SHOP2, SHOP3, SIGN0, SIGN1, SIGN2, SIGN3, SIGN4, SIGN5,
	    FAR0, FAR1, FAR2, FAR3, DASH, SPEED, TILES };
constexpr int ATLAS = 512;
const struct { int x, y, w, h; } TILE[TILES] =
{
	{0, 0, 64, 128}, {64, 0, 64, 128}, {128, 0, 64, 128}, {192, 0, 64, 128},
	{0, 128, 128, 64}, {128, 128, 128, 64}, {256, 128, 128, 64}, {384, 128, 128, 64},
	{0, 192, 128, 64}, {128, 192, 128, 64}, {256, 192, 128, 64}, {384, 192, 128, 64}, {0, 256, 128, 64}, {128, 256, 128, 64},
	{384, 256, 32, 32}, {416, 256, 32, 32}, {448, 256, 32, 32}, {480, 256, 32, 32},
	{256, 0, 256, 128}, {384, 288, 128, 64},
};
GLuint atlas, env_texture, holo_texture, glow_texture, credit_texture;

void tile_pixels (int tile, const uint16_t* pixels)
{
	glBindTexture (GL_TEXTURE_2D, atlas);
	glPixelStorei (GL_UNPACK_ALIGNMENT, 2);
	glTexSubImage2D (GL_TEXTURE_2D, 0, TILE[tile].x, TILE[tile].y, TILE[tile].w, TILE[tile].h, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, pixels);
}

void tile_indexed (int tile, const uint8_t* indices, const uint16_t* palette)
{
	std::vector<uint16_t> pixels ((size_t) TILE[tile].w * TILE[tile].h);
	for (size_t i = 0; i < pixels.size (); i++)
		pixels[i] = palette[indices[i]];
	tile_pixels (tile, pixels.data ());
}

void make_textures ()
{
	glGenTextures (1, &atlas);
	glBindTexture (GL_TEXTURE_2D, atlas);
	glTexImage2D (GL_TEXTURE_2D, 0, GL_RGB, ATLAS, ATLAS, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, nullptr);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	const uint8_t* const facades[4] = {film::facade0, film::facade1, film::facade2, film::facade3};
	const uint16_t* const facade_palettes[4] = {film::facade0Palette, film::facade1Palette, film::facade2Palette, film::facade3Palette};
	const uint8_t* const shops[4] = {film::shop0, film::shop1, film::shop2, film::shop3};
	const uint16_t* const shop_palettes[4] = {film::shop0Palette, film::shop1Palette, film::shop2Palette, film::shop3Palette};
	const uint8_t* const signs[6] = {film::sign0, film::sign1, film::sign2, film::sign3, film::sign4, film::sign5};
	const uint16_t* const sign_palettes[6] = {film::sign0Palette, film::sign1Palette, film::sign2Palette, film::sign3Palette,
						  film::sign4Palette, film::sign5Palette};
	for (int k = 0; k < 4; k++)
	{
		tile_indexed (FACADE0 + k, facades[k], facade_palettes[k]);
		tile_indexed (SHOP0 + k, shops[k], shop_palettes[k]);
		// a facade far off: an eighth of its texels, each the mean of eight
		uint16_t small[32 * 32];
		for (int y = 0; y < 32; y++)
			for (int x = 0; x < 32; x++)
			{
				int r = 0, g = 0, b = 0;
				for (int dy = 0; dy < 4; dy++)
					for (int dx = 0; dx < 2; dx++)
					{
						const uint16_t c = facade_palettes[k][facades[k][(y * 4 + dy) * 64 + x * 2 + dx]];
						r += (c >> 11) & 31;
						g += (c >> 5) & 63;
						b += c & 31;
					}
				small[y * 32 + x] = (uint16_t) ((r / 8) << 11 | (g / 8) << 5 | b / 8);
			}
		tile_pixels (FAR0 + k, small);
	}
	for (int k = 0; k < 6; k++)
		tile_indexed (SIGN0 + k, signs[k], sign_palettes[k]);
	tile_indexed (DASH, film::dashboard, film::dashboardPalette);
	env_texture = kit::texture_indexed (128, 64, film::environment, film::environmentPalette, 5, 0, true);
	std::vector<uint16_t> pixels (64 * 96);
	for (size_t i = 0; i < pixels.size (); i++)
		pixels[i] = film::hologramPalette[film::hologram[i]];
	const kit::Image holo = {64, 96, pixels.data (), true, 0}, glow = {16, 16, film::glow, true, 0};
	holo_texture = kit::texture (holo);
	glow_texture = kit::texture (glow);
	credit_texture = kit::mask (360, 168, film_credits::creditMask);
}

// ---- materials, and the things made of them --------------------------------------------------------

struct Mat
{
	uint16_t color = 0xFFFF;
	uint8_t alpha = 255;
	int tile = PLAIN;
	bool additive = false;
	uint8_t diffuse = 255, specular = 0, gloss = 0;	// gloss: lit, per pixel (the hero's paint)
	int sign = -1;					// a sign's picture: it flickers
	bool water = false;
};

Mat paint (unsigned c, int alpha = 255, bool additive = false)
{
	Mat m;
	m.color = rgb (c);
	m.alpha = (uint8_t) alpha;
	m.additive = additive;
	return m;
}

Mat textured (int tile, int alpha = 255)
{
	Mat m;
	m.alpha = (uint8_t) alpha;
	m.tile = tile;
	return m;
}

struct Range { int first = 0, count = 0; };

// Which of a cut's vertex buffers a thing's faces are in. Things that are drawn alike are together there,
// one after another as they were made: neighbours that stand alike go out as one draw (a draw is what
// costs the host; the RPi has names for 250 buffers, and a street has more things than that)
enum Class
{
	GROUND, GROUND_SHEER,			// painted first, in the order made: floors; what is let through, on them
	SOLID,					// what is always drawn as it is
	CLOSED,					// ... and is closed: boxes, towers (their far sides are not drawn)
	DETAIL, FAR,				// a block's front: from near; its simpler lot, from far off
	MIRRORED,				// solid, and seen again in the wet street
	BILLBOARD,				// turned to the camera
	SHEER,					// blended or added
	GLOSSY,					// lit per pixel (the hero's paint)
	TUBES,					// neon tubes (kit_neon)
	OWN,					// its vertices change: kept here, a buffer of its own
	CLASSES
};
const int CAPACITY[CLASSES] = {3072, 2048, 12288, 8192, 12288, 4096, 4096, 2048, 2048, 4096, 2048, 0};
kit::Mesh arena[CLASSES];
int arena_used[CLASSES];

struct Parts					// a thing while it's built
{
	kit::Mesh mesh, low, tubes;		// its faces; a simpler lot for far off; its neon tubes
	std::vector<int8_t> tiles;		// each vertex's picture
};

struct Thing
{
	Parts* parts;
	kit::Mesh &mesh, &low, &tubes;		// (the parts': gone once it's settled, unless its vertices change)
	std::vector<int8_t>& tiles;
	Range near, far, tube;			// where they are in the cut's buffers
	uint8_t cls = SOLID;
	P position;
	int rotation[3] = {0, 0, 0};
	P low_corner, high_corner;		// its box, as built
	kit::Blend blend = kit::OPAQUE;
	float alpha = 1;
	bool background = false;		// painted first, in the order made: floors, what is under them
	bool billboard = false;			// turned to the camera about the upright
	bool enabled = true;
	bool gloss = false, pictures = false, water = false;
	bool closed = false;			// a closed shape, its faces looking outwards
	bool reflected = false;			// seen again in the wet street
	bool changes = false;
	GLuint texture = 0;			// its own picture, not the atlas
	int far_off = 0;			// not drawn from this distance
	int corners = 0;			// how many vertices the original's has
	int sign = -1, sign_first = 0, sign_count = 0;	// its flickering sign: which, its vertices
	Thing () : parts (new Parts), mesh (parts->mesh), low (parts->low), tubes (parts->tubes), tiles (parts->tiles) {}
	Thing (const Thing&) = delete;
	void release ()
	{
		if (!parts)
			return;
		mesh.free ();
		low.free ();
		tubes.free ();
		delete parts;
		parts = nullptr;
	}
	~Thing ()				{ release (); }
};

std::vector<std::unique_ptr<Thing>> things;
size_t settled;				// the things before this one are (their vertices are on the RPi)
Range water_range;			// the river's panels: one after another, one draw

void arena_open (int cls)
{
	kit::Mesh& all = arena[cls];
	all.free ();
	arena_used[cls] = 0;
	if (!CAPACITY[cls])
		return;
	glGenBuffers (1, &all.buffer);
	glBindBuffer (GL_ARRAY_BUFFER, all.buffer);
	glBufferData (GL_ARRAY_BUFFER, CAPACITY[cls] * sizeof (kit::Vertex), nullptr, GL_STATIC_DRAW);
	all.uploaded = CAPACITY[cls];
	all.kept = false;
}

// (a part's triangles go corner after corner: no index buffer, of which the host would keep a copy)
Range into_arena (int cls, kit::Mesh& part)
{
	static std::vector<kit::Vertex> corners;
	Range r = {arena_used[cls], 0};
	const size_t n = part.indices.empty () ? part.vertices.size () : part.indices.size ();
	if (n == 0)
		return r;
	if (r.first + (int) n > CAPACITY[cls])
	{
		std::printf ("film: vertex buffer %d is full\n", cls);
		return r;
	}
	const kit::Vertex* data = part.vertices.data ();
	if (!part.indices.empty ())
	{
		corners.resize (n);
		for (size_t i = 0; i < n; i++)
			corners[i] = part.vertices[part.indices[i]];
		data = corners.data ();
	}
	glBindBuffer (GL_ARRAY_BUFFER, arena[cls].buffer);
	glBufferSubData (GL_ARRAY_BUFFER, r.first * sizeof (kit::Vertex), n * sizeof (kit::Vertex), data);
	r.count = (int) n;
	arena_used[cls] += (int) n;
	return r;
}

void settle ();

Thing* thing ()
{
	things.emplace_back (new Thing);
	return things.back ().get ();
}

kit::Material material (const Mat& m)
{
	// (added to the picture: Jet adds (alpha + 1) / 256 of the colour)
	kit::Material k (m.color, (uint8_t) std::min (m.additive ? m.alpha + 1 : (int) m.alpha, 255), m.diffuse, m.specular, m.gloss != 0);
	k.gloss = m.gloss;
	return k;
}

int vertex (Thing& t, kit::Mesh& mesh, P p, const Mat& m, int u = 0, int v = 0)
{
	const int k = mesh.add ((float) p.x, (float) p.y, (float) p.z, 0, 1, 0, material (m), u, v);
	if (&mesh == &t.mesh)
		t.tiles.push_back ((int8_t) m.tile);
	if (m.tile != PLAIN)
		t.pictures = true;
	if (m.gloss)
		t.gloss = true;
	if (m.additive)
		t.blend = kit::ADD;
	else if (m.alpha < 255 && t.blend == kit::OPAQUE)
		t.blend = kit::ALPHA;
	return k;
}

// Jet's quad: the picture's first row along a-b
int quad (Thing& t, kit::Mesh& mesh, P a, P b, P c, P d, const Mat& m)
{
	const int k = vertex (t, mesh, a, m, 0, 0);
	vertex (t, mesh, b, m, 1024, 0);
	vertex (t, mesh, c, m, 1024, 1024);
	vertex (t, mesh, d, m, 0, 1024);
	const int first = (int) mesh.indices.size ();
	for (int i : {0, 1, 2, 2, 3, 0})
		mesh.indices.push_back ((uint16_t) (k + i));
	if (m.sign >= 0 && &mesh == &t.mesh)
	{
		if (t.sign < 0)
			t.sign_first = first;
		t.sign = m.sign;
		t.sign_count = first + 6 - t.sign_first;
	}
	return k;
}

int quad (Thing& t, P a, P b, P c, P d, const Mat& m)
{
	t.corners += 4;
	return quad (t, t.mesh, a, b, c, d, m);
}

void triangle (Thing& t, P a, P b, P c, const Mat& m)
{
	const int k = vertex (t, t.mesh, a, m);
	vertex (t, t.mesh, b, m);
	vertex (t, t.mesh, c, m);
	for (int i = 0; i < 3; i++)
		t.mesh.indices.push_back ((uint16_t) (k + i));
	t.corners += 3;
}

// a neon tube from a to b: four vertices, each with its end, the other end, its side; kit_neon does the rest
void tube (Thing& t, P a, P b, const Mat& m, float width)
{
	const kit::Material k = material (m);
	const int first = (int) t.tubes.vertices.size ();
	const P ends[4][2] = {{a, b}, {a, b}, {b, a}, {b, a}};
	static const int side[4] = {-1, 1, -1, 1};
	for (int i = 0; i < 4; i++)
	{
		const int n = t.tubes.add ((float) ends[i][0].x, (float) ends[i][0].y, (float) ends[i][0].z, 0, 1, 0, k, side[i], (int) width);
		kit::Vertex& v = t.tubes.vertices[n];
		v.nx = (int16_t) ends[i][1].x;
		v.ny = (int16_t) ends[i][1].y;
		v.nz = (int16_t) ends[i][1].z;
	}
	for (int i : {0, 1, 2, 2, 3, 0})
		t.tubes.indices.push_back ((uint16_t) (first + i));
	t.corners += 4;
}

Thing* neon_line (P a, P b, const Mat& m, float width = 7)
{
	Thing* t = thing ();
	tube (*t, a, b, m, width);
	return t;
}

Thing* box (int x, int y, int z, int w, int h, int d, const Mat& m, bool background = false)
{
	Thing* t = thing ();
	const int first = (int) t->mesh.vertices.size ();
	t->mesh.box (0, 0, 0, (float) w, (float) h, (float) d, material (m));
	t->tiles.assign (t->mesh.vertices.size () - first, PLAIN);
	t->position = {x, y, z};
	t->background = background;
	t->closed = true;
	t->corners = 24;
	return t;
}

Thing* panel (P a, P b, P c, P d, const Mat& m, bool background = false)
{
	Thing* t = thing ();
	quad (*t, a, b, c, d, m);
	t->background = background;
	t->water = m.water;
	return t;
}

// the things made since `first` that stand still, as one (a block's front): one draw, one matrix
Thing* batch (size_t first)
{
	std::unique_ptr<Thing> all (new Thing);
	for (size_t i = first; i < things.size (); )
	{
		Thing& s = *things[i];
		if (s.billboard || s.background)
		{
			i++;
			continue;
		}
		const int base = (int) all->mesh.vertices.size (), index_base = (int) all->mesh.indices.size ();
		for (kit::Vertex v : s.mesh.vertices)
		{
			v.x = (int16_t) (v.x + s.position.x);
			v.y = (int16_t) (v.y + s.position.y);
			v.z = (int16_t) (v.z + s.position.z);
			all->mesh.vertices.push_back (v);
		}
		all->tiles.insert (all->tiles.end (), s.tiles.begin (), s.tiles.end ());
		for (uint16_t n : s.mesh.indices)
			all->mesh.indices.push_back ((uint16_t) (base + n));
		if (s.sign >= 0)
		{
			if (all->sign < 0)
				all->sign_first = index_base + s.sign_first;
			all->sign = s.sign;
			all->sign_count = index_base + s.sign_first + s.sign_count - all->sign_first;
		}
		const int tube_base = (int) all->tubes.vertices.size ();
		for (kit::Vertex v : s.tubes.vertices)
		{
			v.x = (int16_t) (v.x + s.position.x);
			v.y = (int16_t) (v.y + s.position.y);
			v.z = (int16_t) (v.z + s.position.z);
			v.nx = (int16_t) (v.nx + s.position.x);
			v.ny = (int16_t) (v.ny + s.position.y);
			v.nz = (int16_t) (v.nz + s.position.z);
			all->tubes.vertices.push_back (v);
		}
		for (uint16_t n : s.tubes.indices)
			all->tubes.indices.push_back ((uint16_t) (tube_base + n));
		all->pictures |= s.pictures;
		all->corners += s.corners;
		things.erase (things.begin () + (long) i);
	}
	things.push_back (std::move (all));
	return things.back ().get ();
}

// ---- lights that are bitmaps -----------------------------------------------------------------------

struct Glow
{
	P position;
	int scale;
	int alpha = 255;
	bool enabled = true;
};
std::vector<Glow> glows;

void glow (P p, int scale = 2)
{
	glows.push_back ({p, scale});
}

// ---- the film's state ------------------------------------------------------------------------------

kit::Camera camera;
kit::Mat4 eye;				// the frame's camera: the world before it
float focal;				// ... its focal length, in the scene's pixels
float lens_sent = -1;			// (what the tubes' program was last told of it)
kit::Light light;
uint16_t sky[kit::H];
kit::Program neon_program;
GLint neon_lens;
GLuint water_program;
GLint w_pos, w_mvp, w_area, w_water, w_color;
float seconds_in = 0, shot_time = 0, wheel_phase = 0;
int shot = -1;
bool has_water = false;
Mat water_paint;
struct Searchlight { Thing* mesh; int phase; };
std::vector<Searchlight> searchlights;
std::vector<Thing*> holograms;
Thing* scanner = nullptr;
std::vector<std::pair<Thing*, P>> scrolling;
std::vector<P> approach_glows;
int hero_glow = -1, displayed_speed = -1;
bool credits = false;

struct Particle { float pos[3], vel[3], life, max_life; bool active; };
Particle pool[34];
kit::Mesh spray, rain;

// ---- the city --------------------------------------------------------------------------------------

void building (int x, int z, int width, int depth, int height, int style, bool detail = true)
{
	const Mat dark = paint (style % 2 ? 0x19243B : 0x25213C);
	Thing* tower = thing ();
	const int bevel = std::min (width, depth) / 6;
	const P ring[8] = {{-width / 2 + bevel, 0, -depth / 2}, {width / 2 - bevel, 0, -depth / 2}, {width / 2, 0, -depth / 2 + bevel},
			   {width / 2, 0, depth / 2 - bevel}, {width / 2 - bevel, 0, depth / 2}, {-width / 2 + bevel, 0, depth / 2},
			   {-width / 2, 0, depth / 2 - bevel}, {-width / 2, 0, -depth / 2 + bevel}};
	for (int j = 0; j < 8; j++)
	{
		const P a = ring[j], b = ring[(j + 1) % 8];
		const int ah = height + (style % 3 == 0 ? a.x / 3 : 0), bh = height + (style % 3 == 0 ? b.x / 3 : 0);
		quad (*tower, a, b, b + P {0, bh, 0}, a + P {0, ah, 0}, dark);
		triangle (*tower, {0, height, 0}, a + P {0, ah, 0}, b + P {0, bh, 0}, dark);
	}
	for (size_t i = 0; i + 2 < tower->mesh.indices.size (); i += 3)	// (the walls are given inside out)
		std::swap (tower->mesh.indices[i], tower->mesh.indices[i + 2]);
	tower->closed = true;
	tower->position = {x, 0, z};
	const Mat windows = textured (FACADE0 + style % 4);
	panel ({x - width / 2 + width / 6, 30, z - depth / 2 - 2}, {x + width / 2 - width / 6, 30, z - depth / 2 - 2},
	       {x + width / 2 - width / 6, height - 65, z - depth / 2 - 2}, {x - width / 2 + width / 6, height - 65, z - depth / 2 - 2}, windows);
	panel ({x - width / 2 - 2, 30, z + depth / 2 - depth / 6}, {x - width / 2 - 2, 30, z - depth / 2 + depth / 6},
	       {x - width / 2 - 2, height - 65, z - depth / 2 + depth / 6}, {x - width / 2 - 2, height - 65, z + depth / 2 - depth / 6}, windows);
	if (detail)
	{
		const Mat trim = paint (style % 2 ? 0xEA4BAF : 0x37C8E2);
		neon_line ({x - width / 2 - 4, 0, z - depth / 2 - 4}, {x - width / 2 - 4, height, z - depth / 2 - 4}, trim);
		if (style % 3 == 1)
			box (x, height + 60, z, width / 2, 120, depth / 2, paint (0x36475D));
		if (style % 3 == 0)
		{
			box (x, height + 80, z, 14, 160, 14, trim);
			glow ({x, height + 164, z}, 2);
		}
	}
}

void skyline ()
{
	for (int i = 0; i < 17; i++)
	{
		const int x = (i - 8) * 440, z = 2500 + (i % 3) * 330, h = 600 + (i * 433) % 1400;
		building (x, z, 330 + (i % 3) * 70, 340, h, i, i % 2 == 0);
	}
	// a spire in the middle, a lit bridge before the far bank
	building (240, 2200, 510, 480, 2200, 1);
	box (240, 2280, 2200, 100, 160, 100, paint (0xB954CA));
	glow ({240, 2370, 2200}, 3);
	const Mat beam = paint (0x54C8FF, 65, true), core = paint (0xB9EFFF, 100, true);
	for (int x : {-1400, 1450})
	{
		for (int layer = 0; layer < 2; layer++)
		{
			const int spread = layer ? 115 : 380;
			Thing* b = panel ({-18, 0, 0}, {18, 0, 0}, {spread, 2200, 0}, {-spread, 2200, 0}, layer ? core : beam);
			b->position = {x, 650, 1600};
			searchlights.push_back ({b, x < 0 ? 0 : 3});
		}
		glow ({x, 660, 1600}, 3);
	}
	box (0, 90, 1900, 8000, 24, 60, paint (0x1F3247));
	neon_line ({-4000, 130, 1880}, {4000, 130, 1880}, paint (0x31CEDB));
	for (int i = 0; i < 3; i++)			// figures of light, turned to the camera
	{
		Thing* o = thing ();
		Mat holo;
		holo.alpha = 150;
		holo.additive = true;
		holo.tile = FACADE0;			// (any: its picture is its own)
		// Jet's createQuad: the picture's first row at the bottom
		const int k = vertex (*o, o->mesh, {-190, -285, 0}, holo, 0, 0);
		vertex (*o, o->mesh, {190, -285, 0}, holo, 1024, 0);
		vertex (*o, o->mesh, {190, 285, 0}, holo, 1024, 1024);
		vertex (*o, o->mesh, {-190, 285, 0}, holo, 0, 1024);
		for (int n : {0, 1, 2, 0, 2, 3})
			o->mesh.indices.push_back ((uint16_t) (k + n));
		o->corners = 4;
		o->billboard = true;
		o->texture = holo_texture;
		o->position = {-1700 + i * 1680, 1000 + i * 110, 1550};
		holograms.push_back (o);
	}
	water_paint = paint (0x102D4E, 100);
	water_paint.water = true;
	has_water = true;
	for (int x = -3; x < 3; x++)
		for (int z = -2; z < 2; z++)
			panel ({x * 2100, 0, z * 1800 - 1650}, {(x + 1) * 2100, 0, z * 1800 - 1650}, {(x + 1) * 2100, 0, (z + 1) * 1800 - 1650},
			       {x * 2100, 0, (z + 1) * 1800 - 1650}, water_paint, true);
}

void flip_pictures (Thing* t)			// the far side of the street: its pictures the other way round
{
	if (!t->pictures)
		return;
	for (kit::Vertex& v : t->mesh.vertices)
		v.u = (int16_t) (1024 - v.u);
	for (kit::Vertex& v : t->low.vertices)
		v.u = (int16_t) (1024 - v.u);
}

void street (int half_width = 320, int blocks = 12, bool mirror = false, int detailed_blocks = -1)
{
	const size_t first = things.size ();
	const int road_end = blocks * 720 + 1500;
	panel ({-half_width - 350, 0, -road_end}, {half_width + 350, 0, -road_end}, {half_width + 350, 0, road_end}, {-half_width - 350, 0, road_end},
	       paint (0x141D2B), true);
	const Mat stripe = paint (0x72808C), trim = paint (0x45D1D2), pink = paint (0xD94593), curb = paint (0x394353);
	const Mat wall = paint (0x1D2638), awning = paint (0x3D2749), pipe = paint (0x4C5667);
	Mat signs[6];
	for (int i = 0; i < 6; i++)
	{
		signs[i] = textured (SIGN0 + i);
		signs[i].sign = i;
	}
	for (int i = 0; i < blocks; i++)
	{
		const int z = i * 720 - 700;
		if (half_width > 500)
			for (int x : {-190, 190})
				panel ({x - 3, 2, z}, {x + 3, 2, z}, {x + 3, 2, z + 210}, {x - 3, 2, z + 210}, stripe, true);
		for (int side : {-1, 1})
		{
			const int x = side * (half_width + 330), h = 1400 + (i * 371 + side * 157 + 2000) % 1100;
			// the block's shell, without its street side: the fronts are that
			Thing* shell = box (x, h / 2, z, 620, h, 650, wall);
			{
				std::vector<uint16_t> kept;
				for (size_t n = 0; n + 2 < shell->mesh.vertices.size (); n += 3)
				{
					const kit::Vertex* v = &shell->mesh.vertices[n];
					if (v[0].x == -side * 310 && v[1].x == -side * 310 && v[2].x == -side * 310)
						continue;
					for (int k = 0; k < 3; k++)
						kept.push_back ((uint16_t) (n + k));
				}
				shell->mesh.indices = kept;
			}
			const size_t art_first = things.size ();
			const int face_x = side * (half_width + 16), k = (i + (side > 0 ? 1 : 0)) % 4;
			if (detailed_blocks >= 0 && i >= detailed_blocks)
			{
				// (blocks the camera never comes near: a lit front, no shop)
				Thing* facade = panel ({face_x, 16, z - 300}, {face_x, 16, z + 300}, {face_x, h - 40, z + 300}, {face_x, h - 40, z - 300},
						       textured (FAR0 + k));
				if (side > 0)
					flip_pictures (facade);
				neon_line ({face_x - side, 40, z - 304}, {face_x - side, h - 40, z - 304}, i % 2 ? pink : trim);
				continue;
			}
			panel ({face_x, 350, z - 300}, {face_x, 350, z + 300}, {face_x, h - 40, z + 300}, {face_x, h - 40, z - 300}, textured (FACADE0 + k));
			panel ({face_x, 16, z - 300}, {face_x, 16, z + 300}, {face_x, 305, z + 300}, {face_x, 305, z - 300}, textured (SHOP0 + k));
			panel ({side * half_width, 8, z - 340}, {side * (half_width + 100), 8, z - 340}, {side * (half_width + 100), 8, z + 340},
			       {side * half_width, 8, z + 340}, curb, true);
			// a sloping canopy, a sign that sticks out
			panel ({face_x, 345, z - 308}, {face_x, 345, z + 308}, {side * (half_width - 58), 303, z + 308}, {side * (half_width - 58), 303, z - 308}, awning);
			neon_line ({side * (half_width - 59), 300, z - 309}, {side * (half_width - 59), 300, z + 309}, i % 2 ? pink : trim);
			Thing* lamp = thing ();
			lamp->billboard = true;
			lamp->position = {side * (half_width - 18), 0, z + 280};
			quad (*lamp, {-3, 0, 0}, {3, 0, 0}, {3, 290, 0}, {-3, 290, 0}, pipe);
			quad (*lamp, {-24, 290, 0}, {24, 290, 0}, {24, 295, 0}, {-24, 295, 0}, trim);
			lamp->far_off = 3000;
			const Mat& sm = signs[(i + (side > 0 ? 2 : 0)) % 6];
			const int sx = side * (half_width - 24);
			panel ({sx, 360, z - 220}, {sx, 360, z + 85}, {sx, 462, z + 85}, {sx, 462, z - 220}, sm);
			if (i % 3 == 0)
			{
				panel ({side * (half_width - 4), 470, z - 302}, {side * (half_width - 105), 470, z - 302}, {side * (half_width - 105), 650, z - 302},
				       {side * (half_width - 4), 650, z - 302}, sm);
				glow ({sx, 388, z - 65}, 1);
			}
			neon_line ({side * (half_width - 4), 480, z - 312}, {side * (half_width - 4), 950, z - 312}, i % 2 ? pink : trim);
			if (side > 0)
				for (size_t n = art_first; n < things.size (); n++)
					flip_pictures (things[n].get ());
			if (!mirror)
			{
				const int wx = side * (half_width - 100);		// a wet patch that shines
				panel ({wx - 20, 2, z - 210}, {wx + 20, 2, z - 210}, {wx + 10, 2, z + 240}, {wx - 10, 2, z + 240},
				       paint (i % 2 ? 0xD0398C : 0x309CA5, 38), true);
				Thing* head = batch (art_first);
				// from far off: the same planes and colours, no shop furniture, the windows' small picture
				Thing far_one;
				quad (far_one, {face_x, 350, z - 300}, {face_x, 350, z + 300}, {face_x, h - 40, z + 300}, {face_x, h - 40, z - 300}, textured (FAR0 + k));
				quad (far_one, {face_x, 16, z - 300}, {face_x, 16, z + 300}, {face_x, 305, z + 300}, {face_x, 305, z - 300}, textured (SHOP0 + k));
				quad (far_one, {face_x, 286, z - 309}, {face_x, 286, z + 309}, {face_x, 318, z + 309}, {face_x, 318, z - 309}, i % 2 ? pink : trim);
				if (side > 0)
					flip_pictures (&far_one);
				head->low.vertices = far_one.mesh.vertices;
				head->low.indices = far_one.mesh.indices;
				for (size_t n = 0; n < far_one.tiles.size (); n++)
					head->tiles.push_back (far_one.tiles[n]);	// (the far lot's follow the near lot's)
			}
			if (!mirror)
				settle ();			// (to the RPi, block by block)
		}
		if (half_width < 500 && i % 3 == 1)
		{
			const Mat cable = paint (0x46536D);		// two cables across, high up
			panel ({-half_width, 810, z}, {0, 745, z + 30}, {0, 749, z + 30}, {-half_width, 814, z}, cable);
			panel ({0, 745, z + 30}, {half_width, 805, z + 60}, {half_width, 809, z + 60}, {0, 749, z + 30}, cable);
		}
	}
	if (mirror)
	{
		// the wet street: the fronts, signs and tubes are seen in it; over them a floor that lets them through
		for (size_t i = first; i < things.size (); i++)
			if (!things[i]->background && things[i]->corners <= 4)
				things[i]->reflected = true;
		panel ({-half_width, 3, -2000}, {half_width, 3, -2000}, {half_width, 3, blocks * 720 + 1300}, {-half_width, 3, blocks * 720 + 1300},
		       paint (0x142637, 175), true);
	}
}

void boulevard (int blocks = 12)
{
	street (680, blocks, false);
	const Mat rail = paint (0x344255), cyan = paint (0x77DAD6);
	for (int i = 0; i < 8; i++)
	{
		const int z = i * 950;
		box (-660, 40, z, 28, 80, 28, rail);
		box (660, 40, z, 28, 80, 28, rail);
		box (-660, 85, z, 28, 10, 28, cyan);
		box (660, 85, z, 28, 10, 28, cyan);
	}
}

void city_grid ()
{
	panel ({-8500, 0, -8500}, {8500, 0, -8500}, {8500, 0, 8500}, {-8500, 0, 8500}, paint (0x142438), true);
	for (int i = 0; i < 7; i++)
		for (int j = 0; j < 5; j++)
			building ((i - 3) * 1350, (j - 1) * 1450, 620, 710, 700 + (i * 371 + j * 529) % 1300, i + j, false);
	const Mat lit = paint (0x34AEBE);
	for (int i = -3; i <= 3; i++)
	{
		const int x = i * 1350 + 570;
		neon_line ({x, 2, -3000}, {x, 2, 7800}, lit, 10);
	}
}

// ---- the street, recycled behind the camera --------------------------------------------------------

P centre (const Thing& t)
{
	return (t.low_corner + t.high_corner).divide (2);
}

void measure (Thing& t)				// its box (before its vertices are handed over)
{
	bool any = false;
	auto take = [&] (const kit::Vertex& v, bool other_end)
	{
		const P p = other_end ? P {v.nx, v.ny, v.nz} : P {v.x, v.y, v.z};
		if (!any)
			t.low_corner = t.high_corner = p;
		any = true;
		t.low_corner = {std::min (t.low_corner.x, p.x), std::min (t.low_corner.y, p.y), std::min (t.low_corner.z, p.z)};
		t.high_corner = {std::max (t.high_corner.x, p.x), std::max (t.high_corner.y, p.y), std::max (t.high_corner.z, p.z)};
	};
	for (const kit::Vertex& v : t.mesh.vertices)
		take (v, false);
	for (const kit::Vertex& v : t.tubes.vertices)
	{
		take (v, false);
		take (v, true);
	}
}

struct RoadTrack
{
	struct Item { Thing* thing; P origin; int centre; };
	std::vector<Item> items;
	std::vector<P> halo_origins;
	int period = 0, rear_extent = 2400;
	size_t first_halo = 0;
	P axis = {0, 0, 1};
	void clear ()
	{
		items.clear ();
		halo_origins.clear ();
		period = 0;
	}
	void capture (int length, size_t first_thing = 0, size_t first_glow = 0, bool eastbound = false)
	{
		clear ();
		settle ();
		period = length;
		first_halo = first_glow;
		axis = eastbound ? P {1, 0, 0} : P {0, 0, 1};
		for (size_t n = first_thing; n < things.size (); n++)
		{
			Thing& o = *things[n];
			if (o.high_corner.z - o.low_corner.z > length)
				continue;			// (the road itself goes on)
			const P c = o.position + yawed (centre (o), (float) o.rotation[1]);
			items.push_back ({&o, o.position, eastbound ? c.x : c.z});
		}
		for (size_t n = first_glow; n < glows.size (); n++)
			halo_origins.push_back (glows[n].position);
	}
	int offset (float travel, int c) const
	{
		const int distance = (int) travel;
		return -distance + (int) std::floor ((float) (distance - c + period - rear_extent) / period) * period;
	}
	void restore ()
	{
		for (auto& i : items)
			i.thing->position = i.origin;
		for (size_t i = 0; i < halo_origins.size (); i++)
			glows[first_halo + i].position = halo_origins[i];
	}
	// the long end of the street is kept for the way the shot looks
	void advance (float distance, bool looking_back = false)
	{
		rear_extent = looking_back ? period - 2400 : 2400;
		for (auto& i : items)
			i.thing->position = i.origin + axis * offset (distance, i.centre);
		for (size_t i = 0; i < halo_origins.size (); i++)
			glows[first_halo + i].position = halo_origins[i] + axis * offset (distance, axis.x ? halo_origins[i].x : halo_origins[i].z);
	}
};
RoadTrack road;

// the 570-unit coupe is a 4.5 m car: miles an hour in the street's units
constexpr float UNITS_PER_METRE = 570.0f / 4.5f;
constexpr float speed_from_mph (float mph)	{ return mph * 0.44704f * UNITS_PER_METRE; }
constexpr float ROAD_SPEED = speed_from_mph (70), CHASE_SPEED = speed_from_mph (88);
constexpr float CLIMB_SPEED = 650, CLIMB_RAMP = 0.8f;
float launch_height (float t)		{ return 55 + CLIMB_SPEED * (t < CLIMB_RAMP ? t * t / (2 * CLIMB_RAMP) : t - CLIMB_RAMP / 2); }
float launch_pitch (float t)		{ return std::atan2 (CLIMB_SPEED * clamp01 (t / CLIMB_RAMP), CHASE_SPEED) * 180 / PI; }
constexpr float TURN_START = 2, TURN_END = 3.2f, TURN_RADIUS = 1030;
constexpr float EXIT_TRACKING_START = TURN_END + 1.05f;
constexpr float CORNER_SPEED = TURN_RADIUS * PI / (2 * (TURN_END - TURN_START));
constexpr float BRAKE_START = 0.55f, BRAKE_SECONDS = 1.1f, EXIT_ACCELERATION_SECONDS = 1.8f;

float approach_travel (float t)
{
	const float u = std::min (std::max (t - BRAKE_START, 0.0f), BRAKE_SECONDS);
	return CHASE_SPEED * std::min (t, BRAKE_START) + CHASE_SPEED * u - (CHASE_SPEED - CORNER_SPEED) * u * u / (2 * BRAKE_SECONDS)
	       + CORNER_SPEED * std::max (0.0f, t - BRAKE_START - BRAKE_SECONDS);
}

float exit_travel (float t)
{
	const float u = std::min (t, EXIT_ACCELERATION_SECONDS);
	return CORNER_SPEED * u + (CHASE_SPEED - CORNER_SPEED) * u * u / (2 * EXIT_ACCELERATION_SECONDS)
	       + CHASE_SPEED * std::max (0.0f, t - EXIT_ACCELERATION_SECONDS);
}

float exit_speed (float t)		{ return CORNER_SPEED + (CHASE_SPEED - CORNER_SPEED) * clamp01 (t / EXIT_ACCELERATION_SECONDS); }

// a change of lane: eased, as steering is; its rate turns the car's nose
struct LaneChange { float from, to, start, seconds; };
float lane_position (float t, const LaneChange& c)
{
	const float u = clamp01 ((t - c.start) / c.seconds);
	return c.from + (c.to - c.from) * u * u * (3 - 2 * u);
}
float lane_velocity (float t, const LaneChange& c)
{
	const float u = clamp01 ((t - c.start) / c.seconds);
	return (c.to - c.from) * 6 * u * (1 - u) / c.seconds;
}
LaneChange chase_lane (float t)
{
	if (t < 4.5f)
		return {-380, 0, 0.6f, 1.2f};
	if (t < 8.2f)
		return {0, 380, 4.5f, 1.4f};
	return {380, 0, 8.2f, 1.4f};
}
float steering_yaw (float lateral, float forward)	{ return std::atan2 (lateral, forward) * 180 / PI; }

// ---- the cars --------------------------------------------------------------------------------------

struct Vehicle
{
	struct Part { Thing* thing; P local; int kind; };
	std::vector<Part> parts;
	std::vector<P> glass_points, glass_normals;	// the windows, as built (their picture follows the eye)
	P position;
	float heading = 0;
	int body_pitch = 0, body_roll = 0;
	bool suspension = false;

	// (a positive pitch lifts the nose)
	P direction (P v) const
	{
		if (body_roll)
		{
			const float r = body_roll * PI / 180;
			v = {(int) (v.x * std::cos (r) - v.y * std::sin (r)), (int) (v.x * std::sin (r) + v.y * std::cos (r)), v.z};
		}
		const float a = body_pitch * PI / 180;
		return yawed ({v.x, (int) (v.y * std::cos (a) + v.z * std::sin (a)), (int) (-v.y * std::sin (a) + v.z * std::cos (a))}, heading);
	}
	P world_point (P v) const
	{
		const P pivot = {0, suspension ? 70 : 0, 0};
		return position + pivot + direction (v - pivot);
	}
	// the body's yaw and pitch, a wheel pod's hinge and the wheel's spin, as the three angles a mesh turns by
	P wheel_rotation (int spin, int roll) const
	{
		if (!body_pitch && !body_roll)
			return {spin, (int) heading, roll};
		roll += body_roll;
		const float y = heading * PI / 180, p = body_pitch * PI / 180, r = roll * PI / 180, x = spin * PI / 180;
		const float cy = std::cos (y), sy = std::sin (y), cp = std::cos (p), sp = std::sin (p), cr = std::cos (r), sr = std::sin (r);
		const float cx = std::cos (x), sx = std::sin (x);
		const float m00 = cy * cr - sy * sp * sr, m10 = cp * sr, m20 = -sy * cr - cy * sp * sr;
		const float m21 = sy * sr * cx - cy * sp * cr * cx + cy * cp * sx, m22 = -sy * sr * sx + cy * sp * cr * sx + cy * cp * cx;
		return {(int) std::round (std::atan2 (m21, m22) * 180 / PI), (int) std::round (std::asin (std::min (std::max (-m20, -1.0f), 1.0f)) * 180 / PI),
			(int) std::round (std::atan2 (m10, m00) * 180 / PI)};
	}
	void add (Thing* o, P local = {0, 0, 0}, int kind = 0)
	{
		if (local.x == 0 && local.y == 0 && local.z == 0)
			local = o->position;
		// what stands as the body does (the canopy, what's drawn on the body, doors) becomes part of it: one draw
		Thing* body = parts.empty () ? nullptr : parts[0].thing;
		if (   (kind == 0 || kind == 12) && body && local.x == 0 && local.y == 0 && local.z == 0 && o == things.back ().get ()
		    && !o->texture && !o->changes && o->blend == kit::OPAQUE && body->blend == kit::OPAQUE)
		{
			const int base = (int) body->mesh.vertices.size ();
			body->mesh.vertices.insert (body->mesh.vertices.end (), o->mesh.vertices.begin (), o->mesh.vertices.end ());
			for (uint16_t i : o->mesh.indices)
				body->mesh.indices.push_back ((uint16_t) (base + i));
			body->tiles.insert (body->tiles.end (), o->tiles.begin (), o->tiles.end ());
			body->gloss |= o->gloss;
			body->corners += o->corners;
			things.pop_back ();
			return;
		}
		parts.push_back ({o, local, kind});
	}
	// a face, wound so that it looks the way given
	static bool facing (P a, P b, P c, P outward)
	{
		const P u = b - a, v = c - a;
		const long long f = ((long long) u.y * v.z - (long long) u.z * v.y) * outward.x + ((long long) u.z * v.x - (long long) u.x * v.z) * outward.y
				    + ((long long) u.x * v.y - (long long) u.y * v.x) * outward.z;
		return f >= 0;
	}
	static void face (Thing* o, P a, P b, P c, P d, const Mat& m, P outward)
	{
		if (facing (a, b, c, outward))
			quad (*o, a, b, c, d, m);
		else
			quad (*o, d, c, b, a, m);
	}
	static void tri (Thing* o, P a, P b, P c, const Mat& m, P outward)
	{
		if (!facing (a, b, c, outward))
			std::swap (b, c);
		triangle (*o, a, b, c, m);
	}
	// each triangle's own normal on its vertices (a later triangle's over an earlier one's, as Jet leaves them)
	static void flat_normals (Thing* o)
	{
		for (size_t i = 0; i + 2 < o->mesh.indices.size (); i += 3)
		{
			kit::Vertex* v[3] = {&o->mesh.vertices[o->mesh.indices[i]], &o->mesh.vertices[o->mesh.indices[i + 1]], &o->mesh.vertices[o->mesh.indices[i + 2]]};
			const float u[3] = {(float) (v[1]->x - v[0]->x), (float) (v[1]->y - v[0]->y), (float) (v[1]->z - v[0]->z)};
			const float w[3] = {(float) (v[2]->x - v[0]->x), (float) (v[2]->y - v[0]->y), (float) (v[2]->z - v[0]->z)};
			float n[3] = {u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0]};
			const float l = std::sqrt (n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
			if (l <= 0)
				continue;
			for (auto* p : v)
			{
				p->nx = (int16_t) std::lround (n[0] / l * 32767);
				p->ny = (int16_t) std::lround (n[1] / l * 32767);
				p->nz = (int16_t) std::lround (n[2] / l * 32767);
			}
		}
	}

	void wheel (int side, int z, bool transformable, const Mat& wheel_glow)
	{
		Thing* o = thing ();
		const Mat rubber = paint (0x111724), alloy = paint (0x8EADCA), spoke = paint (0x34475B);
		constexpr int segments = 8;
		auto point = [side] (float a, int radius, int x) { return P {side * x, (int) (radius * std::cos (a)), (int) (radius * std::sin (a))}; };
		for (int i = 0; i < segments; i++)
		{
			const float a = i * 2 * PI / segments, b = (i + 1) * 2 * PI / segments, mid = (a + b) * 0.5f;
			const P radial = {0, (int) (1024 * std::cos (mid)), (int) (1024 * std::sin (mid))};
			// tread, two side walls, the alloy disc, the inner cap: closed, in flight too
			face (o, point (a, 44, -14), point (b, 44, -14), point (b, 44, 14), point (a, 44, 14), rubber, radial);
			face (o, point (a, 44, -14), point (a, 35, -17), point (b, 35, -17), point (b, 44, -14), rubber, {-side, 0, 0});
			tri (o, {side * -17, 0, 0}, point (a, 35, -17), point (b, 35, -17), rubber, {-side, 0, 0});
			face (o, point (a, 44, 14), point (b, 44, 14), point (b, 35, 17), point (a, 35, 17), rubber, {side, 0, 0});
			face (o, point (a, 35, 17), point (b, 35, 17), point (b, 14, 19), point (a, 14, 19), i % 3 ? alloy : spoke, {side, 0, 0});
			tri (o, {side * 19, 0, 0}, point (a, 14, 19), point (b, 14, 19), spoke, {side, 0, 0});
		}
		add (o, {side * 122, 46, z}, side < 0 ? 2 : 3);
		if (transformable)			// the ring that lights when the wheel is a hover pod
		{
			Thing* ring = thing ();
			for (int i = 0; i < segments; i++)
			{
				const float a = i * 2 * PI / segments, b = (i + 1) * 2 * PI / segments;
				quad (*ring, point (a, 30, 23), point (b, 30, 23), point (b, 22, 23), point (a, 22, 23), wheel_glow);
			}
			add (ring, {side * 122, 46, z}, side < 0 ? 12 + 2 : 12 + 3);
		}
	}

	void hero_body (bool transformable, const Mat& wheel_glow)
	{
		Mat metal = paint (0x7899CB), edge = paint (0x465E85);
		metal.diffuse = 165;
		metal.specular = 230;
		edge.diffuse = 175;
		edge.specular = 190;
		metal.gloss = edge.gloss = 32;
		const Mat trim = paint (0x122234), lamps = paint (0xB5F8FF), red = paint (0xFF395D);
		Thing* body = thing ();
		static const int shape_z[] = {-285, -225, -110, 95, 205, 285}, shape_w[] = {108, 132, 128, 124, 128, 110}, shape_y[] = {98, 117, 111, 104, 94, 66};
		auto shape = [&] (int z, int* w, int* y)
		{
			int i = 0;
			while (i < 4 && z > shape_z[i + 1])
				i++;
			const float q = (float) (z - shape_z[i]) / (shape_z[i + 1] - shape_z[i]);
			*w = (int) (shape_w[i] + (shape_w[i + 1] - shape_w[i]) * q);
			*y = (int) (shape_y[i] + (shape_y[i + 1] - shape_y[i]) * q);
		};
		static const int zs[] = {-285, -225, -213, -175, -137, -125, 125, 137, 175, 213, 225, 285};
		static const int arches[] = {38, 38, 82, 109, 82, 38, 38, 80, 101, 80, 38, 38};
		constexpr int sections = 12;
		auto ring = [&] (int i, P* p)
		{
			int w, y;
			shape (zs[i], &w, &y);
			const int z = zs[i], lip = std::min (arches[i], y - 5);
			const P section[10] = {{-w + 18, y + 8, z}, {w - 18, y + 8, z}, {w, y, z}, {w, lip, z}, {85, lip, z}, {85, 26, z}, {-85, 26, z},
					       {-85, lip, z}, {-w, lip, z}, {-w, y, z}};
			std::copy (section, section + 10, p);
		};
		// one closed loft: the wheel arches have ceilings and inner walls, the floor is whole
		static const P outward[10] = {{0, 1, 0}, {1, 1, 0}, {1, 0, 0}, {0, -1, 0}, {1, 0, 0}, {0, -1, 0}, {-1, 0, 0}, {0, -1, 0}, {-1, 0, 0}, {-1, 1, 0}};
		P a[10], b[10];
		ring (0, a);
		for (int i = 1; i < sections; i++)
		{
			ring (i, b);
			for (int j = 0; j < 10; j++)
				face (body, a[j], b[j], b[(j + 1) % 10], a[(j + 1) % 10], j == 0 ? metal : (j == 1 || j == 9) ? edge : (j == 2 || j == 8) ? metal : trim,
				      outward[j]);
			std::copy (b, b + 10, a);
		}
		for (int end : {0, sections - 1})
		{
			ring (end, a);
			for (int j = 0; j < 10; j++)
				tri (body, {0, 65, zs[end]}, a[j], a[(j + 1) % 10], trim, {0, 0, end ? 1 : -1});
		}
		flat_normals (body);
		add (body);

		// the canopy: glass set into frames made of the same corners
		Thing* canopy = thing ();
		Thing* windows = thing ();
		Mat glass;
		glass.tile = FACADE0;			// (its picture is the room it mirrors)
		const P rear_l = {-98, 118, -170}, rear_r = {98, 118, -170}, front_l = {-104, 106, 121}, front_r = {104, 106, 121};
		const P roof_rear_l = {-84, 157, -92}, roof_rear_r = {84, 157, -92}, roof_front_l = {-80, 160, 30}, roof_front_r = {80, 160, 30};
		auto glazed = [&] (P p0, P p1, P p2, P p3, P normal)
		{
			const P outer[4] = {p0, p1, p2, p3};
			P inner[4];
			for (int i = 0; i < 4; i++)
			{
				const P p = outer[i], u = outer[(i + 1) % 4] - p, v = outer[(i + 3) % 4] - p;
				inner[i] = p + (u + v).divide (12);
			}
			for (int i = 0; i < 4; i++)
			{
				const int j = (i + 1) % 4;
				face (canopy, outer[i], outer[j], inner[j], inner[i], edge, normal);
			}
			face (windows, inner[0], inner[1], inner[2], inner[3], glass, normal);
		};
		glazed (front_l, front_r, roof_front_r, roof_front_l, {0, 1, 1});
		glazed (rear_r, rear_l, roof_rear_l, roof_rear_r, {0, 1, -1});
		glazed (rear_l, front_l, roof_front_l, roof_rear_l, {-1, 1, 0});
		glazed (front_r, rear_r, roof_rear_r, roof_front_r, {1, 1, 0});
		face (canopy, roof_rear_l, roof_rear_r, roof_front_r, roof_front_l, metal, {0, 1, 0});
		face (canopy, rear_l, rear_r, front_r, front_l, trim, {0, -1, 0});
		flat_normals (canopy);
		add (canopy);
		flat_normals (windows);
		windows->texture = env_texture;
		windows->changes = true;
		for (const kit::Vertex& v : windows->mesh.vertices)
		{
			glass_points.push_back ({v.x, v.y, v.z});
			glass_normals.push_back ({v.nx / 32, v.ny / 32, v.nz / 32});
		}
		add (windows, {}, 1);

		// what is drawn on the body: side strips, door lines, the lamps
		Thing* details = thing ();
		for (int s : {-1, 1})
		{
			auto side_at = [&] (int z, int y)
			{
				int w, h;
				shape (z, &w, &h);
				return P {s * (w + 1), y, z};
			};
			face (details, side_at (-117, 38), side_at (115, 38), side_at (115, 44), side_at (-117, 44), trim, {s, 0, 0});
			face (details, side_at (-78, 46), side_at (-78, 108), side_at (-75, 108), side_at (-75, 46), trim, {s, 0, 0});
			face (details, side_at (-61, 93), side_at (-29, 93), side_at (-29, 97), side_at (-61, 97), trim, {s, 0, 0});
			face (details, {s * 108, 51, 286}, {s * 43, 51, 286}, {s * 43, 58, 286}, {s * 108, 58, 286}, lamps, {0, 0, 1});
		}
		face (details, {-100, 83, -286}, {100, 83, -286}, {100, 90, -286}, {-100, 90, -286}, red, {0, 0, -1});
		add (details, {}, 12);
		for (int s : {-1, 1})
			for (int z : {-175, 175})
				wheel (s, z, transformable, wheel_glow);
	}

	void traffic_body (bool police, int style)
	{
		static const unsigned colors[] = {0x94776C, 0x547789, 0xBDC2AA, 0x785585, 0x355665, 0xB88542};
		const Mat metal = paint (police ? 0xD4DCDF : colors[style % 6]), trim = paint (0x172532);
		// a closed body with wheel wells
		Thing* chassis = thing ();
		static const int zs[] = {-280, -225, -202, -138, -115, 115, 138, 202, 225, 280};
		static const int lips[] = {40, 40, 104, 104, 40, 40, 104, 104, 40, 40};
		auto ring = [&] (int i, P* points)
		{
			const int z = zs[i], lip = lips[i];
			const P section[10] = {{-114, 118, z}, {114, 118, z}, {130, 106, z}, {130, lip, z}, {96, lip, z}, {96, 36, z}, {-96, 36, z},
					       {-96, lip, z}, {-130, lip, z}, {-130, 106, z}};
			std::copy (section, section + 10, points);
		};
		static const P outward[10] = {{0, 1, 0}, {1, 1, 0}, {1, 0, 0}, {0, -1, 0}, {1, 0, 0}, {0, -1, 0}, {-1, 0, 0}, {0, -1, 0}, {-1, 0, 0}, {-1, 1, 0}};
		P a[10], b[10];
		ring (0, a);
		for (int i = 1; i < 10; i++)
		{
			ring (i, b);
			for (int j = 0; j < 10; j++)
				face (chassis, a[j], b[j], b[(j + 1) % 10], a[(j + 1) % 10], j <= 2 || j >= 8 ? metal : trim, outward[j]);
			std::copy (b, b + 10, a);
		}
		for (int end : {0, 9})
		{
			ring (end, a);
			for (int j = 0; j < 10; j++)
				tri (chassis, {0, 75, zs[end]}, a[j], a[(j + 1) % 10], trim, {0, 0, end ? 1 : -1});
		}
		add (chassis);
		Thing* body = thing ();
		const Mat glass = paint (police ? 0x25495F : 0x2E5768);
		const int roof = style % 2 ? 204 : 178;
		quad (*body, {-101, 113, -170}, {101, 113, -170}, {82, roof, -105}, {-82, roof, -105}, glass);
		quad (*body, {-101, 108, 165}, {-82, roof, 12}, {82, roof, 12}, {101, 108, 165}, glass);
		quad (*body, {-101, 113, -170}, {-82, roof, -105}, {-82, roof, 12}, {-101, 108, 165}, glass);
		quad (*body, {101, 108, 165}, {82, roof, 12}, {82, roof, -105}, {101, 113, -170}, glass);
		quad (*body, {-82, roof, -105}, {82, roof, -105}, {82, roof, 12}, {-82, roof, 12}, metal);
		for (int side : {-1, 1})		// the door's belt, a pillar, a handle, the lamps
		{
			quad (*body, {side * 131, 60, -108}, {side * 131, 60, 108}, {side * 131, 70, 108}, {side * 131, 70, -108}, trim);
			quad (*body, {side * 100, 111, -45}, {side * 100, 111, -33}, {side * 83, roof, -33}, {side * 83, roof, -45}, trim);
			quad (*body, {side * 124, 89, -74}, {side * 124, 89, -34}, {side * 124, 94, -34}, {side * 124, 94, -74}, paint (0xB9CFD5));
			quad (*body, {side * 70 - 21, 80, 282}, {side * 70 + 21, 80, 282}, {side * 70 + 21, 94, 282}, {side * 70 - 21, 94, 282}, paint (0xFFE3AA));
			quad (*body, {side * 75 - 20, 86, -282}, {side * 75 + 20, 86, -282}, {side * 75 + 20, 98, -282}, {side * 75 - 20, 98, -282}, paint (0xF33A50));
		}
		quad (*body, {-48, 55, 283}, {48, 55, 283}, {48, 77, 283}, {-48, 77, 283}, trim);
		add (body);
		const Mat rubber = paint (0x0C141E), rim = paint (0x839BAB);
		for (int side : {-1, 1})
			for (int z : {-170, 170})
			{
				Thing* w = thing ();
				for (int i = 0; i < 6; i++)
				{
					const float p = i * 2 * PI / 6, q = (i + 1) * 2 * PI / 6;
					triangle (*w, {side * 20, 0, 0}, {side * 20, (int) (48 * std::cos (p)), (int) (48 * std::sin (p))},
						  {side * 20, (int) (48 * std::cos (q)), (int) (48 * std::sin (q))}, rubber);
				}
				quad (*w, {side * 21, -21, -21}, {side * 21, 21, -21}, {side * 21, 21, 21}, {side * 21, -21, 21}, rim);
				for (int end : {-1, 1})
					quad (*w, {-18, -24, end * 42}, {18, -24, end * 42}, {18, 24, end * 42}, {-18, 24, end * 42}, rubber);
				add (w, {side * 126, 50, z}, side < 0 ? 2 : 3);
			}
		if (police)
		{
			for (int side : {-1, 1})
			{
				Thing* door = thing ();
				quad (*door, {side * 125, 70, -112}, {side * 125, 70, 72}, {side * 119, 110, 72}, {side * 119, 110, -112}, trim);
				add (door);
				add (panel ({side * 127, 77, -38}, {side * 127, 77, 0}, {side * 124, 100, 0}, {side * 124, 100, -38}, paint (0xA9DCDA)));
			}
			add (box (-45, roof + 14, -40, 80, 17, 37, paint (0xFF294B)), {-45, roof + 14, -40}, 5);
			add (box (45, roof + 14, -40, 80, 17, 37, paint (0x289CFF)), {45, roof + 14, -40}, 6);
		}
	}

	void build (bool police = false, bool lite = false, bool transformable = false, int style = 0)
	{
		parts.clear ();
		glass_points.clear ();
		glass_normals.clear ();
		Mat wheel_glow = paint (0x54EEFF, 255, true);
		if (lite)
			traffic_body (police, style);
		else
			hero_body (transformable, wheel_glow);
		add (panel ({-116, 3, -255}, {116, 3, -255}, {116, 3, 255}, {-116, 3, 255}, paint (0x070E1A)), {}, 11);
		settle ();				// (to the RPi, car by car)
	}

	void pose (P pos, float yaw, float hover = 0, float fire = 0, float blink = 0, float pitch = 0, float lean = 0)
	{
		position = pos;
		heading = yaw;
		body_pitch = (int) std::round (pitch);
		body_roll = (int) std::round (lean);
		suspension = hover == 0 && (body_pitch || body_roll);
		const int glow_alpha = (int) std::min (255.0f, hover * 190 + fire * 65);
		for (auto& p : parts)
		{
			Thing* o = p.thing;
			P local = p.local;
			int roll = 0, kind = p.kind;
			o->enabled = true;
			if (kind >= 14)				// a wheel's ring of light: as its wheel
			{
				kind -= 12;
				o->alpha = (glow_alpha + 1) / 256.0f;
				o->enabled = glow_alpha > 0;
			}
			// (the wheels' outer faces turn down, mirrored on the two sides)
			if (kind == 2 || kind == 3)
			{
				roll = (int) ((kind == 2 ? 90 : -90) * hover);
				local.x += (int) ((kind == 2 ? -28 : 28) * hover);
				local.y += (int) (10 * hover);
			}
			if (kind == 5 || kind == 6)
				o->enabled = ((int) (blink * 8) % 2) == (kind == 5 ? 0 : 1);
			const bool is_wheel = kind == 2 || kind == 3;
			o->position = is_wheel && suspension ? pos + yawed (local, yaw) : world_point (local);
			const P r = is_wheel ? (suspension ? P {(int) wheel_phase, (int) yaw, 0} : wheel_rotation ((int) wheel_phase, roll))
					     : (body_roll ? wheel_rotation (0, 0) : P {-body_pitch, (int) yaw, 0});
			o->rotation[0] = r.x;
			o->rotation[1] = r.y;
			o->rotation[2] = r.z;
			if (kind == 11)				// its shadow, on the road
			{
				o->position = {pos.x, 0, pos.z};
				o->rotation[0] = o->rotation[2] = 0;
				o->rotation[1] = (int) yaw;
				o->enabled = pos.y < 500;
			}
			if (kind == 1)				// the glass: where the room's picture is seen in it
			{
				int anchor = 0;
				for (size_t i = 0; i < o->mesh.vertices.size (); i++)
				{
					const P at = world_point (glass_points[i]), n = direction (glass_normals[i]);
					float nx = (float) n.x, ny = (float) n.y, nz = (float) n.z;
					float vx = camera.x - at.x, vy = camera.y - at.y, vz = camera.z - at.z;
					const float nn = std::sqrt (nx * nx + ny * ny + nz * nz), vv = std::sqrt (vx * vx + vy * vy + vz * vz);
					int tu = 512, tv = 512;
					if (nn > 1e-6f && vv > 1e-6f)
					{
						nx /= nn; ny /= nn; nz /= nn;
						vx /= vv; vy /= vv; vz /= vv;
						const float twice = 2 * (nx * vx + ny * vy + nz * vz);
						const float rx = twice * nx - vx, ry = twice * ny - vy, rz = twice * nz - vz;
						tu = (int) std::lround ((0.5f + std::atan2 (rx, rz) / (2 * PI)) * 1024);
						tv = std::min (std::max ((int) std::lround ((0.5f - std::asin (std::min (std::max (ry, -1.0f), 1.0f)) / PI) * 1024), 0), 1023);
					}
					if (i % 4 == 0)
						anchor = tu;
					while (tu - anchor > 512)
						tu -= 1024;
					while (tu - anchor < -512)
						tu += 1024;
					o->mesh.vertices[i].u = (int16_t) tu;
					o->mesh.vertices[i].v = (int16_t) tv;
				}
			}
		}
	}
	void hide ()
	{
		for (auto& p : parts)
			p.thing->enabled = false;
	}
};
Vehicle hero, police[2], traffic[6];

// ---- the cockpit -----------------------------------------------------------------------------------

void speed (int value)				// the speedometer: two seven-segment digits, into its place in the atlas
{
	if (value == displayed_speed)
		return;
	displayed_speed = value;
	std::vector<uint16_t> pixels (128 * 64, rgb (0x091420));
	static const int masks[] = {0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f};
	static const int rects[7][4] = {{5, 46, 32, 5}, {37, 26, 5, 24}, {37, 3, 5, 22}, {5, 0, 32, 5}, {0, 3, 5, 22}, {0, 26, 5, 24}, {5, 23, 32, 5}};
	for (int d = 0; d < 2; d++)
		for (int i = 0; i < 7; i++)
		{
			const int* r = rects[i];
			const int digit = d ? value % 10 : value / 10;
			const uint16_t c = rgb (masks[digit] & (1 << i) ? 0xFF7343 : 0x241B21);
			for (int y = 0; y < r[3]; y++)
				for (int x = 0; x < r[2]; x++)
					pixels[(6 + r[1] + y) * 128 + 8 + d * 56 + r[0] + x] = c;
		}
	tile_pixels (SPEED, pixels.data ());
}

void cockpit ()
{
	const Mat dark = paint (0x101B2C), trim = paint (0x344357), cyan = paint (0x399CBA);
	// one shallow dashboard; its two screens are parts of its face
	auto face = [] (int x0, int y0, int x1, int y1, const Mat& m) { return panel ({x0, y0, 100}, {x1, y0, 100}, {x1, y1, 100}, {x0, y1, 100}, m); };
	face (-128, 38, 128, 51, dark);
	face (-128, 93, 128, 100, trim);
	face (-128, 51, -117, 93, trim);
	face (-19, 51, 1, 93, trim);
	face (117, 51, 128, 93, trim);
	face (-117, 51, -19, 54, dark);
	face (-117, 90, -19, 93, dark);
	face (1, 51, 117, 54, dark);
	face (1, 90, 117, 93, dark);
	displayed_speed = -1;
	face (-117, 54, -19, 90, textured (SPEED));
	face (1, 54, 117, 90, textured (DASH));
	panel ({-128, 100, 100}, {128, 100, 100}, {119, 105, 137}, {-119, 105, 137}, dark);
	// a scanner: a light running to and fro over its fourteen segments (the original paints them over
	// the dashboard's face, in its plane; with a depth buffer: just before it)
	scanner = thing ();
	scanner->changes = true;
	for (int i = 0; i < 14; i++)
	{
		const int x0 = -111 + i * 16, x1 = -99 + i * 16;
		quad (*scanner, {x0, 96, 99}, {x1, 96, 99}, {x1, 98, 99}, {x0, 98, 99}, paint (0x8A1733));
	}
	if (shot == 4)
	{
		const Mat roof = paint (0x101928);
		panel ({-156, 183, -230}, {156, 183, -230}, {137, 175, -25}, {-137, 175, -25}, roof);
		panel ({-137, 175, -25}, {137, 175, -25}, {111, 167, -12}, {-111, 167, -12}, trim);
		for (int side : {-1, 1})
		{
			panel ({side * 106, 167, -12}, {side * 121, 171, -12}, {side * 141, 87, 115}, {side * 128, 94, 115}, trim);
			panel ({side * 105, 167, -11}, {side * 108, 168, -11}, {side * 130, 94, 114}, {side * 128, 94, 114}, cyan);
			panel ({side * 150, 0, -220}, {side * 150, 0, 115}, {side * 131, 84, 115}, {side * 131, 84, -220}, roof);
			panel ({side * 150, 80, -220}, {side * 150, 80, 115}, {side * 131, 87, 115}, {side * 131, 87, -220}, trim);
			panel ({side * 131, 87, -200}, {side * 131, 87, 100}, {side * 131, 89, 100}, {side * 131, 89, -200}, cyan);
		}
	}
	panel ({-97, 111, 121}, {97, 111, 121}, {108, 73, 285}, {-108, 73, 285}, paint (0x647DAA));	// the bonnet
}

// ---- a cut's set -----------------------------------------------------------------------------------

void relocate (size_t first, P origin, float yaw)
{
	for (size_t i = first; i < things.size (); i++)
	{
		Thing* o = things[i].get ();
		o->position = origin + yawed (o->position, yaw);
		o->rotation[1] += (int) yaw;
	}
	for (auto& g : glows)
		g.position = origin + yawed (g.position, yaw);
}

float shot_lens (int cut, float local)		// the lens: three cuts pull it
{
	if (cut == 5)
		return 62 - 16 * clamp01 (local / 5.0f);
	if (cut == 9)
		return 54 + 16 * clamp01 (local / 5.0f);
	if (cut == 10)
		return 34 + 34 * clamp01 (local / 2.0f);
	static const float lenses[] = {62, 68, 62, 74, 74, 48, 62, 62, 56, 64, 68, 62};
	return lenses[cut];
}

// a vertex's place in the atlas: its picture's corner and size, half a texel in from the edges
void place_pictures (Thing& t, kit::Mesh& mesh, size_t first_tile)
{
	for (size_t i = 0; i < mesh.vertices.size (); i++)
	{
		kit::Vertex& v = mesh.vertices[i];
		const int tile = t.texture ? PLAIN : t.tiles[first_tile + i];
		if (t.texture)
			continue;
		if (tile == PLAIN)
		{
			v.pad = 1;
			continue;
		}
		v.u = (int16_t) std::lround ((TILE[tile].x + 0.5f + v.u * (TILE[tile].w - 1) / 1024.0f) * 1024 / ATLAS);
		v.v = (int16_t) std::lround ((TILE[tile].y + 0.5f + v.v * (TILE[tile].h - 1) / 1024.0f) * 1024 / ATLAS);
	}
}

void load (int which)
{
	credits = false;
	road.clear ();
	searchlights.clear ();
	holograms.clear ();
	hero.parts.clear ();
	for (auto& c : police)
		c.parts.clear ();
	for (auto& c : traffic)
		c.parts.clear ();
	scrolling.clear ();
	approach_glows.clear ();
	scanner = nullptr;
	glows.clear ();
	has_water = false;
	hero_glow = -1;
	for (auto& p : pool)
		p.active = false;
	things.clear ();
	settled = 0;
	water_range = Range ();
	for (int cls = 0; cls < CLASSES; cls++)
		arena_open (cls);
	shot = which;
	const bool in_street = which >= 1 && which <= 9;
	camera.fov = shot_lens (which, 0);
	camera.near_plane = 40;
	camera.far_plane = in_street ? 9000 : 15000;
	for (int y = 0; y < kit::H; y++)
		sky[y] = rgb (((10 + y * 15 / kit::H) << 16) | ((15 + y * 22 / kit::H) << 8) | (34 + y * 40 / kit::H));
	if (shot == 0 || shot == 11)
	{
		skyline ();
		credits = shot == 11;
	}
	else if (shot == 1)
	{
		street (320, 14, true, 7);
	}
	else if (shot == 2 || shot == 3)
	{
		street (320, 16, false);
		road.capture (11520);
		hero.build ();
	}
	else if (shot == 4 || shot == 5)
	{
		if (shot == 4)
		{
			street (320, 16, false);
			road.capture (11520);
		}
		cockpit ();
	}
	else if (shot == 6)
	{
		street (320, 15, false);
		relocate (0, {0, 0, -8500}, 0);
		// the city goes on behind the junction
		for (int i = 0; i < 3; i++)
			building (-1300 - i * 1200, 3300, 850, 820, 1900 + i * 210, i, false);
		for (int i = 0; i < 2; i++)
			building (-1700 - i * 1300, 750, 850, 800, 1750 + i * 240, i + 2, false);
		for (auto& o : things)
			scrolling.push_back ({o.get (), o->position});
		for (auto& g : glows)
			approach_glows.push_back (g.position);
		const size_t first = things.size (), glow_first = glows.size ();
		boulevard (16);
		for (size_t i = first; i < things.size (); i++)
		{
			Thing* o = things[i].get ();
			o->position = P {1900, 0, 2100} + yawed (o->position, 90);
			o->rotation[1] += 90;
		}
		for (size_t i = glow_first; i < glows.size (); i++)
			glows[i].position = P {1900, 0, 2100} + yawed (glows[i].position, 90);
		road.capture (11520, first, glow_first, true);
		hero.build ();
		for (int i = 0; i < 6; i++)
			traffic[i].build (false, true, false, i);
		for (auto& c : police)
			c.build (true, true);
	}
	else if (shot == 7)
	{
		boulevard (16);
		road.capture (11520);
		hero.build ();
		for (int i = 0; i < 6; i++)
			traffic[i].build (false, true, false, i);
		for (auto& c : police)
			c.build (true, true);
	}
	else if (shot == 8 || shot == 9)
	{
		boulevard (16);
		road.capture (11520);
		hero.build (false, false, true);
		for (auto& c : police)
			c.build (true, true);
	}
	else
	{
		city_grid ();
		relocate (0, {0, 0, -3400}, 20);
		hero.build (false, false, true);
	}
	if (!hero.parts.empty ())
	{
		hero_glow = (int) glows.size ();
		for (int i = 0; i < 6; i++)
			glow ({0, 0, 0}, i < 2 ? 1 : 2);
	}
	// to the RPi: the pictures' places worked out, what stands still for good
	settle ();
	unsigned corners = 0;
	for (int used : arena_used)
		corners += (unsigned) used;
	std::printf ("FILM CUT %02d / %s: %u things, %u triangles\n", shot + 1, NAMES[shot], (unsigned) things.size (), corners / 3);
}

// the things made so far, to the RPi: their boxes measured, their pictures' places worked out
void settle ()
{
	for (; settled < things.size (); settled++)
	{
		Thing& t = *things[settled];
		measure (t);
		place_pictures (t, t.mesh, 0);
		place_pictures (t, t.low, t.mesh.vertices.size ());
		t.cls =   t.changes ? OWN : t.background ? (t.blend != kit::OPAQUE ? GROUND_SHEER : GROUND) : t.gloss ? GLOSSY
			: t.blend != kit::OPAQUE ? SHEER : t.billboard ? BILLBOARD : t.reflected ? MIRRORED
			: !t.low.vertices.empty () ? DETAIL : t.closed ? CLOSED : SOLID;
		t.tube = into_arena (TUBES, t.tubes);
		if (t.changes)
		{
			std::vector<int8_t> ().swap (t.tiles);
			continue;
		}
		t.near = into_arena (t.cls, t.mesh);
		if (t.water)
		{
			if (!water_range.count)
				water_range.first = t.near.first;
			water_range.count = t.near.first + t.near.count - water_range.first;
		}
		t.far = into_arena (FAR, t.low);
		t.release ();
	}
}

// ---- weather ---------------------------------------------------------------------------------------

// splashes in the street (rain) or thrown up behind the hero's wheels (spray): a pool set anew each frame
void weather (float t, bool debris, P origin = {0, 0, 0})
{
	for (auto& p : pool)
		p.active = false;
	for (int i = 0; i < (debris ? 26 : 34); i++)
	{
		Particle& p = pool[i];
		const float a = std::fmod (t * (debris ? 1.8f : 2.6f) + i * 0.618f, 1.0f);
		p.active = true;
		p.max_life = debris ? 1 : 0.22f;
		p.life = 0.22f * (1 - a);
		if (debris)
		{
			const P local = yawed ({(int) ((i % 2 ? 1 : -1) * (110 + a * 85)), (int) (8 + std::sin (a * PI) * 30), (int) (-250 - a * 700)}, hero.heading);
			const P velocity = yawed ({(i % 2 ? 1 : -1) * 35, 40, -520}, hero.heading);
			p.pos[0] = (float) (origin.x + local.x);
			p.pos[1] = (float) (origin.y + local.y);
			p.pos[2] = (float) (origin.z + local.z);
			p.vel[0] = (float) velocity.x;
			p.vel[1] = (float) velocity.y;
			p.vel[2] = (float) velocity.z;
		}
		else
		{
			const uint32_t cycle = (uint32_t) std::floor (t * 2.6f + i * 0.618f), seed = (uint32_t) i * 0x9e3779b9u + cycle * 0x85ebca6bu;
			p.pos[0] = (float) ((int) (weather_hash (seed) % 520) - 260);
			p.pos[1] = 3 + std::sin (a * PI) * 10;
			p.pos[2] = (float) ((int) camera.z + 140 + (int) (weather_hash (seed ^ 0xa3c59ac3u) % 1200));
			p.vel[0] = i % 2 ? 55.0f : -55.0f;
			p.vel[1] = 150;
			p.vel[2] = 0;
		}
	}
}

// what is drawn flat on the screen: a mesh a little way before the eye, in sixteenths of a unit. A corner
// is written as it is (no normal to work out: hundreds of them a frame)
constexpr float FLAT_DEPTH = 100, FLAT_SCALE = 16;

void flat_corner (kit::Mesh& mesh, float x, float y, const kit::Vertex& like)
{
	kit::Vertex v = like;
	const float k = FLAT_DEPTH * FLAT_SCALE / focal;
	v.x = (int16_t) ((x - kit::W / 2) * k);
	v.y = (int16_t) ((kit::H / 2 - y) * k);
	v.z = (int16_t) (FLAT_DEPTH * FLAT_SCALE);
	mesh.vertices.push_back (v);
}

kit::Vertex flat_paint (uint16_t color, int alpha)
{
	kit::Vertex v;
	std::memset (&v, 0, sizeof v);
	const kit::Rgb c = kit::rgb565 (color);
	v.r = c.r;
	v.g = c.g;
	v.b = c.b;
	v.a = (uint8_t) alpha;
	v.nz = -32767;
	return v;
}

// the splashes as streaks (Jet's particle system: a triangle along each one's velocity), and the rain: over the picture
void draw_weather ()
{
	spray.clear ();
	for (const auto& p : pool)
	{
		if (!p.active)
			continue;
		const float dx = p.pos[0] - camera.x, dy = p.pos[1] - camera.y, dz = p.pos[2] - camera.z;
		if (dx * dx + dy * dy + dz * dz > 1800.0f * 1800.0f)
			continue;
		const float age = 1.0f - p.life / p.max_life;
		int alpha = 255;
		if (age > 0.65f)
			alpha = std::min (255, std::max (0, (int) (255.0f * (1.0f - (age - 0.65f) / 0.35f))));
		if (alpha < 4)
			continue;
		uint16_t color = 0xEFFF;
		if (age >= 0.35f)
		{
			const float t = std::min (1.0f, (age - 0.35f) / 0.65f);
			color = (uint16_t) ((unsigned) (29.0f * (1.0f - t) + 11.0f * t + 0.5f) << 11 | (unsigned) (63.0f * (1.0f - t) + 46.0f * t + 0.5f) << 5 | 31u);
		}
		float bx, by, bz, tx, ty, tz;
		if (!camera.project (p.pos[0], p.pos[1], p.pos[2], &bx, &by, &bz) || bz <= camera.near_plane || bz >= camera.far_plane)
			continue;
		const float speed = std::sqrt (p.vel[0] * p.vel[0] + p.vel[1] * p.vel[1] + p.vel[2] * p.vel[2]);
		const float streak = speed > 1.0f ? speed * 0.03f : 2.4f;
		if (!camera.project (p.pos[0] + p.vel[0] / speed * streak, p.pos[1] + p.vel[1] / speed * streak, p.pos[2] + p.vel[2] / speed * streak, &tx, &ty, &tz))
			continue;
		const float ddx = tx - bx, ddy = ty - by, length = std::sqrt (ddx * ddx + ddy * ddy);
		const float px = length < 0.5f ? 3.0f : -ddy * 3 / length, py = length < 0.5f ? 0.0f : ddx * 3 / length;
		const kit::Vertex m = flat_paint (color, alpha);
		flat_corner (spray, tx, ty, m);
		flat_corner (spray, bx + px, by + py, m);
		flat_corner (spray, bx - px, by - py, m);
	}
	kit::Draw how;
	how.view_space = true;
	how.model = kit::scaling (1 / FLAT_SCALE, 1 / FLAT_SCALE, 1 / FLAT_SCALE);
	how.depth_test = false;
	how.blend = kit::ALPHA;
	if (!spray.vertices.empty ())
	{
		spray.upload (GL_DYNAMIC_DRAW);
		kit::draw (spray, how);
	}
	if (shot == 4 || shot == 5)
		return;
	// rain: fine fast drops in a volume about the camera, falling along the world's down
	rain.clear ();
	const kit::Vertex drop = flat_paint (rgb (0x91B4CC), 125);
	const float f = focal;
	rain.vertices.reserve (660);
	// (the fall, in the camera's space: where a point 3, -54, 0 from the eye is seen from)
	const kit::Mat4 view = camera.view ();
	const float fall[3] = {(float) (int) (view.m[0] * 3 + view.m[4] * -54), (float) (int) (view.m[1] * 3 + view.m[5] * -54), (float) (int) (view.m[2] * 3 + view.m[6] * -54)};
	for (int i = 0; i < 220; i++)
	{
		const float phase = shot_time * 4.5f + i * 0.6180339f, age = phase - std::floor (phase);
		const uint32_t seed = (uint32_t) i * 0x9e3779b9u + (uint32_t) std::floor (phase) * 0x85ebca6bu;
		const float z = (float) (200 + weather_hash (seed) % 1800);
		const float px = (float) (weather_hash (seed ^ 0xc2b2ae35u) % (uint32_t) (kit::W + 80)) - kit::W / 2 - 40;
		const float py = (float) (weather_hash (seed ^ 0x27d4eb2fu) % (uint32_t) (kit::H + 80)) - kit::H / 2 - 40;
		const float travel = (age - 0.5f) * 1200 / 54;
		const float a[3] = {(float) (int) (px * z / f + fall[0] * travel), (float) (int) (py * z / f + fall[1] * travel), (float) (int) (z + fall[2] * travel)};
		const float b[3] = {a[0] + fall[0], a[1] + fall[1], a[2] + fall[2]};
		if (a[2] <= 80 || b[2] <= 80)
			continue;
		const float x = kit::W / 2 + (float) (int) (a[0] * f / a[2]), y = kit::H / 2 - (float) (int) (a[1] * f / a[2]);
		const float tx = kit::W / 2 + (float) (int) (b[0] * f / b[2]), ty = kit::H / 2 - (float) (int) (b[1] * f / b[2]);
		flat_corner (rain, x, y, drop);
		flat_corner (rain, x + 2, y, drop);
		flat_corner (rain, tx, ty, drop);
	}
	if (!rain.vertices.empty ())
	{
		rain.upload (GL_DYNAMIC_DRAW);
		kit::draw (rain, how);
	}
}

// ---- a cut's motion --------------------------------------------------------------------------------

void set_camera (P p)
{
	camera.x = (float) p.x;
	camera.y = (float) p.y;
	camera.z = (float) p.z;
}

void look_at (P p)
{
	camera.look_at ((float) p.x, (float) p.y, (float) p.z);
}

void pose (float p)
{
	P car = {0, 0, 0};
	float yaw = 0;
	const float t = shot_time;
	camera.fov = shot_lens (shot, t);
	float wheel_travel = t * (shot < 6 ? ROAD_SPEED : CHASE_SPEED);
	if (shot == 6)
		wheel_travel = approach_travel (std::min (t, TURN_START)) + TURN_RADIUS * PI / 2 * clamp01 ((t - TURN_START) / (TURN_END - TURN_START))
			       + exit_travel (std::max (0.0f, t - TURN_END));
	if (shot == 8)
	{
		const float braking = std::min (6.0f, std::max (0.0f, t - 2));
		wheel_travel = CHASE_SPEED * (std::min (t, 2.0f) + braking - braking * braking / 12);
	}
	if (shot >= 9)
		wheel_travel = CHASE_SPEED * 5;
	wheel_phase = std::fmod (wheel_travel * 180 / (44 * PI), 360.0f);
	if (shot == 0 || shot == 11)
	{
		set_camera (lerp ({2300, 410, -1900}, {-1100, 460, -2300}, p));
		look_at (lerp ({-300, 700, 2400}, {600, 760, 2400}, p));
	}
	if (shot == 1)
	{
		set_camera (lerp ({-175, 270, -900}, {-140, 65, -150}, p));
		look_at (lerp ({130, 165, 900}, {170, 70, 1500}, p));
		weather (t, false);
	}
	if (shot == 2)
	{
		road.advance (t * ROAD_SPEED);
		set_camera (lerp ({140, 660, -590}, {120, 600, -530}, p));
		look_at ({-25, 65, -45});
		hero.pose (car, 0);
		weather (t, true, car);
	}
	if (shot == 3)
	{
		road.advance (t * ROAD_SPEED, true);
		car = {45, 0, 0};
		const float a = (-30 + 12 * p) * PI / 180;
		set_camera (car + P {(int) (520 * std::sin (a)), (int) (145 + 20 * p), (int) (520 * std::cos (a))});
		look_at (car + lerp ({35, 95, -20}, {20, 100, -35}, p));
		hero.pose (car, 0);
		weather (t, true, car);
	}
	if (shot == 4 || shot == 5)
	{
		if (shot == 4)
		{
			road.advance (t * ROAD_SPEED);
			set_camera (lerp ({-45, 133, -150}, {-35, 131, -135}, p));
			look_at (lerp ({35, 115, 560}, {65, 114, 620}, p));
			speed (70);
		}
		else
		{
			set_camera (lerp ({-88, 79, -24}, {-82, 77, -6}, p));
			look_at ({-72, 73, 100});
			speed (70 + (int) (18 * clamp01 (t / 5.0f) + 0.5f));
		}
		for (int i = 0; i < 14; i++)
		{
			float cursor = std::fmod (t * 9, 26.0f);
			if (cursor > 13)
				cursor = 26 - cursor;
			const Mat lit = paint (std::fabs ((float) i - cursor) < 1.6f ? 0xFF403C : 0x4A1727);
			scanner->mesh.paint (i * 4, i * 4 + 4, material (lit));
		}
	}
	if (shot == 6)
	{
		const float q = std::max (0.0f, t - TURN_END), travel = exit_travel (q);
		float pitch = 0, lean = 0;
		const float rebase = t >= EXIT_TRACKING_START ? travel : 0;
		const int road_travel = (int) rebase;
		constexpr float traffic_speed = CHASE_SPEED - 1100;
		for (auto& o : scrolling)
			o.first->position = o.second + P {-road_travel, 0, 0};
		for (size_t i = 0; i < approach_glows.size (); i++)
			glows[i].position = approach_glows[i] + P {-road_travel, 0, 0};
		if (t < EXIT_TRACKING_START)
			road.restore ();
		else
			road.advance (travel, true);
		if (t < TURN_START)
		{
			car = {0, 0, 1450 + (int) (approach_travel (t) - approach_travel (TURN_START))};
			pitch = -6 * clamp01 ((t - BRAKE_START) / 0.18f) * (1 - clamp01 ((t - 1.65f) / 0.6f));
			// the braking car comes towards a camera that backs away slower
			const float start_z = 1450 + approach_travel (0) - approach_travel (TURN_START);
			set_camera (lerp ({-175, 240, (int) start_z + 2800}, {-175, 360, 3100}, t / TURN_START));
		}
		else if (t < TURN_END)
		{
			const float u = (t - TURN_START) / (TURN_END - TURN_START), a = u * PI / 2;
			car = {(int) (TURN_RADIUS * (1 - std::cos (a))), 0, 1450 + (int) (TURN_RADIUS * std::sin (a))};
			yaw = u * 90 + 42 * std::sin (u * PI);
			pitch = -3 * (1 - u);
			lean = 5 * std::sin (u * PI);
			// low, across the junction, as the rear steps out
			set_camera (lerp ({880, 160, 2590}, {1860, 270, 2590}, u));
		}
		else
		{
			const float overtaking = std::max (0.0f, (travel - traffic_speed * q) / 1100);
			const LaneChange lane = {-380, 0, 2.2f, 1.0f};
			car = {(int) (TURN_RADIUS + travel - rebase), 0, 2100 - (int) lane_position (overtaking, lane)};
			const float lateral = lane_velocity (overtaking, lane) * std::max (0.0f, (exit_speed (q) - traffic_speed) / 1100);
			yaw = 90 + steering_yaw (lateral, exit_speed (q));
			pitch = 3 * std::sin (PI * clamp01 (q / EXIT_ACCELERATION_SECONDS));
			lean = -2 * std::sin (PI * clamp01 (q / 0.7f));
			if (t < EXIT_TRACKING_START)
				set_camera ({1860 + (int) (40 * q), 270 + (int) (12 * q), 2590 + (int) (15 * q)});
			else
				set_camera ({(int) TURN_RADIUS + 800, 260, 2540});
		}
		// the skid's camera keeps looking down the street as the car passes below; then the cut to the tracking one
		if (t >= TURN_END && t < EXIT_TRACKING_START)
			look_at ({(int) TURN_RADIUS, 75, 2480});
		else
			look_at (car + (t < TURN_START ? P {0, 80, 0} : t < TURN_END ? P {0, 75, 0} : P {100, 100, -70}));
		hero.pose (car, yaw, 0, 0, 0, pitch, lean);
		static const int lanes[] = {0, -380, 380, 380, -380, -380};
		const float traffic_travel = traffic_speed * (t - TURN_END) - rebase;
		for (int i = 0; i < 6; i++)
			traffic[i].pose ({(int) TURN_RADIUS + 6000 + i * 1800 + (int) traffic_travel, 0, 2100 - lanes[i]}, 90);
		// flashing cars far behind, closing in before the pursuit's cut
		const float catchup = clamp01 ((t - EXIT_TRACKING_START) / (DURATIONS[6] - EXIT_TRACKING_START));
		for (int i = 0; i < 2; i++)
		{
			const float gap = (i ? 6500.0f : 5200.0f) + (i ? -4100.0f : -3600.0f) * catchup;
			police[i].pose ({(int) (TURN_RADIUS - gap), 0, 2480 + (i ? 0 : 40)}, 90, 0, 0, t);
			if (t < EXIT_TRACKING_START)
				police[i].hide ();
		}
		weather (t, true, car);
	}
	if (shot == 7)
	{
		road.advance (CHASE_SPEED * t, t < 2 || (t >= 4 && t < 7));
		const LaneChange lane = chase_lane (t);
		car = {(int) lane_position (t, lane), 0, 0};
		yaw = steering_yaw (lane_velocity (t, lane), CHASE_SPEED);
		// hard cuts between tracking cameras
		if (t < 2)
			set_camera (lerp ({440, 460, -3500}, {400, 440, -3000}, t / 2));
		else if (t < 4)
			set_camera (lerp ({240, 280, -850}, {280, 265, -720}, (t - 2) / 2));
		else if (t < 7)
			set_camera (lerp ({-515, 245, 700}, {-475, 235, 590}, (t - 4) / 3));
		else if (t < 10)
			set_camera (lerp ({-555, 250, -580}, {-525, 270, -500}, (t - 7) / 3));
		else
			set_camera (lerp ({500, 850, -450}, {425, 1100, -250}, (t - 10) / 4));
		look_at (car + (t < 2 ? P {0, 80, -500} : t < 4 ? P {-65, 105, 150} : t < 7 ? P {50, 100, -80} : t < 10 ? P {40, 95, 90} : P {-60, 60, 100}));
		hero.pose (car, yaw);
		static const int starts[] = {2300, 6900, 11500, 5000, 9500, 15100}, lanes[] = {-380, 0, 380, 380, -380, -380};
		for (int i = 0; i < 6; i++)
			traffic[i].pose ({lanes[i], 0, starts[i] - (int) (1150 * t)}, 0);
		for (int i = 0; i < 2; i++)
		{
			const float delayed = t - (i ? 1.6f : 0.85f);
			const LaneChange chase = chase_lane (delayed);
			police[i].pose ({(int) lane_position (delayed, chase) + (i ? 0 : -40), 0, (int) (i ? -2400 + 650 * clamp01 (t / 4) : -1600 + 700 * clamp01 (t / 4))},
					steering_yaw (lane_velocity (delayed, chase), CHASE_SPEED), 0, 0, t);
		}
		weather (t, true, car);
	}
	if (shot == 8)
	{
		road.advance (t * CHASE_SPEED);
		car = {0, (int) (55 * clamp01 ((t - 2) / 6)), 0};
		const float h = clamp01 ((t - 2) / 6);
		set_camera (lerp ({-510, 145, -650}, {-570, 165, -570}, p));
		look_at (car + P {50, 90, 20});
		hero.pose (car, 0, h);
		for (int i = 0; i < 2; i++)
			police[i].pose ({i ? 380 : -380, 0, -1000 - i * 500}, 0, 0, 0, t);
	}
	if (shot == 9)				// the car leaves the street; the police stay where they were
	{
		const float travel = CHASE_SPEED * t;
		road.advance (travel, true);
		car = {0, (int) launch_height (t), 0};
		set_camera ({-480, 260 + (int) (580 * t), 1000});
		look_at (car + P {40, 60, 0});
		hero.pose (car, 0, 1, 1, 0, launch_pitch (t));
		for (int i = 0; i < 2; i++)
			police[i].pose ({i ? 380 : -380, 0, -1000 - i * 500 - (int) travel}, 0, 0, 0, 0.1f);
	}
	if (shot == 10)				// over the roofs: the camera waits ahead of the flight
	{
		const float start_y = launch_height (DURATIONS[9]) - 1500;
		car = {0, (int) (start_y + CLIMB_SPEED * t), (int) (CHASE_SPEED * (t - 2))};
		set_camera ({260, (int) (start_y + CLIMB_SPEED * 2), 0});
		look_at (lerp ({0, (int) start_y, (int) (-CHASE_SPEED * 2)}, {0, 0, -2000}, p));
		hero.pose (car, 0, 1, 1, 0, launch_pitch (DURATIONS[9]));
	}
	for (auto& b : searchlights)
	{
		b.mesh->rotation[2] = (int) (36 * std::sin (t * 0.82f + b.phase));
		b.mesh->rotation[1] = (int) (48 * std::sin (t * 0.57f + b.phase));
	}
	for (size_t i = 0; i < holograms.size (); i++)
		holograms[i]->position.y = 1000 + (int) i * 110 + (int) (24 * std::sin (t * 0.7f + i));
	if (hero_glow >= 0)			// the lamps' lights, the hover pods'
	{
		glows[hero_glow].position = hero.world_point ({-78, 57, 290});
		glows[hero_glow + 1].position = hero.world_point ({78, 57, 290});
		for (int i = 0; i < 4; i++)
			glows[hero_glow + 2 + i].position = hero.world_point ({i % 2 ? 150 : -150, 35, i < 2 ? -175 : 175});
	}
}

void seek (float absolute)
{
	seconds_in = absolute;
	int next = 0;
	float local = absolute;
	while (next < CUTS - 1 && local >= DURATIONS[next])
	{
		local -= DURATIONS[next];
		next++;
	}
	shot_time = std::min (local, DURATIONS[next]);
	if (next != shot)
		load (next);
	pose (clamp01 (shot_time / DURATIONS[shot]));
}

void update (float seconds)
{
	seconds_in += seconds;
	if (seconds_in >= DURATION + 1)		// (the original restarts the board here)
		seconds_in = 0;
	seek (std::min (seconds_in, DURATION));
}

void init ()
{
	light.on = light.has_ambient = true;
	light.azimuth = 225;
	light.elevation = 40;
	light.color = {180, 220, 255};
	light.intensity = 255;
	light.ambient = {115, 104, 155};
	make_textures ();
	neon_program = kit::program (&kit_neon_info, sizeof kit_neon_info);
	neon_lens = glGetUniformLocation (neon_program.name, "u_lens");
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
	seek (0);
}

// ---- the picture -----------------------------------------------------------------------------------

void draw_water (GLuint picture)
{
	const float f = (kit::W / 2) / std::tan (camera.fov * PI / 360);
	const int horizon = kit::H / 2 + (int) (std::sin (-camera.pitch * PI / 180) * f);
	float sx, shore;
	camera.project (0, 0, 1900, &sx, &shore);
	const int bias = std::min (std::max ((int) (2 * (shore - horizon)), 0), 255);
	int ax, ay, aw, ah;
	kit::area (&ax, &ay, &aw, &ah);
	const kit::Rgb c = kit::rgb565 (water_paint.color);
	kit::own_gl_begin ();
	glUseProgram (water_program);
	glUniformMatrix4fv (w_mvp, 1, GL_FALSE, kit::view_projection ().m);
	glUniform4f (w_area, (float) ax, (float) (ay + ah), 1.0f / aw, (float) kit::H / ah);
	glUniform4f (w_water, (float) horizon, (float) bias, (float) (int) shore, std::fmod ((float) (int) (shot_time * 240.0f), 360.0f));
	glUniform4f (w_color, c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, 100 / 255.0f);
	glActiveTexture (GL_TEXTURE1);
	glBindTexture (GL_TEXTURE_2D, kit::gradient ());
	glActiveTexture (GL_TEXTURE0);
	glBindTexture (GL_TEXTURE_2D, picture);
	glDisable (GL_BLEND);
	glDisable (GL_CULL_FACE);
	glDisable (GL_DEPTH_TEST);
	glBindBuffer (GL_ARRAY_BUFFER, arena[GROUND_SHEER].buffer);
	for (GLint i = 0; i < 8; i++)
	{
		if (i == w_pos)
			glEnableVertexAttribArray (i);
		else
			glDisableVertexAttribArray (i);
	}
	glVertexAttribPointer (w_pos, 4, GL_SHORT, GL_FALSE, sizeof (kit::Vertex), (void*) 0);
	glDrawArrays (GL_TRIANGLES, water_range.first, water_range.count);
	kit::own_gl_end ();
}

// Neighbours in a buffer that stand alike are one draw: a run gathers them
struct Run
{
	int cls = -1;				// the buffer; -1: none gathered
	int first, count;
	const Thing* like;			// where they stand, how they are drawn: as this one
	bool upside_down;
	kit::Blend blend;
	float alpha;
};
Run faces, tubes;

void send (Run& r)
{
	if (r.cls < 0)
		return;
	const Thing& t = *r.like;
	kit::Draw how;
	how.model = kit::translation ((float) t.position.x, (float) t.position.y, (float) t.position.z);
	if (t.rotation[0] || t.rotation[1] || t.rotation[2] || t.billboard)
		how.model = how.model * kit::rotation ((float) t.rotation[0], (float) (t.rotation[1] + (t.billboard ? camera.yaw : 0)), (float) t.rotation[2]);
	if (r.upside_down)
		how.model = kit::scaling (1, -1, 1) * how.model;
	how.cull = r.cls == CLOSED || r.cls == GLOSSY ? kit::BACK : kit::NONE;
	how.depth_test = !t.background;
	how.blend = r.blend;
	how.depth_write = r.blend == kit::OPAQUE;
	how.alpha = r.alpha;
	how.first = r.first;
	how.count = r.count;
	if (r.cls == TUBES)
	{
		kit::use (neon_program);
		if (lens_sent != focal)
			glUniform4f (neon_lens, focal / (kit::W / 2), focal / (kit::H / 2), focal, camera.near_plane);
		lens_sent = focal;
		how.program = &neon_program;
	}
	else if (r.cls == GLOSSY)
	{
		how.shading = kit::PHONG;
	}
	else
	{
		// (the kinds with pictures have the atlas: a vertex of theirs without one is marked so)
		const bool pictures = r.cls == SOLID || r.cls == DETAIL || r.cls == FAR || r.cls == MIRRORED;
		how.lit = false;
		how.texture = t.texture ? t.texture : pictures ? atlas : 0;
		how.key = t.texture == holo_texture;
	}
	const int cls = r.cls;
	r.cls = -1;
	kit::draw (arena[cls], how);
}

void add (Run& r, int cls, Range part, const Thing& t, bool upside_down, kit::Blend blend, float alpha)
{
	if (!part.count)
		return;
	if (   r.cls == cls && part.first == r.first + r.count && !t.billboard && !r.like->billboard && r.upside_down == upside_down
	    && r.blend == blend && r.alpha == alpha && r.like->texture == t.texture && r.like->background == t.background
	    && r.like->position.x == t.position.x && r.like->position.y == t.position.y && r.like->position.z == t.position.z
	    && r.like->rotation[0] == t.rotation[0] && r.like->rotation[1] == t.rotation[1] && r.like->rotation[2] == t.rotation[2])
	{
		r.count += part.count;
		return;
	}
	send (r);
	r.cls = cls;
	r.first = part.first;
	r.count = part.count;
	r.like = &t;
	r.upside_down = upside_down;
	r.blend = blend;
	r.alpha = alpha;
}

// a thing, if it can be seen: its faces (in the pass for far off: its simpler lot), its sign dim when it
// flickers, its tubes
void show (Thing& t, bool far_pass = false, bool upside_down = false)
{
	if (!t.enabled)
		return;
	// where its box's middle is before the camera: nothing is sent for what can't be seen
	P c = centre (t);
	if (t.rotation[1])
		c = yawed (c, (float) t.rotation[1]);
	c = c + t.position;
	const float cy = upside_down ? (float) -c.y : (float) c.y;
	const float vx = eye.m[0] * c.x + eye.m[4] * cy + eye.m[8] * c.z + eye.m[12];
	const float vy = eye.m[1] * c.x + eye.m[5] * cy + eye.m[9] * c.z + eye.m[13];
	const float vz = eye.m[2] * c.x + eye.m[6] * cy + eye.m[10] * c.z + eye.m[14];
	const float square = vx * vx + vy * vy + vz * vz;
	const float reach = (float) std::max ({t.high_corner.x - t.low_corner.x, t.high_corner.y - t.low_corner.y, t.high_corner.z - t.low_corner.z});
	if (square > (camera.far_plane + reach) * (camera.far_plane + reach) || (t.far_off && square >= (float) t.far_off * t.far_off))
		return;
	const float radius = reach * 0.87f + 8;		// (a ball about the box)
	if (vz + radius < camera.near_plane || std::fabs (vx) - radius > (vz + radius) * (kit::W / 2) / focal
	    || std::fabs (vy) - radius > (vz + radius) * (kit::H / 2) / focal)
		return;
	const bool in_street = shot >= 1 && shot <= 9;
	const bool far = in_street && t.far.count && square >= 3200.0f * 3200.0f;
	if (far != far_pass)
		return;
	if (t.cls == OWN)
	{
		kit::Draw how;
		how.model = kit::translation ((float) t.position.x, (float) t.position.y, (float) t.position.z)
			    * kit::rotation ((float) t.rotation[0], (float) t.rotation[1], (float) t.rotation[2]);
		how.cull = kit::NONE;
		how.lit = false;
		how.texture = t.texture;
		t.mesh.upload (GL_DYNAMIC_DRAW);
		kit::draw (t.mesh, how);
	}
	else if (far)
	{
		add (faces, FAR, t.far, t, upside_down, t.blend, t.alpha);
	}
	else if (t.sign >= 0 && std::fmod (shot_time * 11 + t.sign * 3.7f, 13.0f) < 0.45f)
	{
		// a sign flickers: for a moment it is dim (let through, mostly)
		const int after = t.sign_first + t.sign_count;
		add (faces, t.cls, {t.near.first, t.sign_first}, t, upside_down, t.blend, t.alpha);
		add (faces, t.cls, {t.near.first + after, t.near.count - after}, t, upside_down, t.blend, t.alpha);
		add (faces, t.cls, {t.near.first + t.sign_first, t.sign_count}, t, upside_down, kit::ALPHA, 70 / 255.0f);
	}
	else
	{
		add (faces, t.cls, t.near, t, upside_down, t.blend, t.alpha);
	}
	if (!far)
		add (tubes, TUBES, t.tube, t, upside_down, kit::OPAQUE, 1);
}

// the things of a kind, in the order made
void pass (int cls, bool far_pass = false, bool upside_down = false)
{
	for (auto& t : things)
		if (t->cls == cls)
			show (*t, far_pass, upside_down);
	send (faces);
	send (tubes);
}

void draw_scene (GLuint picture)
{
	kit::background (sky);
	kit::begin (camera, light);
	eye = camera.view ();
	focal = (kit::W / 2) / std::tan (camera.fov * PI / 360);
	// what is under everything; the wet street's mirror (the fronts upside down) under its floor; what is
	// let through, on them (the river is that, with a shader of its own)
	pass (GROUND);
	if (shot == 1)
		pass (MIRRORED, false, true);
	if (picture && water_range.count)
		draw_water (picture);
	for (auto& t : things)
		if (t->cls == GROUND_SHEER && !t->water)
			show (*t);
	send (faces);
	// what stands on it, by the depth buffer; then what is blended or added
	pass (CLOSED);
	pass (SOLID);
	pass (DETAIL);
	pass (DETAIL, true);
	pass (MIRRORED);
	pass (BILLBOARD);
	pass (GLOSSY);
	pass (OWN);
	pass (SHEER);
}

void draw ()
{
	GLuint picture = 0;
	if (has_water)				// the river mirrors the picture: once into a texture, without it
	{
		kit::capture_begin (2);
		draw_scene (0);
		picture = kit::capture_end ();
	}
	draw_scene (picture);
	draw_weather ();

	// the lights' halos
	const P facing = hero.direction ({0, 0, 1024});
	const bool front = hero_glow >= 0 && (long long) facing.x * ((int) camera.x - hero.position.x) + (long long) facing.y * ((int) camera.y - hero.position.y)
						     + (long long) facing.z * ((int) camera.z - hero.position.z) > 0;
	for (size_t i = 0; i < glows.size (); i++)
	{
		const Glow& g = glows[i];
		float x, y, depth;
		if (!camera.project ((float) g.position.x, (float) g.position.y, (float) g.position.z, &x, &y, &depth) || depth <= 80 || depth >= camera.far_plane)
			continue;
		const int sx = kit::W / 2 + (int) (x - kit::W / 2) - 16 * g.scale, sy = kit::H / 2 - (int) (kit::H / 2 - y) - 16 * g.scale;
		int alpha = 255;
		if ((shot == 4 || shot == 5) && !(shot == 4 && sy < kit::H / 2))
			continue;			// (from the seat: only those above the dashboard)
		if (hero_glow >= 0 && (int) i >= hero_glow)
		{
			const int n = (int) i - hero_glow;
			if (n < 2 && !front)
				continue;
			if (n >= 2)
			{
				if (!(shot >= 9 || (shot == 8 && shot_time > 2)))
					continue;
				alpha = shot >= 9 ? 220 : (int) (180 * clamp01 ((shot_time - 2) / 6));
			}
		}
		kit::sprite (glow_texture, 16, 16, sx, sy, alpha, true, g.scale, kit::MIRROR_X | kit::MIRROR_Y);
	}
	// the film's bars; the credits; the fades
	kit::rect (0, 0, kit::W, 12, 0);
	kit::rect (0, 308, kit::W, 12, 0);
	if (credits)
	{
		const int reveal = (int) (255 * clamp01 ((shot_time - CREDITS_START) / CREDITS_FADE_IN));
		if (reveal * 90 / 255 > 0)
			kit::rect (0, 0, kit::W, kit::H, 0, reveal * 90 / 255);
		kit::sprite_mask (credit_texture, 360, 168, 60, 68, 0xFFFF, reveal);
	}
	int dark = shot == 0 ? (int) (255 * (1 - clamp01 (shot_time / 3))) : shot == 10 ? (int) (255 * clamp01 ((shot_time - 5) / 3)) : 0;
	if (shot == 11)
		dark = (int) (255 * std::max (1 - clamp01 (shot_time / 2), clamp01 ((shot_time - CREDITS_START - CREDITS_FADE_IN - CREDITS_HOLD) / CREDITS_FADE_OUT)));
	if (dark > 0)
		kit::rect (0, 0, kit::W, kit::H, 0, dark);
}

}  // namespace

int main ()
{
	return kit::run ({"ESP 88", init, update, draw});
}
