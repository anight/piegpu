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
	m_pHost (nullptr),
	m_bInitialized (FALSE),
	m_bOpen (FALSE),
	m_bChanged (FALSE),
	m_bKernelChanged (FALSE),
	m_Screen (ScreenRoot),
	m_pLVDisplay (nullptr),
	m_pPicture (nullptr),
	m_bWaitRelease (FALSE),
	m_bDown (FALSE),
	m_nX (0),
	m_nY (0),
	m_pCanvasPixels (nullptr)
{
	m_pBuffers[0] = m_pBuffers[1] = nullptr;
	s_pThis = this;
}

void CSettingsApp::Initialize (CST7789DMADisplay *pDisplay, CSettingsHost *pHost)
{
	m_pDisplay = pDisplay;
	m_pHost = pHost;
}

// ---- LVGL on the panel --------------------------------------------------------------------

uint32_t CSettingsApp::TickMs (void)
{
	return CTimer::GetClockTicks () / 1000;
}

void CSettingsApp::Flush (lv_display_t *pDisplay, const lv_area_t *pArea, u8 *pPixels)
{
	CDisplay::TArea Area = {(unsigned) pArea->x1, (unsigned) pArea->x2, (unsigned) pArea->y1, (unsigned) pArea->y2};
	unsigned nWidth = pArea->x2 - pArea->x1 + 1;
	for (int y = pArea->y1; y <= pArea->y2; y++)		// (kept: a screenshot's)
	{
		memcpy (s_pThis->m_pPicture + y * WIDTH + pArea->x1, pPixels + (y - pArea->y1) * nWidth * 2, nWidth * 2);
	}
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
		m_pPicture = new u16[WIDTH * HEIGHT];
		memset (m_pPicture, 0, WIDTH * HEIGHT * 2);
		m_bInitialized = TRUE;
	}
	m_bOpen = TRUE;
	m_bChanged = FALSE;
	m_bKernelChanged = FALSE;
	m_bWaitRelease = TRUE;
	ShowRoot ();
	lv_obj_invalidate (lv_screen_active ());	// (all of it: the panel had the host's picture)
	LOGNOTE ("Settings opened");
}

static void Percent (lv_obj_t *pLabel, int nValue);

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
				ShowDisplay ("Calibrated. The new touch: try Test");
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

	// the volume changed elsewhere (a speaker's buttons): the bar follows
	if (m_Screen == ScreenAudio && m_pVolume && !lv_obj_has_state (m_pVolume, LV_STATE_PRESSED)
	    && lv_slider_get_value (m_pVolume) != (int) m_pAudio->GetDefaultVolume ())
	{
		lv_slider_set_value (m_pVolume, m_pAudio->GetDefaultVolume (), LV_ANIM_OFF);
		Percent (m_pVolumeValue, m_pAudio->GetDefaultVolume ());
	}

	// the Audio page: speakers looked for while it shows; what Bluetooth is
	// doing, and the list, as they change
	m_pHost->SetSpeakerSearch (m_Screen == ScreenAudio);
	if (m_Screen == ScreenAudio && m_pSpeakers)
	{
		const char *pNote = m_pHost->GetBluetoothNote ();
		if (strcmp (lv_label_get_text (m_pBluetoothNote), pNote) != 0)
		{
			lv_label_set_text (m_pBluetoothNote, pNote);
		}
		if (m_pHost->GetSpeakersGeneration () != m_nSpeakersGeneration)
		{
			ShowSpeakers ();
		}
	}

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

static lv_obj_t *Note (lv_obj_t *pParent, const char *pText, lv_align_t Align, int x, int y)
{
	lv_obj_t *p = Label (pParent, pText, Align, x, y);
	lv_obj_set_style_text_font (p, &lv_font_montserrat_12, 0);
	lv_obj_set_style_text_color (p, lv_palette_lighten (LV_PALETTE_GREY, 1), 0);
	return p;
}

