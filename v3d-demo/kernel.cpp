#include "kernel.h"
#include <circle/string.h>
#include <circle/machineinfo.h>
#include <circle/util.h>
#include <v3dperf.h>

// ST7789 SPI panel (SoC GPIO numbers): CS = CE0 (GPIO8), SCLK GPIO11, MOSI GPIO10
#define SPI_CLOCK_SPEED		75000000	// Hz, 300 MHz core / 4 (config.txt core_freq=300)
#define SPI_CHIP_SELECT		0
#define DC_PIN			24
#define RESET_PIN		25
#define DISPLAY_WIDTH		320		// Circle's init sets MADCTL MV: landscape
#define DISPLAY_HEIGHT		240

LOGMODULE ("kernel");

static float Seconds (void)
{
	return CTimer::GetClockTicks64 () / 1000000.0f;
}

CKernel::CKernel (void)
:	m_Timer (&m_Interrupt),
	m_Logger (m_Options.GetLogLevel (), &m_Timer),
	m_DevLink (&m_Interrupt),
	m_Display (&m_Interrupt, DC_PIN, RESET_PIN, DISPLAY_WIDTH, DISPLAY_HEIGHT,
		   SPI_CLOCK_SPEED, SPI_CHIP_SELECT, TRUE),	// little endian RGB565 from the V3D
	m_Graphics (&m_Display),
	m_Scene (&m_V3D)
{
	m_ActLED.Blink (3);	// show we are alive
}

CKernel::~CKernel (void)
{
}

boolean CKernel::Initialize (void)
{
	boolean bOK = TRUE;

	if (bOK)
	{
		bOK = m_Serial.Initialize (115200);
	}

	if (bOK)
	{
		bOK = m_Logger.Initialize (&m_Serial);
	}

	if (bOK)
	{
		bOK = m_Interrupt.Initialize ();
	}

	if (bOK)
	{
		bOK = m_Timer.Initialize ();
	}

	if (bOK)
	{
		bOK = m_DevLink.Initialize ();
	}

	if (bOK)
	{
		bOK = m_Display.Initialize ();
	}

	if (bOK)
	{
		bOK = m_Graphics.Initialize ();
	}

	return bOK;
}

