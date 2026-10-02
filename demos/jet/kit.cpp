// kit.cpp - see kit.hpp
#include "kit.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "pico/stdlib.h"
extern "C" {
#include "pgpu.h"
#include "hud.h"
#include "pgpu_perf.h"
}
#include "kit_mesh_program.h"
#include "kit_phong_program.h"
#include "kit_sprite_program.h"

namespace kit {

namespace {

constexpr float PI = 3.14159265358979f;
float radians (float degrees)	{ return degrees * PI / 180.0f; }

// ---- the programs -------------------------------------------------------------------------------

typedef Program MeshProgram;
MeshProgram mesh_program, phong_program;
GLuint sprite_program;
GLint s_pos, s_uv, s_color, s_scale, s_mode;
GLuint white;

MeshProgram load_mesh_program (const void* info, unsigned size, bool phong)
{
	MeshProgram p;
	for (auto& u : p.last)
		u[0] = u[1] = u[2] = u[3] = -12345.0f;
	p.last_mvp[0] = p.last_view[0] = -12345.0f;
	p.name = glCreateProgram ();
	glProgramBinaryOES (p.name, PGL_PROGRAM_BINARY_PGPU, info, (GLsizei) size);
	GLint linked = 0;
	glGetProgramiv (p.name, GL_LINK_STATUS, &linked);
	if (!linked)
		std::printf ("kit: a program didn't link\n");
	p.pos = glGetAttribLocation (p.name, "a_pos");
	p.normal = glGetAttribLocation (p.name, "a_normal");
	p.color = glGetAttribLocation (p.name, "a_color");
	p.uv = glGetAttribLocation (p.name, "a_uv");
	p.material = glGetAttribLocation (p.name, "a_material");
	p.mvp = glGetUniformLocation (p.name, "u_mvp");
	p.view = glGetUniformLocation (p.name, "u_view");
	p.light = glGetUniformLocation (p.name, "u_light");
	p.ambient = glGetUniformLocation (p.name, "u_ambient");
	p.tex = glGetUniformLocation (p.name, "u_tex");
	p.tint = glGetUniformLocation (p.name, "u_tint");
	p.lod = phong ? -1 : glGetUniformLocation (p.name, "u_lod");
	p.half = phong ? glGetUniformLocation (p.name, "u_half") : -1;
	p.gloss = phong ? glGetUniformLocation (p.name, "u_gloss") : -1;
	glUseProgram (p.name);
	glUniform1i (glGetUniformLocation (p.name, "u_texture"), 0);
	return p;
}

// ---- the frame -----------------------------------------------------------------------------------

Mat4 frame_view, frame_projection;
float light_view[3], light_half[3], light_intensity = -1.0f, light_color[3], ambient[3];
int area_x, area_y, area_w, area_h;
bool capturing;

void place ()
{
	GLint vp[4];
	glGetIntegerv (GL_VIEWPORT, vp);
	if (vp[2] * H <= vp[3] * W)
	{
		area_w = vp[2];
		area_h = vp[2] * H / W;
	}
	else
	{
		area_h = vp[3];
		area_w = vp[3] * W / H;
	}
	area_x = (vp[2] - area_w) / 2;
	area_y = (vp[3] - area_h) / 2;
}

// ---- the 2D layer: a few quads at a time, client arrays ---------------------------------------------

// ---- the GL state, as it was left: a call only for what changes (each is a packet on the link) -------

struct State
{
	GLuint program, texture, layout_buffer, layout_program;
	int cull, depth_test, depth_write, blend;
};
State state;
constexpr GLuint UNKNOWN = 0xFFFFFFFFu;
enum { BLEND_OFF, BLEND_ALPHA, BLEND_ADD, BLEND_MULTIPLY };

void forget_state ()
{
	state.program = state.texture = state.layout_buffer = state.layout_program = UNKNOWN;
	state.cull = state.depth_test = state.depth_write = state.blend = -1;
	glActiveTexture (GL_TEXTURE0);
	glFrontFace (GL_CW);				// Jet's faces run clockwise seen from outside
	glDepthFunc (GL_LEQUAL);
}

void use_program (GLuint program)
{
	if (state.program != program)
		glUseProgram (program);
	state.program = program;
}

void bind_texture (GLuint texture)
{
	if (state.texture != texture)
		glBindTexture (GL_TEXTURE_2D, texture);
	state.texture = texture;
}

// for a texture being made or filled: bound whatever is thought to be (names are reused)
void bind_texture_now (GLuint texture)
{
	glBindTexture (GL_TEXTURE_2D, texture);
	state.texture = texture;
}

void set_cull (Cull cull)
{
	if (state.cull == (int) cull)
		return;
	if (cull == NONE)
	{
		glDisable (GL_CULL_FACE);
	}
	else
	{
		if (state.cull < 0 || state.cull == NONE)
			glEnable (GL_CULL_FACE);
		glCullFace (cull == BACK ? GL_BACK : GL_FRONT);
	}
	state.cull = (int) cull;
}

void set_depth (bool test, bool write)
{
	write = write && test;
	if (state.depth_test != (int) test)
	{
		if (test)
			glEnable (GL_DEPTH_TEST);
		else
			glDisable (GL_DEPTH_TEST);
	}
	if (state.depth_write != (int) write)
		glDepthMask (write ? GL_TRUE : GL_FALSE);
	state.depth_test = test;
	state.depth_write = write;
}

void set_blend (int blend)
{
	if (state.blend == blend)
		return;
	if (blend == BLEND_OFF)
	{
		glDisable (GL_BLEND);
	}
	else
	{
		if (state.blend <= BLEND_OFF)
			glEnable (GL_BLEND);
		if (blend == BLEND_MULTIPLY)
			glBlendFunc (GL_ZERO, GL_SRC_COLOR);
		else
			glBlendFunc (GL_SRC_ALPHA, blend == BLEND_ADD ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
	}
	state.blend = blend;
}

// ---- the 2D layer: quads of the same kind gathered and drawn together, client arrays ---------------

struct SpriteVertex { float x, y, u, v; uint8_t rgba[4]; };
constexpr int BATCH_QUADS = 48;
SpriteVertex batch[BATCH_QUADS * 6];
int batched = 0;
GLuint batch_texture;
int batch_blend;
bool batch_cut, batch_shape;
float sprite_mode[3] = {-1, -1, -1};
GLuint gradient_texture;
uint16_t gradient_copy[H];

enum QuadBlend { SOLID, BLENDED, ADDED, MULTIPLIED };

void flush_quads ()
{
	if (!batched)
		return;
	use_program (sprite_program);
	const float mode[3] = {batch_texture ? 1.0f : 0.0f, batch_cut ? 1.0f : 0.0f, batch_shape ? 1.0f : 0.0f};
	if (std::memcmp (mode, sprite_mode, sizeof mode) != 0)
	{
		glUniform4f (s_mode, mode[0], mode[1], mode[2], 0.0f);
		std::memcpy (sprite_mode, mode, sizeof mode);
	}
	bind_texture (batch_texture ? batch_texture : white);
	set_depth (false, false);
	set_cull (NONE);
	set_blend (batch_blend);
	if (state.layout_program != sprite_program)
	{
		glBindBuffer (GL_ARRAY_BUFFER, 0);
		for (GLint i = 0; i < 8; i++)
		{
			if (i == s_pos || i == s_uv || i == s_color)
				glEnableVertexAttribArray (i);
			else
				glDisableVertexAttribArray (i);
		}
		glVertexAttribPointer (s_pos, 2, GL_FLOAT, GL_FALSE, sizeof (SpriteVertex), &batch[0].x);
		glVertexAttribPointer (s_uv, 2, GL_FLOAT, GL_FALSE, sizeof (SpriteVertex), &batch[0].u);
		glVertexAttribPointer (s_color, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof (SpriteVertex), &batch[0].rgba);
		state.layout_program = sprite_program;
		state.layout_buffer = 0;
	}
	glDrawArrays (GL_TRIANGLES, 0, batched * 6);
	batched = 0;
}

void draw_quad (GLuint tex, bool cut, QuadBlend blend, float x0, float y0, float x1, float y1,
		float u0, float v0, float u1, float v1, const uint8_t rgba[4], bool shape = false)
{
	const int how = blend == SOLID ? BLEND_OFF : blend == BLENDED ? BLEND_ALPHA : blend == ADDED ? BLEND_ADD : BLEND_MULTIPLY;
	if (batched && (batched == BATCH_QUADS || tex != batch_texture || how != batch_blend || cut != batch_cut || shape != batch_shape))
		flush_quads ();
	batch_texture = tex;
	batch_blend = how;
	batch_cut = cut;
	batch_shape = shape;
	const float c[4][4] = {{x0, y0, u0, v0}, {x1, y0, u1, v0}, {x1, y1, u1, v1}, {x0, y1, u0, v1}};
	static const int order[6] = {0, 1, 2, 0, 2, 3};
	for (int i = 0; i < 6; i++)
	{
		SpriteVertex& v = batch[batched * 6 + i];
		v.x = c[order[i]][0];
		v.y = c[order[i]][1];
		v.u = c[order[i]][2];
		v.v = c[order[i]][3];
		std::memcpy (v.rgba, rgba, 4);
	}
	batched++;
}

}  // namespace

static void texture_parameters (bool linear, bool repeat)
{
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
}

// ---- matrices ---------------------------------------------------------------------------------------

Mat4 identity ()
{
	Mat4 r = {{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}};
	return r;
}

Mat4 operator* (const Mat4& a, const Mat4& b)
{
	Mat4 r;
	for (int c = 0; c < 4; c++)
		for (int row = 0; row < 4; row++)
		{
			float s = 0;
			for (int k = 0; k < 4; k++)
				s += a.m[k * 4 + row] * b.m[c * 4 + k];
			r.m[c * 4 + row] = s;
		}
	return r;
}

Mat4 translation (float x, float y, float z)
{
	Mat4 r = identity ();
	r.m[12] = x;
	r.m[13] = y;
	r.m[14] = z;
	return r;
}

Mat4 scaling (float x, float y, float z)
{
	Mat4 r = identity ();
	r.m[0] = x;
	r.m[5] = y;
	r.m[10] = z;
	return r;
}

// Jet's object matrix: Rz Ry Rx, with
//   Rx: y' = y cx - z sx, z' = y sx + z cx;  Ry: x' = x cy + z sy, z' = -x sy + z cy;
//   Rz: x' = x cz - y sz, y' = x sz + y cz
static Mat4 rotation_radians (float x, float y, float z)
{
	const float cx = std::cos (x), sx = std::sin (x), cy = std::cos (y), sy = std::sin (y);
	const float cz = std::cos (z), sz = std::sin (z);
	// K = Ry Rx
	const float k[3][3] = {{cy, sy * sx, sy * cx}, {0, cx, -sx}, {-sy, cy * sx, cy * cx}};
	Mat4 r = identity ();
	for (int col = 0; col < 3; col++)
	{
		r.m[col * 4 + 0] = cz * k[0][col] - sz * k[1][col];
		r.m[col * 4 + 1] = sz * k[0][col] + cz * k[1][col];
		r.m[col * 4 + 2] = k[2][col];
	}
	return r;
}

Mat4 rotation (float x, float y, float z)
{
	return rotation_radians (radians (x), radians (y), radians (z));
}

void Camera::look_at (float tx, float ty, float tz)
{
	const float dx = tx - x, dy = ty - y, dz = tz - z;
	pitch = -std::atan2 (dy, std::sqrt (dx * dx + dz * dz)) * 180.0f / PI;
	yaw = std::atan2 (dx, dz) * 180.0f / PI;
	roll = 0;
}

// Jet's camera: a point less the camera's place, turned back about y, then x, then z
Mat4 Camera::view () const
{
	const float ax = radians (-pitch), ay = radians (-yaw), az = radians (-roll);
	const float cx = std::cos (ax), sx = std::sin (ax), cy = std::cos (ay), sy = std::sin (ay);
	const float cz = std::cos (az), sz = std::sin (az);
	// Rz Rx Ry
	const float ry[3][3] = {{cy, 0, sy}, {0, 1, 0}, {-sy, 0, cy}};
	const float rx[3][3] = {{1, 0, 0}, {0, cx, -sx}, {0, sx, cx}};
	const float rz[3][3] = {{cz, -sz, 0}, {sz, cz, 0}, {0, 0, 1}};
	float xy[3][3], all[3][3];
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			xy[i][j] = rx[i][0] * ry[0][j] + rx[i][1] * ry[1][j] + rx[i][2] * ry[2][j];
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			all[i][j] = rz[i][0] * xy[0][j] + rz[i][1] * xy[1][j] + rz[i][2] * xy[2][j];
	Mat4 r = identity ();
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			r.m[j * 4 + i] = all[i][j];
	return r * translation (-x, -y, -z);
}

// Jet projects x f / z about the screen's middle, f = (width / 2) / tan (fov / 2); z is the distance
Mat4 Camera::projection () const
{
	const float f = (W / 2) / std::tan (radians (fov) * 0.5f);
	Mat4 r;
	std::memset (&r, 0, sizeof r);
	r.m[0] = f / (W / 2);
	r.m[5] = f / (H / 2);
	r.m[10] = (far_plane + near_plane) / (far_plane - near_plane);
	r.m[11] = 1.0f;
	r.m[14] = -2.0f * far_plane * near_plane / (far_plane - near_plane);
	return r;
}

bool Camera::project (float px, float py, float pz, float* sx, float* sy, float* depth) const
{
	const Mat4 v = view ();
	const float vx = v.m[0] * px + v.m[4] * py + v.m[8] * pz + v.m[12];
	const float vy = v.m[1] * px + v.m[5] * py + v.m[9] * pz + v.m[13];
	const float vz = v.m[2] * px + v.m[6] * py + v.m[10] * pz + v.m[14];
	if (vz <= 0.0f)
		return false;
	const float f = (W / 2) / std::tan (radians (fov) * 0.5f);
	*sx = W / 2 + vx * f / vz;
	*sy = H / 2 - vy * f / vz;
	if (depth)
		*depth = vz;
	return true;
}

void Camera::unproject (float sx, float sy, float depth, float out[3]) const
{
	const float f = (W / 2) / std::tan (radians (fov) * 0.5f);
	out[0] = (sx - W / 2) * depth / f;
	out[1] = (H / 2 - sy) * depth / f;
	out[2] = depth;
}

// ---- meshes ---------------------------------------------------------------------------------------

static int16_t clamp16 (float v)
{
	return (int16_t) std::lround (std::min (std::max (v, -32767.0f), 32767.0f));
}

int Mesh::add (float x, float y, float z, float nx, float ny, float nz, const Material& m, int u, int v)
{
	Vertex o;
	std::memset (&o, 0, sizeof o);
	o.x = clamp16 (x);
	o.y = clamp16 (y);
	o.z = clamp16 (z);
	const float l = std::sqrt (nx * nx + ny * ny + nz * nz);
	if (l > 0.0f)
	{
		o.nx = clamp16 (nx / l * 32767.0f);
		o.ny = clamp16 (ny / l * 32767.0f);
		o.nz = clamp16 (nz / l * 32767.0f);
	}
	o.u = clamp16 ((float) u);
	o.v = clamp16 ((float) v);
	vertices.push_back (o);
	paint ((int) vertices.size () - 1, (int) vertices.size (), m);
	return (int) vertices.size () - 1;
}

void Mesh::paint (int from, int to, const Material& m)
{
	const Rgb c = rgb565 (m.color);
	for (int i = from; i < to; i++)
	{
		Vertex& o = vertices[i];
		o.r = c.r;
		o.g = c.g;
		o.b = c.b;
		o.a = m.alpha;
		o.diffuse = m.diffuse;
		o.specular = m.specular;
		o.lit = m.lit;
		o.gloss = m.gloss;
	}
}

void Mesh::triangle (const float a[3], const float b[3], const float c[3], const Material& m, const int (*uv)[2])
{
	const float e[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, g[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
	const float n[3] = {e[1] * g[2] - e[2] * g[1], e[2] * g[0] - e[0] * g[2], e[0] * g[1] - e[1] * g[0]};
	const float* p[3] = {a, b, c};
	for (int i = 0; i < 3; i++)
		add (p[i][0], p[i][1], p[i][2], n[0], n[1], n[2], m, uv ? uv[i][0] : 0, uv ? uv[i][1] : 0);
}

void Mesh::quad (const float a[3], const float b[3], const float c[3], const float d[3], const Material& m,
		 const int (*uv)[2])
{
	if (uv)
	{
		const int first[3][2] = {{uv[0][0], uv[0][1]}, {uv[1][0], uv[1][1]}, {uv[2][0], uv[2][1]}};
		const int second[3][2] = {{uv[0][0], uv[0][1]}, {uv[2][0], uv[2][1]}, {uv[3][0], uv[3][1]}};
		triangle (a, b, c, m, first);
		triangle (a, c, d, m, second);
		return;
	}
	triangle (a, b, c, m);
	triangle (a, c, d, m);
}

// the faces as Jet's Primitives::createCube has them: each seen from outside
// runs bottom left, bottom right, top right, top left (its texture upright)
void Mesh::box (float cx, float cy, float cz, float w, float h, float d, const Material& m, bool textured)
{
	const float x0 = cx - w / 2, x1 = cx + w / 2, y0 = cy - h / 2, y1 = cy + h / 2, z0 = cz - d / 2, z1 = cz + d / 2;
	const float face[6][4][3] =
	{
		{{x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}},	// +z
		{{x1, y0, z0}, {x0, y0, z0}, {x0, y1, z0}, {x1, y1, z0}},	// -z
		{{x0, y0, z0}, {x0, y0, z1}, {x0, y1, z1}, {x0, y1, z0}},	// -x
		{{x1, y0, z1}, {x1, y0, z0}, {x1, y1, z0}, {x1, y1, z1}},	// +x
		{{x0, y1, z1}, {x1, y1, z1}, {x1, y1, z0}, {x0, y1, z0}},	// +y
		{{x0, y0, z0}, {x1, y0, z0}, {x1, y0, z1}, {x0, y0, z1}},	// -y
	};
	static const int uv[4][2] = {{0, 1024}, {1024, 1024}, {1024, 0}, {0, 0}};
	static const int back[4][2] = {{1024, 1024}, {0, 1024}, {0, 0}, {1024, 0}};	// (createCube's back face: mirrored)
	for (int f = 0; f < 6; f++)
		quad (face[f][0], face[f][1], face[f][2], face[f][3], m, !textured ? nullptr : f == 1 ? back : uv);
}

void Mesh::grid (int w, int d, int rows, int columns, const Material& m, const Material& m2)
{
	const int row_step = d / rows, column_step = w / columns;
	for (int r = 0; r < rows - 1; r++)
		for (int c = 0; c < columns - 1; c++)
		{
			const float x0 = (float) (c * column_step - w / 2), x1 = x0 + column_step;
			const float z0 = (float) (r * row_step - d / 2), z1 = z0 + row_step;
			const float p[4][3] = {{x0, 0, z0}, {x1, 0, z0}, {x1, 0, z1}, {x0, 0, z1}};
			const Material& cell = (r + c) % 2 == 0 ? m : m2;
			const int from = (int) vertices.size ();
			quad (p[0], p[1], p[2], p[3], cell);
			for (int i = from; i < (int) vertices.size (); i++)	// (Jet's grid: every normal up)
			{
				vertices[i].nx = vertices[i].nz = 0;
				vertices[i].ny = 32767;
			}
		}
}

void Mesh::plane (int w, int h, int x, int y, int z, const Material& m, int u, int v, int tw, int th)
{
	const float a[3] = {(float) (x - w / 2), (float) (y - h / 2), (float) z};
	const float b[3] = {(float) (x + w / 2), (float) (y - h / 2), (float) z};
	const float c[3] = {(float) (x + w / 2), (float) (y + h / 2), (float) z};
	const float d[3] = {(float) (x - w / 2), (float) (y + h / 2), (float) z};
	const int uv[4][2] = {{u, v + th}, {u + tw, v + th}, {u + tw, v}, {u, v}};
	quad (a, b, c, d, m, uv);
}

void Mesh::upload (GLenum usage)
{
	if (!buffer)
		glGenBuffers (1, &buffer);
	glBindBuffer (GL_ARRAY_BUFFER, buffer);
	state.layout_buffer = UNKNOWN;			// (what is bound is another now)
	if ((int) vertices.size () != uploaded)
		glBufferData (GL_ARRAY_BUFFER, vertices.size () * sizeof (Vertex), vertices.data (), usage);
	else
		glBufferSubData (GL_ARRAY_BUFFER, 0, vertices.size () * sizeof (Vertex), vertices.data ());
	uploaded = (int) vertices.size ();
	if ((int) indices.size () != uploaded_indices)	// (the indices of a mesh that moves stay as they are)
	{
		if (!index_buffer)
			glGenBuffers (1, &index_buffer);
		glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, index_buffer);
		glBufferData (GL_ELEMENT_ARRAY_BUFFER, indices.size () * sizeof (uint16_t), indices.data (), GL_STATIC_DRAW);
		uploaded_indices = (int) indices.size ();
	}
}

void Mesh::hand_over ()
{
	upload ();
	std::vector<Vertex> ().swap (vertices);
	std::vector<uint16_t> ().swap (indices);
	kept = false;
}

void Mesh::free ()
{
	if (buffer)
		glDeleteBuffers (1, &buffer);
	if (index_buffer)
		glDeleteBuffers (1, &index_buffer);
	buffer = index_buffer = 0;
	uploaded = uploaded_indices = 0;
	kept = true;
	std::vector<Vertex> ().swap (vertices);
	std::vector<uint16_t> ().swap (indices);
}

Program program (const void* info, unsigned size, bool phong)
{
	return load_mesh_program (info, size, phong);
}

void begin (const Camera& camera, const Light& light)
{
	frame_view = camera.view ();
	frame_projection = camera.projection ();
	light_intensity = -1.0f;
	ambient[0] = ambient[1] = ambient[2] = 0.0f;
	if (light.on || light.has_ambient)
	{
		// Jet: towards the light (cos e cos a, sin e, cos e sin a), then into view space
		const float a = radians (light.azimuth), e = radians (light.elevation);
		const float world[3] = {std::cos (e) * std::cos (a), std::sin (e), std::cos (e) * std::sin (a)};
		for (int i = 0; i < 3; i++)
			light_view[i] = frame_view.m[i] * world[0] + frame_view.m[4 + i] * world[1] + frame_view.m[8 + i] * world[2];
		light_intensity = light.on ? std::min (light.intensity, 255.0f) : 0.0f;
		// the gloss' half vector: between the light and the eye (the eye is at -z)
		float h[3] = {light_view[0], light_view[1], light_view[2] - 1.0f};
		const float l = std::sqrt (h[0] * h[0] + h[1] * h[1] + h[2] * h[2]);
		for (int i = 0; i < 3; i++)
			light_half[i] = l > 0.0f ? h[i] / l : 0.0f;
		light_color[0] = light.color.r / 255.0f;
		light_color[1] = light.color.g / 255.0f;
		light_color[2] = light.color.b / 255.0f;
		if (light.has_ambient)
		{
			ambient[0] = light.ambient.r;
			ambient[1] = light.ambient.g;
			ambient[2] = light.ambient.b;
		}
	}
}

Mat4 view_projection ()
{
	return frame_projection * frame_view;
}

// ---- drawing into a texture ----------------------------------------------------------------------

namespace {
GLuint capture_fb, capture_texture, capture_depth;
int capture_w, capture_h;
}

void capture_begin (int divide)
{
	flush_quads ();
	const int w = std::max (area_w / divide, 1), h = std::max (area_h / divide, 1);
	if (capture_w != w || capture_h != h)
	{
		if (!capture_fb)
		{
			glGenFramebuffers (1, &capture_fb);
			glGenTextures (1, &capture_texture);
			glGenRenderbuffers (1, &capture_depth);
		}
		bind_texture_now (capture_texture);
		glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		texture_parameters (true, false);
		glBindRenderbuffer (GL_RENDERBUFFER, capture_depth);
		glRenderbufferStorage (GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, w, h);
		glBindFramebuffer (GL_FRAMEBUFFER, capture_fb);
		glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, capture_texture, 0);
		glFramebufferRenderbuffer (GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, capture_depth);
		if (glCheckFramebufferStatus (GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
			std::printf ("kit: no %dx%d texture to draw into\n", w, h);
		capture_w = w;
		capture_h = h;
	}
	glBindFramebuffer (GL_FRAMEBUFFER, capture_fb);
	glViewport (0, 0, w, h);
	glScissor (0, 0, w, h);
	glDepthMask (GL_TRUE);
	state.depth_write = 1;
	glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

GLuint capture_end ()
{
	flush_quads ();
	glBindFramebuffer (GL_FRAMEBUFFER, 0);
	glViewport (area_x, area_y, area_w, area_h);
	glScissor (area_x, area_y, area_w, area_h);
	return capture_texture;
}

void draw (Mesh& mesh, const Draw& how)
{
	flush_quads ();
	if (mesh.kept && (mesh.uploaded != (int) mesh.vertices.size () || (int) mesh.indices.size () != mesh.uploaded_indices))
	{
		mesh.upload ();
		state.layout_buffer = UNKNOWN;		// (the buffers bound are others now)
	}
	const bool indexed = mesh.uploaded_indices > 0;
	const int all = indexed ? mesh.uploaded_indices : mesh.uploaded;
	const int count = how.count < 0 ? all - how.first : how.count;
	if (count <= 0)
		return;
	const bool phong = how.shading == PHONG;
	MeshProgram& p = how.program ? *how.program : phong ? phong_program : mesh_program;
	const Mat4 model_view = how.view_space ? how.model : frame_view * how.model;
	const Mat4 mvp = frame_projection * model_view;
	use_program (p.name);
	// (most uniforms are the same draw after draw: sent when they change)
	if (std::memcmp (p.last_mvp, mvp.m, sizeof mvp.m) != 0)
	{
		glUniformMatrix4fv (p.mvp, 1, GL_FALSE, mvp.m);
		std::memcpy (p.last_mvp, mvp.m, sizeof mvp.m);
	}
	// the model's place before the camera: for the light (and a scene's own program)
	if ((how.program || (how.lit && light_intensity >= 0.0f)) && std::memcmp (p.last_view, model_view.m, sizeof model_view.m) != 0)
	{
		glUniformMatrix4fv (p.view, 1, GL_FALSE, model_view.m);
		std::memcpy (p.last_view, model_view.m, sizeof model_view.m);
	}
	auto set = [&p] (int which, GLint location, float x, float y, float z, float w)
	{
		float* last = p.last[which];
		if (location < 0 || (last[0] == x && last[1] == y && last[2] == z && last[3] == w))
			return;
		last[0] = x;
		last[1] = y;
		last[2] = z;
		last[3] = w;
		glUniform4f (location, x, y, z, w);
	};
	set (0, p.light, light_view[0], light_view[1], light_view[2], light_intensity);
	set (1, p.ambient, ambient[0], ambient[1], ambient[2], how.alpha);
	set (2, p.tex, how.texture ? 1.0f : 0.0f, how.affine ? 1.0f : 0.0f, how.key ? 1.0f : 0.0f, how.zero ? 1.0f : 0.0f);
	set (3, p.tint, how.tint.r / 255.0f, how.tint.g / 255.0f, how.tint.b / 255.0f, 1.0f);
	set (4, p.lod, how.lod_near, how.lod_far, 0.0f, 0.0f);
	set (5, p.half, light_half[0], light_half[1], light_half[2], how.flat_phong ? 0.0f : 1.0f);
	set (6, p.gloss, light_color[0], light_color[1], light_color[2], how.cel);
	bind_texture (how.texture ? how.texture : white);
	set_cull (how.cull);
	set_depth (how.depth_test, how.depth_write);
	set_blend (how.blend == OPAQUE ? BLEND_OFF : how.blend == ADD ? BLEND_ADD : BLEND_ALPHA);

	if (state.layout_buffer != mesh.buffer || state.layout_program != p.name)
	{
		glBindBuffer (GL_ARRAY_BUFFER, mesh.buffer);
		for (GLint i = 0; i < 8; i++)
		{
			if (i == p.pos || i == p.normal || i == p.color || i == p.uv || i == p.material)
				glEnableVertexAttribArray (i);
			else
				glDisableVertexAttribArray (i);
		}
		glVertexAttribPointer (p.pos, 4, GL_SHORT, GL_FALSE, sizeof (Vertex), (void*) 0);
		if (p.normal >= 0)				// (a scene's own program may not use them all)
			glVertexAttribPointer (p.normal, 4, GL_SHORT, GL_TRUE, sizeof (Vertex), (void*) 8);
		if (p.color >= 0)
			glVertexAttribPointer (p.color, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof (Vertex), (void*) 16);
		if (p.uv >= 0)
			glVertexAttribPointer (p.uv, 2, GL_SHORT, GL_FALSE, sizeof (Vertex), (void*) 20);
		if (p.material >= 0)
			glVertexAttribPointer (p.material, 4, GL_UNSIGNED_BYTE, GL_FALSE, sizeof (Vertex), (void*) 24);
		if (indexed)
			glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, mesh.index_buffer);
		state.layout_buffer = mesh.buffer;
		state.layout_program = p.name;
	}
	if (indexed)
		glDrawElements (GL_TRIANGLES, count, GL_UNSIGNED_SHORT, (void*) (uintptr_t) (how.first * 2));
	else
		glDrawArrays (GL_TRIANGLES, how.first, count);
}

void use (Program& program)
{
	flush_quads ();
	use_program (program.name);
}

void own_gl_begin ()
{
	flush_quads ();
}

void own_gl_end ()
{
	forget_state ();
}

// ---- textures and the 2D layer --------------------------------------------------------------------

GLuint texture565 (int width, int height, const uint16_t* pixels, bool linear, bool repeat)
{
	GLuint name;
	glGenTextures (1, &name);
	bind_texture_now (name);
	glPixelStorei (GL_UNPACK_ALIGNMENT, 2);
	glTexImage2D (GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, pixels);
	texture_parameters (linear, repeat);
	return name;
}

GLuint texture_indexed (int width, int height, const uint8_t* indices, const uint16_t* palette, int colours,
			int offset, bool repeat, GLuint name)
{
	std::vector<uint16_t> pixels ((size_t) width * height);
	for (size_t i = 0; i < pixels.size (); i++)
		pixels[i] = palette[(indices[i] + offset) % colours];
	if (!name)
		return texture565 (width, height, pixels.data (), false, repeat);
	bind_texture_now (name);
	glPixelStorei (GL_UNPACK_ALIGNMENT, 2);
	glTexSubImage2D (GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, pixels.data ());
	return name;
}

GLuint texture (const Image& image, bool linear, bool repeat)
{
	if (!image.keyed)
		return texture565 (image.width, image.height, image.pixels, linear, repeat);
	// the holes clear; their colour that of a neighbour that isn't one, so that
	// a filtered edge doesn't darken towards the key
	const int n = image.width * image.height;
	std::vector<uint8_t> rgba ((size_t) n * 4);
	for (int i = 0; i < n; i++)
	{
		uint16_t p = image.pixels[i];
		const bool hole = p == image.key;
		if (hole)
		{
			const int x = i % image.width, y = i / image.width;
			static const int near[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {-1, -1}, {1, -1}, {-1, 1}};
			for (const auto& d : near)
			{
				const int nx = x + d[0], ny = y + d[1];
				if (nx >= 0 && ny >= 0 && nx < image.width && ny < image.height
				    && image.pixels[ny * image.width + nx] != image.key)
				{
					p = image.pixels[ny * image.width + nx];
					break;
				}
			}
		}
		const Rgb c = rgb565 (p);
		rgba[4 * i + 0] = c.r;
		rgba[4 * i + 1] = c.g;
		rgba[4 * i + 2] = c.b;
		rgba[4 * i + 3] = hole ? 0 : 255;
	}
	GLuint name;
	glGenTextures (1, &name);
	bind_texture_now (name);
	glPixelStorei (GL_UNPACK_ALIGNMENT, 4);
	glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, image.width, image.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data ());
	texture_parameters (linear, repeat);
	return name;
}

GLuint mask (int width, int height, const uint8_t* bits)
{
	std::vector<uint8_t> alpha ((size_t) width * height);
	for (size_t i = 0; i < alpha.size (); i++)
		alpha[i] = bits[i / 8] & (0x80 >> (i % 8)) ? 255 : 0;
	GLuint name;
	glGenTextures (1, &name);
	bind_texture_now (name);
	glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D (GL_TEXTURE_2D, 0, GL_ALPHA, width, height, 0, GL_ALPHA, GL_UNSIGNED_BYTE, alpha.data ());
	texture_parameters (area_w != W, false);
	return name;
}

void sprite_mask (GLuint tex, int width, int height, int x, int y, uint16_t color, int alpha)
{
	if (alpha <= 0)
		return;
	const Rgb c = rgb565 (color);
	const uint8_t rgba[4] = {c.r, c.g, c.b, (uint8_t) std::min (alpha, 255)};
	draw_quad (tex, false, BLENDED, (float) x, (float) y, (float) (x + width), (float) (y + height), 0, 0, 1, 1, rgba, true);
}

void sprite (GLuint tex, int width, int height, int x, int y, int alpha, bool additive, int scale, unsigned flags)
{
	if (alpha <= 0 || scale <= 0)
		return;
	const uint8_t rgba[4] = {255, 255, 255, (uint8_t) std::min (alpha, 255)};
	// MIRROR_X/Y append a flipped copy (up to four quads); FLIP_X/Y turn the whole round
	const bool mx = flags & MIRROR_X, my = flags & MIRROR_Y, fx = flags & FLIP_X, fy = flags & FLIP_Y;
	const int w = width * scale, h = height * scale, ew = mx ? 2 * w : w, eh = my ? 2 * h : h;
	// at the scene's own size a bitmap's texels are pixels: cut the holes; scaled, blend their edges
	const bool exact = area_w == W;
	for (int qy = 0; qy < (my ? 2 : 1); qy++)
		for (int qx = 0; qx < (mx ? 2 : 1); qx++)
		{
			int x0 = qx * w, y0 = qy * h;
			float u0 = qx ? 1 : 0, u1 = qx ? 0 : 1, v0 = qy ? 1 : 0, v1 = qy ? 0 : 1;
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
			draw_quad (tex, exact, additive ? ADDED : !exact || alpha < 255 ? BLENDED : SOLID, (float) (x + x0), (float) (y + y0),
				   (float) (x + x0 + w), (float) (y + y0 + h), u0, v0, u1, v1, rgba);
		}
}

void rect (int x, int y, int w, int h, uint16_t color, int alpha, bool additive)
{
	const Rgb c = rgb565 (color);
	const uint8_t rgba[4] = {c.r, c.g, c.b, (uint8_t) alpha};
	draw_quad (0, false, additive ? ADDED : alpha < 255 ? BLENDED : SOLID, (float) x, (float) y, (float) (x + w), (float) (y + h), 0, 0, 0, 0, rgba);
}

void background (uint16_t color)
{
	const uint8_t rgba[4] = {rgb565 (color).r, rgb565 (color).g, rgb565 (color).b, 255};
	draw_quad (0, false, SOLID, 0, 0, W, H, 0, 0, 0, 0, rgba);
}

void background (const uint16_t* rows)
{
	if (!gradient_texture || std::memcmp (rows, gradient_copy, sizeof gradient_copy) != 0)
	{
		std::memcpy (gradient_copy, rows, sizeof gradient_copy);
		if (!gradient_texture)
			glGenTextures (1, &gradient_texture);
		bind_texture_now (gradient_texture);
		glPixelStorei (GL_UNPACK_ALIGNMENT, 2);
		glTexImage2D (GL_TEXTURE_2D, 0, GL_RGB, 1, H, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, gradient_copy);
		texture_parameters (false, false);
	}
	static const uint8_t plain[4] = {255, 255, 255, 255};
	draw_quad (gradient_texture, false, SOLID, 0, 0, W, H, 0, 0, 1, 1, plain);
}

// Jet's CRT effect darkens the odd rows of its 320. Here a row is as many of
// the real screen's as the picture is scaled by (rounded: whole rows, no moire).
// Only the dark rows are drawn, as quads multiplied into the picture, from a
// buffer made once: blending every pixel of the screen would cost the GPU twice that
void scanlines (int intensity)
{
	static GLuint buffer;
	static int made_intensity = -1, made_height = -1, stripes;
	flush_quads ();
	if (made_intensity != intensity || made_height != area_h)
	{
		const int thick = std::max (1, (area_h + H / 2) / H);
		const float row = (float) H / area_h;		// a real row, in the scene's
		const uint8_t gain = (uint8_t) (255 - intensity);
		stripes = area_h / (2 * thick);
		std::vector<SpriteVertex> v ((size_t) stripes * 6);
		for (int i = 0; i < stripes; i++)
		{
			const float y0 = (2 * i + 1) * thick * row, y1 = y0 + thick * row;
			const float c[6][2] = {{0, y0}, {(float) W, y0}, {(float) W, y1}, {0, y0}, {(float) W, y1}, {0, y1}};
			for (int k = 0; k < 6; k++)
				v[(size_t) i * 6 + k] = {c[k][0], c[k][1], 0, 0, {gain, gain, gain, 255}};
		}
		if (!buffer)
			glGenBuffers (1, &buffer);
		glBindBuffer (GL_ARRAY_BUFFER, buffer);
		glBufferData (GL_ARRAY_BUFFER, v.size () * sizeof (SpriteVertex), v.data (), GL_STATIC_DRAW);
		made_intensity = intensity;
		made_height = area_h;
	}
	use_program (sprite_program);
	const float mode[3] = {0, 0, 0};
	if (std::memcmp (mode, sprite_mode, sizeof mode) != 0)
	{
		glUniform4f (s_mode, 0.0f, 0.0f, 0.0f, 0.0f);
		std::memcpy (sprite_mode, mode, sizeof mode);
	}
	bind_texture (white);
	set_depth (false, false);
	set_cull (NONE);
	set_blend (BLEND_MULTIPLY);
	glBindBuffer (GL_ARRAY_BUFFER, buffer);
	for (GLint i = 0; i < 8; i++)
	{
		if (i == s_pos || i == s_uv || i == s_color)
			glEnableVertexAttribArray (i);
		else
			glDisableVertexAttribArray (i);
	}
	glVertexAttribPointer (s_pos, 2, GL_FLOAT, GL_FALSE, sizeof (SpriteVertex), (void*) 0);
	glVertexAttribPointer (s_uv, 2, GL_FLOAT, GL_FALSE, sizeof (SpriteVertex), (void*) 8);
	glVertexAttribPointer (s_color, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof (SpriteVertex), (void*) 16);
	glDrawArrays (GL_TRIANGLES, 0, stripes * 6);
	glBindBuffer (GL_ARRAY_BUFFER, 0);
	state.layout_program = state.layout_buffer = UNKNOWN;
}

GLuint gradient ()
{
	return gradient_texture;
}

void area (int* x, int* y, int* w, int* h)
{
	*x = area_x;
	*y = area_y;
	*w = area_w;
	*h = area_h;
}

// ---- the program ----------------------------------------------------------------------------------

namespace {

void setup ()
{
	mesh_program = load_mesh_program (&kit_mesh_info, sizeof kit_mesh_info, false);
	phong_program = load_mesh_program (&kit_phong_info, sizeof kit_phong_info, true);
	sprite_program = glCreateProgram ();
	glProgramBinaryOES (sprite_program, PGL_PROGRAM_BINARY_PGPU, &kit_sprite_info, sizeof kit_sprite_info);
	s_pos = glGetAttribLocation (sprite_program, "a_pos");
	s_uv = glGetAttribLocation (sprite_program, "a_uv");
	s_color = glGetAttribLocation (sprite_program, "a_color");
	s_scale = glGetUniformLocation (sprite_program, "u_scale");
	s_mode = glGetUniformLocation (sprite_program, "u_mode");
	glUseProgram (sprite_program);
	glUniform2f (s_scale, 2.0f / W, -2.0f / H);
	glUniform1i (glGetUniformLocation (sprite_program, "u_texture"), 0);
	static const uint8_t one[4] = {255, 255, 255, 255};
	glGenTextures (1, &white);
	bind_texture_now (white);
	glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, one);
	texture_parameters (false, false);
	glDisable (GL_DITHER);
}

// the scene's frame: the screen cleared, the scene's place on it, its picture
void frame (const Scene& scene)
{
	pglSamples (4);				// the V3D's antialiasing
	GLint vp[4];
	glGetIntegerv (GL_VIEWPORT, vp);
	glDisable (GL_SCISSOR_TEST);
	glDepthMask (GL_TRUE);
	glClearColor (0.0f, 0.0f, 0.0f, 1.0f);
	glClearDepthf (1.0f);
	glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	place ();
	glViewport (area_x, area_y, area_w, area_h);
	glScissor (area_x, area_y, area_w, area_h);
	glEnable (GL_SCISSOR_TEST);
	forget_state ();
	scene.draw ();
	flush_quads ();
	glDisable (GL_SCISSOR_TEST);
	glDisable (GL_BLEND);
	glDisable (GL_DEPTH_TEST);
	glDisable (GL_CULL_FACE);
	glViewport (vp[0], vp[1], vp[2], vp[3]);
}

#if JET_SHOT
// JET_SHOT=SECONDS:FILE (a PC): the scene at that moment, as the original's
// capture tool (JetExamples tools/screenshots/capture.cpp) makes its pictures:
// the same seed, the timeline in steps of 1/60 s; drawn off screen at the
// scene's own size and written to FILE as a PPM
void shot (const char* what, const Scene& scene)
{
	const char* colon = std::strchr (what, ':');
	const int last = std::max (2, (int) std::lround (std::atof (what) * 60));
	if (!colon || !pglInitSurface (W, H))
	{
		std::printf ("kit: JET_SHOT=SECONDS:FILE, and the RPi\n");
		std::exit (2);
	}
	capturing = true;
	setup ();
	std::srand (123);
	scene.init ();
	for (int f = 1; f <= last; f++)
	{
		scene.update (1.0f / 60);
		if (f >= last - 2)			// (the frames before settle what a frame takes of the last one)
		{
			frame (scene);
			if (f < last)
				pglSwapBuffers ();
		}
	}
	if (last < 3)
		frame (scene);
	std::vector<uint8_t> pixels ((size_t) W * H * 4);
	glPixelStorei (GL_PACK_ALIGNMENT, 1);
	for (int i = 0; i < 20; i++)
		glFinish ();
	while (glGetError ())
	{
	}
	for (int y = 0; y < H; y += 16)
		glReadPixels (0, y, W, std::min (16, H - y), GL_RGBA, GL_UNSIGNED_BYTE, &pixels[(size_t) y * W * 4]);
	const GLenum e = glGetError ();
	FILE* out = std::fopen (colon + 1, "wb");
	if (!out || e)
	{
		uint32_t rpi[3];
		pglGetRPiError (rpi);
		std::printf ("kit: no picture (GL error 0x%x; the RPi's: %u, command 0x%X, detail %u)\n", (unsigned) e,
			     (unsigned) rpi[0], (unsigned) rpi[1], (unsigned) rpi[2]);
		std::exit (1);
	}
	std::fprintf (out, "P6\n%d %d\n255\n", W, H);
	for (int y = H - 1; y >= 0; y--)
		for (int x = 0; x < W; x++)
			std::fwrite (&pixels[((size_t) y * W + x) * 4], 1, 3, out);
	std::fclose (out);
	uint32_t rpi[3];
	pglGetRPiError (rpi);
	std::printf ("kit: %s at %.3f s (the RPi's last error: %u, command 0x%X)\n", colon + 1, last / 60.0f,
		     (unsigned) rpi[0], (unsigned) rpi[1]);
	std::exit (0);
}
#endif

}  // namespace

int run (const Scene& scene)
{
	stdio_init_all ();
	std::printf ("\njet: %s\n", scene.name);
	pgpu_init ();
	while (!pgpu_wait_ready (1000))
	{
	}
	pgpu_set_reply_phase (1);
#if JET_SHOT
	if (const char* what = std::getenv ("JET_SHOT"))
		shot (what, scene);
#endif
	int tries = 0;
	while (!pglInit () && ++tries < 5)	/* the first reply can be missed */
	{
	}
	if (!hud_init ())
		std::printf ("jet: the HUD program didn't link\n");
	setup ();
	scene.init ();
	while (glGetError ())			// (what the RPi said of the program before this one, cut off mid-stream)
	{
	}

	perf_t perf;
	std::memset (&perf, 0, sizeof perf);
	bool numbers = true, new_perf = true;
	char caption[48] = "";
	absolute_time_t last = get_absolute_time ();
	unsigned windows = 0;
	uint32_t update_us = 0, draw_us = 0, frames = 0;
	// One frame in flight: the host makes the next frame while the RPi renders this one (it waits for
	// the frame before the one it has just sent). The RPi's FRAME pulses count the frames shown; over
	// USB there are none: the RPi is asked
	const bool pulses = std::strcmp (pgpu_link_name (), "USB") != 0;
	uint32_t sent = 0, shown_base = pgpu_frame_count ();
	for (;;)
	{
		const absolute_time_t now = get_absolute_time ();
		float elapsed = absolute_time_diff_us (last, now) / 1e6f;
		last = now;
		elapsed = elapsed <= 0.0f ? 0.001f : elapsed > 0.25f ? 0.25f : elapsed;
		for (int c; (c = getchar_timeout_us (0)) != PICO_ERROR_TIMEOUT; )
		{
			if (c == 'h' || c == 'H')
			{
				numbers = !numbers;
				new_perf = true;
			}
		}
		const absolute_time_t update_start = get_absolute_time ();
		scene.update (elapsed);
		const absolute_time_t draw_start = get_absolute_time ();
		frame (scene);
		update_us += (uint32_t) absolute_time_diff_us (update_start, draw_start);
		draw_us += (uint32_t) absolute_time_diff_us (draw_start, get_absolute_time ());
		frames++;
		const char* line = scene.caption ? scene.caption () : "";
		if (std::strncmp (line, caption, sizeof caption - 1) != 0)
		{
			std::snprintf (caption, sizeof caption, "%s", line);
			new_perf = true;
		}
		if (new_perf)
		{
			GLint vp[4];
			glGetIntegerv (GL_VIEWPORT, vp);
			const float ps = hud_perf_scale (vp[3]);
			hud_begin ();
			if (numbers)
				hud_perf (vp[2] - hud_perf_width (ps) - 2, 2, ps, &perf);
			if (caption[0])
				hud_text_scaled (4, vp[3] - 24 * ps, caption, HUD_RGBA (255, 255, 255, 200), ps);
			hud_end ();
		}
		hud_draw ();
		pglSwapBuffers ();
		sent++;
		const absolute_time_t wait_start = get_absolute_time ();
		if (!pulses)
		{
			pgpu_wait_frame (100);
		}
		else
		{
			while (pgpu_frame_count () - shown_base + 1 < sent)
				if (!pgpu_wait_frame (100))
				{
					shown_base = pgpu_frame_count () + 1 - sent;	// (a pulse was missed)
					break;
				}
		}
		new_perf = perf_frame ((uint32_t) absolute_time_diff_us (wait_start, get_absolute_time ()), &perf);
		if (new_perf && ++windows % 5 == 0)
		{
			// (the host's own time a frame: the scene's motion; its GL calls)
			const GLenum e = glGetError ();
			std::printf ("jet: %.1f frames a second, GPU %.0f%%, render %.2f ms; here: update %.2f ms, draw %.2f ms a frame; GL error 0x%x\n",
				     perf.fps, perf.gpu * 100, perf.render_ms, update_us / 1000.0f / frames, draw_us / 1000.0f / frames, (unsigned) e);
			update_us = draw_us = frames = 0;
		}
	}
}

}  // namespace kit
