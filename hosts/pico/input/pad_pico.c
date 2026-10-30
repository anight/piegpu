/*
 * pad_pico.c - pad.h on the Pico: the analog stick on its ADC (joystick.c)
 * and an Adafruit Gamepad QT on I2C (seesaw_gamepad.c), picosdl's drivers and
 * its wiring (board.h). Each is looked for at the start; what's there is read
 * every PAD_POLL_US at most (the controller's reading is three I2C transfers,
 * about a millisecond).
 *
 * With PGPU_PAD_BLUETOOTH (a Pico 2 W), a Bluetooth game controller as well:
 * the HID host (../bt) looks for one in its pairing mode, remembers it and
 * reconnects to it; its stick and hat join the others' (the further each way
 * wins), its buttons theirs. Its buttons' numbers vary by maker: the table
 * below is the common one; what it sends is printed as it changes, to see.
 */
#include "pad.h"
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "board.h"
#include "joystick.h"
#include "seesaw_gamepad.h"
#ifdef PGPU_PAD_BLUETOOTH
#include "pico/cyw43_arch.h"
#include "btstack.h"
#include "bt_app.h"
#include "gamepad_decode.h"

static bool have_bt;

/* the controller's buttons (bit n: its button n + 1) as the pad's: the usual layout of Android
   controllers (1 A, 2 B, 4 X, 5 Y, 11 SELECT, 12 START: checked with a small "game / key" pad, in
   its game mode), and 3 and 6 taken as X and Y too (other makers') */
static const struct { uint32_t bit; unsigned pad; } bt_buttons[] =
{
	{1u << 0, PAD_A}, {1u << 1, PAD_B}, {1u << 2, PAD_X}, {1u << 3, PAD_X}, {1u << 4, PAD_Y}, {1u << 5, PAD_Y},
	{1u << 10, PAD_SELECT}, {1u << 11, PAD_START}, {1u << 13, PAD_STICK}
};

/* a controller in its keyboard mode: the arrows and w a s d steer; these keys are buttons */
static const struct { uint8_t key; unsigned pad; } bt_keys[] =
{
	{0x28, PAD_START}, {0x2C, PAD_A}, {0x1D, PAD_B}, {0x1B, PAD_X}, {0x1C, PAD_Y}, {0x29, PAD_SELECT}
};
#endif

#define PAD_POLL_US	10000
#define PAD_DEADZONE	0.12f		/* the controller's stick: as the analog one's */

static bool have_stick, have_controller;
static pad_t last;
static uint64_t last_us;

bool pad_init (void)
{
	have_stick = joystickInit ();
	have_controller = seesaw_gamepad_init ();
	printf ("pad: stick %s, game controller %s\n", have_stick ? "present" : "absent",
		have_controller ? "present" : "absent");
	memset (&last, 0, sizeof last);
	last_us = 0;
#ifdef PGPU_PAD_BLUETOOTH
	have_bt = cyw43_arch_init () == 0;
	if (have_bt)
	{
		bt_app_look_for (BT_DEVICE_GAMEPAD);
		bt_app_setup ();
		hci_power_control (HCI_POWER_ON);
		printf ("pad: Bluetooth up: a game controller in its pairing mode is looked for (one paired: reconnected)\n");
	}
	else
	{
		printf ("pad: no Bluetooth (the radio didn't start)\n");
	}
	return have_stick || have_controller || have_bt;
#else
	return have_stick || have_controller;
#endif
}

/* the controller's axis: 0 .. 1023 about 512, its middle left out */
static float axis (unsigned raw, bool invert)
{
	float v = ((float) raw - 512.0f) / 512.0f;
	v = v > PAD_DEADZONE ? (v - PAD_DEADZONE) / (1.0f - PAD_DEADZONE)
	  : v < -PAD_DEADZONE ? (v + PAD_DEADZONE) / (1.0f - PAD_DEADZONE) : 0.0f;
	v = v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v;
	return invert ? -v : v;
}

static float further (float a, float b)
{
	return (a < 0 ? -a : a) >= (b < 0 ? -b : b) ? a : b;
}

