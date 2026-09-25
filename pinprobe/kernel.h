#ifndef _kernel_h
#define _kernel_h

#include <circle/actled.h>
#include <circle/koptions.h>
#include <circle/devicenameservice.h>
#include <circle/nulldevice.h>
#include <circle/exceptionhandler.h>
#include <circle/interrupt.h>
#include <circle/timer.h>
#include <circle/logger.h>
#include <circle/spimaster.h>
#include <circle/2dgraphics.h>
#include <circle/types.h>
#include <ili9486display.h>
#include <devlink.h>

enum TShutdownMode
{
	ShutdownNone,
	ShutdownHalt,
	ShutdownReboot
};

class CKernel
{
public:
	CKernel (void);
	~CKernel (void);

	boolean Initialize (void);

	TShutdownMode Run (void);

private:
	void DumpHeader (const char *pWhen);
	void ProbePulls (void);
	void ProbeTouch (void);
	void FillScreen (T2DColor Color, const char *pText);
	void Phase (unsigned nPhase, const char *pDescription);
	void Ready (unsigned nPhase);
	void Wait (unsigned nSeconds);

private:
	// do not change this order
	CActLED			m_ActLED;
	CKernelOptions		m_Options;
	CDeviceNameService	m_DeviceNameService;
	CNullDevice		m_Null;		// no UART: GPIO14/15 stay untouched
	CExceptionHandler	m_ExceptionHandler;
	CInterruptSystem	m_Interrupt;
	CTimer			m_Timer;
	CLogger			m_Logger;
	CDevLink		m_DevLink;

	CSPIMaster		m_SPIMaster;
	CILI9486Display		m_Display;
	C2DGraphics		m_Graphics;
};

#endif
