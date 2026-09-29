// JetGpu.cpp - Jet's pixel work on the V3D (JetGpu.hpp). Replaces Jet's
// Renderer.cpp in the build: Rasterizer::drawTriangle lights the triangle the
// way Jet does, per vertex, and hands it to a batch; batches become GL draws
// (client arrays, pgl) in the order Jet submits them.
//
// Jet's lighting (Renderer.cpp, MIT, CubeCoders): brightness = a squared
// Lambert term times the light intensity and the material's diffuse, plus a
// view-facing specular term; per channel the ambient is added, capped at
// 255 + specular; the colour is scaled by it, and above 255 blows out towards
// white. Here the per-channel factor goes to the GPU per vertex (textured) or
// the finished colour (untextured). PHONG is lit per vertex here.
#include "JetGpu.hpp"
#include "Renderer.hpp"
#include "Sprite2D.hpp"
#include "Texture.hpp"
#include "JetConfig.hpp"
#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cstring>
#include "pgl.h"
#include "jet_program.h"
#include "jetwater_program.h"
#include "pico/time.h"

#if JET_PROFILE
#define PROFILE_BEGIN	const uint32_t profile_start = time_us_32 ()
#define PROFILE_END(t)	(t) += time_us_32 () - profile_start
#else
#define PROFILE_BEGIN
#define PROFILE_END(t)
#endif

using namespace Renderer;

