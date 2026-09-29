//
// pcmtest: PCM/I2S block of the BCM2835 in master mode, stepping the bit
// clock from 1 MHz to 125 MHz (PLLD 500 MHz / integer divisor).
//
//   PCM_CLK  GPIO18  header pin 12   (scope: bit clock)
//   PCM_FS   GPIO19  header pin 35   (scope: frame sync = bit clock / 64)
//   PCM_DIN  GPIO20  header pin 38   \ loopback jumper 40 -> 38 lets the RPi
//   PCM_DOUT GPIO21  header pin 40   / verify every received word
//
// Frames: 64 bit clocks, two 32-bit channels back to back (all bits payload).
// TX and RX run on DMA. The bit clock is also measured from the RX word rate.
//
#include "kernel.h"
#include <circle/bcm2835.h>
#include <circle/memio.h>
#include <circle/synchronize.h>
#include <circle/util.h>

LOGMODULE ("pcmtest");

#define CS_A_RXERR		(1 << 16)
#define CS_A_TXERR		(1 << 15)
#define CS_A_SYNC		(1 << 24)
#define CS_A_DMAEN		(1 << 9)
#define CS_A_RXCLR		(1 << 4)
#define CS_A_TXCLR		(1 << 3)
#define CS_A_TXON		(1 << 2)
#define CS_A_RXON		(1 << 1)
#define CS_A_EN			(1 << 0)

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

#define MAX_WORDS		(1024 * 1024)	// 4 MB per buffer
#define STEP_SECONDS		20

// PLLD (500 MHz) divisors: 1, 5, 10, 20, 25, 31.25, 41.7, 50, 62.5, 83.3, 100, 125 MHz
#ifdef HIGH_STEPS_ONLY
static const unsigned Divisors[] = {10, 8, 6, 5, 4};		// 50 .. 125 MHz
#else
static const unsigned Divisors[] = {500, 100, 50, 25, 20, 16, 12, 10, 8, 6, 5, 4};
#endif

static u32 Pattern (unsigned nBurst, unsigned i)
{
	return (nBurst << 24) ^ (i * 2654435761U) ^ 0x5A5A0001;	// no zeros, all distinct
}

static void *AllocAligned (size_t nSize)
{
	u8 *p = new u8[nSize + 64];
	return (void *) (((uintptr) p + 63) & ~(uintptr) 63);
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

CKernel::CKernel (void)
:	m_Timer (&m_Interrupt),
	m_Logger (m_Options.GetLogLevel (), &m_Timer),
	m_DevLink (&m_Interrupt),
	m_PCMClock (GPIOClockPCM, GPIOClockSourcePLLD),
	m_PinCLK (18, GPIOModeAlternateFunction0),
	m_PinFS (19, GPIOModeAlternateFunction0),
	m_PinDIN (20, GPIOModeAlternateFunction0),
	m_PinDOUT (21, GPIOModeAlternateFunction0),
	m_nBurst (0)
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
	Wait (2);			// let the host connect

	m_pTxBuffer = (u32 *) AllocAligned (MAX_WORDS * 4);
	m_pRxBuffer = (u32 *) AllocAligned (MAX_WORDS * 4);

	LOGNOTE ("Scope: PCM_CLK pin 12 (GPIO18), PCM_FS pin 35 (= CLK/64), DOUT pin 40");
	LOGNOTE ("Loopback jumper pin 40 -> pin 38 enables data verification");
	LOGNOTE ("%u steps of %u s each", sizeof Divisors / sizeof Divisors[0], STEP_SECONDS);

	for (unsigned nStep = 0; nStep < sizeof Divisors / sizeof Divisors[0]; nStep++)
	{
		unsigned nDiv = Divisors[nStep];
		LOGNOTE ("=== STEP %u: PCM_CLK = 500 MHz / %u = %u.%03u MHz, FS = %u Hz",
			 nStep + 1, nDiv, 500 / nDiv, 500000 / nDiv % 1000, 500000000 / nDiv / 64);

		RunStep (nDiv, STEP_SECONDS);
	}

	m_PCMClock.Stop ();
	write32 (ARM_PCM_CS_A, 0);
	LOGNOTE ("=== DONE, PCM clock stopped");

	while (1)
	{
		m_DevLink.Update ();
	}

	return ShutdownHalt;
}

