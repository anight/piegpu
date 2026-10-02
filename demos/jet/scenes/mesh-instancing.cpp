// mesh-instancing - JetExamples' "Repeat" (esp32-mesh-instancing, MIT,
// CubeCoders) for piegpu's OpenGL: fifteen rotors turning, eight seconds as
// fifteen meshes, each in its colour, eight as one mesh drawn fifteen times,
// a colour and a place given with each draw. The picture is the same; what
// the RPi stores of them is not (the caption says how much: its vertex and
// index buffers).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include "kit.hpp"

namespace {

constexpr int COUNT = 15, COLUMNS = 5;
constexpr float SECONDS_PER_MODE = 8.0f, PI = 3.14159265358979323846f;
const unsigned COLOURS[5] = {0xDF805E, 0xEAC77D, 0xB7D4C2, 0x70B7B6, 0xB2B8DA};

kit::Camera camera;
kit::Light light;
kit::Mesh copies[COUNT], shared;
uint16_t sky[kit::H];
GLuint title, footer, caption;
float seconds_in = 0;
int mode = -1;

// the captions: the original's 5 x 7 letters, written into bitmaps
const uint8_t* glyph (char c)
{
	static const uint8_t letters[][5] =
	{
		{126, 17, 17, 17, 126}, {127, 73, 73, 73, 54}, {62, 65, 65, 65, 34}, {127, 65, 65, 34, 28},
		{127, 73, 73, 73, 65}, {127, 9, 9, 9, 1}, {62, 65, 73, 73, 122}, {127, 8, 8, 8, 127},
		{0, 65, 127, 65, 0}, {32, 64, 65, 63, 1}, {127, 8, 20, 34, 65}, {127, 64, 64, 64, 64},
		{127, 2, 12, 2, 127}, {127, 4, 8, 16, 127}, {62, 65, 65, 65, 62}, {127, 9, 9, 9, 6},
		{62, 65, 81, 33, 94}, {127, 9, 25, 41, 70}, {38, 73, 73, 73, 50}, {1, 1, 127, 1, 1},
		{63, 64, 64, 64, 63}, {31, 32, 64, 32, 31}, {63, 64, 56, 64, 63}, {99, 20, 8, 20, 99},
		{7, 8, 112, 8, 7}, {97, 81, 73, 69, 67},
	};
	static const uint8_t digits[][5] =
	{
		{62, 81, 73, 69, 62}, {0, 66, 127, 64, 0}, {66, 97, 81, 73, 70}, {33, 65, 69, 75, 49},
		{24, 20, 18, 127, 16}, {39, 69, 69, 69, 57}, {60, 74, 73, 73, 48}, {1, 113, 9, 5, 3},
		{54, 73, 73, 73, 54}, {6, 73, 73, 41, 30},
	};
	static const uint8_t blank[5] = {0, 0, 0, 0, 0}, dash[5] = {8, 8, 8, 8, 8}, dot[5] = {0, 96, 96, 0, 0};
	if (c >= 'A' && c <= 'Z')
		return letters[c - 'A'];
	if (c >= '0' && c <= '9')
		return digits[c - '0'];
	return c == '-' ? dash : c == '.' ? dot : blank;
}

struct Label
{
	int width, height;
	std::vector<uint16_t> pixels;
	Label (int w, int h) : width (w), height (h), pixels ((size_t) w * h, 0) {}
	void text (const char* s, int x, int y, int scale = 1, uint16_t colour = 0xFFFF)
	{
		for (; *s; s++, x += 6 * scale)
			for (int xx = 0; xx < 5; xx++)
				for (int yy = 0; yy < 7; yy++)
					if (glyph (*s)[xx] & (1 << yy))
						for (int dy = 0; dy < scale; dy++)
							for (int dx = 0; dx < scale; dx++)
							{
								const int px = x + xx * scale + dx, py = y + yy * scale + dy;
								if (px >= 0 && px < width && py >= 0 && py < height)
									pixels[(size_t) py * width + px] = colour;
							}
	}
	GLuint texture () const
	{
		const kit::Image image = {width, height, pixels.data (), true, 0};
		return kit::texture (image);
	}
};

// a rotor: a ring of twenty teeth, its section five flat faces
void make_rotor (kit::Mesh& mesh, const kit::Material& material)
{
	constexpr int segments = 20;
	auto point = [] (int i, int profile, float out[3])
	{
		const float a = i * 2 * PI / segments, outer = i % 2 ? 58.0f : 65.0f;
		const float radius[5] = {25.0f, outer - 8, outer, outer - 8, 25.0f};
		static const float z[5] = {-16, -16, 0, 16, 16};
		out[0] = std::round (radius[profile] * std::cos (a));
		out[1] = std::round (radius[profile] * std::sin (a));
		out[2] = z[profile];
	};
	for (int i = 0; i < segments; i++)
		for (int p = 0; p < 5; p++)
		{
			float v[4][3];
			point (i, p, v[0]);
			point (i + 1, p, v[1]);
			point (i + 1, (p + 1) % 5, v[2]);
			point (i, (p + 1) % 5, v[3]);
			const float e[3] = {v[1][0] - v[0][0], v[1][1] - v[0][1], v[1][2] - v[0][2]};
			const float g[3] = {v[2][0] - v[0][0], v[2][1] - v[0][1], v[2][2] - v[0][2]};
			const float n[3] = {e[1] * g[2] - e[2] * g[1], e[2] * g[0] - e[0] * g[2], e[0] * g[1] - e[1] * g[0]};
			const int first = (int) mesh.vertices.size ();
			for (const auto& corner : v)
				mesh.add (corner[0], corner[1], corner[2], n[0], n[1], n[2], material);
			for (int k : {0, 1, 2, 2, 3, 0})
				mesh.indices.push_back ((uint16_t) (first + k));
		}
}

kit::Material paint (unsigned hex)
{
	kit::Material m (kit::to565 (hex), 255, 255, 150);
	m.gloss = 24;
	return m;
}

void select (int next)
{
	if (next == mode)
		return;
	mode = next;
	// what the RPi keeps of the rotors: a mesh each, or one
	size_t bytes = 0;
	for (auto& mesh : copies)
	{
		mesh.free ();
		if (!mode)
		{
			make_rotor (mesh, paint (COLOURS[(&mesh - copies) % 5]));
			bytes += mesh.vertices.size () * sizeof (kit::Vertex) + mesh.indices.size () * sizeof (uint16_t);
			mesh.hand_over ();
		}
	}
	shared.free ();
	if (mode)
	{
		make_rotor (shared, paint (0xFFFFFF));
		bytes = shared.vertices.size () * sizeof (kit::Vertex) + shared.indices.size () * sizeof (uint16_t);
		shared.hand_over ();
	}
	Label label (360, 33);
	char text[80];
	std::snprintf (text, sizeof text, mode ? "%d INSTANCES - 1 MESH" : "%d COPIES - %d MESHES", COUNT, COUNT);
	label.text (text, 0, 0, 2, mode ? 0xAEB8 : 0xF5AF);
	std::snprintf (text, sizeof text, "MESH DATA %u.%u KIB - SAME %d ROTORS", (unsigned) (bytes / 1024),
		       (unsigned) (bytes * 10 / 1024 % 10), COUNT);
	label.text (text, 0, 23);
	if (caption)
		glDeleteTextures (1, &caption);
	caption = label.texture ();
	std::printf ("INSTANCING mode=%s objects=%d stored_meshes=%d mesh_bytes=%u\n", mode ? "shared" : "copies", COUNT,
		     mode ? 1 : COUNT, (unsigned) bytes);
}

void update (float seconds)
{
	seconds_in = std::fmod (seconds_in + seconds, SECONDS_PER_MODE * 2);
	select ((int) (seconds_in / SECONDS_PER_MODE) % 2);
}

void init ()
{
	camera.z = -880;
	camera.fov = 57;
	camera.near_plane = 32;
	camera.far_plane = 3000;
	light.on = true;
	light.azimuth = 225;
	light.elevation = 35;
	light.color = {255, 244, 225};
	light.intensity = 235;
	light.ambient = {76, 83, 95};
	light.has_ambient = true;
	for (int y = 0; y < kit::H; y++)
		sky[y] = kit::to565 (((22 + y / 9) << 16) | ((35 + y / 7) << 8) | (49 + y / 6));
	Label heading (300, 34), foot (444, 8);
	heading.text ("REPEAT", 0, 0, 3, 0xF6F8);
	heading.text ("SHARED MESH INSTANCING", 0, 26);
	foot.text ("SAME PICTURE - LESS MEMORY - MEASURE THE COST", 0, 0, 1, 0xBDF7);
	title = heading.texture ();
	footer = foot.texture ();
	update (0);
}

void draw ()
{
	const float local = std::fmod (seconds_in, SECONDS_PER_MODE);
	kit::background (sky);
	kit::begin (camera, light);
	kit::Draw how;
	how.shading = kit::PHONG;
	for (int i = 0; i < COUNT; i++)
	{
		// two turns a mode: the motion goes on as the meshes are changed
		how.model = kit::translation ((float) ((i % COLUMNS - 2) * 145), (float) ((1 - i / COLUMNS) * 130), (float) ((i % 3 - 1) * 20))
			    * kit::rotation ((float) (-18 + (i / COLUMNS) * 12), (float) ((i % COLUMNS - 2) * 11),
					     (float) (((int) std::round (local * 90) + i * 18) % 360));
		how.tint = mode ? kit::rgb565 (kit::to565 (COLOURS[i % 5])) : kit::Rgb {255, 255, 255};
		kit::draw (mode ? shared : copies[i], how);
	}
	kit::rect (0, 0, 480, 64, 0x10E4);
	kit::rect (0, 262, 480, 58, 0x10E4);
	kit::rect (18, 256, (int) (444 * local / SECONDS_PER_MODE), 3, 0xDDAF);
	kit::sprite (title, 300, 34, 18, 12);
	kit::sprite (caption, 360, 33, 18, 270);
	kit::sprite (footer, 444, 8, 18, 308);
}

}  // namespace

int main ()
{
	return kit::run ({"Repeat", init, update, draw});
}
