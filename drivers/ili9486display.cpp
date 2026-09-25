//
// ili9486display.cpp
//
// Driver for ILI9486-based 3.5" 480x320 SPI display HATs.
// Init sequence from the Linux fbtft fb_ili9486 driver / piscreen overlay.
//
// Based on CILI9341Display from Circle (GPLv3).
//
#include "ili9486display.h"
#include <circle/timer.h>
#include <circle/util.h>
#include <circle/stdarg.h>
#include <assert.h>

// Display commands (MIPI DCS + ILI9486 specific)
#define ILI9486_SLEEP_OUT		0x11
#define ILI9486_DISPLAY_OFF		0x28
#define ILI9486_DISPLAY_ON		0x29
#define ILI9486_COLUMN_ADDRESS_SET	0x2A
#define ILI9486_PAGE_ADDRESS_SET	0x2B
#define ILI9486_MEMORY_WRITE		0x2C
#define ILI9486_MEMORY_ACCESS_CONTROL	0x36
	#define ILI9486_MADCTL_BGR		0x08
	#define ILI9486_MADCTL_MV		0x20
	#define ILI9486_MADCTL_MX		0x40
	#define ILI9486_MADCTL_MY		0x80
#define ILI9486_PIXEL_FORMAT_SET	0x3A
#define ILI9486_INTERFACE_MODE_CONTROL	0xB0
#define ILI9486_POWER_CONTROL_3		0xC2
#define ILI9486_VCOM_CONTROL_1		0xC5
#define ILI9486_POSITIVE_GAMMA		0xE0
#define ILI9486_NEGATIVE_GAMMA		0xE1
#define ILI9486_DIGITAL_GAMMA_1		0xE2

// Native (rotation 0) resolution
#define ILI9486_WIDTH			320
#define ILI9486_HEIGHT			480

// The BCM2835 SPI master has a transfer size limit (must be even here)
#define MAX_TRANSFER_SIZE		0xFFFC

CILI9486Display::CILI9486Display (CSPIMaster *pSPIMaster,
				  unsigned nDCPin, unsigned nResetPin, unsigned nBackLightPin,
				  unsigned nClockSpeed, unsigned nChipSelect)
:	CDisplay (RGB565_BE),
	m_pSPIMaster (pSPIMaster),
	m_nResetPin (nResetPin),
	m_nBackLightPin (nBackLightPin),
	m_nWidth (ILI9486_WIDTH),
	m_nHeight (ILI9486_HEIGHT),
	m_nClockSpeed (nClockSpeed),
	m_nChipSelect (nChipSelect),
	m_nRotation (0),
	m_DCPin (nDCPin, GPIOModeOutput)
{
	assert (nDCPin != None);

	if (m_nBackLightPin != None)
	{
		m_BackLightPin.AssignPin (m_nBackLightPin);
		m_BackLightPin.SetMode (GPIOModeOutput, FALSE);
	}

	if (m_nResetPin != None)
	{
		m_ResetPin.AssignPin (m_nResetPin);
		m_ResetPin.SetMode (GPIOModeOutput, FALSE);
	}
}

CILI9486Display::~CILI9486Display (void)
{
}

void CILI9486Display::SetRotation (unsigned nDegrees)
{
	assert (nDegrees < 360 && nDegrees % 90 == 0);

	m_nRotation = nDegrees;
}

boolean CILI9486Display::Initialize (void)
{
	assert (m_pSPIMaster != 0);

	if (m_nBackLightPin != None)
	{
		m_BackLightPin.Write (HIGH);
	}

	if (m_nResetPin != None)
	{
		m_ResetPin.Write (HIGH);
		CTimer::SimpleMsDelay (10);
		m_ResetPin.Write (LOW);
		CTimer::SimpleMsDelay (10);
		m_ResetPin.Write (HIGH);
		CTimer::SimpleMsDelay (120);
	}

	// Init sequence

	CommandAndData (ILI9486_INTERFACE_MODE_CONTROL, 1, 0x00);

	Command (ILI9486_SLEEP_OUT);
	CTimer::SimpleMsDelay (250);

	CommandAndData (ILI9486_PIXEL_FORMAT_SET, 1, 0x55);	// 16 bpp

	u8 uchMADCtl = ILI9486_MADCTL_BGR;
	switch (m_nRotation)
	{
	case 0:
		uchMADCtl |= ILI9486_MADCTL_MY;
		break;

	case 90:
		uchMADCtl |= ILI9486_MADCTL_MV;
		break;

	case 180:
		uchMADCtl |= ILI9486_MADCTL_MX;
		break;

	case 270:
		uchMADCtl |= ILI9486_MADCTL_MY | ILI9486_MADCTL_MX | ILI9486_MADCTL_MV;
		break;

	default:
		assert (0);
		break;
	}

	CommandAndData (ILI9486_MEMORY_ACCESS_CONTROL, 1, uchMADCtl);

	if (m_nRotation % 180)
	{
		m_nWidth = ILI9486_HEIGHT;
		m_nHeight = ILI9486_WIDTH;
	}

	CommandAndData (ILI9486_POWER_CONTROL_3, 1, 0x44);

	CommandAndData (ILI9486_VCOM_CONTROL_1, 4, 0x00, 0x00, 0x00, 0x00);

	CommandAndData (ILI9486_POSITIVE_GAMMA, 15,
			0x0F, 0x1F, 0x1C, 0x0C, 0x0F, 0x08, 0x48, 0x98,
			0x37, 0x0A, 0x13, 0x04, 0x11, 0x0D, 0x00);

	CommandAndData (ILI9486_NEGATIVE_GAMMA, 15,
			0x0F, 0x32, 0x2E, 0x0B, 0x0D, 0x05, 0x47, 0x75,
			0x37, 0x06, 0x10, 0x03, 0x24, 0x20, 0x00);

	CommandAndData (ILI9486_DIGITAL_GAMMA_1, 15,
			0x0F, 0x32, 0x2E, 0x0B, 0x0D, 0x05, 0x47, 0x75,
			0x37, 0x06, 0x10, 0x03, 0x24, 0x20, 0x00);

	Command (ILI9486_SLEEP_OUT);
	CTimer::SimpleMsDelay (5);

	Clear ();
	On ();

	return TRUE;
}

