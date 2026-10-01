/*
 * mouse - a Bluetooth mouse for the Pico 2 W, Classic or LE (the HID host in
 * ../bt, looking for a mouse), shown on the RPi's screen:
 *
 * - it looks for a mouse, LE and Classic in turn: put the mouse in its pairing
 *   mode. The first one found is connected to;
 * - connected, a pointer follows the mouse; the screen shows its buttons, the
 *   wheel and where the pointer is, and each click leaves a mark. The mouse is
 *   remembered (BTstack's store in the flash, with its keys) and reached for
 *   at the next start.
 *
 * On the console: s the state, r forget the mouse and look again, n look
 * again, d the Bluetooth log on or off. The stick's button held at the start
 * forgets the mouse too.
 */
#include <stdio.h>
#include <string.h>
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "btstack.h"
#include "gles/pgl.h"
#include "pgpu.h"
#include "hud.h"
#include "screen.h"
#include "bt_app.h"
#include "debug.h"
#include "mouse_decode.h"
#include "psdl_pico_log.h"

#define FORGET_PIN	22			/* the stick's button (active low) */
#define MARKS		24			/* clicks kept on the screen */

#define WHITE		HUD_RGBA (255, 255, 255, 255)
#define GREY		HUD_RGBA (170, 175, 185, 255)
#define DARK		HUD_RGBA (60, 64, 72, 255)
#define YELLOW		HUD_RGBA (255, 215, 40, 255)
#define GREEN		HUD_RGBA (90, 220, 110, 255)
#define BLUE		HUD_RGBA (110, 170, 255, 255)
#define RED		HUD_RGBA (255, 110, 100, 255)

/* what the mouse did since the main loop last looked (the Bluetooth stack's context adds to it) */
static volatile int moved_x, moved_y, wheeled, panned;
static volatile unsigned buttons, pressed;	/* held now; went down since */
static volatile unsigned reports;

static void on_mouse (const mouse_event_t *event)
{
	moved_x += event->dx;
	moved_y += event->dy;
	wheeled += event->wheel;
	panned += event->pan;
	buttons = event->buttons;
	pressed |= event->changed & event->buttons;
	reports++;
}

/* the Bluetooth stack runs in the radio driver's context: ours while this is held */
static void bt_lock (void)	{ async_context_acquire_lock_blocking (cyw43_arch_async_context ()); }
static void bt_unlock (void)	{ async_context_release_lock (cyw43_arch_async_context ()); }

/* a line in the middle of the screen, in the HUD's capitals */
static float line (const GLint vp[4], float y, float scale, uint32_t color, const char *text)
{
	char upper[48];
	unsigned n = 0;
	for (; text[n] && n < sizeof upper - 1; n++)
	{
		upper[n] = text[n] >= 'a' && text[n] <= 'z' ? (char) (text[n] - 32) : text[n];
	}
	upper[n] = '\0';
	hud_text_scaled ((float) (int) ((vp[2] - n * HUD_CHAR_W * scale) / 2), y, upper, color, scale);
	return y + (HUD_CHAR_H + 3) * scale;
}

/* the pointer: an arrow of rows, its tip at (x, y) */
static void pointer (float x, float y, float s)
{
	static const uint8_t widths[] = {1, 2, 3, 4, 5, 6, 7, 8, 5, 2, 2};	/* a row's pixels, from the tip down */
	for (unsigned i = 0; i < sizeof widths; i++)
	{
		float from = i >= 9 ? 3 * s : 0;				/* (the tail) */
		hud_rect (x + from - s, y + i * s, (widths[i] + 2) * s, s, HUD_RGBA (0, 0, 0, 255));
	}
	for (unsigned i = 0; i < sizeof widths; i++)
	{
		float from = i >= 9 ? 3 * s : 0;
		hud_rect (x + from, y + i * s, widths[i] * s, s, WHITE);
	}
}

