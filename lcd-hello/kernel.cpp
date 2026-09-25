#include "kernel.h"
#include <circle/string.h>

// 3.5" ILI9486 HAT wiring (SoC GPIO numbers)
#define SPI_CLOCK_SPEED		20800000	// Hz (250 MHz core / even divider; 25 MHz corrupts mid-tones, 31.25 MHz fails)
#define SPI_CHIP_SELECT		0		// LCD on CE0 (touch is on CE1)
#define DC_PIN			24
#define RESET_PIN		25
#define ROTATION		90		// landscape 480x320 (use 270 to flip)

static const char FromKernel[] = "kernel";

CKernel::CKernel (void)
:	m_Timer (&m_Interrupt),
	m_Logger (m_Options.GetLogLevel (), &m_Timer),
	m_DevLink (&m_Interrupt),
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
		bOK = m_Serial.Initialize (115200);	// log on UART (GPIO14 TX)
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
	m_Logger.Write (FromKernel, LogNotice, "Compile time: " __DATE__ " " __TIME__);
	m_Logger.Write (FromKernel, LogNotice, "Display %ux%u, SPI %u Hz",
			m_Display.GetWidth (), m_Display.GetHeight (), SPI_CLOCK_SPEED);

	unsigned nLastSecond = (unsigned) -1;
	while (1)
	{
		m_DevLink.Update ();

		unsigned nSecond = m_Timer.GetUptime ();
		if (nSecond == nLastSecond)
		{
			continue;
		}
		nLastSecond = nSecond;

		DrawStatic ();

		// SPI speed and build time identify this boot, so a stale image is obvious
		CString Uptime;
		Uptime.Format ("Uptime: %u s   SPI %u.%u MHz   Build " __TIME__, nSecond,
			       SPI_CLOCK_SPEED / 1000000, SPI_CLOCK_SPEED / 100000 % 10);
		m_Graphics.DrawText (m_Graphics.GetWidth () / 2, 290, COLOR2D (255, 255, 0),
				     Uptime, C2DGraphics::AlignCenter);

		unsigned nStart = m_Timer.GetClockTicks ();
		m_Graphics.UpdateDisplay ();
		unsigned nFrameUs = m_Timer.GetClockTicks () - nStart;

		m_Logger.Write (FromKernel, LogNotice, "Uptime %u s, frame sent in %u us (%u.%u fps max)",
				nSecond, nFrameUs, 10000000 / nFrameUs / 10, 10000000 / nFrameUs % 10);

		// heartbeat on the ACT LED
		m_ActLED.On ();
		CTimer::SimpleMsDelay (50);
		m_ActLED.Off ();
	}

	return ShutdownHalt;
}

void CKernel::DrawStatic (void)
{
	unsigned nWidth = m_Graphics.GetWidth ();

	m_Graphics.ClearScreen (COLOR2D (0, 0, 0));

	m_Graphics.DrawText (nWidth / 2, 30, COLOR2D (255, 255, 255), "Hello, world!",
			     C2DGraphics::AlignCenter, Font12x22,
			     CCharGenerator::FontFlagsDoubleBoth);

	m_Graphics.DrawText (nWidth / 2, 100, COLOR2D (128, 128, 128),
			     "Circle on Raspberry Pi Zero - ILI9486 480x320",
			     C2DGraphics::AlignCenter);

	// color bars to check RGB/BGR order: must read red, green, blue, ...
	static const struct
	{
		T2DColor	Color;
		const char	*pName;
	}
	Bars[] =
	{
		{COLOR2D (255, 0, 0),		"RED"},
		{COLOR2D (0, 255, 0),		"GREEN"},
		{COLOR2D (0, 0, 255),		"BLUE"},
		{COLOR2D (255, 255, 0),		"YELLOW"},
		{COLOR2D (0, 255, 255),		"CYAN"},
		{COLOR2D (255, 0, 255),		"MAGENTA"},
		{COLOR2D (255, 255, 255),	"WHITE"},
		{COLOR2D (0, 0, 0),		"BLACK"},
	};
	const unsigned nBars = sizeof Bars / sizeof Bars[0];
	const unsigned nBarWidth = nWidth / nBars;

	for (unsigned i = 0; i < nBars; i++)
	{
		unsigned x = i * nBarWidth;

		m_Graphics.DrawRect (x, 140, nBarWidth, 110, Bars[i].Color);
		m_Graphics.DrawRectOutline (x, 140, nBarWidth, 110, COLOR2D (80, 80, 80));
		m_Graphics.DrawText (x + nBarWidth / 2, 255, COLOR2D (200, 200, 200),
				     Bars[i].pName, C2DGraphics::AlignCenter, Font6x7);
	}

	// frame around the whole screen to check the edges
	m_Graphics.DrawRectOutline (0, 0, nWidth, m_Graphics.GetHeight (), COLOR2D (0, 255, 0));
}
