//
// st7789dma.cpp
//
#include "st7789dma.h"
#include <circle/timer.h>
#include <circle/util.h>
#include <assert.h>

#define ST7789_SWRESET	0x01
#define ST7789_SLPOUT	0x11
#define ST7789_INVOFF	0x20
#define ST7789_DISPON	0x29
#define ST7789_GSCAN	0x45
#define ST7789_CASET	0x2A
#define ST7789_RASET	0x2B
#define ST7789_RAMWR	0x2C
#define ST7789_MADCTL	0x36
#define ST7789_COLMOD	0x3A
#define ST7789_RAMCTRL	0xB0
#define ST7789_FRMCTR2	0xB2
#define ST7789_GCTRL	0xB7
#define ST7789_VCOMS	0xBB
#define ST7789_LCMCTRL	0xC0
#define ST7789_VDVVRHEN	0xC2
#define ST7789_VRHS	0xC3
#define ST7789_VDVS	0xC4
#define ST7789_FRCTRL2	0xC6
#define ST7789_PWCTRL1	0xD0
#define ST7789_GMCTRP1	0xE0
#define ST7789_GMCTRN1	0xE1

#define MAX_CHUNK	0xFFFC		// SPI DLEN is 16 bits; keep chunks 4-byte aligned

CST7789DMADisplay::CST7789DMADisplay (CInterruptSystem *pInterrupt,
				      unsigned nDCPin, unsigned nResetPin,
				      unsigned nWidth, unsigned nHeight,
				      unsigned nClockSpeed, unsigned nChipSelect,
				      boolean bLittleEndian)
:	CDisplay (bLittleEndian ? RGB565 : RGB565_BE),
	m_SPI (pInterrupt, nClockSpeed, 0, 0, FALSE),	// normal DMA channels
	m_DCPin (nDCPin, GPIOModeOutput),
	m_nResetPin (nResetPin),
	m_nWidth (nWidth),
	m_nHeight (nHeight),
	m_nChipSelect (nChipSelect),
	m_nClockSpeed (nClockSpeed),
	m_bLittleEndian (bLittleEndian),
	m_nAuxChipSelect (0),
	m_nAuxClockSpeed (0),
	m_nAuxBytes (0),
	m_pAuxTx (nullptr),
	m_pAuxRx (nullptr),
	m_pAuxRoutine (nullptr),
	m_pAuxParam (nullptr),
	m_pDummyRx (nullptr),
	m_nFree (0),
	m_pNext (nullptr),
	m_nRemaining (0),
	m_bBusy (FALSE),
	m_pRoutine (nullptr),
	m_pParam (nullptr)
{
	if (m_nResetPin != None)
	{
		m_ResetPin.AssignPin (m_nResetPin);
		m_ResetPin.SetMode (GPIOModeOutput, FALSE);
	}
}

CST7789DMADisplay::~CST7789DMADisplay (void)
{
}

