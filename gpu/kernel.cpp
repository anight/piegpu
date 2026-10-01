//
// gpu: the RPi side of piegpu (docs/protocol.md).
// Receives the command stream from the host over a link (link/: the Pico's
// I2S, or a PC over USB) and renders with the V3D to an output (display/: the
// ST7789 panel, or HDMI: by default HDMI while a monitor is connected, else
// the panel).
//
#include "kernel.h"
#include <math.h>
#include "build_info.h"
#include <v3dcheck.h>
#include <circle/2dgraphics.h>
#include <circle/machineinfo.h>
#include <circle/bcmpropertytags.h>
#include <circle/string.h>
#include <circle/util.h>
#include <circle/synchronize.h>
#include <pgpu_protocol.h>
#include <circle/memory.h>

#define MAX_PACKETS_PER_LOOP	64
#define MAX_US_PER_LOOP		1000	// then VCHIQ's tasks (video) get their turn
#define QUIET_HOST_US		1000000	// no packets mid-frame: the host went away

LOGMODULE ("gpu");

#define CLOCK_ID_V3D		5		// (Circle's list hasn't it)

// while a V3D job runs: VCHIQ's tasks (the video decoder's messages) run too;
// a few more milliseconds without them at the end of a frame (a frame's
// conversion into its texture, or a test's 4 ms wait) stalled the decoder for
// good after four frames
static void V3DWait (void)
{
	CScheduler::Get ()->Yield ();
}

CKernel::CKernel (void)
:	m_bCmdLineRead (FALSE),
	m_bBluetooth (FALSE),
	m_Timer (&m_Interrupt),
	// the firmware starts the ARM at its lowest clock (the Zero 2 W: 600 of
	// 1000 MHz). The temperature is the firmware's to watch, at its own limit
	// (it throttles, and the throttle flags say so): CCPUThrottle's Update ()
	// isn't called, which would slow the ARM down at 60 C (socmaxtemp)
	m_CPUThrottle (strcmp (m_Options.GetAppOptionString ("cpu", "max"), "low") == 0
		       ? CPUSpeedLow : CPUSpeedMaximum),
	m_Logger (m_Options.GetLogLevel (), &m_Timer),
	m_Gadget (&m_Interrupt),
	m_DevLink (&m_Interrupt, &m_Gadget),
	m_Installer (&m_Interrupt, &m_Timer, &m_DevLink),
	m_nHostLine (0),
	m_VCHIQ (CMemorySystem::Get (), &m_Interrupt),
	m_OutputMode (OutputAuto),
	m_HostMode (HostAuto),
	m_bGUD (FALSE),
	m_bJobCheck (TRUE),
	m_nARMClock (0),
	m_bPanelPresent (TRUE),
	m_nHDMIPixels (CRenderer::MaxPixels),
	m_Panel (&m_Interrupt),
	m_Touch (&m_Panel),
	m_pScreen (nullptr),
	m_GUD (&m_Gadget),
	m_bOutputPending (FALSE),
	m_bFrameSeen (FALSE),
	m_nLastFrame (0),
	m_USBBulkLink (&m_Gadget),
	m_USBLink (&m_DevLink),
	m_Renderer (&m_V3D),
	m_Commands (&m_Renderer, &m_I2SLink),
	m_Audio (&m_VCHIQ, m_Commands.GetVideo ()),
#ifdef PGPU_WIRELESS
	m_Bluetooth (&m_Interrupt),
#endif
	m_SettingsApp (&m_Touch, &m_Backlight, &m_Audio),
	m_bPressing (FALSE),
	m_nPressStart (0),
	m_nPressX (0),
	m_nPressY (0),
	m_nV3DMin (0),
	m_nV3DMax (0),
	m_nTestTouchEnd (0),
	m_bTestTouch (FALSE),
	m_bTestTouchChanged (FALSE),
	m_nTestTouchX (0),
	m_nTestTouchY (0),
#ifdef ARM_ALLOW_MULTI_CORE
	m_Cores (&m_Audio),
#endif
	m_pLinks {&m_USBBulkLink, &m_USBLink, &m_I2SLink}
{
}

CKernel::~CKernel (void)
{
}

boolean CKernel::Initialize (void)
{
	const char *pHost = m_Options.GetAppOptionString ("host", "auto");
	m_HostMode =   strcmp (pHost, "usb") == 0 ? HostUSB
		     : strcmp (pHost, "i2s") == 0 ? HostI2S : HostAuto;
	const char *pOutput = m_Options.GetAppOptionString ("output", "auto");
	m_OutputMode =   strcmp (pOutput, "panel") == 0 ? OutputPanel
		       : strcmp (pOutput, "hdmi") == 0 ? OutputHDMI : OutputAuto;
	m_bGUD = strcmp (m_Options.GetAppOptionString ("gud", "off"), "on") == 0;	// (off by default)
	m_bJobCheck = strcmp (m_Options.GetAppOptionString ("clcheck", "on"), "off") != 0;
	m_nHDMIPixels = m_Options.GetAppOptionDecimal ("hdmi_pixels", m_nHDMIPixels);
	m_Commands.SetAudio (&m_Audio);
	if (m_nHDMIPixels < 320 * 240 || m_nHDMIPixels > CRenderer::MaxPixels)
	{
		m_nHDMIPixels = CRenderer::MaxPixels;
	}
	m_DevLink.RegisterRebootHandler (CRunLog::Restarting);

	return    m_Logger.Initialize (&m_Null)
	       && m_RunLog.Initialize (&m_Logger, m_Options.GetLogLevel ())
	       && m_Interrupt.Initialize ()
	       && m_Timer.Initialize ()
	       && (!m_bGUD || m_GUD.Initialize ())	// (before the gadget starts)
	       && (m_Gadget.SetStream (&m_USBBulkLink), TRUE)
	       && m_DevLink.Initialize ()
	       && LoadSettings ()
	       && DetectPanel ()
	       && m_VCHIQ.Initialize ()
	       && (!(m_bPanelPresent || m_OutputMode == OutputPanel) || m_Panel.Initialize ())
	       && (!m_bPanelPresent || (m_Backlight.Initialize (atoi (Setting ("brightness", "100"))), TRUE))
	       && (!m_bPanelPresent		// (touch=off, or none there: without)
		   || (m_Touch.Initialize (Setting ("touch", "auto"), Setting ("touchcal")), TRUE))
	       && (m_OutputMode == OutputPanel || m_HDMI.Initialize ())
#ifdef ARM_ALLOW_MULTI_CORE
	       && m_Cores.Initialize ()
#endif
	       ;
}

// the user's settings (settings.txt on the card, if there's one): read once,
// before what they set up; the volume's now
boolean CKernel::LoadSettings (void)
{
	char *pText = new char[CSettings::MaxBytes];
	int nBytes = m_Installer.ReadFile ("settings.txt", pText, CSettings::MaxBytes);
	if (nBytes >= 0)
	{
		m_Settings.Parse (pText, nBytes);
		CString Keys;
		for (unsigned i = 0; i < m_Settings.GetCount (); i++)
		{
			Keys.Append (i ? " " : "");
			Keys.Append (m_Settings.GetKey (i));
			Keys.Append ("=");
			Keys.Append (m_Settings.Get (m_Settings.GetKey (i)));
		}
		LOGNOTE ("Settings: settings.txt: %s", m_Settings.GetCount () ? (const char *) Keys : "(none)");
	}
	else
	{
		LOGNOTE ("Settings: no settings.txt (the kernel command line's, or the defaults)");
	}
	delete [] pText;

	m_Audio.SetDefaultVolume (atoi (Setting ("volume", "10")));	// percent
	m_Audio.SetMute (strcmp (Setting ("mute", "off"), "on") == 0);
	m_bBluetooth = strcmp (Setting ("bluetooth", "off"), "on") == 0;
	return TRUE;
}

// Settings changed something: settings.txt with it (the other keys there kept)
void CKernel::SaveSettings (void)
{
	CString Value;
	Value.Format ("%u", m_Audio.GetDefaultVolume ());
	m_Settings.Set ("volume", Value);
	m_Settings.Set ("mute", m_Audio.IsMuted () ? "on" : "off");
	Value.Format ("%u", m_Backlight.GetBrightness ());
	m_Settings.Set ("brightness", Value);
	if (m_Touch.IsPresent ())
	{
		m_Settings.Set ("touchcal", m_Touch.GetCalibration ());
	}
	if (HasBluetooth ())
	{
		m_Settings.Set ("bluetooth", m_bBluetooth ? "on" : "off");
	}
	char *pText = new char[CSettings::MaxBytes];
	unsigned nBytes = m_Settings.Format (pText, CSettings::MaxBytes);
	if (!m_Installer.WriteFile ("settings.txt", pText, nBytes))
	{
		LOGWARN ("Settings: settings.txt can't be written (no card?)");
	}
	delete [] pText;
}

