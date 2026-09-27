// circle_micros.cpp - the microsecond clock of compat/mmal_circle.h
#include <circle/timer.h>

extern "C" unsigned long long circle_micros (void)
{
	return CTimer::GetClockTicks64 ();
}
