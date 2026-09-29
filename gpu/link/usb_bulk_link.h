//
// usb_bulk_link.h
//
// The command stream from a PC over the RPi's GL interface (devtools/
// pgpugadget: interface 2, a bulk endpoint each way; transports/pc-usb with
// libusb). The same packets as on the other links (docs/protocol.md 4).
//
// Commands arrive by DMA in a ring of buffers; a buffer is given to the
// gadget only while it's free, so USB itself holds the host back when the
// ring is full (no CREDIT replies). Replies go out from a ring of bytes,
// up to 16 KB a transfer. Active once the host has sent on it (until the
// RPi restarts).
//
#ifndef _usb_bulk_link_h
#define _usb_bulk_link_h

#include "byte_stream_link.h"
#include <pgpugadget.h>
#include <circle/spinlock.h>
#include <circle/types.h>

class CUSBBulkLink : public CByteStreamLink, public CGadgetStream
{
public:
	CUSBBulkLink (CPiGPUGadget *pGadget);
	~CUSBBulkLink (void);

	boolean Initialize (void);

	boolean IsActive (void)			{ return m_bActive; }

	/// \brief Give the gadget a free buffer (after it connected)
	void Update (void);

	/// \brief Wait (up to 10 ms) for room for replies
	void Flush (void);

private:
	unsigned ReadStream (void *pBuffer, unsigned nMax);
	boolean WriteStream (const void *pData, unsigned nLength);

	// CGadgetStream (the USB interrupt)
	void OnStreamReceived (size_t nLength);
	void OnStreamSent (void);
	void OnStreamDisconnect (void);

	// with the spin lock held
	void Receive (void);
	void Send (void);
	unsigned GetTxFree (void) const		{ return TxBytes - (m_nTxIn - m_nTxOut); }

private:
	CPiGPUGadget *m_pGadget;
	CSpinLock m_SpinLock;
	volatile boolean m_bActive;

	// commands: buffer (m_nRxRead + m_nRxFull) % RxBuffers is the one the
	// gadget receives into, while m_bRxReceiving
	static const unsigned RxBuffers = 8;
	static const unsigned RxBufferBytes = 16384;
	u8 *m_pRxMemory;
	u8 *m_pRx[RxBuffers];
	unsigned m_nRxLength[RxBuffers];
	unsigned m_nRxRead;			// the buffer being read
	unsigned m_nRxOffset;			// in it
	volatile unsigned m_nRxFull;		// received, not read
	volatile boolean m_bRxReceiving;

	// replies: bytes m_nTxOut .. m_nTxIn (free-running), the first
	// m_nTxSending of them in a transfer
	static const unsigned TxBytes = 512 * 1024;	// (a power of 2) a READ_PIXELS of 256x256
	static const unsigned MaxTransfer = 16384;
	u8 *m_pTx;
	volatile unsigned m_nTxIn;
	volatile unsigned m_nTxOut;
	volatile unsigned m_nTxSending;
};

#endif
