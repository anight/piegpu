//
// touch.cpp
//
#include "touch.h"
#include <circle/gpiopin.h>
#include <circle/synchronize.h>
#include <circle/logger.h>
#include <circle/timer.h>
#include <circle/util.h>
#include <assert.h>

LOGMODULE ("touch");

// the XPT2046's control byte: start, channel, 12 bits, differential, power
#define CONTROL(a, pd)		(0x80 | (a) << 4 | (pd))
#define POWER_DOWN		0		// (and the pen interrupt on)
#define ADC_ON			1
#define CH_Y			1
#define CH_Z1			3
#define CH_Z2			4
#define CH_X			5

// a conversion's 12 bits: MSB first, after a null bit, in the 2 bytes after its command
#define VALUE(p)		(((p)[0] << 8 | (p)[1]) >> 3 & 0xFFF)

#define WIDTH			320		// the panel, as shown
#define HEIGHT			240
#define MISO_PIN		9

CTouch::CTouch (CPanelOutput *pPanel)
:	m_pDriver (pPanel->GetDriver ()),
	m_bPresent (FALSE),
	m_nX0 (250), m_nX1 (3850), m_nY0 (250), m_nY1 (3850),
	m_bSwap (TRUE),
	m_nReadings (0),
	m_nLastReading (0),
	m_nSeen (0),
	m_bDown (FALSE),
	m_nPresses (0),
	m_nX (0), m_nY (0), m_nRawX (0), m_nRawY (0), m_nPressure (0)
{
	memset (m_Raw, 0, sizeof m_Raw);
}

boolean CTouch::Initialize (const char *pOption, const char *pCalibration)
{
	if (pOption != nullptr && strcmp (pOption, "off") == 0)
	{
		LOGNOTE ("Touch: off (touch=off)");
		return FALSE;
	}

	if (pCalibration != nullptr)
	{
		int v[5] = {0, 0, 0, 0, 0}, n = 0;
		for (const char *p = pCalibration; *p && n < 5; n++)
		{
			v[n] = atoi (p);
			while (*p && *p != ',')
			{
				p++;
			}
			if (*p == ',')
			{
				p++;
			}
		}
		if (n >= 4 && v[0] != v[1] && v[2] != v[3])
		{
			m_nX0 = v[0];
			m_nX1 = v[1];
			m_nY0 = v[2];
			m_nY1 = v[3];
			m_bSwap = n == 5 && v[4] != 0;
		}
		else
		{
			LOGWARN ("touchcal=%s: not x0,x1,y0,y1[,swap]; the defaults", pCalibration);
		}
	}

	m_bPresent = Detect ();
	if (!m_bPresent)
	{
		LOGNOTE ("Touch: no XPT2046 answers on CE1 (GPIO7)");
		return FALSE;
	}

	// a reading: its commands, each followed by a byte for the result's
	// second half (the next command overlaps it)
	static const u8 Commands[Bytes] =
	{
		CONTROL (CH_Z1, ADC_ON), 0, CONTROL (CH_Z2, ADC_ON), 0,
		CONTROL (CH_Y, ADC_ON), 0,		// (thrown away: the first is noisy)
		CONTROL (CH_Y, ADC_ON), 0, CONTROL (CH_X, ADC_ON), 0,
		CONTROL (CH_Y, ADC_ON), 0, CONTROL (CH_X, ADC_ON), 0,
		CONTROL (CH_Y, ADC_ON), 0, CONTROL (CH_X, POWER_DOWN), 0,
		0, 0
	};
	m_pDriver->SetAux (ChipSelect, ClockSpeed, Commands, Bytes, AuxRoutine, this);

	LOGNOTE ("Touch: XPT2046 on CE1 (GPIO7), read after each panel frame; calibration %d,%d,%d,%d,%u",
		 m_nX0, m_nX1, m_nY0, m_nY1, m_bSwap ? 1 : 0);

	return TRUE;
}