bool pad_read (pad_t *p)
{
	uint64_t now = time_us_64 ();
	*p = last;
	if ((have_stick || have_controller) && now - last_us >= PAD_POLL_US)
	{
		last_us = now;
		float x = 0.0f, y = 0.0f;
		unsigned buttons = last.buttons & ~PAD_STICK;
		if (have_stick)
		{
			struct Joystick joy;
			joystickRead (&joy);
			x = joy.x;
			y = joy.y;
			buttons |= joy.pressed ? PAD_STICK : 0;
		}
		seesaw_gamepad_state_t g;
		if (have_controller && seesaw_gamepad_read (&g))	/* (a transfer lost: as it was) */
		{
			/* its Y grows downwards (as SDL's, which picosdl gives it to) */
			x = further (x, axis (g.x, PSDL_BOARD_GAMEPAD_INVERT_X));
			y = further (y, axis (g.y, !PSDL_BOARD_GAMEPAD_INVERT_Y));
			buttons = (buttons & PAD_STICK) | (g.a ? PAD_A : 0) | (g.b ? PAD_B : 0) | (g.x_btn ? PAD_X : 0)
				  | (g.y_btn ? PAD_Y : 0) | (g.select ? PAD_SELECT : 0) | (g.start ? PAD_START : 0);
		}
		else if (have_controller)
		{
			x = further (x, last.x);
			y = further (y, last.y);
		}
		last.x = x;
		last.y = y;
		last.buttons = buttons;
	}
#ifdef PGPU_PAD_BLUETOOTH
	if (have_bt)
	{
		static gamepad_state_t shown;
		gamepad_state_t s;
		gamepad_decode_get (&s);
		pad_t b = {0};
		if (s.connected)
		{
			if (s.pad.has_stick)
			{
				b.x = axis ((unsigned) ((s.pad.x + 1.0f) * 512.0f), false);
				b.y = axis ((unsigned) ((s.pad.y + 1.0f) * 512.0f), true);	/* (its y grows down) */
			}
			if (s.pad.hat >= 0)
			{
				static const int8_t hx[8] = {0, 1, 1, 1, 0, -1, -1, -1}, hy[8] = {1, 1, 0, -1, -1, -1, 0, 1};
				b.x = further (b.x, hx[s.pad.hat]);
				b.y = further (b.y, hy[s.pad.hat]);
			}
			for (unsigned i = 0; i < sizeof bt_buttons / sizeof bt_buttons[0]; i++)
			{
				b.buttons |= (s.pad.buttons & bt_buttons[i].bit) ? bt_buttons[i].pad : 0;
			}
			for (int k = 0; k < KBD_MAX_KEYS; k++)
			{
				uint8_t key = s.keys[k];
				b.x = further (b.x, key == 0x4F || key == 0x07 ? 1.0f : key == 0x50 || key == 0x04 ? -1.0f : 0.0f);
				b.y = further (b.y, key == 0x52 || key == 0x1A ? 1.0f : key == 0x51 || key == 0x16 ? -1.0f : 0.0f);
				for (unsigned i = 0; key && i < sizeof bt_keys / sizeof bt_keys[0]; i++)
				{
					b.buttons |= key == bt_keys[i].key ? bt_keys[i].pad : 0;
				}
			}
			/* what it sends, as it changes (the first 40): to see a new controller's numbers */
			static int changes;
			if (changes < 40 && (s.pad.buttons != shown.pad.buttons || s.pad.hat != shown.pad.hat || memcmp (s.keys, shown.keys, sizeof s.keys)
			    || (s.pad.x > 0.5f) != (shown.pad.x > 0.5f) || (s.pad.x < -0.5f) != (shown.pad.x < -0.5f)
			    || (s.pad.y > 0.5f) != (shown.pad.y > 0.5f) || (s.pad.y < -0.5f) != (shown.pad.y < -0.5f)))
			{
				changes++;
				printf ("pad: bluetooth: buttons %08lx hat %d stick %+.2f %+.2f keys %02x %02x %02x (%lu reports)\n",
					(unsigned long) s.pad.buttons, s.pad.hat, (double) s.pad.x, (double) s.pad.y, s.keys[0], s.keys[1],
					s.keys[2], (unsigned long) s.reports);
				shown = s;
			}
		}
		p->x = further (p->x, b.x);
		p->y = further (p->y, b.y);
		p->buttons |= b.buttons;
		return true;
	}
#endif
	return have_stick || have_controller;
}