namespace JetGpu {
namespace {

struct Vertex
{
	int16_t x, y, z, texture;	// pixels, camera depth (0: 2D), texture weight (1024 = 1)
	int16_t u, v;			// Jet fixed point (1024 = one texture)
	uint8_t rgba[4];		// colour, or light factor / 2 when textured
};
static_assert (sizeof (Vertex) == 16, "the GPU reads 16-byte vertices");

enum Address { WRAP_UV = 0, CLAMP_UV = 1, ZERO_UV = 2 };

// what a batch shares: one GL draw
struct State
{
	GLuint texture;			// 0: untextured
	bool key;			// colour key: alpha 0 texels are holes
	uint8_t address;
	bool additive;
	uint16_t flat;			// texture LOD's fade colour (RGB565)
	const Material* water;		// WATER_REFLECT: drawn by the water program
	bool scanlines;			// the CRT effect: odd rows only
	bool operator== (const State& o) const
	{
		return texture == o.texture && key == o.key && address == o.address
		       && additive == o.additive && flat == o.flat && water == o.water
		       && scanlines == o.scanlines;
	}
};

constexpr int MAX_VERTICES = 1536;	// per draw: 24 KB
Vertex batch[MAX_VERTICES];
int n_batch;
State state;

GLuint program, white, gradient_texture;
GLint a_pos, a_uv, a_color, u_mode, u_flat, u_rows;

// the water: its program, the scene's water state, and the previous frame to
// mirror: with water the scene is drawn into one of two textures (frame
// buffer objects) and that onto the panel; the next frame mirrors it. All
// GPU work: copying the panel into a texture costs the RPi's ARM ~15 ms.
GLuint water_program, frame_texture[2], frame_buffer[2];
GLint w_pos, w_uv, w_color, w_water, w_water2, w_colour, w_sky_rows;
bool frame_valid, water_drawn, offscreen;
bool composed;				// this frame's texture is on the panel: drawing goes there now
int current;

float water_time;
int water_line;
int W, H, TOP, panel_w, panel_h;
int area_x, area_y, area_w, area_h;	// the scene on the screen (GL window coordinates)
int draws, triangles, last_draws, last_triangles;
uint32_t us_textures, us_draws, us_triangles;
uint16_t gradient_copy[512];
int gradient_rows;

// ---- textures: Jet's (RGB565, colour key, palette) as GL textures, cached ----

struct Cached
{
	const Texture* texture;
	const uint16_t* data;
	const uint16_t* palette;
	int width, height, palette_offset;
	bool key, linear;
	uint16_t key_color;
	uint32_t sum;			// a sample of the texels: a changed image is sent again
	GLuint name;
	unsigned last_frame;
};
constexpr int MAX_TEXTURES = 48;
Cached cache[MAX_TEXTURES];
unsigned frame_number;

uint32_t texel_sum (const Texture* t)
{
	// texels in flash (const arrays: XIP from 0x10000000) never change, and
	// reading them through the XIP cache costs ~3 ms a frame for a 128 KB image
	if ((uintptr_t) t->data < 0x20000000u)
		return 0;
	uint32_t sum = 0, n = (uint32_t) (t->width * t->height);
	for (uint32_t i = 0; i < n; i += 7)
		sum = sum * 31 + t->data[i];
	return sum;
}

uint16_t texel (const Texture* t, int i)
{
	if (!t->palette)
		return t->data[i];
	int index = t->data[i] + t->paletteOffset;
	if (t->paletteSize > 0)
		index %= t->paletteSize;
	return t->palette[index & 0xFF];
}

void upload (Cached& c, const Texture* t)
{
	c.texture = t;
	c.data = t->data;
	c.palette = t->palette;
	c.width = t->width;
	c.height = t->height;
	c.palette_offset = t->paletteOffset;
	c.key = t->hasAlpha;
	c.key_color = t->alphaColor;
#if BILINEAR_FILTER
	c.linear = t->bilinear && !t->palette;
#else
	c.linear = false;
#endif
	c.sum = texel_sum (t);
	if (!c.name)
		glGenTextures (1, &c.name);
	glBindTexture (GL_TEXTURE_2D, c.name);
	const int n = t->width * t->height;
	if (!t->hasAlpha && !t->palette)
	{
		glPixelStorei (GL_UNPACK_ALIGNMENT, 2);
		glTexImage2D (GL_TEXTURE_2D, 0, GL_RGB, t->width, t->height, 0, GL_RGB,
			      GL_UNSIGNED_SHORT_5_6_5, t->data);
	}
	else
	{
		// the colour key as alpha, palette indices as colours: RGBA 8888
		uint8_t* rgba = (uint8_t*) std::malloc ((size_t) n * 4);
		for (int i = 0; i < n; i++)
		{
			uint16_t p = texel (t, i);
			bool hole = t->hasAlpha && p == t->alphaColor;
			rgba[4*i + 0] = (uint8_t) (((p >> 11) & 31) * 255 / 31);
			rgba[4*i + 1] = (uint8_t) (((p >> 5) & 63) * 255 / 63);
			rgba[4*i + 2] = (uint8_t) ((p & 31) * 255 / 31);
			rgba[4*i + 3] = hole ? 0 : 255;
		}
		glPixelStorei (GL_UNPACK_ALIGNMENT, 4);
		glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, t->width, t->height, 0, GL_RGBA,
			      GL_UNSIGNED_BYTE, rgba);
		std::free (rgba);
	}
	// wrapping is done in the shader: GL ES 2.0 repeats only 2^n sizes
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, c.linear ? GL_LINEAR : GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, c.linear ? GL_LINEAR : GL_NEAREST);
}

// the GL texture for a Jet texture, sent (again) when it has changed
GLuint texture_lookup (const Texture* t)
{
	Cached* free_slot = nullptr;
	Cached* oldest = &cache[0];
	for (Cached& c : cache)
	{
		if (c.texture == t)
		{
			if (c.last_frame != frame_number)
			{
				c.last_frame = frame_number;
				bool changed = c.data != t->data || c.palette != t->palette || c.width != t->width
					       || c.height != t->height || c.key != t->hasAlpha
					       || c.key_color != t->alphaColor
					       || (t->palette && c.palette_offset != t->paletteOffset)
					       || c.sum != texel_sum (t);
				if (changed)
					upload (c, t);
			}
			return c.name;
		}
		if (!c.texture && !free_slot)
			free_slot = &c;
		if (c.last_frame < oldest->last_frame)
			oldest = &c;
	}
	Cached& c = free_slot ? *free_slot : *oldest;
	c.last_frame = frame_number;
	upload (c, t);
	return c.name;
}

GLuint texture_of (const Texture* t)
{
	PROFILE_BEGIN;
	GLuint name = texture_lookup (t);
	PROFILE_END (us_textures);
	return name;
}

// ---- batches --------------------------------------------------------------------

