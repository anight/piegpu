//
// i2s_link.cpp
//
#include "i2s_link.h"
#include <pgpu_protocol.h>
#include <circle/bcm2835.h>
#include <circle/dmacommon.h>
#include <circle/machineinfo.h>
#include <circle/memio.h>
#include <circle/synchronize.h>
#include <circle/timer.h>
#include <circle/util.h>
#include <assert.h>

// PCM registers (see pcmslave)
#define CS_A_DMAEN		(1 << 9)
#define CS_A_RXCLR		(1 << 4)
#define CS_A_TXCLR		(1 << 3)
#define CS_A_TXON		(1 << 2)
#define CS_A_RXON		(1 << 1)
#define CS_A_EN			(1 << 0)

#define MODE_A_CLKM		(1 << 23)	// clock slave
#define MODE_A_CLKI		(1 << 22)	// sample on the rising edge
#define MODE_A_FSM		(1 << 21)	// frame sync slave
#define MODE_A_FSI		(1 << 20)	// frame starts with FS low (I2S)
#define MODE_A_FLEN__SHIFT	10
#define MODE_A_FSLEN__SHIFT	0

#define XC_CH1WEX		(1 << 31)
#define XC_CH1EN		(1 << 30)
#define XC_CH1POS__SHIFT	20
#define XC_CH1WID__SHIFT	16
#define XC_CH2WEX		(1 << 15)
#define XC_CH2EN		(1 << 14)
#define XC_CH2POS__SHIFT	4
#define XC_CH2WID__SHIFT	0

#define PIN_READY		16

CI2SLink::CI2SLink (void)
:	m_PinCLK (18, GPIOModeAlternateFunction0),
	m_PinFS (19, GPIOModeAlternateFunction0),
	m_PinDIN (20, GPIOModeAlternateFunction0),
	m_PinDOUT (21, GPIOModeAlternateFunction0),
	m_PinReady (PIN_READY, GPIOModeOutput),
	m_pRing (nullptr),
	m_nRead (0),
	m_nInvalidated (0),
	m_bReady (FALSE),
	m_pTxRing (nullptr),
	m_nTxPos (0),
	m_nTxLastIndex (0),
	m_nTxNext (0),
	m_nPendingHead (0),
	m_nPendingCount (0),
	m_nTotalCRCErrors (0),
	m_pPacket (nullptr)
{
	m_PinReady.Write (LOW);			// not ready until initialized
	memset (&m_Stats, 0, sizeof m_Stats);
}

CI2SLink::~CI2SLink (void)
{
}

boolean CI2SLink::Initialize (void)
{
	for (u32 i = 0; i < 256; i++)
	{
		u32 c = i;
		for (unsigned k = 0; k < 8; k++)
		{
			c = c & 1 ? PGPU_CRC_POLY_REFLECTED ^ (c >> 1) : c >> 1;
		}
		m_CRCTable[i] = c;
	}

	u8 *p = new u8[RingWords * 4 + 64];
	m_pRing = (u32 *) (((uintptr) p + 63) & ~(uintptr) 63);
	memset (m_pRing, 0, RingWords * 4);
	CleanAndInvalidateDataCacheRange ((uintptr) m_pRing, RingWords * 4);
	m_nRingBus = BUS_ADDRESS ((uintptr) m_pRing);

	p = new u8[TxRingWords * 4 + 64];
	m_pTxRing = (u32 *) (((uintptr) p + 63) & ~(uintptr) 63);
	memset (m_pTxRing, 0, TxRingWords * 4);
	CleanAndInvalidateDataCacheRange ((uintptr) m_pTxRing, TxRingWords * 4);
	m_nTxRingBus = BUS_ADDRESS ((uintptr) m_pTxRing);

	m_pPacket = new u32[PGPU_MAX_PAYLOAD + 2];

	SetupPCM ();
	SetupTxDMA ();
	SetupDMA ();

	m_bReady = TRUE;
	m_PinReady.Write (HIGH);

	return TRUE;
}

