//From picosdl (backend/pico/joystick.h). Changed here: joystickInit says whether
//a stick is there.

#ifndef _JOYSTICK_H_
#define _JOYSTICK_H_

#include <stdbool.h>

//A simple two-axis analog stick with a push button.
//X is on ADC1 (gpio 27), Y is on ADC0 (gpio 26), the button is on gpio 22 (active low).

struct Joystick {
	float x;		//-1.0 (left)  .. +1.0 (right), 0.0 inside the deadzone
	float y;		//-1.0 (down)  .. +1.0 (up),    0.0 inside the deadzone
	bool pressed;		//button is currently held down (debounced)
	bool justPressed;	//button went down since the previous joystickRead()
};

//Must be called with the stick at rest - its resting position is taken as the center.
//False if nothing like a stick at rest is on the pins (a reading far from the middle,
//or one that wanders): pins left open read anything
bool joystickInit(void);

void joystickRead(struct Joystick *joy);

#endif
