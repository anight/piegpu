#ifndef _kernel_h
#define _kernel_h

#include <circle/actled.h>
#include <circle/koptions.h>
#include <circle/devicenameservice.h>
#include <circle/nulldevice.h>
#include <circle/exceptionhandler.h>
#include <circle/interrupt.h>
#include <circle/timer.h>
#include <circle/cputhrottle.h>
#include <circle/logger.h>
#include <circle/sched/scheduler.h>
#include <circle/types.h>
#include <circle/2dgraphics.h>
#include <circle/chargenerator.h>
#include <circle/font.h>
#include <devlink.h>
#include <runlog.h>
#include <pgpugadget.h>
#include <vc4/vchiq/vchiqdevice.h>
#include <v3d.h>
#include "link/i2s_link.h"
#include "link/usb_link.h"
#include "link/usb_bulk_link.h"
#include "display/panel_output.h"
#include "display/touch.h"
#include "display/hdmi_output.h"
#include "display/hdmi_monitor.h"
#include "display/gud_display.h"
#include "display/offscreen_output.h"
#include "renderer.h"
#include "commands.h"
#include "install/installer.h"

enum TShutdownMode
{
	ShutdownNone,
	ShutdownHalt,
	ShutdownReboot
};

#ifdef ARM_ALLOW_MULTI_CORE
#include <circle/multicore.h>
#include <circle/memory.h>

// the cores beside core 0 (the Zero 2 W's): core 1 decodes the audio stream
// (CAudio::DecodeLoop), cores 2 and 3 halt
class CCores : public CMultiCoreSupport
{
public:
	CCores (CAudio *pAudio)
	:	CMultiCoreSupport (CMemorySystem::Get ()),
		m_pAudio (pAudio)
	{
	}

	void Run (unsigned nCore) override
	{
		if (nCore == 1)
		{
			m_pAudio->DecodeLoop ();
		}
	}

private:
	CAudio *m_pAudio;
};
#endif

class CKernel
{
public:
	CKernel (void);
	~CKernel (void);

	boolean Initialize (void);

	TShutdownMode Run (void);

private:
	void HostInput (void);
	boolean DetectPanel (void);
	void ChooseOutput (COutput **ppOutput, unsigned *pWidth, unsigned *pHeight);
	void HDMISize (unsigned *pWidth, unsigned *pHeight);
	void ApplyOutput (void);
	boolean HostIdle (void) const;
	void EndSession (CLink *pLink);
	void SendDisplay (boolean bSend = TRUE);
	void ShowText (COutput *pOutput, const char *pTitle, const char *pLine1, const char *pLine2);
	void ShowLines (COutput *pOutput, const char *const *ppLines, unsigned nLines);
	void ShowSplash (COutput *pOutput);
	void DrawScaledText (C2DGraphics &Graphics, unsigned x0, unsigned y0, unsigned k,
			     unsigned x, unsigned y, boolean bCentre, T2DColor Color,
			     const char *pText, const TFont &rFont);
	void ShowPanelNotice (void);
	static u32 GetThrottled (void);
	void DumpScreenshot (void);
	void DumpAudio (void);
	void SoundTest (void);
	boolean WriteBase64 (const u8 *p, unsigned nBytes);

private:
	// do not change this order
	CActLED			m_ActLED;
	CKernelOptions		m_Options;
	CDeviceNameService	m_DeviceNameService;
	CNullDevice		m_Null;		// no UART: GPIO14/15 untouched
	CExceptionHandler	m_ExceptionHandler;
	CInterruptSystem	m_Interrupt;
	CTimer			m_Timer;
	CCPUThrottle		m_CPUThrottle;		// the ARM at its maximum clock (cpu=low: not); no Update ()
	CLogger			m_Logger;
	CRunLog			m_RunLog;
	CScheduler		m_Scheduler;		// VCHIQ's tasks (video) run when the main loop yields
	CPieGPUGadget		m_Gadget;		// USB: serial port + monitor (GUD)
	CDevLink		m_DevLink;
	CInstaller		m_Installer;
	char			m_HostLine[CInstaller::MaxLine + 1];	// a line from the host (HostInput)
	unsigned		m_nHostLine;
	CVCHIQDevice		m_VCHIQ;		// the VideoCore's services (video)

	// the outputs (README "Kernel command line"): output=auto|panel|hdmi, panel=none,
	// hdmi_pixels=N in cmdline.txt
	enum TOutputMode {OutputAuto, OutputPanel, OutputHDMI};
	TOutputMode		m_OutputMode;
	// where GL commands come from: host=auto (a PC over USB once it opens
	// its stream, else I2S), usb (only a PC), i2s (only a Pico or ESP32-P4)
	enum THostMode {HostAuto, HostUSB, HostI2S};
	THostMode		m_HostMode;
	boolean			m_bGUD;			// gud=on: a USB monitor (off by default)
	boolean			m_bJobCheck;		// clcheck=on (the default): control lists checked (v3dcheck.h)
	unsigned		m_nARMClock;		// the rate last logged, Hz
	boolean			m_bPanelPresent;
	unsigned		m_nHDMIPixels;		// the largest screen on HDMI (default: native)
	CPanelOutput		m_Panel;
	CTouch			m_Touch;		// the panel's touch screen (on its SPI)
	CHDMIOutput		m_HDMI;
	CHDMIMonitor		m_Monitor;
	COutput			*m_pScreen;		// the panel or HDMI: where the picture is
	CGUDDisplay		m_GUD;			// a PC's desktop on the screen (a USB monitor)
	COffscreenOutput	m_Offscreen;		// the GL frames' while the desktop shows
	boolean			m_bOutputPending;	// to be chosen again between frames
	boolean			m_bFrameSeen;		// a host's FRAME_END since boot
	unsigned		m_nLastFrame;		// its time (CTimer::GetClockTicks)
	CV3D			m_V3D;
	CI2SLink		m_I2SLink;		// commands from the Pico
	CUSBBulkLink		m_USBBulkLink;		// commands from a PC over USB: the GL interface (libusb)
	CUSBLink		m_USBLink;		// ... or the serial port
	CRenderer		m_Renderer;
	CCommands		m_Commands;
	CAudio			m_Audio;		// the audio stream, on HDMI (volume= option)
#ifdef ARM_ALLOW_MULTI_CORE
	CCores			m_Cores;		// core 1: the audio stream's decoder
#endif

	static const unsigned Links = 3;
	CLink			*m_pLinks[Links];	// in priority order: the first active one serves
};

#endif
