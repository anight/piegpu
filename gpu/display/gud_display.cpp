//
// gud_display.cpp
//
// The requests' formats and what the PC's driver does with the answers:
// include/drm/gud.h and drivers/gpu/drm/gud/gud_{drv,connector,pipe}.c.
//
#include "gud_display.h"
#include <circle/logger.h>
#include <circle/synchronize.h>
#include <circle/timer.h>
#include <circle/util.h>
#include <assert.h>

LOGMODULE ("gud");

// include/drm/gud.h
#define GUD_DISPLAY_MAGIC			0x1d50614d
#define GUD_DISPLAY_FLAG_STATUS_ON_SET		(1 << 0)

#define GUD_REQ_GET_STATUS			0x00
#define GUD_STATUS_OK				0x00
#define GUD_STATUS_REQUEST_NOT_SUPPORTED	0x02
#define GUD_STATUS_INVALID_PARAMETER		0x04
#define GUD_REQ_GET_DESCRIPTOR			0x01
#define GUD_REQ_GET_FORMATS			0x40
#define GUD_PIXEL_FORMAT_RGB565			0x40
#define GUD_REQ_GET_PROPERTIES			0x41
#define GUD_REQ_GET_CONNECTORS			0x50
#define GUD_CONNECTOR_TYPE_PANEL		0
#define GUD_CONNECTOR_FLAGS_POLL_STATUS		(1 << 0)
#define GUD_REQ_GET_CONNECTOR_PROPERTIES	0x51
#define GUD_REQ_SET_CONNECTOR_FORCE_DETECT	0x53
#define GUD_REQ_GET_CONNECTOR_STATUS		0x54
#define GUD_CONNECTOR_STATUS_DISCONNECTED	0x00
#define GUD_CONNECTOR_STATUS_CONNECTED		0x01
#define GUD_CONNECTOR_STATUS_CHANGED		(1 << 7)
#define GUD_REQ_GET_CONNECTOR_MODES		0x55
#define GUD_REQ_GET_CONNECTOR_EDID		0x56
#define GUD_DISPLAY_MODE_FLAG_PREFERRED		(1 << 10)
#define GUD_REQ_SET_BUFFER			0x60
#define GUD_REQ_SET_STATE_CHECK			0x61
#define GUD_REQ_SET_STATE_COMMIT		0x62
#define GUD_REQ_SET_CONTROLLER_ENABLE		0x63
#define GUD_REQ_SET_DISPLAY_ENABLE		0x64

#define REQUEST_IN				0x80

// the connector says "disconnected" this long after the PC's probe, then
// "connected" (with CHANGED): a real hotplug at the PC's next poll (10 s),
// on which GNOME's mutter builds a monitor for a GPU that came after it
// started (for one added while it runs, it doesn't)
#define CONNECT_DELAY_US			3000000

