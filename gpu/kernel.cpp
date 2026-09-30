//
// gpu: the RPi side of piegpu (docs/protocol.md).
// Receives the command stream from the host over a link (link/: the Pico's
// I2S, or a PC over USB) and renders with the V3D to an output (display/: the
// ST7789 panel, or HDMI: by default HDMI while a monitor is connected, else
// the panel).
//
#include "kernel.h"
#include "build_info.h"
#include <v3dcheck.h>
#include <circle/2dgraphics.h>
#include <circle/machineinfo.h>
#include <circle/bcmpropertytags.h>
#include <circle/string.h>
#include <circle/util.h>
#include <circle/synchronize.h>
#include <pgpu_protocol.h>
#include <circle/memory.h>

#define MAX_PACKETS_PER_LOOP	64
#define MAX_US_PER_LOOP		1000	// then VCHIQ's tasks (video) get their turn
#define QUIET_HOST_US		1000000	// no packets mid-frame: the host went away

LOGMODULE ("gpu");

// while a V3D job runs: VCHIQ's tasks (the video decoder's messages) run too;
// a few more milliseconds without them at the end of a frame (a frame's
// conversion into its texture, or a test's 4 ms wait) stalled the decoder for
// good after four frames
static void V3DWait (void)
{
	CScheduler::Get ()->Yield ();
}

CKernel::CKernel (void)
:	m_Timer (&m_Interrupt),
	// the firmware starts the ARM at its lowest clock (the Zero 2 W: 600 of
	// 1000 MHz). The temperature is the firmware's to watch, at its own limit
	// (it throttles, and the throttle flags say so): CCPUThrottle's Update ()
	// isn't called, which would slow the ARM down at 60 C (socmaxtemp)
	m_CPUThrottle (strcmp (m_Options.GetAppOptionString ("cpu", "max"), "low") == 0
		       ? CPUSpeedLow : CPUSpeedMaximum),
	m_Logger (m_Options.GetLogLevel (), &m_Timer),
	m_Gadget (&m_Interrupt),
	m_DevLink (&m_Interrupt, &m_Gadget),
	m_Installer (&m_Interrupt, &m_Timer, &m_DevLink),
	m_nHostLine (0),
	m_VCHIQ (CMemorySystem::Get (), &m_Interrupt),
	m_OutputMode (OutputAuto),
	m_HostMode (HostAuto),
	m_bGUD (TRUE),
	m_bJobCheck (TRUE),
	m_nARMClock (0),
	m_bPanelPresent (TRUE),
	m_nHDMIPixels (CRenderer::MaxPixels),
	m_Panel (&m_Interrupt),
	m_pScreen (nullptr),
	m_GUD (&m_Gadget),
	m_bOutputPending (FALSE),
	m_bFrameSeen (FALSE),
	m_nLastFrame (0),
	m_USBBulkLink (&m_Gadget),
	m_USBLink (&m_DevLink),
	m_Renderer (&m_V3D),
	m_Commands (&m_Renderer, &m_I2SLink),
	m_Audio (&m_VCHIQ, m_Commands.GetVideo ()),
#ifdef ARM_ALLOW_MULTI_CORE
	m_Cores (&m_Audio),
#endif
	m_pLinks {&m_USBBulkLink, &m_USBLink, &m_I2SLink}
{
}

CKernel::~CKernel (void)
{
}

boolean CKernel::Initialize (void)
{
	const char *pHost = m_Options.GetAppOptionString ("host", "auto");
	m_HostMode =   strcmp (pHost, "usb") == 0 ? HostUSB
		     : strcmp (pHost, "i2s") == 0 ? HostI2S : HostAuto;
	const char *pOutput = m_Options.GetAppOptionString ("output", "auto");
	m_OutputMode =   strcmp (pOutput, "panel") == 0 ? OutputPanel
		       : strcmp (pOutput, "hdmi") == 0 ? OutputHDMI : OutputAuto;
	m_bGUD = strcmp (m_Options.GetAppOptionString ("gud", "on"), "off") != 0;
	m_bJobCheck = strcmp (m_Options.GetAppOptionString ("clcheck", "on"), "off") != 0;
	m_nHDMIPixels = m_Options.GetAppOptionDecimal ("hdmi_pixels", m_nHDMIPixels);
	m_Audio.SetDefaultVolume (m_Options.GetAppOptionDecimal ("volume", 10));	// percent
	m_Commands.SetAudio (&m_Audio);
	if (m_nHDMIPixels < 320 * 240 || m_nHDMIPixels > CRenderer::MaxPixels)
	{
		m_nHDMIPixels = CRenderer::MaxPixels;
	}
	m_DevLink.RegisterRebootHandler (CRunLog::Restarting);

	return    m_Logger.Initialize (&m_Null)
	       && m_RunLog.Initialize (&m_Logger, m_Options.GetLogLevel ())
	       && m_Interrupt.Initialize ()
	       && m_Timer.Initialize ()
	       && (!m_bGUD || m_GUD.Initialize ())	// (before the gadget starts)
	       && (m_Gadget.SetStream (&m_USBBulkLink), TRUE)
	       && m_DevLink.Initialize ()
	       && DetectPanel ()
	       && m_VCHIQ.Initialize ()
	       && (!(m_bPanelPresent || m_OutputMode == OutputPanel) || m_Panel.Initialize ())
	       && (m_OutputMode == OutputPanel || m_HDMI.Initialize ())
#ifdef ARM_ALLOW_MULTI_CORE
	       && m_Cores.Initialize ()
#endif
	       ;
}

