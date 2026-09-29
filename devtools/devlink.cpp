//
// devlink.cpp
//
#include "devlink.h"
#include <circle/devicenameservice.h>
#include <circle/logger.h>
#include <circle/startup.h>
#include <circle/util.h>
#include <assert.h>

// (one copy of each, so the matchers can tell "at the start" by the pointer)
static constexpr char s_RebootMagic[] = DEVLINK_REBOOT_MAGIC;
static constexpr char s_StreamMagic[] = DEVLINK_STREAM_MAGIC;
static_assert (s_RebootMagic[0] == s_StreamMagic[0], "ScanMagic skips to their first character");

LOGMODULE ("devlink");

CDevLink::CDevLink (CInterruptSystem *pInterrupt, CDWUSBGadget *pGadget)
:	m_pGadget (pGadget ? pGadget : new CUSBCDCGadget (pInterrupt, DEVLINK_USB_VENDOR_ID,
							      DEVLINK_USB_PRODUCT_ID)),
	m_pSerial (nullptr),
	m_pPrevLogTarget (nullptr),
	m_bHostActive (FALSE),
	m_bReplayDone (FALSE),
	m_pMagicPtr (s_RebootMagic),
	m_pStreamMagicPtr (s_StreamMagic),
	m_bStream (FALSE),
	m_pStream (nullptr),
	m_nStreamIn (0),
	m_nStreamOut (0),
	m_nStreamReceived (0),
	m_nRxIn (0),
	m_nRxOut (0)
{
}

CDevLink::~CDevLink (void)
{
	delete [] m_pStream;
}

boolean CDevLink::Initialize (void)
{
	m_Watchdog.Start (DEVLINK_WATCHDOG_SECONDS);

	m_pStream = new u8[StreamSize];

	return m_pStream && m_pGadget->Initialize ();
}

void CDevLink::Update (void)
{
	m_Watchdog.Start (DEVLINK_WATCHDOG_SECONDS);	// re-trigger

	if (   m_pGadget->UpdatePlugAndPlay ()
	    && !m_pSerial)
	{
		m_pSerial = static_cast<CUSBSerialDevice *> (
			CDeviceNameService::Get ()->GetDevice ("utty1", FALSE));
		if (m_pSerial)
		{
			m_pSerial->RegisterRemovedHandler (DeviceRemovedHandler, this);
			m_pSerial->SetOptions (SERIAL_OPTION_ONLCR);
		}
	}

	if (!m_pSerial)
	{
		return;
	}

	char Buffer[4096];
	int nResult;
	do
	{
		// in stream mode, read only what the ring can take: the rest stays
		// in the gadget, and the host waits (USB flow control)
		unsigned nFree = (m_nStreamOut + StreamSize - m_nStreamIn - 1) % StreamSize;
		if (m_bStream && nFree < sizeof Buffer)
		{
			break;
		}

		nResult = m_pSerial->Read (Buffer, sizeof Buffer);
		if (nResult > 0)
		{
			// The host has opened the port and is listening now
			m_bHostActive = TRUE;

			Receive (Buffer, nResult);
		}
	}
	while (nResult > 0);			// all of it: the gadget's queue is 64 KB

	if (   m_bHostActive
	    && !m_bReplayDone
	    && !m_bStream)
	{
		// Replay the log ring buffer in small chunks, so that the gadget's
		// (non-blocking) send queue does not overflow. Keep the log target
		// unchanged until done, so that new messages go into the ring buffer.
		CLogger *pLogger = CLogger::Get ();
		nResult = pLogger->Read (Buffer, sizeof Buffer);
		if (nResult > 0)
		{
			m_pSerial->Write (Buffer, nResult);
		}
		else
		{
			m_pPrevLogTarget = pLogger->GetTarget ();
			pLogger->SetNewTarget (m_pSerial);

			m_bReplayDone = TRUE;

			LOGNOTE ("Host connected");
		}
	}
}

void CDevLink::Receive (const char *pData, unsigned nLength)
{
	while (nLength)
	{
		// up to the end of the stream magic (the stream starts after it), or all
		unsigned n = ScanMagic (pData, nLength);
		if (m_bStream)
		{
			// into the ring (Update () checked the space), in blocks
			m_nStreamReceived += n;
			for (unsigned nLeft = n; nLeft; )
			{
				unsigned nChunk = StreamSize - m_nStreamIn < nLeft ? StreamSize - m_nStreamIn : nLeft;
				memcpy (m_pStream + m_nStreamIn, pData + (n - nLeft), nChunk);
				m_nStreamIn = (m_nStreamIn + nChunk) % StreamSize;
				nLeft -= nChunk;
			}
		}
		else
		{
			for (unsigned i = 0; i < n; i++)
			{
				unsigned nNext = (m_nRxIn + 1) % RxBufferSize;
				if (nNext != m_nRxOut)		// drop when full
				{
					m_RxBuffer[m_nRxIn] = pData[i];
					m_nRxIn = nNext;
				}
			}
		}
		pData += n;
		nLength -= n;

		// the magic again: a new host session (the magic in the stream is
		// skipped as garbage)
		if (*m_pStreamMagicPtr == '\0')
		{
			StartStream ();			// the following bytes are the stream
		}
	}
}