// The V3D's clock: v3d=MHz, within what the firmware allows (its maximum
// without the option). The firmware takes any rate there while the ARM is at
// its low clock (cpu=low); with the ARM at its maximum (the firmware's turbo
// mode) it keeps the V3D at its maximum too, whatever is asked (measured on a
// Zero: 250 and 275 asked, 300 MHz running)
unsigned CKernel::GetClock (u32 nClockId, u32 nTag)
{
	CBcmPropertyTags Tags;
	TPropertyTagClockRate Rate;
	Rate.nClockId = nClockId;
	return Tags.GetTag (nTag, &Rate, sizeof Rate, 4) ? (Rate.nRate + 500000) / 1000000 : 0;	// MHz
}

void CKernel::SetV3DClock (void)
{
	m_nV3DMin = GetClock (CLOCK_ID_V3D, PROPTAG_GET_MIN_CLOCK_RATE);
	m_nV3DMax = GetClock (CLOCK_ID_V3D, PROPTAG_GET_MAX_CLOCK_RATE);
	if (!m_nV3DMax)
	{
		return;
	}
	unsigned nMHz = m_Options.GetAppOptionDecimal ("v3d", m_nV3DMax);
	if (nMHz < m_nV3DMin || nMHz > m_nV3DMax)
	{
		LOGWARN ("v3d=%u: not within %u-%u MHz; %u", nMHz, m_nV3DMin, m_nV3DMax, m_nV3DMax);
		nMHz = m_nV3DMax;
	}
	CBcmPropertyTags Tags;
	TPropertyTagSetClockRate Set;
	Set.nClockId = CLOCK_ID_V3D;
	Set.nRate = nMHz * 1000000;
	Set.nSkipSettingTurbo = SKIP_SETTING_TURBO;
	unsigned nRunning = 0;
	if (Tags.GetTag (PROPTAG_SET_CLOCK_RATE, &Set, sizeof Set, 12))
	{
		nRunning = GetClock (CLOCK_ID_V3D, PROPTAG_GET_CLOCK_RATE_MEASURED);
	}
	if (nRunning != nMHz)
	{
		LOGWARN ("V3D clock: %u MHz asked, %u MHz running%s", nMHz, nRunning,
			 nRunning > nMHz ? " (the firmware keeps it there while the ARM is at its maximum)" : "");
	}
}

void CKernel::GetClockRange (boolean bV3D, unsigned *pMinMHz, unsigned *pMaxMHz)
{
	*pMinMHz = bV3D ? m_nV3DMin : m_CPUThrottle.GetMinClockRate () / 1000000;
	*pMaxMHz = bV3D ? m_nV3DMax : m_CPUThrottle.GetMaxClockRate () / 1000000;
}

// Settings' Test: what to say of it
const char *CKernel::TestSound (void)
{
	if (!m_Audio.PlayTest ())
	{
		return "Not now: a stream is playing";
	}
#ifdef PGPU_WIRELESS
	if (m_Bluetooth.GetLink () == CBluetooth::LinkReady)
	{
		return m_Audio.IsMuted () ? "Played on the speaker, muted" : "On the speaker: left, right, both";
	}
#endif
	return   !m_Monitor.GetState ().bConnected ? "Played, but no monitor is on HDMI to hear it"
	       : m_Audio.IsMuted () ? "Played, muted" : "Left, right, both";
}

// The Zero W and the Zero 2 W have the chip (the Zero doesn't), and their
// builds the code (PGPU_WIRELESS, devtools/build-gpu.sh: the Zero's is built
// without it)
static boolean IsWirelessBoard (void)
{
	TMachineModel Model = CMachineInfo::Get ()->GetMachineModel ();
	return Model == MachineModelZeroW || Model == MachineModelZero2W;
}

boolean CKernel::HasBluetooth (void) const
{
#ifdef PGPU_WIRELESS
	return IsWirelessBoard ();
#else
	return FALSE;
#endif
}

void CKernel::SetBluetooth (boolean bOn)
{
	m_bBluetooth = bOn;
#ifdef PGPU_WIRELESS
	if (HasBluetooth ())
	{
		m_Bluetooth.SetEnabled (bOn);
	}
#endif
}

// what Settings says under its Bluetooth switch: why it's greyed, what it's doing
const char *CKernel::GetBluetoothNote (void)
{
#ifdef PGPU_WIRELESS
	if (HasBluetooth ())
	{
		switch (m_Bluetooth.GetState ())
		{
		case CBluetooth::StateOff:	return "Off. On: it looks for speakers nearby";
		case CBluetooth::StateStarting:	return "Starting...";
		case CBluetooth::StateFailed:	return "The Bluetooth chip doesn't answer";
		default:			break;
		}
		switch (m_Bluetooth.GetLink ())
		{
		case CBluetooth::LinkConnecting:	m_BluetoothNote.Format ("Calling %s...", m_Bluetooth.GetName (m_Bluetooth.GetLinkAddress ()));
							return m_BluetoothNote;
		case CBluetooth::LinkPairing:		return "Pairing...";
		case CBluetooth::LinkSetup:		return "Setting up the sound...";
		case CBluetooth::LinkReady:		m_BluetoothNote.Format ("The sound goes to %s", m_Bluetooth.GetName (m_Bluetooth.GetLinkAddress ()));
							return m_BluetoothNote;
		case CBluetooth::LinkClosing:		return "Letting it go...";
		default:				break;
		}
		if (m_Bluetooth.GetLinkError ()[0])
		{
			m_BluetoothNote.Format ("%s. Looking for speakers...", m_Bluetooth.GetLinkError ());
			return m_BluetoothNote;
		}
		return "Looking for speakers nearby...";
	}
#endif
	return IsWirelessBoard () ? "The Zero's kernel: no Bluetooth in it" : "This board has no Bluetooth";
}

void CKernel::SetSpeakerSearch (boolean bOn)
{
#ifdef PGPU_WIRELESS
	m_Bluetooth.SetScanning (bOn);
#endif
}

unsigned CKernel::GetSpeakersGeneration (void)
{
#ifdef PGPU_WIRELESS
	return m_Bluetooth.GetGeneration ();
#else
	return 0;
#endif
}

// the speakers paired with first, the last used at the top (one isn't found
// while it's connected, or switched off), then the others found
unsigned CKernel::GetSpeakers (TSpeaker *pSpeakers, unsigned nMax)
{
	unsigned n = 0;
#ifdef PGPU_WIRELESS
	if (m_Bluetooth.GetState () != CBluetooth::StateReady)
	{
		return 0;
	}
	const CBluetooth::TBond *pBonds;
	unsigned nBonds = m_Bluetooth.GetBonds (&pBonds);
	const CBluetooth::TDevice *pDevices;
	unsigned nDevices = m_Bluetooth.GetDevices (&pDevices);
	for (unsigned i = 0; i < nBonds + nDevices && n < nMax; i++)
	{
		const u8 *pAddress = i < nBonds ? pBonds[i].Address : pDevices[i - nBonds].Address;
		boolean bPaired = i < nBonds;
		if (!bPaired && m_Bluetooth.FindBond (pAddress))
		{
			continue;
		}
		TSpeaker &S = pSpeakers[n++];
		CBluetooth::FormatAddress (pAddress, S.Address);
		strncpy (S.Name, m_Bluetooth.GetName (pAddress), sizeof S.Name - 1);
		S.Name[sizeof S.Name - 1] = '\0';
		CBluetooth::TLink Link = memcmp (pAddress, m_Bluetooth.GetLinkAddress (), 6) == 0 ? m_Bluetooth.GetLink ()
					 : CBluetooth::LinkNone;
		S.bConnected = Link == CBluetooth::LinkReady;
		S.bPaired = bPaired;
		S.pState =   Link == CBluetooth::LinkReady ? LV_SYMBOL_OK "  connected"
			   : Link == CBluetooth::LinkConnecting ? "calling..."
			   : Link == CBluetooth::LinkPairing ? "pairing..."
			   : Link == CBluetooth::LinkSetup ? "setting up..."
			   : bPaired ? "paired" : "";
	}
#endif
	return n;
}

void CKernel::ForgetSpeaker (const char *pAddress)
{
#ifdef PGPU_WIRELESS
	u8 Address[6];
	if (CBluetooth::ParseAddress (pAddress, Address))
	{
		m_Bluetooth.Forget (Address);
	}
#endif
}

#ifdef PGPU_WIRELESS
// The speakers paired with, in speakers.txt on the card (Settings pairs and
// forgets; nothing else writes it): a line each, the last used first: its
// address, the key the two share and its kind, its name
#define SPEAKERS_FILE	"speakers.txt"

