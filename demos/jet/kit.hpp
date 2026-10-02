// kit.hpp - what the scenes in this directory share (README.md). They are
// CubeCoders' JetExamples (github.com/CubeCoders/JetExamples, MIT), each
// written again for piegpu's OpenGL ES: the meshes in vertex buffers, moved
// and lit by the RPi's vertex shaders, a depth buffer, hardware antialiasing
// (4x MSAA). Nothing of the Jet engine runs here; what a scene needs of its
// conventions is kept, so that a scene's numbers (its positions, angles,
// colours, its timeline) are the original's:
//
//  - units and angles: integers, degrees; a mesh turns about x, then y, then
//    z; the camera looks along +z, y up, and turns the other way round;
//  - the screen is 480 x 320, the size the scenes were made for: drawn as
//    large as fits on the real one, the captions (the original's bitmaps)
//    scaled with it;
//  - light: Jet's formula (a squared Lambert term, a view-facing specular
//    one, ambient added per channel, above full a colour blows out towards
//    white), worked out by the vertex shader, or per pixel (Phong, with
//    Jet's glossy highlight, and the cel bands);
//  - a draw costs a small host a few packets on the link: the kit remembers
//    the GL state and the uniforms it left and sends what changes, gathers
//    the 2D layer's quads, and keeps one frame in flight (the host makes the
//    next frame while the RPi renders this one);
//  - a scene's moments: a PC build with JET_SHOT=SECONDS:FILE in the
//    environment runs the scene to that moment in steps of 1/60 s, as the
//    original's own capture tool does, and writes the picture (tools/compare.py
//    puts the two side by side).
#pragma once

#include <cstdint>
#include <vector>
#include <sys/types.h>
#include "pgl.h"
#undef quad					// (newlib's <sys/types.h> has a macro of that name: taken in here, once)

namespace kit {

constexpr int W = 480, H = 320;			// the scenes' screen

// ---- colours, as the scenes write them -------------------------------------------------

struct Rgb { uint8_t r, g, b; };
inline Rgb rgb565 (uint16_t c)
{
	return {(uint8_t) (((c >> 11) & 31) * 255 / 31), (uint8_t) (((c >> 5) & 63) * 255 / 63), (uint8_t) ((c & 31) * 255 / 31)};
}
inline uint16_t to565 (unsigned rgb888)		// 0xRRGGBB
{
	return (uint16_t) (((rgb888 >> 19) & 31) << 11 | ((rgb888 >> 10) & 63) << 5 | ((rgb888 >> 3) & 31));
}

// ---- matrices: column major, as GL takes them ----------------------------------------------

struct Mat4 { float m[16]; };
Mat4 identity ();
Mat4 operator* (const Mat4& a, const Mat4& b);
Mat4 translation (float x, float y, float z);
Mat4 scaling (float x, float y, float z);
/// a mesh's turn: about x, then y, then z (degrees)
Mat4 rotation (float x, float y, float z);

struct Camera
{
	float x = 0, y = 0, z = 0;		// where it is
	float pitch = 0, yaw = 0, roll = 0;	// degrees, about x, y, z
	float fov = 60;				// across the screen's width, degrees
	float near_plane = 128, far_plane = 1024;
	/// \brief towards a point (pitch and yaw; no roll)
	void look_at (float tx, float ty, float tz);
	Mat4 view () const;
	Mat4 projection () const;
	/// \brief a point of the world on the screen (480 x 320, y down); false if behind
	bool project (float px, float py, float pz, float* sx, float* sy, float* depth = nullptr) const;
	/// \brief a point of the screen, so far ahead, in the camera's space
	void unproject (float sx, float sy, float depth, float out[3]) const;
};

// ---- meshes ---------------------------------------------------------------------------------

struct Material
{
	uint16_t color = 0xFFFF;		// RGB565
	uint8_t alpha = 255;
	uint8_t diffuse = 255, specular = 0;	// the light's coefficients
	bool lit = true;			// false: its colour as it is (emissive, unlit, additive)
	uint8_t gloss = 0;			// Phong only: the highlight's exponent (0: Jet's broad one)
	Material () {}
	Material (uint16_t c, uint8_t a = 255, uint8_t d = 255, uint8_t s = 0, bool l = true)
	:	color (c), alpha (a), diffuse (d), specular (s), lit (l) {}
};

struct Vertex					// 28 bytes, as the programs take them
{
	int16_t x, y, z, pad;			// pad 1: no texture on it, in a textured draw
	int16_t nx, ny, nz, pad2;		// 32767 = 1
	uint8_t r, g, b, a;
	int16_t u, v;				// 1024 = one texture
	uint8_t diffuse, specular, lit, gloss;
};

struct Mesh
{
	std::vector<Vertex> vertices;
	std::vector<uint16_t> indices;		// empty: the vertices three by three
	GLuint buffer = 0, index_buffer = 0;
	int uploaded = 0, uploaded_indices = 0;
	bool kept = true;			// false: the RPi has it, the copy here is gone (keep ())