// the last page's widgets are gone with it (the events tell them apart by these)
void CSettingsApp::Forget (void)
{
	m_pDone = m_pBack = m_pNote = nullptr;
	m_pMenu[0] = m_pMenu[1] = m_pMenu[2] = nullptr;
	m_pBrightness = m_pCalibrate = m_pTest = m_pVolume = m_pMute = m_pSoundTest = m_pBluetooth = nullptr;
	m_pRestart = m_pLater = m_pWarning = m_pYes = m_pNo = nullptr;
	m_pBluetoothNote = m_pSpeakers = m_pHeld = nullptr;
	m_nSpeakers = 0;
	m_ForgetAddress[0] = '\0';
	for (unsigned i = 0; i < KernelOptions; i++)
	{
		m_pOption[i] = nullptr;
	}
}

// a page: its title; Done on the first, a way back on the others
lv_obj_t *CSettingsApp::Page (TScreen Screen, const char *pTitle)
{
	m_Screen = Screen;
	lv_obj_t *pScreen = NewScreen ();
	Forget ();
	if (Screen == ScreenRoot)
	{
		m_pDone = Button (pScreen, "Done", LV_ALIGN_TOP_RIGHT, -10, 8, 76, 34);
		lv_obj_add_event_cb (m_pDone, OnEvent, LV_EVENT_CLICKED, this);
	}
	else if (Screen != ScreenRestart)
	{
		m_pBack = Button (pScreen, LV_SYMBOL_LEFT, LV_ALIGN_TOP_LEFT, 8, 8, 46, 34);
		lv_obj_add_event_cb (m_pBack, OnEvent, LV_EVENT_CLICKED, this);
	}
	lv_obj_t *pLabel = Label (pScreen, pTitle, LV_ALIGN_TOP_LEFT, m_pBack ? 66 : 12, 14);
	lv_obj_set_style_text_font (pLabel, &lv_font_montserrat_20, 0);
	return pScreen;
}

void CSettingsApp::ShowRoot (void)
{
	lv_obj_t *pScreen = Page (ScreenRoot, LV_SYMBOL_SETTINGS "  Settings");
	static const char *Names[3] = {LV_SYMBOL_IMAGE "   Display", LV_SYMBOL_AUDIO "   Audio", LV_SYMBOL_LIST "   Kernel"};
	for (int i = 0; i < 3; i++)
	{
		m_pMenu[i] = lv_button_create (pScreen);
		lv_obj_set_size (m_pMenu[i], 300, 44);
		lv_obj_align (m_pMenu[i], LV_ALIGN_TOP_MID, 0, 56 + i * 52);
		lv_obj_set_style_bg_color (m_pMenu[i], lv_palette_darken (LV_PALETTE_GREY, 3), 0);
		Label (m_pMenu[i], Names[i], LV_ALIGN_LEFT_MID, 2, 0);
		Label (m_pMenu[i], LV_SYMBOL_RIGHT, LV_ALIGN_RIGHT_MID, -2, 0);
		lv_obj_add_event_cb (m_pMenu[i], OnEvent, LV_EVENT_CLICKED, this);
	}
	Note (pScreen, "Hold a finger on the screen to come back here", LV_ALIGN_BOTTOM_MID, 0, -8);
}

void CSettingsApp::ShowDisplay (const char *pNote)
{
	lv_obj_t *pScreen = Page (ScreenDisplay, "Display");

	Label (pScreen, "Brightness", LV_ALIGN_TOP_LEFT, 12, 70);
	m_pBrightness = Slider (pScreen, 74, 5, 100, m_pBacklight->GetBrightness ());
	m_pBrightnessValue = Label (pScreen, "", LV_ALIGN_TOP_RIGHT, -12, 70);
	Percent (m_pBrightnessValue, m_pBacklight->GetBrightness ());
	lv_obj_add_event_cb (m_pBrightness, OnEvent, LV_EVENT_VALUE_CHANGED, this);

	Label (pScreen, "Touch", LV_ALIGN_TOP_LEFT, 12, 134);
	m_pCalibrate = Button (pScreen, "Calibrate", LV_ALIGN_TOP_LEFT, 112, 124, 100, 36);
	m_pTest = Button (pScreen, "Test", LV_ALIGN_TOP_LEFT, 222, 124, 86, 36);
	lv_obj_add_event_cb (m_pCalibrate, OnEvent, LV_EVENT_CLICKED, this);
	lv_obj_add_event_cb (m_pTest, OnEvent, LV_EVENT_CLICKED, this);
	if (!m_pTouch->IsPresent ())
	{
		lv_obj_add_state (m_pCalibrate, LV_STATE_DISABLED);
		lv_obj_add_state (m_pTest, LV_STATE_DISABLED);
	}
	m_pNote = Note (pScreen, pNote ? pNote : "", LV_ALIGN_BOTTOM_MID, 0, -8);
}

