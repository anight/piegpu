//
// settings_app.cpp
//
#include "settings_app.h"
#include <circle/timer.h>
#include <circle/logger.h>
#include <circle/util.h>
#include <math.h>

LOGMODULE ("settings");

#define WIDTH		320
#define HEIGHT		240
#define BUFFER_LINES	48		// LVGL draws this many lines at a time (two buffers)
#define SLACK		12.0f		// the most a calibrated target may be off, pixels

static const int Target[5][2] = {{24, 24}, {296, 24}, {296, 216}, {24, 216}, {160, 120}};

static CSettingsApp *s_pThis;		// (LVGL's callbacks: one app)

static int Abs (int n)			{ return n < 0 ? -n : n; }

CSettingsApp::CSettingsApp (CTouch *pTouch, CBacklight *pBacklight, CAudio *pAudio)
:	m_pTouch (pTouch),
	m_pBacklight (pBacklight),
	m_pAudio (pAudio),
	m_pDisplay (nullptr),
	m_pSave (nullptr),
	m_pSaveParam (nullptr),
	m_bInitialized (FALSE),
	m_bOpen (FALSE),
	m_bChanged (FALSE),
	m_Screen (ScreenMain),
	m_pLVDisplay (nullptr),
	m_bWaitRelease (FALSE),
	m_bDown (FALSE),
	m_nX (0),
	m_nY (0),
	m_pCanvasPixels (nullptr)
{
	m_pBuffers[0] = m_pBuffers[1] = nullptr;
	s_pThis = this;
}

void CSettingsApp::Initialize (CST7789DMADisplay *pDisplay, TSaveRoutine *pSave, void *pParam)
{
	m_pDisplay = pDisplay;
	m_pSave = pSave;
	m_pSaveParam = pParam;
}

// ---- LVGL on the panel --------------------------------------------------------------------

uint32_t CSettingsApp::TickMs (void)
{
	return CTimer::GetClockTicks () / 1000;
}

void CSettingsApp::Flush (lv_display_t *pDisplay, const lv_area_t *pArea, u8 *pPixels)
{
	CDisplay::TArea Area = {(unsigned) pArea->x1, (unsigned) pArea->x2, (unsigned) pArea->y1, (unsigned) pArea->y2};
	s_pThis->m_pDisplay->SetArea (Area, pPixels, FlushDone, pDisplay);
}

void CSettingsApp::FlushDone (void *pParam)
{
	lv_display_flush_ready ((lv_display_t *) pParam);
}