// the scene's top row in GL window coordinates, for shaders that need rows
float top_row ()
{
	return offscreen && !composed ? H - 0.5f : (float) (area_y + area_h) - 0.5f;
}

void flush_water ()
{
	const Material* m = state.water;
	glUseProgram (water_program);
	const uint16_t c = m->color;
	glUniform4f (w_water, (float) ((int) m->specular * H / 2400), (float) (int) (water_time * 240.0f),
		     (float) (water_line > 0 ? water_line : H / 2), (float) m->waterYBias);
	glUniform4f (w_water2, (float) m->waterReflectionMaxY, m->alpha / 255.0f, (float) H,
		     top_row ());		// the scene's top row (GL y)
	glUniform4f (w_colour, ((c >> 11) & 31) / 31.0f, ((c >> 5) & 63) / 63.0f, (c & 31) / 31.0f, (float) W);
	glUniform1f (w_sky_rows, (float) gradient_rows);
	glActiveTexture (GL_TEXTURE1);
	glBindTexture (GL_TEXTURE_2D, gradient_rows ? gradient_texture : white);
	glActiveTexture (GL_TEXTURE0);
	glBindTexture (GL_TEXTURE_2D, frame_valid ? frame_texture[1 - current] : white);
	glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glBindBuffer (GL_ARRAY_BUFFER, 0);
	glVertexAttribPointer (w_pos, 4, GL_SHORT, GL_FALSE, sizeof (Vertex), &batch[0].x);
	glVertexAttribPointer (w_uv, 2, GL_SHORT, GL_FALSE, sizeof (Vertex), &batch[0].u);
	glVertexAttribPointer (w_color, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof (Vertex), &batch[0].rgba);
	glDrawArrays (GL_TRIANGLES, 0, n_batch);
	water_drawn = true;
}

void flush_batch ();

void flush ()
{
	if (!n_batch)
		return;
	PROFILE_BEGIN;
	flush_batch ();
	PROFILE_END (us_draws);
}

void flush_batch ()
{
	if (state.water)
	{
		flush_water ();
		draws++;
		triangles += n_batch / 3;
		n_batch = 0;
		return;
	}
	glUseProgram (program);
	glBindTexture (GL_TEXTURE_2D, state.texture ? state.texture : white);
	glUniform4f (u_mode, state.texture ? 1.0f : 0.0f, state.key ? 1.0f : 0.0f, (float) state.address, 0.0f);
	glUniform3f (u_flat, ((state.flat >> 11) & 31) / 31.0f, ((state.flat >> 5) & 63) / 63.0f,
		     (state.flat & 31) / 31.0f);
	glUniform2f (u_rows, state.scanlines ? top_row () : 0.0f,
		     offscreen && !composed ? 1.0f : (float) H / area_h);
	glBlendFunc (GL_SRC_ALPHA, state.additive ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
	glBindBuffer (GL_ARRAY_BUFFER, 0);		// client arrays (a bound buffer takes offsets)
	glVertexAttribPointer (a_pos, 4, GL_SHORT, GL_FALSE, sizeof (Vertex), &batch[0].x);
	glVertexAttribPointer (a_uv, 2, GL_SHORT, GL_FALSE, sizeof (Vertex), &batch[0].u);
	glVertexAttribPointer (a_color, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof (Vertex), &batch[0].rgba);
	glDrawArrays (GL_TRIANGLES, 0, n_batch);
	draws++;
	triangles += n_batch / 3;
	n_batch = 0;
}

Vertex* reserve (const State& s, int n)
{
	if (n_batch && (!(s == state) || n_batch + n > MAX_VERTICES))
		flush ();
	state = s;
	Vertex* v = &batch[n_batch];
	n_batch += n;
	return v;
}

int16_t clamp16 (int32_t v)
{
	return (int16_t) std::min (std::max (v, (int32_t) -32768), (int32_t) 32767);
}

void rgba_of (uint8_t* out, uint16_t c, uint8_t alpha)
{
	out[0] = (uint8_t) (((c >> 11) & 31) * 255 / 31);
	out[1] = (uint8_t) (((c >> 5) & 63) * 255 / 63);
	out[2] = (uint8_t) ((c & 31) * 255 / 31);
	out[3] = alpha;
}

// a screen rectangle: pixels x0..x1, y0..y1, texture coordinates (Jet units)
void rectangle (const State& s, int x0, int y0, int x1, int y1, int u0, int v0, int u1, int v1,
		const uint8_t rgba[4])
{
	Vertex* v = reserve (s, 6);
	const Vertex c[4] =
	{
		{(int16_t) x0, (int16_t) y0, 0, 1024, (int16_t) u0, (int16_t) v0, {}},
		{(int16_t) x1, (int16_t) y0, 0, 1024, (int16_t) u1, (int16_t) v0, {}},
		{(int16_t) x1, (int16_t) y1, 0, 1024, (int16_t) u1, (int16_t) v1, {}},
		{(int16_t) x0, (int16_t) y1, 0, 1024, (int16_t) u0, (int16_t) v1, {}},
	};
	static const int order[6] = {0, 1, 2, 0, 2, 3};
	for (int i = 0; i < 6; i++)
	{
		v[i] = c[order[i]];
		std::memcpy (v[i].rgba, rgba, 4);
	}
}

}  // namespace