void CSettingsApp::ShowAudio (void)
{
	lv_obj_t *pScreen = Page (ScreenAudio, "Audio");

	// under the title: what scrolls (the speakers' list grows it)
	lv_obj_t *pBody = lv_obj_create (pScreen);
	lv_obj_set_size (pBody, WIDTH, HEIGHT - 50);
	lv_obj_align (pBody, LV_ALIGN_TOP_LEFT, 0, 50);
	lv_obj_set_style_border_width (pBody, 0, 0);
	lv_obj_set_style_radius (pBody, 0, 0);
	lv_obj_set_style_pad_all (pBody, 0, 0);
	lv_obj_set_style_bg_opa (pBody, LV_OPA_TRANSP, 0);
	lv_obj_set_scroll_dir (pBody, LV_DIR_VER);

	m_pVolumeLabel = Label (pBody, "Volume", LV_ALIGN_TOP_LEFT, 12, 10);
	m_pVolume = Slider (pBody, 14, 0, 100, m_pAudio->GetDefaultVolume ());
	m_pVolumeValue = Label (pBody, "", LV_ALIGN_TOP_RIGHT, -12, 10);
	Percent (m_pVolumeValue, m_pAudio->GetDefaultVolume ());
	lv_obj_add_event_cb (m_pVolume, OnEvent, LV_EVENT_VALUE_CHANGED, this);
	lv_color_t Grey = lv_palette_darken (LV_PALETTE_GREY, 2);		// (as it looks while muted)
	lv_obj_set_style_bg_color (m_pVolume, Grey, LV_PART_INDICATOR | LV_STATE_DISABLED);
	lv_obj_set_style_bg_color (m_pVolume, Grey, LV_PART_KNOB | LV_STATE_DISABLED);
	lv_obj_set_style_bg_color (m_pVolume, lv_palette_darken (LV_PALETTE_GREY, 4), LV_PART_MAIN | LV_STATE_DISABLED);
	lv_obj_set_style_bg_opa (m_pVolume, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DISABLED);

	Label (pBody, LV_SYMBOL_MUTE "  Mute", LV_ALIGN_TOP_LEFT, 12, 52);
	m_pMute = lv_switch_create (pBody);
	lv_obj_set_size (m_pMute, 56, 30);
	lv_obj_align (m_pMute, LV_ALIGN_TOP_LEFT, 112, 45);
	if (m_pAudio->IsMuted ())
	{
		lv_obj_add_state (m_pMute, LV_STATE_CHECKED);
	}
	lv_obj_add_event_cb (m_pMute, OnEvent, LV_EVENT_VALUE_CHANGED, this);
	ShowMuted ();
	m_pSoundTest = Button (pBody, LV_SYMBOL_PLAY "  Test", LV_ALIGN_TOP_RIGHT, -12, 42, 100, 36);
	lv_obj_add_event_cb (m_pSoundTest, OnEvent, LV_EVENT_CLICKED, this);
	m_pNote = Note (pBody, "", LV_ALIGN_TOP_LEFT, 12, 84);

	// Bluetooth: a board without it can't have it on. On: what it's doing,
	// and the speakers (ShowSpeakers, as they come and go)
	boolean bHas = m_pHost->HasBluetooth ();
	lv_obj_t *pLabel = Label (pBody, LV_SYMBOL_BLUETOOTH "  Bluetooth", LV_ALIGN_TOP_LEFT, 12, 116);
	m_pBluetooth = lv_switch_create (pBody);
	lv_obj_set_size (m_pBluetooth, 56, 30);
	lv_obj_align (m_pBluetooth, LV_ALIGN_TOP_RIGHT, -12, 109);
	if (bHas && m_pHost->GetBluetooth ())
	{
		lv_obj_add_state (m_pBluetooth, LV_STATE_CHECKED);
	}
	if (!bHas)
	{
		lv_obj_add_state (m_pBluetooth, LV_STATE_DISABLED);
		lv_obj_set_style_text_color (pLabel, lv_palette_darken (LV_PALETTE_GREY, 1), 0);
	}
	lv_obj_add_event_cb (m_pBluetooth, OnEvent, LV_EVENT_VALUE_CHANGED, this);
	m_pBluetoothNote = Note (pBody, m_pHost->GetBluetoothNote (), LV_ALIGN_TOP_LEFT, 12, 146);

	m_pSpeakers = lv_obj_create (pBody);
	lv_obj_set_size (m_pSpeakers, WIDTH - 20, LV_SIZE_CONTENT);
	lv_obj_align (m_pSpeakers, LV_ALIGN_TOP_LEFT, 10, 168);
	lv_obj_set_style_border_width (m_pSpeakers, 0, 0);
	lv_obj_set_style_pad_all (m_pSpeakers, 0, 0);
	lv_obj_set_style_pad_row (m_pSpeakers, 6, 0);
	lv_obj_set_style_pad_bottom (m_pSpeakers, 8, 0);
	lv_obj_set_style_bg_opa (m_pSpeakers, LV_OPA_TRANSP, 0);
	lv_obj_set_flex_flow (m_pSpeakers, LV_FLEX_FLOW_COLUMN);
	lv_obj_remove_flag (m_pSpeakers, LV_OBJ_FLAG_SCROLLABLE);
	ShowSpeakers ();
}

