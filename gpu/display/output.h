//
// output.h
//
// Where finished frames go. The renderer draws into its own buffers (RGB565,
// little endian, rows top down) and shows each finished one here; the output
// takes the pixels (a DMA to the panel, a copy to a framebuffer, ...) and says
// when the buffer may be drawn into again. panel_output: the ST7789 on SPI0;
// hdmi_output: the firmware's framebuffer (any size, scaled to the monitor),
// whose pages the renderer draws into directly (GetBuffers).
//
#ifndef _output_h
#define _output_h

#include <circle/display.h>
#include <circle/types.h>

class COutput
{
public:
	typedef CDisplay::TAreaCompletionRoutine TDoneRoutine;

	virtual ~COutput (void) {}

	virtual boolean Initialize (void) = 0;

	/// \return The display as a CDisplay (size; drawing before any frame: the splash)
	virtual CDisplay *GetDisplay (void) = 0;

	unsigned GetWidth (void)		{ return GetDisplay ()->GetWidth (); }
	unsigned GetHeight (void)		{ return GetDisplay ()->GetHeight (); }

	/// \brief Change the size (between frames, after WaitIdle)
	/// \return FALSE if the output can't have this size (it keeps its size then)
	virtual boolean SetSize (unsigned nWidth, unsigned nHeight)
	{
		return nWidth == GetWidth () && nHeight == GetHeight ();
	}

	/// \brief The output's own frame buffers, if the renderer can draw into
	///	   them (Show then just shows one: no copy); valid until SetSize
	/// \return Number of buffers (0: none, the renderer uses its own)
	virtual unsigned GetBuffers (u16 **ppBuffers, unsigned nMax)	{ return 0; }

	/// \brief Show a whole frame (GetWidth x GetHeight RGB565 pixels)
	/// \param pDone Called (maybe from an interrupt) once the pixels have been taken
	/// \brief The frame Show will get next: what can be done for it while
	///	   the frame before is still going out
	virtual void Prepare (const void *pPixels)	{ }

	virtual void Show (const void *pPixels, TDoneRoutine *pDone, void *pParam) = 0;

	/// \brief Wait until the frame shown last has been taken
	virtual void WaitIdle (void) = 0;

	/// \return The frame on screen as the output holds it (RGB565), for
	///	    screenshots; nullptr if the output keeps no copy (the panel)
	virtual const void *GetShownFrame (void)	{ return nullptr; }
};

#endif
