//
// bt_uart.cpp
//
#include "bt_uart.h"
#include <circle/bcm2835.h>
#include <circle/bcm2835int.h>
#include <circle/bcmpropertytags.h>
#include <circle/machineinfo.h>
#include <circle/memio.h>
#include <circle/synchronize.h>
#include <circle/timer.h>
#include <circle/logger.h>

LOGMODULE ("btuart");

// the PL011's bits
#define FR_BUSY			(1 << 3)
#define FR_RXFE			(1 << 4)
#define FR_TXFF			(1 << 5)
#define FR_TXFE			(1 << 7)
#define LCRH_FEN		(1 << 4)
#define LCRH_WLEN8		(3 << 5)
#define CR_UARTEN		(1 << 0)
#define CR_TXE			(1 << 8)
#define CR_RXE			(1 << 9)
#define CR_RTSEN		(1 << 14)
#define CR_CTSEN		(1 << 15)
#define INT_RX			(1 << 4)
#define INT_TX			(1 << 5)
#define INT_RT			(1 << 6)	// (received, and the line idle since)
#define INT_OE			(1 << 10)
#define DR_OE			(1 << 11)
#define IFLS_RX_HALF_TX_EIGHTH	(2 << 3 | 0)

CBTUart::CBTUart (CInterruptSystem *pInterrupt)
:	m_pInterrupt (pInterrupt),
	m_bConnected (FALSE),
	m_nClock (0),
	m_nActualBaud (0),
	m_pRx (nullptr),
	m_pTx (nullptr),
	m_nRxIn (0), m_nRxOut (0), m_nTxIn (0), m_nTxOut (0),
	m_nLost (0)
{
}

boolean CBTUart::Initialize (unsigned nBaud)
{
	m_nClock = CMachineInfo::Get ()->GetClockRate (CLOCK_ID_UART);
	if (!m_nClock)
	{
		LOGWARN ("The UART's clock isn't known");
		return FALSE;
	}
	write32 (ARM_UART0_CR, 0);
	write32 (ARM_UART0_IMSC, 0);
	m_nRxIn = m_nRxOut = m_nTxIn = m_nTxOut = 0;
	if (!m_bConnected)			// (once: the rings, the pins, the interrupt)
	{
		m_pRx = new u8[RxBytes];
		m_pTx = new u8[TxBytes];
		m_TxD.AssignPin (32);
		m_TxD.SetMode (GPIOModeAlternateFunction3);
		m_RxD.AssignPin (33);
		m_RxD.SetMode (GPIOModeAlternateFunction3);
		m_RxD.SetPullMode (GPIOPullModeUp);
		m_CTS.AssignPin (30);
		m_CTS.SetMode (GPIOModeAlternateFunction3);
		m_CTS.SetPullMode (GPIOPullModeUp);
		m_RTS.AssignPin (31);
		m_RTS.SetMode (GPIOModeAlternateFunction3);
		m_pInterrupt->ConnectIRQ (ARM_IRQ_UART, InterruptHandler, this);
		m_bConnected = TRUE;
	}
	SetBaud (nBaud);
	LOGNOTE ("UART0 on GPIO32/33 (CTS/RTS on GPIO30/31): its clock %u Hz, %u baud", m_nClock, m_nActualBaud);
	return TRUE;
}

void CBTUart::SetBaud (unsigned nBaud)
{
	// what's on its way out first (100 ms at most)
	unsigned nStart = CTimer::GetClockTicks ();
	while (!IsTxIdle () && CTimer::GetClockTicks () - nStart < 100000)
	{
	}

	// the divider: the clock by 16 times the rate, in 64ths
	unsigned nDivider64 = (unsigned) (((u64) m_nClock * 4 + nBaud / 2) / nBaud);
	m_nActualBaud = (unsigned) ((u64) m_nClock * 4 / nDivider64);

	write32 (ARM_UART0_CR, 0);
	write32 (ARM_UART0_IMSC, 0);
	write32 (ARM_UART0_ICR, 0x7FF);
	write32 (ARM_UART0_IBRD, nDivider64 >> 6);
	write32 (ARM_UART0_FBRD, nDivider64 & 63);
	write32 (ARM_UART0_LCRH, LCRH_WLEN8 | LCRH_FEN);
	write32 (ARM_UART0_IFLS, IFLS_RX_HALF_TX_EIGHTH);
	write32 (ARM_UART0_IMSC, INT_RX | INT_RT | INT_OE);
	write32 (ARM_UART0_CR, CR_UARTEN | CR_TXE | CR_RXE | CR_RTSEN | CR_CTSEN);
}

boolean CBTUart::IsTxIdle (void) const
{
	return m_nTxIn == m_nTxOut && !(read32 (ARM_UART0_FR) & FR_BUSY);
}

unsigned CBTUart::Read (u8 *pBuffer, unsigned nMax)
{
	unsigned nIn = m_nRxIn, nOut = m_nRxOut, n = 0;
	while (n < nMax && nOut != nIn)
	{
		pBuffer[n++] = m_pRx[nOut];
		nOut = (nOut + 1) & (RxBytes - 1);
	}
	m_nRxOut = nOut;
	return n;
}

boolean CBTUart::Write (const u8 *pBuffer, unsigned nBytes)
{
	if (nBytes > GetTxFree ())
	{
		return FALSE;
	}
	unsigned nIn = m_nTxIn;
	for (unsigned i = 0; i < nBytes; i++)
	{
		m_pTx[nIn] = pBuffer[i];
		nIn = (nIn + 1) & (TxBytes - 1);
	}
	DataMemBarrier ();
	m_nTxIn = nIn;

	EnterCritical ();		// the first bytes into the FIFO: its interrupt takes over
	Pump ();
	LeaveCritical ();
	return TRUE;
}

void CBTUart::Pump (void)
{
	// received
	while (!(read32 (ARM_UART0_FR) & FR_RXFE))
	{
		u32 nData = read32 (ARM_UART0_DR);
		if (nData & DR_OE)
		{
			m_nLost = m_nLost + 1;
		}
		unsigned nNext = (m_nRxIn + 1) & (RxBytes - 1);
		if (nNext != m_nRxOut)
		{
			m_pRx[m_nRxIn] = (u8) nData;
			m_nRxIn = nNext;
		}
		else
		{
			m_nLost = m_nLost + 1;
		}
	}

	// to send: the FIFO filled; its interrupt while there's more
	while (m_nTxOut != m_nTxIn && !(read32 (ARM_UART0_FR) & FR_TXFF))
	{
		write32 (ARM_UART0_DR, m_pTx[m_nTxOut]);
		m_nTxOut = (m_nTxOut + 1) & (TxBytes - 1);
	}
	u32 nMask = INT_RX | INT_RT | INT_OE | (m_nTxOut != m_nTxIn ? INT_TX : 0);
	write32 (ARM_UART0_IMSC, nMask);
}

void CBTUart::InterruptHandler (void *pParam)
{
	CBTUart *pThis = static_cast<CBTUart *> (pParam);
	write32 (ARM_UART0_ICR, read32 (ARM_UART0_MIS));
	pThis->Pump ();
}