// text from the host (the USB serial link): "s" alone asks for a screenshot,
// lines that start with "PGI " go to the installer (gpu/install)
void CKernel::HostInput (void)
{
	int c;
	while ((c = m_DevLink.GetChar ()) >= 0)
	{
		if (c == '\r' || c == '\n')
		{
			m_HostLine[m_nHostLine] = '\0';
			if (strncmp (m_HostLine, "PGI ", 4) == 0)
			{
				m_Installer.Command (m_HostLine);
			}
			m_nHostLine = 0;
		}
		else if (c == 's' && m_nHostLine == 0)
		{
			DumpScreenshot ();
		}
		else if (m_nHostLine < CInstaller::MaxLine)
		{
			m_HostLine[m_nHostLine++] = (char) c;
		}
	}
}

// panel=auto (the default): a panel if one answers over MISO; panel=yes: one
// is there (MISO not wired); panel=none: there's none
boolean CKernel::DetectPanel (void)
{
	const char *pPanel = m_Options.GetAppOptionString ("panel", "auto");
	if (strcmp (pPanel, "auto") != 0)
	{
		m_bPanelPresent = strcmp (pPanel, "none") != 0;
		LOGNOTE ("Panel: %s (panel=%s)", m_bPanelPresent ? "there" : "none", pPanel);
		return TRUE;
	}

	CPanelOutput::TPanelInfo Info;
	m_bPanelPresent = CPanelOutput::Detect (&Info);
	if (m_bPanelPresent)
	{
		LOGNOTE ("Panel: ST7789, module ID %02X %02X %02X; status %08X, power mode %02X, "
			 "MADCTL %02X, pixel format %02X, self-diagnostic %02X", Info.nID >> 16,
			 (Info.nID >> 8) & 0xFF, Info.nID & 0xFF, Info.nStatus, Info.nPowerMode,
			 Info.nMADCTL, Info.nPixelFormat, Info.nSelfDiagnostic);
	}
	else
	{
		LOGNOTE ("Panel: none answers on MISO (GPIO9)");
	}

	return TRUE;
}

// where frames go now, at which size
void CKernel::ChooseOutput (COutput **ppOutput, unsigned *pWidth, unsigned *pHeight)
{
	if (   m_OutputMode == OutputPanel
	    || (   m_OutputMode == OutputAuto && m_bPanelPresent
		&& !m_Monitor.GetState ().bConnected))
	{
		*ppOutput = &m_Panel;
		*pWidth = 320;
		*pHeight = 240;
		return;
	}

	*ppOutput = &m_HDMI;
	HDMISize (pWidth, pHeight);
}

// the screen on HDMI: the monitor's preferred mode (or else the mode sent),
// or, if that's more than hdmi_pixels (default: as large as the renderer
// goes, 1920x1200), divided by the smallest whole number that brings it down;
// the firmware scales it to the HDMI mode. The width is a multiple of 16 (the
// framebuffer's pitch). Without a monitor the screen keeps its size.
void CKernel::HDMISize (unsigned *pWidth, unsigned *pHeight)
{
	const THDMIState &M = m_Monitor.GetState ();
	unsigned nWidth = M.bEDID ? M.nWidth : M.nSignalWidth;
	unsigned nHeight = M.bEDID ? M.nHeight : M.nSignalHeight;
	if (!M.bConnected || nWidth < 16 || nHeight < 16)
	{
		*pWidth = m_HDMI.GetWidth ();
		*pHeight = m_HDMI.GetHeight ();
		return;
	}

	unsigned nDivisor = 1;
	while (   (nWidth / nDivisor) * (nHeight / nDivisor) > m_nHDMIPixels
	       || nWidth / nDivisor > CRenderer::MaxWidth
	       || nHeight / nDivisor > CRenderer::MaxHeight)
	{
		nDivisor++;
	}
	*pWidth = (nWidth / nDivisor) & ~15;
	*pHeight = (nHeight / nDivisor) & ~1;
}