boolean CST7789DMADisplay::Initialize (void)
{
	u8 *p = new u8[MAX_CHUNK + 64];
	m_pDummyRx = (u8 *) (((uintptr) p + 63) & ~(uintptr) 63);
	p = new u8[2 * MaxAux + 64];			// (whole cache lines each)
	m_pAuxTx = (u8 *) (((uintptr) p + 63) & ~(uintptr) 63);
	m_pAuxRx = m_pAuxTx + MaxAux;
	for (unsigned i = 0; i < 2; i++)
	{
		p = new u8[m_nWidth * m_nHeight * sizeof (u16) + 64];
		m_pTurned[i] = (u16 *) (((uintptr) p + 63) & ~(uintptr) 63);
	}

	if (!m_SPI.Initialize ())
	{
		return FALSE;
	}

	if (m_nResetPin != None)
	{
		m_ResetPin.Write (HIGH);
		CTimer::SimpleMsDelay (50);
		m_ResetPin.Write (LOW);
		CTimer::SimpleMsDelay (50);
		m_ResetPin.Write (HIGH);
		CTimer::SimpleMsDelay (50);
	}

	// init sequence as in Circle's CST7789Display (known to work on this panel)
	Command (ST7789_SWRESET);
	CTimer::SimpleMsDelay (150);

	// the panel's own order: 240 across, 320 down, refreshed top to bottom.
	// Frames come in landscape (m_nWidth x m_nHeight) and are turned into it
	// (Turn): written along the panel's scan, not across it as with MV
	Command (ST7789_MADCTL);	Data8 (0x00);
	static const u8 Porch[] = {0x0C, 0x0C, 0x00, 0x33, 0x33};
	Command (ST7789_FRMCTR2);	Data (Porch, sizeof Porch);
	Command (ST7789_COLMOD);	Data8 (0x05);		// 16 bpp
	// RAMCTRL: bit 3 of the second byte selects little endian RGB565 input
	const u8 RamCtrl[] = {0x00, (u8) (m_bLittleEndian ? 0xF8 : 0xF0)};
	Command (ST7789_RAMCTRL);	Data (RamCtrl, sizeof RamCtrl);
	Command (ST7789_GCTRL);		Data8 (0x14);
	Command (ST7789_VCOMS);		Data8 (0x37);
	Command (ST7789_LCMCTRL);	Data8 (0x2C);
	Command (ST7789_VDVVRHEN);	Data8 (0x01);
	Command (ST7789_VRHS);		Data8 (0x12);
	Command (ST7789_VDVS);		Data8 (0x20);
	static const u8 PwCtrl[] = {0xA4, 0xA1};
	Command (ST7789_PWCTRL1);	Data (PwCtrl, sizeof PwCtrl);
	Command (ST7789_FRCTRL2);	Data8 (0x0F);		// 60 Hz panel refresh
	static const u8 GammaP[] = {0xD0, 0x04, 0x0D, 0x11, 0x13, 0x2B, 0x3F,
				    0x54, 0x4C, 0x18, 0x0D, 0x0B, 0x1F, 0x23};
	Command (ST7789_GMCTRP1);	Data (GammaP, sizeof GammaP);
	static const u8 GammaN[] = {0xD0, 0x04, 0x0C, 0x11, 0x13, 0x2C, 0x3F,
				    0x44, 0x51, 0x2F, 0x1F, 0x1F, 0x20, 0x23};
	Command (ST7789_GMCTRN1);	Data (GammaN, sizeof GammaN);
	Command (ST7789_INVOFF);	// this panel is not inverted (Circle sends INVON)
	Command (ST7789_SLPOUT);
	Command (ST7789_DISPON);
	CTimer::SimpleMsDelay (100);

	Clear ();

	return TRUE;
}

void CST7789DMADisplay::Clear (TRawColor nColor)
{
	WaitIdle ();

	u16 Line[m_nHeight];				// (a line of the panel's: the landscape's height)
	for (unsigned x = 0; x < m_nHeight; x++)
	{
		Line[x] = (u16) nColor;
	}

	SetWindow (0, 0, m_nHeight-1, m_nWidth-1);
	for (unsigned y = 0; y < m_nWidth; y++)
	{
		Data (Line, sizeof Line);
	}
}

void CST7789DMADisplay::SetPixel (unsigned nPosX, unsigned nPosY, TRawColor nColor)
{
	WaitIdle ();

	SetWindow (m_nHeight-1 - nPosY, nPosX, m_nHeight-1 - nPosY, nPosX);
	u16 usColor = (u16) nColor;
	Data (&usColor, sizeof usColor);
}

// A landscape area (nWidth x nHeight pixels) into the panel's order (nHeight
// across, nWidth down): the panel's column i is the landscape's row nHeight -
// 1 - i, its row j the landscape's column j (as MADCTL's MX | MV showed it).
// Eight landscape columns at a time: eight panel rows written along, the
// landscape read a cache line a row
void CST7789DMADisplay::Turn (const u16 *pFrom, unsigned nWidth, unsigned nHeight, u16 *pTo)
{
	for (unsigned j = 0; j < nWidth; j += 8)
	{
		unsigned nColumns = nWidth - j < 8 ? nWidth - j : 8;
		const u16 *pSource = pFrom + (nHeight - 1) * nWidth + j;
		u16 *pRows = pTo + j * nHeight;
		for (unsigned i = 0; i < nHeight; i++, pSource -= nWidth, pRows++)
		{
			for (unsigned k = 0; k < nColumns; k++)
			{
				pRows[k * nHeight] = pSource[k];
			}
		}
	}
}

