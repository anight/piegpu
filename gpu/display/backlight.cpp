//
// backlight.cpp
//
#include "backlight.h"
#include <circle/logger.h>

LOGMODULE ("backlight");

CBacklight::CBacklight (void)
:	m_PWM (GPIOClockSourceOscillator, Divider, Range, TRUE),
	m_nPercent (100)
{
}

boolean CBacklight::Initialize (unsigned nPercent)
{
	m_Pin.AssignPin (Pin);
	m_Pin.SetMode (GPIOModeAlternateFunction0);
	if (!m_PWM.Start ())
	{
		LOGWARN ("Backlight: the PWM didn't start");
		return FALSE;
	}
	SetBrightness (nPercent);
	LOGNOTE ("Backlight: GPIO12 (pin 32), PWM at 10 kHz, brightness %u%%", m_nPercent);
	return TRUE;
}

void CBacklight::SetBrightness (unsigned nPercent)
{
	m_nPercent = nPercent > 100 ? 100 : nPercent;
	m_PWM.Write (PWM_CHANNEL1, Range * m_nPercent / 100);
}
