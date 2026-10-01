/*
 * pad_pico.c - pad.h on the Pico: the analog stick on its ADC (joystick.c)
 * and an Adafruit Gamepad QT on I2C (seesaw_gamepad.c), picosdl's drivers and
 * its wiring (board.h). Each is looked for at the start; what's there is read
 * every PAD_POLL_US at most (the controller's reading is three I2C transfers,
 * about a millisecond).
 */
#include "pad.h"
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "board.h"
#include "joystick.h"
#include "seesaw_gamepad.h"

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
	return have_stick || have_controller;
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
	*p = last;
	return have_stick || have_controller;
}