void CILI9486Display::On (void)
{
	Command (ILI9486_DISPLAY_ON);
}

void CILI9486Display::Off (void)
{
	Command (ILI9486_DISPLAY_OFF);
}

void CILI9486Display::Clear (TRawColor nColor)
{
	assert (m_nWidth > 0);
	assert (m_nHeight > 0);

	SetWindow (0, 0, m_nWidth-1, m_nHeight-1);

	u16 Buffer[m_nWidth];
	for (unsigned x = 0; x < m_nWidth; x++)
	{
		Buffer[x] = (u16) nColor;
	}

	for (unsigned y = 0; y < m_nHeight; y++)
	{
		Write (TRUE, Buffer, sizeof Buffer);
	}
}

void CILI9486Display::SetPixel (unsigned nPosX, unsigned nPosY, TRawColor nColor)
{
	SetWindow (nPosX, nPosY, nPosX, nPosY);

	u16 usColor = (u16) nColor;
	Write (TRUE, &usColor, sizeof usColor);
}

void CILI9486Display::SetArea (const TArea &rArea, const void *pPixels,
			       TAreaCompletionRoutine *pRoutine,
			       void *pParam)
{
	SetWindow (rArea.x1, rArea.y1, rArea.x2, rArea.y2);

	size_t ulSize = (rArea.y2 - rArea.y1 + 1) * (rArea.x2 - rArea.x1 + 1) * sizeof (u16);

	while (ulSize)
	{
		size_t ulBlockSize = ulSize >= MAX_TRANSFER_SIZE ? MAX_TRANSFER_SIZE : ulSize;

		Write (TRUE, pPixels, ulBlockSize);

		pPixels = (const void *) ((uintptr) pPixels + ulBlockSize);

		ulSize -= ulBlockSize;
	}

	if (pRoutine)
	{
		(*pRoutine) (pParam);
	}
}

void CILI9486Display::SetWindow (unsigned x0, unsigned y0, unsigned x1, unsigned y1)
{
	assert (x0 <= x1);
	assert (y0 <= y1);
	assert (x1 < m_nWidth);
	assert (y1 < m_nHeight);

	CommandAndData (ILI9486_COLUMN_ADDRESS_SET, 4, x0 >> 8, x0 & 0xFF, x1 >> 8, x1 & 0xFF);
	CommandAndData (ILI9486_PAGE_ADDRESS_SET, 4, y0 >> 8, y0 & 0xFF, y1 >> 8, y1 & 0xFF);

	Command (ILI9486_MEMORY_WRITE);
}

// Each command and parameter byte is sent as a 16-bit big endian word
void CILI9486Display::CommandAndData (u8 uchCmd, unsigned nDataLen, ...)
{
	u8 Cmd[2] = {0x00, uchCmd};
	Write (FALSE, Cmd, sizeof Cmd);

	if (nDataLen == 0)
	{
		return;
	}

	va_list var;
	va_start (var, nDataLen);

	u8 Buffer[nDataLen * 2];
	for (unsigned i = 0; i < nDataLen; i++)
	{
		Buffer[i*2] = 0x00;
		Buffer[i*2 + 1] = (u8) va_arg (var, int);
	}

	va_end (var);

	Write (TRUE, Buffer, sizeof Buffer);
}

void CILI9486Display::Write (boolean bIsData, const void *pData, size_t nLength)
{
	assert (pData != 0);
	assert (nLength > 0);
	assert (nLength % 2 == 0);		// shift registers need whole 16-bit words
	assert (nLength <= MAX_TRANSFER_SIZE);
	assert (m_pSPIMaster != 0);

	m_DCPin.Write (bIsData ? HIGH : LOW);

	m_pSPIMaster->SetClock (m_nClockSpeed);
	m_pSPIMaster->SetMode (0, 0);

#ifndef NDEBUG
	int nResult =
#endif
		m_pSPIMaster->Write (m_nChipSelect, pData, nLength);
	assert (nResult == (int) nLength);
}