void CKernel::SetupPCM (void)
{
	PeripheralEntry ();

	write32 (ARM_PCM_CS_A, 0);
	CTimer::SimpleusDelay (100);

	// master mode (CLKM=0, FSM=0): frame 64 clocks, FS high for 32 clocks
	write32 (ARM_PCM_MODE_A, (63 << MODE_A_FLEN__SHIFT) | (32 << MODE_A_FSLEN__SHIFT));

	// two 32-bit channels (16 + 8 + 8), positions 0 and 32
	u32 nChannels =   XC_CH1WEX | XC_CH1EN | (0 << XC_CH1POS__SHIFT) | (8 << XC_CH1WID__SHIFT)
			| XC_CH2WEX | XC_CH2EN | (32 << XC_CH2POS__SHIFT) | (8 << XC_CH2WID__SHIFT);
	write32 (ARM_PCM_TXC_A, nChannels);
	write32 (ARM_PCM_RXC_A, nChannels);

	write32 (ARM_PCM_CS_A, CS_A_EN);
	CTimer::SimpleusDelay (100);
	write32 (ARM_PCM_CS_A, CS_A_EN | CS_A_TXCLR | CS_A_RXCLR);
	CTimer::SimpleusDelay (100);
	write32 (ARM_PCM_CS_A, CS_A_EN | CS_A_DMAEN);

	PeripheralExit ();
}

void CKernel::RunStep (unsigned nDivisor, unsigned nSeconds)
{
	write32 (ARM_PCM_CS_A, 0);
	m_PCMClock.Stop ();
	if (!m_PCMClock.Start (nDivisor))
	{
		LOGERR ("Cannot start PCM clock with divisor %u", nDivisor);
		return;
	}
	SetupPCM ();

	// about 0.25 s per burst: bit clock / 32 words per second
	unsigned nWordsPerSecond = 500000000 / nDivisor / 32;
	unsigned nWords = nWordsPerSecond / 4;
	if (nWords > MAX_WORDS)
	{
		nWords = MAX_WORDS;
	}
	if (nWords < 4096)
	{
		nWords = 4096;
	}

	unsigned nEnd = m_Timer.GetUptime () + nSeconds;
	unsigned nNextReport = m_Timer.GetUptime () + 4;
	u64 nWordsChecked = 0, nWordErrors = 0, nBitErrors = 0, nRxUsTotal = 0, nRxWords = 0;
	unsigned nBursts = 0, nNoSync = 0, nTxErr = 0, nRxErr = 0;
	int nLatency = -1;

	while (m_Timer.GetUptime () < nEnd)
	{
		m_DevLink.Update ();

		unsigned nRxUs, nWErr, nBErr;
		int nLat;
		boolean bSync = Burst (nWords, &nRxUs, &nWErr, &nBErr, &nLat);

		u32 nCS = m_nCSAtRxDone;
		if (nCS & CS_A_TXERR) nTxErr++;
		if (nCS & CS_A_RXERR) nRxErr++;

		nBursts++;
		nRxUsTotal += nRxUs;
		nRxWords += nWords;
		if (bSync)
		{
			nWordsChecked += nWords - nLat / 32 - 1;
			nWordErrors += nWErr;
			nBitErrors += nBErr;
			nLatency = nLat;
		}
		else
		{
			nNoSync++;
		}

		if (m_Timer.GetUptime () >= nNextReport)
		{
			// measured bit clock from the RX word rate (32 bit clocks per word)
			unsigned nKHz = (unsigned) (nRxWords * 32 * 1000 / nRxUsTotal);
			LOGNOTE ("  measured %u.%03u MHz | bursts %u, no loopback sync %u | "
				 "words checked %u, word errors %u, bit errors %u | "
				 "TXERR %u RXERR %u | loopback delay %d bits",
				 nKHz / 1000, nKHz % 1000, nBursts, nNoSync,
				 (unsigned) nWordsChecked, (unsigned) nWordErrors, (unsigned) nBitErrors,
				 nTxErr, nRxErr, nLatency);

			nNextReport += 4;
			nWordsChecked = nWordErrors = nBitErrors = nRxUsTotal = nRxWords = 0;
			nBursts = nNoSync = nTxErr = nRxErr = 0;
		}
	}
}