// between frames: the output and screen size for the monitor's state now,
// and the GL frames on it, or off screen while a PC's desktop shows there
void CKernel::ApplyOutput (void)
{
	m_bOutputPending = FALSE;

	COutput *pScreen;
	unsigned nWidth, nHeight;
	ChooseOutput (&pScreen, &nWidth, &nHeight);
	m_GUD.SetScreen (nWidth, nHeight);
	boolean bDesktop = m_GUD.IsActive ();
	COutput *pTarget = bDesktop ? &m_Offscreen : pScreen;
	boolean bScreen =    pScreen != m_pScreen
			  || nWidth != m_Renderer.GetWidth ()
			  || nHeight != m_Renderer.GetHeight ();
	if (!bScreen && pTarget == m_Renderer.GetOutput ())
	{
		if (pScreen == &m_HDMI)
		{
			ShowPanelNotice ();		// (the monitor's EDID came)
			if (!bDesktop && HostIdle ())
			{
				ShowSplash (pScreen);	// with the monitor's name now
			}
		}
		SendDisplay ();
		return;
	}

	boolean bWasDesktop = m_Renderer.GetOutput () == &m_Offscreen;
	if (bDesktop)
	{
		m_Renderer.GetOutput ()->WaitIdle ();
		if (!pScreen->SetSize (nWidth, nHeight))
		{
			LOGERR ("Can't show %ux%u on %s", nWidth, nHeight, pScreen == &m_HDMI ? "HDMI" : "the panel");
			return;
		}
	}
	if (!m_Renderer.SetOutput (pTarget, nWidth, nHeight))
	{
		LOGERR ("Can't show %ux%u on %s", nWidth, nHeight, pScreen == &m_HDMI ? "HDMI" : "the panel");
		return;
	}
	m_pScreen = pScreen;
	m_Commands.ScreenChanged ();
	LOGNOTE ("Screen: %ux%u on %s%s", nWidth, nHeight, pScreen == &m_HDMI ? "HDMI" : "the panel",
		 bDesktop ? ", the PC's desktop" : "");
	if (bDesktop)
	{
		m_GUD.Start (pScreen);
	}
	else if (bWasDesktop || HostIdle ())
	{
		ShowSplash (pScreen);			// (the desktop's gone, or no host draws: until the next frame)
	}

	// the panel says where the screen is (again when the monitor's EDID comes)
	if (pScreen == &m_HDMI)
	{
		ShowPanelNotice ();
	}

	SendDisplay ();
}

// STREAM_END (docs/protocol.md 13): the host's session is over. Everything is
// reset (as RESET: the streams closed, the objects gone), the link is idle
// again (the serial port: text, the log and the installer), and the splash
// shows until the next host draws.
void CKernel::EndSession (CLink *pLink)
{
	LOGNOTE ("The host ended its session");
	m_Commands.Reset ();
	pLink->EndSession ();
	m_bFrameSeen = FALSE;
	if (!m_GUD.IsActive ())
	{
		ShowSplash (m_pScreen);
	}
}

// no host draws: none has ended a frame, or not for a second
boolean CKernel::HostIdle (void) const
{
	return !m_bFrameSeen || CTimer::GetClockTicks () - m_nLastFrame > QUIET_HOST_US;
}

// with the screen on HDMI: the panel says so, and what goes there
void CKernel::ShowPanelNotice (void)
{
	if (!m_bPanelPresent)
	{
		return;
	}

	// the monitor (its EDID: name, preferred mode), the HDMI signal the
	// firmware sends (the pixel valve's size, the rate measured; config.txt
	// sets it at boot) and the screen GL renders into, which the firmware
	// scales to the signal
	const THDMIState &M = m_Monitor.GetState ();
	CString Model, Native, Signal, Render;
	if (!M.bConnected)
	{
		Model = "none";
		Native = "-";
		Signal = "-";
	}
	else
	{
		if (!M.bEDID)
		{
			Model = "(reading EDID)";
			Native = "-";
		}
		else
		{
			Model = M.Name[0] ? M.Name : "(no name)";
			Native.Format ("%ux%u@%uHz", M.nWidth, M.nHeight, (M.nRefreshMilliHz + 500) / 1000);
		}
		unsigned nRefresh = m_HDMI.MeasureRefresh ();
		Signal.Format ("%ux%u@%uHz", M.nSignalWidth, M.nSignalHeight, (nRefresh + 500) / 1000);
	}
	Render.Format ("%ux%u", m_HDMI.GetWidth (), m_HDMI.GetHeight ());

	const char *Label[] = {"Monitor:", "Native resolution:", "HDMI resolution:", "Render resolution:"};
	const char *Value[] = {Model, Native, Signal, Render};
	CString Lines[4];
	const char *pLines[4];
	for (unsigned i = 0; i < 4; i++)
	{
		Lines[i].Format ("%-19s%s", Label[i], Value[i]);
		pLines[i] = Lines[i];
	}
	ShowLines (&m_Panel, pLines, 4);
	LOGNOTE ("Panel: monitor %s, native %s, HDMI %s, render %s", (const char *) Model,
		 (const char *) Native, (const char *) Signal, (const char *) Render);
}

