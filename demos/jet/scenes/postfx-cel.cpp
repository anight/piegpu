// postfx-cel - JetExamples' "Cel / Teapot" (esp32-postfx-cel, MIT,
// CubeCoders) for piegpu's OpenGL: the teapot lit smoothly, then with its
// light cut into four bands (Jet drops the brightness' low six bits; the
// glossy highlight stays as it is), seven seconds each.
#include <cmath>
#include <cstdio>
#include "teapot.hpp"
#include "../assets/cel_labels.hpp"

namespace {

using namespace teapot_scene;
float seconds_in = 0, pitch, yaw, roll;
int mode = -1;
GLuint title, state[2], note, detail;

void update (float seconds)
{
	seconds_in = std::fmod (seconds_in + seconds, 14.0f);
	const int next = (int) (seconds_in / 7.0f) % 2;
	if (next != mode)
		std::printf ("Cel shading: %s; four diffuse bands; additive gloss unchanged\n", next ? "ON" : "OFF");
	mode = next;
	const float phase = std::fmod (seconds_in, 7.0f) / 7.0f;
	pitch = -14.0f + 20.0f * std::sin (phase * 6.283185307f);
	roll = 10.0f * std::sin (phase * 12.566370614f);
	yaw = (float) ((int) (25.0f + 360.0f * phase) % 360);
}

void init ()
{
	setup ();
	smooth.hand_over ();
	title = kit::texture (cel_labels::title);
	state[0] = kit::texture (cel_labels::off);
	state[1] = kit::texture (cel_labels::on);
	note = kit::texture (cel_labels::note);
	detail = kit::texture (cel_labels::detail);
	update (0);
}

void draw ()
{
	kit::background (gradient);
	kit::begin (camera, light);
	kit::Draw how;
	how.model = kit::rotation ((float) (int) pitch, (float) (int) yaw, (float) (int) roll);
	how.shading = kit::PHONG;
	how.cel = mode ? 64.0f : 1.0f;
	kit::draw (smooth, how);
	kit::sprite (title, cel_labels::title.width, cel_labels::title.height, 18, 14);
	kit::sprite (state[mode], cel_labels::off.width, cel_labels::off.height, 18, 45);
	kit::sprite (detail, cel_labels::detail.width, cel_labels::detail.height, 18, 271);
	kit::sprite (note, cel_labels::note.width, cel_labels::note.height, 18, 295);
}

}  // namespace

int main ()
{
	return kit::run ({"Cel / Teapot", init, update, draw});
}