// One TX+RX DMA burst. Returns TRUE if the loopback data was found in RX.
boolean CKernel::Burst (unsigned nWords, unsigned *pRxUs, unsigned *pWordErrors,
			unsigned *pBitErrors, int *pLatency)
{
	m_nBurst++;
	for (unsigned i = 0; i < nWords; i++)
	{
		m_pTxBuffer[i] = Pattern (m_nBurst, i);
	}
	memset (m_pRxBuffer, 0, nWords * 4);
	CleanAndInvalidateDataCacheRange ((uintptr) m_pTxBuffer, nWords * 4);
	CleanAndInvalidateDataCacheRange ((uintptr) m_pRxBuffer, nWords * 4);

	// stop, clear FIFOs and error flags
	write32 (ARM_PCM_CS_A, CS_A_EN | CS_A_DMAEN | CS_A_TXCLR | CS_A_RXCLR
			       | CS_A_TXERR | CS_A_RXERR);
	CTimer::SimpleusDelay (20);		// FIFO clear needs 2 PCM clocks

	CDMAChannel RxDMA (DMA_CHANNEL_NORMAL);
	CDMAChannel TxDMA (DMA_CHANNEL_NORMAL);
	RxDMA.SetupIORead (m_pRxBuffer, ARM_PCM_FIFO_A, nWords * 4, DREQSourcePCMRX);
	TxDMA.SetupIOWrite (ARM_PCM_FIFO_A, m_pTxBuffer, nWords * 4, DREQSourcePCMTX);
	RxDMA.Start ();
	TxDMA.Start ();				// preloads the TX FIFO

	CTimer::SimpleusDelay (5);
	unsigned nStart = CTimer::GetClockTicks ();
	write32 (ARM_PCM_CS_A, CS_A_EN | CS_A_DMAEN | CS_A_TXON | CS_A_RXON);

	RxDMA.Wait ();
	*pRxUs = CTimer::GetClockTicks () - nStart;
	m_nCSAtRxDone = read32 (ARM_PCM_CS_A);	// error flags before RX can run on
	TxDMA.Wait ();

	// stop TX/RX, keep error flags for the caller
	write32 (ARM_PCM_CS_A, CS_A_EN | CS_A_DMAEN);

	CleanAndInvalidateDataCacheRange ((uintptr) m_pRxBuffer, nWords * 4);

	// Find where the TX bit stream shows up in RX: RX word j holds stream bits
	// [32j, 32j+32); TX word k starts at stream bit 32k + nDelay, where the
	// loopback delay nDelay = 32 * nWord + nShift (MSB first).
	auto RxAt = [this] (unsigned nWord, unsigned nShift, unsigned k) -> u32
	{
		u32 nHigh = m_pRxBuffer[nWord + k];
		return nShift ? (nHigh << nShift) | (m_pRxBuffer[nWord + k + 1] >> (32 - nShift)) : nHigh;
	};

	int nLatency = -1;			// in bits
	unsigned nWord = 0, nShift = 0;
	for (unsigned w = 0; w < 256 && w + 2 < nWords && nLatency < 0; w++)
	{
		for (unsigned b = 0; b < 32; b++)
		{
			if (   RxAt (w, b, 0) == m_pTxBuffer[0]
			    && RxAt (w, b, 1) == m_pTxBuffer[1])
			{
				nWord = w;
				nShift = b;
				nLatency = w * 32 + b;
				break;
			}
		}
	}

	*pLatency = nLatency;
	*pWordErrors = *pBitErrors = 0;
	if (nLatency < 0)
	{
		return FALSE;
	}

	for (unsigned k = 0; nWord + k + 1 < nWords; k++)
	{
		u32 nDiff = RxAt (nWord, nShift, k) ^ m_pTxBuffer[k];
		if (nDiff)
		{
			(*pWordErrors)++;
			*pBitErrors += PopCount (nDiff);
		}
	}

	return TRUE;
}

void CKernel::Wait (unsigned nSeconds)
{
	unsigned nEnd = m_Timer.GetUptime () + nSeconds;
	while (m_Timer.GetUptime () < nEnd)
	{
		m_DevLink.Update ();
	}
}
