//
// hdmiprobe: what can the RPi see of an HDMI monitor being plugged in
// and out, and of the ST7789 panel being there or not? Polls the HPD pin
// (GPIO46), the firmware's EDID and display tags and logs every change;
// probes the panel's lines with the pull resistors and tries to read its ID.
//
#include "kernel.h"
#include <circle/bcm2835.h>
#include <circle/bcmpropertytags.h>
#include <circle/memio.h>
#include <circle/string.h>
#include <circle/util.h>

#define WIDTH		m_nWidth
#define HEIGHT		m_nHeight

#define HPD_PIN		46
#define DC_PIN		24
#define RESET_PIN	25

#define PROPTAG_GET_DISPLAY_SETTINGS	0x00040014

LOGMODULE ("hdmiprobe");

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
	return (read32 (nPin < 32 ? ARM_GPIO_GPLEV0 : ARM_GPIO_GPLEV0 + 4) >> (nPin % 32)) & 1;
}

static void SetLevel (unsigned nPin, unsigned nLevel)
{
	write32 ((nLevel ? ARM_GPIO_GPSET0 : ARM_GPIO_GPCLR0) + nPin / 32 * 4, 1 << (nPin % 32));
}

static void SetPull (unsigned nPin, unsigned nPull)	// 0 off, 1 down, 2 up
{
	write32 (ARM_GPIO_GPPUD, nPull);
	CTimer::SimpleusDelay (5);
	write32 (ARM_GPIO_GPPUDCLK0, 1 << nPin);
	CTimer::SimpleusDelay (5);
	write32 (ARM_GPIO_GPPUD, 0);
	write32 (ARM_GPIO_GPPUDCLK0, 0);
	CTimer::SimpleMsDelay (5);
}

static void DescribeEDID (const u8 *b, CString *pOut)
{
	static const u8 Header[8] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
	u8 uchSum = 0;
	for (unsigned i = 0; i < 128; i++) uchSum += b[i];
	const u8 *d = b + 54;	// first detailed timing: the preferred mode
	unsigned nClock = (d[0] | d[1] << 8) * 10;	// kHz
	unsigned nH = d[2] | (d[4] & 0xF0) << 4;
	unsigned nV = d[5] | (d[7] & 0xF0) << 4;
	unsigned nHTotal = nH + (d[3] | (d[4] & 0x0F) << 8);
	unsigned nVTotal = nV + (d[6] | (d[7] & 0x0F) << 8);
	unsigned nHz = nHTotal && nVTotal ? nClock * 1000 / (nHTotal * nVTotal) : 0;
	char Name[14] = "";
	for (unsigned n = 0; n < 4; n++)
	{
		const u8 *p = b + 54 + 18 * n;
		if (p[0] == 0 && p[1] == 0 && p[3] == 0xFC)
		{
		for (unsigned i = 0; i < 13 && p[5 + i] != 0x0A; i++) Name[i] = p[5 + i];
		}
	}
	pOut->Format (" edid=ok hdr=%s sum=%s mfg=%02X%02X ext=%u pref=%ux%u@%u \"%s\"",
		  memcmp (b, Header, 8) == 0 ? "ok" : "bad", uchSum == 0 ? "ok" : "bad",
		  b[8], b[9], b[126], nH, nV, nHz, Name);
}

