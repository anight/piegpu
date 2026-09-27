//
// hdmi_output.h
//
// HDMI (the Zero's mini-HDMI): the firmware's framebuffer (Circle's
// CBcmFrameBuffer), RGB565 of any size - the firmware scales it to the HDMI
// mode - with two pages: a frame is copied by DMA into the hidden one, which
// is then shown (SetVirtualOffset). WaitIdle waits for the vertical sync, so
// frames go out at the monitor's rate (60 Hz), one a refresh, without tearing.
// SetSize allocates a new framebuffer (the firmware replaces the old one).
//
#ifndef _hdmi_output_h
#define _hdmi_output_h

#include "output.h"
#include <circle/bcmframebuffer.h>
#include <circle/dmachannel.h>

class CHDMIOutput : public COutput
{
public:
	CHDMIOutput (void);
	~CHDMIOutput (void);

	boolean Initialize (void)		{ return SetSize (320, 240); }

	CDisplay *GetDisplay (void)		{ return m_pFrameBuffer; }

	/// \param nWidth a multiple of 16 (the framebuffer's pitch must be nWidth * 2)
	boolean SetSize (unsigned nWidth, unsigned nHeight);

	void Show (const void *pPixels, TDoneRoutine *pDone, void *pParam);

	void WaitIdle (void);

	const void *GetShownFrame (void);

private:
	u8 *GetPage (unsigned nPage) const;

private:
	CBcmFrameBuffer *m_pFrameBuffer;
	unsigned m_nWidth;
	unsigned m_nHeight;
	CDMAChannel m_DMA;
	unsigned m_nShown;			// the page on screen (0 or 1)
	boolean m_bFlipped;			// since the last vertical sync
};

#endif
