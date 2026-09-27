//
// pcmslave: the BCM2835 PCM block as I2S clock and frame slave, receiving a
// known pattern from the Pico (experiments/pcmsrc) by DMA. Verifies every bit and
// measures the incoming bit clock from the RX word rate.
//
//   PCM_CLK  GPIO18  pin 12  <- Pico GP17 (BCLK)
//   PCM_FS   GPIO19  pin 35  <- Pico GP18 (FS)
//   PCM_DIN  GPIO20  pin 38  <- Pico GP16 (data)
//   GPIO21 (pin 40) is left as an input.
//
// Pattern word k (k = 0..8191, repeating): (k * 2654435761) ^ 0x5A5A0001
//
#include "kernel.h"
#include <circle/bcm2835.h>
#include <circle/memio.h>
#include <circle/synchronize.h>
#include <circle/util.h>

LOGMODULE ("pcmslave");

#define CS_A_RXERR		(1 << 16)
#define CS_A_DMAEN		(1 << 9)
#define CS_A_RXCLR		(1 << 4)
#define CS_A_RXON		(1 << 1)
#define CS_A_EN			(1 << 0)

#define MODE_A_CLKM		(1 << 23)	// clock slave (input)
#define MODE_A_CLKI		(1 << 22)	// sample on the rising edge
#define MODE_A_FSM		(1 << 21)	// frame sync slave (input)
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

#define PATTERN_WORDS		8192
#define PATTERN_MUL		2654435761U
#define PATTERN_XOR		0x5A5A0001U

#define MAX_WORDS		(1024 * 1024)
#define BURST_TIMEOUT_US	1000000
#define REPORT_SECONDS		4

static u32 Pattern (u32 k)
{
	return (k * PATTERN_MUL) ^ PATTERN_XOR;
}

static unsigned PopCount (u32 x)
{
	unsigned n = 0;
	for (; x; x &= x - 1)
	{
		n++;
	}
	return n;
}

static void *AllocAligned (size_t nSize)
{
	u8 *p = new u8[nSize + 64];
	return (void *) (((uintptr) p + 63) & ~(uintptr) 63);
}