void CKernel::LoadSpeakers (void)
{
	char *pText = new char[CSettings::MaxBytes];
	int nBytes = m_Installer.ReadFile (SPEAKERS_FILE, pText, CSettings::MaxBytes - 1);
	CBluetooth::TBond Bonds[CBluetooth::MaxBonds];
	unsigned nBonds = 0;
	pText[nBytes > 0 ? nBytes : 0] = '\0';
	for (char *pLine = pText; *pLine && nBonds < CBluetooth::MaxBonds; )
	{
		char *pEnd = pLine;
		while (*pEnd && *pEnd != '\n')
		{
			pEnd++;
		}
		char *pNext = *pEnd ? pEnd + 1 : pEnd;
		while (pEnd > pLine && (pEnd[-1] == '\r' || pEnd[-1] == ' '))
		{
			pEnd--;
		}
		*pEnd = '\0';
		// "AA:BB:CC:DD:EE:FF 32 hex digits:kind name"
		CBluetooth::TBond &Bond = Bonds[nBonds];
		memset (&Bond, 0, sizeof Bond);
		if (pEnd - pLine >= 17 + 1 + 32 + 2 && pLine[0] != '#' && CBluetooth::ParseAddress (pLine, Bond.Address)
		    && pLine[17] == ' ' && pLine[50] == ':')
		{
			boolean bKey = TRUE;
			for (unsigned i = 0; i < 16; i++)
			{
				char Hex[3] = {pLine[18 + 2 * i], pLine[19 + 2 * i], '\0'}, *pStop;
				Bond.Key[i] = (u8) strtoul (Hex, &pStop, 16);
				bKey = bKey && pStop == Hex + 2;
			}
			char *pName;
			Bond.nKeyType = (u8) strtoul (pLine + 51, &pName, 10);
			while (*pName == ' ')
			{
				pName++;
			}
			strncpy (Bond.Name, pName, sizeof Bond.Name - 1);
			if (bKey)
			{
				nBonds++;
			}
		}
		pLine = pNext;
	}
	delete [] pText;
	m_Bluetooth.SetBonds (Bonds, nBonds);
	if (nBonds)
	{
		LOGNOTE ("Bluetooth: %u speaker%s paired with (" SPEAKERS_FILE "), the last used \"%s\"", nBonds, nBonds == 1 ? "" : "s",
			 Bonds[0].Name);
	}
}

void CKernel::SaveSpeakers (void)
{
	const CBluetooth::TBond *pBonds;
	unsigned nBonds = m_Bluetooth.GetBonds (&pBonds);
	CString Text ("# piegpu: the Bluetooth speakers paired with (Settings: Audio), the last used\n"
		      "# first: address, the key the two share and its kind, name\n"), Line, Byte;
	for (unsigned i = 0; i < nBonds; i++)
	{
		char Address[18];
		CBluetooth::FormatAddress (pBonds[i].Address, Address);
		Line.Format ("%s ", Address);
		for (unsigned k = 0; k < 16; k++)
		{
			Byte.Format ("%02X", pBonds[i].Key[k]);
			Line.Append (Byte);
		}
		Byte.Format (":%u %s\n", pBonds[i].nKeyType, pBonds[i].Name);
		Line.Append (Byte);
		Text.Append (Line);
	}
	if (!m_Installer.WriteFile (SPEAKERS_FILE, (const char *) Text, Text.GetLength ()))
	{
		LOGWARN ("Bluetooth: " SPEAKERS_FILE " can't be written (no card?): the pairing lasts till the restart");
	}
}
#endif

// a tap on a speaker: connect to it (pairing, if it's new); the connected one: let go
void CKernel::PickSpeaker (const char *pAddress)
{
#ifdef PGPU_WIRELESS
	u8 Address[6];
	if (!CBluetooth::ParseAddress (pAddress, Address))
	{
		return;
	}
	if (m_Bluetooth.GetLink () != CBluetooth::LinkNone && memcmp (Address, m_Bluetooth.GetLinkAddress (), 6) == 0)
	{
		m_Bluetooth.Disconnect ();
	}
	else
	{
		m_Bluetooth.Connect (Address);
	}
#endif
}

// The kernel's options, for Settings to change: cmdline.txt as the card has
// it now (the installer page may have written it since the start; options
// this build doesn't know stay as they are). Without the file: what this
// start had, under what Settings changes (the file is made of the changes)
void CKernel::LoadKernelOptions (void)
{
	char *pText = new char[CSettings::MaxBytes];
	int nBytes = m_Installer.ReadFile ("cmdline.txt", pText, CSettings::MaxBytes);
	m_bCmdLineRead = nBytes >= 0;
	m_CmdLine.ParseLine (pText, nBytes > 0 ? nBytes : 0);
	delete [] pText;
}

const char *CKernel::GetKernelOption (const char *pKey)
{
	const char *pValue = m_CmdLine.Get (pKey);
	return pValue || m_bCmdLineRead ? pValue : m_Options.GetAppOptionString (pKey);
}

void CKernel::SetKernelOption (const char *pKey, const char *pValue)
{
	if (pValue)
	{
		m_CmdLine.Set (pKey, pValue);
	}
	else
	{
		m_CmdLine.Remove (pKey);
	}
}

boolean CKernel::SaveKernelOptions (void)
{
	if (!m_CmdLine.IsComplete ())		// (it would come back shorter)
	{
		LOGWARN ("Settings: cmdline.txt has more than Settings can keep: left as it is");
		return FALSE;
	}
	char *pText = new char[CSettings::MaxBytes];
	unsigned nBytes = m_CmdLine.FormatLine (pText, CSettings::MaxBytes);
	boolean bOK = m_Installer.WriteFile ("cmdline.txt", pText, nBytes);
	if (bOK)
	{
		LOGNOTE ("Settings: cmdline.txt: %s", pText);
	}
	else
	{
		LOGWARN ("Settings: cmdline.txt can't be written (no card?)");
	}
	delete [] pText;
	return bOK;
}

void CKernel::Restart (void)
{
	m_Installer.Unmount ();
	LOGNOTE ("Rebooting (Settings)");
	CTimer::SimpleMsDelay (100);
	CRunLog::Restarting ();
	reboot ();
}

// a press held 2 s in about one place: once till it's let go
boolean CKernel::LongPress (const u32 *pTouch)
{
	static const unsigned HoldUs = 2000000;
	static const int Slack = 24;		// pixels it may wander
	boolean bDown = !!(pTouch[0] & PGPU_TOUCH_DOWN);
	int x = pTouch[1] & 0xFFFF, y = pTouch[1] >> 16;
	if (!bDown)
	{
		m_bPressing = FALSE;
		m_nPressStart = 0;
		return FALSE;
	}
	if (!m_bPressing || x < m_nPressX - Slack || x > m_nPressX + Slack || y < m_nPressY - Slack || y > m_nPressY + Slack)
	{
		m_bPressing = TRUE;		// (a new press, or it moved: from here)
		m_nPressStart = CTimer::GetClockTicks () | 1;
		m_nPressX = x;
		m_nPressY = y;
		return FALSE;
	}
	if (m_nPressStart && CTimer::GetClockTicks () - m_nPressStart >= HoldUs)
	{
		m_nPressStart = 0;		// (once a press)
		return TRUE;
	}
	return FALSE;
}

// Settings has the panel. A host on I2S is told to wait (READY low: it stops
// before its next batch, and goes on where it was after). A PC over USB
// can't be (it gives up after 10 s without credit): its frames are rendered
// at the panel's pace but not shown. To the host the touch is let go
void CKernel::OpenSettings (void)
{
	if (!m_bPanelPresent || m_OutputMode == OutputHDMI)
	{
		return;
	}
	m_I2SLink.SetHold (TRUE);
	m_Panel.SetHeld (TRUE);
	m_Panel.WaitIdle ();
	u32 Touch[CTouch::Words];
	m_Touch.GetState (Touch);
	Touch[0] &= ~PGPU_TOUCH_DOWN;
	m_Commands.SetTouch (Touch);
	m_SettingsApp.Open ();
}

// the panel back: the host's next frame, or the splash (or HDMI's notice)
// if none comes
void CKernel::CloseSettings (void)
{
	m_Panel.WaitIdle ();
	m_Panel.SetHeld (FALSE);
	m_I2SLink.SetHold (FALSE);
	if (m_pScreen != &m_Panel)
	{
		ShowPanelNotice ();
	}
	else if (!m_bFrameSeen || CTimer::GetClockTicks () - m_nLastFrame > 500000)
	{
		ShowSplash (&m_Panel);
	}
}

