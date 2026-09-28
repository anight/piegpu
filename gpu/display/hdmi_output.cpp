//
// hdmi_output.cpp
//
#include "hdmi_output.h"
#include <circle/logger.h>
#include <circle/timer.h>
#include <circle/bcm2835.h>
#include <circle/memio.h>

// the display scaler (HVS) channel HDMI uses on the Zero: its status has the
// frame count in bits 17:12 (6 bits); measured: 60 a second on HDMI
#define HVS_DISPSTAT1		(ARM_IO_BASE + 0x400058)
#define SYNC_TIMEOUT_US		100000

LOGMODULE ("hdmi");

CHDMIOutput::CHDMIOutput (void)
:	m_pFrameBuffer (nullptr),
	m_nWidth (0),
	m_nHeight (0),
	m_nShown (0),
	m_bFlipped (FALSE),
	m_nFlipFrame (0),
	m_nSyncTimeouts (0)
{
}

CHDMIOutput::~CHDMIOutput (void)
{
	delete m_pFrameBuffer;
}

boolean CHDMIOutput::SetSize (unsigned nWidth, unsigned nHeight)
{
	if (m_pFrameBuffer && nWidth == m_nWidth && nHeight == m_nHeight)
	{
		return TRUE;
	}

	CBcmFrameBuffer *pFrameBuffer = new CBcmFrameBuffer (nWidth, nHeight, 16, nWidth, Pages * nHeight);
	if (!pFrameBuffer->Initialize ())
	{
		LOGERR ("No %ux%u framebuffer", nWidth, nHeight);
		delete pFrameBuffer;
		return FALSE;
	}
	if (   pFrameBuffer->GetDepth () != 16
	    || pFrameBuffer->GetPitch () != nWidth * 2
	    || pFrameBuffer->GetVirtHeight () < Pages * nHeight)
	{
		LOGERR ("Framebuffer %ux%u (virtual %ux%u), %u bits, pitch %u: not %ux%ux16 with three pages",
			pFrameBuffer->GetWidth (), pFrameBuffer->GetHeight (), pFrameBuffer->GetVirtWidth (),
			pFrameBuffer->GetVirtHeight (), pFrameBuffer->GetDepth (), pFrameBuffer->GetPitch (),
			nWidth, nHeight);
		delete pFrameBuffer;
		return FALSE;
	}

	delete m_pFrameBuffer;
	m_pFrameBuffer = pFrameBuffer;
	m_nWidth = nWidth;
	m_nHeight = nHeight;
	m_nShown = 0;
	m_bFlipped = FALSE;
	LOGNOTE ("Framebuffer %ux%u RGB565, three pages at %08X", nWidth, nHeight,
		 m_pFrameBuffer->GetBuffer ());

	return TRUE;
}

u8 *CHDMIOutput::GetPage (unsigned nPage) const
{
	return (u8 *) (uintptr) m_pFrameBuffer->GetBuffer () + nPage * m_nHeight * m_nWidth * 2;
}

unsigned CHDMIOutput::GetBuffers (u16 **ppBuffers, unsigned nMax)
{
	unsigned n = nMax < Pages ? nMax : Pages;
	for (unsigned i = 0; i < n; i++)
	{
		ppBuffers[i] = (u16 *) GetPage (i);
	}

	return n;
}

// a page the renderer has drawn: on screen from the next vertical sync
void CHDMIOutput::Show (const void *pPixels, TDoneRoutine *pDone, void *pParam)
{
	unsigned nPage = 0;
	while (nPage < Pages && GetPage (nPage) != pPixels)
	{
		nPage++;
	}
	if (nPage == Pages)
	{
		LOGERR ("Not a page: %p", pPixels);
		return;
	}

	m_pFrameBuffer->SetVirtualOffset (0, nPage * m_nHeight);
	m_nFlipFrame = FrameCount ();		// the firmware has the offset: shown from the next sync
	m_nShown = nPage;
	m_bFlipped = TRUE;

	if (pDone)
	{
		(*pDone) (pParam);
	}
}

const void *CHDMIOutput::GetShownFrame (void)
{
	return GetPage (m_nShown);
}

unsigned CHDMIOutput::FrameCount (void)
{
	return (read32 (HVS_DISPSTAT1) >> 12) & 0x3F;
}

// until the frame count moves on from nFrom (a vertical sync); FALSE after the timeout
boolean CHDMIOutput::WaitFrame (unsigned nFrom, unsigned nTimeoutUs)
{
	unsigned nStart = CTimer::GetClockTicks ();
	while (FrameCount () == nFrom)
	{
		if (CTimer::GetClockTicks () - nStart > nTimeoutUs)
		{
			if (m_nSyncTimeouts++ % 60 == 0)
			{
				LOGWARN ("No vertical sync for %u ms (%u times)", nTimeoutUs / 1000,
					 m_nSyncTimeouts);
			}
			return FALSE;
		}
	}
	return TRUE;
}

unsigned CHDMIOutput::MeasureRefresh (void)
{
	WaitIdle ();
	WaitFrame (FrameCount (), SYNC_TIMEOUT_US);
	unsigned nStart = CTimer::GetClockTicks ();
	for (unsigned i = 0; i < 10; i++)
	{
		if (!WaitFrame (FrameCount (), SYNC_TIMEOUT_US))
		{
			return 0;
		}
	}
	unsigned nUs = CTimer::GetClockTicks () - nStart;

	return nUs ? (unsigned) (10000000000ULL / nUs) : 0;
}

// one frame a refresh: after a flip, the vertical sync that shows it
void CHDMIOutput::WaitIdle (void)
{
	if (m_bFlipped)
	{
		WaitFrame (m_nFlipFrame, SYNC_TIMEOUT_US);
		m_bFlipped = FALSE;
	}
}