TShutdownMode CKernel::Run (void)
{
	LOGNOTE ("Compile time: " __DATE__ " " __TIME__);
	LOGNOTE ("Core clock %u MHz, SPI %u Hz requested",
		 CMachineInfo::Get ()->GetClockRate (CLOCK_ID_CORE) / 1000000, SPI_CLOCK_SPEED);

	// give the host time to connect, so that errors below are seen
	for (unsigned i = 0; i < 200; i++)
	{
		m_DevLink.Update ();
		CTimer::SimpleMsDelay (10);
	}

	if (   !m_V3D.Initialize ()
	    || !m_Scene.Initialize (1920, 1080))
	{
		LOGPANIC ("V3D init failed");
	}

	// How fast does the V3D render this scene, independent of the display?
	Benchmark (480, 320, 60);
	Benchmark (1280, 720, 30);
	Benchmark (1920, 1080, 30);

	// Live demo on the SPI panel: the V3D renders RGB565 straight into two
	// alternating panel buffers; DMA sends one while the other is rendered.
	const unsigned nWidth = m_Graphics.GetWidth ();
	const unsigned nHeight = m_Graphics.GetHeight ();
	m_Scene.SetResolution (nWidth, nHeight, TRUE);

	// the overlay is drawn by the ARM into the 2D graphics buffer, only its
	// two bars are copied into the V3D frame
	const u16 *pOverlay = (const u16 *) m_Graphics.GetBuffer ();
	const unsigned nBarHeight = 20;
	const unsigned nBarBytes = nBarHeight * nWidth * sizeof (u16);
	const unsigned nBottomBar = (nHeight - nBarHeight) * nWidth;
	const CDisplay::TArea FullArea = {0, nWidth-1, 0, nHeight-1};
	unsigned nBuffer = 0;

	unsigned nFrames = 0;
	unsigned nGPUUs = 0, nOverlayUs = 0, nWaitUs = 0;
	unsigned nLastReport = m_Timer.GetUptime ();
	CString Stats ("measuring...");

	while (1)
	{
		m_DevLink.Update ();

		unsigned nBinUs, nRenderUs;
		if (!m_Scene.Render (Seconds (), nBuffer, &nBinUs, &nRenderUs))
		{
			LOGPANIC ("Render failed");
		}
		nGPUUs += nBinUs + nRenderUs;

		unsigned nStart = CTimer::GetClockTicks ();
		m_Graphics.DrawRect (0, 0, nWidth, nBarHeight, COLOR2D (0, 0, 0));
		m_Graphics.DrawText (6, 2, COLOR2D (255, 255, 255),
				     "V3D RGB565 + DMA - Build " __TIME__);
		m_Graphics.DrawRect (0, nHeight-nBarHeight, nWidth, nBarHeight, COLOR2D (0, 0, 0));
		m_Graphics.DrawText (6, nHeight-18, COLOR2D (255, 255, 0), Stats);

		u16 *pFrame = (u16 *) m_Scene.GetFrameBuffer (nBuffer);
		memcpy (pFrame, pOverlay, nBarBytes);
		memcpy (pFrame + nBottomBar, pOverlay + nBottomBar, nBarBytes);
		nOverlayUs += CTimer::GetClockTicks () - nStart;

		// wait for the previous frame's DMA, then send this one in the background
		// (the DMA setup writes the overlay bars back from the ARM cache)
		nStart = CTimer::GetClockTicks ();
		m_Display.WaitIdle ();
		nWaitUs += CTimer::GetClockTicks () - nStart;

		m_Display.SetArea (FullArea, pFrame, PanelDone, this);
		nBuffer ^= 1;

		nFrames++;

		unsigned nNow = m_Timer.GetUptime ();
		if (nNow != nLastReport)
		{
			Stats.Format ("GPU %u.%u ovl %u.%u wait %u.%u ms %u fps",	// must fit 320 px
				      nGPUUs / nFrames / 1000, nGPUUs / nFrames / 100 % 10,
				      nOverlayUs / nFrames / 1000, nOverlayUs / nFrames / 100 % 10,
				      nWaitUs / nFrames / 1000, nWaitUs / nFrames / 100 % 10,
				      nFrames / (nNow - nLastReport));
			LOGNOTE ("%ux%u: %s", nWidth, nHeight, (const char *) Stats);

			nFrames = 0;
			nGPUUs = nOverlayUs = nWaitUs = 0;
			nLastReport = nNow;
		}
	}

	return ShutdownHalt;
}

void CKernel::PanelDone (void *pParam)
{
	// nothing to do: WaitIdle () synchronizes; a routine makes SetArea asynchronous
}

void CKernel::Benchmark (unsigned nWidth, unsigned nHeight, unsigned nFrames)
{
	m_Scene.SetResolution (nWidth, nHeight);

	// warm up
	for (unsigned i = 0; i < 3; i++)
	{
		if (!m_Scene.Render (Seconds ()))
		{
			LOGERR ("%ux%u: render failed", nWidth, nHeight);
			return;
		}
	}

	unsigned nBinTotal = 0, nRenderTotal = 0;
	unsigned nStart = CTimer::GetClockTicks ();
	for (unsigned i = 0; i < nFrames; i++)
	{
		m_DevLink.Update ();

		unsigned nBinUs, nRenderUs;
		if (!m_Scene.Render (Seconds (), 0, &nBinUs, &nRenderUs))
		{
			LOGERR ("%ux%u: render failed", nWidth, nHeight);
			return;
		}
		nBinTotal += nBinUs;
		nRenderTotal += nRenderUs;
	}
	unsigned nUs = (CTimer::GetClockTicks () - nStart) / nFrames;

	LOGNOTE ("Benchmark %ux%u: %u.%u ms/frame = %u fps (bin %u us, render %u us)",
		 nWidth, nHeight, nUs / 1000, nUs / 100 % 10, 1000000 / nUs,
		 nBinTotal / nFrames, nRenderTotal / nFrames);

	// V3D performance counters over a few frames (32-bit counters)
	const unsigned nPerfFrames = 5;
	CV3DPerf Perf;
	Perf.Start ();
	nStart = CTimer::GetClockTicks ();
	for (unsigned i = 0; i < nPerfFrames; i++)
	{
		m_Scene.Render (Seconds ());
	}
	nUs = CTimer::GetClockTicks () - nStart;

	CString Title;
	Title.Format ("V3D %ux%u x%u frames", nWidth, nHeight, nPerfFrames);
	Perf.StopAndLog (Title, nUs, nWidth * nHeight * nPerfFrames);
	m_DevLink.Update ();
}