// a 128-byte EDID 1.4 for the screen now: vendor "PGU" (an unassigned PNP
// ID), product name "pigpu", the board's serial number, one detailed
// timing (the mode below). The size says 16x9 cm: an aspect ratio, not a
// size (EDID 1.4 allows it); mutter then names the monitor by vendor and
// product ("PGU pigpu", meta_monitor_make_display_name), where a real size
// would give "PGU 2.8\"" and none just "PGU".
static void MakeEDID (u8 *p, unsigned w, unsigned h, const char *pSerial)
{
	static const u8 Header[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
	static const u8 Chromaticity[10] = {0xEE, 0x91, 0xA3, 0x54, 0x4C, 0x99, 0x26, 0x0F, 0x50, 0x54};

	memset (p, 0, 128);
	memcpy (p, Header, 8);
	u16 nVendor = ('P' - '@') << 10 | ('G' - '@') << 5 | ('U' - '@');
	p[8] = nVendor >> 8;			// big endian
	p[9] = nVendor;
	p[10] = 1;				// product code
	p[17] = 2026 - 1990;			// year of manufacture
	p[18] = 1;				// EDID 1.4
	p[19] = 4;
	p[20] = 0x80;				// digital input
	p[21] = 16;				// "16 x 9 cm": the aspect ratio
	p[22] = 9;
	p[23] = 120;				// gamma 2.2
	p[24] = 0x02;				// RGB 4:4:4; the first timing is the native one
	memcpy (p + 25, Chromaticity, 10);	// sRGB
	for (unsigned i = 38; i < 54; i++)
	{
		p[i] = 0x01;			// standard timings: unused
	}

	u8 *d = p + 54;				// detailed timing: the mode (GET_CONNECTOR_MODES)
	unsigned hb = 24, vb = 3, nClock = (w + hb) * (h + vb) * 60 / 10000;	// 10 kHz
	d[0] = nClock;
	d[1] = nClock >> 8;
	d[2] = w;
	d[3] = hb;
	d[4] = (w >> 8) << 4 | hb >> 8;
	d[5] = h;
	d[6] = vb;
	d[7] = (h >> 8) << 4 | vb >> 8;
	d[8] = 8;				// hsync offset, width
	d[9] = 8;
	d[10] = 1 << 4 | 1;			// vsync offset, width
	d[12] = 160;				// "160 x 90 mm", as above
	d[13] = 90;
	d[17] = 0x18;				// digital separate sync

	while (pSerial[0] == '0' && pSerial[1])	// (13 characters fit: no leading zeros)
	{
		pSerial++;
	}
	struct { u8 nTag; const char *pText; } Text[2] = {{0xFC, "pigpu"}, {0xFF, pSerial}};
	for (unsigned k = 0; k < 2; k++)	// display name, serial number
	{
		d = p + 72 + k * 18;
		d[3] = Text[k].nTag;
		unsigned i = 0;
		for (; i < 13 && Text[k].pText[i]; i++)
		{
			d[5 + i] = Text[k].pText[i];
		}
		if (i < 13)
		{
			d[5 + i++] = 0x0A;
		}
		for (; i < 13; i++)
		{
			d[5 + i] = ' ';
		}
	}
	p[108 + 3] = 0x10;			// dummy descriptor

	u8 nSum = 0;
	for (unsigned i = 0; i < 127; i++)
	{
		nSum += p[i];
	}
	p[127] = -nSum;
}

static void Put16 (u8 *p, u16 n)	{ p[0] = n; p[1] = n >> 8; }
static void Put32 (u8 *p, u32 n)	{ Put16 (p, n); Put16 (p + 2, n >> 16); }
static u16 Get16 (const u8 *p)		{ return p[0] | p[1] << 8; }
static u32 Get32 (const u8 *p)		{ return Get16 (p) | (u32) Get16 (p + 2) << 16; }

CGUDDisplay::CGUDDisplay (CPiGPUGadget *pGadget)
:	m_pGadget (pGadget),
	m_SpinLock (IRQ_LEVEL),
	m_nScreenWidth (MinWidth),
	m_nScreenHeight (MinHeight),
	m_bChanged (FALSE),
	m_uchStatus (GUD_STATUS_OK),
	m_nCheckedWidth (0),
	m_nCheckedHeight (0),
	m_nWidth (0),
	m_nHeight (0),
	m_bControllerOn (FALSE),
	m_bDisplayOn (FALSE),
	m_pBandMemory (nullptr),
	m_nReceiving (-1),
	m_bPending (FALSE),
	m_nNextBand (0),
	m_pOwnPicture (nullptr),
	m_pPicture (nullptr),
	m_nPictureWidth (0),
	m_nPictureHeight (0),
	m_pShownOn (nullptr),
	m_bDirty (FALSE),
	m_bWasActive (FALSE),
	m_nUpdates (0),
	m_nBytes (0),
	m_nLastReport (0),
	m_nProbeTicks (0)
{
}

CGUDDisplay::~CGUDDisplay (void)
{
	delete [] m_pBandMemory;
	delete [] m_pOwnPicture;
}

boolean CGUDDisplay::Initialize (void)
{
	// the receive buffers: cache-line aligned (the USB DMA cleans and
	// invalidates their lines)
	m_pBandMemory = new u8[2 * BandBytes + 64];
	m_pOwnPicture = new u16[MaxWidth * MaxHeight];
	if (!m_pBandMemory || !m_pOwnPicture)
	{
		return FALSE;
	}
	u8 *p = (u8 *) (((uintptr) m_pBandMemory + 63) & ~(uintptr) 63);
	for (unsigned i = 0; i < 2; i++)
	{
		m_Band[i].pData = p + i * BandBytes;
		m_Band[i].bFull = FALSE;
	}
	m_pGadget->SetFunction (this);

	return TRUE;
}

void CGUDDisplay::SetScreen (unsigned nWidth, unsigned nHeight)
{
	if (nWidth != m_nScreenWidth || nHeight != m_nScreenHeight)
	{
		m_nScreenWidth = nWidth;
		m_nScreenHeight = nHeight;
		m_bChanged = TRUE;			// the PC's next poll (10 s) sees it
	}
}

boolean CGUDDisplay::IsActive (void) const
{
	return    m_bControllerOn && m_bDisplayOn
	       && m_nWidth == m_nScreenWidth && m_nHeight == m_nScreenHeight;
}

// ---- the USB interrupt ----------------------------------------------------------

int CGUDDisplay::OnVendorRequest (const TSetupData *pSetup, u8 *pData, size_t nBufferSize)
{
	if (pSetup->bmRequestType & REQUEST_IN)
	{
		int nLength = GetRequest (pSetup->bRequest, pSetup->wValue, pData, nBufferSize);
		if (pSetup->bRequest != GUD_REQ_GET_STATUS)
		{
			m_uchStatus = nLength < 0 ? GUD_STATUS_REQUEST_NOT_SUPPORTED : GUD_STATUS_OK;
		}
		return nLength;
	}

	m_uchStatus = SetRequest (pSetup->bRequest, pSetup->wValue, pData, pSetup->wLength);
	return 0;
}

int CGUDDisplay::GetRequest (u8 bRequest, u16 wValue, u8 *pData, size_t nBufferSize)
{
	switch (bRequest)
	{
	case GUD_REQ_GET_STATUS:
		pData[0] = m_uchStatus;
		return 1;

	case GUD_REQ_GET_DESCRIPTOR:			// struct gud_display_descriptor_req
		m_nProbeTicks = CTimer::GetClockTicks () | 1;	// the PC's probe
		Put32 (pData, GUD_DISPLAY_MAGIC);
		pData[4] = 1;				// version
		Put32 (pData + 5, GUD_DISPLAY_FLAG_STATUS_ON_SET);
		pData[9] = 0;				// compression: none
		Put32 (pData + 10, BandBytes);		// max_buffer_size
		Put32 (pData + 14, MinWidth);
		Put32 (pData + 18, MaxWidth);
		Put32 (pData + 22, MinHeight);
		Put32 (pData + 26, MaxHeight);
		return 30;

	case GUD_REQ_GET_FORMATS:
		pData[0] = GUD_PIXEL_FORMAT_RGB565;
		return 1;

	case GUD_REQ_GET_PROPERTIES:			// none (an empty answer: a STALL
	case GUD_REQ_GET_CONNECTOR_PROPERTIES:		// would fail the PC's probe)
		return 0;

	case GUD_REQ_GET_CONNECTORS:			// struct gud_connector_descriptor_req
		pData[0] = GUD_CONNECTOR_TYPE_PANEL;
		Put32 (pData + 1, GUD_CONNECTOR_FLAGS_POLL_STATUS);
		return 5;

	case GUD_REQ_GET_CONNECTOR_STATUS:
		if (wValue != 0)
		{
			return -1;
		}
		if (!m_nProbeTicks || CTimer::GetClockTicks () - m_nProbeTicks < CONNECT_DELAY_US)
		{
			pData[0] = GUD_CONNECTOR_STATUS_DISCONNECTED;
			m_bChanged = TRUE;		// (connected later: CHANGED)
			return 1;
		}
		pData[0] = GUD_CONNECTOR_STATUS_CONNECTED | (m_bChanged ? GUD_CONNECTOR_STATUS_CHANGED : 0);
		m_bChanged = FALSE;
		return 1;

	case GUD_REQ_GET_CONNECTOR_EDID:
		if (wValue != 0)
		{
			return -1;
		}
		MakeEDID (pData, m_nScreenWidth, m_nScreenHeight, m_pGadget->GetSerialNumber ());
		return 128;

	case GUD_REQ_GET_CONNECTOR_MODES: {		// struct gud_display_mode_req: the screen
		if (wValue != 0)
		{
			return -1;
		}
		unsigned w = m_nScreenWidth, h = m_nScreenHeight;
		u8 *m = pData;
		Put16 (m + 4, w);			// hdisplay, hsync_start, _end, htotal
		Put16 (m + 6, w + 8);
		Put16 (m + 8, w + 16);
		Put16 (m + 10, w + 24);
		Put16 (m + 12, h);			// vdisplay, vsync_start, _end, vtotal
		Put16 (m + 14, h + 1);
		Put16 (m + 16, h + 2);
		Put16 (m + 18, h + 3);
		Put32 (m, (w + 24) * (h + 3) * 60 / 1000);	// clock (kHz): 60 Hz
		Put32 (m + 20, GUD_DISPLAY_MODE_FLAG_PREFERRED);
		return 24;
		}

	default:					// (also the EDID: modes come from above)
		return -1;
	}
}

u8 CGUDDisplay::SetRequest (u8 bRequest, u16 wValue, const u8 *pData, size_t nLength)
{
	switch (bRequest)
	{
	case GUD_REQ_SET_CONNECTOR_FORCE_DETECT:
		return GUD_STATUS_OK;

	case GUD_REQ_SET_STATE_CHECK: {			// struct gud_state_req
		if (nLength < 26)
		{
			return GUD_STATUS_INVALID_PARAMETER;
		}
		unsigned w = Get16 (pData + 4), h = Get16 (pData + 12);
		if (   pData[24] != GUD_PIXEL_FORMAT_RGB565 || pData[25] != 0
		    || w != m_nScreenWidth || h != m_nScreenHeight)
		{
			return GUD_STATUS_INVALID_PARAMETER;
		}
		m_nCheckedWidth = w;
		m_nCheckedHeight = h;
		return GUD_STATUS_OK;
		}

	case GUD_REQ_SET_STATE_COMMIT:
		m_nWidth = m_nCheckedWidth;
		m_nHeight = m_nCheckedHeight;
		return GUD_STATUS_OK;

	case GUD_REQ_SET_CONTROLLER_ENABLE:
		if (nLength < 1)
		{
			return GUD_STATUS_INVALID_PARAMETER;
		}
		m_bControllerOn = pData[0] != 0;
		return GUD_STATUS_OK;

	case GUD_REQ_SET_DISPLAY_ENABLE:
		if (nLength < 1)
		{
			return GUD_STATUS_INVALID_PARAMETER;
		}
		m_bDisplayOn = pData[0] != 0;
		return GUD_STATUS_OK;

	case GUD_REQ_SET_BUFFER:
		return SetBuffer (pData, nLength);

	default:
		return GUD_STATUS_REQUEST_NOT_SUPPORTED;
	}
}

// struct gud_set_buffer_req: a rectangle whose pixels the bulk transfer brings
u8 CGUDDisplay::SetBuffer (const u8 *pData, size_t nLength)
{
	if (nLength < 25)
	{
		return GUD_STATUS_INVALID_PARAMETER;
	}
	unsigned x = Get32 (pData), y = Get32 (pData + 4);
	unsigned w = Get32 (pData + 8), h = Get32 (pData + 12);
	u32 nBytes = Get32 (pData + 16);
	if (   !w || !h || x + w > m_nWidth || y + h > m_nHeight || nBytes != w * h * 2
	    || nBytes > BandBytes || pData[20] != 0)		// compression
	{
		return GUD_STATUS_INVALID_PARAMETER;
	}

	m_SpinLock.Acquire ();
	m_PendingBand.x = x;
	m_PendingBand.y = y;
	m_PendingBand.nWidth = w;
	m_PendingBand.nHeight = h;
	m_bPending = TRUE;
	StartReceive ();
	m_SpinLock.Release ();

	return GUD_STATUS_OK;
}

// (with m_SpinLock held)
boolean CGUDDisplay::StartReceive (void)
{
	if (!m_bPending || m_nReceiving >= 0)
	{
		return FALSE;
	}
	unsigned i = m_nNextBand;
	if (m_Band[i].bFull)
	{
		return FALSE;				// (bands are drawn in order)
	}
	TBand &B = m_Band[i];
	B.x = m_PendingBand.x;
	B.y = m_PendingBand.y;
	B.nWidth = m_PendingBand.nWidth;
	B.nHeight = m_PendingBand.nHeight;
	m_bPending = FALSE;
	m_nReceiving = i;
	if (!m_pGadget->ReceiveBulk (B.pData, B.nWidth * B.nHeight * 2))
	{
		m_nReceiving = -1;
		return FALSE;
	}

	return TRUE;
}

void CGUDDisplay::OnBulkReceived (size_t nLength)
{
	m_SpinLock.Acquire ();
	int i = m_nReceiving;
	if (i >= 0)
	{
		TBand &B = m_Band[i];
		if (nLength == B.nWidth * B.nHeight * 2)
		{
			B.bFull = TRUE;
			m_nNextBand = i ^ 1;
		}
		m_nReceiving = -1;
	}
	m_SpinLock.Release ();
}

void CGUDDisplay::OnDisconnect (void)
{
	m_bControllerOn = FALSE;
	m_bDisplayOn = FALSE;
	m_nWidth = m_nHeight = 0;
	m_bPending = FALSE;
	m_nReceiving = -1;
}

// ---- the main loop --------------------------------------------------------------

boolean CGUDDisplay::Update (COutput *pOutput)
{
	// the bands in the order received: the older one first (not while the
	// desktop waits for the kernel to put it on screen: Start)
	for (unsigned k = 0; k < 2 && !(IsActive () && !m_pShownOn); k++)
	{
		unsigned i = m_nNextBand;		// the one after the newest...
		if (!m_Band[i].bFull)
		{
			i ^= 1;				// ...or the only full one
		}
		TBand &B = m_Band[i];
		if (!B.bFull)
		{
			break;
		}
		if (IsActive () && m_pPicture)
		{
			Draw (B);
		}
		m_SpinLock.Acquire ();
		B.bFull = FALSE;
		StartReceive ();			// (a band waiting for a buffer)
		m_SpinLock.Release ();
	}

	if (m_bDirty && IsActive () && pOutput == m_pShownOn)
	{
		ShowPicture (pOutput);
	}

	unsigned nNow = CTimer::GetClockTicks ();	// the updates, a second
	if (nNow - m_nLastReport >= 1000000)
	{
		if (m_nUpdates)
		{
			LOGNOTE ("PC desktop: %u rectangles, %u KB", m_nUpdates, m_nBytes / 1024);
		}
		m_nUpdates = 0;
		m_nBytes = 0;
		m_nLastReport = nNow;
	}

	boolean bActive = IsActive ();
	boolean bChanged = bActive != m_bWasActive;
	if (bChanged)
	{
		LOGNOTE ("PC desktop %s (%ux%u)", bActive ? "on" : "off", m_nScreenWidth, m_nScreenHeight);
		m_bWasActive = bActive;
		if (!bActive)
		{
			m_pShownOn = nullptr;
		}
	}

	return bChanged;
}

// the band's rows into the picture
void CGUDDisplay::Draw (const TBand &rBand)
{
	if (   rBand.x + rBand.nWidth > m_nPictureWidth
	    || rBand.y + rBand.nHeight > m_nPictureHeight)
	{
		return;					// (from before a size change)
	}
	const u8 *pFrom = rBand.pData;
	for (unsigned y = 0; y < rBand.nHeight; y++)
	{
		u16 *pTo = m_pPicture + (rBand.y + y) * m_nPictureWidth + rBand.x;
		memcpy (pTo, pFrom, rBand.nWidth * 2);
		if (m_pPicture != m_pOwnPicture)	// the output's page: on screen
		{
			CleanAndInvalidateDataCacheRange ((uintptr) pTo, rBand.nWidth * 2);
		}
		pFrom += rBand.nWidth * 2;
	}
	m_bDirty = TRUE;
	m_nUpdates++;
	m_nBytes += rBand.nWidth * rBand.nHeight * 2;
}

void CGUDDisplay::Start (COutput *pOutput)
{
	m_nPictureWidth = pOutput->GetWidth ();
	m_nPictureHeight = pOutput->GetHeight ();
	u16 *pPage = nullptr;
	pOutput->WaitIdle ();
	m_pPicture = pOutput->GetBuffers (&pPage, 1) ? pPage : m_pOwnPicture;
	memset (m_pPicture, 0, m_nPictureWidth * m_nPictureHeight * 2);
	if (m_pPicture != m_pOwnPicture)
	{
		CleanAndInvalidateDataCacheRange ((uintptr) m_pPicture, m_nPictureWidth * m_nPictureHeight * 2);
	}
	m_pShownOn = pOutput;
	pOutput->Show (m_pPicture, nullptr, nullptr);
	m_bDirty = FALSE;
}

// the picture's latest state to the output: a page is on screen already; our
// own picture goes to the panel again (at most at its frame rate: Show waits
// for the one before)
void CGUDDisplay::ShowPicture (COutput *pOutput)
{
	m_bDirty = FALSE;
	if (m_pPicture != m_pOwnPicture)
	{
		return;
	}
	pOutput->WaitIdle ();
	pOutput->Show (m_pPicture, nullptr, nullptr);
}
