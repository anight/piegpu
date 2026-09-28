//
// gpu: the Pi Zero side of the Pico GPU (docs/protocol.md).
// Receives the command stream from the host over a link (link/: the Pico's
// I2S, or a PC over USB) and renders with the V3D to an output (display/: the
// ST7789 panel, or HDMI: by default HDMI while a monitor is connected, else
// the panel).
//
#include "kernel.h"
#include <circle/2dgraphics.h>
#include <circle/machineinfo.h>
#include <circle/string.h>
#include <circle/util.h>
#include <circle/synchronize.h>
#include <pgpu_protocol.h>
#include <circle/memory.h>

#define MAX_PACKETS_PER_LOOP	64
#define MAX_US_PER_LOOP		1000	// then VCHIQ's tasks (video) get their turn

LOGMODULE ("gpu");

CKernel::CKernel (void)
:	m_Timer (&m_Interrupt),
	m_Logger (m_Options.GetLogLevel (), &m_Timer),
	m_DevLink (&m_Interrupt),
	m_VCHIQ (CMemorySystem::Get (), &m_Interrupt),
	m_OutputMode (OutputAuto),
	m_bPanelPresent (TRUE),
	m_nHDMIPixels (CRenderer::MaxPixels),
	m_Panel (&m_Interrupt),
	m_bOutputPending (FALSE),
	m_USBLink (&m_DevLink),
	m_Renderer (&m_V3D),
	m_Commands (&m_Renderer, &m_I2SLink),
	m_pLinks {&m_USBLink, &m_I2SLink}
{
}

CKernel::~CKernel (void)
{
}