// Is one there? With MISO pulled up, the XPT2046 drives a conversion's null
// bit and the zeros after its 12 bits; nothing there reads all ones. 8 times
boolean CTouch::Detect (void)
{
	CGPIOPin MISO (MISO_PIN, GPIOModeAlternateFunction0);
	MISO.SetPullMode (GPIOPullModeUp);
	CTimer::SimpleusDelay (20);

	boolean bPresent = TRUE;
	for (unsigned i = 0; i < 8 && bPresent; i++)
	{
		static const u8 Tx[3] = {CONTROL (CH_Z1, POWER_DOWN), 0, 0};
		u8 Rx[3];
		m_pDriver->Transfer (ChipSelect, ClockSpeed, Tx, Rx, sizeof Rx);
		if ((Rx[1] & 0x80) || (Rx[2] & 0x07))
		{
			bPresent = FALSE;
		}
	}

	MISO.SetPullMode (GPIOPullModeOff);
	return bPresent;
}

void CTouch::AuxRoutine (const u8 *pRx, void *pParam)
{
	CTouch *pThis = static_cast<CTouch *> (pParam);
	assert (pThis != nullptr);

	memcpy (pThis->m_Raw, pRx, Bytes);
	pThis->m_nLastReading = CTimer::GetClockTicks ();
	DataMemBarrier ();
	pThis->m_nReadings = pThis->m_nReadings + 1;
}

int CTouch::BestTwo (int a, int b, int c)
{
	int ab = a > b ? a - b : b - a, ac = a > c ? a - c : c - a, bc = b > c ? b - c : c - b;
	return   ab <= ac && ab <= bc ? (a + b) / 2
	       : ac <= ab && ac <= bc ? (a + c) / 2 : (b + c) / 2;
}

boolean CTouch::Update (u32 *pPayload)
{
	if (!m_bPresent)
	{
		return FALSE;
	}

	// no frames to the panel lately: a reading now (the bus is idle)
	if (CTimer::GetClockTicks () - m_nLastReading > IdleUs)
	{
		m_pDriver->AuxNow ();
	}

	u8 Raw[Bytes];
	EnterCritical ();
	unsigned nReadings = m_nReadings;
	memcpy (Raw, m_Raw, Bytes);
	LeaveCritical ();
	if (nReadings == m_nSeen)
	{
		return FALSE;
	}
	m_nSeen = nReadings;

	// the results start one byte after their commands (the first byte out is a command's)
	const u8 *r = Raw + 1;
	int nZ1 = VALUE (r), nZ2 = VALUE (r + 2);
	int nY = BestTwo (VALUE (r + 6), VALUE (r + 10), VALUE (r + 14));
	int nX = BestTwo (VALUE (r + 8), VALUE (r + 12), VALUE (r + 16));
	int nPressure = nZ1 + 4095 - nZ2;
	if (nZ1 == 0)
	{
		nPressure = 0;				// (not touched: Z1 reads 0)
	}

	boolean bDown = m_bDown;
	if (nPressure >= Pressed)
	{
		bDown = TRUE;
	}
	else if (nPressure < Released)
	{
		bDown = FALSE;
	}

	boolean bChanged = bDown != m_bDown;
	if (bDown)
	{
		int nA = m_bSwap ? nY : nX, nB = m_bSwap ? nX : nY;
		int x = (nA - m_nX0) * (WIDTH - 1) / (m_nX1 - m_nX0);
		int y = (nB - m_nY0) * (HEIGHT - 1) / (m_nY1 - m_nY0);
		x = x < 0 ? 0 : x > WIDTH - 1 ? WIDTH - 1 : x;
		y = y < 0 ? 0 : y > HEIGHT - 1 ? HEIGHT - 1 : y;
		bChanged = bChanged || x != m_nX || y != m_nY;
		m_nX = (u16) x;
		m_nY = (u16) y;
		m_nRawX = (u16) nX;
		m_nRawY = (u16) nY;
		m_nPressure = (u16) nPressure;
		if (!m_bDown)
		{
			m_nPresses++;
		}
	}
	m_bDown = bDown;

	if (bChanged)
	{
		GetState (pPayload);
	}

	return bChanged;
}

void CTouch::GetState (u32 *pPayload) const
{
	pPayload[0] = (m_bDown ? PGPU_TOUCH_DOWN : 0) | (m_nPresses & 0xFFFF) << 16;
	pPayload[1] = m_nX | (u32) m_nY << 16;
	pPayload[2] = m_nRawX | (u32) m_nRawY << 16;
	pPayload[3] = m_nPressure;
}