// the speakers' rows, as the list is now: its name, what it's doing; the connected one lit
void CSettingsApp::ShowSpeakers (void)
{
	CSettingsHost::TSpeaker Speakers[MaxSpeakers];
	m_nSpeakersGeneration = m_pHost->GetSpeakersGeneration ();
	m_nSpeakers = m_pHost->GetSpeakers (Speakers, MaxSpeakers);
	lv_obj_clean (m_pSpeakers);
	for (unsigned i = 0; i < m_nSpeakers; i++)
	{
		strcpy (m_SpeakerAddress[i], Speakers[i].Address);
		m_bSpeakerPaired[i] = Speakers[i].bPaired;
		lv_obj_t *pRow = m_pSpeaker[i] = lv_button_create (m_pSpeakers);
		lv_obj_set_size (pRow, WIDTH - 20, 38);
		lv_obj_set_style_bg_color (pRow, Speakers[i].bConnected ? lv_palette_main (LV_PALETTE_BLUE)
					   : lv_palette_darken (LV_PALETTE_GREY, 3), 0);
		lv_obj_t *pName = Label (pRow, Speakers[i].Name, LV_ALIGN_LEFT_MID, 0, 0);
		lv_obj_set_width (pName, 180);
		lv_label_set_long_mode (pName, LV_LABEL_LONG_DOT);
		lv_obj_t *pState = Label (pRow, Speakers[i].pState, LV_ALIGN_RIGHT_MID, 0, 0);
		lv_obj_set_style_text_font (pState, &lv_font_montserrat_12, 0);
		lv_obj_add_event_cb (pRow, OnEvent, LV_EVENT_CLICKED, this);
		lv_obj_add_event_cb (pRow, OnEvent, LV_EVENT_LONG_PRESSED, this);
	}
}

// muted: the volume greyed, and not to be moved
void CSettingsApp::ShowMuted (void)
{
	boolean bMuted = m_pAudio->IsMuted ();
	lv_obj_set_state (m_pVolume, LV_STATE_DISABLED, bMuted);
	lv_obj_t *pLabels[2] = {m_pVolumeLabel, m_pVolumeValue};
	for (unsigned i = 0; i < 2; i++)
	{
		if (bMuted)
		{
			lv_obj_set_style_text_color (pLabels[i], lv_palette_darken (LV_PALETTE_GREY, 1), 0);
		}
		else
		{
			lv_obj_remove_local_style_prop (pLabels[i], LV_STYLE_TEXT_COLOR, 0);
		}
	}
}

