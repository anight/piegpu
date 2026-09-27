//
// hdmi_output.cpp
//
#include "hdmi_output.h"
#include <circle/logger.h>

// the copy goes by a lite DMA channel (the Zero has only four normal ones: the
// panel's SPI and the I2S link take them), which moves at most 64 KB at a time
#define DMA_CHUNK		(60 * 1024)

LOGMODULE ("hdmi");

CHDMIOutput::CHDMIOutput (void)
:	m_pFrameBuffer (nullptr),
	m_nWidth (0),
	m_nHeight (0),
	m_DMA (DMA_CHANNEL_LITE),
	m_nShown (0),
	m_bFlipped (FALSE)
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

	CBcmFrameBuffer *pFrameBuffer = new CBcmFrameBuffer (nWidth, nHeight, 16, nWidth, 2 * nHeight);
	if (!pFrameBuffer->Initialize ())
	{
		LOGERR ("No %ux%u framebuffer", nWidth, nHeight);
		delete pFrameBuffer;
		return FALSE;
	}
	if (   pFrameBuffer->GetDepth () != 16
	    || pFrameBuffer->GetPitch () != nWidth * 2
	    || pFrameBuffer->GetVirtHeight () < 2 * nHeight)
	{
		LOGERR ("Framebuffer %ux%u (virtual %ux%u), %u bits, pitch %u: not %ux%ux16 with two pages",
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
	LOGNOTE ("Framebuffer %ux%u RGB565, two pages at %08X", nWidth, nHeight,
		 m_pFrameBuffer->GetBuffer ());

	return TRUE;
}

u8 *CHDMIOutput::GetPage (unsigned nPage) const
{
	return (u8 *) (uintptr) m_pFrameBuffer->GetBuffer () + nPage * m_nHeight * m_nWidth * 2;
}

// the frame into the hidden page, then that page on screen (from the next
// vertical sync); the renderer's buffer is free once the copy is done
void CHDMIOutput::Show (const void *pPixels, TDoneRoutine *pDone, void *pParam)
{
	unsigned nPage = m_nShown ^ 1;

	u8 *pTo = GetPage (nPage);
	const u8 *pFrom = (const u8 *) pPixels;
	for (unsigned nLeft = m_nWidth * m_nHeight * 2; nLeft; )
	{
		unsigned nChunk = nLeft < DMA_CHUNK ? nLeft : DMA_CHUNK;
		m_DMA.SetupMemCopy (pTo, pFrom, nChunk);
		m_DMA.Start ();
		m_DMA.Wait ();
		pTo += nChunk;
		pFrom += nChunk;
		nLeft -= nChunk;
	}

	m_pFrameBuffer->SetVirtualOffset (0, nPage * m_nHeight);
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

// one frame a refresh: after a flip, wait for the vertical sync that shows it
void CHDMIOutput::WaitIdle (void)
{
	if (m_bFlipped)
	{
		m_pFrameBuffer->WaitForVerticalSync ();
		m_bFlipped = FALSE;
	}
}