// the landscape area's pixels, turned and in the free buffer: out by DMA
void CST7789DMADisplay::Send (const TArea &rArea, TAreaCompletionRoutine *pRoutine, void *pParam)
{
	SetWindow (m_nHeight-1 - rArea.y2, rArea.x1, m_nHeight-1 - rArea.y1, rArea.x2);
	m_DCPin.Write (HIGH);

	m_pNext = (const u8 *) m_pTurned[m_nFree];
	m_nRemaining = (rArea.x2 - rArea.x1 + 1) * (rArea.y2 - rArea.y1 + 1) * sizeof (u16);
	m_pRoutine = pRoutine;
	m_pParam = pParam;
	m_bBusy = TRUE;
	m_nFree ^= 1;

	StartChunk ();

	if (!pRoutine)
	{
		WaitIdle ();
	}
}

void CST7789DMADisplay::SetArea (const TArea &rArea, const void *pPixels,
				 TAreaCompletionRoutine *pRoutine, void *pParam)
{
	assert (pPixels != 0);
	assert (rArea.x1 <= rArea.x2 && rArea.x2 < m_nWidth);
	assert (rArea.y1 <= rArea.y2 && rArea.y2 < m_nHeight);

	WaitIdle ();

	Turn ((const u16 *) pPixels, rArea.x2 - rArea.x1 + 1, rArea.y2 - rArea.y1 + 1, m_pTurned[m_nFree]);
	Send (rArea, pRoutine, pParam);
}

void CST7789DMADisplay::PrepareFrame (const void *pPixels)
{
	Turn ((const u16 *) pPixels, m_nWidth, m_nHeight, m_pTurned[m_nFree]);
}

// The frame starts while the panel's scan is between SyncFirst and SyncLast:
// past the top (the scan stays ahead of what's written: it's a little faster,
// 320 lines in about 15.5 ms to our 16.4), and not so late that its next pass
// would catch the writing up before the bottom. So each refresh shows one
// whole frame
void CST7789DMADisplay::ShowFrame (TAreaCompletionRoutine *pRoutine, void *pParam)
{
	WaitIdle ();

	unsigned nStart = CTimer::GetClockTicks (), nLine;
	do
	{
		nLine = GetScanLine ();
	}
	while ((nLine < SyncFirst || nLine > SyncLast) && CTimer::GetClockTicks () - nStart < 20000);

	const TArea Full = {0, m_nWidth-1, 0, m_nHeight-1};
	Send (Full, pRoutine, pParam);
}

// GSCAN: read at 4 MHz (the panel reads slowly), a dummy bit before its 10 bits
unsigned CST7789DMADisplay::GetScanLine (void)
{
	static const u8 Tx[4] = {ST7789_GSCAN, 0, 0, 0};
	u8 Rx[4];
	m_DCPin.Write (LOW);
	m_SPI.SetClock (4000000);
	m_SPI.WriteReadSync (m_nChipSelect, Tx, Rx, sizeof Rx);
	m_SPI.SetClock (m_nClockSpeed);

	return ((Rx[1] << 8 | Rx[2]) << 1 | Rx[3] >> 7) & 0x3FF;
}

void CST7789DMADisplay::WaitIdle (void)
{
	while (m_bBusy)
	{
		// DMA completion interrupts do the work
	}
}

void CST7789DMADisplay::StartChunk (void)
{
	size_t nChunk = m_nRemaining > MAX_CHUNK ? MAX_CHUNK : m_nRemaining;
	const u8 *pChunk = m_pNext;

	m_pNext = pChunk + nChunk;
	m_nRemaining -= nChunk;

	m_SPI.SetCompletionRoutine (SPICompletion, this);
	m_SPI.StartWriteRead (m_nChipSelect, pChunk, m_pDummyRx, nChunk);
}

void CST7789DMADisplay::SPICompletion (boolean bStatus, void *pParam)
{
	CST7789DMADisplay *pThis = static_cast<CST7789DMADisplay *> (pParam);
	assert (pThis != 0);

	if (pThis->m_nRemaining > 0)
	{
		pThis->StartChunk ();

		return;
	}

	if (pThis->m_nAuxBytes)			// the other device's turn, then the frame is done
	{
		pThis->m_SPI.SetClock (pThis->m_nAuxClockSpeed);
		pThis->m_SPI.SetCompletionRoutine (AuxCompletion, pThis);
		pThis->m_SPI.StartWriteRead (pThis->m_nAuxChipSelect, pThis->m_pAuxTx, pThis->m_pAuxRx,
					     pThis->m_nAuxBytes);

		return;
	}

	pThis->FrameDone ();
}

