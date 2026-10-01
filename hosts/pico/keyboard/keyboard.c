/*
 * keyboard - a Bluetooth keyboard for the Pico 2 W, Classic or LE (picosdl's
 * host for them, bt/), shown on the RPi's screen:
 *
 * - it looks for a keyboard, LE and Classic in turn: put the keyboard in its
 *   pairing mode. The first one found is connected to;
 * - pairing may want digits typed on the keyboard and Enter: the screen shows
 *   them;
 * - connected, the screen shows what's typed and each key as it goes down and
 *   up. The keyboard is remembered (BTstack's store in the flash, with its
 *   keys) and reached for at the next start.
 *
 * On the console: s the state, r forget the keyboard and look again, n look
 * again, d the Bluetooth log on or off. The stick's button held at the start
 * forgets the keyboard too.
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
#include "kbd_decode.h"
#include "psdl_pico_log.h"

#define FORGET_PIN	22			/* the stick's button (active low) */
#define TYPED_COLS	24
#define TYPED_ROWS	3

#define WHITE		HUD_RGBA (255, 255, 255, 255)
#define GREY		HUD_RGBA (170, 175, 185, 255)
#define YELLOW		HUD_RGBA (255, 215, 40, 255)
#define GREEN		HUD_RGBA (90, 220, 110, 255)
#define BLUE		HUD_RGBA (110, 170, 255, 255)

/* the keys as they come (the Bluetooth stack's context), for the main loop */
#define EVENTS		32
static kbd_event_t events[EVENTS];
static volatile unsigned events_in, events_out;

static void on_key (const kbd_event_t *event)
{
	unsigned next = (events_in + 1) % EVENTS;
	if (next != events_out)
	{
		events[events_in] = *event;
		events_in = next;
	}
}

/* the Bluetooth stack runs in the radio driver's context: ours while this is held */
static void bt_lock (void)	{ async_context_acquire_lock_blocking (cyw43_arch_async_context ()); }
static void bt_unlock (void)	{ async_context_release_lock (cyw43_arch_async_context ()); }

static char typed[TYPED_ROWS][TYPED_COLS + 1];
static unsigned typed_col;
static char last_key[40];

static void type (char c)
{
	if (c == '\n' || typed_col == TYPED_COLS)
	{
		memmove (typed[0], typed[1], sizeof typed - sizeof typed[0]);
		memset (typed[TYPED_ROWS - 1], 0, sizeof typed[0]);
		typed_col = 0;
	}
	if (c == '\b')
	{
		if (typed_col)
		{
			typed[TYPED_ROWS - 1][--typed_col] = '\0';
		}
	}
	else if (c != '\n')
	{
		typed[TYPED_ROWS - 1][typed_col++] = c;
	}
}

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

int main (void)
{
	stdio_init_all ();
	gpio_init (FORGET_PIN);
	gpio_set_dir (FORGET_PIN, GPIO_IN);
	gpio_pull_up (FORGET_PIN);
	pgpu_init ();
	printf ("\nkeyboard: waiting for the RPi (READY)...\n");
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
		printf ("keyboard: the HUD program didn't link\n");
	}
	glDisable (GL_DEPTH_TEST);
	glDisable (GL_CULL_FACE);

	/* the radio and the Bluetooth stack: they run by themselves from here (the
	   driver's interrupts), what they say is printed from the loop below */
	bool forget = !gpio_get (FORGET_PIN), radio = cyw43_arch_init () == 0;
	psdl_log_init ();
	if (radio)
	{
		bt_app_setup ();
		kbd_decode_init (on_key, bt_app_set_keyboard_leds);
		hci_power_control (HCI_POWER_ON);
	}
	printf ("keyboard: %s; console: s the state, r forget the keyboard and look again, n look again, d the log\n",
		radio ? "Bluetooth is starting" : "no radio");

	GLint vp[4] = {0};
	while (true)
	{
		screen_update ("keyboard", vp);
		psdl_log_drain ();

		bt_app_status_t st;
		memset (&st, 0, sizeof st);
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
			bt_unlock ();
		}

		while (events_out != events_in)
		{
			const kbd_event_t *e = &events[events_out];
			kbd_event_format (e, last_key, sizeof last_key);
			if (e->pressed)
			{
				if (e->ch)
				{
					type (e->ch);
				}
				else if (e->usage == 0x28 || e->usage == 0x58)	/* Enter */
				{
					type ('\n');
				}
				else if (e->usage == 0x2A)			/* Backspace */
				{
					type ('\b');
				}
			}
			events_out = (events_out + 1) % EVENTS;
		}

		glViewport (vp[0], vp[1], vp[2], vp[3]);
		glClearColor (0.07f, 0.08f, 0.10f, 1.0f);
		glClear (GL_COLOR_BUFFER_BIT);
		float hs = vp[3] >= 480 ? (float) (vp[3] / 240) : 1.0f;
		float y = vp[3] * 0.06f;
		const char *who = st.name[0] ? st.name : st.address;
		char s[48];
		hud_begin ();
		y = line (vp, y, hs, BLUE, "BLUETOOTH KEYBOARD");
		y += 10 * hs;
		if (!radio)
		{
			line (vp, y, hs, YELLOW, "NO RADIO");
		}
		else if (st.passkey[0])
		{
			snprintf (s, sizeof s, "TYPE %s", st.passkey);
			y = line (vp, y, hs * 2, YELLOW, s);
			y = line (vp, y, hs, WHITE, "ON THE KEYBOARD");
			line (vp, y, hs, WHITE, "THEN PRESS ENTER");
		}
		else switch (st.phase)
		{
		case BT_APP_STARTING:
			line (vp, y, hs, GREY, "STARTING");
			break;
		case BT_APP_SEARCHING:
			y = line (vp, y, hs, WHITE, "LOOKING FOR A KEYBOARD");
			y = line (vp, y, hs, GREY, st.kind == BT_LINK_LE ? "LE" : "CLASSIC");
			y += 10 * hs;
			line (vp, y, hs * 0.5f, GREY, "PUT THE KEYBOARD IN ITS PAIRING MODE");
			break;
		case BT_APP_CONNECTING:
		case BT_APP_WAITING:
			y = line (vp, y, hs, WHITE, st.phase == BT_APP_CONNECTING ? "CONNECTING TO" : "WAITING FOR");
			line (vp, y, hs, YELLOW, who);
			break;
		case BT_APP_CONNECTED:
			y = line (vp, y, hs, YELLOW, who);
			snprintf (s, sizeof s, "CONNECTED OVER %s", bt_link_kind_name (st.kind));
			y = line (vp, y, hs, GREEN, s);
			y += 10 * hs;
			for (int i = 0; i < TYPED_ROWS; i++)
			{
				char row[TYPED_COLS + 1];
				snprintf (row, sizeof row, "%-*s", TYPED_COLS, typed[i]);
				for (unsigned k = 0; row[k]; k++)
				{
					row[k] = row[k] >= 'a' && row[k] <= 'z' ? (char) (row[k] - 32) : row[k];
				}
				hud_text_scaled ((float) (int) ((vp[2] - TYPED_COLS * HUD_CHAR_W * hs) / 2), y, row, WHITE, hs);
				y += (HUD_CHAR_H + 3) * hs;
			}
			y += 6 * hs;
			line (vp, y, hs * 0.5f, GREY, last_key);
			break;
		}
		hud_end ();
		hud_draw ();
		pglSwapBuffers ();
		pgpu_wait_frame (100);			/* pace on the screen */
	}
}
