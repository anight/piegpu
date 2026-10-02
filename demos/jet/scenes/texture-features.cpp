// texture-features - JetExamples' "Texture Lab" (esp32-texture-features, MIT,
// CubeCoders) for piegpu's OpenGL: a panel in a console, what a texture does
// outside its edges (it repeats, its edge goes on, it's a hole), a colour
// key, a palette cycled, and the texture fading into the material's colour
// with the distance; seven seconds each.
#include <cmath>
#include <cstdio>
#include "kit.hpp"
#include "../assets/texlab.hpp"
#include "../assets/texlab_labels.hpp"

namespace {

constexpr float PI = 3.14159265359f, STAGE_SECONDS = 7.0f, CYCLE_SECONDS = 49.0f;
constexpr int LOD_NEAR = 1100, LOD_FAR = 1650;

kit::Camera camera;
kit::Light light;				// none: every colour as it is
kit::Mesh console, panel, plant;
uint16_t gradient[kit::H];
GLuint tile_wrap, tile_clamp, tile_keyed, checker, foliage_opaque, foliage_keyed, lava;
GLuint title, mode_label[7], desc_label[7], key_off, key_on, state_label[3];
float seconds_in = 0;
int stage = -1, lod_state = 0, lava_offset = -1, panel_z = 1000;
int frame_first, back_first, back_count, rails_first;	// the console's parts, in its mesh

kit::Material paint (unsigned hex)
{
	return kit::Material (kit::to565 (hex), 255, 255, 0, false);
}

void update (float seconds)
{
	static const char* const names[] = {"WRAP", "CLAMP", "ZERO + BLACK KEY", "COLOUR KEY", "PALETTE CYCLING", "LOD OFF", "LOD ON"};
	seconds_in = std::fmod (seconds_in + seconds, CYCLE_SECONDS);
	const int next = (int) (seconds_in / STAGE_SECONDS);
	const float phase = seconds_in - next * STAGE_SECONDS;
	if (next != stage)
		std::printf ("Texture Lab: %s\n", names[next]);
	stage = next;
	panel_z = 1000;
	lod_state = 0;
	panel.clear ();
	const kit::Material material = paint (0x27677D);
	if (stage <= 2)
	{
		// the same travel for each way of addressing: beyond all four edges
		panel.plane (720, 360, 0, 0, 0, material, (int) (-600 + 350 * std::sin (phase * 0.9f)),
		       (int) (-350 + 220 * std::cos (phase * 0.8f)), 2304, 1536);
	}
	else if (stage == 4)
	{
		panel.plane (720, 360, 0, 0, 0, material, 0, 0, 2048, 1024);
		const int offset = (int) (phase * 18.0f) % 64;
		if (offset != lava_offset)
			lava = kit::texture_indexed (64, 64, texlab::lavaIndices, texlab::lavaPalette, 64, offset, true, lava);
		lava_offset = offset;
	}
	else
	{
		panel.plane (720, 360, 0, 0, 0, material);
		if (stage >= 5)
		{
			panel_z = (int) (850 + 1050 * (0.5f - 0.5f * std::cos (phase * 2 * PI / STAGE_SECONDS)));
			lod_state = panel_z >= LOD_FAR ? 2 : panel_z > LOD_NEAR ? 1 : 0;
		}
	}
}

void init ()
{
	camera.fov = 58;
	camera.near_plane = 40;
	camera.far_plane = 3500;
	for (int y = 0; y < kit::H; y++)
	{
		const unsigned red = 12 + y * 14 / kit::H, green = 29 + y * 31 / kit::H, blue = 48 + y * 32 / kit::H;
		gradient[y] = (uint16_t) ((red >> 3) << 11 | (green >> 2) << 5 | (blue >> 3));
	}
	// the console about the panels, back to front: the picture is painted in this order
	const kit::Material dark = paint (0x172B40), edge = paint (0x426879);
	console.box (0, -245, 1170, 900, 85, 290, dark);
	frame_first = (int) console.vertices.size ();
	console.plane (810, 440, 0, 0, 1100, edge);
	console.plane (780, 410, 0, 0, 1080, dark);
	back_first = (int) console.vertices.size ();
	console.plane (750, 380, 0, 0, 1060, paint (0x1C384C), 0, 0, 4096, 2048);
	rails_first = (int) console.vertices.size ();
	back_count = rails_first - back_first;
	console.plane (16, 380, -400, 0, 1040, paint (0x33DEC8));
	console.plane (16, 380, 400, 0, 1040, paint (0xEB639E));
	plant.plane (270, 350, 0, 0, 0, paint (0xFFFFFF));
	console.hand_over ();				// (they stay as they are: the RPi has them)
	plant.hand_over ();

	tile_wrap = kit::texture565 (64, 64, texlab::tile, false, true);
	tile_clamp = kit::texture565 (64, 64, texlab::tile, false, false);
	const kit::Image black_key = {64, 64, texlab::tile, true, 0};
	tile_keyed = kit::texture (black_key);
	checker = kit::texture565 (32, 32, texlab::checker, false, true);
	foliage_opaque = kit::texture565 (96, 128, texlab::foliage);
	const kit::Image keyed = {96, 128, texlab::foliage, true, 0xF81F};
	foliage_keyed = kit::texture (keyed);

	using namespace texlab_labels;
	title = kit::texture (texlab_labels::title);
	const kit::Image* const modes[7] = {&wrap, &clamp, &zero, &key, &palette, &lodoff, &lodon};
	const kit::Image* const descriptions[7] = {&wrapDesc, &clampDesc, &zeroDesc, &keyDesc, &paletteDesc, &offDesc, &onDesc};
	const kit::Image* const states[3] = {&full, &fade, &flat};
	for (int i = 0; i < 7; i++)
	{
		mode_label[i] = kit::texture (*modes[i]);
		desc_label[i] = kit::texture (*descriptions[i]);
	}
	for (int i = 0; i < 3; i++)
		state_label[i] = kit::texture (*states[i]);
	key_off = kit::texture (keyOff);
	key_on = kit::texture (keyOn);
	update (0);
}

void draw ()
{
	kit::background (gradient);
	kit::begin (camera, light);
	kit::Draw how;
	how.depth_test = false;
	how.count = frame_first;
	kit::draw (console, how);			// the plinth
	how.cull = kit::NONE;
	how.first = frame_first;
	how.count = back_first - frame_first;
	kit::draw (console, how);			// the frame
	how.first = back_first;
	how.count = back_count;
	how.texture = stage >= 5 ? 0 : checker;	// (the LOD stages: a plain back, the fade is easier seen)
	kit::draw (console, how);
	how.texture = 0;
	how.first = rails_first;
	how.count = -1;
	kit::draw (console, how);

	kit::Draw front;
	front.depth_test = false;
	front.cull = kit::NONE;
	if (stage == 3)
	{
		// the same picture twice: its key colour as it is, and as holes
		front.model = kit::translation (-182, 0, 960);
		front.texture = foliage_opaque;
		kit::draw (plant, front);
		front.model = kit::translation (182, 0, 960);
		front.texture = foliage_keyed;
		front.key = true;
		kit::draw (plant, front);
	}
	else
	{
		front.model = kit::translation (0, 0, (float) panel_z);
		front.texture = stage == 0 ? tile_wrap : stage == 2 ? tile_keyed : stage == 4 ? lava : tile_clamp;
		front.zero = front.key = stage == 2;
		if (stage == 6)
		{
			front.lod_near = LOD_NEAR;
			front.lod_far = LOD_FAR;
		}
		kit::draw (panel, front);
	}

	using namespace texlab_labels;
	kit::sprite (title, texlab_labels::title.width, texlab_labels::title.height, 18, 14);
	kit::sprite (mode_label[stage], stage < 2 ? 330 : 334, 22, 18, 45);
	kit::sprite (desc_label[stage], 450, 19, 18, 294);
	if (stage == 3)
	{
		kit::sprite (key_off, keyOff.width, keyOff.height, 92, 265);
		kit::sprite (key_on, keyOn.width, keyOn.height, 277, 265);
	}
	if (stage == 6)
		kit::sprite (state_label[lod_state], lod_state == 2 ? 260 : 230, 18, 112, 267);
}

}  // namespace

int main ()
{
	return kit::run ({"Texture Lab", init, update, draw});
}