void CSettingsApp::ReadPointer (lv_indev_t *pIndev, lv_indev_data_t *pData)
{
	pData->point.x = s_pThis->m_nX;
	pData->point.y = s_pThis->m_nY;
	pData->state = s_pThis->m_bDown && !s_pThis->m_bWaitRelease ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

void CSettingsApp::Open (void)
{
	if (!m_bInitialized)
	{
		lv_init ();
		lv_tick_set_cb (TickMs);
		m_pLVDisplay = lv_display_create (WIDTH, HEIGHT);
		lv_display_set_color_format (m_pLVDisplay, LV_COLOR_FORMAT_RGB565);
		unsigned nBytes = WIDTH * BUFFER_LINES * 2;
		m_pBuffers[0] = new u32[nBytes / 4];
		m_pBuffers[1] = new u32[nBytes / 4];
		lv_display_set_buffers (m_pLVDisplay, m_pBuffers[0], m_pBuffers[1], nBytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
		lv_display_set_flush_cb (m_pLVDisplay, Flush);
		lv_indev_t *pPointer = lv_indev_create ();
		lv_indev_set_type (pPointer, LV_INDEV_TYPE_POINTER);
		lv_indev_set_read_cb (pPointer, ReadPointer);
		lv_theme_t *pTheme = lv_theme_default_init (m_pLVDisplay, lv_palette_main (LV_PALETTE_BLUE),
							    lv_palette_main (LV_PALETTE_ORANGE), true, &lv_font_montserrat_14);
		lv_display_set_theme (m_pLVDisplay, pTheme);
		m_pCanvasPixels = new u16[WIDTH * HEIGHT];
		m_bInitialized = TRUE;
	}
	m_bOpen = TRUE;
	m_bChanged = FALSE;
	m_bWaitRelease = TRUE;
	ShowMain ();
	lv_obj_invalidate (lv_screen_active ());	// (all of it: the panel had the host's picture)
	LOGNOTE ("Settings opened");
}

boolean CSettingsApp::Update (const u32 *pTouch)
{
	if (!m_bOpen)
	{
		return FALSE;
	}
	boolean bDown = !!(pTouch[0] & PGPU_TOUCH_DOWN);
	m_nX = pTouch[1] & 0xFFFF;
	m_nY = pTouch[1] >> 16;
	if (!bDown)
	{
		m_bWaitRelease = FALSE;
	}

	if (m_Screen == ScreenCalibrate && !m_bWaitRelease)
	{
		// a target's readings: while pressed; taken when it's let go
		if (bDown)
		{
			m_Sum[0] += pTouch[2] & 0xFFFF;
			m_Sum[1] += pTouch[2] >> 16;
			m_nSamples++;
		}
		else if (m_nSamples)
		{
			m_Raw[m_nTarget][0] = m_Sum[0] / m_nSamples;
			m_Raw[m_nTarget][1] = m_Sum[1] / m_nSamples;
			m_Sum[0] = m_Sum[1] = 0.0f;
			m_nSamples = 0;
			if (++m_nTarget < Targets)
			{
				PlaceTarget ();
			}
			else if (FitCalibration ())
			{
				ShowMain ();
				lv_label_set_text (m_pNote, "Calibrated. The new touch: try Test");
			}
			else
			{
				ShowCalibrate ("Too far off: once more");
			}
		}
	}
	else if (m_Screen == ScreenTest && bDown && !m_bWaitRelease)
	{
		// dabs from the last point to this one (a new stroke when pressed again)
		if (!m_bDown)
		{
			m_nLastX = m_nX;
			m_nLastY = m_nY;
		}
		int dx = m_nX - m_nLastX, dy = m_nY - m_nLastY, n = (Abs (dx) > Abs (dy) ? Abs (dx) : Abs (dy)) / 2 + 1;
		for (int k = 1; k <= n; k++)
		{
			Dab (m_nLastX + dx * k / n, m_nLastY + dy * k / n);
		}
		m_nLastX = m_nX;
		m_nLastY = m_nY;
		lv_obj_invalidate (m_pCanvas);
		char Text[24];
		lv_snprintf (Text, sizeof Text, "%d, %d", m_nX, m_nY);
		lv_label_set_text (m_pPosition, Text);
	}
	m_bDown = bDown;

	lv_timer_handler ();
	return m_bOpen;
}

// ---- the screens -------------------------------------------------------------------------

static lv_obj_t *NewScreen (void)
{
	lv_obj_t *pOld = lv_screen_active ();
	lv_obj_t *pScreen = lv_obj_create (nullptr);
	lv_obj_remove_flag (pScreen, LV_OBJ_FLAG_SCROLLABLE);
	lv_screen_load (pScreen);
	if (pOld)
	{
		lv_obj_delete (pOld);
	}
	return pScreen;
}

static lv_obj_t *Label (lv_obj_t *pParent, const char *pText, lv_align_t Align, int x, int y)
{
	lv_obj_t *p = lv_label_create (pParent);
	lv_label_set_text (p, pText);
	lv_obj_align (p, Align, x, y);
	return p;
}

static lv_obj_t *Button (lv_obj_t *pParent, const char *pText, lv_align_t Align, int x, int y, int w, int h)
{
	lv_obj_t *p = lv_button_create (pParent);
	lv_obj_set_size (p, w, h);
	lv_obj_align (p, Align, x, y);
	lv_obj_t *pLabel = lv_label_create (p);
	lv_label_set_text (pLabel, pText);
	lv_obj_center (pLabel);
	return p;
}

static lv_obj_t *Slider (lv_obj_t *pParent, int y, int nMin, int nMax, int nValue)
{
	lv_obj_t *p = lv_slider_create (pParent);
	lv_obj_set_size (p, 140, 10);
	lv_obj_align (p, LV_ALIGN_TOP_LEFT, 120, y);
	lv_slider_set_range (p, nMin, nMax);
	lv_slider_set_value (p, nValue, LV_ANIM_OFF);
	lv_obj_set_ext_click_area (p, 12);		// (a finger's easier to catch it with)
	return p;
}

static void Percent (lv_obj_t *pLabel, int nValue)
{
	char Text[8];
	lv_snprintf (Text, sizeof Text, "%d%%", nValue);
	lv_label_set_text (pLabel, Text);
}

void CSettingsApp::ShowMain (void)
{
	m_Screen = ScreenMain;
	lv_obj_t *pScreen = NewScreen ();
	lv_obj_t *pTitle = Label (pScreen, LV_SYMBOL_SETTINGS "  Settings", LV_ALIGN_TOP_LEFT, 12, 14);
	lv_obj_set_style_text_font (pTitle, &lv_font_montserrat_20, 0);
	m_pDone = Button (pScreen, "Done", LV_ALIGN_TOP_RIGHT, -10, 8, 76, 34);

	Label (pScreen, LV_SYMBOL_IMAGE "  Brightness", LV_ALIGN_TOP_LEFT, 12, 66);
	m_pBrightness = Slider (pScreen, 70, 5, 100, m_pBacklight->GetBrightness ());
	m_pBrightnessValue = Label (pScreen, "", LV_ALIGN_TOP_RIGHT, -12, 66);
	Percent (m_pBrightnessValue, m_pBacklight->GetBrightness ());

	Label (pScreen, LV_SYMBOL_VOLUME_MAX "  Volume", LV_ALIGN_TOP_LEFT, 12, 112);
	m_pVolume = Slider (pScreen, 116, 0, 100, m_pAudio->GetDefaultVolume ());
	m_pVolumeValue = Label (pScreen, "", LV_ALIGN_TOP_RIGHT, -12, 112);
	Percent (m_pVolumeValue, m_pAudio->GetDefaultVolume ());

	Label (pScreen, LV_SYMBOL_EDIT "  Touch", LV_ALIGN_TOP_LEFT, 12, 162);
	m_pCalibrate = Button (pScreen, "Calibrate", LV_ALIGN_TOP_LEFT, 112, 152, 100, 36);
	m_pTest = Button (pScreen, "Test", LV_ALIGN_TOP_LEFT, 222, 152, 86, 36);

	m_pNote = Label (pScreen, "Long-press the screen anytime to come back", LV_ALIGN_BOTTOM_MID, 0, -10);
	lv_obj_set_style_text_font (m_pNote, &lv_font_montserrat_12, 0);
	lv_obj_set_style_text_color (m_pNote, lv_palette_lighten (LV_PALETTE_GREY, 1), 0);

	lv_obj_t *pEvents[] = {m_pDone, m_pBrightness, m_pVolume, m_pCalibrate, m_pTest};
	for (lv_obj_t *p : pEvents)
	{
		lv_obj_add_event_cb (p, OnEvent, LV_EVENT_ALL, this);
	}
}

void CSettingsApp::ShowCalibrate (const char *pNote)
{
	m_Screen = ScreenCalibrate;
	lv_obj_t *pScreen = NewScreen ();
	lv_obj_set_style_bg_color (pScreen, lv_color_black (), 0);
	m_pNote = Label (pScreen, pNote ? pNote : "Tap the targets", LV_ALIGN_TOP_MID, 0, 72);
	m_pPosition = Label (pScreen, "", LV_ALIGN_TOP_MID, 0, 160);
	for (int i = 0; i < 2; i++)		// the cross: a bar each way
	{
		m_pTarget[i] = lv_obj_create (pScreen);
		lv_obj_set_size (m_pTarget[i], i ? 3 : 29, i ? 29 : 3);
		lv_obj_set_style_bg_color (m_pTarget[i], lv_color_white (), 0);
		lv_obj_set_style_border_width (m_pTarget[i], 0, 0);
		lv_obj_set_style_radius (m_pTarget[i], 0, 0);
		lv_obj_remove_flag (m_pTarget[i], LV_OBJ_FLAG_CLICKABLE);
	}
	m_nTarget = 0;
	m_nSamples = 0;
	m_Sum[0] = m_Sum[1] = 0.0f;
	m_bWaitRelease = m_bDown;		// (the press on Calibrate isn't a target's)
	PlaceTarget ();
}

void CSettingsApp::PlaceTarget (void)
{
	int x = Target[m_nTarget][0], y = Target[m_nTarget][1];
	lv_obj_set_pos (m_pTarget[0], x - 14, y - 1);
	lv_obj_set_pos (m_pTarget[1], x - 1, y - 14);
	char Text[24];
	lv_snprintf (Text, sizeof Text, "%u of %u", m_nTarget + 1, Targets);
	lv_label_set_text (m_pPosition, Text);
}

void CSettingsApp::ShowTest (void)
{
	m_Screen = ScreenTest;
	lv_obj_t *pScreen = NewScreen ();
	m_pCanvas = lv_canvas_create (pScreen);
	lv_canvas_set_buffer (m_pCanvas, m_pCanvasPixels, WIDTH, HEIGHT, LV_COLOR_FORMAT_RGB565);
	lv_canvas_fill_bg (m_pCanvas, lv_color_hex (0x101418), LV_OPA_COVER);
	lv_obj_remove_flag (m_pCanvas, LV_OBJ_FLAG_CLICKABLE);
	m_pPosition = Label (pScreen, "Draw with a finger", LV_ALIGN_TOP_LEFT, 10, 12);
	m_pBack = Button (pScreen, "Back", LV_ALIGN_TOP_RIGHT, -10, 8, 76, 34);
	lv_obj_add_event_cb (m_pBack, OnEvent, LV_EVENT_ALL, this);
	m_bWaitRelease = m_bDown;		// (the press on Test isn't a stroke)
}

void CSettingsApp::Dab (int x, int y)
{
	for (int dy = -2; dy <= 2; dy++)
		for (int dx = -2; dx <= 2; dx++)
		{
			int px = x + dx, py = y + dy;
			if (px >= 0 && px < WIDTH && py >= 0 && py < HEIGHT && dx * dx + dy * dy <= 5)
			{
				m_pCanvasPixels[py * WIDTH + px] = 0xFE60;	// (amber, RGB565)
			}
		}
}

void CSettingsApp::OnEvent (lv_event_t *pEvent)
{
	CSettingsApp *pThis = (CSettingsApp *) lv_event_get_user_data (pEvent);
	lv_obj_t *pTarget = (lv_obj_t *) lv_event_get_target (pEvent);
	lv_event_code_t Code = lv_event_get_code (pEvent);
	if (Code == LV_EVENT_VALUE_CHANGED && pTarget == pThis->m_pBrightness)
	{
		int n = lv_slider_get_value (pTarget);
		pThis->m_pBacklight->SetBrightness (n);
		Percent (pThis->m_pBrightnessValue, n);
		pThis->m_bChanged = TRUE;
	}
	else if (Code == LV_EVENT_VALUE_CHANGED && pTarget == pThis->m_pVolume)
	{
		int n = lv_slider_get_value (pTarget);
		pThis->m_pAudio->SetVolume (n);
		Percent (pThis->m_pVolumeValue, n);
		pThis->m_bChanged = TRUE;
	}
	else if (Code != LV_EVENT_CLICKED)
	{
	}
	else if (pTarget == pThis->m_pDone)
	{
		if (pThis->m_bChanged && pThis->m_pSave)
		{
			(*pThis->m_pSave) (pThis->m_pSaveParam);
		}
		pThis->m_bOpen = FALSE;
		LOGNOTE ("Settings closed%s", pThis->m_bChanged ? " (saved)" : "");
	}
	else if (pTarget == pThis->m_pCalibrate)
	{
		pThis->ShowCalibrate ();
	}
	else if (pTarget == pThis->m_pTest)
	{
		pThis->ShowTest ();
	}
	else if (pTarget == pThis->m_pBack)
	{
		pThis->ShowMain ();
	}
}

// ---- the calibration ---------------------------------------------------------------------
//
// The readings at the targets give the panel's pixels by an affine map (least
// squares: its normal equations, 3 x 3, by Cramer's rule); the touch takes the
// nearest calibration of its kind (x from one reading, y from the other:
// touchcal=), checked against the targets.

static double Det3 (const double m[3][3])
{
	return   m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
	       + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
}

boolean CSettingsApp::FitCalibration (void)
{
	double a[3][3] = {{0}}, r[2][3] = {{0}}, c[2][3];
	for (unsigned i = 0; i < Targets; i++)
	{
		double v[3] = {m_Raw[i][0], m_Raw[i][1], 1.0};
		for (int j = 0; j < 3; j++)
		{
			for (int k = 0; k < 3; k++)
				a[j][k] += v[j] * v[k];
			r[0][j] += v[j] * Target[i][0];
			r[1][j] += v[j] * Target[i][1];
		}
	}
	double det = Det3 (a);
	if (det == 0.0)
	{
		return FALSE;
	}
	for (int o = 0; o < 2; o++)
		for (int col = 0; col < 3; col++)
		{
			double m[3][3];
			memcpy (m, a, sizeof m);
			for (int row = 0; row < 3; row++)
				m[row][col] = r[o][row];
			c[o][col] = Det3 (m) / det;
		}

	// touchcal=: x from the reading that moves it most; the readings at the edges
	boolean bSwap = fabs (c[0][1]) > fabs (c[0][0]);
	double xa = bSwap ? c[0][1] : c[0][0], ya = bSwap ? c[1][0] : c[1][1];
	int nX0 = (int) (-c[0][2] / xa), nX1 = (int) ((WIDTH - 1 - c[0][2]) / xa);
	int nY0 = (int) (-c[1][2] / ya), nY1 = (int) ((HEIGHT - 1 - c[1][2]) / ya);
	float fWorst = 0.0f;
	for (unsigned i = 0; i < Targets; i++)
	{
		float fA = bSwap ? m_Raw[i][1] : m_Raw[i][0], fB = bSwap ? m_Raw[i][0] : m_Raw[i][1];
		float x = (fA - nX0) * (WIDTH - 1) / (nX1 - nX0), y = (fB - nY0) * (HEIGHT - 1) / (nY1 - nY0);
		float e = sqrtf ((x - Target[i][0]) * (x - Target[i][0]) + (y - Target[i][1]) * (y - Target[i][1]));
		fWorst = e > fWorst ? e : fWorst;
	}
	LOGNOTE ("Calibration: touchcal=%d,%d,%d,%d,%u, off by up to %d pixels", nX0, nX1, nY0, nY1, bSwap ? 1 : 0,
		 (int) (fWorst + 0.5f));
	if (fWorst > SLACK || nX0 == nX1 || nY0 == nY1)
	{
		return FALSE;
	}
	m_pTouch->SetCalibration (nX0, nX1, nY0, nY1, bSwap);
	m_bChanged = TRUE;
	return TRUE;
}
