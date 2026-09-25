//
// ili9486display.h
//
// Driver for ILI9486-based 3.5" 480x320 SPI display HATs
// ("3.5inch RPi LCD (A)", MPI3501, Waveshare, Goodtft, PiScreen style).
//
// On these boards the ILI9486 runs in 16-bit parallel mode behind shift
// registers, so everything sent over SPI must be 16-bit words: commands and
// parameters are sent as 0x00 followed by the byte, pixels as big endian RGB565.
//
// Based on CILI9341Display from Circle (GPLv3).
//
#ifndef _drivers_ili9486display_h
#define _drivers_ili9486display_h

#include <circle/display.h>
#include <circle/spimaster.h>
#include <circle/gpiopin.h>
#include <circle/types.h>

class CILI9486Display : public CDisplay	/// Driver for ILI9486 SPI display HATs
{
public:
	static const unsigned None = GPIO_PINS;

public:
	/// \param pSPIMaster Pointer to SPI master object
	/// \param nDCPin GPIO pin number for DC pin (HAT: 24)
	/// \param nResetPin GPIO pin number for Reset pin (HAT: 25, optional)
	/// \param nBackLightPin GPIO pin number for backlight pin (optional)
	/// \param nClockSpeed SPI clock frequency in Hz
	/// \param nChipSelect SPI chip select (HAT: 0)
	/// \note GPIO pin numbers are SoC number, not header positions.
	CILI9486Display (CSPIMaster *pSPIMaster,
			 unsigned nDCPin, unsigned nResetPin = None, unsigned nBackLightPin = None,
			 unsigned nClockSpeed = 16000000, unsigned nChipSelect = 0);

	~CILI9486Display (void);

	/// \brief Set the global rotation of the display
	/// \param nDegrees Rotation in degrees (0, 180: portrait 320x480; 90, 270: landscape 480x320)
	/// \note Must be set before calling Initialize().
	void SetRotation (unsigned nDegrees);
	/// \return Rotation angle in degrees (0, 90, 180, 270)
	unsigned GetRotation (void) const	{ return m_nRotation; }

	/// \return Operation successful?
	boolean Initialize (void);

	/// \return Display width in number of pixels
	unsigned GetWidth (void) const		{ return m_nWidth; }
	/// \return Display height in number of pixels
	unsigned GetHeight (void) const		{ return m_nHeight; }
	/// \return Number of bits per pixels
	unsigned GetDepth (void) const		{ return 16; }

	/// \brief Set display on
	void On (void);
	/// \brief Set display off
	void Off (void);

	/// \brief Clear entire display with color
	/// \param nColor Raw color value (RGB565_BE, default Black)
	void Clear (TRawColor nColor = 0);

	/// \brief Set a single pixel to color
	/// \param nPosX X-position (0..width-1)
	/// \param nPosY Y-postion (0..height-1)
	/// \param nColor Raw color value (RGB565_BE)
	void SetPixel (unsigned nPosX, unsigned nPosY, TRawColor nColor);

	/// \brief Set area (rectangle) on the display to the raw colors in pPixels
	/// \param rArea Coordinates of the area (zero-based)
	/// \param pPixels Pointer to array with raw color values (RGB565_BE)
	/// \param pRoutine Routine to be called on completion
	/// \param pParam User parameter to be handed over to completion routine
	void SetArea (const TArea &rArea, const void *pPixels,
		      TAreaCompletionRoutine *pRoutine = nullptr,
		      void *pParam = nullptr);

private:
	void SetWindow (unsigned x0, unsigned y0, unsigned x1, unsigned y1);

	void Command (u8 uchCmd)	{ CommandAndData (uchCmd, 0); }
	void CommandAndData (u8 uchCmd, unsigned nDataLen, ...);

	void Write (boolean bIsData, const void *pData, size_t nLength);

private:
	CSPIMaster *m_pSPIMaster;
	unsigned m_nResetPin;
	unsigned m_nBackLightPin;
	unsigned m_nWidth;
	unsigned m_nHeight;
	unsigned m_nClockSpeed;
	unsigned m_nChipSelect;

	unsigned m_nRotation;

	CGPIOPin m_DCPin;
	CGPIOPin m_ResetPin;
	CGPIOPin m_BackLightPin;
};

#endif
