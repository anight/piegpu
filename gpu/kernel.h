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
#include "display/backlight.h"
#include "settings.h"
#include "ui/settings_app.h"
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

#ifdef PGPU_WIRELESS
	#include "bt/bluetooth.h"
#endif

class CKernel : public CSettingsHost
{
public:
	CKernel (void);
	~CKernel (void);

	boolean Initialize (void);

	TShutdownMode Run (void);

private:
	void HostInput (void);
	boolean DetectPanel (void);
	boolean LoadSettings (void);

	// Settings' (CSettingsHost)
	void SaveSettings (void);
	boolean HasBluetooth (void) const;
	const char *GetBluetoothNote (void);
	void SetSpeakerSearch (boolean bOn);
	unsigned GetSpeakers (TSpeaker *pSpeakers, unsigned nMax);
	unsigned GetSpeakersGeneration (void);
	void PickSpeaker (const char *pAddress);
	void ForgetSpeaker (const char *pAddress);
#ifdef PGPU_WIRELESS
	void LoadSpeakers (void);
	void SaveSpeakers (void);
	void UpdateSpeakerVolume (void);
#endif
	boolean GetBluetooth (void) const	{ return m_bBluetooth; }
	void SetBluetooth (boolean bOn);
	void LoadKernelOptions (void);
	const char *GetKernelOption (const char *pKey);
	void SetKernelOption (const char *pKey, const char *pValue);
	boolean SaveKernelOptions (void);
	void GetClockRange (boolean bV3D, unsigned *pMinMHz, unsigned *pMaxMHz);
	const char *TestSound (void);
	void Restart (void);

#ifdef PGPU_WIRELESS
	void BluetoothLine (const char *pLine);
#endif
	void SetV3DClock (void);
	static unsigned GetClock (u32 nClockId, u32 nTag);	// MHz

	boolean LongPress (const u32 *pTouch);
	void OpenSettings (void);
	void CloseSettings (void);
	/// \return A user setting: settings.txt's, else the kernel command line's, else pDefault
	const char *Setting (const char *pKey, const char *pDefault = nullptr);
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
	CSettings		m_Settings;		// settings.txt (the user's)
	CSettings		m_CmdLine;		// cmdline.txt as the card has it (Settings: Kernel)
	boolean			m_bCmdLineRead;		// (else: what this start had)
	boolean			m_bBluetooth;		// bluetooth= (for speakers, to come)
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
	CBacklight		m_Backlight;		// the panel's (PWM)
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
#ifdef PGPU_WIRELESS
	CBluetooth		m_Bluetooth;		// for a speaker (bt/)
	CString			m_BluetoothNote;
	boolean			m_bSpeakerVolume;	// the speaker's own volume is set from here
	unsigned		m_nSpeakerVolume;	// what it was given last, percent (~0: nothing yet)
	unsigned		m_nVolumeSaveAt;	// its buttons changed the volume: settings.txt then (0: no)
#endif
	CSettingsApp		m_SettingsApp;		// on the panel, a long press opens it (gpu/ui)
	boolean			m_bPressing;		// the long press: held since, where
	unsigned		m_nPressStart;
	int			m_nPressX, m_nPressY;
	unsigned		m_nV3DMin, m_nV3DMax;	// the V3D's clock as the firmware allows it, MHz
	unsigned		m_nTestTouchEnd;	// the TOUCH host line's finger: down till then (0: let go)
	boolean			m_bTestTouch, m_bTestTouchChanged;
	unsigned		m_nTestTouchX, m_nTestTouchY;
#ifdef ARM_ALLOW_MULTI_CORE
	CCores			m_Cores;		// core 1: the audio stream's decoder
#endif

	static const unsigned Links = 3;
	CLink			*m_pLinks[Links];	// in priority order: the first active one serves
};

#endif
