// Runtime.cpp - the JetExamples runtime contract (Esp32Jet::start, Runtime.hpp)
// on piegpu. As picojet's examples/runtime/Runtime.cpp, the example
// scenes compile unmodified and the frame follows upstream's order - update,
// render, effects, afterRender, present - but Jet's pixel work is done by the
// V3D (jet/gpu/JetGpu.cpp): the Pico keeps no framebuffer.
//
// The scene is 320x240 (picojet's is 320x200): it fills the panel, and on a
// larger screen (HDMI) it's scaled up to fit, centred (JetGpu::place). Over
// it, as in the other demos, the half-size HUD in the screen's top right
// (frame rate and loads: hud.c, perf.c) and the example's title bottom left.
#include "Runtime.hpp"
#include "Display.hpp"
#include "JetGpu.hpp"
#include <cctype>
#include <cstdio>
#include <cstring>
#include <malloc.h>
#include "pico/stdlib.h"
#include "pgl.h"
extern "C" {
#include "pgpu.h"
#include "hud.h"
#include "pgpu_perf.h"
}

#ifndef PICOJET_EXAMPLE_NAME
#define PICOJET_EXAMPLE_NAME "Jet example"
#endif

/* Supplied by the example, as on the ESP32 (renamed jet_app_main: jet.cmake). */
extern "C" void app_main (void);

#ifdef main	/* renamed by the host's build (ESP-IDF: pgpu_app_main), called from C */
extern "C" int main (void);
#endif

int main (void)
{
	stdio_init_all ();
	std::printf ("\njet: %s\n", PICOJET_EXAMPLE_NAME);
	app_main ();
	for (;;)
		tight_loop_contents ();
}

namespace Esp32Jet {

const char* caption = nullptr;

namespace {

constexpr int W = Display::RENDER_WIDTH;
constexpr int H = Display::RENDER_HEIGHT;
constexpr int TOP = 0;				// the scene's first row on the panel

void hud (const perf_t& perf)
{
	char s[48];
	unsigned screen_w, screen_h;
	pglGetScreenSize (&screen_w, &screen_h);
	hud_begin ();
	hud_perf (screen_w - hud_perf_width (0.5f) - 2, 2, 0.5f, &perf);	// half size, top right
	std::snprintf (s, sizeof s, "%s", caption ? caption : PICOJET_EXAMPLE_NAME);
	for (char* p = s; *p; p++)
		*p = (char) std::toupper ((unsigned char) *p);	// the HUD font is upper case
	hud_text_scaled (4, screen_h - 12, s, HUD_RGBA (255, 255, 255, 200), 0.5f);
	hud_end ();
}

}  // namespace

void start (Init init, Update update, Update afterRender, RenderEffects renderEffects)
{
	pgpu_init ();
	while (!pgpu_wait_ready (1000))
	{
	}
	pgpu_set_reply_phase (1);
	int tries = 0;
	while (!pglInit () && ++tries < 5)	/* the first reply can be missed */
	{
	}
	glDisable (GL_DITHER);
	hud_init ();
	JetGpu::init (W, H, TOP);

	/* No framebuffer: the GPU draws (JetGpu hooks in Scene.cpp). Allocated
	 * once and never freed, as upstream does. */
	Renderer::Scene* scene = new Renderer::Scene (nullptr, nullptr, W, H);
	init (*scene);
	std::printf ("jet: %dx%d on the GPU, scene built, heap %u KB\n", W, H,
		     (unsigned) (mallinfo ().uordblks / 1024));

	perf_t perf;
	std::memset (&perf, 0, sizeof perf);
	hud (perf);
	absolute_time_t last = get_absolute_time ();
	unsigned windows = 0;
	/* one frame in flight: the Pico builds the next frame while the RPi
	   renders this one (FRAME pulses count the presented frames) */
	uint32_t sent = 0, presented_base = pgpu_frame_count ();
#if JET_PROFILE
	/* where the Pico's frame goes, microseconds summed over the report */
	uint32_t p_update = 0, p_render = 0, p_after = 0, p_end = 0, p_present = 0, p_wait = 0, p_frames = 0;
#define STAMP(sum) do { uint32_t now_us = time_us_32 (); (sum) += now_us - stamp; stamp = now_us; } while (0)
#else
#define STAMP(sum) do { } while (0)
#endif
	for (;;)
	{
		absolute_time_t now = get_absolute_time ();
		float elapsed = absolute_time_diff_us (last, now) / 1e6f;
		last = now;
		if (elapsed <= 0.0f)
			elapsed = 1.0f / 1000.0f;

#if JET_PROFILE
		uint32_t stamp = time_us_32 ();
#endif
		if (update)
			update (elapsed);
		STAMP (p_update);

		glClearColor (0.0f, 0.0f, 0.0f, 1.0f);
		glClear (GL_COLOR_BUFFER_BIT);
		JetGpu::beginFrame ();
		scene->render ();
		STAMP (p_render);
		if (renderEffects)
			scene->lastFrameRasterizedTriangles += (int) renderEffects (*scene);
		if (afterRender)
			afterRender (elapsed);
		STAMP (p_after);
		JetGpu::endFrame ();
		STAMP (p_end);

		hud_draw ();
		pglSwapBuffers ();
		STAMP (p_present);
		sent++;
		absolute_time_t wait_start = get_absolute_time ();
		while (pgpu_frame_count () - presented_base + 1 < sent)
		{
			if (absolute_time_diff_us (wait_start, get_absolute_time ()) > 100000)
			{
				presented_base = pgpu_frame_count () + 1 - sent;	/* a pulse was missed */
				break;
			}
			tight_loop_contents ();
		}
		STAMP (p_wait);
#if JET_PROFILE
		if (++p_frames == 300)
		{
			uint32_t tex, draw, tri;
			JetGpu::profile (&tex, &draw, &tri);
			std::printf ("profile (us/frame): update %lu, render %lu (drawTriangle %lu, "
				     "texture checks %lu, GL draws %lu), effects %lu, end %lu, hud+swap %lu, "
				     "wait %lu\n", p_update / 300, p_render / 300, tri / 300, tex / 300,
				     draw / 300, p_after / 300, p_end / 300, p_present / 300, p_wait / 300);
			p_update = p_render = p_after = p_end = p_present = p_wait = p_frames = 0;
		}
#endif
		if (perf_frame (absolute_time_diff_us (wait_start, get_absolute_time ()), &perf))
		{
			hud (perf);
			if (++windows % 5 == 0)
			{
				GLenum e = glGetError ();
				std::printf ("jet: %.1f fps, GPU %.0f%% CPU-G %.0f%% CPU-H %.0f%%, render %.2f ms, "
					     "%d tris, %d draws, heap %u KB, GL error 0x%x\n", perf.fps,
					     perf.gpu * 100, perf.cpu_g * 100, perf.cpu_h * 100, perf.render_ms,
					     scene->lastFrameRasterizedTriangles, JetGpu::lastDraws (),
					     (unsigned) (mallinfo ().uordblks / 1024), (unsigned) e);
			}
		}
	}
}

}  // namespace Esp32Jet