// the DISPLAY reply (docs/protocol.md 9)
void CKernel::SendDisplay (boolean bSend)
{
	const THDMIState &M = m_Monitor.GetState ();
	u32 Display[PGPU_DISPLAY_WORDS];
	memset (Display, 0, sizeof Display);
	Display[0] =   (m_pScreen == &m_HDMI ? PGPU_OUTPUT_HDMI : PGPU_OUTPUT_PANEL)
		     | (M.bConnected ? PGPU_DISPLAY_HDMI_CONNECTED : 0)
		     | (m_bPanelPresent ? PGPU_DISPLAY_PANEL_PRESENT : 0)
		     | (M.bEDID ? PGPU_DISPLAY_EDID : 0);
	Display[1] = m_Renderer.GetWidth () | m_Renderer.GetHeight () << 16;
	Display[2] = M.nWidth | M.nHeight << 16;
	Display[3] = M.nRefreshMilliHz;
	Display[4] = M.nSignalWidth | M.nSignalHeight << 16;
	memcpy (&Display[5], M.Name, sizeof M.Name);		// 13 characters, zero padded
	m_Commands.SetDisplay (Display, PGPU_DISPLAY_WORDS, bSend);
}

TShutdownMode CKernel::Run (void)
{
	LOGNOTE ("Compile time: " __DATE__ " " __TIME__);
	LOGNOTE ("Build: %s", GetBuildInfo ());
	// supported for now: the Zero / Zero W and the Zero 2 W (other RPi boards
	// may run this build, but nothing is verified on them: the HDMI hot-plug
	// line, for one, is a guess there)
	TMachineModel Model = CMachineInfo::Get ()->GetMachineModel ();
	if (   Model != MachineModelZero && Model != MachineModelZeroW
	    && Model != MachineModelZero2W)
	{
		LOGWARN ("%s isn't supported yet (for now: the Zero / Zero W and the Zero 2 W)",
			 CMachineInfo::Get ()->GetMachineName ());
	}
	LOGNOTE ("Core clock %u MHz", CMachineInfo::Get ()->GetClockRate (CLOCK_ID_CORE) / 1000000);
	m_nARMClock = m_CPUThrottle.GetClockRate ();
	LOGNOTE ("ARM clock %u MHz (%u-%u MHz), V3D %u MHz; SoC %u C (the firmware's limit: %u C)",
		 m_nARMClock / 1000000, m_CPUThrottle.GetMinClockRate () / 1000000,
		 m_CPUThrottle.GetMaxClockRate () / 1000000,
		 CMachineInfo::Get ()->GetClockRate (5) / 1000000,	// (5: the V3D's clock id)
		 m_CPUThrottle.GetTemperature (), m_CPUThrottle.GetMaxTemperature ());
	// the firmware's throttle flags (under-voltage and so on, since boot)
	LOGNOTE ("Throttled %05X", GetThrottled ());
	m_RunLog.Report ();		// how the previous run ended
	LOGNOTE ("Host: %s", m_HostMode == HostUSB ? "USB only (host=usb)"
			     : m_HostMode == HostI2S ? "I2S only (host=i2s)" : "USB when a PC streams, else I2S (host=auto)");
	LOGNOTE ("USB: %s", m_bGUD ? "serial port, GL interface and monitor (GUD)" : "serial port and GL interface, no monitor (gud=off)");

	m_Monitor.Initialize ();
	COutput *pOutput;
	unsigned nWidth, nHeight;
	ChooseOutput (&pOutput, &nWidth, &nHeight);
	if (!pOutput->SetSize (nWidth, nHeight))
	{
		LOGPANIC ("No %ux%u screen", nWidth, nHeight);
	}
	LOGNOTE ("Screen: %ux%u on %s", nWidth, nHeight, pOutput == &m_HDMI ? "HDMI" : "the panel");
	m_pScreen = pOutput;
	m_GUD.SetScreen (nWidth, nHeight);

	if (   !m_V3D.Initialize ()
	    || !m_Renderer.Initialize (pOutput))
	{
		LOGPANIC ("V3D init failed");
	}
	// after the renderer's start, which clears its buffers (on HDMI the
	// framebuffer's pages)
	ShowSplash (pOutput);
	if (pOutput == &m_HDMI)
	{
		ShowPanelNotice ();
	}
	m_Commands.Reset ();
	m_Commands.InitializeVideo ();

	for (unsigned i = 0; i < Links; i++)
	{
		if (!m_pLinks[i]->Initialize ())
		{
			LOGPANIC ("Link %u init failed", i);
		}
	}
	LOGNOTE ("I2S slave: CLK pin 12, FS pin 35, DIN pin 38, DOUT pin 40; READY pin 36, FRAME pin 37");

	SendDisplay (FALSE);
	m_Commands.SendInfo ();			// once after boot, with DISPLAY (docs/protocol.md 9)

	CV3D::SetWaitHandler (V3DWait);
	if (m_bJobCheck)
	{
		CV3D::SetJobCheck (V3DCheckJob);	// each job's control lists checked before it runs
	}

	unsigned nLastReport = m_Timer.GetUptime ();
	unsigned nWindowStart = CTimer::GetClockTicks ();	// microseconds
	unsigned nBusyUs = 0;			// receiving and executing packets
	unsigned nLastPacket = CTimer::GetClockTicks ();
	while (1)
	{
		m_Scheduler.Yield ();			// VCHIQ's tasks
		m_DevLink.Update ();
		HostInput ();

		// commands from the first active link that host= allows (a PC over
		// USB once it has sent on the GL interface or switched the serial
		// port to its binary stream, else the Pico or the P4 over I2S); the
		// others' input is discarded
		CLink *pLink = nullptr;
		for (unsigned k = 0; k < Links; k++)
		{
			m_pLinks[k]->Update ();
			boolean bAllowed =    m_HostMode == HostAuto
					   || (m_HostMode == HostUSB && m_pLinks[k] != &m_I2SLink)
					   || (m_HostMode == HostI2S && m_pLinks[k] == &m_I2SLink);
			if (!pLink && bAllowed && m_pLinks[k]->IsActive ())
			{
				pLink = m_pLinks[k];
			}
			else
			{
				u32 nHeader;
				while (m_pLinks[k]->GetPacket (&nHeader) != nullptr)
				{
				}
			}
		}
		m_Commands.SetLink (pLink);		// (none: host=usb before a PC's session)

		u32 nHeader;
		const u32 *pPayload;
		unsigned nLoopStart = CTimer::GetClockTicks (), i;
		for (i = 0;
		        pLink
		     && i < MAX_PACKETS_PER_LOOP
		     && CTimer::GetClockTicks () - nLoopStart < MAX_US_PER_LOOP
		     && (pPayload = pLink->GetPacket (&nHeader)) != nullptr;
		     i++)
		{
			if (PGPU_HEADER_OP (nHeader) == PGPU_OP_DEBUG_SCREENSHOT)
			{
				DumpScreenshot ();
			}
			else if (PGPU_HEADER_OP (nHeader) == PGPU_OP_STREAM_END)
			{
				EndSession (pLink);
				break;
			}
			else
			{
				m_Commands.Execute (nHeader, pPayload);
				if (PGPU_HEADER_OP (nHeader) == PGPU_OP_FRAME_END)
				{
					m_bFrameSeen = TRUE;
					m_nLastFrame = CTimer::GetClockTicks ();
					if (m_bOutputPending)
					{
						ApplyOutput ();
					}
				}
			}
		}
		if (i)
		{
			nBusyUs += CTimer::GetClockTicks () - nLoopStart;
			nLastPacket = CTimer::GetClockTicks ();
		}

		m_Commands.UpdateVideo ();

		// the monitor plugged in or out: a new screen from the next frame
		if (m_Monitor.Update ())
		{
			m_bOutputPending = TRUE;
		}
		// a PC's desktop on or off
		if (m_GUD.Update (m_pScreen))
		{
			m_bOutputPending = TRUE;
		}
		// a screen change waits for the frame's end, but not for a host
		// that went quiet in its middle (killed, say): that frame is dropped
		if (   m_bOutputPending && !m_Commands.IsBetweenFrames ()
		    && CTimer::GetClockTicks () - nLastPacket > QUIET_HOST_US)
		{
			LOGNOTE ("The host went quiet in the middle of a frame: dropped for the new screen");
			m_Commands.AbandonFrame ();
		}
		if (m_bOutputPending && m_Commands.IsBetweenFrames ())
		{
			ApplyOutput ();
		}

		unsigned nNow = m_Timer.GetUptime ();
		if (nNow != nLastReport)
		{
			TI2SLinkStats R = m_I2SLink.GetStats ();
			TCommandStats C = m_Commands.GetStats ();
			unsigned nFrames = C.nFrames ? C.nFrames : 1;

			// the CPU's own work: the waits for the V3D and the panel happen
			// while executing commands (both poll)
			unsigned nTicks = CTimer::GetClockTicks ();
			unsigned nWaitUs = C.nRenderUs + C.nPresentWaitUs;
			TLoadStats Load = {nTicks - nWindowStart, C.nFrames, C.nRenderUs,
					   nBusyUs > nWaitUs ? nBusyUs - nWaitUs : 0, C.nPresentWaitUs};
			m_Commands.SetLoadStats (Load);
			nWindowStart = nTicks;
			nBusyUs = 0;

			LOGNOTE ("%u fps, %u draws %u tris/frame, prims %u clipped %u rejected %u dropped %u, "
				 "render %u us, panel wait %u us",
				 C.nFrames, C.nDraws / nFrames, C.nTriangles / nFrames,
				 C.nPrimitives / nFrames, C.nClipped / nFrames, C.nRejected / nFrames,
				 C.nDropped, C.nRenderUs / nFrames, C.nPresentWaitUs / nFrames);
			LOGNOTE ("link: %u packets, %u KB, CRC err %u, garbage %u, max fill %u KB, READY low %u | "
				 "cmd errors %u (last %03X) | replies %u dropped %u late %u | "
				 "heap %u KB free, textures %u KB",
				 R.nPackets, (R.nWords - R.nIdleWords) / 256, R.nCRCErrors,
				 R.nGarbageWords, R.nMaxFill / 256, R.nReadyLow,
				 C.nErrors, C.nLastError, R.nReplies, R.nRepliesDropped, R.nTxLate,
				 (unsigned) (CMemorySystem::Get ()->GetHeapFreeSpace (HEAP_LOW) / 1024),
				 m_Commands.GetTextureBytes () / 1024);

			// the ARM's clock, when the firmware changed it
			unsigned nARMClock = m_CPUThrottle.GetClockRate ();
			if (nARMClock && nARMClock != m_nARMClock)
			{
				LOGNOTE ("ARM clock now %u MHz (SoC %u C)", nARMClock / 1000000,
					 m_CPUThrottle.GetTemperature ());
				m_nARMClock = nARMClock;
			}

			// under-voltage, capping, throttling, temperature limit: now
			// (bits 0-3) or since boot (16-19)
			u32 nThrottled = GetThrottled ();
			if (nThrottled)
			{
				LOGWARN ("Throttled %05X:%s%s%s%s", nThrottled,
					 nThrottled & 0x10001 ? " under-voltage" : "",
					 nThrottled & 0x20002 ? " frequency capped" : "",
					 nThrottled & 0x40004 ? " throttled" : "",
					 nThrottled & 0x80008 ? " soft temperature limit" : "");
			}

			nLastReport = nNow;
		}
	}

	return ShutdownHalt;
}

