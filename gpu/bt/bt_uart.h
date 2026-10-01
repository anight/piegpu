//
// bt_uart.h
//
// The wire to the board's Bluetooth controller (the Zero W's and the Zero
// 2 W's BCM4343x): the PL011 UART on GPIO32 (TXD) and GPIO33 (RXD), with its
// flow control lines on GPIO30 (CTS) and GPIO31 (RTS), all ALT3: pins inside
// the board, none on the header (GPIO14/15 stay untouched). Bytes go through
// two rings: the UART's interrupt fills the one and empties the other, so the
// main loop may be away for a while (a frame's rendering) without a byte lost.
//
#ifndef _gpu_bt_bt_uart_h
#define _gpu_bt_bt_uart_h

#include <circle/interrupt.h>
#include <circle/gpiopin.h>
#include <circle/types.h>

class CBTUart
{
public:
	static const unsigned RxBytes = 16384, TxBytes = 32768;	// (powers of 2)

	CBTUart (CInterruptSystem *pInterrupt);

	/// \brief The pins, the UART at this rate, its interrupt
	boolean Initialize (unsigned nBaud);
	/// \brief Another rate, once what's written has gone out
	void SetBaud (unsigned nBaud);
	unsigned GetBaud (void) const		{ return m_nActualBaud; }

	/// \return Bytes taken from what was received (up to nMax)
	unsigned Read (u8 *pBuffer, unsigned nMax);
	/// \brief Queue bytes to send: all of them, or none (FALSE: no room)
	boolean Write (const u8 *pBuffer, unsigned nBytes);
	unsigned GetTxFree (void) const		{ return TxBytes - 1 - ((m_nTxIn - m_nTxOut) & (TxBytes - 1)); }
	boolean IsTxIdle (void) const;

	/// \return Bytes lost: the UART's overruns, and those the ring had no room for
	unsigned GetLost (void) const		{ return m_nLost; }

private:
	void Pump (void);			// the rings and the FIFOs (interrupts off, or in the handler)
	static void InterruptHandler (void *pParam);

private:
	CInterruptSystem *m_pInterrupt;
	CGPIOPin m_TxD, m_RxD, m_CTS, m_RTS;
	boolean m_bConnected;
	unsigned m_nClock;			// the UART's, Hz
	unsigned m_nActualBaud;

	u8 *m_pRx, *m_pTx;
	volatile unsigned m_nRxIn, m_nRxOut;	// (in: the handler's, out: the reader's)
	volatile unsigned m_nTxIn, m_nTxOut;	// (in: the writer's, out: the handler's)
	volatile unsigned m_nLost;
};

#endif