void CST7789DMADisplay::AuxCompletion (boolean bStatus, void *pParam)
{
	CST7789DMADisplay *pThis = static_cast<CST7789DMADisplay *> (pParam);
	assert (pThis != 0);

	pThis->m_SPI.SetClock (pThis->m_nClockSpeed);
	if (bStatus && pThis->m_pAuxRoutine)
	{
		(*pThis->m_pAuxRoutine) (pThis->m_pAuxRx, pThis->m_pAuxParam);
	}

	pThis->FrameDone ();
}

void CST7789DMADisplay::FrameDone (void)
{
	TAreaCompletionRoutine *pRoutine = m_pRoutine;
	void *pRoutineParam = m_pParam;
	m_pRoutine = nullptr;

	m_bBusy = FALSE;

	if (pRoutine)
	{
		(*pRoutine) (pRoutineParam);
	}
}

void CST7789DMADisplay::SetAux (unsigned nChipSelect, unsigned nClockSpeed, const void *pTx, unsigned nBytes,
				TAuxRoutine *pRoutine, void *pParam)
{
	assert (nBytes <= MaxAux);
	assert (m_pAuxTx != nullptr);			// (after Initialize)
	WaitIdle ();

	m_nAuxBytes = 0;
	m_nAuxChipSelect = nChipSelect;
	m_nAuxClockSpeed = nClockSpeed;
	memcpy (m_pAuxTx, pTx, nBytes);
	m_pAuxRoutine = pRoutine;
	m_pAuxParam = pParam;
	m_nAuxBytes = nBytes;
}

boolean CST7789DMADisplay::AuxNow (void)
{
	if (m_bBusy || !m_nAuxBytes)
	{
		return FALSE;
	}

	Transfer (m_nAuxChipSelect, m_nAuxClockSpeed, m_pAuxTx, m_pAuxRx, m_nAuxBytes);
	if (m_pAuxRoutine)
	{
		(*m_pAuxRoutine) (m_pAuxRx, m_pAuxParam);
	}

	return TRUE;
}

void CST7789DMADisplay::Transfer (unsigned nChipSelect, unsigned nClockSpeed, const void *pTx, void *pRx, unsigned nBytes)
{
	WaitIdle ();

	m_SPI.SetClock (nClockSpeed);
	m_SPI.WriteReadSync (nChipSelect, pTx, pRx, nBytes);
	m_SPI.SetClock (m_nClockSpeed);
}

void CST7789DMADisplay::SetWindow (unsigned x0, unsigned y0, unsigned x1, unsigned y1)
{
	assert (x0 <= x1 && x1 < m_nHeight);		// (the panel's: 240 across, 320 down)
	assert (y0 <= y1 && y1 < m_nWidth);

	u8 Column[4] = {(u8) (x0 >> 8), (u8) x0, (u8) (x1 >> 8), (u8) x1};
	Command (ST7789_CASET);
	Data (Column, sizeof Column);

	u8 Row[4] = {(u8) (y0 >> 8), (u8) y0, (u8) (y1 >> 8), (u8) y1};
	Command (ST7789_RASET);
	Data (Row, sizeof Row);

	Command (ST7789_RAMWR);
}

void CST7789DMADisplay::Command (u8 uchCmd)
{
	m_DCPin.Write (LOW);
	m_SPI.WriteReadSync (m_nChipSelect, &uchCmd, nullptr, 1);
}

void CST7789DMADisplay::Data (const void *pData, size_t nLength)
{
	m_DCPin.Write (HIGH);

	const u8 *p = (const u8 *) pData;
	while (nLength)
	{
		size_t nChunk = nLength > MAX_CHUNK ? MAX_CHUNK : nLength;
		m_SPI.WriteReadSync (m_nChipSelect, p, nullptr, nChunk);
		p += nChunk;
		nLength -= nChunk;
	}
}