// The kernel's options (README "Kernel command line"): what each can be, the
// first its default; nullptr: the option left out. The clocks' choices are
// made for the board (FillOption)
enum { OptionCPU = 6, OptionV3D = 7 };
static const struct
{
	const char *pKey, *pName, *pChoices, *pValues[6];
	const char *pWarnValue, *pWarning;	// a value that takes Settings away at the next start
}
Options[] =
{
	{"host", "Commands from", "auto\nUSB\nI2S", {"auto", "usb", "i2s"}},
	{"output", "Screen", "auto\npanel\nHDMI", {"auto", "panel", "hdmi"},
	 "hdmi", "With the screen always on HDMI the panel shows nothing, Settings neither."},
	{"panel", "Panel", "auto\nyes\nnone", {"auto", "yes", "none"},
	 "none", "Without the panel there's no Settings."},
	{"hdmi_pixels", "HDMI size, at most", "native\n1920x1080\n1280x720\n1024x600\n800x480\n640x480",
	 {nullptr, "2073600", "921600", "614400", "384000", "307200"}},
	{"gud", "USB monitor", "off\non", {"off", "on"}},
	{"touch", "Touch screen", "auto\noff", {"auto", "off"},
	 "off", "Without the touch screen Settings can't be opened."},
	{"cpu", "CPU clock", nullptr, {"max", "low"}},
	{"v3d", "V3D clock", nullptr, {nullptr}},
	{"clcheck", "V3D job check", "on\noff", {"on", "off"}},
};

// An option's list: its choices, their values (m_Value), and the one that's
// set chosen. What the card has that isn't one of them: one more choice
void CSettingsApp::FillOption (unsigned i)
{
	char Choices[128] = "";
	unsigned n = 0;
	if (i == OptionCPU)			// the ARM's two ends
	{
		unsigned nMin, nMax;
		m_pHost->GetClockRange (FALSE, &nMin, &nMax);
		lv_snprintf (Choices, sizeof Choices, "%u MHz\n%u MHz", nMax, nMin);
		strcpy (m_Value[i][0], "max");
		strcpy (m_Value[i][1], "low");
		n = 2;
	}
	else if (i == OptionV3D)		// its maximum (the option left out), then down to its minimum
	{					// by 25 MHz; with the ARM at its maximum: only that
		unsigned nMin, nMax;
		m_pHost->GetClockRange (TRUE, &nMin, &nMax);
		const char *pCPU = m_pHost->GetKernelOption ("cpu");
		if (!pCPU || strcmp (pCPU, "low") != 0)
		{
			nMin = nMax;
		}
		for (unsigned nMHz = nMax; nMHz >= nMin && nMHz > 0 && n < MaxChoices; nMHz -= 25)
		{
			unsigned nLength = strlen (Choices);
			lv_snprintf (Choices + nLength, sizeof Choices - nLength, "%s%u MHz", n ? "\n" : "", nMHz);
			lv_snprintf (m_Value[i][n], sizeof m_Value[i][n], "%u", nMHz);
			n++;
		}
		if (n == 0)			// (the firmware didn't say)
		{
			strcpy (Choices, "-");
			n = 1;
		}
		m_Value[i][0][0] = '\0';
	}
	else
	{
		lv_snprintf (Choices, sizeof Choices, "%s", Options[i].pChoices);
		for (; n < 6 && (n == 0 || Options[i].pValues[n]); n++)
		{
			lv_snprintf (m_Value[i][n], sizeof m_Value[i][n], "%s", Options[i].pValues[n] ? Options[i].pValues[n] : "");
		}
	}
	m_nValues[i] = n;

	const char *pValue = m_pHost->GetKernelOption (Options[i].pKey);
	unsigned nSelected = 0;
	if (pValue)
	{
		for (nSelected = 0; nSelected < n && strcmp (pValue, m_Value[i][nSelected]) != 0; nSelected++)
		{
		}
		if (nSelected == n && i == OptionV3D)		// (not to be had, as the ARM's clock is: the maximum)
		{
			nSelected = 0;
		}
		else if (nSelected == n)
		{
			unsigned nLength = strlen (Choices);
			lv_snprintf (Choices + nLength, sizeof Choices - nLength, "\n%s", pValue);
		}
	}
	lv_dropdown_set_options (m_pOption[i], Choices);
	lv_dropdown_set_selected (m_pOption[i], nSelected);
}

