//
// output.h
//
// Where finished frames go. The renderer draws into its own buffers (RGB565,
// little endian, rows top down) and shows each finished one here; the output
// takes the pixels (a DMA to the panel, a copy to a framebuffer, ...) and says
// when the buffer may be drawn into again. panel_output: the ST7789 on SPI0.
// An HDMI output is another class of this kind (e.g. over Circle's
// CBcmFrameBuffer, itself a CDisplay).
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

	/// \brief Show a whole frame (GetWidth x GetHeight RGB565 pixels)
	/// \param pDone Called (maybe from an interrupt) once the pixels have been taken
	virtual void Show (const void *pPixels, TDoneRoutine *pDone, void *pParam) = 0;

	/// \brief Wait until the frame shown last has been taken
	virtual void WaitIdle (void) = 0;
};

#endif
