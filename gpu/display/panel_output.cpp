//
// panel_output.cpp
//
#include "panel_output.h"
#include <circle/gpiopin.h>
#include <circle/logger.h>
#include <circle/timer.h>
#include <circle/util.h>

LOGMODULE ("panel");

#define SPI_CLOCK_SPEED		75000000
#define CS_PIN			8		// SPI0: CE0, MISO, MOSI, SCLK
#define MISO_PIN		9
#define MOSI_PIN		10
#define SCLK_PIN		11
#define DC_PIN			24
#define RESET_PIN		25
#define WIDTH			320
#define HEIGHT			240

CPanelOutput::CPanelOutput (CInterruptSystem *pInterrupt)
:	m_Display (pInterrupt, DC_PIN, RESET_PIN, WIDTH, HEIGHT, SPI_CLOCK_SPEED, 0,
		   TRUE)			// little endian RGB565 from the V3D
{
}

void CPanelOutput::Show (const void *pPixels, TDoneRoutine *pDone, void *pParam)
{
	const CDisplay::TArea Full = {0, WIDTH - 1, 0, HEIGHT - 1};
	m_Display.SetArea (Full, pPixels, pDone, pParam);
}

// ---- reading the panel's registers ----------------------------------------------
//
// Bit-banged at about 500 kHz: the ST7789 reads at most about 6.7 MHz (a 150 ns
// read cycle), the driver's SPI runs at 75 MHz. SPI mode 0: the command goes
// out with D/C low, then the panel shifts its answer out on MISO after each
// falling SCLK edge (sampled here at the rising edge).

namespace
{
	struct TBitBang
	{
		CGPIOPin CS, SCLK, MOSI, MISO, DC, Reset;

		TBitBang (void)
		:	CS (CS_PIN, GPIOModeOutput), SCLK (SCLK_PIN, GPIOModeOutput),
			MOSI (MOSI_PIN, GPIOModeOutput), MISO (MISO_PIN, GPIOModeInput),
			DC (DC_PIN, GPIOModeOutput), Reset (RESET_PIN, GPIOModeOutput)
		{
			CS.Write (HIGH);
			SCLK.Write (LOW);
		}

		~TBitBang (void)			// the pins back to SPI0
		{
			CS.SetMode (GPIOModeAlternateFunction0);
			SCLK.SetMode (GPIOModeAlternateFunction0);
			MOSI.SetMode (GPIOModeAlternateFunction0);
			MISO.SetMode (GPIOModeAlternateFunction0);
		}

		void Clock (void)
		{
			CTimer::SimpleusDelay (1);
			SCLK.Write (HIGH);
		}

		u64 Read (u8 nCommand, unsigned nBits)
		{
			CS.Write (LOW);
			DC.Write (LOW);
			for (int i = 7; i >= 0; i--)
			{
				MOSI.Write ((nCommand >> i) & 1);
				Clock ();
				CTimer::SimpleusDelay (1);
				SCLK.Write (LOW);
			}
			DC.Write (HIGH);
			MOSI.Write (LOW);

			u64 nValue = 0;
			for (unsigned i = 0; i < nBits; i++)
			{
				Clock ();
				nValue = nValue << 1 | MISO.Read ();
				CTimer::SimpleusDelay (1);
				SCLK.Write (LOW);
			}
			CTimer::SimpleusDelay (1);
			CS.Write (HIGH);
			CTimer::SimpleusDelay (2);

			return nValue;
		}
	};
}

// The panel answers RDDID (04h: a dummy bit, then its three ID bytes) the same
// with MISO pulled up and pulled down, and releases the line after them (the
// pull shows). With nothing on MISO the pull-up reads all ones and the
// pull-down all zeros. Checked 8 times each.
boolean CPanelOutput::Detect (TPanelInfo *pInfo)
{
	TBitBang B;
	B.Reset.Write (LOW);				// a hardware reset: its registers' defaults
	CTimer::SimpleusDelay (20);
	B.Reset.Write (HIGH);
	CTimer::SimpleMsDelay (120);

	boolean bPresent = TRUE;
	u32 nID = 0;
	for (unsigned i = 0; i < 8 && bPresent; i++)
	{
		for (unsigned nPull = 0; nPull < 2; nPull++)
		{
			B.MISO.SetPullMode (nPull ? GPIOPullModeDown : GPIOPullModeUp);
			CTimer::SimpleusDelay (20);
			u32 nValue = (u32) B.Read (0x04, 32);	// dummy, 24 ID bits, 7 released
			u32 nThisID = (nValue >> 7) & 0xFFFFFF;
			if (   (nValue & 0x7F) != (nPull ? 0x00 : 0x7F)
			    || (i + nPull > 0 && nThisID != nID))
			{
				bPresent = FALSE;
				break;
			}
			nID = nThisID;
		}
	}
	B.MISO.SetPullMode (GPIOPullModeUp);
	CTimer::SimpleusDelay (20);

	if (pInfo != nullptr)
	{
		memset (pInfo, 0, sizeof *pInfo);
		pInfo->bPresent = bPresent;
		if (bPresent)
		{
			pInfo->nID = nID;
			pInfo->nStatus = (u32) (B.Read (0x09, 40) >> 7);	// dummy, 32 bits
			pInfo->nPowerMode = (u8) (B.Read (0x0A, 8));
			pInfo->nMADCTL = (u8) (B.Read (0x0B, 8));
			pInfo->nPixelFormat = (u8) (B.Read (0x0C, 8));
			pInfo->nSelfDiagnostic = (u8) (B.Read (0x0F, 8));
		}
	}
	B.MISO.SetPullMode (GPIOPullModeOff);

	return bPresent;
}
