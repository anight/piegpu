//
// gpu: the Pi Zero side of the Pico GPU (docs/protocol.md).
// Receives the command stream from the host over a link (link/: the Pico's
// I2S, or a PC over USB) and renders with the V3D to an output (display/: the
// ST7789 panel).
//
#include "kernel.h"
#include <circle/2dgraphics.h>
#include <circle/machineinfo.h>
#include <circle/string.h>
#include <circle/synchronize.h>
#include <pgpu_protocol.h>
#include <circle/memory.h>

#define MAX_PACKETS_PER_LOOP	64

LOGMODULE ("gpu");

CKernel::CKernel (void)
:	m_Timer (&m_Interrupt),
	m_Logger (m_Options.GetLogLevel (), &m_Timer),
	m_DevLink (&m_Interrupt),
	m_Output (&m_Interrupt),
	m_USBLink (&m_DevLink),
	m_Renderer (&m_V3D, &m_Output),
	m_Commands (&m_Renderer, &m_I2SLink),
	m_pLinks {&m_USBLink, &m_I2SLink}
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
	       && m_Output.Initialize ();
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

	for (unsigned i = 0; i < Links; i++)
	{
		if (!m_pLinks[i]->Initialize ())
		{
			LOGPANIC ("Link %u init failed", i);
		}
	}
	LOGNOTE ("I2S slave: CLK pin 12, FS pin 35, DIN pin 38, DOUT pin 40; READY pin 36, FRAME pin 37");

	m_Commands.SendInfo ();			// once after boot (docs/protocol.md 9)

	unsigned nLastReport = m_Timer.GetUptime ();
	unsigned nWindowStart = CTimer::GetClockTicks ();	// microseconds
	unsigned nBusyUs = 0;			// receiving and executing packets
	while (1)
	{
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
		     i < MAX_PACKETS_PER_LOOP && (pPayload = pLink->GetPacket (&nHeader)) != nullptr;
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
	C2DGraphics Graphics (m_Output.GetDisplay ());
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
	m_Output.WaitIdle ();
}