// the firmware's throttle flags (0 if it doesn't say)
u32 CKernel::GetThrottled (void)
{
	CBcmPropertyTags Tags;
	TPropertyTagSimple Throttled;
	Throttled.nValue = 0;
	if (!Tags.GetTag (PROPTAG_GET_THROTTLED, &Throttled, sizeof Throttled, 4))
	{
		return 0;
	}

	return Throttled.nValue;
}

// the frame on screen (the output's copy - HDMI's page - or else the last
// presented one) as base64 lines between markers, for devtools/screenshot.py
void CKernel::DumpScreenshot (void)
{
	static const char Base64[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

	const u8 *p = (const u8 *) m_Renderer.GetOutput ()->GetShownFrame ();
	if (p == nullptr)
	{
		p = (const u8 *) m_Renderer.GetLastFrame ();
	}
	unsigned nBytes = m_Renderer.GetWidth () * m_Renderer.GetHeight () * 2;
	CleanAndInvalidateDataCacheRange ((uintptr) p, nBytes);	// written by the V3D

	CString Header;
	Header.Format ("\n#SCREENSHOT %u %u rgb565le\n", m_Renderer.GetWidth (), m_Renderer.GetHeight ());
	m_DevLink.Write ((const char *) Header, Header.GetLength ());

	char Line[100];
	for (unsigned i = 0; i < nBytes; )
	{
		unsigned n = 0;
		for (unsigned k = 0; k < 24 && i < nBytes; k++, i += 3)	// 72 bytes per line
		{
			u32 v = p[i] << 16 | (i + 1 < nBytes ? p[i + 1] : 0) << 8 | (i + 2 < nBytes ? p[i + 2] : 0);
			Line[n++] = Base64[(v >> 18) & 63];
			Line[n++] = Base64[(v >> 12) & 63];
			Line[n++] = i + 1 < nBytes ? Base64[(v >> 6) & 63] : '=';
			Line[n++] = i + 2 < nBytes ? Base64[v & 63] : '=';
		}
		Line[n++] = '\n';
		if (!m_DevLink.Write (Line, n))
		{
			LOGWARN ("Screenshot aborted");
			return;
		}
	}

	m_DevLink.Write ("#END\n", 5);
	m_DevLink.Update ();
}

// the idle screen: what piegpu is, where its screen is and what it waits
// for. Laid out for 320x240 and scaled by a whole factor (the panel: 1,
// 1024x600: 2, 1920x1080: 4); it stays until the host's first frame
void CKernel::ShowSplash (COutput *pOutput)
{
	const THDMIState &M = m_Monitor.GetState ();
	unsigned nWidth = pOutput->GetWidth (), nHeight = pOutput->GetHeight ();

	CString Status, Monitor, Render, Board, Clocks, Build;
	if (pOutput == &m_HDMI)
	{
		if (M.bConnected)
		{
			unsigned nRefresh = m_HDMI.MeasureRefresh ();
			Status.Format ("HDMI %ux%u@%uHz ready", M.nSignalWidth, M.nSignalHeight,
				       (nRefresh + 500) / 1000);
		}
		else
		{
			Status = "HDMI ready, no monitor";
		}
	}
	else
	{
		Status.Format ("Panel %ux%u ready", nWidth, nHeight);
	}
	if (pOutput != &m_HDMI)
	{
		Monitor = "ST7789";			// the splash is on the panel
	}
	else if (!M.bConnected)
	{
		Monitor = "none";
	}
	else if (!M.bEDID)
	{
		Monitor = "(reading EDID)";
	}
	else
	{
		Monitor.Format ("%s %ux%u@%uHz", M.Name[0] ? M.Name : "(no name)", M.nWidth, M.nHeight,
				(M.nRefreshMilliHz + 500) / 1000);
	}
	Render.Format ("%ux%u", nWidth, nHeight);
	Board = CMachineInfo::Get ()->GetMachineName ();
	Clocks.Format ("ARM %u MHz, V3D %u MHz", m_nARMClock / 1000000,
		       CMachineInfo::Get ()->GetClockRate (5) / 1000000);	// (5: the V3D's clock id)
	const char *pHost = m_HostMode == HostUSB ? "USB (host=usb)"
			  : m_HostMode == HostI2S ? "I2S (host=i2s)" : "I2S or USB (host=auto)";
	Build = GetBuildVersion ();

	const char *Label[] = {"Monitor:", "Render:", "Board:", "Clocks:", "Host:", "Build:"};
	const char *Value[] = {Monitor, Render, Board, Clocks, pHost, Build};
	const unsigned Lines = sizeof Label / sizeof Label[0];

	C2DGraphics Graphics (pOutput->GetDisplay ());
	if (!Graphics.Initialize ())
	{
		return;
	}
	unsigned k = nWidth / 320 < nHeight / 240 ? nWidth / 320 : nHeight / 240;
	k = k ? k : 1;
	unsigned x0 = (nWidth - 320 * k) / 2, y0 = (nHeight - 240 * k) / 2;
	Graphics.ClearScreen (COLOR2D (0, 0, 64));

	// in 320x240 units: the title, the status, the block of labelled lines
	// (left-aligned, centred as a block), what it waits for
	unsigned y = 18;
	DrawScaledText (Graphics, x0, y0, k, 160, y, TRUE, COLOR2D (255, 255, 255), "piegpu", Font12x22);
	y += 30;
	DrawScaledText (Graphics, x0, y0, k, 160, y, TRUE, COLOR2D (120, 255, 120), Status, Font8x16);
	y += 28;
	CString Line[Lines];
	unsigned nChars = 0;
	for (unsigned i = 0; i < Lines; i++)
	{
		Line[i].Format ("%-9s%s", Label[i], Value[i]);
		nChars = Line[i].GetLength () > nChars ? Line[i].GetLength () : nChars;
	}
	unsigned xBlock = nChars * 8 < 320 ? (320 - nChars * 8) / 2 : 0;
	for (unsigned i = 0; i < Lines; i++, y += 18)
	{
		DrawScaledText (Graphics, x0, y0, k, xBlock, y, FALSE, COLOR2D (255, 255, 255), Line[i], Font8x16);
	}
	y += 10;
	DrawScaledText (Graphics, x0, y0, k, 160, y, TRUE, COLOR2D (160, 160, 160),
			"waiting for OpenGL ES or video", Font8x16);
	Graphics.UpdateDisplay ();
	pOutput->WaitIdle ();
	LOGNOTE ("Splash on %s: %s", pOutput == &m_HDMI ? "HDMI" : "the panel", (const char *) Status);
}

// text at (x, y) of a 320x240 layout (centred on x, or from it), every font
// pixel drawn as a k x k square at (x0, y0) + k * (x, y)
void CKernel::DrawScaledText (C2DGraphics &Graphics, unsigned x0, unsigned y0, unsigned k,
			      unsigned x, unsigned y, boolean bCentre, T2DColor Color,
			      const char *pText, const TFont &rFont)
{
	CCharGenerator Font (rFont);
	unsigned nChars = strlen (pText);
	if (bCentre)
	{
		unsigned nWidth = nChars * Font.GetCharWidth ();
		x = nWidth < 2 * x ? x - nWidth / 2 : 0;
	}
	for (unsigned i = 0; i < nChars; i++)
	{
		for (unsigned fy = 0; fy < Font.GetUnderline (); fy++)
		{
			CCharGenerator::TPixelLine Line = Font.GetPixelLine (pText[i], fy);
			for (unsigned fx = 0; fx < Font.GetCharWidth (); fx++)
			{
				if (Font.GetPixel (fx, Line))
				{
					unsigned px = x0 + k * (x + i * Font.GetCharWidth () + fx);
					unsigned py = y0 + k * (y + fy);
					if (px + k <= Graphics.GetWidth () && py + k <= Graphics.GetHeight ())
					{
						Graphics.DrawRect (px, py, k, k, Color);
					}
				}
			}
		}
	}
}

// lines in one font and colour, as a left-aligned block in the middle of an
// output (not while it shows frames)
void CKernel::ShowLines (COutput *pOutput, const char *const *ppLines, unsigned nLines)
{
	C2DGraphics Graphics (pOutput->GetDisplay ());
	if (!Graphics.Initialize ())
	{
		return;
	}

	const unsigned CharWidth = 8, LineHeight = 20;		// Font8x16, spaced
	unsigned nChars = 0;
	for (unsigned i = 0; i < nLines; i++)
	{
		unsigned n = strlen (ppLines[i]);
		nChars = n > nChars ? n : nChars;
	}
	unsigned nBlock = nChars * CharWidth;
	unsigned x = nBlock < Graphics.GetWidth () ? (Graphics.GetWidth () - nBlock) / 2 : 0;
	unsigned y = (Graphics.GetHeight () - nLines * LineHeight) / 2;
	Graphics.ClearScreen (COLOR2D (0, 0, 64));
	for (unsigned i = 0; i < nLines; i++)
	{
		Graphics.DrawText (x, y + i * LineHeight, COLOR2D (255, 255, 255), ppLines[i]);
	}
	Graphics.UpdateDisplay ();
	pOutput->WaitIdle ();
}

// a title and two lines in the middle of an output (not while it shows frames)
void CKernel::ShowText (COutput *pOutput, const char *pTitle, const char *pLine1, const char *pLine2)
{
	C2DGraphics Graphics (pOutput->GetDisplay ());
	if (!Graphics.Initialize ())
	{
		return;
	}

	unsigned nWidth = Graphics.GetWidth ();
	unsigned y = Graphics.GetHeight () / 2 - (*pLine1 || *pLine2 ? 30 : 11);
	Graphics.ClearScreen (COLOR2D (0, 0, 64));
	Graphics.DrawText (nWidth / 2, y, COLOR2D (255, 255, 255), pTitle,
			   C2DGraphics::AlignCenter, Font12x22);
	Graphics.DrawText (nWidth / 2, y + 40, COLOR2D (255, 255, 0), pLine1, C2DGraphics::AlignCenter);
	Graphics.DrawText (nWidth / 2, y + 60, COLOR2D (160, 160, 160), pLine2, C2DGraphics::AlignCenter);
	Graphics.UpdateDisplay ();
	pOutput->WaitIdle ();
}
