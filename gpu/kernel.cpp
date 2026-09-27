//
// gpu: the Pi Zero side of the Pico GPU (docs/protocol.md).
// Receives the command stream from the Pico over I2S and renders with the V3D
// on the ST7789 panel.
//
#include "kernel.h"
#include <circle/2dgraphics.h>
#include <circle/machineinfo.h>
#include <circle/string.h>
#include <circle/synchronize.h>
#include <pgpu_protocol.h>
#include <circle/memory.h>

// ST7789 panel on SPI0 (CE0), 75 MHz = 300 MHz core / 4 (config.txt core_freq=300)
#define SPI_CLOCK_SPEED		75000000
#define DC_PIN			24
#define RESET_PIN		25
#define DISPLAY_WIDTH		320
#define DISPLAY_HEIGHT		240

#define MAX_PACKETS_PER_LOOP	64

LOGMODULE ("gpu");

CKernel::CKernel (void)
:	m_Timer (&m_Interrupt),
	m_Logger (m_Options.GetLogLevel (), &m_Timer),
	m_DevLink (&m_Interrupt),
	m_Display (&m_Interrupt, DC_PIN, RESET_PIN, DISPLAY_WIDTH, DISPLAY_HEIGHT,
		   SPI_CLOCK_SPEED, 0, TRUE),		// little endian RGB565 from the V3D
	m_HostLink (&m_DevLink),
	m_Renderer (&m_V3D, &m_Display),
	m_Commands (&m_Renderer, &m_Receiver)
{
}

CKernel::~CKernel (void)
{
}

boolean CKernel::Initialize (void)
{
	return    m_Logger.Initialize (&m_Null)
	       && m_Interrupt.Initialize ()
	       && m_Timer.Initialize ()
	       && m_DevLink.Initialize ()
	       && m_Display.Initialize ();
}

TShutdownMode CKernel::Run (void)
{
	LOGNOTE ("Compile time: " __DATE__ " " __TIME__);
	LOGNOTE ("Core clock %u MHz", CMachineInfo::Get ()->GetClockRate (CLOCK_ID_CORE) / 1000000);

	ShowSplash ();

	if (   !m_V3D.Initialize ()
	    || !m_Renderer.Initialize ())
	{
		LOGPANIC ("V3D init failed");
	}
	m_Commands.Reset ();

	if (   !m_Receiver.Initialize ()
	    || !m_HostLink.Initialize ())
	{
		LOGPANIC ("Receiver init failed");
	}
	LOGNOTE ("I2S slave: CLK pin 12, FS pin 35, DIN pin 38, DOUT pin 40; READY pin 36, FRAME pin 37");

	m_Commands.SendInfo ();			// once after boot (docs/protocol.md 9)

	unsigned nLastReport = m_Timer.GetUptime ();
	unsigned nWindowStart = CTimer::GetClockTicks ();	// microseconds
	unsigned nBusyUs = 0;			// receiving and executing packets
	u32 nCredited = 0;			// stream bytes reported to the host
	while (1)
	{
		m_DevLink.Update ();
		if (m_DevLink.GetChar () == 's')		// from the host: screenshot
		{
			DumpScreenshot ();
		}

		// commands from the Pico (I2S), or from a PC over USB once it has
		// switched the USB link to its binary stream (then the Pico is ignored)
		boolean bHost = m_DevLink.IsStreaming ();
		if (bHost)
		{
			m_Commands.SetHostLink (&m_HostLink);

			// flow control: the host may send what the gadget's queue holds
			// beyond what we have taken
			u32 nReceived = m_DevLink.GetStreamReceived ();
			if (nReceived != nCredited)
			{
				m_HostLink.SendReply (PGPU_REPLY_CREDIT, &nReceived, 1);
				nCredited = nReceived;
			}

			u32 nHeader;
			while (m_Receiver.GetPacket (&nHeader) != nullptr)
			{
			}
		}

		u32 nHeader;
		const u32 *pPayload;
		unsigned nLoopStart = CTimer::GetClockTicks (), i;
		for (i = 0;
		     i < MAX_PACKETS_PER_LOOP
		     && (pPayload = bHost ? m_HostLink.GetPacket (&nHeader)
					  : m_Receiver.GetPacket (&nHeader)) != nullptr;
		     i++)
		{
			if (PGPU_HEADER_OP (nHeader) == PGPU_OP_DEBUG_SCREENSHOT)
			{
				DumpScreenshot ();
			}
			else
			{
				m_Commands.Execute (nHeader, pPayload);
			}
		}
		if (i)
		{
			nBusyUs += CTimer::GetClockTicks () - nLoopStart;
		}

		unsigned nNow = m_Timer.GetUptime ();
		if (nNow != nLastReport)
		{
			TReceiverStats R = m_Receiver.GetStats ();
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

// the last presented frame as base64 lines between markers, for
// devtools/screenshot.py
void CKernel::DumpScreenshot (void)
{
	static const char Base64[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

	const u8 *p = (const u8 *) m_Renderer.GetLastFrame ();
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

void CKernel::ShowSplash (void)
{
	C2DGraphics Graphics (&m_Display);
	if (!Graphics.Initialize ())
	{
		return;
	}

	unsigned nWidth = Graphics.GetWidth ();
	Graphics.ClearScreen (COLOR2D (0, 0, 64));
	Graphics.DrawText (nWidth / 2, 90, COLOR2D (255, 255, 255), "pico-gpu",
			   C2DGraphics::AlignCenter, Font12x22);
	Graphics.DrawText (nWidth / 2, 130, COLOR2D (255, 255, 0), "waiting for the Pico",
			   C2DGraphics::AlignCenter);
	Graphics.DrawText (nWidth / 2, 150, COLOR2D (160, 160, 160), "build " __TIME__,
			   C2DGraphics::AlignCenter);
	Graphics.UpdateDisplay ();
	m_Display.WaitIdle ();
}
