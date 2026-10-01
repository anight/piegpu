//
// panel_output.h
//
// The ST7789 panel (320x240) on SPI0 CE0, by DMA (drivers/st7789dma): 75 MHz
// = 300 MHz core / 4 (config.txt core_freq=300), D/C on GPIO24, reset on
// GPIO25. The V3D writes little endian RGB565; the driver turns each frame
// into the panel's own order (it scans 240 across, 320 down) while the frame
// before goes out, and starts it in step with the panel's scan: no tearing.
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

	// Is a panel there? Its registers over MISO (GPIO9), before Initialize:
	// it's reset, and read at about 500 kHz (the ST7789 reads slowly)
	struct TPanelInfo
	{
		boolean bPresent;
		u32 nID;			// RDDID: the module maker's ID1, ID2, ID3
		u32 nStatus;			// RDDST
		u8 nPowerMode;			// RDDPM
		u8 nMADCTL;			// RDDMADCTL
		u8 nPixelFormat;		// RDDCOLMOD
		u8 nSelfDiagnostic;		// RDDSDR
	};
	static boolean Detect (TPanelInfo *pInfo);

	boolean Initialize (void)		{ return m_Display.Initialize (); }

	CDisplay *GetDisplay (void)		{ return &m_Display; }

	void Prepare (const void *pPixels);
	void Show (const void *pPixels, TDoneRoutine *pDone, void *pParam);

	/// \brief Held: frames aren't shown (the Settings app has the panel)
	void SetHeld (boolean bHeld)		{ m_bHeld = bHeld; }
	boolean IsHeld (void) const		{ return m_bHeld; }

	void WaitIdle (void)			{ m_Display.WaitIdle (); }

	/// \brief The panel's driver: the touch controller on the same bus (touch.h)
	CST7789DMADisplay *GetDriver (void)	{ return &m_Display; }

private:
	CST7789DMADisplay m_Display;
	volatile boolean m_bHeld;
	static const unsigned HeldFrameUs = 16667;	// held: a frame's time all the same
	unsigned m_nHeldShow;
	const void *m_pPrepared;			// the frame made ready (Prepare)
};

#endif
