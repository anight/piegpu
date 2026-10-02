// template-cube - JetExamples' "Rotating cube" (esp32-template-cube, MIT,
// CubeCoders) for piegpu's OpenGL: a cube of six colours turning, no light.
#include <cmath>
#include "kit.hpp"

namespace {

kit::Camera camera;
kit::Mesh cube;
float pitch = 20.0f, yaw = 30.0f, roll = 0.0f;

void init ()
{
	camera.z = -550;
	camera.near_plane = 16;
	camera.far_plane = 2000;
	// Jet's debug cube: red front (+z), green back, blue left, yellow right, magenta top, cyan bottom
	static const uint16_t colors[6] = {0xF800, 0x07E0, 0x001F, 0xFFE0, 0xF81F, 0x07FF};
	kit::Mesh face;
	face.box (0, 0, 0, 200, 200, 200, kit::Material (0xFFFF, 255, 255, 0, false));
	cube = face;
	for (int f = 0; f < 6; f++)
		cube.paint (f * 6, f * 6 + 6, kit::Material (colors[f], 255, 255, 0, false));
}

void update (float seconds)
{
	pitch = std::fmod (pitch + 23.0f * seconds, 360.0f);
	yaw = std::fmod (yaw + 37.0f * seconds, 360.0f);
	roll = std::fmod (roll + 11.0f * seconds, 360.0f);
}

void draw ()
{
	kit::background (0x0841);
	kit::begin (camera, kit::Light ());
	kit::Draw how;
	how.model = kit::rotation ((float) (int) pitch, (float) (int) yaw, (float) (int) roll);
	kit::draw (cube, how);
}

}  // namespace

int main ()
{
	return kit::run ({"Rotating cube", init, update, draw});
}