void CI2SLink::SetupPCM (void)
{
	PeripheralEntry ();

	write32 (ARM_PCM_CS_A, 0);
	CTimer::SimpleusDelay (100);

	write32 (ARM_PCM_MODE_A,   MODE_A_CLKM | MODE_A_FSM | MODE_A_CLKI | MODE_A_FSI
				 | (63 << MODE_A_FLEN__SHIFT) | (32 << MODE_A_FSLEN__SHIFT));

	// I2S: each 32-bit channel starts one clock after the FS edge
	write32 (ARM_PCM_RXC_A,   XC_CH1WEX | XC_CH1EN | (1 << XC_CH1POS__SHIFT) | (8 << XC_CH1WID__SHIFT)
				| XC_CH2WEX | XC_CH2EN | (33 << XC_CH2POS__SHIFT) | (8 << XC_CH2WID__SHIFT));
	write32 (ARM_PCM_TXC_A,   XC_CH1WEX | XC_CH1EN | (1 << XC_CH1POS__SHIFT) | (8 << XC_CH1WID__SHIFT)
				| XC_CH2WEX | XC_CH2EN | (33 << XC_CH2POS__SHIFT) | (8 << XC_CH2WID__SHIFT));

	write32 (ARM_PCM_CS_A, CS_A_EN);
	CTimer::SimpleusDelay (100);
	write32 (ARM_PCM_CS_A, CS_A_EN | CS_A_RXCLR | CS_A_TXCLR);
	CTimer::SimpleusDelay (100);
	write32 (ARM_PCM_CS_A, CS_A_EN | CS_A_DMAEN);

	PeripheralExit ();
}

void CI2SLink::SetupDMA (void)
{
	m_nDMAChannel = CMachineInfo::Get ()->AllocateDMAChannel (DMA_CHANNEL_NORMAL);
	assert (m_nDMAChannel <= DMA_CHANNEL_MAX);

	// one control block that chains to itself: the ring is refilled forever
	u8 *p = new u8[32 + 32];
	m_pControlBlock = (u32 *) (((uintptr) p + 31) & ~(uintptr) 31);
	u32 nBus = BUS_ADDRESS ((uintptr) m_pControlBlock);

	m_pControlBlock[0] =   (DREQSourcePCMRX << TI_PERMAP_SHIFT)
			     | (DEFAULT_BURST_LENGTH << TI_BURST_LENGTH_SHIFT)
			     | TI_SRC_DREQ | TI_DEST_WIDTH | TI_DEST_INC | TI_WAIT_RESP;
	m_pControlBlock[1] = GPU_IO_BASE + (ARM_PCM_FIFO_A & 0xFFFFFF);		// source
	m_pControlBlock[2] = m_nRingBus;						// destination
	m_pControlBlock[3] = RingWords * 4;						// length
	m_pControlBlock[4] = 0;								// stride
	m_pControlBlock[5] = nBus;							// next: itself
	m_pControlBlock[6] = 0;
	m_pControlBlock[7] = 0;
	CleanAndInvalidateDataCacheRange ((uintptr) m_pControlBlock, 32);

	PeripheralEntry ();

	write32 (ARM_DMA_ENABLE, read32 (ARM_DMA_ENABLE) | (1 << m_nDMAChannel));
	CTimer::SimpleusDelay (1000);

	write32 (ARM_DMACHAN_CS (m_nDMAChannel), CS_RESET);
	while (read32 (ARM_DMACHAN_CS (m_nDMAChannel)) & CS_RESET)
	{
		// wait
	}

	write32 (ARM_DMACHAN_CONBLK_AD (m_nDMAChannel), nBus);
	write32 (ARM_DMACHAN_CS (m_nDMAChannel),   CS_WAIT_FOR_OUTSTANDING_WRITES
						 | (DEFAULT_PANIC_PRIORITY << CS_PANIC_PRIORITY_SHIFT)
						 | (DEFAULT_PRIORITY << CS_PRIORITY_SHIFT)
						 | CS_ACTIVE);

	// start receiving and transmitting (the TX DMA has filled the FIFO)
	CTimer::SimpleusDelay (100);
	write32 (ARM_PCM_CS_A, CS_A_EN | CS_A_DMAEN | CS_A_RXON | CS_A_TXON);

	PeripheralExit ();
}

void CI2SLink::SetupTxDMA (void)
{
	m_nTxDMAChannel = CMachineInfo::Get ()->AllocateDMAChannel (DMA_CHANNEL_NORMAL);
	assert (m_nTxDMAChannel <= DMA_CHANNEL_MAX);

	u8 *p = new u8[32 + 32];
	m_pTxControlBlock = (u32 *) (((uintptr) p + 31) & ~(uintptr) 31);
	u32 nBus = BUS_ADDRESS ((uintptr) m_pTxControlBlock);

	m_pTxControlBlock[0] =   (DREQSourcePCMTX << TI_PERMAP_SHIFT)
			       | (DEFAULT_BURST_LENGTH << TI_BURST_LENGTH_SHIFT)
			       | TI_DEST_DREQ | TI_SRC_WIDTH | TI_SRC_INC | TI_WAIT_RESP;
	m_pTxControlBlock[1] = m_nTxRingBus;						// source
	m_pTxControlBlock[2] = GPU_IO_BASE + (ARM_PCM_FIFO_A & 0xFFFFFF);		// destination
	m_pTxControlBlock[3] = TxRingWords * 4;
	m_pTxControlBlock[4] = 0;
	m_pTxControlBlock[5] = nBus;							// next: itself
	m_pTxControlBlock[6] = 0;
	m_pTxControlBlock[7] = 0;
	CleanAndInvalidateDataCacheRange ((uintptr) m_pTxControlBlock, 32);

	PeripheralEntry ();

	write32 (ARM_DMA_ENABLE, read32 (ARM_DMA_ENABLE) | (1 << m_nTxDMAChannel));
	CTimer::SimpleusDelay (1000);

	write32 (ARM_DMACHAN_CS (m_nTxDMAChannel), CS_RESET);
	while (read32 (ARM_DMACHAN_CS (m_nTxDMAChannel)) & CS_RESET)
	{
		// wait
	}

	write32 (ARM_DMACHAN_CONBLK_AD (m_nTxDMAChannel), nBus);
	write32 (ARM_DMACHAN_CS (m_nTxDMAChannel),   CS_WAIT_FOR_OUTSTANDING_WRITES
						   | (DEFAULT_PANIC_PRIORITY << CS_PANIC_PRIORITY_SHIFT)
						   | (DEFAULT_PRIORITY << CS_PRIORITY_SHIFT)
						   | CS_ACTIVE);

	PeripheralExit ();
}