// ---- the runtime's calls ------------------------------------------------------------------

// where the scene goes: the screen (the viewport, whole between frames),
// as large as fits in the scene's proportions, centred; rows from TOP on.
// Again each frame: the screen changes with an HDMI monitor
void place ()
{
	GLint vp[4];
	glGetIntegerv (GL_VIEWPORT, vp);
	panel_w = vp[2];
	panel_h = vp[3];
	if (panel_w * (TOP + H) <= panel_h * W)
	{
		area_w = panel_w;
		area_h = panel_w * H / W;
	}
	else
	{
		area_h = panel_h * H / (TOP + H);
		area_w = area_h * W / H;
	}
	int top = TOP * area_h / H;
	area_x = (panel_w - area_w) / 2;
	area_y = (panel_h - area_h - top) / 2;
}

void init (int width, int height, int top)
{
	W = width;
	H = height;
	TOP = top;
	place ();

	program = glCreateProgram ();
	glProgramBinaryOES (program, PGL_PROGRAM_BINARY_PGPU, &jet_info, sizeof jet_info);
	a_pos = glGetAttribLocation (program, "a_pos");
	a_uv = glGetAttribLocation (program, "a_uv");
	a_color = glGetAttribLocation (program, "a_color");
	u_mode = glGetUniformLocation (program, "u_mode");
	u_flat = glGetUniformLocation (program, "u_flat");
	u_rows = glGetUniformLocation (program, "u_rows");
	glUseProgram (program);
	glUniform2f (glGetUniformLocation (program, "u_scale"), 2.0f / W, -2.0f / H);
	glUniform1i (glGetUniformLocation (program, "u_texture"), 0);

	static const uint8_t one[4] = {255, 255, 255, 255};
	glGenTextures (1, &white);
	glBindTexture (GL_TEXTURE_2D, white);
	glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, one);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glGenTextures (1, &gradient_texture);

	water_program = glCreateProgram ();
	glProgramBinaryOES (water_program, PGL_PROGRAM_BINARY_PGPU, &jetwater_info, sizeof jetwater_info);
	w_pos = glGetAttribLocation (water_program, "a_pos");
	w_uv = glGetAttribLocation (water_program, "a_uv");
	w_color = glGetAttribLocation (water_program, "a_color");
	w_water = glGetUniformLocation (water_program, "u_water");
	w_water2 = glGetUniformLocation (water_program, "u_water2");
	w_colour = glGetUniformLocation (water_program, "u_color");
	w_sky_rows = glGetUniformLocation (water_program, "u_sky_rows");
	glUseProgram (water_program);
	glUniform2f (glGetUniformLocation (water_program, "u_scale"), 2.0f / W, -2.0f / H);
	glUniform1i (glGetUniformLocation (water_program, "u_frame"), 0);
	glUniform1i (glGetUniformLocation (water_program, "u_sky"), 1);
}

