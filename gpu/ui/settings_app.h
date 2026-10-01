//
// settings_app.h
//
// Settings on the panel (LVGL, gpu/ui/lv_conf.h), opened by a long press at
// any time: while it's open the panel is its (the host's frames are rendered
// but not shown, its touches not sent). The user's settings, applied as
// they're changed:
//
//   Brightness	the panel's backlight (5 .. 100%: never dark)
//   Volume	the sound's on HDMI
//   Touch	Calibrate: a target at each corner and the middle, the
//		readings fitted to touchcal= (again if off by more than 12
//		pixels); Test: a finger draws
//
// Done closes it; what changed goes to settings.txt (the kernel's routine).
//
#ifndef _gpu_ui_settings_app_h
#define _gpu_ui_settings_app_h

#include <st7789dma.h>
#include <circle/types.h>
#include <lvgl.h>
#include "../display/touch.h"
#include "../display/backlight.h"
#include "audio.h"

class CSettingsApp
{
public:
	typedef void TSaveRoutine (void *pParam);

	CSettingsApp (CTouch *pTouch, CBacklight *pBacklight, CAudio *pAudio);

	/// \param pDisplay The panel's driver (LVGL's flushes go to it)
	/// \param pSave Called on Done if something changed (to write settings.txt)
	void Initialize (CST7789DMADisplay *pDisplay, TSaveRoutine *pSave, void *pParam);

	void Open (void);
	boolean IsOpen (void) const		{ return m_bOpen; }

	/// \brief Call often while open (the main loop), with the touch's state
	/// \return FALSE once it's closed (Done)
	boolean Update (const u32 *pTouch);

private:
	enum TScreen { ScreenMain, ScreenCalibrate, ScreenTest };

	void ShowMain (void);
	void ShowCalibrate (const char *pNote = nullptr);
	void ShowTest (void);
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
	TSaveRoutine *m_pSave;
	void *m_pSaveParam;

	boolean m_bInitialized;
	boolean m_bOpen;
	boolean m_bChanged;
	TScreen m_Screen;
	lv_display_t *m_pLVDisplay;
	u32 *m_pBuffers[2];

	// the pointer for LVGL: ignored till the long press that opened it lets go
	boolean m_bWaitRelease;
	boolean m_bDown;
	int m_nX, m_nY;

	// the widgets the events tell apart
	lv_obj_t *m_pDone, *m_pBrightness, *m_pBrightnessValue, *m_pVolume, *m_pVolumeValue;
	lv_obj_t *m_pCalibrate, *m_pTest, *m_pBack, *m_pNote;
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
