//
// i2s_link.h
//
// Command stream receiver (docs/protocol.md sections 2-4): the PCM block as
// I2S clock and frame slave, a self-looping DMA control block filling a ring
// buffer, and a packet parser (idle words, CRC32, resynchronisation).
// Drives READY (GPIO16) from the ring's fill level.
//
// Replies (section 9) go out on PCM_DOUT (GPIO21) from a transmit ring that
// a second self-looping DMA control block plays forever. The ring holds idle
// words (0); a reply is written some distance ahead of the DMA's read
// position and zeroed again once the DMA has passed it.
//
#ifndef _i2s_link_h
#define _i2s_link_h

#include "link.h"
#include <circle/gpiopin.h>
#include <circle/types.h>

struct TI2SLinkStats
{
	u32 nWords;		// words taken from the ring
	u32 nIdleWords;
	u32 nPackets;
	u32 nCRCErrors;
	u32 nGarbageWords;	// non-idle words outside packets
	u32 nMaxFill;		// words
	u32 nReadyLow;		// times READY was dropped
	u32 nReplies;
	u32 nRepliesDropped;	// transmit backlog full
	u32 nTxLate;		// a reply was zeroed more than a ring lap late (sent twice)
};

class CI2SLink : public CLink	/// the Pico's I2S link (docs/protocol.md 2-4)
{
public:
	static const unsigned RingWords = 256 * 1024;		// 1 MB
	static const unsigned ReadyLowWords = 32 * 1024;	// drop READY below 128 KB free
	static const unsigned ReadyHighWords = 64 * 1024;	// raise it again at 256 KB free

	static const unsigned TxRingWords = 256 * 1024;		// 1 MB: 112 ms per lap at 75 MHz
	static const unsigned TxMarginWords = 1024;		// write replies this far ahead of the DMA
	static const unsigned MaxReplyWords = 64;

public:
	CI2SLink (void);
	~CI2SLink (void);

	boolean Initialize (void);

	/// \brief Return the next valid packet, or nullptr if none is complete yet
	/// \param pHeader Receives the header word
	/// \return Payload (LENGTH words), valid until the next call
	const u32 *GetPacket (u32 *pHeader);

	/// \brief Update READY from the ring fill level (called by GetPacket too)
	void UpdateReady (void);

	/// \brief Queue a reply packet (header and CRC are added)
	/// \return FALSE if it was dropped
	boolean SendReply (u8 uchOpcode, const u32 *pPayload, unsigned nLength);

	/// \brief Zero sent replies in the transmit ring (called by GetPacket too)
	void UpdateTx (void);

	void Flush (void)			{ UpdateTx (); }

	u32 GetCRCErrors (void) const		{ return m_nTotalCRCErrors; }
	unsigned GetFreeBytes (void) const;
	unsigned GetBufferBytes (void) const	{ return RingWords * 4; }

	/// \return Statistics since the last call, then reset them
	TI2SLinkStats GetStats (void);

private:
	unsigned GetWriteIndex (void) const;
	unsigned Available (void) const;
	void InvalidateNew (void);

	void SetupPCM (void);
	void SetupDMA (void);
	void SetupTxDMA (void);
	unsigned GetTxReadIndex (void) const;
	void WriteTx (u64 nPos, const u32 *pWords, unsigned nWords);

	u32 CRC (u32 nCRC, u32 nWord) const;

private:
	CGPIOPin m_PinCLK;
	CGPIOPin m_PinFS;
	CGPIOPin m_PinDIN;
	CGPIOPin m_PinDOUT;
	CGPIOPin m_PinReady;

	u32 *m_pRing;
	u32 m_nRingBus;
	unsigned m_nRead;		// word index into the ring
	unsigned m_nInvalidated;	// ring words up to here are fresh in the cache

	unsigned m_nDMAChannel;
	u32 *m_pControlBlock;

	boolean m_bReady;

	u32 *m_pTxRing;
	u32 m_nTxRingBus;
	unsigned m_nTxDMAChannel;
	u32 *m_pTxControlBlock;
	u64 m_nTxPos;			// DMA read position, unwrapped (words)
	unsigned m_nTxLastIndex;
	u64 m_nTxNext;			// where the next reply goes, unwrapped

	struct TPending
	{
		u64 nStart;
		unsigned nWords;
	};
	static const unsigned MaxPending = 256;
	TPending m_Pending[MaxPending];	// in transmit order
	unsigned m_nPendingHead;
	unsigned m_nPendingCount;

	u32 m_nTotalCRCErrors;

	u32 *m_pPacket;			// linear copy of the current payload
	u32 m_CRCTable[256];

	TI2SLinkStats m_Stats;
};

#endif