// a choice made: the option's value from now (none: the option left out)
void CSettingsApp::SetOption (unsigned i, unsigned nChoice)
{
	const char *pNew = m_Value[i][nChoice];
	m_pHost->SetKernelOption (Options[i].pKey, pNew[0] ? pNew : nullptr);
	m_bKernelChanged = TRUE;
	if (i == OptionCPU)		// the V3D's rates go with the ARM's: at its maximum
	{				// the V3D's own is no more
		const char *pCPU = m_pHost->GetKernelOption ("cpu");
		if ((!pCPU || strcmp (pCPU, "low") != 0) && m_pHost->GetKernelOption ("v3d"))
		{
			m_pHost->SetKernelOption ("v3d", nullptr);
		}
		FillOption (OptionV3D);
	}
}

// A choice that takes Settings away at the next start: sure? (Then only
// cmdline.txt on the card, by the installer page or by hand, brings it back)
void CSettingsApp::Warn (unsigned i, unsigned nChoice)
{
	m_nWarnOption = i;
	m_nWarnChoice = nChoice;
	m_ForgetAddress[0] = '\0';
	char Text[200];
	lv_snprintf (Text, sizeof Text, "%s\nTo get it back: the installer page, or cmdline.txt on the card.\n\nSet it?",
		     Options[i].pWarning);
	Ask (LV_SYMBOL_WARNING "  Warning", Text);
}

// a question to answer with Yes or No (OnEvent: m_pYes, m_pNo)
void CSettingsApp::Ask (const char *pTitle, const char *pText)
{
	const char *Text = pText;
	m_pWarning = lv_msgbox_create (nullptr);
	lv_obj_set_width (m_pWarning, 290);
	lv_msgbox_add_title (m_pWarning, pTitle);
	lv_msgbox_add_text (m_pWarning, Text);
	m_pYes = lv_msgbox_add_footer_button (m_pWarning, "Yes");
	m_pNo = lv_msgbox_add_footer_button (m_pWarning, "No");
	lv_obj_set_style_bg_color (m_pYes, lv_palette_darken (LV_PALETTE_GREY, 2), 0);
	lv_obj_set_size (m_pYes, 96, 38);			// (a finger's)
	lv_obj_set_size (m_pNo, 96, 38);
	lv_obj_set_style_pad_hor (lv_msgbox_get_content (m_pWarning), 12, 0);
	lv_obj_set_style_pad_bottom (lv_msgbox_get_footer (m_pWarning), 10, 0);
	lv_obj_add_event_cb (m_pYes, OnEvent, LV_EVENT_CLICKED, this);
	lv_obj_add_event_cb (m_pNo, OnEvent, LV_EVENT_CLICKED, this);
}

