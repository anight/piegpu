//
// touch.h
//
// The panel's touch screen: an XPT2046 on the panel's SPI0 with its own chip
// select, CE1 (GPIO7, pin 26), sharing SCLK, MOSI and MISO (GPIO11, 10, 9;
// the panel lets MISO go when it isn't answering). T_IRQ isn't used: the
// pressure is read each time.
//
// Read at 2 MHz right after each frame's pixels have gone to the panel: the
// panel driver's aux transfer, chained to the last DMA chunk (about 80 us);
// while no frames go to the panel (HDMI, or nothing drawn), from the main
// loop every 16 ms. A reading as Circle's CXPT2046TouchScreen: the pressure
// (Z1, Z2), Y and X three times each (the best two of each averaged; one Y
// before them thrown away, the first is noisy), the controller powered down
// after them. Touched at a pressure of 300 and more with the X and Y readings
// agreeing, twice running; let go when Z1 reads near 0, twice running. Other
// readings change nothing: about one a second comes with Z2 at full scale
// and a Z1 that says pressed (they made phantom presses, and let go of a
// held touch).
//
// The position is the panel's pixels (320x240, as it's shown), from the
// controller's readings by the calibration (touchcal=, Initialize); the
// readings go to the host too (TOUCH, docs/protocol.md 9), to calibrate by.
//
#ifndef _gpu_display_touch_h
#define _gpu_display_touch_h

#include "panel_output.h"
#include <pgpu_protocol.h>
#include <circle/string.h>
#include <circle/types.h>

class CTouch
{
public:
	static const unsigned Words = PGPU_TOUCH_WORDS;

	CTouch (CPanelOutput *pPanel);

	/// \param pOption touch=auto (the default: a controller if one answers), off
	/// \param pCalibration touchcal=x0,x1,y0,y1,swap: the readings at the
	///	   panel's left and right edges, top and bottom; swap 1: its x from the
	///	   controller's Y (nullptr: the defaults)
	/// \return Is a controller there?
	boolean Initialize (const char *pOption, const char *pCalibration);

	boolean IsPresent (void) const		{ return m_bPresent; }

	/// \brief Call often (main loop): reads while the panel gets no frames
	/// \return TRUE if the touch changed (pressed, moved, let go): the TOUCH
	///	    reply's payload in pPayload
	boolean Update (u32 *pPayload);

	/// \brief The state now (the TOUCH reply's payload)
	void GetState (u32 *pPayload) const;

	/// \brief The calibration (as touchcal=: the readings at the panel's
	///	   left and right edges, top and bottom; bSwap: its x from the Y reading)
	void SetCalibration (int nX0, int nX1, int nY0, int nY1, boolean bSwap);
	/// \return touchcal='s value for it
	CString GetCalibration (void) const;

private:
	boolean Detect (void);
	static void AuxRoutine (const u8 *pRx, void *pParam);
	static int BestTwo (int a, int b, int c);
	static int Spread (int a, int b, int c);

private:
	static const unsigned ClockSpeed = 2000000;	// the XPT2046's (125 k conversions a second)
	static const unsigned ChipSelect = 1;		// CE1
	static const unsigned Bytes = 20;		// a reading: 10 commands of 16 clocks
	static const unsigned IdleUs = 16000;		// between readings without frames
	static const int Pressed = 300;
	static const int MinZ1 = 20;			// under it: not touched
	static const int MaxZ2 = 4000;			// over it with a Z1 that says pressed: a bad reading
	static const int MaxSpread = 64;		// a touch's X (and Y) readings this close

	CST7789DMADisplay *m_pDriver;
	boolean m_bPresent;

	// calibration: the readings at the panel's edges
	int m_nX0, m_nX1, m_nY0, m_nY1;
	boolean m_bSwap;

	// the last reading (the aux routine writes it, maybe in an interrupt)
	u8 m_Raw[Bytes];
	volatile unsigned m_nReadings;
	volatile unsigned m_nLastReading;		// CTimer::GetClockTicks ()
	unsigned m_nSeen;

	// the state
	boolean m_bDown;
	unsigned m_nGood;				// touched readings running (pressed on the second)
	unsigned m_nIdle;				// untouched ones (let go on the second)
	unsigned m_nPresses;
	u16 m_nX, m_nY, m_nRawX, m_nRawY, m_nPressure;
};

#endif
