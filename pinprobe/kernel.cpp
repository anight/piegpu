//
// pinprobe: find out from the running system how the 26-pin display HAT is
// connected. Reports the GPIO function/level registers, probes undriven pins
// with the internal pull resistors, tests RESET and chip selects with the
// display (for the camera), and talks to the touch controller.
//
#include "kernel.h"
#include <circle/bcm2835.h>
#include <circle/memio.h>
#include <circle/string.h>

// display configuration under test (same as the demos)
#define SPI_CLOCK_SPEED		20800000
#define DC_PIN			24
#define RESET_PIN		25

LOGMODULE ("pinprobe");

// 26-pin header: physical pin -> GPIO (from the Pi Zero schematic, J8), -1 = power/GND
static const int HeaderGPIO[27] =
{
	-1,			// (no pin 0)
	-1, -1,			// 1 3V3,      2 5V
	 2, -1,			// 3 GPIO2,    4
	 3, -1,			// 5 GPIO3,    6
	 4, 14,			// 7 GPIO4,    8 GPIO14
	-1, 15,			// 9,         10 GPIO15
	17, 18,			// 11 GPIO17, 12 GPIO18
	27, -1,			// 13 GPIO27, 14
	22, 23,			// 15 GPIO22, 16 GPIO23
	-1, 24,			// 17,        18 GPIO24
	10, -1,			// 19 GPIO10, 20
	 9, 25,			// 21 GPIO9,  22 GPIO25
	11,  8,			// 23 GPIO11, 24 GPIO8
	-1,  7,			// 25,        26 GPIO7
};

static const char *FunctionName[8] =
	{"input", "output", "ALT5", "ALT4", "ALT0", "ALT1", "ALT2", "ALT3"};

static unsigned GetFunction (unsigned nPin)
{
	return (read32 (ARM_GPIO_GPFSEL0 + nPin / 10 * 4) >> (nPin % 10 * 3)) & 7;
}

static void SetFunction (unsigned nPin, unsigned nFunction)
{
	u32 nAddr = ARM_GPIO_GPFSEL0 + nPin / 10 * 4;
	unsigned nShift = nPin % 10 * 3;
	write32 (nAddr, (read32 (nAddr) & ~(7 << nShift)) | nFunction << nShift);
}

static unsigned GetLevel (unsigned nPin)
{
	return (read32 (ARM_GPIO_GPLEV0) >> nPin) & 1;
}

static void SetPull (unsigned nPin, unsigned nPull)	// 0 off, 1 down, 2 up
{
	write32 (ARM_GPIO_GPPUD, nPull);
	CTimer::SimpleusDelay (5);
	write32 (ARM_GPIO_GPPUDCLK0, 1 << nPin);
	CTimer::SimpleusDelay (5);
	write32 (ARM_GPIO_GPPUD, 0);
	write32 (ARM_GPIO_GPPUDCLK0, 0);
	CTimer::SimpleMsDelay (2);				// let the pin settle
}

CKernel::CKernel (void)
:	m_Timer (&m_Interrupt),
	m_Logger (m_Options.GetLogLevel (), &m_Timer),
	m_DevLink (&m_Interrupt),
	m_SPIMaster (SPI_CLOCK_SPEED, 0, 0),
	m_Display (&m_SPIMaster, DC_PIN, RESET_PIN, CILI9486Display::None, SPI_CLOCK_SPEED, 0),
	m_Graphics (&m_Display)
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
	       && m_DevLink.Initialize ();
}

TShutdownMode CKernel::Run (void)
{
	LOGNOTE ("Compile time: " __DATE__ " " __TIME__);
	Wait (2);				// let the host connect

	DumpHeader ("after boot, before SPI/display init");

	m_SPIMaster.Initialize ();
	m_Display.SetRotation (90);
	m_Display.Initialize ();
	m_Graphics.Initialize ();

	DumpHeader ("display running (as in the demos)");

	ProbePulls ();
	ProbeTouch ();

	// Display control line tests, watched by the camera
	Phase (1, "reference: display on CE0, RESET high");
	FillScreen (COLOR2D (0, 0, 160), "PHASE 1: CE0, RESET high");
	Ready (1);

	Phase (2, "RESET (GPIO25) held LOW");
	write32 (ARM_GPIO_GPCLR0, 1 << RESET_PIN);
	CTimer::SimpleMsDelay (200);
	Ready (2);

	Phase (3, "RESET released, display re-initialized on CE0");
	write32 (ARM_GPIO_GPSET0, 1 << RESET_PIN);
	m_Display.Initialize ();
	FillScreen (COLOR2D (0, 120, 0), "PHASE 3: re-init on CE0");
	Ready (3);

	Phase (4, "red screen sent with chip select CE1 instead of CE0");
	{
		CILI9486Display DisplayCE1 (&m_SPIMaster, DC_PIN, CILI9486Display::None,
					    CILI9486Display::None, SPI_CLOCK_SPEED, 1);
		DisplayCE1.SetRotation (90);
		DisplayCE1.Initialize ();		// no reset pin: init commands only
		DisplayCE1.Clear (m_Display.GetColor (CDisplay::BrightRed));
	}
	Ready (4);

	Phase (5, "done");
	while (1)
	{
		m_DevLink.Update ();
	}

	return ShutdownHalt;
}