const char *CKernel::Setting (const char *pKey, const char *pDefault)
{
	const char *pValue = m_Settings.Get (pKey);
	return pValue ? pValue : m_Options.GetAppOptionString (pKey, pDefault);
}

#ifdef PGPU_WIRELESS
// the BT host line (debugging): ON, OFF, SCAN, STOP, LIST, CONNECT address, DISCONNECT, FORGET, TEST
void CKernel::BluetoothLine (const char *pLine)
{
	if (strcmp (pLine, "ON") == 0 || strcmp (pLine, "OFF") == 0)
	{
		m_Bluetooth.SetEnabled (pLine[1] == 'N');
	}
	else if (strcmp (pLine, "SCAN") == 0 || strcmp (pLine, "STOP") == 0)
	{
		m_Bluetooth.SetScanning (pLine[1] == 'C');
	}
	else if (strncmp (pLine, "CONNECT ", 8) == 0)
	{
		u8 Address[6];
		if (CBluetooth::ParseAddress (pLine + 8, Address))
		{
			m_Bluetooth.Connect (Address);
		}
	}
	else if (strcmp (pLine, "DISCONNECT") == 0)
	{
		m_Bluetooth.Disconnect ();
	}
	else if (strncmp (pLine, "FORGET ", 7) == 0)
	{
		ForgetSpeaker (pLine + 7);
	}
	else if (strcmp (pLine, "TEST") == 0)
	{
		LOGNOTE ("BT: test sound: %s", TestSound ());
	}
	else if (strcmp (pLine, "LIST") == 0)
	{
		char Link[18];
		CBluetooth::FormatAddress (m_Bluetooth.GetLinkAddress (), Link);
		LOGNOTE ("BT: link %u to %s%s%s; media packets %u sent, %u dropped; the main loop away %u ms at most, %u skips",
			 m_Bluetooth.GetLink (), Link, m_Bluetooth.GetLinkError ()[0] ? ", last: " : "", m_Bluetooth.GetLinkError (),
			 m_Bluetooth.GetAudioOut ()->GetSent (), m_Bluetooth.GetAudioOut ()->GetDropped (),
			 m_Bluetooth.GetAudioOut ()->GetLongestGap () / 1000, m_Bluetooth.GetAudioOut ()->GetSkips ());
		const CBluetooth::TDevice *pDevices;
		unsigned n = m_Bluetooth.GetDevices (&pDevices);
		LOGNOTE ("BT: state %u, %u audio devices%s", m_Bluetooth.GetState (), n, m_Bluetooth.IsScanning () ? ", scanning" : "");
		for (unsigned i = 0; i < n; i++)
		{
			char Address[18];
			CBluetooth::FormatAddress (pDevices[i].Address, Address);
			LOGNOTE ("BT:   %s  class %06X  %d dBm  \"%s\"", Address, pDevices[i].nClass, pDevices[i].nRSSI, pDevices[i].Name);
		}
	}
}
#endif

// text from the host (the USB serial link): "s" alone asks for a screenshot,
// lines that start with "PGI " go to the installer (gpu/install)
void CKernel::HostInput (void)
{
	int c;
	while ((c = m_DevLink.GetChar ()) >= 0)
	{
		if (c == '\r' || c == '\n')
		{
			m_HostLine[m_nHostLine] = '\0';
			if (strncmp (m_HostLine, "PGI ", 4) == 0)
			{
				m_Installer.Command (m_HostLine);
			}
			else if (strcmp (m_HostLine, "PCM") == 0)	// debugging: the sound sent to HDMI
			{
				DumpAudio ();
			}
			else if (strcmp (m_HostLine, "SOUNDTEST") == 0)	// debugging: an underrun on purpose
			{
				SoundTest ();
			}
#ifdef PGPU_WIRELESS
			else if (strncmp (m_HostLine, "BT ", 3) == 0)	// debugging: Bluetooth by hand
			{
				BluetoothLine (m_HostLine + 3);
			}
#endif
			else if (strncmp (m_HostLine, "TOUCH ", 6) == 0)	// debugging: "TOUCH x y ms", a finger there
			{								// (the panel's pixels) for so long
				char *pEnd;
				m_nTestTouchX = strtoul (m_HostLine + 6, &pEnd, 10);
				m_nTestTouchY = strtoul (pEnd, &pEnd, 10);
				m_nTestTouchEnd = (CTimer::GetClockTicks () + strtoul (pEnd, nullptr, 10) * 1000) | 1;
				m_bTestTouch = m_bTestTouchChanged = TRUE;
			}
			m_nHostLine = 0;
		}
		else if (c == 's' && m_nHostLine == 0)
		{
			DumpScreenshot ();
		}
		else if (m_nHostLine < CInstaller::MaxLine)
		{
			m_HostLine[m_nHostLine++] = (char) c;
		}
	}
}

// panel=auto (the default): a panel if one answers over MISO; panel=yes: one
// is there (MISO not wired); panel=none: there's none
boolean CKernel::DetectPanel (void)
{
	const char *pPanel = m_Options.GetAppOptionString ("panel", "auto");
	if (strcmp (pPanel, "auto") != 0)
	{
		m_bPanelPresent = strcmp (pPanel, "none") != 0;
		LOGNOTE ("Panel: %s (panel=%s)", m_bPanelPresent ? "there" : "none", pPanel);
		return TRUE;
	}

	CPanelOutput::TPanelInfo Info;
	m_bPanelPresent = CPanelOutput::Detect (&Info);
	if (m_bPanelPresent)
	{
		LOGNOTE ("Panel: ST7789, module ID %02X %02X %02X; status %08X, power mode %02X, "
			 "MADCTL %02X, pixel format %02X, self-diagnostic %02X", Info.nID >> 16,
			 (Info.nID >> 8) & 0xFF, Info.nID & 0xFF, Info.nStatus, Info.nPowerMode,
			 Info.nMADCTL, Info.nPixelFormat, Info.nSelfDiagnostic);
	}
	else
	{
		LOGNOTE ("Panel: none answers on MISO (GPIO9)");
	}

	return TRUE;
}

// where frames go now, at which size
void CKernel::ChooseOutput (COutput **ppOutput, unsigned *pWidth, unsigned *pHeight)
{
	if (   m_OutputMode == OutputPanel
	    || (   m_OutputMode == OutputAuto && m_bPanelPresent
		&& !m_Monitor.GetState ().bConnected))
	{
		*ppOutput = &m_Panel;
		*pWidth = 320;
		*pHeight = 240;
		return;
	}

	*ppOutput = &m_HDMI;
	HDMISize (pWidth, pHeight);
}

// the screen on HDMI: the monitor's preferred mode (or else the mode sent),
// or, if that's more than hdmi_pixels (default: as large as the renderer
// goes, 1920x1200), divided by the smallest whole number that brings it down;
// the firmware scales it to the HDMI mode. The width is a multiple of 16 (the
// framebuffer's pitch). Without a monitor the screen keeps its size.
void CKernel::HDMISize (unsigned *pWidth, unsigned *pHeight)
{
	const THDMIState &M = m_Monitor.GetState ();
	unsigned nWidth = M.bEDID ? M.nWidth : M.nSignalWidth;
	unsigned nHeight = M.bEDID ? M.nHeight : M.nSignalHeight;
	if (!M.bConnected || nWidth < 16 || nHeight < 16)
	{
		*pWidth = m_HDMI.GetWidth ();
		*pHeight = m_HDMI.GetHeight ();
		return;
	}

	unsigned nDivisor = 1;
	while (   (nWidth / nDivisor) * (nHeight / nDivisor) > m_nHDMIPixels
	       || nWidth / nDivisor > CRenderer::MaxWidth
	       || nHeight / nDivisor > CRenderer::MaxHeight)
	{
		nDivisor++;
	}
	*pWidth = (nWidth / nDivisor) & ~15;
	*pHeight = (nHeight / nDivisor) & ~1;
}

