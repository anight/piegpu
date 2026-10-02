// lod-billboards - JetExamples' "Woodland" (esp32-lod-billboards, MIT,
// CubeCoders) for piegpu's OpenGL: a pine the camera backs away from; near,
// its full mesh; further, a simpler one; then it fades into a picture of
// itself turned to the camera (a billboard), as the trees behind it are.
// The second half of the cycle keeps the full mesh, for comparison.
#include <cmath>
#include <cstdio>
#include "kit.hpp"
#include "../assets/woodland.hpp"
#include "../assets/woodland_trees.hpp"
#include "../assets/woodland_labels.hpp"

namespace {

constexpr float PI = 3.14159265359f;
constexpr int LOD_STEP = 1800, FADE_NEAR = 2400, FADE_FAR = 2600;

kit::Camera camera;
kit::Light light;				// none: every colour as it is
kit::Mesh land, high, low, card;
uint16_t sky[kit::H];
GLuint pine, title, mode_label[5], desc_label[2];
float seconds_in = 0;
int stage = -1, distance = 0;
bool reference = false;

kit::Material paint (unsigned hex)
{
	return kit::Material (kit::to565 (hex), 255, 255, 0, false);
}

void quad (kit::Mesh& mesh, const float a[3], const float b[3], const float c[3], const float d[3], const kit::Material& m)
{
	mesh.triangle (a, b, c, m);
	mesh.triangle (c, d, a, m);
}

template <size_t N> void tree (kit::Mesh& mesh, const woodland_trees::Face (&faces)[N])
{
	for (const auto& f : faces)
	{
		const float a[3] = {(float) f.p[0], (float) f.p[1], (float) f.p[2]};
		const float b[3] = {(float) f.p[3], (float) f.p[4], (float) f.p[5]};
		const float c[3] = {(float) f.p[6], (float) f.p[7], (float) f.p[8]};
		mesh.triangle (a, b, c, kit::Material (f.colour, 255, 255, 0, false));
	}
}

void update (float seconds)
{
	static const char* const names[] = {"FULL MESH", "SIMPLE MESH", "TRANSITION", "BILLBOARD", "REFERENCE FULL MESH"};
	seconds_in = std::fmod (seconds_in + seconds, 32.0f);
	reference = seconds_in >= 16;
	const float phase = std::fmod (seconds_in, 16.0f), a = phase * 2 * PI / 16;
	const float radius = 1250 + 2050 * (0.5f - 0.5f * std::cos (a)), angle = 0.30f * std::sin (a);
	camera.x = (float) (int) (radius * std::sin (angle));
	camera.y = 300;
	camera.z = (float) -(int) (radius * std::cos (angle));
	camera.look_at (0, 280, 0);
	const float dx = camera.x, dy = camera.y - 280, dz = camera.z;
	distance = (int) std::sqrt (dx * dx + dy * dy + dz * dz);
	const int next = reference ? 4 : distance < LOD_STEP ? 0 : distance <= FADE_NEAR ? 1 : distance < FADE_FAR ? 2 : 3;
	if (next != stage)
		std::printf ("Woodland: %s; distance %d\n", names[next], distance);
	stage = next;
}

void init ()
{
	camera.fov = 58;
	camera.near_plane = 40;
	camera.far_plane = 9000;
	for (int y = 0; y < kit::H; y++)
		sky[y] = kit::to565 (((111 + y * 70 / 320) << 16) | ((170 + y * 49 / 320) << 8) | (207 + y * 28 / 320));
	// the land, far to near: hills, the ground, a path and a fence (for scale, as the camera backs away)
	const kit::Material hill = paint (0x8DA993), wood = paint (0x8A7455);
	for (int i = 0; i < 12; i++)
	{
		const float x = (float) (-6500 + i * 1100);
		const float a[3] = {x, 0, 4300}, b[3] = {x + 630, (float) (450 + (i * 211) % 400), 4300}, c[3] = {x + 1300, 0, 4300};
		land.triangle (a, b, c, hill);
	}
	{
		const float a[3] = {-9000, 0, -6500}, b[3] = {9000, 0, -6500}, c[3] = {9000, 0, 6500}, d[3] = {-9000, 0, 6500};
		quad (land, a, b, c, d, paint (0x9EAD70));
	}
	{
		const float a[3] = {-470, 1, -5000}, b[3] = {-260, 1, -5000}, c[3] = {-260, 1, 3500}, d[3] = {-470, 1, 3500};
		quad (land, a, b, c, d, paint (0xC6B896));
	}
	for (int z = -650; z <= 2300; z += 350)
	{
		const float a[3] = {530, 0, (float) (z - 10)}, b[3] = {530, 0, (float) (z + 10)};
		const float c[3] = {530, 110, (float) (z + 10)}, d[3] = {530, 110, (float) (z - 10)};
		quad (land, a, b, c, d, wood);
	}
	for (int y : {40, 85})
	{
		const float a[3] = {530, (float) y, -650}, b[3] = {530, (float) y, 2300};
		const float c[3] = {530, (float) (y + 10), 2300}, d[3] = {530, (float) (y + 10), -650};
		quad (land, a, b, c, d, wood);
	}
	tree (high, woodland_trees::high);
	tree (low, woodland_trees::low);
	// the picture of a tree: Jet's createQuad (its texture's first row at the bottom)
	{
		const float a[3] = {-256, -320, 0}, b[3] = {256, -320, 0}, c[3] = {256, 320, 0}, d[3] = {-256, 320, 0};
		const int uv[4][2] = {{0, 0}, {1024, 0}, {1024, 1024}, {0, 1024}};
		card.quad (a, b, c, d, paint (0xFFFFFF), uv);
	}
	for (kit::Mesh* mesh : {&land, &high, &low, &card})	// (they stay as they are: the RPi has them)
		mesh->hand_over ();
	const kit::Image keyed = {128, 160, woodland::pine, true, 0};
	pine = kit::texture (keyed);

	using namespace woodland_labels;
	title = kit::texture (woodland_labels::title);
	const kit::Image* const modes[5] = {&woodland_labels::high, &woodland_labels::low, &fade, &billboard, &woodland_labels::reference};
	for (int i = 0; i < 5; i++)
		mode_label[i] = kit::texture (*modes[i]);
	desc_label[0] = kit::texture (autoDesc);
	desc_label[1] = kit::texture (referenceDesc);
	update (0);
}

void draw ()
{
	kit::background (sky);
	kit::begin (camera, light);
	kit::Draw ground;				// painted in its order, under everything
	ground.depth_test = false;
	ground.cull = kit::NONE;
	kit::draw (land, ground);

	kit::Draw picture;				// turned to the camera about the upright
	picture.texture = pine;
	picture.key = true;
	picture.cull = kit::NONE;
	for (int i = 0; i < 6; i++)
	{
		picture.model = kit::translation ((float) ((i - 3) * 670 + 290), 280, (float) (2100 + (i % 2) * 300))
				* kit::rotation (0, camera.yaw, 0);
		kit::draw (card, picture);
	}

	// the tree: a mesh by its distance, fading out as its picture fades in
	float mesh_alpha = 1, picture_alpha = 0;
	const kit::Mesh* mesh = &high;
	if (!reference)
	{
		const int level = distance / LOD_STEP;
		mesh = level == 0 ? &high : level == 1 ? &low : nullptr;
		if (distance >= FADE_FAR)
			mesh = nullptr;
		else if (distance > FADE_NEAR)
			mesh_alpha = (float) (255 - (distance - FADE_NEAR) * 255 / (FADE_FAR - FADE_NEAR)) / 255.0f;
		if (distance > FADE_NEAR)
			picture_alpha = distance < FADE_FAR ? (float) ((distance - FADE_NEAR) * 255 / (FADE_FAR - FADE_NEAR)) / 255.0f : 1;
	}
	if (mesh && mesh_alpha > 0)
	{
		kit::Draw how;
		how.model = kit::translation (0, 280, 0);
		how.alpha = mesh_alpha;
		how.blend = mesh_alpha < 1 ? kit::ALPHA : kit::OPAQUE;
		kit::draw (*const_cast<kit::Mesh*> (mesh), how);
	}
	if (picture_alpha > 0)
	{
		picture.model = kit::translation (0, 280, 0) * kit::rotation (0, camera.yaw, 0);
		picture.alpha = picture_alpha;
		picture.blend = picture_alpha < 1 ? kit::ALPHA : kit::OPAQUE;
		kit::draw (card, picture);
	}

	kit::rect (0, 0, 480, 65, 0x0842, 145);
	kit::rect (0, 289, 480, 31, 0x0842, 145);
	kit::sprite (title, 310, 27, 18, 13);
	kit::sprite (mode_label[stage], 340, 19, 18, 44);
	kit::sprite (desc_label[reference], 455, 19, 18, 299);
}

}  // namespace

int main ()
{
	return kit::run ({"Woodland", init, update, draw});
}
