//
// panel_output.h
//
// The ST7789 panel (320x240) on SPI0 CE0, by DMA (drivers/st7789dma): 75 MHz
// = 300 MHz core / 4 (config.txt core_freq=300), D/C on GPIO24, reset on
// GPIO25. The V3D writes little endian RGB565, which the driver sends as is.
//
#ifndef _panel_output_h
#define _panel_output_h

#include "output.h"
#include <st7789dma.h>
#include <circle/interrupt.h>

class CPanelOutput : public COutput
{
public:
	CPanelOutput (CInterruptSystem *pInterrupt);

	boolean Initialize (void)		{ return m_Display.Initialize (); }

	CDisplay *GetDisplay (void)		{ return &m_Display; }

	void Show (const void *pPixels, TDoneRoutine *pDone, void *pParam);

	void WaitIdle (void)			{ m_Display.WaitIdle (); }

private:
	CST7789DMADisplay m_Display;
};

#endif