// between frames: the output and screen size for the monitor's state now,
// and the GL frames on it, or off screen while a PC's desktop shows there
void CKernel::ApplyOutput (void)
{
	m_bOutputPending = FALSE;

	COutput *pScreen;
	unsigned nWidth, nHeight;
	ChooseOutput (&pScreen, &nWidth, &nHeight);
	m_GUD.SetScreen (nWidth, nHeight);
	boolean bDesktop = m_GUD.IsActive ();
	COutput *pTarget = bDesktop ? &m_Offscreen : pScreen;
	boolean bScreen =    pScreen != m_pScreen
			  || nWidth != m_Renderer.GetWidth ()
			  || nHeight != m_Renderer.GetHeight ();
	if (!bScreen && pTarget == m_Renderer.GetOutput ())
	{
		if (pScreen == &m_HDMI)
		{
			ShowPanelNotice ();		// (the monitor's EDID came)
			if (!bDesktop && HostIdle ())
			{
				ShowSplash (pScreen);	// with the monitor's name now
			}
		}
		SendDisplay ();
		return;
	}

	boolean bWasDesktop = m_Renderer.GetOutput () == &m_Offscreen;
	if (bDesktop)
	{
		m_Renderer.GetOutput ()->WaitIdle ();
		if (!pScreen->SetSize (nWidth, nHeight))
		{
			LOGERR ("Can't show %ux%u on %s", nWidth, nHeight, pScreen == &m_HDMI ? "HDMI" : "the panel");
			return;
		}
	}
	if (!m_Renderer.SetOutput (pTarget, nWidth, nHeight))
	{
		LOGERR ("Can't show %ux%u on %s", nWidth, nHeight, pScreen == &m_HDMI ? "HDMI" : "the panel");
		return;
	}
	m_pScreen = pScreen;
	m_Commands.ScreenChanged ();
	LOGNOTE ("Screen: %ux%u on %s%s", nWidth, nHeight, pScreen == &m_HDMI ? "HDMI" : "the panel",
		 bDesktop ? ", the PC's desktop" : "");
	if (bDesktop)
	{
		m_GUD.Start (pScreen);
	}
	else if (bWasDesktop || HostIdle ())
	{
		ShowSplash (pScreen);			// (the desktop's gone, or no host draws: until the next frame)
	}

	// the panel says where the screen is (again when the monitor's EDID comes)
	if (pScreen == &m_HDMI)
	{
		ShowPanelNotice ();
	}

	SendDisplay ();
}

// STREAM_END (docs/protocol.md 13): the host's session is over. Everything is
// reset (as RESET: the streams closed, the objects gone), the link is idle
// again (the serial port: text, the log and the installer), and the splash
// shows until the next host draws.
void CKernel::EndSession (CLink *pLink)
{
	LOGNOTE ("The host ended its session");
	m_Commands.Reset ();
	pLink->EndSession ();
	m_bFrameSeen = FALSE;
	if (!m_GUD.IsActive ())
	{
		ShowSplash (m_pScreen);
	}
}

// no host draws: none has ended a frame, or not for a second
boolean CKernel::HostIdle (void) const
{
	return !m_bFrameSeen || CTimer::GetClockTicks () - m_nLastFrame > QUIET_HOST_US;
}

// with the screen on HDMI: the panel says so, and what goes there
void CKernel::ShowPanelNotice (void)
{
	if (!m_bPanelPresent || m_Panel.IsHeld ())	// (Settings has the panel)
	{
		return;
	}

	// the monitor (its EDID: name, preferred mode), the HDMI signal the
	// firmware sends (the pixel valve's size, the rate measured; config.txt
	// sets it at boot) and the screen GL renders into, which the firmware
	// scales to the signal
	const THDMIState &M = m_Monitor.GetState ();
	CString Model, Native, Signal, Render;
	if (!M.bConnected)
	{
		Model = "none";
		Native = "-";
		Signal = "-";
	}
	else
	{
		if (!M.bEDID)
		{
			Model = "(reading EDID)";
			Native = "-";
		}
		else
		{
			Model = M.Name[0] ? M.Name : "(no name)";
			Native.Format ("%ux%u@%uHz", M.nWidth, M.nHeight, (M.nRefreshMilliHz + 500) / 1000);
		}
		unsigned nRefresh = m_HDMI.MeasureRefresh ();
		Signal.Format ("%ux%u@%uHz", M.nSignalWidth, M.nSignalHeight, (nRefresh + 500) / 1000);
	}
	Render.Format ("%ux%u", m_HDMI.GetWidth (), m_HDMI.GetHeight ());

	const char *Label[] = {"Monitor:", "Native resolution:", "HDMI resolution:", "Render resolution:"};
	const char *Value[] = {Model, Native, Signal, Render};
	CString Lines[4];
	const char *pLines[4];
	for (unsigned i = 0; i < 4; i++)
	{
		Lines[i].Format ("%-19s%s", Label[i], Value[i]);
		pLines[i] = Lines[i];
	}
	ShowLines (&m_Panel, pLines, 4);
	LOGNOTE ("Panel: monitor %s, native %s, HDMI %s, render %s", (const char *) Model,
		 (const char *) Native, (const char *) Signal, (const char *) Render);
}

// the DISPLAY reply (docs/protocol.md 9)
void CKernel::SendDisplay (boolean bSend)
{
	const THDMIState &M = m_Monitor.GetState ();
	u32 Display[PGPU_DISPLAY_WORDS];
	memset (Display, 0, sizeof Display);
	Display[0] =   (m_pScreen == &m_HDMI ? PGPU_OUTPUT_HDMI : PGPU_OUTPUT_PANEL)
		     | (M.bConnected ? PGPU_DISPLAY_HDMI_CONNECTED : 0)
		     | (m_bPanelPresent ? PGPU_DISPLAY_PANEL_PRESENT : 0)
		     | (M.bEDID ? PGPU_DISPLAY_EDID : 0);
	Display[1] = m_Renderer.GetWidth () | m_Renderer.GetHeight () << 16;
	Display[2] = M.nWidth | M.nHeight << 16;
	Display[3] = M.nRefreshMilliHz;
	Display[4] = M.nSignalWidth | M.nSignalHeight << 16;
	memcpy (&Display[5], M.Name, sizeof M.Name);		// 13 characters, zero padded
	m_Commands.SetDisplay (Display, PGPU_DISPLAY_WORDS, bSend);
}

