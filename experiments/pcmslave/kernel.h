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
#include <circle/gpiopin.h>
#include <circle/dmachannel.h>
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
	void SetupPCM (void);
	boolean Burst (unsigned nWords, unsigned *pRxUs);
	boolean Verify (unsigned nWords, unsigned *pChecked, unsigned *pWordErrors,
			unsigned *pBitErrors, int *pBitOffset);

	static void DMACompletion (unsigned nChannel, unsigned nBuffer, boolean bStatus, void *pParam);

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

	CGPIOPin		m_PinCLK;
	CGPIOPin		m_PinFS;
	CGPIOPin		m_PinDIN;
	CGPIOPin		m_PinDOUT;

	CDMAChannel		m_RxDMA;
	volatile boolean	m_bDMADone;

	u32			*m_pRxBuffer;
	u32			m_nPatternInverse;
	u32			m_nCSAtRxDone;
};

#endif
