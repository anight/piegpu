/*
 * pincheck.c - before wiring the RPi: holds the link's output and REPLY pins
 * high, to be measured (3.3 V expected; the RPi's GPIOs are 3.3 V only), and
 * prints the READY and FRAME inputs once a second (PGPU_APP=pincheck)
 */
#include <stdio.h>
#include <unistd.h>
#include "driver/gpio.h"

static const struct { int gpio, pin; const char *name; } outs[] =
{
	{20, 13, "BCLK"}, {21, 11, "DATA"}, {22, 12, "FS"}, {23, 7, "REPLY"},
};

int main (void)
{
	for (unsigned i = 0; i < sizeof outs / sizeof outs[0]; i++)
	{
		gpio_reset_pin (outs[i].gpio);
		gpio_set_direction (outs[i].gpio, GPIO_MODE_OUTPUT);
		gpio_set_level (outs[i].gpio, 1);
		printf ("pincheck: %s GPIO%d (header pin %d) high: measure it against GND (pin 9)\n",
			outs[i].name, outs[i].gpio, outs[i].pin);
	}
	gpio_reset_pin (4);
	gpio_set_direction (4, GPIO_MODE_INPUT);
	gpio_reset_pin (5);
	gpio_set_direction (5, GPIO_MODE_INPUT);
	gpio_pulldown_en (5);
	for (;;)
	{
		printf ("pincheck: READY (GPIO4, pin 18) %d, FRAME (GPIO5, pin 16) %d\n",
			gpio_get_level (4), gpio_get_level (5));
		sleep (1);
	}
}