// Read EDID block 0 from the monitor over the HDMI DDC bus (BSC2, 0x50) directly
static int ReadDDC (u8 *pBlock)
{
	const uintptr Base = ARM_IO_BASE + 0x805000;
	enum { C = 0x00, S = 0x04, DLEN = 0x08, A = 0x0C, FIFO = 0x10, DIV = 0x14 };
	u32 nDiv = read32 (Base + DIV);
	write32 (Base + DIV, 3000);				// 300 MHz / 3000 = 100 kHz
	write32 (Base + S, 1 << 9 | 1 << 8 | 1 << 1);		// clear CLKT, ERR, DONE
	write32 (Base + A, 0x50);
	write32 (Base + DLEN, 1);
	write32 (Base + C, 1 << 15 | 1 << 4);			// I2CEN, CLEAR
	write32 (Base + FIFO, 0);				// offset 0
	write32 (Base + C, 1 << 15 | 1 << 7);			// I2CEN, ST (write)
	unsigned nTimeout = 100000;
	while (!(read32 (Base + S) & 1 << 1) && --nTimeout) CTimer::SimpleusDelay (1);
	u32 nStatus = read32 (Base + S);
	int nResult = 0;
	if (!nTimeout) nResult = -1;
	else if (nStatus & 1 << 8) nResult = -2;		// NACK
	else
	{
		write32 (Base + S, 1 << 9 | 1 << 8 | 1 << 1);
		write32 (Base + DLEN, 128);
		write32 (Base + C, 1 << 15 | 1 << 7 | 1 << 4 | 1);	// I2CEN, ST, CLEAR, READ
		unsigned n = 0;
		nTimeout = 200000;
		while (n < 128 && --nTimeout)
		{
			u32 st = read32 (Base + S);
			if (st & 1 << 5) pBlock[n++] = read32 (Base + FIFO);	// RXD
			else if (st & (1 << 8 | 1 << 9)) break;
			else CTimer::SimpleusDelay (1);
		}
		nStatus = read32 (Base + S);
		nResult = n == 128 ? 128 : (nStatus & 1 << 8) ? -3 : -4;
	}
	write32 (Base + S, 1 << 9 | 1 << 8 | 1 << 1);
	write32 (Base + C, 1 << 15);
	write32 (Base + DIV, nDiv);
	return nResult;
}