unsigned CI2SLink::GetTxReadIndex (void) const
{
	u32 nSource = read32 (ARM_DMACHAN_SOURCE_AD (m_nTxDMAChannel));
	unsigned nIndex = (nSource - m_nTxRingBus) / 4;

	return nIndex >= TxRingWords ? 0 : nIndex;
}

void CI2SLink::WriteTx (u64 nPos, const u32 *pWords, unsigned nWords)
{
	for (unsigned i = 0; i < nWords; i++)
	{
		unsigned nIndex = (nPos + i) % TxRingWords;
		m_pTxRing[nIndex] = pWords ? pWords[i] : 0;
		if (i == nWords - 1 || (nIndex & 7) == 7 || nIndex == TxRingWords - 1)
		{
			CleanAndInvalidateDataCacheRange ((uintptr) &m_pTxRing[nIndex & ~7], 32);
		}
	}
}

void CI2SLink::UpdateTx (void)
{
	unsigned nIndex = GetTxReadIndex ();
	m_nTxPos += (nIndex + TxRingWords - m_nTxLastIndex) % TxRingWords;
	m_nTxLastIndex = nIndex;

	while (m_nPendingCount)
	{
		TPending &P = m_Pending[m_nPendingHead];
		if (m_nTxPos < P.nStart + P.nWords)
		{
			break;				// not read by the DMA yet
		}
		if (m_nTxPos >= P.nStart + TxRingWords)
		{
			m_Stats.nTxLate++;
		}

		WriteTx (P.nStart, nullptr, P.nWords);

		m_nPendingHead = (m_nPendingHead + 1) % MaxPending;
		m_nPendingCount--;
	}
}

boolean CI2SLink::SendReply (u8 uchOpcode, const u32 *pPayload, unsigned nLength)
{
	UpdateTx ();

	unsigned nWords = nLength + 2;
	if (   nWords > MaxReplyWords
	    || m_nPendingCount == MaxPending)
	{
		m_Stats.nRepliesDropped++;
		return FALSE;
	}

	u64 nMin = m_nTxPos + TxMarginWords;
	if (m_nTxNext < nMin)
	{
		m_nTxNext = nMin;
	}
	if (m_nTxNext + nWords > m_nTxPos + TxRingWords - TxMarginWords)
	{
		m_Stats.nRepliesDropped++;		// the Pico doesn't clock: backlog full
		return FALSE;
	}

	u32 Packet[MaxReplyWords];
	Packet[0] = PGPU_HEADER (PGPU_SYNC_REPLY, uchOpcode, nLength);
	u32 nCRC = CRC (PGPU_CRC_INIT, Packet[0]);
	for (unsigned i = 0; i < nLength; i++)
	{
		Packet[1 + i] = pPayload[i];
		nCRC = CRC (nCRC, pPayload[i]);
	}
	Packet[1 + nLength] = nCRC ^ 0xFFFFFFFF;

	WriteTx (m_nTxNext, Packet, nWords);

	TPending &P = m_Pending[(m_nPendingHead + m_nPendingCount) % MaxPending];
	P.nStart = m_nTxNext;
	P.nWords = nWords;
	m_nPendingCount++;

	m_nTxNext += nWords;
	m_Stats.nReplies++;

	return TRUE;
}

unsigned CI2SLink::GetFreeBytes (void) const
{
	return (RingWords - Available ()) * 4;
}

unsigned CI2SLink::GetWriteIndex (void) const
{
	u32 nDest = read32 (ARM_DMACHAN_DEST_AD (m_nDMAChannel));
	unsigned nIndex = (nDest - m_nRingBus) / 4;

	return nIndex >= RingWords ? 0 : nIndex;	// at the wrap the CB is reloaded
}

