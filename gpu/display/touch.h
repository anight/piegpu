//
// touch.h
//
// The panel's touch screen: an XPT2046 on the panel's SPI0 with its own chip
// select, CE1 (GPIO7, pin 26), sharing SCLK, MOSI and MISO (GPIO11, 10, 9;
// the panel lets MISO go when it isn't answering). T_IRQ isn't used: the
// pressure is read each time.
//
// Read at 2 MHz right after each frame's pixels have gone to the panel: the
// panel driver's aux transfer, chained to the last DMA chunk (about 150 us);
// while no frames go to the panel (HDMI, or nothing drawn), from the main
// loop every 16 ms. A reading: the pressure (Z1, Z2), then Y eight times and
// X eight times, the controller powered down after them. Touched at a
// pressure of 300 and more with the X and Y samples agreeing, twice running;
// let go when Z1 reads near 0, twice running. Other readings change nothing:
// about one a second comes with Z2 at full scale and a Z1 that says pressed
// (they made phantom presses, and let go of a held touch).
//
// The samples are noisy: under a held finger one is off by 2 pixels (rms; up
// to 10), whatever the SPI clock (measured at 2, 1 and 0.5 MHz: 2.3, 2.1,
// 1.6 pixels), and the position jumped back and forth 30 times a second. So
// a reading's position is the mean of the middle four of its eight samples,
// and the one reported is steadied over the readings: smoothed (a quarter of
// the way to each new one), and followed 1.25 pixels behind, where a held
// finger's noise doesn't reach (on the recorded readings of a finger held and
// dragged: 0 to 0.7 changes of direction a second, the finger's own among
// them).
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
	static int Middle (const u8 *p, int *pSpread);
	static void Follow (int nPosition, int *pSmooth, int *pShown);

private:
	static const unsigned ClockSpeed = 2000000;	// the XPT2046's (125 k conversions a second)
	static const unsigned ChipSelect = 1;		// CE1
	static const unsigned Samples = 8;		// of Y, and of X, in a reading
	static const unsigned Bytes = 2 * (2 + 2 * Samples) + 2;	// a reading: its commands, 16 clocks each
	static const unsigned IdleUs = 16000;		// between readings without frames
	static const int Pressed = 300;
	static const int MinZ1 = 20;			// under it: not touched
	static const int MaxZ2 = 4000;			// over it with a Z1 that says pressed: a bad reading
	static const int MaxSpread = 128;		// a touch's X (and Y) samples this close (the middle four)
	static const int Jump = 12 * 16;		// a position this far off the smoothed one: taken at once
	static const int Slack = 20;			// the reported one follows this far behind (1/16 pixels)

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

	// the position steadied (1/16 pixels): smoothed, and the one reported
	boolean m_bTracking;
	int m_nSmoothX, m_nSmoothY, m_nShownX, m_nShownY;
};

#endif