void CKernel::DumpHeader (const char *pWhen)
{
	LOGNOTE ("--- GPIO registers, %s", pWhen);
	for (unsigned nPin = 1; nPin <= 26; nPin++)
	{
		int nGPIO = HeaderGPIO[nPin];
		if (nGPIO < 0)
		{
			continue;
		}

		LOGNOTE ("header pin %2u  GPIO%-2d  %-6s  level %u", nPin, nGPIO,
			 FunctionName[GetFunction (nGPIO)], GetLevel (nGPIO));
	}
	m_DevLink.Update ();
}

// A pin nobody drives follows the internal pull (~50k); a pin that is driven
// or has a stronger external resistor keeps its level.
void CKernel::ProbePulls (void)
{
	LOGNOTE ("--- pull probe (pins temporarily set to input)");
	for (unsigned nPin = 1; nPin <= 26; nPin++)
	{
		int nGPIO = HeaderGPIO[nPin];
		if (   nGPIO < 0
		    || nGPIO == DC_PIN || nGPIO == RESET_PIN		// driven by us
		    || nGPIO == 8 || nGPIO == 10 || nGPIO == 11)	// CE0, MOSI, SCLK (driven by us)
		{
			continue;
		}

		unsigned nFunction = GetFunction (nGPIO);
		SetFunction (nGPIO, 0);				// input

		SetPull (nGPIO, 2);
		unsigned nUp = GetLevel (nGPIO);
		SetPull (nGPIO, 1);
		unsigned nDown = GetLevel (nGPIO);
		SetPull (nGPIO, 0);
		unsigned nOff = GetLevel (nGPIO);

		SetFunction (nGPIO, nFunction);			// restore

		const char *pResult =   nUp == 1 && nDown == 0 ? "follows pulls (not driven)"
				      : nUp == 1 && nDown == 1 ? "stays HIGH (driven or pulled up externally)"
				      : nUp == 0 && nDown == 0 ? "stays LOW (driven or pulled down externally)"
				      : "inverted?!";
		LOGNOTE ("header pin %2u  GPIO%-2d  pull-up %u  pull-down %u  no pull %u  -> %s",
			 nPin, nGPIO, nUp, nDown, nOff, pResult);
		m_DevLink.Update ();
	}
}

// XPT2046/ADS7846-style touch controller on CE1: read the internal temperature
// channel, which returns a stable mid-scale value if a chip answers.
void CKernel::ProbeTouch (void)
{
	LOGNOTE ("--- touch controller probe on CE1 (1 MHz, mode 0)");

	static const struct { u8 uchCmd; const char *pName; } Channels[] =
	{
		{0x84, "TEMP0"},
		{0xF4, "TEMP1"},
		{0xD4, "X"},
		{0x94, "Y"},
		{0xB4, "Z1"},
	};

	for (unsigned nMISOPull = 1; nMISOPull <= 2; nMISOPull++)
	{
		SetPull (9, nMISOPull);			// MISO pulled down, then up

		CString Line;
		for (auto &Ch : Channels)
		{
			u8 Tx[3] = {Ch.uchCmd, 0, 0};
			u8 Rx[3] = {0, 0, 0};
			m_SPIMaster.SetClock (1000000);
			m_SPIMaster.SetMode (0, 0);
			m_SPIMaster.WriteRead (1, Tx, Rx, sizeof Tx);

			unsigned nValue = (Rx[1] << 8 | Rx[2]) >> 3;	// 12 bit result
			CString Item;
			Item.Format (" %s=%u (%02X %02X %02X)", Ch.pName, nValue, Rx[0], Rx[1], Rx[2]);
			Line.Append (Item);
		}
		LOGNOTE ("MISO pull-%s:%s", nMISOPull == 1 ? "down" : "up  ", (const char *) Line);
		m_DevLink.Update ();
	}

	SetPull (9, 0);
}

void CKernel::FillScreen (T2DColor Color, const char *pText)
{
	m_Graphics.ClearScreen (Color);
	m_Graphics.DrawText (m_Graphics.GetWidth () / 2, 140, COLOR2D (255, 255, 255), pText,
			     C2DGraphics::AlignCenter, Font12x22);
	m_Graphics.DrawText (m_Graphics.GetWidth () / 2, 190, COLOR2D (255, 255, 255),
			     "pinprobe build " __TIME__, C2DGraphics::AlignCenter);
	m_Graphics.UpdateDisplay ();
}

// Phases are timed by the device: the host watches for "PHASE n READY" and
// takes a photo. (Host-to-device data can stall on the CDC gadget, see notes.)
void CKernel::Phase (unsigned nPhase, const char *pDescription)
{
	LOGNOTE ("PHASE %u: %s", nPhase, pDescription);
	m_DevLink.Update ();
}

void CKernel::Ready (unsigned nPhase)
{
	LOGNOTE ("PHASE %u READY", nPhase);
	Wait (10);				// time for the camera
}

void CKernel::Wait (unsigned nSeconds)
{
	unsigned nEnd = m_Timer.GetUptime () + nSeconds;
	while (m_Timer.GetUptime () < nEnd)
	{
		m_DevLink.Update ();
	}
}
