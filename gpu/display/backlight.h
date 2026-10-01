//
// backlight.h
//
// The panel's backlight, dimmed by hardware PWM: its LED pin on GPIO12
// (physical pin 32, PWM0) at 10 kHz. Without the pin wired there (the LED
// pin on 3.3 V) the backlight is just on, whatever the brightness.
//
#ifndef _gpu_display_backlight_h
#define _gpu_display_backlight_h

#include <circle/pwmoutput.h>
#include <circle/gpiopin.h>
#include <circle/types.h>

class CBacklight
{
public:
	CBacklight (void);

	/// \param nPercent The brightness, 0 (off) .. 100
	boolean Initialize (unsigned nPercent);

	void SetBrightness (unsigned nPercent);
	unsigned GetBrightness (void) const	{ return m_nPercent; }

private:
	static const unsigned Pin = 12;			// PWM0 (ALT0)
	static const unsigned Divider = 2;		// the 19.2 MHz oscillator: 9.6 MHz ...
	static const unsigned Range = 960;		// ... 10 kHz

	CPWMOutput m_PWM;
	CGPIOPin m_Pin;
	unsigned m_nPercent;
};

#endif