int main (void)
{
	stdio_init_all ();
	gpio_init (FORGET_PIN);
	gpio_set_dir (FORGET_PIN, GPIO_IN);
	gpio_pull_up (FORGET_PIN);
	pgpu_init ();
	printf ("\nmouse: waiting for the RPi (READY)...\n");
	while (!pgpu_wait_ready (1000))
	{
	}
	pgpu_set_reply_phase (1);
	int tries = 0;
	while (!pglInit () && ++tries < 5)		/* the first reply can be missed */
	{
	}
	if (!hud_init ())
	{
		printf ("mouse: the HUD program didn't link\n");
	}
	glDisable (GL_DEPTH_TEST);
	glDisable (GL_CULL_FACE);

	/* the radio and the Bluetooth stack: they run by themselves from here (the
	   driver's interrupts), what they say is printed from the loop below */
	bool forget = !gpio_get (FORGET_PIN), radio = cyw43_arch_init () == 0;
	psdl_log_init ();
	if (radio)
	{
		bt_app_look_for (BT_DEVICE_MOUSE);
		bt_app_setup ();
		mouse_decode_init (on_mouse);
		hci_power_control (HCI_POWER_ON);
	}
	printf ("mouse: %s; console: s the state, r forget the mouse and look again, n look again, d the log\n",
		radio ? "Bluetooth is starting" : "no radio");

	GLint vp[4] = {0};
	float px = -1, py = -1;				/* the pointer, pixels (not placed yet) */
	int wheel = 0, pan = 0;
	static struct { float x, y; unsigned button; } marks[MARKS];
	unsigned n_marks = 0, seen = 0, said = 0;
	while (true)
	{
		screen_update ("mouse", vp);
		psdl_log_drain ();
		if (px < 0 && vp[2])
		{
			px = vp[2] / 2;
			py = vp[3] / 2;
		}

		bt_app_status_t st;
		memset (&st, 0, sizeof st);
		int dx = 0, dy = 0;
		unsigned held = 0, clicked = 0;
		if (radio)
		{
			int c = getchar_timeout_us (0);
			bt_lock ();
			bt_app_get_status (&st);
			if (forget && st.phase != BT_APP_STARTING)	/* (the stack is up: its store can be reached) */
			{
				forget = false;
				c = 'r';
			}
			switch (c)
			{
			case 's': bt_app_print_status (); break;
			case 'r': bt_app_forget_pairing (); bt_app_search_again (); break;
			case 'n': bt_app_search_again (); break;
			case 'd':
				pico_bt_keyboard_verbose = !pico_bt_keyboard_verbose;
				psdl_log ("[app] the log is %s\n", pico_bt_keyboard_verbose ? "on" : "off");
				break;
			}
			dx = moved_x;
			dy = moved_y;
			wheel += wheeled;
			pan += panned;
			moved_x = moved_y = wheeled = panned = 0;
			held = buttons;
			clicked = pressed;
			pressed = 0;
			seen = reports;
			bt_unlock ();
		}
		float hs = vp[3] >= 480 ? (float) (vp[3] / 240) : 1.0f;
		px += dx * hs;
		py += dy * hs;
		px = px < 0 ? 0 : px > vp[2] - 1 ? vp[2] - 1 : px;
		py = py < 0 ? 0 : py > vp[3] - 1 ? vp[3] - 1 : py;
		for (unsigned b = 0; b < 3; b++)
		{
			if (clicked & (1u << b))
			{
				marks[n_marks % MARKS].x = px;
				marks[n_marks % MARKS].y = py;
				marks[n_marks++ % MARKS].button = b;
				printf ("mouse: button %u pressed at %d,%d\n", b + 1, (int) px, (int) py);
			}
		}
		if (seen && !said)
		{
			said = 1;
			printf ("mouse: its reports come\n");
		}

		glViewport (vp[0], vp[1], vp[2], vp[3]);
		glClearColor (0.07f, 0.08f, 0.10f, 1.0f);
		glClear (GL_COLOR_BUFFER_BIT);
		float y = vp[3] * 0.06f;
		const char *who = st.name[0] ? st.name : st.address;
		char s[48];
		hud_begin ();
		y = line (vp, y, hs, BLUE, "BLUETOOTH MOUSE");
		y += 10 * hs;
		if (!radio)
		{
			line (vp, y, hs, YELLOW, "NO RADIO");
		}
		else switch (st.phase)
		{
		case BT_APP_STARTING:
			line (vp, y, hs, GREY, "STARTING");
			break;
		case BT_APP_SEARCHING:
			y = line (vp, y, hs, WHITE, "LOOKING FOR A MOUSE");
			y = line (vp, y, hs, GREY, st.kind == BT_LINK_LE ? "LE" : "CLASSIC");
			y += 10 * hs;
			y = line (vp, y, hs * 0.5f, GREY, "PUT THE MOUSE IN ITS PAIRING MODE");
			if (st.unpairable[0])			/* one is there, calling the host it's paired with */
			{
				y += 10 * hs;
				y = line (vp, y, hs, YELLOW, st.unpairable);
				line (vp, y, hs * 0.5f, YELLOW, "IS THERE, BUT NOT IN ITS PAIRING MODE");
			}
			break;
		case BT_APP_CONNECTING:
		case BT_APP_WAITING:
			y = line (vp, y, hs, WHITE, st.phase == BT_APP_CONNECTING ? "CONNECTING TO" : "WAITING FOR");
			line (vp, y, hs, YELLOW, who);
			break;
		case BT_APP_CONNECTED:
		{
			y = line (vp, y, hs, YELLOW, who);
			snprintf (s, sizeof s, "CONNECTED OVER %s", bt_link_kind_name (st.kind));
			y = line (vp, y, hs, GREEN, s);
			/* the clicks' marks, its three buttons, the wheels, where the pointer is */
			static const uint32_t colors[3] = {RED, GREEN, BLUE};	/* left, right, middle */
			for (unsigned i = 0; i < (n_marks < MARKS ? n_marks : MARKS); i++)
			{
				hud_rect (marks[i].x - 3 * hs, marks[i].y - 3 * hs, 6 * hs, 6 * hs, colors[marks[i].button]);
			}
			float bw = 44 * hs, bh = 26 * hs, bx = (vp[2] - 3 * bw - 2 * 6 * hs) / 2, by = vp[3] - bh - 34 * hs;
			static const unsigned order[3] = {0, 2, 1};		/* as they lie: left, middle, right */
			static const char *const names[3] = {"L", "M", "R"};
			for (unsigned i = 0; i < 3; i++)
			{
				bool down = held & (1u << order[i]);
				float x = bx + i * (bw + 6 * hs);
				hud_rect (x, by, bw, bh, down ? colors[order[i]] : DARK);
				hud_text_scaled ((float) (int) (x + (bw - HUD_CHAR_W * hs) / 2), (float) (int) (by + (bh - 14 * hs) / 2),
						 names[i], WHITE, hs);
			}
			snprintf (s, sizeof s, "X %d  Y %d  WHEEL %d  PAN %d", (int) px, (int) py, wheel, pan);
			line (vp, vp[3] - 26 * hs, hs * 0.5f, GREY, s);
			line (vp, vp[3] - 14 * hs, hs * 0.5f, GREY, seen ? "" : "MOVE THE MOUSE");
			pointer (px, py, hs);
			break;
		}
		}
		hud_end ();
		hud_draw ();
		pglSwapBuffers ();
		pgpu_wait_frame (100);			/* pace on the screen */
	}
}
