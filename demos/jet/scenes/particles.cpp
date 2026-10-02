// particles - JetExamples' "Particles" (esp32-particles, MIT, CubeCoders) for
// piegpu's OpenGL: sparks (added to the picture) and a water spray (blended)
// from two plinths; a pool of 200 that a burst can't outgrow; and those too
// far from the camera left out. Jet's particle system (its ParticleSystem.hpp:
// the emitters, the colours over a particle's life, a streak along its
// velocity) is here; the streaks are a vertex buffer the V3D draws.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "kit.hpp"
#include "../assets/particles_labels.hpp"

namespace {

constexpr int POOL = 200;
constexpr float SCALE = 16.0f;			// the streaks' vertices: sixteenths of a unit

struct Particle
{
	float pos[3], vel[3], life, max_life;
	bool splash, active;
};

kit::Camera camera;
kit::Light light;				// none: every colour as it is
kit::Mesh room, sparks, drops;
Particle pool[POOL];
uint16_t gradient[kit::H];
GLuint title, mode_label[4], desc_label[4], live, cap, drawn, digit[10];
float seconds_in = 0, accumulator = 0, spark_clock = 0, splash_clock = 0;
int mode = -1, lamp_first[2];
unsigned rendered = 0;

kit::Material paint (unsigned hex)
{
	return kit::Material (kit::to565 (hex), 255, 255, 0, false);
}

float rand_f ()
{
	return (float) (std::rand () & 0x7FFF) / 32767.0f;
}

Particle* allocate ()
{
	for (auto& p : pool)
		if (!p.active)
			return &p;
	return nullptr;
}

unsigned active_count ()
{
	unsigned n = 0;
	for (const auto& p : pool)
		n += p.active;
	return n;
}

void emit_sparks (const float origin[3], const float normal[3], float speed, int count, const float base[3])
{
	const float base_speed = 120.0f + speed * 0.18f;
	for (int n = 0; n < count; n++)
	{
		Particle* p = allocate ();
		if (!p)
			break;
		const float rx = rand_f () - 0.5f, ry = std::fabs (rand_f ()) * 0.6f + 0.25f, rz = rand_f () - 0.5f;
		float dir[3] = {normal[0] * 0.7f + rx, normal[1] * 0.5f + ry, normal[2] * 0.7f + rz};
		float length = std::sqrt (dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
		if (length < 1e-4f)
			length = 1.0f;
		const float spd = base_speed * (0.6f + rand_f () * 0.9f);
		for (int i = 0; i < 3; i++)
		{
			p->vel[i] = dir[i] / length * spd + base[i];
			p->pos[i] = origin[i];
		}
		const float life = std::min (1.0f, speed / 600.0f);
		p->max_life = 0.10f + 0.20f * life + rand_f () * (0.10f + 0.25f * life);
		p->life = p->max_life;
		p->splash = false;
		p->active = true;
	}
}

void emit_splash (const float origin[3], const float travel[3], const float side[3], const float up[3], float speed,
		  int count, const float base[3])
{
	const float base_speed = 100.0f + speed * 0.12f;
	for (int n = 0; n < count; n++)
	{
		Particle* p = allocate ();
		if (!p)
			break;
		const float rx = rand_f () - 0.5f, rz = rand_f () - 0.5f;
		float dir[3];
		dir[0] = travel[0] * 0.90f + side[0] * 0.55f + up[0] * 0.60f + rx * 0.25f;
		dir[1] = travel[1] * 0.90f + side[1] * 0.55f + up[1] * 0.60f + std::fabs (rand_f ()) * 0.30f;
		dir[2] = travel[2] * 0.90f + side[2] * 0.55f + up[2] * 0.60f + rz * 0.25f;
		float length = std::sqrt (dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
		if (length < 1e-4f)
			length = 1.0f;
		const float spd = base_speed * (0.55f + rand_f () * 0.75f);
		for (int i = 0; i < 3; i++)
		{
			p->vel[i] = dir[i] / length * spd + base[i] * 0.25f;
			p->pos[i] = origin[i];
		}
		p->max_life = 0.12f + rand_f () * 0.3f;
		p->life = p->max_life;
		p->splash = true;
		p->active = true;
	}
}

void select (int next)
{
	static const char* const names[] = {"ADDITIVE SPARKS", "WATER SPRAY", "POOL LIMIT / 200", "DISTANCE CULL"};
	if (next == mode)
		return;
	mode = next;
	spark_clock = splash_clock = 0;
	for (auto& p : pool)
		p.active = false;
	// the plinths' lamps: lit when theirs is on
	room.paint (lamp_first[0], lamp_first[0] + 36, paint (mode == 1 ? 0x344354 : 0x66E9FF));
	room.paint (lamp_first[1], lamp_first[1] + 36, paint (mode == 0 ? 0x344354 : 0xEDB5FF));
	room.upload ();
	std::printf ("Particles: %s\n", names[mode]);
}

void step (float dt)
{
	seconds_in = std::fmod (seconds_in + dt, 28.0f);
	select ((int) (seconds_in / 7));
	const float gravity = -9.8f * 60.0f;
	for (auto& p : pool)
	{
		if (!p.active)
			continue;
		p.vel[1] += gravity * dt;
		for (int i = 0; i < 3; i++)
			p.pos[i] += p.vel[i] * dt;
		p.life -= dt;
		if (p.life <= 0.0f)
			p.active = false;
	}
	spark_clock += dt;
	splash_clock += dt;
	const bool spark = mode != 1, water = mode != 0;
	if (spark && spark_clock >= 0.55f)
	{
		spark_clock -= 0.55f;
		// (more asked for than the pool has: it's capped)
		static const float origin[3] = {-175, -140, 0}, normal[3] = {0.25f, 1, 0}, base[3] = {0, 320, 0};
		emit_sparks (origin, normal, 1100.0f, mode == 2 ? 260 : 52, base);
	}
	if (water && splash_clock >= 0.035f)
	{
		splash_clock -= 0.035f;
		const float a = seconds_in * 4;
		static const float origin[3] = {175, -140, 0}, up[3] = {0, 1, 0};
		const float side[3] = {std::cos (a), 0, std::sin (a)}, base[3] = {850 * std::cos (a), 1500, 500 * std::sin (a)};
		emit_splash (origin, up, side, up, 1300, 6, base);
	}
	if (!spark)
		spark_clock = 0;
	if (!water)
		splash_clock = 0;
}

void update (float seconds)
{
	// the emitters and the motion at 120 Hz, whatever a frame takes
	accumulator += std::min (std::max (seconds, 0.0f), 0.1f);
	constexpr float tick = 1.0f / 120.0f;
	while (accumulator >= tick)
	{
		step (tick);
		accumulator -= tick;
	}
	const float phase = std::fmod (seconds_in, 7.0f);
	const float pullback = mode == 3 ? 1100.0f * (0.5f - 0.5f * std::cos (phase * 6.2831853f / 7.0f)) : 0;
	camera.z = (float) -(int) (850 + pullback);
}

// the live particles as streaks: a triangle each, its tip along the velocity,
// its base across it on the screen (as Jet shapes them, in the camera's space)
void build ()
{
	sparks.clear ();
	drops.clear ();
	rendered = 0;
	for (const auto& p : pool)
	{
		if (!p.active)
			continue;
		const float dx = p.pos[0] - camera.x, dy = p.pos[1] - camera.y, dz = p.pos[2] - camera.z;
		if (dx * dx + dy * dy + dz * dz > 1500.0f * 1500.0f)
			continue;			// too far to be worth drawing
		const float age = 1.0f - p.life / p.max_life;
		int alpha = 255;
		if (age > 0.65f)
			alpha = std::min (255, std::max (0, (int) (255.0f * (1.0f - (age - 0.65f) / 0.35f))));
		if (alpha < 4)
			continue;
		uint16_t color;
		if (p.splash)
		{
			if (age < 0.35f)
			{
				color = 0xEFFF;
			}
			else
			{
				const float t = std::min (1.0f, (age - 0.35f) / 0.65f);
				color = (uint16_t) ((unsigned) (29.0f * (1.0f - t) + 11.0f * t + 0.5f) << 11
						    | (unsigned) (63.0f * (1.0f - t) + 46.0f * t + 0.5f) << 5 | 31u);
			}
		}
		else if (age < 0.25f)
		{
			color = 0xFFFF;
		}
		else if (age < 0.45f)
		{
			const float t = (age - 0.25f) * 5.0f;
			color = (uint16_t) ((unsigned) (31.0f * (1.0f - t) + 0.5f) << 11
					    | (unsigned) (63.0f * (1.0f - t) + 42.0f * t + 0.5f) << 5 | 31u);
		}
		else
		{
			color = 0x055F;
		}
		float bx, by, bz, tx, ty, tz;
		if (!camera.project (p.pos[0], p.pos[1], p.pos[2], &bx, &by, &bz) || bz <= camera.near_plane
		    || bz >= camera.far_plane)
			continue;
		const float speed = std::sqrt (p.vel[0] * p.vel[0] + p.vel[1] * p.vel[1] + p.vel[2] * p.vel[2]);
		const float streak = speed > 1.0f ? speed * (p.splash ? 0.03f : 0.02f) : 2.0f;
		float along[3] = {0, 1, 0};
		if (speed > 1e-3f)
			for (int i = 0; i < 3; i++)
				along[i] = p.vel[i] / speed;
		if (!camera.project (p.pos[0] + along[0] * streak, p.pos[1] + along[1] * streak, p.pos[2] + along[2] * streak,
				     &tx, &ty, &tz))
			continue;
		const float half = p.splash ? 3.0f : 2.0f, ddx = tx - bx, ddy = ty - by;
		const float length = std::sqrt (ddx * ddx + ddy * ddy);
		const float px = length < 0.5f ? half : -ddy * half / length, py = length < 0.5f ? 0.0f : ddx * half / length;
		if (std::max (tx, bx + std::fabs (px)) < 0 || std::min (tx, bx - std::fabs (px)) >= kit::W
		    || std::max (ty, by + std::fabs (py)) < 0 || std::min (ty, by - std::fabs (py)) >= kit::H)
			continue;			// (off the screen)
		kit::Mesh& mesh = p.splash ? drops : sparks;
		const kit::Material material (color, (uint8_t) alpha, 255, 0, false);
		const float corner[3][3] = {{tx, ty, tz}, {bx + px, by + py, bz}, {bx - px, by - py, bz}};
		for (const auto& c : corner)
		{
			float v[3];
			camera.unproject (c[0], c[1], c[2], v);
			mesh.add (v[0] * SCALE, v[1] * SCALE, v[2] * SCALE, 0, 0, -1, material);
		}
		rendered++;
	}
}

void init ()
{
	camera.fov = 58;
	camera.near_plane = 40;
	camera.far_plane = 4000;
	for (int y = 0; y < kit::H; y++)
		gradient[y] = kit::to565 (((12 + y * 13 / 320) << 16) | ((21 + y * 22 / 320) << 8) | (39 + y * 28 / 320));
	const kit::Material wall = paint (0x152A43), line = paint (0x28485C), metal = paint (0x426679), inset = paint (0x223B4F);
	room.box (0, 0, 290, 1080, 490, 20, wall);
	for (int x = -500; x <= 500; x += 100)
		room.box ((float) x, 0, 270, 3, 440, 4, line);
	for (int y = -200; y <= 200; y += 100)
		room.box (0, (float) y, 268, 1020, 3, 4, line);
	int lamp = 0;
	for (int x : {-175, 175})
	{
		room.box ((float) x, -198, 0, 170, 44, 160, inset);
		room.box ((float) x, -170, 0, 130, 18, 120, metal);
		lamp_first[lamp++] = (int) room.vertices.size ();
		room.box ((float) x, -155, 0, 86, 12, 78, paint (0x66E9FF));
	}
	// the floor: Jet's grid, moved to its place. (Its faces look down: from above,
	// where the camera is, it isn't seen, in the original either)
	const int floor_first = (int) room.vertices.size ();
	room.grid (980, 600, 4, 6, paint (0x253E53), paint (0x1C3044));
	for (int i = floor_first; i < (int) room.vertices.size (); i++)
	{
		room.vertices[i].y = (int16_t) (room.vertices[i].y - 220);
		room.vertices[i].z = (int16_t) (room.vertices[i].z + 80);
	}

	using namespace particles_labels;
	title = kit::texture (particles_labels::title);
	const kit::Image* const modes[4] = {&spark, &water, &particles_labels::pool, &distance};
	const kit::Image* const descriptions[4] = {&sparkDesc, &waterDesc, &poolDesc, &distanceDesc};
	const kit::Image* const digits[10] = {&digit0, &digit1, &digit2, &digit3, &digit4, &digit5, &digit6, &digit7, &digit8, &digit9};
	for (int i = 0; i < 4; i++)
	{
		mode_label[i] = kit::texture (*modes[i]);
		desc_label[i] = kit::texture (*descriptions[i]);
	}
	for (int i = 0; i < 10; i++)
		digit[i] = kit::texture (*digits[i]);
	live = kit::texture (particles_labels::live);
	cap = kit::texture (particles_labels::cap);
	drawn = kit::texture (particles_labels::drawn);
	select (0);
	update (0);
}

void draw ()
{
	kit::background (gradient);
	kit::begin (camera, light);
	kit::draw (room, kit::Draw ());

	build ();
	kit::Draw streaks;
	streaks.view_space = true;
	streaks.model = kit::scaling (1 / SCALE, 1 / SCALE, 1 / SCALE);
	streaks.cull = kit::NONE;
	streaks.depth_write = false;
	streaks.blend = kit::ADD;
	sparks.upload (GL_DYNAMIC_DRAW);
	kit::draw (sparks, streaks);
	streaks.blend = kit::ALPHA;
	drops.upload (GL_DYNAMIC_DRAW);
	kit::draw (drops, streaks);

	kit::sprite (title, 310, 25, 18, 14);
	kit::sprite (mode_label[mode], 330, 22, 18, 45);
	kit::sprite (desc_label[mode], 450, 19, 18, 295);
	kit::sprite (live, 40, 18, 18, 271);
	kit::sprite (cap, 65, 18, 103, 271);
	kit::sprite (drawn, 60, 18, 220, 271);
	const unsigned values[2] = {active_count (), rendered};
	for (int i = 0; i < 6; i++)
	{
		const unsigned div = i % 3 == 0 ? 100 : i % 3 == 1 ? 10 : 1;
		kit::sprite (digit[(values[i / 3] / div) % 10], 12, 18, (i < 3 ? 62 : 280) + (i % 3) * 12, 271);
	}
}

}  // namespace

int main ()
{
	return kit::run ({"Particles", init, update, draw});
}