TShutdownMode CKernel::Run (void)
{
	LOGNOTE ("Compile time: " __DATE__ " " __TIME__);
	LOGNOTE ("Build: %s", GetBuildInfo ());
	// supported for now: the Zero / Zero W and the Zero 2 W (other RPi boards
	// may run this build, but nothing is verified on them: the HDMI hot-plug
	// line, for one, is a guess there)
	TMachineModel Model = CMachineInfo::Get ()->GetMachineModel ();
	if (   Model != MachineModelZero && Model != MachineModelZeroW
	    && Model != MachineModelZero2W)
	{
		LOGWARN ("%s isn't supported yet (for now: the Zero / Zero W and the Zero 2 W)",
			 CMachineInfo::Get ()->GetMachineName ());
	}
#ifndef PGPU_WIRELESS
	if (IsWirelessBoard ())
	{
		LOGNOTE ("This kernel is the Zero's, without the wireless code: no Bluetooth on this board (its own kernel has it)");
	}
#endif
	LOGNOTE ("Core clock %u MHz", CMachineInfo::Get ()->GetClockRate (CLOCK_ID_CORE) / 1000000);
	m_nARMClock = m_CPUThrottle.GetClockRate ();
	SetV3DClock ();
	// (the clocks as the firmware measures them, not as they were asked for)
	LOGNOTE ("ARM clock %u MHz (%u-%u MHz), V3D %u MHz (%u-%u MHz); SoC %u C (the firmware's limit: %u C)",
		 GetClock (CLOCK_ID_ARM, PROPTAG_GET_CLOCK_RATE_MEASURED), m_CPUThrottle.GetMinClockRate () / 1000000,
		 m_CPUThrottle.GetMaxClockRate () / 1000000,
		 GetClock (CLOCK_ID_V3D, PROPTAG_GET_CLOCK_RATE_MEASURED), m_nV3DMin, m_nV3DMax,
		 m_CPUThrottle.GetTemperature (), m_CPUThrottle.GetMaxTemperature ());
#ifdef PGPU_WIRELESS
	m_Bluetooth.SetTrace (strcmp (m_Options.GetAppOptionString ("btlog", "off"), "on") == 0);
	LoadSpeakers ();
	m_Audio.SetOther (m_Bluetooth.GetAudioOut ());
	if (m_bBluetooth && HasBluetooth ())
	{
		m_Bluetooth.SetEnabled (TRUE);
	}
#endif
	// the firmware's throttle flags (under-voltage and so on, since boot)
	LOGNOTE ("Throttled %05X", GetThrottled ());
	m_RunLog.Report ();		// how the previous run ended
	LOGNOTE ("Host: %s", m_HostMode == HostUSB ? "USB only (host=usb)"
			     : m_HostMode == HostI2S ? "I2S only (host=i2s)" : "USB when a PC streams, else I2S (host=auto)");
	LOGNOTE ("USB: %s", m_bGUD ? "serial port, GL interface and monitor (GUD)" : "serial port and GL interface, no monitor (gud=off)");

	m_Monitor.Initialize ();
	COutput *pOutput;
	unsigned nWidth, nHeight;
	ChooseOutput (&pOutput, &nWidth, &nHeight);
	if (!pOutput->SetSize (nWidth, nHeight))
	{
		LOGPANIC ("No %ux%u screen", nWidth, nHeight);
	}
	LOGNOTE ("Screen: %ux%u on %s", nWidth, nHeight, pOutput == &m_HDMI ? "HDMI" : "the panel");
	m_pScreen = pOutput;
	m_GUD.SetScreen (nWidth, nHeight);

	if (   !m_V3D.Initialize ()
	    || !m_Renderer.Initialize (pOutput))
	{
		LOGPANIC ("V3D init failed");
	}
	// after the renderer's start, which clears its buffers (on HDMI the
	// framebuffer's pages)
	ShowSplash (pOutput);
	if (pOutput == &m_HDMI)
	{
		ShowPanelNotice ();
	}
	m_Commands.Reset ();
	m_Commands.InitializeVideo ();

	for (unsigned i = 0; i < Links; i++)
	{
		if (!m_pLinks[i]->Initialize ())
		{
			LOGPANIC ("Link %u init failed", i);
		}
	}
	LOGNOTE ("I2S slave: CLK pin 12, FS pin 35, DIN pin 38, DOUT pin 40; READY pin 36, FRAME pin 37");

	m_SettingsApp.Initialize (m_Panel.GetDriver (), this);

	SendDisplay (FALSE);
	if (m_Touch.IsPresent ())		// (a TOUCH reply after each INFO only with a touch screen)
	{
		u32 Touch[CTouch::Words];
		m_Touch.GetState (Touch);
		m_Commands.SetTouch (Touch);
	}
	m_Commands.SendInfo ();			// once after boot, with DISPLAY (docs/protocol.md 9)

	CV3D::SetWaitHandler (V3DWait);
	if (m_bJobCheck)
	{
		CV3D::SetJobCheck (V3DCheckJob);	// each job's control lists checked before it runs
	}

	unsigned nLastReport = m_Timer.GetUptime ();
	unsigned nWindowStart = CTimer::GetClockTicks ();	// microseconds
	unsigned nBusyUs = 0;			// receiving and executing packets
	unsigned nLastPacket = CTimer::GetClockTicks ();
	while (1)
	{
		m_Scheduler.Yield ();			// VCHIQ's tasks
		m_DevLink.Update ();
		HostInput ();

		// commands from the first active link that host= allows (a PC over
		// USB once it has sent on the GL interface or switched the serial
		// port to its binary stream, else the Pico or the P4 over I2S); the
		// others' input is discarded
		CLink *pLink = nullptr;
		for (unsigned k = 0; k < Links; k++)
		{
			m_pLinks[k]->Update ();
			boolean bAllowed =    m_HostMode == HostAuto
					   || (m_HostMode == HostUSB && m_pLinks[k] != &m_I2SLink)
					   || (m_HostMode == HostI2S && m_pLinks[k] == &m_I2SLink);
			if (!pLink && bAllowed && m_pLinks[k]->IsActive ())
			{
				pLink = m_pLinks[k];
			}
			else
			{
				u32 nHeader;
				while (m_pLinks[k]->GetPacket (&nHeader) != nullptr)
				{
				}
			}
		}
		m_Commands.SetLink (pLink);		// (none: host=usb before a PC's session)

		u32 nHeader;
		const u32 *pPayload;
		unsigned nLoopStart = CTimer::GetClockTicks (), i;
		for (i = 0;
		        pLink
		     && i < MAX_PACKETS_PER_LOOP
		     && CTimer::GetClockTicks () - nLoopStart < MAX_US_PER_LOOP
		     && (pPayload = pLink->GetPacket (&nHeader)) != nullptr;
		     i++)
		{
			if (PGPU_HEADER_OP (nHeader) == PGPU_OP_DEBUG_SCREENSHOT)
			{
				DumpScreenshot ();
			}
			else if (PGPU_HEADER_OP (nHeader) == PGPU_OP_STREAM_END)
			{
				EndSession (pLink);
				break;
			}
			else
			{
				m_Commands.Execute (nHeader, pPayload);
				if (PGPU_HEADER_OP (nHeader) == PGPU_OP_FRAME_END)
				{
					m_bFrameSeen = TRUE;
					m_nLastFrame = CTimer::GetClockTicks ();
					if (m_bOutputPending)
					{
						ApplyOutput ();
					}
				}
			}
		}
		if (i)
		{
			nBusyUs += CTimer::GetClockTicks () - nLoopStart;
			nLastPacket = CTimer::GetClockTicks ();
		}

		m_Commands.UpdateVideo ();
#ifdef PGPU_WIRELESS
		m_Bluetooth.Update ();
		if (m_Bluetooth.BondChanged ())		// paired (or no more; or another one used): kept
		{
			SaveSpeakers ();
		}
#endif

		// the touch screen: read after each panel frame (or here, without
		// them); a change goes to the host, unless Settings is open (a long
		// press opens it)
		u32 Touch[CTouch::Words];
		boolean bTouchChanged = m_Touch.Update (Touch);
		m_Touch.GetState (Touch);
		if (m_bTestTouch && !(Touch[0] & PGPU_TOUCH_DOWN))	// the TOUCH host line's finger: there, and
		{							// where it was let go, till a real one
			Touch[1] = m_nTestTouchX | m_nTestTouchY << 16;
			if (m_nTestTouchEnd && (int) (m_nTestTouchEnd - CTimer::GetClockTicks ()) > 0)
			{
				Touch[0] |= PGPU_TOUCH_DOWN;
			}
			else if (m_nTestTouchEnd)
			{
				m_nTestTouchEnd = 0;
				m_bTestTouchChanged = TRUE;
			}
			bTouchChanged = m_bTestTouchChanged;		// (put down, moved, let go)
			m_bTestTouchChanged = FALSE;
		}
		else
		{
			m_bTestTouch = FALSE;
		}
		if (m_SettingsApp.IsOpen ())
		{
			if (!m_SettingsApp.Update (Touch))
			{
				CloseSettings ();
			}
		}
		else if (LongPress (Touch))
		{
			OpenSettings ();
		}
		else if (bTouchChanged)
		{
			m_Commands.SetTouch (Touch);
		}

		// the monitor plugged in or out: a new screen from the next frame
		if (m_Monitor.Update ())
		{
			m_bOutputPending = TRUE;
		}
		// a PC's desktop on or off
		if (m_GUD.Update (m_pScreen))
		{
			m_bOutputPending = TRUE;
		}
		// a screen change waits for the frame's end, but not for a host
		// that went quiet in its middle (killed, say): that frame is dropped
		if (   m_bOutputPending && !m_Commands.IsBetweenFrames ()
		    && CTimer::GetClockTicks () - nLastPacket > QUIET_HOST_US)
		{
			LOGNOTE ("The host went quiet in the middle of a frame: dropped for the new screen");
			m_Commands.AbandonFrame ();
		}
		if (m_bOutputPending && m_Commands.IsBetweenFrames ())
		{
			ApplyOutput ();
		}

		unsigned nNow = m_Timer.GetUptime ();
		if (nNow != nLastReport)
		{
			TI2SLinkStats R = m_I2SLink.GetStats ();
			TCommandStats C = m_Commands.GetStats ();
			unsigned nFrames = C.nFrames ? C.nFrames : 1;

			// the CPU's own work: the waits for the V3D and the panel happen
			// while executing commands (both poll)
			unsigned nTicks = CTimer::GetClockTicks ();
			unsigned nWaitUs = C.nRenderUs + C.nPresentWaitUs;
			TLoadStats Load = {nTicks - nWindowStart, C.nFrames, C.nRenderUs,
					   nBusyUs > nWaitUs ? nBusyUs - nWaitUs : 0, C.nPresentWaitUs};
			m_Commands.SetLoadStats (Load);
			nWindowStart = nTicks;
			nBusyUs = 0;

			LOGNOTE ("%u fps, %u draws %u tris/frame, prims %u clipped %u rejected %u dropped %u, "
				 "render %u us, panel wait %u us",
				 C.nFrames, C.nDraws / nFrames, C.nTriangles / nFrames,
				 C.nPrimitives / nFrames, C.nClipped / nFrames, C.nRejected / nFrames,
				 C.nDropped, C.nRenderUs / nFrames, C.nPresentWaitUs / nFrames);
			LOGNOTE ("link: %u packets, %u KB, CRC err %u, garbage %u, max fill %u KB, READY low %u | "
				 "cmd errors %u (last %03X) | replies %u dropped %u late %u | "
				 "heap %u KB free, textures %u KB",
				 R.nPackets, (R.nWords - R.nIdleWords) / 256, R.nCRCErrors,
				 R.nGarbageWords, R.nMaxFill / 256, R.nReadyLow,
				 C.nErrors, C.nLastError, R.nReplies, R.nRepliesDropped, R.nTxLate,
				 (unsigned) (CMemorySystem::Get ()->GetHeapFreeSpace (HEAP_LOW) / 1024),
				 m_Commands.GetTextureBytes () / 1024);

			// the ARM's clock, when the firmware changed it
			unsigned nARMClock = m_CPUThrottle.GetClockRate ();
			if (nARMClock && nARMClock != m_nARMClock)
			{
				LOGNOTE ("ARM clock now %u MHz (SoC %u C)", nARMClock / 1000000,
					 m_CPUThrottle.GetTemperature ());
				m_nARMClock = nARMClock;
			}

			// under-voltage, capping, throttling, temperature limit: now
			// (bits 0-3) or since boot (16-19)
			u32 nThrottled = GetThrottled ();
			if (nThrottled)
			{
				LOGWARN ("Throttled %05X:%s%s%s%s", nThrottled,
					 nThrottled & 0x10001 ? " under-voltage" : "",
					 nThrottled & 0x20002 ? " frequency capped" : "",
					 nThrottled & 0x40004 ? " throttled" : "",
					 nThrottled & 0x80008 ? " soft temperature limit" : "");
			}

			nLastReport = nNow;
		}
	}

	return ShutdownHalt;
}

