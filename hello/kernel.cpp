#include "kernel.h"
#include <circle/timer.h>

CKernel::CKernel (void)
{
}

CKernel::~CKernel (void)
{
}

boolean CKernel::Initialize (void)
{
	return TRUE;
}

TShutdownMode CKernel::Run (void)
{
	while (1)	// blink forever
	{
		m_ActLED.On ();
		CTimer::SimpleMsDelay (200);

		m_ActLED.Off ();
		CTimer::SimpleMsDelay (500);
	}

	return ShutdownHalt;
}
