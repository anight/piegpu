//
// offscreen_output.h
//
// An output that shows nothing: the renderer draws the GL host's frames into
// its own buffers while the screen shows something else (the PC's desktop,
// gud_display). Any size.
//
#ifndef _offscreen_output_h
#define _offscreen_output_h

#include "output.h"
#include <circle/display.h>

class COffscreenOutput : public COutput
{
public:
	boolean Initialize (void) override		{ return TRUE; }

	CDisplay *GetDisplay (void) override		{ return &m_Display; }

	boolean SetSize (unsigned nWidth, unsigned nHeight) override
	{
		m_Display.m_nWidth = nWidth;
		m_Display.m_nHeight = nHeight;
		return TRUE;
	}

	void Show (const void *pPixels, TDoneRoutine *pDone, void *pParam) override
	{
		if (pDone)
		{
			(*pDone) (pParam);
		}
	}

	void WaitIdle (void) override {}

private:
	class CNoDisplay : public CDisplay
	{
	public:
		CNoDisplay (void) : CDisplay (RGB565), m_nWidth (320), m_nHeight (240) {}

		unsigned GetWidth (void) const override		{ return m_nWidth; }
		unsigned GetHeight (void) const override	{ return m_nHeight; }
		unsigned GetDepth (void) const override		{ return 16; }
		void SetPixel (unsigned nPosX, unsigned nPosY, TRawColor nColor) override {}
		void SetArea (const TArea &rArea, const void *pPixels,
			      TAreaCompletionRoutine *pRoutine = nullptr, void *pParam = nullptr) override
		{
			if (pRoutine)
			{
				(*pRoutine) (pParam);
			}
		}

		unsigned m_nWidth, m_nHeight;
	};

	CNoDisplay m_Display;
};

#endif