// the firmware's throttle flags (0 if it doesn't say)
u32 CKernel::GetThrottled (void)
{
	CBcmPropertyTags Tags;
	TPropertyTagSimple Throttled;
	Throttled.nValue = 0;
	if (!Tags.GetTag (PROPTAG_GET_THROTTLED, &Throttled, sizeof Throttled, 4))
	{
		return 0;
	}

	return Throttled.nValue;
}

// the frame on screen (the output's copy - HDMI's page - or else the last
// presented one) as base64 lines between markers, for devtools/screenshot.py
// bytes as base64 lines (72 bytes a line) to the host
boolean CKernel::WriteBase64 (const u8 *p, unsigned nBytes)
{
	static const char Base64[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

	char Line[100];
	for (unsigned i = 0; i < nBytes; )
	{
		unsigned n = 0;
		for (unsigned k = 0; k < 24 && i < nBytes; k++, i += 3)
		{
			u32 v = p[i] << 16 | (i + 1 < nBytes ? p[i + 1] : 0) << 8 | (i + 2 < nBytes ? p[i + 2] : 0);
			Line[n++] = Base64[(v >> 18) & 63];
			Line[n++] = Base64[(v >> 12) & 63];
			Line[n++] = i + 1 < nBytes ? Base64[(v >> 6) & 63] : '=';
			Line[n++] = i + 2 < nBytes ? Base64[v & 63] : '=';
		}
		Line[n++] = '\n';
		if (!m_DevLink.Write (Line, n))
		{
			return FALSE;
		}
	}
	return TRUE;
}

void CKernel::DumpScreenshot (void)
{
	const u8 *p = (const u8 *) m_Renderer.GetOutput ()->GetShownFrame ();
	if (p == nullptr)
	{
		p = (const u8 *) m_Renderer.GetLastFrame ();
	}
	unsigned nWidth = m_Renderer.GetWidth (), nHeight = m_Renderer.GetHeight ();
	unsigned nBytes = nWidth * nHeight * 2;
	if (m_SettingsApp.IsOpen ())		// the panel is its: what it has there
	{
		p = (const u8 *) m_SettingsApp.GetPicture ();
		nWidth = 320;
		nHeight = 240;
		nBytes = nWidth * nHeight * 2;
	}
	else
	{
		CleanAndInvalidateDataCacheRange ((uintptr) p, nBytes);	// written by the V3D
	}

	CString Header;
	Header.Format ("\n#SCREENSHOT %u %u rgb565le\n", nWidth, nHeight);
	m_DevLink.Write ((const char *) Header, Header.GetLength ());
	if (!WriteBase64 (p, nBytes))
	{
		LOGWARN ("Screenshot aborted");
		return;
	}
	m_DevLink.Write ("#END\n", 5);
	m_DevLink.Update ();
}

// the SOUNDTEST host line (debugging, no GL session): a 440 Hz tone at 10%
// with the main loop stalled for 150 ms in its middle, without yielding: the
// VCHIQ tasks can't hand the VideoCore its chunks meanwhile, so it runs dry
// (an underrun there, while the ring stays full). Then PCM shows the chunks'
// timing and the VideoCore's completion flags.
void CKernel::SoundTest (void)
{
	CAudioOut *pOut = m_Audio.GetHDMIOutput (48000);
	if (!pOut || !pOut->Open ())
	{
		LOGWARN ("SOUNDTEST: no sound output");
		return;
	}
	pOut->Flush ();
	pOut->SetPaused (FALSE);

	static s16 Tone[2 * 48000];			// 1 s, a whole number of periods
	for (unsigned i = 0; i < 48000; i++)
	{
		Tone[2 * i] = Tone[2 * i + 1] = (s16) (3277.0 * sin (2.0 * 3.14159265358979 * 440.0 * i / 48000.0));
	}
	pOut->Write (Tone, 48000);
	pOut->Write (Tone, 48000);			// (the ring's 2 s)

	unsigned nFlags = pOut->GetCompleteFlagCount ();
	CScheduler::Get ()->MsSleep (500);
	unsigned nStall = CTimer::GetClockTicks ();
	LOGNOTE ("SOUNDTEST: the main loop stalls for 150 ms now (%u)", nStall);
	CTimer::SimpleMsDelay (150);
	CScheduler::Get ()->MsSleep (1200);
	LOGNOTE ("SOUNDTEST: completions with flags %u before, %u after (the last %08X, %d ms after the stall's start); "
		 "%llu frames of silence in all", nFlags, pOut->GetCompleteFlagCount (),
		 pOut->GetLastCompleteFlags (), (int) (pOut->GetLastCompleteFlagTime () - nStall) / 1000,
		 pOut->GetUnderrunFrames ());
}

// the PCM host line (debugging): what the audio output handed to the
// VideoCore lately (16-bit stereo, little-endian), then its chunks: the time
// each was asked for (us), its frames, those from the ring (the rest was
// silence), the frames the ring held before it. devtools/pcmdump.py reads it.
void CKernel::DumpAudio (void)
{
	CAudioOut *pOut = m_Audio.GetLastOut ();
	if (!pOut)
	{
		static const char None[] = "\n#PCM none\n#END\n";
		m_DevLink.Write (None, sizeof None - 1);
		return;
	}
	pOut->FreezeCapture (TRUE);

	const s16 *pFirst, *pSecond;
	unsigned nFirst, nSecond;
	pOut->GetCapture (&pFirst, &nFirst, &pSecond, &nSecond);
	CString Header;
	Header.Format ("\n#PCM %u %u s16le-stereo\n", pOut->GetSampleRate (), nFirst + nSecond);
	m_DevLink.Write ((const char *) Header, Header.GetLength ());
	boolean bOK =    WriteBase64 ((const u8 *) pFirst, nFirst * 4)
		      && WriteBase64 ((const u8 *) pSecond, nSecond * 4);

	const CAudioOut::TChunk *pC1, *pC2;
	unsigned nC1, nC2;
	pOut->GetChunks (&pC1, &nC1, &pC2, &nC2);
	Header.Format ("#VCFLAGS %u %08X %u\n", pOut->GetCompleteFlagCount (), pOut->GetLastCompleteFlags (),
		       pOut->GetLastCompleteFlagTime ());
	m_DevLink.Write ((const char *) Header, Header.GetLength ());
	Header.Format ("#CHUNKS %u\n", nC1 + nC2);
	m_DevLink.Write ((const char *) Header, Header.GetLength ());
	for (unsigned i = 0; bOK && i < nC1 + nC2; i++)
	{
		const CAudioOut::TChunk &C = i < nC1 ? pC1[i] : pC2[i - nC1];
		CString Line;
		Line.Format ("%u %u %u %u\n", C.nTime, C.nFrames, C.nFromRing, C.nQueued);
		bOK = m_DevLink.Write ((const char *) Line, Line.GetLength ());
	}

	m_DevLink.Write ("#END\n", 5);
	pOut->FreezeCapture (FALSE);
	if (!bOK)
	{
		LOGWARN ("PCM dump aborted");
	}
}

// the idle screen: what piegpu is, where its screen is and what it waits
// for. Laid out for 320x240 and scaled by a whole factor (the panel: 1,
// 1024x600: 2, 1920x1080: 4); it stays until the host's first frame
void CKernel::ShowSplash (COutput *pOutput)
{
	if (pOutput == &m_Panel && m_Panel.IsHeld ())	// (Settings has the panel)
	{
		return;
	}
	const THDMIState &M = m_Monitor.GetState ();
	unsigned nWidth = pOutput->GetWidth (), nHeight = pOutput->GetHeight ();

	CString Status, Monitor, Render, Board, Clocks, Build, Built;
	if (pOutput == &m_HDMI)
	{
		if (M.bConnected)
		{
			unsigned nRefresh = m_HDMI.MeasureRefresh ();
			Status.Format ("HDMI %ux%u@%uHz ready", M.nSignalWidth, M.nSignalHeight,
				       (nRefresh + 500) / 1000);
		}
		else
		{
			Status = "HDMI ready, no monitor";
		}
	}
	else
	{
		Status.Format ("Panel %ux%u ready", nWidth, nHeight);
	}
	if (pOutput != &m_HDMI)
	{
		Monitor = "ST7789";			// the splash is on the panel
	}
	else if (!M.bConnected)
	{
		Monitor = "none";
	}
	else if (!M.bEDID)
	{
		Monitor = "(reading EDID)";
	}
	else
	{
		Monitor.Format ("%s %ux%u@%uHz", M.Name[0] ? M.Name : "(no name)", M.nWidth, M.nHeight,
				(M.nRefreshMilliHz + 500) / 1000);
	}
	Render.Format ("%ux%u", nWidth, nHeight);
	Board = CMachineInfo::Get ()->GetMachineName ();
	Clocks.Format ("ARM %u MHz, V3D %u MHz", m_nARMClock / 1000000,
		       GetClock (CLOCK_ID_V3D, PROPTAG_GET_CLOCK_RATE_MEASURED));
	const char *pHost = m_HostMode == HostUSB ? "USB (host=usb)"
			  : m_HostMode == HostI2S ? "I2S (host=i2s)" : "I2S or USB (host=auto)";
	Build.Format ("%s (%s)", GetBuildVersion (), GetBuildGit ());
	const char *pBuilt = GetBuildTime ();	// "2026-09-30T00:27:32Z" -> "2026-09-30 00:27:32 UTC"
	unsigned nBuilt = strlen (pBuilt);
	char Time[32];
	if (nBuilt > 11 && nBuilt < sizeof Time && pBuilt[10] == 'T' && pBuilt[nBuilt - 1] == 'Z')
	{
		memcpy (Time, pBuilt, nBuilt - 1);
		Time[10] = ' ';
		Time[nBuilt - 1] = '\0';
		Built.Format ("%s UTC", Time);
	}
	else
	{
		Built = pBuilt;
	}

	const char *Label[] = {"Monitor:", "Render:", "Board:", "Clocks:", "Host:", "Version:", "Built:"};
	const char *Value[] = {Monitor, Render, Board, Clocks, pHost, Build, Built};
	const unsigned Lines = sizeof Label / sizeof Label[0];

	// on HDMI the splash goes into the framebuffer's first page: that page on
	// screen again (after a host's frames another one may be), the renderer's
	// next frame into another
	if (pOutput == &m_HDMI && m_Renderer.GetOutput () == &m_HDMI)
	{
		m_Renderer.ResetBuffers ();
	}

	C2DGraphics Graphics (pOutput->GetDisplay ());
	if (!Graphics.Initialize ())
	{
		return;
	}
	unsigned k = nWidth / 320 < nHeight / 240 ? nWidth / 320 : nHeight / 240;
	k = k ? k : 1;
	unsigned x0 = (nWidth - 320 * k) / 2, y0 = (nHeight - 240 * k) / 2;
	Graphics.ClearScreen (COLOR2D (0, 0, 64));

	// in 320x240 units: the title, the status, the block of labelled lines
	// (left-aligned, centred as a block), what it waits for
	unsigned y = 18;
	DrawScaledText (Graphics, x0, y0, k, 160, y, TRUE, COLOR2D (255, 255, 255), "piegpu", Font12x22);
	y += 30;
	DrawScaledText (Graphics, x0, y0, k, 160, y, TRUE, COLOR2D (120, 255, 120), Status, Font8x16);
	y += 28;
	CString Line[Lines];
	unsigned nChars = 0;
	for (unsigned i = 0; i < Lines; i++)
	{
		Line[i].Format ("%-9s%s", Label[i], Value[i]);
		nChars = Line[i].GetLength () > nChars ? Line[i].GetLength () : nChars;
	}
	unsigned xBlock = nChars * 8 < 320 ? (320 - nChars * 8) / 2 : 0;
	for (unsigned i = 0; i < Lines; i++, y += 18)
	{
		DrawScaledText (Graphics, x0, y0, k, xBlock, y, FALSE, COLOR2D (255, 255, 255), Line[i], Font8x16);
	}
	y += 10;
	DrawScaledText (Graphics, x0, y0, k, 160, y, TRUE, COLOR2D (160, 160, 160),
			"waiting for OpenGL ES or media", Font8x16);
	Graphics.UpdateDisplay ();
	pOutput->WaitIdle ();
	LOGNOTE ("Splash on %s: %s", pOutput == &m_HDMI ? "HDMI" : "the panel", (const char *) Status);
}

// text at (x, y) of a 320x240 layout (centred on x, or from it), every font
// pixel drawn as a k x k square at (x0, y0) + k * (x, y)
void CKernel::DrawScaledText (C2DGraphics &Graphics, unsigned x0, unsigned y0, unsigned k,
			      unsigned x, unsigned y, boolean bCentre, T2DColor Color,
			      const char *pText, const TFont &rFont)
{
	CCharGenerator Font (rFont);
	unsigned nChars = strlen (pText);
	if (bCentre)
	{
		unsigned nWidth = nChars * Font.GetCharWidth ();
		x = nWidth < 2 * x ? x - nWidth / 2 : 0;
	}
	for (unsigned i = 0; i < nChars; i++)
	{
		for (unsigned fy = 0; fy < Font.GetUnderline (); fy++)
		{
			CCharGenerator::TPixelLine Line = Font.GetPixelLine (pText[i], fy);
			for (unsigned fx = 0; fx < Font.GetCharWidth (); fx++)
			{
				if (Font.GetPixel (fx, Line))
				{
					unsigned px = x0 + k * (x + i * Font.GetCharWidth () + fx);
					unsigned py = y0 + k * (y + fy);
					if (px + k <= Graphics.GetWidth () && py + k <= Graphics.GetHeight ())
					{
						Graphics.DrawRect (px, py, k, k, Color);
					}
				}
			}
		}
	}
}

// lines in one font and colour, as a left-aligned block in the middle of an
// output (not while it shows frames)
void CKernel::ShowLines (COutput *pOutput, const char *const *ppLines, unsigned nLines)
{
	C2DGraphics Graphics (pOutput->GetDisplay ());
	if (!Graphics.Initialize ())
	{
		return;
	}

	const unsigned CharWidth = 8, LineHeight = 20;		// Font8x16, spaced
	unsigned nChars = 0;
	for (unsigned i = 0; i < nLines; i++)
	{
		unsigned n = strlen (ppLines[i]);
		nChars = n > nChars ? n : nChars;
	}
	unsigned nBlock = nChars * CharWidth;
	unsigned x = nBlock < Graphics.GetWidth () ? (Graphics.GetWidth () - nBlock) / 2 : 0;
	unsigned y = (Graphics.GetHeight () - nLines * LineHeight) / 2;
	Graphics.ClearScreen (COLOR2D (0, 0, 64));
	for (unsigned i = 0; i < nLines; i++)
	{
		Graphics.DrawText (x, y + i * LineHeight, COLOR2D (255, 255, 255), ppLines[i]);
	}
	Graphics.UpdateDisplay ();
	pOutput->WaitIdle ();
}

// a title and two lines in the middle of an output (not while it shows frames)
void CKernel::ShowText (COutput *pOutput, const char *pTitle, const char *pLine1, const char *pLine2)
{
	C2DGraphics Graphics (pOutput->GetDisplay ());
	if (!Graphics.Initialize ())
	{
		return;
	}

	unsigned nWidth = Graphics.GetWidth ();
	unsigned y = Graphics.GetHeight () / 2 - (*pLine1 || *pLine2 ? 30 : 11);
	Graphics.ClearScreen (COLOR2D (0, 0, 64));
	Graphics.DrawText (nWidth / 2, y, COLOR2D (255, 255, 255), pTitle,
			   C2DGraphics::AlignCenter, Font12x22);
	Graphics.DrawText (nWidth / 2, y + 40, COLOR2D (255, 255, 0), pLine1, C2DGraphics::AlignCenter);
	Graphics.DrawText (nWidth / 2, y + 60, COLOR2D (160, 160, 160), pLine2, C2DGraphics::AlignCenter);
	Graphics.UpdateDisplay ();
	pOutput->WaitIdle ();
}