// the two magics, in the host's bytes: reboots on the reboot magic; returns
// the number of bytes up to the end of the stream magic, or all of them
unsigned CDevLink::ScanMagic (const char *pData, unsigned nLength)
{
	const char *p = pData, *pEnd = pData + nLength;
	while (p < pEnd)
	{
		if (m_pMagicPtr == s_RebootMagic && m_pStreamMagicPtr == s_StreamMagic)
		{
			while (p < pEnd && *p != s_RebootMagic[0])	// (both start with it)
			{
				p++;
			}
			if (p == pEnd)
			{
				break;
			}
		}

		char c = *p++;
		if (c != *m_pMagicPtr)
		{
			m_pMagicPtr = s_RebootMagic;
		}
		if (c == *m_pMagicPtr && *++m_pMagicPtr == '\0')
		{
			LOGNOTE ("Reboot requested by host");

			reboot ();
		}

		if (c != *m_pStreamMagicPtr)
		{
			m_pStreamMagicPtr = s_StreamMagic;
		}
		if (c == *m_pStreamMagicPtr && *++m_pStreamMagicPtr == '\0')
		{
			return p - pData;
		}
	}

	return nLength;
}

void CDevLink::StartStream (void)
{
	LOGNOTE ("Binary stream from the host");

	m_pStreamMagicPtr = s_StreamMagic;
	if (!m_bReplayDone)
	{
		// the log goes to the host from now on (without the replay)
		CLogger *pLogger = CLogger::Get ();
		m_pPrevLogTarget = pLogger->GetTarget ();
		pLogger->SetNewTarget (m_pSerial);
		m_bReplayDone = TRUE;
	}
	m_bStream = TRUE;
	m_nStreamReceived = 0;			// counted from after the magic
	m_nRxIn = m_nRxOut = 0;

	Write (DEVLINK_STREAM_ACK, sizeof DEVLINK_STREAM_ACK - 1);
}

unsigned CDevLink::StreamRead (void *pBuffer, unsigned nMax)
{
	u8 *p = (u8 *) pBuffer;
	unsigned n = 0;
	while (n < nMax && m_nStreamOut != m_nStreamIn)
	{
		// up to the write position or the end of the ring
		unsigned nEnd = m_nStreamIn > m_nStreamOut ? m_nStreamIn : StreamSize;
		unsigned nChunk = nEnd - m_nStreamOut < nMax - n ? nEnd - m_nStreamOut : nMax - n;
		memcpy (p + n, m_pStream + m_nStreamOut, nChunk);
		n += nChunk;
		m_nStreamOut = (m_nStreamOut + nChunk) % StreamSize;
	}

	return n;
}

int CDevLink::GetChar (void)
{
	if (m_nRxOut == m_nRxIn)
	{
		return -1;
	}

	char c = m_RxBuffer[m_nRxOut];
	m_nRxOut = (m_nRxOut + 1) % RxBufferSize;

	return (unsigned char) c;
}

void CDevLink::DeviceRemovedHandler (CDevice *pDevice, void *pContext)
{
	CDevLink *pThis = static_cast<CDevLink *> (pContext);
	assert (pThis);

	if (pThis->m_bReplayDone)
	{
		CLogger::Get ()->SetNewTarget (pThis->m_pPrevLogTarget);
	}

	pThis->m_pSerial = nullptr;
	pThis->m_bHostActive = FALSE;
	pThis->m_bReplayDone = FALSE;
}

boolean CDevLink::Write (const void *pData, unsigned nLength)
{
	const u8 *p = (const u8 *) pData;
	unsigned nStart = CTimer::GetClockTicks ();
	boolean bOK = TRUE;

	if (!m_pSerial)
	{
		return FALSE;
	}

	// raw: with ONLCR, Write() returns the length after the \n -> \r\n expansion
	m_pSerial->SetOptions (0);

	while (nLength)
	{
		int nResult = m_pSerial->Write (p, nLength);
		if (   nResult < 0
		    || CTimer::GetClockTicks () - nStart > 1000000)
		{
			bOK = FALSE;
			break;
		}
		assert ((unsigned) nResult <= nLength);
		p += nResult;
		nLength -= nResult;
	}

	m_pSerial->SetOptions (SERIAL_OPTION_ONLCR);

	return bOK;
}
