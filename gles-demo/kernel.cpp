#include "kernel.h"
#include <circle/string.h>
#include <v3dperf.h>

// 3.5" ILI9486 HAT wiring (SoC GPIO numbers)
#define SPI_CLOCK_SPEED		20800000	// Hz, fastest clean speed of this HAT
#define SPI_CHIP_SELECT		0
#define DC_PIN			24
#define RESET_PIN		25
#define ROTATION		90		// landscape 480x320

LOGMODULE ("kernel");

static float Seconds (void)
{
	return CTimer::GetClockTicks64 () / 1000000.0f;
}

CKernel::CKernel (void)
:	m_Timer (&m_Interrupt),
	m_Logger (m_Options.GetLogLevel (), &m_Timer),
	m_DevLink (&m_Interrupt),
	m_VCHIQ (CMemorySystem::Get (), &m_Interrupt),
	m_SPIMaster (SPI_CLOCK_SPEED, 0, 0),
	m_Display (&m_SPIMaster, DC_PIN, RESET_PIN, CILI9486Display::None,
		   SPI_CLOCK_SPEED, SPI_CHIP_SELECT),
	m_Graphics (&m_Display)
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
		bOK = m_VCHIQ.Initialize ();
	}

	if (bOK)
	{
		bOK = m_SPIMaster.Initialize ();
	}

	if (bOK)
	{
		m_Display.SetRotation (ROTATION);

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

	if (!m_Renderer.Initialize ())
	{
		LOGPANIC ("Renderer init failed");
	}

	// How fast can the GPU render this scene, independent of the display?
	Benchmark (480, 320, 60);
	Benchmark (1280, 720, 30);
	Benchmark (1920, 1080, 30);

	// Live demo on the SPI panel
	const unsigned nWidth = m_Graphics.GetWidth ();
	const unsigned nHeight = m_Graphics.GetHeight ();
	if (!m_Renderer.SetSurface (nWidth, nHeight))
	{
		LOGPANIC ("Cannot create render target");
	}

	u16 *pFrame = (u16 *) m_Graphics.GetBuffer ();

	unsigned nFrames = 0;
	unsigned nGPUUs = 0, nReadUs = 0, nPanelUs = 0;
	unsigned nLastReport = m_Timer.GetUptime ();
	CString Stats ("measuring...");

	while (1)
	{
		m_DevLink.Update ();

		unsigned nStart = CTimer::GetClockTicks ();
		m_Renderer.Render (Seconds ());
		m_Renderer.Finish ();
		nGPUUs += CTimer::GetClockTicks () - nStart;

		nReadUs += m_Renderer.ReadPixelsRGB565BE (pFrame);

		// overlay (drawn by the ARM into the same buffer)
		m_Graphics.DrawRect (0, 0, nWidth, 20, COLOR2D (0, 0, 0));
		m_Graphics.DrawText (6, 2, COLOR2D (255, 255, 255),
				     "OpenGL ES 2.0 on VideoCore IV - Build " __TIME__);
		m_Graphics.DrawRect (0, nHeight-20, nWidth, 20, COLOR2D (0, 0, 0));
		m_Graphics.DrawText (6, nHeight-18, COLOR2D (255, 255, 0), Stats);

		nStart = CTimer::GetClockTicks ();
		m_Graphics.UpdateDisplay ();
		nPanelUs += CTimer::GetClockTicks () - nStart;

		nFrames++;

		unsigned nNow = m_Timer.GetUptime ();
		if (nNow != nLastReport)
		{
			Stats.Format ("GPU %u.%u ms  read %u ms  panel %u ms  %u fps",
				      nGPUUs / nFrames / 1000, nGPUUs / nFrames / 100 % 10,
				      nReadUs / nFrames / 1000, nPanelUs / nFrames / 1000,
				      nFrames / (nNow - nLastReport));
			LOGNOTE ("%ux%u: %s", nWidth, nHeight, (const char *) Stats);

			nFrames = 0;
			nGPUUs = nReadUs = nPanelUs = 0;
			nLastReport = nNow;
		}
	}

	return ShutdownHalt;
}

void CKernel::Benchmark (unsigned nWidth, unsigned nHeight, unsigned nFrames)
{
	if (!m_Renderer.SetSurface (nWidth, nHeight))
	{
		return;
	}

	// warm up (first frames include shader setup)
	for (unsigned i = 0; i < 3; i++)
	{
		m_Renderer.Render (Seconds ());
		m_Renderer.Finish ();
	}

	unsigned nStart = CTimer::GetClockTicks ();
	for (unsigned i = 0; i < nFrames; i++)
	{
		m_DevLink.Update ();

		m_Renderer.Render (Seconds ());
		m_Renderer.Finish ();
	}
	unsigned nUs = (CTimer::GetClockTicks () - nStart) / nFrames;

	LOGNOTE ("Benchmark %ux%u: %u.%u ms/frame = %u fps (plasma shader + cube, glFinish each frame)",
		 nWidth, nHeight, nUs / 1000, nUs / 100 % 10, 1000000 / nUs);

	// V3D performance counters over a few frames (32-bit counters)
	const unsigned nPerfFrames = 5;
	CV3DPerf Perf;
	Perf.Start ();
	nStart = CTimer::GetClockTicks ();
	for (unsigned i = 0; i < nPerfFrames; i++)
	{
		m_Renderer.Render (Seconds ());
		m_Renderer.Finish ();
	}
	nUs = CTimer::GetClockTicks () - nStart;

	CString Title;
	Title.Format ("GLES %ux%u x%u frames", nWidth, nHeight, nPerfFrames);
	Perf.StopAndLog (Title, nUs, nWidth * nHeight * nPerfFrames);
	m_DevLink.Update ();
}
