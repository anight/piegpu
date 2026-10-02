// neon-car - JetExamples' "Neon car" (esp32-neon-car, MIT, CubeCoders) for
// piegpu's OpenGL: a stock car on a workshop's platform, the camera going
// round it. The car is a model (an OBJ, converted by tools/import_assets.py)
// with a paletted livery, its paint lit per pixel with a glossy highlight;
// its windows mirror a picture of a room (environment mapping: where a ray
// from the eye, reflected by the glass, meets a panorama; worked out for the
// windows' few vertices each frame).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include "kit.hpp"
#include "../assets/car.hpp"
#include "../assets/car_model.hpp"
#include "../assets/car_labels.hpp"

namespace {

constexpr float PI = 3.14159265359f;
constexpr int CAR_Y = 52;

kit::Camera camera;
kit::Light light;
kit::Mesh room, ribs, body, glass;
uint16_t sky[kit::H];
GLuint livery, environment, title, caption;
float seconds_in = 0;
std::vector<int> glass_source;			// the model's vertex each of the glass' is

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

void make_room ()
{
	const kit::Material floor = paint (0x19283A), wall = paint (0x141C2F), wall_alt = paint (0x231733), frame = paint (0x26354A);
	const kit::Material cyan = paint (0x32D7EC), pink = paint (0xE542BC), white = paint (0xA4C5D8);
	quad (room, {-2300, 0, -2300}, {2300, 0, -2300}, {2300, 0, 2300}, {-2300, 0, 2300}, floor);
	for (int i = 0; i < 8; i++)
	{
		const float a = 2 * PI * i / 8, b = 2 * PI * (i + 1) / 8;
		const float x = (float) (int) (1900 * std::sin (a)), z = (float) (int) (1900 * std::cos (a));
		const float xx = (float) (int) (1900 * std::sin (b)), zz = (float) (int) (1900 * std::cos (b));
		quad (room, {x, 0, z}, {xx, 0, zz}, {xx, 1000, zz}, {x, 1000, z}, i % 2 ? wall : wall_alt);
	}
	// the floor's lanes, the platform (eight sides' worth of segments), its rim, the car's shadow: each a little higher
	const kit::Material grid = paint (0x2D4E60), deck = paint (0x2A3C51), edge = paint (0x131F2F);
	for (int n = -4; n <= 4; n++)
	{
		const float q = (float) (n * 440);
		quad (room, {q - 2, 1, -1800}, {q + 2, 1, -1800}, {q + 2, 1, 1800}, {q - 2, 1, 1800}, grid);
		quad (room, {-1800, 1, q - 2}, {1800, 1, q - 2}, {1800, 1, q + 2}, {-1800, 1, q + 2}, grid);
	}
	auto at = [] (float a, float radius, float y) { return P {(float) (int) (radius * std::sin (a)), y, (float) (int) (radius * std::cos (a))}; };
	for (int i = 0; i < 32; i++)
	{
		const float a = 2 * PI * i / 32, b = 2 * PI * (i + 1) / 32;
		const P centre = {0, 18, 0}, pa = at (a, 720, 18), pb = at (b, 720, 18);
		room.triangle (&centre.x, &pa.x, &pb.x, deck);
		quad (room, at (a, 720, 0), at (b, 720, 0), at (b, 720, 18), at (a, 720, 18), edge);
		quad (room, at (a, 685, 19), at (a, 694, 19), at (b, 694, 19), at (b, 685, 19), i % 8 < 4 ? cyan : pink);
	}
	quad (room, {-180, 20, -465}, {180, 20, -465}, {180, 20, 465}, {-180, 20, 465}, paint (0x121D2D));
	// the ribs on the walls, their lights: outside the camera's round
	for (int i = 0; i < 8; i++)
	{
		const float a = 2 * PI * i / 8;
		const int x = (int) (1780 * std::sin (a)), z = (int) (1780 * std::cos (a));
		ribs.box ((float) x, 425, (float) z, 45, 850, 45, frame);
		ribs.box ((float) (int) (x * 0.992f), 435, (float) (int) (z * 0.992f), 13, 630, 13, i % 2 ? pink : cyan);
		ribs.box ((float) (int) (x * 0.86f), 855, (float) (int) (z * 0.86f), 220, 15, 120, white);
	}
}

// the model: its vertices mirrored in x (the livery reads the right way then;
// the normals, drawn inwards, turn outwards), a mesh for the paint and the
// wheels, one for the glass
void load_car ()
{
	kit::Material lacquer (0xFFFF, 255, 165, 205);
	lacquer.gloss = 32;
	const kit::Material rubber (0xFFFF, 255, 255, 0, false);
	for (const auto& part : car_model::parts)
	{
		const bool window = std::strstr (part.material, "Window") != nullptr;
		const bool wheel = !std::strcmp (part.material, "01_-_Default.004") || !std::strcmp (part.material, "01_-_Default.005")
				   || !std::strcmp (part.material, "01_-_Default.006");
		kit::Mesh& mesh = window ? glass : body;
		std::vector<int> map (sizeof car_model::vertices / sizeof car_model::vertices[0], -1);
		for (int t = part.first; t < part.first + part.count; t++)
			for (int k = 0; k < 3; k++)
			{
				const int source = car_model::triangles[t][k];
				if (map[source] < 0)
				{
					const car_model::Vertex& v = car_model::vertices[source];
					map[source] = mesh.add ((float) -v.x, v.y, v.z, v.nx, (float) -v.ny, (float) -v.nz,
								wheel || window ? rubber : lacquer, v.u, v.v);
					if (window)
						glass_source.push_back (source);
				}
				mesh.indices.push_back ((uint16_t) map[source]);
			}
	}
	std::printf ("The car: %u vertices, %u triangles; its glass: %u, %u\n", (unsigned) body.vertices.size (),
		     (unsigned) body.indices.size () / 3, (unsigned) glass.vertices.size (), (unsigned) glass.indices.size () / 3);
}

// where the room's panorama is seen in the glass, from the eye: for each of its vertices
void reflect ()
{
	int anchor = 0;
	for (size_t i = 0; i < glass.vertices.size (); i++)
	{
		const car_model::Vertex& source = car_model::vertices[glass_source[i]];
		float n[3] = {(float) source.nx, (float) -source.ny, (float) -source.nz};
		float v[3] = {camera.x + source.x, camera.y - (source.y + CAR_Y), camera.z - source.z};
		const float nn = std::sqrt (n[0] * n[0] + n[1] * n[1] + n[2] * n[2]), vv = std::sqrt (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
		int tu = 512, tv = 512;
		if (nn > 1e-6f && vv > 1e-6f)
		{
			for (int k = 0; k < 3; k++)
			{
				n[k] /= nn;
				v[k] /= vv;
			}
			const float twice = 2 * (n[0] * v[0] + n[1] * v[1] + n[2] * v[2]);
			const float r[3] = {twice * n[0] - v[0], twice * n[1] - v[1], twice * n[2] - v[2]};
			tu = (int) std::lround ((0.5f + std::atan2 (r[0], r[2]) / (2 * PI)) * 1024);
			tv = std::min (std::max ((int) std::lround ((0.5f - std::asin (std::min (std::max (r[1], -1.0f), 1.0f)) / PI) * 1024), 0), 1023);
		}
		if (i == 0)
			anchor = tu;
		while (tu - anchor > 512)		// (a pane across the panorama's seam: on one side of it)
			tu -= 1024;
		while (tu - anchor < -512)
			tu += 1024;
		glass.vertices[i].u = (int16_t) tu;
		glass.vertices[i].v = (int16_t) tv;
	}
	glass.upload (GL_DYNAMIC_DRAW);
}

void update (float seconds)
{
	seconds_in = std::fmod (seconds_in + seconds, 3600.0f);
	const float a = 0.7f + seconds_in * 2 * PI / 20.0f, radius = 1460 + 80 * std::sin (seconds_in * 2 * PI / 40.0f);
	camera.x = (float) (int) (radius * std::sin (a));
	camera.y = (float) (480 + (int) (60 * std::sin (a * 2)));
	camera.z = (float) (int) (radius * std::cos (a));
	camera.look_at (0, 175, 0);
}

void init ()
{
	camera.fov = 62;
	camera.near_plane = 96;
	camera.far_plane = 6000;
	light.on = true;
	light.azimuth = 220;
	light.elevation = 52;
	light.color = {205, 231, 255};
	light.intensity = 255;
	light.ambient = {130, 123, 150};
	light.has_ambient = true;
	make_room ();
	load_car ();
	room.hand_over ();				// (they stay as they are: the RPi has them; the glass changes)
	ribs.hand_over ();
	body.hand_over ();
	livery = kit::texture_indexed (256, 256, car::liveryIndices, car::liveryPalette, 256);
	environment = kit::texture565 (128, 64, car::environment, false, true);
	title = kit::texture (car_labels::title);
	caption = kit::texture (car_labels::caption);
	update (0);
}

void draw ()
{
	kit::background ((uint16_t) kit::to565 (0x0F1827));
	kit::begin (camera, light);
	kit::Draw how;
	how.cull = kit::NONE;
	kit::draw (room, how);
	kit::Draw rib;					// a rib, its light, its lamp: painted one over the other
	rib.depth_test = false;				// (the light's strip lies within the rib)
	kit::draw (ribs, rib);
	kit::Draw car;
	car.model = kit::translation (0, CAR_Y, 0);
	car.shading = kit::PHONG;
	car.texture = livery;
	kit::draw (body, car);
	reflect ();
	car.texture = environment;
	kit::draw (glass, car);
	kit::sprite (title, car_labels::title.width, car_labels::title.height, 20, 17);
	kit::sprite (caption, car_labels::caption.width, car_labels::caption.height, 20, 292);
}

}  // namespace

int main ()
{
	return kit::run ({"Neon car", init, update, draw});
}