CKernel::CKernel (void)
:	m_Timer (&m_Interrupt),
	m_Logger (m_Options.GetLogLevel (), &m_Timer),
	m_DevLink (&m_Interrupt),
	m_PinCLK (18, GPIOModeAlternateFunction0),
	m_PinFS (19, GPIOModeAlternateFunction0),
	m_PinDIN (20, GPIOModeAlternateFunction0),
	m_PinDOUT (21, GPIOModeInput),
	m_RxDMA (DMA_CHANNEL_NORMAL, &m_Interrupt),
	m_bDMADone (FALSE)
{
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

TShutdownMode CKernel::Run (void)
{
	LOGNOTE ("Compile time: " __DATE__ " " __TIME__);

	unsigned nWait = m_Timer.GetUptime () + 2;	// let the host connect
	while (m_Timer.GetUptime () < nWait)
	{
		m_DevLink.Update ();
	}

	// modular inverse of the pattern multiplier (Newton iteration)
	u32 nInv = PATTERN_MUL;
	for (unsigned i = 0; i < 5; i++)
	{
		nInv *= 2 - PATTERN_MUL * nInv;
	}
	m_nPatternInverse = nInv;

	m_pRxBuffer = (u32 *) AllocAligned (MAX_WORDS * 4);

	SetupPCM ();
	LOGNOTE ("PCM slave: CLK pin 12, FS pin 35, DIN pin 38; I2S, 2 x 32-bit channels");

	unsigned nWords = 16384;
	unsigned nNextReport = m_Timer.GetUptime () + REPORT_SECONDS;
	u64 nChecked = 0, nWordErrors = 0, nBitErrors = 0, nRxUs = 0, nRxWords = 0;
	unsigned nBursts = 0, nNoSync = 0, nTimeouts = 0, nRxErr = 0;
	int nBitOffset = -1;

	while (1)
	{
		m_DevLink.Update ();

		unsigned nUs;
		if (!Burst (nWords, &nUs))
		{
			nTimeouts++;
			nWords = 16384;
		}
		else
		{
			nBursts++;
			nRxUs += nUs;
			nRxWords += nWords;
			if (m_nCSAtRxDone & CS_A_RXERR)
			{
				nRxErr++;
			}

			unsigned nC, nWE, nBE;
			int nOff;
			if (Verify (nWords, &nC, &nWE, &nBE, &nOff))
			{
				nChecked += nC;
				nWordErrors += nWE;
				nBitErrors += nBE;
				nBitOffset = nOff;
			}
			else
			{
				nNoSync++;
			}

			// aim at about 0.25 s per burst
			u64 nNext = (u64) nWords * 250000 / (nUs ? nUs : 1);
			nWords = nNext > MAX_WORDS ? MAX_WORDS : nNext < 4096 ? 4096 : (unsigned) nNext;
		}

		if (m_Timer.GetUptime () >= nNextReport)
		{
			unsigned nKHz = nRxUs ? (unsigned) (nRxWords * 32 * 1000 / nRxUs) : 0;
			LOGNOTE ("BCLK %u.%03u MHz | bursts %u, timeouts %u, no sync %u | "
				 "checked %u, word errors %u, bit errors %u | RXERR %u | offset %d",
				 nKHz / 1000, nKHz % 1000, nBursts, nTimeouts, nNoSync,
				 (unsigned) nChecked, (unsigned) nWordErrors, (unsigned) nBitErrors,
				 nRxErr, nBitOffset);

			nNextReport = m_Timer.GetUptime () + REPORT_SECONDS;
			nChecked = nWordErrors = nBitErrors = nRxUs = nRxWords = 0;
			nBursts = nNoSync = nTimeouts = nRxErr = 0;
		}
	}

	return ShutdownHalt;
}

void CKernel::SetupPCM (void)
{
	PeripheralEntry ();

	write32 (ARM_PCM_CS_A, 0);
	CTimer::SimpleusDelay (100);

	write32 (ARM_PCM_MODE_A,   MODE_A_CLKM | MODE_A_FSM | MODE_A_CLKI | MODE_A_FSI
				 | (63 << MODE_A_FLEN__SHIFT) | (32 << MODE_A_FSLEN__SHIFT));

	// I2S: each 32-bit channel starts one clock after the FS edge
	write32 (ARM_PCM_RXC_A,   XC_CH1WEX | XC_CH1EN | (1 << XC_CH1POS__SHIFT) | (8 << XC_CH1WID__SHIFT)
				| XC_CH2WEX | XC_CH2EN | (33 << XC_CH2POS__SHIFT) | (8 << XC_CH2WID__SHIFT));
	write32 (ARM_PCM_TXC_A, 0);

	write32 (ARM_PCM_CS_A, CS_A_EN);
	CTimer::SimpleusDelay (100);
	write32 (ARM_PCM_CS_A, CS_A_EN | CS_A_RXCLR);
	CTimer::SimpleusDelay (100);
	write32 (ARM_PCM_CS_A, CS_A_EN | CS_A_DMAEN);

	PeripheralExit ();
}

void CKernel::DMACompletion (unsigned nChannel, unsigned nBuffer, boolean bStatus, void *pParam)
{
	CKernel *pThis = static_cast<CKernel *> (pParam);
	pThis->m_bDMADone = TRUE;
}

boolean CKernel::Burst (unsigned nWords, unsigned *pRxUs)
{
	memset (m_pRxBuffer, 0, nWords * 4);
	CleanAndInvalidateDataCacheRange ((uintptr) m_pRxBuffer, nWords * 4);

	write32 (ARM_PCM_CS_A, CS_A_EN | CS_A_DMAEN | CS_A_RXCLR | CS_A_RXERR);
	CTimer::SimpleusDelay (20);		// FIFO clear needs 2 PCM clocks

	m_bDMADone = FALSE;
	m_RxDMA.SetupIORead (m_pRxBuffer, ARM_PCM_FIFO_A, nWords * 4, DREQSourcePCMRX);
	m_RxDMA.SetCompletionRoutine (DMACompletion, this);
	m_RxDMA.Start ();

	unsigned nStart = CTimer::GetClockTicks ();
	write32 (ARM_PCM_CS_A, CS_A_EN | CS_A_DMAEN | CS_A_RXON);

	while (!m_bDMADone)
	{
		if (CTimer::GetClockTicks () - nStart > BURST_TIMEOUT_US)
		{
			write32 (ARM_PCM_CS_A, CS_A_EN | CS_A_DMAEN);
			m_RxDMA.Cancel ();
			return FALSE;
		}
	}
	*pRxUs = CTimer::GetClockTicks () - nStart;
	m_nCSAtRxDone = read32 (ARM_PCM_CS_A);

	write32 (ARM_PCM_CS_A, CS_A_EN | CS_A_DMAEN);
	CleanAndInvalidateDataCacheRange ((uintptr) m_pRxBuffer, nWords * 4);

	return TRUE;
}

// Find the pattern at any bit offset in the RX stream, then compare all words.
boolean CKernel::Verify (unsigned nWords, unsigned *pChecked, unsigned *pWordErrors,
			 unsigned *pBitErrors, int *pBitOffset)
{
	auto RxAt = [this] (unsigned nWord, unsigned nShift, unsigned k) -> u32
	{
		u32 nHigh = m_pRxBuffer[nWord + k];
		return nShift ? (nHigh << nShift) | (m_pRxBuffer[nWord + k + 1] >> (32 - nShift)) : nHigh;
	};

	for (unsigned w = 2; w < 34; w++)		// skip words from before sync
	{
		for (unsigned s = 0; s < 32; s++)
		{
			u32 k = (RxAt (w, s, 0) ^ PATTERN_XOR) * m_nPatternInverse;
			if (   k >= PATTERN_WORDS
			    || RxAt (w, s, 1) != Pattern ((k + 1) % PATTERN_WORDS)
			    || RxAt (w, s, 2) != Pattern ((k + 2) % PATTERN_WORDS))
			{
				continue;
			}

			unsigned nWordErrors = 0, nBitErrors = 0, nChecked = 0;
			for (unsigned i = 0; w + i + 1 < nWords; i++, nChecked++)
			{
				u32 nDiff = RxAt (w, s, i) ^ Pattern ((k + i) % PATTERN_WORDS);
				if (nDiff)
				{
					nWordErrors++;
					nBitErrors += PopCount (nDiff);
				}
			}

			*pChecked = nChecked;
			*pWordErrors = nWordErrors;
			*pBitErrors = nBitErrors;
			*pBitOffset = w * 32 + s;

			return TRUE;
		}
	}

	return FALSE;
}
