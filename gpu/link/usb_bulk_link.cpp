//
// usb_bulk_link.cpp
//
#include "usb_bulk_link.h"
#include <circle/timer.h>
#include <circle/util.h>

CUSBBulkLink::CUSBBulkLink (CPiGPUGadget *pGadget)
:	m_pGadget (pGadget),
	m_SpinLock (IRQ_LEVEL),
	m_bActive (FALSE),
	m_pRxMemory (nullptr),
	m_nRxRead (0),
	m_nRxOffset (0),
	m_nRxFull (0),
	m_bRxReceiving (FALSE),
	m_pTx (nullptr),
	m_nTxIn (0),
	m_nTxOut (0),
	m_nTxSending (0)
{
	for (unsigned i = 0; i < RxBuffers; i++)
	{
		m_pRx[i] = nullptr;
		m_nRxLength[i] = 0;
	}
}

CUSBBulkLink::~CUSBBulkLink (void)
{
	delete [] m_pRxMemory;
	delete [] m_pTx;
}

boolean CUSBBulkLink::Initialize (void)
{
	// the receive buffers: cache-line aligned (the USB DMA cleans and
	// invalidates their lines)
	m_pRxMemory = new u8[RxBuffers * RxBufferBytes + 64];
	m_pTx = new u8[TxBytes];
	if (!m_pRxMemory || !m_pTx)
	{
		return FALSE;
	}
	u8 *p = (u8 *) (((uintptr) m_pRxMemory + 63) & ~(uintptr) 63);
	for (unsigned i = 0; i < RxBuffers; i++)
	{
		m_pRx[i] = p + i * RxBufferBytes;
	}

	return CByteStreamLink::Initialize ();
}

void CUSBBulkLink::Update (void)
{
	if (!m_pTx)
	{
		return;				// (not initialized)
	}

	m_SpinLock.Acquire ();
	Receive ();
	Send ();
	m_SpinLock.Release ();
}

void CUSBBulkLink::Flush (void)
{
	unsigned nStart = CTimer::GetClockTicks ();
	while (   GetTxFree () < MaxTransfer
	       && CTimer::GetClockTicks () - nStart < 10000)
	{
		m_SpinLock.Acquire ();
		Send ();			// (after a reconnection)
		m_SpinLock.Release ();
	}
}

unsigned CUSBBulkLink::ReadStream (void *pBuffer, unsigned nMax)
{
	u8 *p = (u8 *) pBuffer;
	unsigned n = 0;
	while (n < nMax && m_nRxFull)
	{
		// the buffer at m_nRxRead is ours until it's given back
		unsigned nChunk = m_nRxLength[m_nRxRead] - m_nRxOffset;
		if (nChunk > nMax - n)
		{
			nChunk = nMax - n;
		}
		memcpy (p + n, m_pRx[m_nRxRead] + m_nRxOffset, nChunk);
		n += nChunk;
		m_nRxOffset += nChunk;

		if (m_nRxOffset == m_nRxLength[m_nRxRead])
		{
			m_SpinLock.Acquire ();
			m_nRxRead = (m_nRxRead + 1) % RxBuffers;
			m_nRxOffset = 0;
			m_nRxFull--;
			Receive ();			// (the ring was full)
			m_SpinLock.Release ();
		}
	}

	return n;
}

boolean CUSBBulkLink::WriteStream (const void *pData, unsigned nLength)
{
	if (!m_pTx || nLength > GetTxFree ())
	{
		return FALSE;
	}

	// m_nTxIn .. m_nTxOut + TxBytes is ours: the interrupt only sends (and
	// frees) what's before m_nTxIn
	const u8 *p = (const u8 *) pData;
	unsigned nPos = m_nTxIn % TxBytes;
	unsigned nFirst = TxBytes - nPos < nLength ? TxBytes - nPos : nLength;
	memcpy (m_pTx + nPos, p, nFirst);
	memcpy (m_pTx, p + nFirst, nLength - nFirst);

	m_SpinLock.Acquire ();
	m_nTxIn += nLength;
	Send ();
	m_SpinLock.Release ();

	return TRUE;
}

// a free buffer to the gadget, if it hasn't one
void CUSBBulkLink::Receive (void)
{
	if (   !m_bRxReceiving
	    && m_nRxFull < RxBuffers
	    && m_pGadget->StreamReceive (m_pRx[(m_nRxRead + m_nRxFull) % RxBuffers], RxBufferBytes))
	{
		m_bRxReceiving = TRUE;
	}
}

// the next replies to the gadget, if it isn't sending. Every transfer ends
// with a short packet (never a multiple of 64 bytes, the full-speed packet
// size), so that the host's read returns; the rest follows.
void CUSBBulkLink::Send (void)
{
	unsigned nBytes = m_nTxIn - m_nTxOut;
	if (m_nTxSending || !nBytes)
	{
		return;
	}
	unsigned nPos = m_nTxOut % TxBytes;
	if (nBytes > TxBytes - nPos)
	{
		nBytes = TxBytes - nPos;			// to the end of the ring
	}
	if (nBytes > MaxTransfer)
	{
		nBytes = MaxTransfer;
	}
	if (nBytes % 64 == 0)
	{
		nBytes -= 4;					// (replies are words)
	}
	if (m_pGadget->StreamSend (m_pTx + nPos, nBytes))
	{
		m_nTxSending = nBytes;
	}
}

void CUSBBulkLink::OnStreamReceived (size_t nLength)
{
	m_SpinLock.Acquire ();
	m_bRxReceiving = FALSE;
	if (nLength)				// (not a zero-length packet alone)
	{
		m_nRxLength[(m_nRxRead + m_nRxFull) % RxBuffers] = nLength;
		m_nRxFull++;
		m_bActive = TRUE;
	}
	Receive ();
	m_SpinLock.Release ();
}

void CUSBBulkLink::OnStreamSent (void)
{
	m_SpinLock.Acquire ();
	m_nTxOut += m_nTxSending;
	m_nTxSending = 0;
	Send ();
	m_SpinLock.Release ();
}

void CUSBBulkLink::OnStreamDisconnect (void)
{
	m_SpinLock.Acquire ();
	m_bRxReceiving = FALSE;
	m_nTxSending = 0;			// (sent again after a reconnection)
	m_SpinLock.Release ();
}