// the scene into textures from now on (the first frame with water)
void start_offscreen ()
{
	glGenTextures (2, frame_texture);
	glGenFramebuffers (2, frame_buffer);
	for (int i = 0; i < 2; i++)
	{
		glBindTexture (GL_TEXTURE_2D, frame_texture[i]);
		glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glBindFramebuffer (GL_FRAMEBUFFER, frame_buffer[i]);
		glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, frame_texture[i], 0);
	}
	glBindFramebuffer (GL_FRAMEBUFFER, 0);
	offscreen = true;
}

void beginFrame ()
{
	frame_number++;
	composed = false;
	place ();
	draws = triangles = 0;
	if (offscreen)
	{
		// into this frame's texture
		glBindFramebuffer (GL_FRAMEBUFFER, frame_buffer[current]);
		glViewport (0, 0, W, H);
		glScissor (0, 0, W, H);
	}
	else
	{
		// the scene's place on the screen
		glViewport (area_x, area_y, area_w, area_h);
		glScissor (area_x, area_y, area_w, area_h);
	}
	glEnable (GL_SCISSOR_TEST);
	glDisable (GL_DEPTH_TEST);
	glDisable (GL_CULL_FACE);
	glEnable (GL_BLEND);
	glActiveTexture (GL_TEXTURE0);
	for (GLint i = 0; i < 4; i++)
	{
		if (i == a_pos || i == a_uv || i == a_color)
			glEnableVertexAttribArray (i);
		else
			glDisableVertexAttribArray (i);
	}
}

// offscreen: this frame's texture onto the panel (its rows run bottom up);
// what follows (sprites) is drawn on the panel, so the water never mirrors
// sprites - upstream composites them at scanout, after the buffer it mirrors
void compose ()
{
	if (!offscreen || composed)
		return;
	flush ();
	glBindFramebuffer (GL_FRAMEBUFFER, 0);
	glViewport (area_x, area_y, area_w, area_h);
	glScissor (area_x, area_y, area_w, area_h);
	static const uint8_t unlit[4] = {128, 128, 128, 255};
	const State s = {frame_texture[current], false, CLAMP_UV, false, 0, nullptr, false};
	rectangle (s, 0, 0, W, H, 0, 1024, 1024, 0, unlit);
	flush ();
	composed = true;
}

void endFrame ()
{
	flush ();
	if (offscreen)
	{
		compose ();
		frame_valid = true;			// the next frame's water mirrors this one
		current = 1 - current;
	}
	else if (water_drawn)
	{
		start_offscreen ();			// from the next frame
	}
	water_drawn = false;
	glDisable (GL_SCISSOR_TEST);
	glDisable (GL_BLEND);
	glViewport (0, 0, panel_w, panel_h);
	last_draws = draws;
	last_triangles = triangles;
}

int lastDraws ()	{ return last_draws; }

void profile (uint32_t* textures, uint32_t* draws_us, uint32_t* triangles_us)
{
	*textures = us_textures;
	*draws_us = us_draws;
	*triangles_us = us_triangles;
	us_textures = us_draws = us_triangles = 0;
}
int lastTriangles ()	{ return last_triangles; }

void clear (const uint16_t* gradient, uint16_t color, int rows)
{
	flush ();
	if (!gradient)
	{
		glClearColor (((color >> 11) & 31) / 31.0f, ((color >> 5) & 63) / 63.0f, (color & 31) / 31.0f, 1.0f);
		glClear (GL_COLOR_BUFFER_BIT);
		return;
	}
	// a colour per row: a texture one texel wide, over the whole scene
	rows = std::min (rows, 512);
	if (rows != gradient_rows || std::memcmp (gradient, gradient_copy, rows * 2))
	{
		std::memcpy (gradient_copy, gradient, rows * 2);
		gradient_rows = rows;
		glBindTexture (GL_TEXTURE_2D, gradient_texture);
		glPixelStorei (GL_UNPACK_ALIGNMENT, 2);
		glTexImage2D (GL_TEXTURE_2D, 0, GL_RGB, 1, rows, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, gradient_copy);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	}
	static const uint8_t unlit[4] = {128, 128, 128, 255};	// light factor 1
	const State s = {gradient_texture, false, CLAMP_UV, false, 0, nullptr, false};
	rectangle (s, 0, 0, W, H, 0, 0, 1024, 1024 * H / rows, unlit);
	flush ();
}

