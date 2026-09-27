//
// hdmi_output.h
//
// HDMI (the Zero's mini-HDMI): the firmware's framebuffer (Circle's
// CBcmFrameBuffer), RGB565 of any size - the firmware scales it to the HDMI
// mode - with three pages, which the renderer draws into directly
// (GetBuffers): Show makes a page the one on screen from the next vertical
// sync (SetVirtualOffset), WaitIdle waits for that sync. So frames go out at
// the monitor's rate, one a refresh, without tearing and without a copy: one
// page is on screen, one waits for the sync, one is drawn.
// SetSize allocates a new framebuffer (the firmware replaces the old one).
//
#ifndef _hdmi_output_h
#define _hdmi_output_h

#include "output.h"
#include <circle/bcmframebuffer.h>

class CHDMIOutput : public COutput
{
public:
	CHDMIOutput (void);
	~CHDMIOutput (void);

	boolean Initialize (void)		{ return SetSize (320, 240); }

	CDisplay *GetDisplay (void)		{ return m_pFrameBuffer; }

	/// \param nWidth a multiple of 16 (the framebuffer's pitch must be nWidth * 2)
	boolean SetSize (unsigned nWidth, unsigned nHeight);

	unsigned GetBuffers (u16 **ppBuffers, unsigned nMax);

	void Show (const void *pPixels, TDoneRoutine *pDone, void *pParam);

	void WaitIdle (void);

	const void *GetShownFrame (void);

	/// \brief Measure the HDMI refresh rate (times a few vertical syncs: about 0.2 s)
	/// \return Millihertz
	unsigned MeasureRefresh (void);

private:
	u8 *GetPage (unsigned nPage) const;

private:
	CBcmFrameBuffer *m_pFrameBuffer;
	unsigned m_nWidth;
	unsigned m_nHeight;
	static const unsigned Pages = 3;
	unsigned m_nShown;			// the page on screen
	boolean m_bFlipped;			// since the last vertical sync
};

#endif