unsigned CI2SLink::Available (void) const
{
	return (GetWriteIndex () + RingWords - m_nRead) % RingWords;
}

// The DMA writes the ring behind the ARM's cache: invalidate the lines that
// have arrived since the last call (the ARM never writes the ring).
void CI2SLink::InvalidateNew (void)
{
	unsigned nWrite = GetWriteIndex ();
	unsigned nFrom = m_nInvalidated & ~7;		// 8 words = one 32 byte line

	if (nWrite >= nFrom)
	{
		CleanAndInvalidateDataCacheRange ((uintptr) (m_pRing + nFrom), (nWrite - nFrom) * 4 + 32);
	}
	else
	{
		CleanAndInvalidateDataCacheRange ((uintptr) (m_pRing + nFrom), (RingWords - nFrom) * 4);
		CleanAndInvalidateDataCacheRange ((uintptr) m_pRing, nWrite * 4 + 32);
	}

	m_nInvalidated = nWrite;
}

u32 CI2SLink::CRC (u32 nCRC, u32 nWord) const
{
	for (unsigned i = 0; i < 4; i++, nWord >>= 8)	// little endian bytes
	{
		nCRC = m_CRCTable[(nCRC ^ nWord) & 0xFF] ^ (nCRC >> 8);
	}

	return nCRC;
}

const u32 *CI2SLink::GetPacket (u32 *pHeader)
{
	UpdateReady ();
	UpdateTx ();
	InvalidateNew ();

	// words that are in the ring and fresh in the cache (read the DMA position
	// once: a peripheral register read per word would be far too slow)
	unsigned nAvailable = (m_nInvalidated + RingWords - m_nRead) % RingWords;

	while (1)
	{
		if (nAvailable == 0)
		{
			return nullptr;
		}

		u32 nHeader = m_pRing[m_nRead];
		if (nHeader == PGPU_IDLE_WORD)
		{
			m_nRead = (m_nRead + 1) % RingWords;
			nAvailable--;
			m_Stats.nWords++;
			m_Stats.nIdleWords++;
			continue;
		}

		unsigned nLength = PGPU_HEADER_LEN (nHeader);
		if (   PGPU_HEADER_SYNC (nHeader) != PGPU_SYNC_COMMAND
		    || nLength > PGPU_MAX_PAYLOAD)
		{
			m_nRead = (m_nRead + 1) % RingWords;
			nAvailable--;
			m_Stats.nWords++;
			m_Stats.nGarbageWords++;
			continue;
		}

		if (nAvailable < nLength + 2)
		{
			return nullptr;			// packet not complete yet
		}

		// copy the payload and check the CRC
		u32 nCRC = CRC (PGPU_CRC_INIT, nHeader);
		unsigned nIndex = (m_nRead + 1) % RingWords;
		for (unsigned i = 0; i < nLength; i++)
		{
			u32 nWord = m_pRing[nIndex];
			m_pPacket[i] = nWord;
			nCRC = CRC (nCRC, nWord);
			nIndex = (nIndex + 1) % RingWords;
		}
		nCRC ^= 0xFFFFFFFF;

		if (nCRC != m_pRing[nIndex])
		{
			// drop the header word only and search again (resynchronisation)
			m_nRead = (m_nRead + 1) % RingWords;
			nAvailable--;
			m_Stats.nWords++;
			m_Stats.nCRCErrors++;
			m_nTotalCRCErrors++;

			u32 Error[3] = {PGPU_ERR_CRC, 0, nHeader};
			SendReply (PGPU_REPLY_ERROR, Error, 3);
			continue;
		}

		m_nRead = (nIndex + 1) % RingWords;
		m_Stats.nWords += nLength + 2;
		m_Stats.nPackets++;

		*pHeader = nHeader;
		return m_pPacket;
	}
}

void CI2SLink::UpdateReady (void)
{
	unsigned nFill = Available ();
	if (nFill > m_Stats.nMaxFill)
	{
		m_Stats.nMaxFill = nFill;
	}

	unsigned nFree = RingWords - nFill;
	if (m_bReady && nFree < ReadyLowWords)
	{
		m_bReady = FALSE;
		m_PinReady.Write (LOW);
		m_Stats.nReadyLow++;
	}
	else if (!m_bReady && nFree >= ReadyHighWords)
	{
		m_bReady = TRUE;
		m_PinReady.Write (HIGH);
	}
}

TI2SLinkStats CI2SLink::GetStats (void)
{
	TI2SLinkStats Stats = m_Stats;
	memset (&m_Stats, 0, sizeof m_Stats);

	return Stats;
}