void sprite (const Sprite2D& sp, uint8_t alpha)
{
	compose ();			// sprites go over the scene, not into its texture
	const Texture* tex = sp.material->diffuseMap;
	const bool additive = sp.blendMode == BlendMode::BLEND_ADD;
	uint8_t rgba[4];
	if (!tex)
	{
		rgba_of (rgba, sp.material->color, additive ? 255 : alpha);
		const State s = {0, false, CLAMP_UV, additive, 0, nullptr, false};
		rectangle (s, sp.x, sp.y, sp.x + sp.width * sp.scale, sp.y + sp.height * sp.scale,
			   0, 0, 0, 0, rgba);
		return;
	}
	// additive sprites are not scaled by alpha (Sprite2D.hpp)
	rgba[0] = rgba[1] = rgba[2] = 128;
	rgba[3] = additive ? 255 : alpha;
	const State s = {texture_of (tex), tex->hasAlpha, CLAMP_UV, additive, 0, nullptr, false};

	// MIRROR_X/Y append a flipped copy (up to four quads); FLIP_X/Y reverse
	// the expanded image
	const bool mx = sp.textureFlags & Sprite2D::MIRROR_X, my = sp.textureFlags & Sprite2D::MIRROR_Y;
	const bool fx = sp.textureFlags & Sprite2D::FLIP_X, fy = sp.textureFlags & Sprite2D::FLIP_Y;
	const int w = tex->width * sp.scale, h = tex->height * sp.scale;
	const int ew = mx ? 2 * w : w, eh = my ? 2 * h : h;
	for (int qy = 0; qy < (my ? 2 : 1); qy++)
		for (int qx = 0; qx < (mx ? 2 : 1); qx++)
		{
			int x0 = qx * w, y0 = qy * h;
			int u0 = qx ? 1024 : 0, u1 = qx ? 0 : 1024;	// the copy is mirrored
			int v0 = qy ? 1024 : 0, v1 = qy ? 0 : 1024;
			if (fx)
			{
				x0 = ew - x0 - w;
				std::swap (u0, u1);
			}
			if (fy)
			{
				y0 = eh - y0 - h;
				std::swap (v0, v1);
			}
			rectangle (s, sp.x + x0, sp.y + y0, sp.x + x0 + w, sp.y + y0 + h, u0, v0, u1, v1, rgba);
		}
}

void crt (uint8_t intensity)
{
	// PostFX::applyCRT darkens the odd rows by intensity / 255: one quad, the
	// shader keeps the odd rows
	static const State s = {0, false, CLAMP_UV, false, 0, nullptr, true};
	const uint8_t black[4] = {0, 0, 0, intensity};
	rectangle (s, 0, 0, W, H, 0, 0, 0, 0, black);
}

}  // namespace JetGpu

// ---- Jet's rasteriser, on the GPU -------------------------------------------------------------

namespace {

#if LIGHTING
// Renderer.cpp's jetShadeBrightness (Jet, MIT, CubeCoders), unchanged in effect
uint16_t shade (const Vector3& N, const Vector3& L, uint16_t light, uint8_t diffuse, uint8_t specular)
{
	if (light > 255)
		light = 255;
	const uint32_t max_brightness = 255u + specular;
	int64_t lit = Vector3::dotProduct (N, L);
	if (lit <= 0)
		return 0;
	uint32_t lambert = (uint32_t) (lit >> 12);
	if (lambert > 255)
		lambert = 255;
	lambert = (lambert * lambert + 128) >> 8;
	lambert = (lambert * light) >> 8;
	uint32_t diffuse_term = (lambert * diffuse) >> 8;
	if (diffuse_term > max_brightness)
		diffuse_term = max_brightness;
	uint32_t specular_term = 0;
	if (specular && N.z < 0 && lambert > 0)
	{
		uint32_t facing = (uint32_t) (-N.z);
		if (facing > FIXED_POINT_SCALE)
			facing = FIXED_POINT_SCALE;
		uint32_t vf8 = std::min (facing >> 2, (uint32_t) 255);
		uint32_t vf2 = (vf8 * vf8) >> 8;
		vf2 = (vf2 * lambert) >> 8;
		vf2 = (vf2 * light) >> 8;
		specular_term = (vf2 * specular) >> 8;
	}
	return (uint16_t) std::min (diffuse_term + specular_term, max_brightness);
}
#endif

// colour channel c (0..1) at light t (255 = 1, above blows out towards white)
uint8_t modulate (float c, uint32_t t)
{
	float f = t <= 255 ? c * t / 255.0f : c + (1.0f - c) * (t - 255) / 256.0f;
	return (uint8_t) std::min (f * 255.0f + 0.5f, 255.0f);
}

}  // namespace

