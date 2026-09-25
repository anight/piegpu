#ifndef _kernel_h
#define _kernel_h

#include <circle/actled.h>
#include <circle/koptions.h>
#include <circle/devicenameservice.h>
#include <circle/serial.h>
#include <circle/exceptionhandler.h>
#include <circle/interrupt.h>
#include <circle/timer.h>
#include <circle/logger.h>
#include <circle/sched/scheduler.h>
#include <circle/spimaster.h>
#include <circle/2dgraphics.h>
#include <circle/types.h>
#include <vc4/vchiq/vchiqdevice.h>
#include <ili9486display.h>
#include <devlink.h>
#include "renderer.h"

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
	void Benchmark (unsigned nWidth, unsigned nHeight, unsigned nFrames);

private:
	// do not change this order
	CActLED			m_ActLED;
	CKernelOptions		m_Options;
	CDeviceNameService	m_DeviceNameService;
	CSerialDevice		m_Serial;
	CExceptionHandler	m_ExceptionHandler;
	CInterruptSystem	m_Interrupt;
	CTimer			m_Timer;
	CLogger			m_Logger;
	CScheduler		m_Scheduler;
	CDevLink		m_DevLink;

	CVCHIQDevice		m_VCHIQ;

	CSPIMaster		m_SPIMaster;
	CILI9486Display		m_Display;
	C2DGraphics		m_Graphics;

	CRenderer		m_Renderer;
};

#endif
