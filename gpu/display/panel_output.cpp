//
// panel_output.cpp
//
#include "panel_output.h"

#define SPI_CLOCK_SPEED		75000000
#define DC_PIN			24
#define RESET_PIN		25
#define WIDTH			320
#define HEIGHT			240

CPanelOutput::CPanelOutput (CInterruptSystem *pInterrupt)
:	m_Display (pInterrupt, DC_PIN, RESET_PIN, WIDTH, HEIGHT, SPI_CLOCK_SPEED, 0,
		   TRUE)			// little endian RGB565 from the V3D
{
}

void CPanelOutput::Show (const void *pPixels, TDoneRoutine *pDone, void *pParam)
{
	const CDisplay::TArea Full = {0, WIDTH - 1, 0, HEIGHT - 1};
	m_Display.SetArea (Full, pPixels, pDone, pParam);
}
