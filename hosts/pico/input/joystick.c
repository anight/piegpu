//From picosdl (backend/pico/joystick.c). Changed here: joystickInit says whether
//a stick is there, and what it prints.

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "board.h"
#include "joystick.h"

//Pins and the axis inversions come from board.h, which is the one place the
//board's wiring is described.
#define JOY_PIN_ADC_X		PSDL_BOARD_JOY_X_PIN
#define JOY_PIN_ADC_Y		PSDL_BOARD_JOY_Y_PIN
#define JOY_PIN_BUTTON		PSDL_BOARD_JOY_BUTTON_PIN

#define JOY_ADC_INPUT_X		(JOY_PIN_ADC_X - 26)
#define JOY_ADC_INPUT_Y		(JOY_PIN_ADC_Y - 26)
#define JOY_ADC_MAX		4095		//12-bit ADC

//flip these if an axis reads backwards for how your stick is wired
#define JOY_INVERT_X		PSDL_BOARD_JOY_INVERT_X
#define JOY_INVERT_Y		PSDL_BOARD_JOY_INVERT_Y

#define JOY_DEADZONE		0.12f		//fraction of full deflection ignored around the center
#define JOY_CALIB_DISCARD	16		//first reads after adc_gpio_init() are still settling
#define JOY_CALIB_SAMPLES	64
#define JOY_DEBOUNCE_US		20000
#define JOY_REST_MIN		(JOY_ADC_MAX / 4)	//a stick at rest reads between these...
#define JOY_REST_MAX		(JOY_ADC_MAX * 3 / 4)
#define JOY_REST_NOISE		200		//...and within this of itself (5% of the range)

static uint16_t mCenterX, mCenterY;
static uint32_t mBtnLastChangeTime;
static bool mBtnRaw, mBtnStable;

static uint16_t joyPrvReadRaw(uint32_t adcInput)
{
	adc_select_input(adcInput);
	return adc_read();
}

//maps a raw reading to -1.0 .. +1.0 around the calibrated center, with the deadzone removed
static float joyPrvNormalize(uint16_t raw, uint16_t center, bool invert)
{
	float v;

	if (raw >= center)
		v = (float)(raw - center) / (float)(JOY_ADC_MAX - center);
	else
		v = -(float)(center - raw) / (float)center;

	if (v > JOY_DEADZONE)
		v = (v - JOY_DEADZONE) / (1.f - JOY_DEADZONE);
	else if (v < -JOY_DEADZONE)
		v = (v + JOY_DEADZONE) / (1.f - JOY_DEADZONE);
	else
		v = 0.f;

	return invert ? -v : v;
}

bool joystickInit(void)
{
	uint32_t sumX = 0, sumY = 0;
	uint16_t minX = JOY_ADC_MAX, maxX = 0, minY = JOY_ADC_MAX, maxY = 0;
	unsigned i;

	adc_init();
	adc_gpio_init(JOY_PIN_ADC_X);
	adc_gpio_init(JOY_PIN_ADC_Y);

	gpio_init(JOY_PIN_BUTTON);
	gpio_set_dir(JOY_PIN_BUTTON, GPIO_IN);
	gpio_pull_up(JOY_PIN_BUTTON);		//button shorts the pin to ground when pressed

	for (i = 0; i < JOY_CALIB_DISCARD; i++) {
		(void)joyPrvReadRaw(JOY_ADC_INPUT_X);
		(void)joyPrvReadRaw(JOY_ADC_INPUT_Y);
	}

	for (i = 0; i < JOY_CALIB_SAMPLES; i++) {
		uint16_t x = joyPrvReadRaw(JOY_ADC_INPUT_X), y = joyPrvReadRaw(JOY_ADC_INPUT_Y);

		sumX += x;
		sumY += y;
		minX = x < minX ? x : minX;
		maxX = x > maxX ? x : maxX;
		minY = y < minY ? y : minY;
		maxY = y > maxY ? y : maxY;
	}

	mCenterX = sumX / JOY_CALIB_SAMPLES;
	mCenterY = sumY / JOY_CALIB_SAMPLES;

	//a center sitting on either rail would make joyPrvNormalize() divide by zero
	if (mCenterX < 1)
		mCenterX = 1;
	else if (mCenterX > JOY_ADC_MAX - 1)
		mCenterX = JOY_ADC_MAX - 1;
	if (mCenterY < 1)
		mCenterY = 1;
	else if (mCenterY > JOY_ADC_MAX - 1)
		mCenterY = JOY_ADC_MAX - 1;

	mBtnRaw = false;
	mBtnStable = false;
	mBtnLastChangeTime = time_us_32();

	//a stick at rest sits near the middle of both axes and stays there
	if (mCenterX < JOY_REST_MIN || mCenterX > JOY_REST_MAX || mCenterY < JOY_REST_MIN || mCenterY > JOY_REST_MAX ||
	    maxX - minX > JOY_REST_NOISE || maxY - minY > JOY_REST_NOISE) {
		printf("pad: no stick on GP%u/GP%u (x %u..%u, y %u..%u at rest)\n", JOY_PIN_ADC_X, JOY_PIN_ADC_Y,
			minX, maxX, minY, maxY);
		return false;
	}

	printf("pad: a stick on GP%u/GP%u, its button on GP%u: center x=%u y=%u\n", JOY_PIN_ADC_X, JOY_PIN_ADC_Y,
		JOY_PIN_BUTTON, mCenterX, mCenterY);
	return true;
}

void joystickRead(struct Joystick *joy)
{
	bool wasPressed = mBtnStable;
	bool raw = !gpio_get(JOY_PIN_BUTTON);	//active low
	uint32_t now = time_us_32();

	joy->x = joyPrvNormalize(joyPrvReadRaw(JOY_ADC_INPUT_X), mCenterX, JOY_INVERT_X);
	joy->y = joyPrvNormalize(joyPrvReadRaw(JOY_ADC_INPUT_Y), mCenterY, JOY_INVERT_Y);

	if (raw != mBtnRaw) {			//still bouncing - restart the timer
		mBtnRaw = raw;
		mBtnLastChangeTime = now;
	} else if (now - mBtnLastChangeTime >= JOY_DEBOUNCE_US) {
		mBtnStable = raw;
	}

	joy->pressed = mBtnStable;
	joy->justPressed = mBtnStable && !wasPressed;
}