void CSettingsApp::ShowKernel (void)
{
	lv_obj_t *pScreen = Page (ScreenKernel, "Kernel");
	m_pHost->LoadKernelOptions ();
	m_bKernelChanged = FALSE;

	// the options: a list to scroll
	lv_obj_t *pList = lv_obj_create (pScreen);
	lv_obj_set_size (pList, WIDTH, HEIGHT - 50);
	lv_obj_align (pList, LV_ALIGN_TOP_LEFT, 0, 50);
	lv_obj_set_style_border_width (pList, 0, 0);
	lv_obj_set_style_radius (pList, 0, 0);
	lv_obj_set_style_pad_all (pList, 6, 0);
	lv_obj_set_style_pad_row (pList, 6, 0);
	lv_obj_set_flex_flow (pList, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_scroll_dir (pList, LV_DIR_VER);
	for (unsigned i = 0; i < KernelOptions; i++)
	{
		lv_obj_t *pRow = lv_obj_create (pList);
		lv_obj_set_size (pRow, WIDTH - 12, 42);
		lv_obj_set_style_border_width (pRow, 0, 0);
		lv_obj_set_style_pad_all (pRow, 0, 0);
		lv_obj_set_style_bg_opa (pRow, LV_OPA_TRANSP, 0);
		lv_obj_remove_flag (pRow, LV_OBJ_FLAG_SCROLLABLE);
		Label (pRow, Options[i].pName, LV_ALIGN_LEFT_MID, 6, 0);

		m_pOption[i] = lv_dropdown_create (pRow);
		FillOption (i);
		lv_obj_set_width (m_pOption[i], 132);
		lv_obj_align (m_pOption[i], LV_ALIGN_RIGHT_MID, -2, 0);
		lv_obj_add_event_cb (m_pOption[i], OnEvent, LV_EVENT_VALUE_CHANGED, this);
	}
}

// the kernel's options changed and written (or not: no card): now, or later?
void CSettingsApp::ShowRestart (boolean bWritten)
{
	lv_obj_t *pScreen = Page (ScreenRestart, "Kernel");
	lv_obj_t *pText = Label (pScreen, bWritten ? "The kernel's settings are saved.\nThey take effect when piegpu restarts.\n\nRestart now?"
						   : "The kernel's settings couldn't be saved:\ncmdline.txt can't be written\n(is there a card?)",
				 LV_ALIGN_TOP_LEFT, 14, 62);
	lv_obj_set_width (pText, WIDTH - 28);
	lv_label_set_long_mode (pText, LV_LABEL_LONG_WRAP);
	if (bWritten)
	{
		m_pRestart = Button (pScreen, "Restart now", LV_ALIGN_BOTTOM_LEFT, 14, -16, 140, 44);
		lv_obj_add_event_cb (m_pRestart, OnEvent, LV_EVENT_CLICKED, this);
	}
	m_pLater = Button (pScreen, bWritten ? "Later" : "OK", LV_ALIGN_BOTTOM_RIGHT, -14, -16, 130, 44);
	lv_obj_set_style_bg_color (m_pLater, lv_palette_darken (LV_PALETTE_GREY, 2), 0);
	lv_obj_add_event_cb (m_pLater, OnEvent, LV_EVENT_CLICKED, this);
}

// a page left
void CSettingsApp::Back (void)
{
	switch (m_Screen)
	{
	case ScreenKernel:
		if (m_bKernelChanged)			// written now: Later keeps them for the next start
		{
			m_bKernelChanged = FALSE;
			ShowRestart (m_pHost->SaveKernelOptions ());
			break;
		}
		ShowRoot ();
		break;

	case ScreenTest:
	case ScreenCalibrate:
		ShowDisplay ();
		break;

	default:
		ShowRoot ();
		break;
	}
}

void CSettingsApp::ShowCalibrate (const char *pNote)
{
	m_Screen = ScreenCalibrate;
	lv_obj_t *pScreen = NewScreen ();
	Forget ();
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
	Forget ();
	m_pCanvas = lv_canvas_create (pScreen);
	lv_canvas_set_buffer (m_pCanvas, m_pCanvasPixels, WIDTH, HEIGHT, LV_COLOR_FORMAT_RGB565);
	lv_canvas_fill_bg (m_pCanvas, lv_color_hex (0x101418), LV_OPA_COVER);
	lv_obj_remove_flag (m_pCanvas, LV_OBJ_FLAG_CLICKABLE);
	m_pPosition = Label (pScreen, "Draw with a finger", LV_ALIGN_TOP_LEFT, 10, 12);
	m_pBack = Button (pScreen, LV_SYMBOL_LEFT, LV_ALIGN_TOP_RIGHT, -10, 8, 46, 34);
	lv_obj_add_event_cb (m_pBack, OnEvent, LV_EVENT_CLICKED, this);
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
	if (Code == LV_EVENT_VALUE_CHANGED)
	{
		if (pTarget == pThis->m_pBrightness)
		{
			int n = lv_slider_get_value (pTarget);
			pThis->m_pBacklight->SetBrightness (n);
			Percent (pThis->m_pBrightnessValue, n);
			pThis->m_bChanged = TRUE;
		}
		else if (pTarget == pThis->m_pVolume)
		{
			int n = lv_slider_get_value (pTarget);
			pThis->m_pAudio->SetVolume (n);
			Percent (pThis->m_pVolumeValue, n);
			pThis->m_bChanged = TRUE;
		}
		else if (pTarget == pThis->m_pBluetooth)
		{
			pThis->m_pHost->SetBluetooth (lv_obj_has_state (pTarget, LV_STATE_CHECKED));
			pThis->m_bChanged = TRUE;
		}
		else if (pTarget == pThis->m_pMute)
		{
			pThis->m_pAudio->SetMute (lv_obj_has_state (pTarget, LV_STATE_CHECKED));
			pThis->ShowMuted ();
			pThis->m_bChanged = TRUE;
		}
		for (unsigned i = 0; i < KernelOptions; i++)
		{
			if (pTarget == pThis->m_pOption[i])
			{
				// one of its choices: its value (none: the option left out); the
				// one more, that the card had: as it was
				unsigned n = lv_dropdown_get_selected (pTarget);
				if (n >= pThis->m_nValues[i])
				{
					continue;
				}
				const char *pOld = pThis->m_pHost->GetKernelOption (Options[i].pKey);
				const char *pNew = pThis->m_Value[i][n];
				pOld = pOld ? pOld : pThis->m_Value[i][0];	// (left out: its default)
				if (strcmp (pOld, pNew) == 0)
				{
					continue;
				}
				if (Options[i].pWarnValue && strcmp (pNew, Options[i].pWarnValue) == 0)
				{
					pThis->Warn (i, n);
				}
				else
				{
					pThis->SetOption (i, n);
				}
			}
		}
		return;
	}
	if (Code == LV_EVENT_LONG_PRESSED)		// on a speaker paired with: forget it?
	{
		for (unsigned i = 0; i < pThis->m_nSpeakers; i++)
		{
			if (pTarget == pThis->m_pSpeaker[i] && pThis->m_bSpeakerPaired[i] && !pThis->m_pWarning)
			{
				pThis->m_pHeld = pTarget;
				strcpy (pThis->m_ForgetAddress, pThis->m_SpeakerAddress[i]);
				char Text[120];
				lv_snprintf (Text, sizeof Text, "Forget this speaker?\nIt has to be paired with again to be used.");
				pThis->Ask (LV_SYMBOL_BLUETOOTH "  Paired speaker", Text);
			}
		}
		return;
	}
	if (Code != LV_EVENT_CLICKED)
	{
		return;
	}
	for (unsigned i = 0; i < pThis->m_nSpeakers; i++)
	{
		if (pTarget == pThis->m_pSpeaker[i])
		{
			if (pTarget == pThis->m_pHeld)		// (the long press's end)
			{
				pThis->m_pHeld = nullptr;
			}
			else
			{
				pThis->m_pHost->PickSpeaker (pThis->m_SpeakerAddress[i]);
			}
			return;
		}
	}
	if (pTarget == pThis->m_pDone)
	{
		if (pThis->m_bChanged)
		{
			pThis->m_pHost->SaveSettings ();
		}
		pThis->m_bOpen = FALSE;
		pThis->m_pHost->SetSpeakerSearch (FALSE);
		LOGNOTE ("Settings closed%s", pThis->m_bChanged ? " (saved)" : "");
	}
	else if (pTarget == pThis->m_pBack)
	{
		pThis->Back ();
	}
	else if (pTarget == pThis->m_pMenu[0])
	{
		pThis->ShowDisplay ();
	}
	else if (pTarget == pThis->m_pMenu[1])
	{
		pThis->ShowAudio ();
	}
	else if (pTarget == pThis->m_pMenu[2])
	{
		pThis->ShowKernel ();
	}
	else if (pTarget == pThis->m_pCalibrate)
	{
		pThis->ShowCalibrate ();
	}
	else if (pTarget == pThis->m_pTest)
	{
		pThis->ShowTest ();
	}
	else if (pTarget == pThis->m_pYes || pTarget == pThis->m_pNo)
	{
		if (pThis->m_ForgetAddress[0])			// (the question was about a speaker)
		{
			if (pTarget == pThis->m_pYes)
			{
				pThis->m_pHost->ForgetSpeaker (pThis->m_ForgetAddress);
			}
			pThis->m_ForgetAddress[0] = '\0';
		}
		else if (pTarget == pThis->m_pYes)
		{
			pThis->SetOption (pThis->m_nWarnOption, pThis->m_nWarnChoice);
		}
		else
		{
			pThis->FillOption (pThis->m_nWarnOption);	// (as it was)
		}
		lv_msgbox_close_async (pThis->m_pWarning);
		pThis->m_pWarning = pThis->m_pYes = pThis->m_pNo = nullptr;
	}
	else if (pTarget == pThis->m_pSoundTest)
	{
		lv_label_set_text (pThis->m_pNote, pThis->m_pHost->TestSound ());
	}
	else if (pTarget == pThis->m_pRestart)
	{
		if (pThis->m_bChanged)
		{
			pThis->m_pHost->SaveSettings ();
		}
		pThis->m_pHost->Restart ();
	}
	else if (pTarget == pThis->m_pLater)
	{
		pThis->ShowRoot ();
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
