//
// settings_app.h
//
// Settings on the panel (LVGL, gpu/ui/lv_conf.h), opened by a long press at
// any time: while it's open the panel is its (the host's frames are rendered
// but not shown, its touches not sent; a host on I2S is told to wait).
//
//   Display	Brightness: the panel's backlight (5 .. 100%: never dark)
//		Touch: Calibrate (a target at each corner and the middle, the
//		readings fitted to touchcal=; again if off by more than 12
//		pixels), Test (a finger draws)
//   Audio	Volume: the sound's on HDMI; Mute; Test (a note on the left,
//		one on the right, one on both)
//		Bluetooth: on or off (for speakers, to come); greyed on a
//		board without it, and with the Zero's kernel (built without)
//   Kernel	the kernel command line's options (cmdline.txt), each a list
//		to choose from; the clocks' are the rates the firmware has
//		for this board, in MHz (the ARM's: its two ends; the V3D's:
//		its own while the ARM is at the low one, else the maximum).
//		They're read at the start: leaving the page with one changed
//		writes cmdline.txt and asks whether to restart now. A choice
//		that takes Settings itself away at the next start (the touch
//		screen off, no panel, the screen always on HDMI) asks first
//
// What it changes of the user's settings is applied at once; Done closes it
// and has them written (settings.txt). The kernel does the writing, and
// knows the board and the options: CSettingsHost.
//
#ifndef _gpu_ui_settings_app_h
#define _gpu_ui_settings_app_h

#include <st7789dma.h>
#include <circle/types.h>
#include <lvgl.h>
#include "../display/touch.h"
#include "../display/backlight.h"
#include "audio.h"

class CSettingsHost		/// what Settings asks of the kernel
{
public:
	virtual ~CSettingsHost (void) {}

	/// \brief The user's settings (the volume, the brightness, the touch's
	///	   calibration, Bluetooth, as they are now) into settings.txt
	virtual void SaveSettings (void) = 0;

	/// \return FALSE on a board without it, or with a kernel built without
	virtual boolean HasBluetooth (void) const = 0;
	/// \return What to say under its switch (why it's greyed, or what it does)
	virtual const char *GetBluetoothNote (void) const = 0;
	virtual boolean GetBluetooth (void) const = 0;
	virtual void SetBluetooth (boolean bOn) = 0;

	/// \brief The kernel command line as the card has it (cmdline.txt), to change
	virtual void LoadKernelOptions (void) = 0;
	/// \return The option's value there, or nullptr (not set)
	virtual const char *GetKernelOption (const char *pKey) = 0;
	/// \param pValue nullptr: the option removed
	virtual void SetKernelOption (const char *pKey, const char *pValue) = 0;
	/// \return FALSE if cmdline.txt couldn't be written (no card)
	virtual boolean SaveKernelOptions (void) = 0;
	/// \brief The rates the firmware has for the ARM's clock, or the V3D's
	virtual void GetClockRange (boolean bV3D, unsigned *pMinMHz, unsigned *pMaxMHz) = 0;

	/// \brief Play the test sound
	/// \return What to say of it
	virtual const char *TestSound (void) = 0;

	virtual void Restart (void) = 0;
};

class CSettingsApp
{
public:
	CSettingsApp (CTouch *pTouch, CBacklight *pBacklight, CAudio *pAudio);

	/// \param pDisplay The panel's driver (LVGL's flushes go to it)
	void Initialize (CST7789DMADisplay *pDisplay, CSettingsHost *pHost);

	void Open (void);
	boolean IsOpen (void) const		{ return m_bOpen; }

	/// \brief Call often while open (the main loop), with the touch's state
	/// \return FALSE once it's closed (Done)
	boolean Update (const u32 *pTouch);

	/// \return What it has on the panel (320 x 240, RGB565), for a screenshot
	const u16 *GetPicture (void) const	{ return m_pPicture; }

private:
	enum TScreen { ScreenRoot, ScreenDisplay, ScreenAudio, ScreenKernel, ScreenRestart, ScreenCalibrate, ScreenTest };
	static const unsigned KernelOptions = 9;
	static const unsigned MaxChoices = 8;

	void Forget (void);
	lv_obj_t *Page (TScreen Screen, const char *pTitle);
	void ShowRoot (void);
	void ShowDisplay (const char *pNote = nullptr);
	void ShowAudio (void);
	void ShowMuted (void);
	void ShowKernel (void);
	void FillOption (unsigned nOption);
	void SetOption (unsigned nOption, unsigned nChoice);
	void Warn (unsigned nOption, unsigned nChoice);
	void ShowRestart (boolean bWritten);
	void ShowCalibrate (const char *pNote = nullptr);
	void ShowTest (void);
	void Back (void);
	void PlaceTarget (void);
	boolean FitCalibration (void);
	void Dab (int x, int y);

	static void Flush (lv_display_t *pDisplay, const lv_area_t *pArea, u8 *pPixels);
	static void FlushDone (void *pParam);
	static void ReadPointer (lv_indev_t *pIndev, lv_indev_data_t *pData);
	static uint32_t TickMs (void);
	static void OnEvent (lv_event_t *pEvent);

private:
	CTouch *m_pTouch;
	CBacklight *m_pBacklight;
	CAudio *m_pAudio;
	CST7789DMADisplay *m_pDisplay;
	CSettingsHost *m_pHost;

	boolean m_bInitialized;
	boolean m_bOpen;
	boolean m_bChanged;			// the user's settings (settings.txt)
	boolean m_bKernelChanged;		// the kernel's (cmdline.txt), on its page
	TScreen m_Screen;
	lv_display_t *m_pLVDisplay;
	u32 *m_pBuffers[2];
	u16 *m_pPicture;			// what was sent to the panel (a screenshot's)

	// the pointer for LVGL: ignored till the long press that opened it lets go
	boolean m_bWaitRelease;
	boolean m_bDown;
	int m_nX, m_nY;

	// the widgets the events tell apart
	lv_obj_t *m_pDone, *m_pBack, *m_pMenu[3];
	lv_obj_t *m_pBrightness, *m_pBrightnessValue, *m_pCalibrate, *m_pTest, *m_pNote;
	lv_obj_t *m_pVolume, *m_pVolumeLabel, *m_pVolumeValue, *m_pMute, *m_pSoundTest, *m_pBluetooth;
	lv_obj_t *m_pOption[KernelOptions];
	char m_Value[KernelOptions][MaxChoices][16];	// a list's choices as the option's values ("": left out)
	unsigned m_nValues[KernelOptions];
	lv_obj_t *m_pRestart, *m_pLater;
	lv_obj_t *m_pWarning, *m_pYes, *m_pNo;	// a choice to confirm: the option, the choice
	unsigned m_nWarnOption, m_nWarnChoice;
	lv_obj_t *m_pTarget[2], *m_pCanvas, *m_pPosition;

	// calibrating: the targets' readings (those while pressed, averaged)
	static const unsigned Targets = 5;
	unsigned m_nTarget;
	float m_Sum[2];
	unsigned m_nSamples;
	float m_Raw[Targets][2];

	// testing: the canvas's pixels, the last point
	u16 *m_pCanvasPixels;
	int m_nLastX, m_nLastY;
};

#endif