	/// \brief a vertex: where, its normal (any length), its texture's place (1024 = one texture)
	int add (float x, float y, float z, float nx, float ny, float nz, const Material& m, int u = 0, int v = 0);
	/// \brief a triangle of three points, its normal the face's (flat): for a mesh without indices
	void triangle (const float a[3], const float b[3], const float c[3], const Material& m,
		       const int (*uv)[2] = nullptr);
	/// \brief ... of four: a b c, a c d
	void quad (const float a[3], const float b[3], const float c[3], const float d[3], const Material& m,
		   const int (*uv)[2] = nullptr);
	/// \brief a box about a centre, its faces flat; with texture coordinates: a whole texture a face
	void box (float cx, float cy, float cz, float w, float h, float d, const Material& m, bool textured = false);
	/// \brief Jet's createGrid: rows x columns of points on the ground (y = 0) from the corner -w/2, -d/2,
	///	   w/columns and d/rows apart (so it ends a step short of the far sides); the cells two materials in turn
	void grid (int w, int d, int rows, int columns, const Material& m, const Material& m2);
	/// \brief an upright rectangle about a point, facing the camera's side (-z), a texture upright on it:
	///	   the part of it from u, v, so wide and high (1024 = all of it)
	void plane (int w, int h, int x, int y, int z, const Material& m, int u = 0, int v = 0, int tw = 1024, int th = 1024);
	/// \brief the material's colour and light anew in vertices from..to (it changed)
	void paint (int from, int to, const Material& m);
	/// \brief to the RPi (again, if it's there already: it has changed)
	void upload (GLenum usage = GL_STATIC_DRAW);
	/// \brief ... and the copy here given up (a mesh that won't change: the memory is the host's)
	void hand_over ();
	/// \brief the RPi's buffers given back
	void free ();
	void clear ()				{ vertices.clear (); indices.clear (); }
};

/// a program of the scene's own in place of the kit's: its vertex shader takes the same attributes and
/// gives the same varyings (the fragment shader is kit_mesh's), so that a mesh can be shaped on the GPU
struct Program
{
	GLuint name = 0;
	GLint pos, normal, color, uv, material;
	GLint mvp, view, light, ambient, tex, tint, lod, half, gloss;
	float last[7][4], last_mvp[16], last_view[16];	// (what its uniforms were given last: sent again only if changed)
};
Program program (const void* info, unsigned size, bool phong = false);

enum Blend { OPAQUE, ALPHA, ADD };
enum Cull { BACK, FRONT, NONE };
enum Shading { VERTEX, PHONG };

struct Draw					// how a mesh is drawn
{
	Mat4 model = identity ();
	bool view_space = false;		// the mesh is in the camera's space already (model: into it)
	GLuint texture = 0;			// 0: the vertices' colours
	bool key = false;			// its alpha 0 texels are holes
	bool affine = false;			// texture coordinates over the screen as they are (no perspective)
	bool zero = false;			// outside the texture: colour 0 (a hole, with key)
	float lod_near = 0, lod_far = 0;	// the texture fades into the material's colour between these distances
	Blend blend = OPAQUE;
	Cull cull = BACK;
	bool depth_test = true, depth_write = true;
	float alpha = 1.0f;			// over the materials' own
	Rgb tint = {255, 255, 255};		// ... and a colour over theirs (one mesh in several colours)
	Shading shading = VERTEX;
	float cel = 1.0f;			// PHONG: the brightness in steps of so much (1: smooth)
	bool flat_phong = false;		// PHONG program, but the vertices' light (for the cel bands on Gouraud)
	int first = 0, count = -1;		// vertices (or indices); -1: all
	Program* program = nullptr;		// the scene's own (its other uniforms set before, after kit::use)
	bool lit = true;			// false: none of its vertices is lit (a hint: less to send)
};

struct Light
{
	bool on = false;			// off: every colour as it is
	float azimuth = 0, elevation = 0;	// where it shines from, degrees
	Rgb color = {255, 255, 255};
	float intensity = 255;
	Rgb ambient = {0, 0, 0};
	bool has_ambient = false;
};

/// \brief the frame's camera and light: before the meshes
void begin (const Camera& camera, const Light& light);
void draw (Mesh& mesh, const Draw& how);
/// \brief a scene's own program made current, for setting its other uniforms (the kit keeps track of the
///	   GL state it left: a scene tells it of what it changes itself)
void use (Program& program);
/// \brief around GL calls of the scene's own: what the kit had gathered is drawn first; and what state
///	   they leave is taken as unknown
void own_gl_begin ();
void own_gl_end ();
/// \brief the frame's camera: a point of the world into clip space
Mat4 view_projection ();

/// \brief what's drawn from here to capture_end goes into a texture (for an effect that needs the
///	   picture: a reflection), not to the screen
/// \param divide the texture is the scene's size on the screen divided by this (a reflection in
///	   rippled water needs no more than half)
void capture_begin (int divide = 1);
/// \return the texture (its first row the picture's bottom, as GL has it)
GLuint capture_end ();

// ---- textures and the 2D layer --------------------------------------------------------------

struct Image					// a scene's bitmap: RGB565, one colour may be its holes
{
	int width, height;
	const uint16_t* pixels;
	bool keyed;
	uint16_t key;
};

/// \brief a GL texture of it (RGB565, or RGBA with the holes clear); filtered or not
GLuint texture (const Image& image, bool linear = false, bool repeat = false);
GLuint texture565 (int width, int height, const uint16_t* pixels, bool linear = false, bool repeat = false);
/// \brief ... of byte indices into a palette, every index moved on by offset (the palette's cycling);
///	   into name, if it's there already
GLuint texture_indexed (int width, int height, const uint8_t* indices, const uint16_t* palette, int colours,
			int offset = 0, bool repeat = false, GLuint name = 0);

/// \brief a texture of a bitmap of single bits (a row after another, the high bit first): its shape, in any colour
GLuint mask (int width, int height, const uint8_t* bits);
/// \brief ... on the screen, in a colour
void sprite_mask (GLuint texture, int width, int height, int x, int y, uint16_t color, int alpha = 255);

enum SpriteFlags { FLIP_X = 1, FLIP_Y = 2, MIRROR_X = 4, MIRROR_Y = 8 };

/// \brief a bitmap on the screen (480 x 320, y down), over what's drawn: at x, y, scale times its size
void sprite (GLuint texture, int width, int height, int x, int y, int alpha = 255, bool additive = false,
	     int scale = 1, unsigned flags = 0);
/// \brief a rectangle of one colour
void rect (int x, int y, int w, int h, uint16_t color, int alpha = 255, bool additive = false);
/// \brief the background: one colour, or one a row (320 of them)
void background (uint16_t color);
void background (const uint16_t* rows);
/// \brief the last background's rows as a texture (1 x 320, the top first)
GLuint gradient ();
/// \brief a picture tube's lines over what's drawn: every other row darker by so much (of 255)
void scanlines (int intensity);

// ---- the program ------------------------------------------------------------------------------

struct Scene
{
	const char* name;
	void (*init) ();
	void (*update) (float seconds);
	void (*draw) ();			// the frame: background, begin, the meshes, the sprites
	const char* (*caption) () = nullptr;	// a line under the picture (upper case), if it has one to say
};

/// \brief runs it for good (or, with JET_SHOT, to its moment)
int run (const Scene& scene);

/// \brief the scene's place on the real screen (GL window coordinates), and the real pixels a scene pixel is
void area (int* x, int* y, int* w, int* h);

}  // namespace kit

#ifdef main	// renamed by the host's build (ESP-IDF: pgpu_app_main) and called from C
extern "C" int main ();
#endif