boolean CKernel::Initialize (void)
{
	const char *pOutput = m_Options.GetAppOptionString ("output", "auto");
	m_OutputMode =   strcmp (pOutput, "panel") == 0 ? OutputPanel
		       : strcmp (pOutput, "hdmi") == 0 ? OutputHDMI : OutputAuto;
	m_nHDMIPixels = m_Options.GetAppOptionDecimal ("hdmi_pixels", m_nHDMIPixels);
	if (m_nHDMIPixels < 320 * 240 || m_nHDMIPixels > CRenderer::MaxPixels)
	{
		m_nHDMIPixels = CRenderer::MaxPixels;
	}

	return    m_Logger.Initialize (&m_Null)
	       && m_Interrupt.Initialize ()
	       && m_Timer.Initialize ()
	       && m_DevLink.Initialize ()
	       && DetectPanel ()
	       && m_VCHIQ.Initialize ()
	       && (!(m_bPanelPresent || m_OutputMode == OutputPanel) || m_Panel.Initialize ())
	       && (m_OutputMode == OutputPanel || m_HDMI.Initialize ());
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

// between frames: the output and screen size for the monitor's state now
void CKernel::ApplyOutput (void)
{
	m_bOutputPending = FALSE;

	COutput *pOutput, *pOld = m_Renderer.GetOutput ();
	unsigned nWidth, nHeight;
	ChooseOutput (&pOutput, &nWidth, &nHeight);
	if (   pOutput != pOld
	    || nWidth != m_Renderer.GetWidth ()
	    || nHeight != m_Renderer.GetHeight ())
	{
		if (!m_Renderer.SetOutput (pOutput, nWidth, nHeight))
		{
			LOGERR ("Can't show %ux%u on %s", nWidth, nHeight, pOutput == &m_HDMI ? "HDMI" : "the panel");
		}
		else
		{
			m_Commands.ScreenChanged ();
			LOGNOTE ("Screen: %ux%u on %s", nWidth, nHeight, pOutput == &m_HDMI ? "HDMI" : "the panel");
		}
	}

	// the panel says where the screen is (again when the monitor's EDID comes)
	if (m_Renderer.GetOutput () == &m_HDMI)
	{
		ShowPanelNotice ();
	}

	SendDisplay ();
}

// with the screen on HDMI: the panel says so
void CKernel::ShowPanelNotice (void)
{
	if (!m_bPanelPresent)
	{
		return;
	}

	// the HDMI signal: its size from the pixel valve, its rate measured
	const THDMIState &M = m_Monitor.GetState ();
	CString Line;
	if (!M.bConnected)
	{
		Line = "HDMI no monitor";
	}
	else
	{
		unsigned nRefresh = m_HDMI.MeasureRefresh ();
		Line.Format ("HDMI %ux%u@%uHz", M.nSignalWidth, M.nSignalHeight, (nRefresh + 500) / 1000);
	}
	ShowText (&m_Panel, Line, "", "");
	LOGNOTE ("Panel: %s", (const char *) Line);
}

// the DISPLAY reply (docs/protocol.md 9)
void CKernel::SendDisplay (boolean bSend)
{
	const THDMIState &M = m_Monitor.GetState ();
	u32 Display[PGPU_DISPLAY_WORDS];
	memset (Display, 0, sizeof Display);
	Display[0] =   (m_Renderer.GetOutput () == &m_HDMI ? PGPU_OUTPUT_HDMI : PGPU_OUTPUT_PANEL)
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
	LOGNOTE ("Core clock %u MHz", CMachineInfo::Get ()->GetClockRate (CLOCK_ID_CORE) / 1000000);

	m_Monitor.Initialize ();
	COutput *pOutput;
	unsigned nWidth, nHeight;
	ChooseOutput (&pOutput, &nWidth, &nHeight);
	if (!pOutput->SetSize (nWidth, nHeight))
	{
		LOGPANIC ("No %ux%u screen", nWidth, nHeight);
	}
	LOGNOTE ("Screen: %ux%u on %s", nWidth, nHeight, pOutput == &m_HDMI ? "HDMI" : "the panel");
	ShowSplash (pOutput);
	if (pOutput == &m_HDMI)
	{
		ShowPanelNotice ();
	}

	if (   !m_V3D.Initialize ()
	    || !m_Renderer.Initialize (pOutput))
	{
		LOGPANIC ("V3D init failed");
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

	unsigned nLastReport = m_Timer.GetUptime ();
	unsigned nWindowStart = CTimer::GetClockTicks ();	// microseconds
	unsigned nBusyUs = 0;			// receiving and executing packets
	while (1)
	{
		m_Scheduler.Yield ();			// VCHIQ's tasks
		m_DevLink.Update ();
		if (m_DevLink.GetChar () == 's')		// from the host: screenshot
		{
			DumpScreenshot ();
		}

		// commands from the first active link (a PC over USB once it has
		// switched to its binary stream, else the Pico); the others' input
		// is discarded
		CLink *pLink = nullptr;
		for (unsigned k = 0; k < Links; k++)
		{
			m_pLinks[k]->Update ();
			if (!pLink && m_pLinks[k]->IsActive ())
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
		m_Commands.SetLink (pLink);

		u32 nHeader;
		const u32 *pPayload;
		unsigned nLoopStart = CTimer::GetClockTicks (), i;
		for (i = 0;
		        i < MAX_PACKETS_PER_LOOP
		     && CTimer::GetClockTicks () - nLoopStart < MAX_US_PER_LOOP
		     && (pPayload = pLink->GetPacket (&nHeader)) != nullptr;
		     i++)
		{
			if (PGPU_HEADER_OP (nHeader) == PGPU_OP_DEBUG_SCREENSHOT)
			{
				DumpScreenshot ();
			}
			else
			{
				m_Commands.Execute (nHeader, pPayload);
				if (m_bOutputPending && PGPU_HEADER_OP (nHeader) == PGPU_OP_FRAME_END)
				{
					ApplyOutput ();
				}
			}
		}
		if (i)
		{
			nBusyUs += CTimer::GetClockTicks () - nLoopStart;
		}

		m_Commands.UpdateVideo ();

		// the monitor plugged in or out: a new screen from the next frame
		if (m_Monitor.Update ())
		{
			m_bOutputPending = TRUE;
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

			nLastReport = nNow;
		}
	}

	return ShutdownHalt;
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

void CKernel::ShowSplash (COutput *pOutput)
{
	ShowText (pOutput, "pico-gpu", "waiting for the host", "build " __TIME__);
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
