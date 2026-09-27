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
#include <circle/bcmframebuffer.h>
#include <circle/types.h>
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
	void ProbePanel (void);
	void Poll (boolean bForce);
	void Fill (unsigned nFrame);

private:
	// do not change this order
	CActLED			m_ActLED;
	CKernelOptions		m_Options;
	CDeviceNameService	m_DeviceNameService;
	CNullDevice		m_Null;
	CExceptionHandler	m_ExceptionHandler;
	CInterruptSystem	m_Interrupt;
	CTimer			m_Timer;
	CLogger			m_Logger;
	CDevLink		m_DevLink;

	CBcmFrameBuffer		*m_pFrameBuffer;
	unsigned		m_nWidth, m_nHeight;

	char			m_Last[512];
};

#endif
