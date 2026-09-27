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
#include <circle/types.h>
#include <devlink.h>
#include <v3d.h>
#include "link/i2s_link.h"
#include "link/usb_link.h"
#include "display/panel_output.h"
#include "renderer.h"
#include "commands.h"

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
	void ShowSplash (void);
	void DumpScreenshot (void);

private:
	// do not change this order
	CActLED			m_ActLED;
	CKernelOptions		m_Options;
	CDeviceNameService	m_DeviceNameService;
	CNullDevice		m_Null;		// no UART: GPIO14/15 untouched
	CExceptionHandler	m_ExceptionHandler;
	CInterruptSystem	m_Interrupt;
	CTimer			m_Timer;
	CLogger			m_Logger;
	CDevLink		m_DevLink;

	CPanelOutput		m_Output;		// the ST7789 (HDMI would go here)
	CV3D			m_V3D;
	CI2SLink		m_I2SLink;		// commands from the Pico
	CUSBLink		m_USBLink;		// commands from a PC over USB (tests)
	CRenderer		m_Renderer;
	CCommands		m_Commands;

	static const unsigned Links = 2;
	CLink			*m_pLinks[Links];	// in priority order: the first active one serves
};

#endif