namespace Renderer {

bool Rasterizer::shouldDrawPixel (int, int, uint8_t alpha)	{ return alpha != 0; }

uint16_t Rasterizer::grayscaleToRGB565 (uint8_t g)
{
	return (uint16_t) ((g >> 3) << 11 | (g >> 2) << 5 | (g >> 3));
}

bool Rasterizer::drawTriangle (const RenderVertex& v1, const RenderVertex& v2, const RenderVertex& v3,
			       Material* material, DirectionalLight* directionalLight,
			       AmbientLight* ambientLight, bool, bool, bool, int, uint8_t objAlpha,
			       bool brightnessPrecomputed, int32_t avgZHint)
{
	using namespace JetGpu;
	PROFILE_BEGIN;
	struct Timer { uint32_t start; ~Timer () {
#if JET_PROFILE
		us_triangles += time_us_32 () - start;
#endif
	} } timer = {
#if JET_PROFILE
		profile_start
#else
		0
#endif
	};
	(void) timer;
	const uint8_t alpha = objAlpha == 255 ? material->alpha
					      : (uint8_t) ((uint16_t) material->alpha * objAlpha / 255);
	if (!alpha)
		return false;

	// the triangle's depth, culled against near and far (Jet's FAST_Z)
	int32_t z = avgZHint;
	if (z == INT32_MIN)
	{
		z = (v1.position.z + v2.position.z + v3.position.z) / 3;
		if (z < camera->nearPlane || z > camera->farPlane)
			return false;
	}
#if SKIP_ZERO_AREA_TRIANGLES
	{
		const int32_t min_x = std::min ({v1.position.x, v2.position.x, v3.position.x}) & ~1;
		const int32_t max_x = std::max ({v1.position.x, v2.position.x, v3.position.x}) & ~1;
		const int32_t min_y = std::min ({v1.position.y, v2.position.y, v3.position.y}) & ~1;
		const int32_t max_y = std::max ({v1.position.y, v2.position.y, v3.position.y}) & ~1;
		if (min_x == max_x || min_y == max_y)
			return false;
	}
#endif

	const bool additive = material->shadingMode == ShadingMode::ADDITIVE;
	const RenderVertex* v[3] = {&v1, &v2, &v3};

#if MAX_PICK_QUERIES > 0
	// which surface covers a queried pixel: the closest (the triangle's depth)
	if (pickQueries && pickResults && pickQueryCount > 0)
	{
		for (int p = 0; p < pickQueryCount; ++p)
		{
			const PickQuery& q = pickQueries[p];
			if (q.x < 0 || q.y < 0)
				continue;
			// the pixel centre inside the triangle (either winding)
			const float px = q.x + 0.5f, py = q.y + 0.5f;
			float e[3];
			for (int i = 0; i < 3; i++)
			{
				const Vector3& a = v[i]->position;
				const Vector3& b = v[(i + 1) % 3]->position;
				e[i] = (float) (b.x - a.x) * (py - a.y) - (float) (b.y - a.y) * (px - a.x);
			}
			if (!((e[0] >= 0 && e[1] >= 0 && e[2] >= 0) || (e[0] <= 0 && e[1] <= 0 && e[2] <= 0)))
				continue;
			PickResult& r = pickResults[p];
			if (!r.hit || z < r.depth)
			{
				r.hit = true;
				r.object = currentPickObject;
				r.triangleIndex = currentPickTriangleIndex;
#if JET_MESH_INSTANCING
				r.mesh = currentPickMesh;
				r.instanceIndex = currentPickInstanceIndex;
#endif
				r.depth = z;
				r.x = q.x;
				r.y = q.y;
			}
		}
	}
#endif

	// the light per vertex and channel (255 = the plain colour)
	uint32_t t[3][3];
	for (auto& ti : t)
		ti[0] = ti[1] = ti[2] = 255;
#if LIGHTING
	const bool emissive = material->emissive || material->shadingMode == ShadingMode::UNLIT
			      || material->shadingMode == ShadingMode::WATER_REFLECT || additive;
	if (!emissive && (directionalLight || ambientLight))
	{
		const uint8_t amb[3] = {ambientLight ? ambientLight->color.r : (uint8_t) 0,
					ambientLight ? ambientLight->color.g : (uint8_t) 0,
					ambientLight ? ambientLight->color.b : (uint8_t) 0};
		const uint16_t light = directionalLight ? directionalLight->intensity : 0;
		const uint32_t max_brightness = 255u + material->specular;
		uint16_t b[3] = {0, 0, 0};
		if (directionalLight)
		{
			const bool per_vertex = material->shadingMode == ShadingMode::GOURAUD
						|| material->shadingMode == ShadingMode::PHONG;
			for (int i = 0; i < (per_vertex ? 3 : 1); i++)
				b[i] = brightnessPrecomputed ? v[i]->lambertBrightness
					: shade (v[i]->normal, directionalLight->lightDir, light,
						 material->diffuse, material->specular);
			if (!per_vertex)
				b[1] = b[2] = b[0];
		}
		for (int i = 0; i < 3; i++)
			for (int c = 0; c < 3; c++)
				t[i][c] = std::min ((uint32_t) b[i] + amb[c], max_brightness);
	}
#else
	(void) directionalLight;
	(void) ambientLight;
	(void) brightnessPrecomputed;
#endif

	// textured, or faded to the material colour by distance (texture LOD)
	GLuint texture = 0;
	int weight = 1024;
	State s = {0, false, CLAMP_UV, additive, 0, nullptr, false};
	if (material->shadingMode == ShadingMode::WATER_REFLECT)
	{
		s.water = material;			// drawn per pixel by the water program
		water_time = waterTime;
		water_line = waterlineY;
	}
#if TEXTURE_MAPPING
	if (!s.water && material->diffuseMap)
	{
		if (textureLodEnabled && textureLodFar > textureLodNear)
		{
			if (z >= textureLodFar)
				weight = 0;
			else if (z > textureLodNear)
				weight = (int) ((int64_t) (textureLodFar - z) * 1024 / (textureLodFar - textureLodNear));
		}
		if (weight > 0)
		{
			const Texture* tex = material->diffuseMap;
			texture = texture_of (tex);
			s.texture = texture;
			s.key = tex->hasAlpha;
			s.address = tex->addressMode == TextureAddressMode::WRAP ? WRAP_UV
				    : tex->addressMode == TextureAddressMode::CLAMP ? CLAMP_UV : ZERO_UV;
			s.flat = material->color;
		}
	}
#endif

	Vertex* out = reserve (s, 3);
	const uint16_t c = material->color;
	const float base[3] = {((c >> 11) & 31) / 31.0f, ((c >> 5) & 63) / 63.0f, (c & 31) / 31.0f};
	for (int i = 0; i < 3; i++)
	{
		Vertex& o = out[i];
		o.x = clamp16 (v[i]->position.x);
		o.y = clamp16 (v[i]->position.y);
		o.z = clamp16 (std::max (v[i]->position.z, (int32_t) 1));
		o.texture = (int16_t) weight;
#if TEXTURE_MAPPING
		o.u = clamp16 (v[i]->uv.x);
		o.v = clamp16 (v[i]->uv.y);
#else
		o.u = o.v = 0;
#endif
		for (int ch = 0; ch < 3; ch++)
			o.rgba[ch] = texture ? (uint8_t) std::min (t[i][ch] / 2, (uint32_t) 255) : modulate (base[ch], t[i][ch]);
		o.rgba[3] = alpha;
	}
	return true;
}

}  // namespace Renderer
