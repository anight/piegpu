/*
 * board.h - where the Pico's stick and game controller are wired: picosdl's
 * board (its backend/pico/board.h, the names kept), so that one board serves
 * both. The link to the RPi is on GP16-21 (transports/pico-i2s).
 */
#ifndef PICO_INPUT_BOARD_H
#define PICO_INPUT_BOARD_H

/* The stick: two analog axes and a button. The axis pins are ADC inputs
   (GP26-29: joystick.c takes the input's number as the pin - 26). X and Y are
   not in pin order: that is how the stick is wired */
#define PSDL_BOARD_JOY_X_PIN		27	/* ADC1 */
#define PSDL_BOARD_JOY_Y_PIN		26	/* ADC0 */
#define PSDL_BOARD_JOY_BUTTON_PIN	22	/* active low */
#define PSDL_BOARD_JOY_INVERT_X		false
#define PSDL_BOARD_JOY_INVERT_Y		true	/* (up lowers the reading) */

/* The game controller: an Adafruit Gamepad QT (a seesaw device) on I2C1.
   400 kHz and the 100 us between a register's pointer and its read are
   picosdl's, measured there */
#define PSDL_BOARD_GAMEPAD_I2C		i2c1	/* GP6/GP7 are I2C1 */
#define PSDL_BOARD_GAMEPAD_SDA_PIN	6
#define PSDL_BOARD_GAMEPAD_SCL_PIN	7
#define PSDL_BOARD_GAMEPAD_BAUD		400000
#define PSDL_BOARD_GAMEPAD_ADDR		0x50	/* seesaw's default; jumpers give 51-53 */
#define PSDL_BOARD_GAMEPAD_READ_DELAY_US 100
#define PSDL_BOARD_GAMEPAD_INVERT_X	true	/* (as it's held: left raises the reading) */
#define PSDL_BOARD_GAMEPAD_INVERT_Y	false

#endif