CKernel::CKernel (void)
:	m_Timer (&m_Interrupt),
	m_Logger (m_Options.GetLogLevel (), &m_Timer),
	m_DevLink (&m_Interrupt),
	m_pFrameBuffer (nullptr),
	m_nWidth (320),
	m_nHeight (240)
{
	m_Last[0] = '\0';
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

void CKernel::ProbePanel (void)
{
	static const unsigned Pins[] = {8, 9, 10, 11, DC_PIN, RESET_PIN};
	for (unsigned nPin : Pins)
	{
		unsigned nFunction = GetFunction (nPin);
		SetFunction (nPin, 0);
		SetPull (nPin, 1);
		unsigned nDown = GetLevel (nPin);
		SetPull (nPin, 2);
		unsigned nUp = GetLevel (nPin);
		SetPull (nPin, 0);
		unsigned nOff = GetLevel (nPin);
		SetFunction (nPin, nFunction);
		LOGNOTE ("panel pin GPIO%u: pull-down %u, pull-up %u, no pull %u (fsel %u)",
			 nPin, nDown, nUp, nOff, nFunction);
	}

	// reset the panel, then try to read its ID (RDDID 04h) and status (RDDST 09h)
	SetFunction (RESET_PIN, 1);
	SetLevel (RESET_PIN, 0);
	CTimer::SimpleMsDelay (20);
	SetLevel (RESET_PIN, 1);
	CTimer::SimpleMsDelay (150);
	SetFunction (DC_PIN, 1);
	SetLevel (DC_PIN, 0);

	CSPIMaster SPI (1000000, 0, 0);
	if (!SPI.Initialize ())
	{
		LOGWARN ("SPI init failed");
		return;
	}
	for (unsigned nPull = 1; nPull <= 2; nPull++)
	{
		SetPull (9, nPull);
		static const u8 Cmds[] = {0x04, 0x09, 0xDA, 0xDB, 0xDC};
		for (u8 uchCmd : Cmds)
		{
			u8 Tx[6] = {uchCmd}, Rx[6] = {0};
			SPI.WriteRead (0, Tx, Rx, sizeof Tx);
			LOGNOTE ("MISO pull-%s: cmd %02X -> %02X %02X %02X %02X %02X",
				 nPull == 1 ? "down" : "up  ", uchCmd, Rx[1], Rx[2], Rx[3], Rx[4], Rx[5]);
		}
	}
	SetPull (9, 0);
}

void CKernel::Poll (boolean bForce)
{
	CString Line;
	CBcmPropertyTags Tags;

	Line.Format ("HPD(GPIO46)=%u fsel=%u", GetLevel (HPD_PIN), GetFunction (HPD_PIN));

	TPropertyTagSimple Num;
	if (Tags.GetTag (PROPTAG_GET_NUM_DISPLAYS, &Num, sizeof Num))
	{
		CString s; s.Format (" displays=%u", Num.nValue); Line.Append (s);
	}
	else
	{
		Line.Append (" displays=?");
	}

	TPropertyTagDisplayDimensions Dim;
	if (Tags.GetTag (PROPTAG_GET_DISPLAY_DIMENSIONS, &Dim, sizeof Dim))
	{
		CString s; s.Format (" dims=%ux%u", Dim.nWidth, Dim.nHeight); Line.Append (s);
	}
	else
	{
		Line.Append (" dims=?");
	}

	struct { TPropertyTag Tag; u32 Words[13]; } PACKED Settings;
	memset (Settings.Words, 0, sizeof Settings.Words);
	if (Tags.GetTag (PROPTAG_GET_DISPLAY_SETTINGS, &Settings, sizeof Settings, 4))
	{
		CString s; s.Format (" settings(len %u)=", Settings.Tag.nValueLength & ~(1U << 31));
		Line.Append (s);
		for (unsigned i = 0; i < 8; i++)
		{
			s.Format ("%u%s", Settings.Words[i], i < 7 ? "," : "");
			Line.Append (s);
		}
	}
	else
	{
		Line.Append (" settings=?");
	}

	// the HDMI pixel valve's timing (PV2, as in Linux vc4_regs.h)
	{
		const uintptr PV = ARM_IO_BASE + 0x807000;
		u32 c = read32 (PV + 0x00), ha = read32 (PV + 0x0C), hb = read32 (PV + 0x10);
		u32 va = read32 (PV + 0x14), vb = read32 (PV + 0x18);
		unsigned nHAct = hb & 0xFFFF, nVAct = vb & 0xFFFF;
		unsigned nHTot = nHAct + (hb >> 16) + (ha >> 16) + (ha & 0xFFFF);
		unsigned nVTot = nVAct + (vb >> 16) + (va >> 16) + (va & 0xFFFF);
		CString t; t.Format (" pv2=%ux%u total %ux%u ctl %X", nHAct, nVAct, nHTot, nVTot, c);
		Line.Append (t);
	}

	struct { TPropertyTag Tag; u32 Top, Bottom, Left, Right; } PACKED Overscan;
	if (Tags.GetTag (0x0004000A, &Overscan, sizeof Overscan))
	{
		CString t; t.Format (" overscan=%u,%u,%u,%u", Overscan.Top, Overscan.Bottom, Overscan.Left, Overscan.Right);
		Line.Append (t);
	}

	TPropertyTagEDIDBlock EDID;
	EDID.nBlockNumber = EDID_FIRST_BLOCK;
	EDID.nStatus = 0x12345678;
	u64 nStart = CTimer::GetClockTicks64 ();
	boolean bEDID = Tags.GetTag (PROPTAG_GET_EDID_BLOCK, &EDID, sizeof EDID, 4);
	unsigned nEDIDus = (unsigned) (CTimer::GetClockTicks64 () - nStart);
	CString s;
	if (!bEDID)
	{
		s.Format (" edid=fail");
	}
	else if (EDID.nStatus != EDID_STATUS_SUCCESS)
	{
		s.Format (" edid=status %08X len %u", EDID.nStatus, EDID.Tag.nValueLength & ~(1U << 31));
	}
	else
	{
		DescribeEDID (EDID.Block, &s);
	}
	Line.Append (s);

	if (bForce)
	{
		u8 Block[128];
		u64 nStart = CTimer::GetClockTicks64 ();
		int nDDC = ReadDDC (Block);
		unsigned nUs = (unsigned) (CTimer::GetClockTicks64 () - nStart);
		CString d;
		if (nDDC == 128) DescribeEDID (Block, &d);
		else d.Format (" error %d", nDDC);
		LOGNOTE ("direct DDC read:%s (%u us)", (const char *) d, nUs);

		if (m_pFrameBuffer)
		{
			nStart = CTimer::GetClockTicks64 ();
			for (unsigned i = 0; i < 30; i++) m_pFrameBuffer->WaitForVerticalSync ();
			unsigned nVsyncUs = (unsigned) (CTimer::GetClockTicks64 () - nStart);
			LOGNOTE ("vsync: 30 in %u us = %u.%02u Hz", nVsyncUs, 30000000 / nVsyncUs,
				 3000000000U / nVsyncUs % 100);
		}
	}

	if (bForce || strcmp (Line, m_Last) != 0)
	{
		LOGNOTE ("%s (edid call %u us)", (const char *) Line, nEDIDus);
		strncpy (m_Last, Line, sizeof m_Last - 1);
	}
}

void CKernel::Fill (unsigned nFrame)
{
	static const u16 Bars[8] = {0xFFFF, 0xFFE0, 0x07FF, 0x07E0, 0xF81F, 0xF800, 0x001F, 0x0000};
	u16 *p = (u16 *) (uintptr) m_pFrameBuffer->GetBuffer ();
	unsigned nPitch = m_pFrameBuffer->GetPitch () / 2;
	unsigned nBox = nFrame * 4 % (WIDTH - 40);
	for (unsigned y = 0; y < HEIGHT; y++)
	{
		for (unsigned x = 0; x < WIDTH; x++)
		{
			u16 c = Bars[x * 8 / WIDTH];
			if (y >= 100 && y < 140 && x >= nBox && x < nBox + 40) c = (nFrame & 16) ? 0xF800 : 0x001F;
			if (x == 0 || y == 0 || x == WIDTH - 1 || y == HEIGHT - 1) c = 0xF800;
			p[y * nPitch + x] = c;
		}
	}
}

TShutdownMode CKernel::Run (void)
{
	LOGNOTE ("hdmiprobe built " __DATE__ " " __TIME__);

	ProbePanel ();
	Poll (TRUE);

	m_pFrameBuffer = new CBcmFrameBuffer (WIDTH, HEIGHT, 16);
	boolean bFB = m_pFrameBuffer->Initialize ();
	LOGNOTE ("framebuffer %ux%u init %s", WIDTH, HEIGHT, bFB ? "ok" : "FAILED");
	Poll (TRUE);

	static const unsigned Sizes[][2] = {{320, 240}, {512, 300}, {640, 360}};
	unsigned nSize = 0, nLastResize = CTimer::GetClockTicks ();
	unsigned nFrame = 0;
	unsigned nLastForce = CTimer::GetClockTicks ();
	while (1)
	{
		m_DevLink.Update ();
		if (bFB && CTimer::GetClockTicks () - nLastResize > 15 * CLOCKHZ)
		{
			nLastResize = CTimer::GetClockTicks ();
			nSize = (nSize + 1) % 3;
			delete m_pFrameBuffer;
			m_nWidth = Sizes[nSize][0];
			m_nHeight = Sizes[nSize][1];
			m_pFrameBuffer = new CBcmFrameBuffer (m_nWidth, m_nHeight, 16);
			bFB = m_pFrameBuffer->Initialize ();
			LOGNOTE ("framebuffer resized to %ux%u: %s, buffer %08X pitch %u", m_nWidth, m_nHeight,
				 bFB ? "ok" : "FAILED", bFB ? m_pFrameBuffer->GetBuffer () : 0,
				 bFB ? m_pFrameBuffer->GetPitch () : 0);
		}
		if (bFB)
		{
			Fill (nFrame++);
		}
		boolean bForce = CTimer::GetClockTicks () - nLastForce > 10 * CLOCKHZ;
		if (bForce)
		{
			nLastForce = CTimer::GetClockTicks ();
		}
		Poll (bForce);
		CTimer::SimpleMsDelay (50);
	}

	return ShutdownHalt;
}
