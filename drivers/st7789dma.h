//
// st7789dma.h
//
// ST7789 SPI display driver with DMA pixel transfers (CSPIMasterDMA on SPI0).
// Commands and window setup are sent polled; pixel data (SetArea) goes out by
// DMA in chunks of up to 64 KB, each chunk's completion interrupt starting the
// next. SetArea() can run asynchronously (with a completion routine), so the
// caller can prepare the next frame while the current one is transferred.
//
// Another device on the bus (another chip select, a slower clock: a touch
// controller) gets a short transfer after each frame's pixels, chained to the
// last chunk's completion, so it never waits for the panel nor the panel for
// it; AuxNow runs it between frames when none go to the panel.
//
// Init sequence from Circle's CST7789Display (GPLv3).
//
#ifndef _drivers_st7789dma_h
#define _drivers_st7789dma_h

#include <circle/display.h>
#include <circle/spimasterdma.h>
#include <circle/interrupt.h>
#include <circle/gpiopin.h>
#include <circle/types.h>

class CST7789DMADisplay : public CDisplay	/// ST7789 with DMA pixel transfers
{
public:
	static const unsigned None = GPIO_PINS;

public:
	/// \param pInterrupt Interrupt system (for the DMA completion interrupts)
	/// \param nDCPin GPIO pin number for DC
	/// \param nResetPin GPIO pin number for RESET (or None)
	/// \param nWidth Width in pixels (320 for a 240x320 panel, landscape)
	/// \param nHeight Height in pixels
	/// \param nClockSpeed SPI clock in Hz (core clock / even divider)
	/// \param nChipSelect SPI0 chip select (0 or 1)
	/// \param bLittleEndian Pixels are little endian RGB565 (e.g. from the V3D),
	///	   the panel is told so (RAMCTRL); otherwise big endian RGB565
	CST7789DMADisplay (CInterruptSystem *pInterrupt,
			   unsigned nDCPin, unsigned nResetPin,
			   unsigned nWidth, unsigned nHeight,
			   unsigned nClockSpeed, unsigned nChipSelect = 0,
			   boolean bLittleEndian = FALSE);

	~CST7789DMADisplay (void);

	boolean Initialize (void);

	unsigned GetWidth (void) const		{ return m_nWidth; }
	unsigned GetHeight (void) const		{ return m_nHeight; }
	unsigned GetDepth (void) const		{ return 16; }

	/// \brief Fill the display with a raw color (in the display's RGB565 byte order), polled
	void Clear (TRawColor nColor = 0);

	void SetPixel (unsigned nPosX, unsigned nPosY, TRawColor nColor);

	/// \brief Transfer an area by DMA
	/// \param pPixels RGB565 (byte order as configured), 4-byte aligned, unchanged until done
	/// \param pRoutine If given, returns immediately and calls pRoutine (from
	///	   interrupt context) when the transfer is done; otherwise waits
	void SetArea (const TArea &rArea, const void *pPixels,
		      TAreaCompletionRoutine *pRoutine = nullptr,
		      void *pParam = nullptr);

	/// \return Is a DMA transfer running?
	boolean IsBusy (void) const		{ return m_bBusy; }

	/// \brief Wait until a running DMA transfer has finished
	void WaitIdle (void);

	/// \brief Called with the bytes read by the aux transfer (from interrupt
	///	   context after a frame, from AuxNow's caller otherwise)
	typedef void TAuxRoutine (const u8 *pRx, void *pParam);

	/// \brief The transfer to make on nChipSelect at nClockSpeed after each
	///	   frame (nBytes of pTx out, as many in; up to MaxAux)
	void SetAux (unsigned nChipSelect, unsigned nClockSpeed, const void *pTx, unsigned nBytes,
		     TAuxRoutine *pRoutine, void *pParam);
	static const unsigned MaxAux = 64;

	/// \brief Make the aux transfer now, polled, if the bus is idle (not
	///	   from an interrupt; the caller starts no frame meanwhile)
	/// \return FALSE: a frame is being sent (the aux transfer follows it)
	boolean AuxNow (void);

	/// \brief A polled transfer on another chip select at another clock
	///	   (the bus idle), the panel's clock back after it
	void Transfer (unsigned nChipSelect, unsigned nClockSpeed, const void *pTx, void *pRx, unsigned nBytes);

private:
	void SetWindow (unsigned x0, unsigned y0, unsigned x1, unsigned y1);
	void Command (u8 uchCmd);
	void Data (const void *pData, size_t nLength);
	void Data8 (u8 uchData)			{ Data (&uchData, 1); }

	void StartChunk (void);
	static void SPICompletion (boolean bStatus, void *pParam);
	static void AuxCompletion (boolean bStatus, void *pParam);
	void FrameDone (void);

private:
	CSPIMasterDMA m_SPI;
	CGPIOPin m_DCPin;
	CGPIOPin m_ResetPin;
	unsigned m_nResetPin;

	unsigned m_nWidth;
	unsigned m_nHeight;
	unsigned m_nChipSelect;
	unsigned m_nClockSpeed;
	boolean m_bLittleEndian;

	unsigned m_nAuxChipSelect;
	unsigned m_nAuxClockSpeed;
	unsigned m_nAuxBytes;			// 0: none
	u8 *m_pAuxTx, *m_pAuxRx;		// DMA buffers, cache line aligned
	TAuxRoutine *m_pAuxRoutine;
	void *m_pAuxParam;

	u8 *m_pDummyRx;				// RX DMA target (SPI is full duplex)

	const u8 * volatile m_pNext;		// next chunk of the running transfer
	volatile size_t m_nRemaining;
	volatile boolean m_bBusy;
	TAreaCompletionRoutine *m_pRoutine;
	void *m_pParam;
};

#endif
