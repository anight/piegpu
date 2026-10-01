//
// bluetooth.cpp
//
#include "bluetooth.h"
#include "bt_sdp.h"
#include <circle/bcmpropertytags.h>
#include <circle/logger.h>
#include <circle/string.h>
#include <circle/timer.h>
#include <circle/util.h>

LOGMODULE ("bt");

#define BAUD_START		115200		// the controller's after its reset
#define BAUD_RUN		921600		// (92 KB/s: the sound takes 45)
#define STEP_TIMEOUT_US		2000000
#define RESET_TIMEOUT_US	500000		// (its answer comes in 40 ms, when it comes)
#define SCAN_PAUSE_US		3000000		// between scans
#define KEEP_SCANS		3		// a device not seen for so many scans is gone
#define LINK_TIMEOUT_US		30000000	// a connection not ready after so long is given up
#define RECALL_US		10000000	// a paired speaker called again so often (a call takes 5 s)
#define THEIR_CHANNEL_US	1500000		// a speaker that called opens the stream's channel itself: so long for it

// class of device: Audio/Video, hi-fi audio; it captures, it's audio
static const u8 ClassOfDevice[3] = {0x28, 0x04, 0x28};
static const char LocalName[] = "piegpu";

enum
{
	StepReset, StepVersion, StepSetAddress, StepAddress, StepBuffers, StepBaud, StepEventMask, StepPairingMode,
	StepInquiryMode, StepClass, StepName, StepScanEnable, StepDone
};

CBluetooth::CBluetooth (CInterruptSystem *pInterrupt)
:	m_Uart (pInterrupt),
	m_HCI (&m_Uart, this),
	m_State (StateOff),
	m_nStep (0),
	m_nStepStart (0),
	m_nTries (0),
	m_bScanning (FALSE),
	m_bInquiring (FALSE),
	m_bNaming (FALSE),
	m_nScan (0),
	m_nScanIdle (0),
	m_nDevices (0),
	m_nGeneration (0),
	m_L2CAP (&m_HCI, this),
	m_AVDTP (this),
	m_Audio (&m_AVDTP, this),
	m_Link (LinkNone),
	m_nLinkSince (0),
	m_nHandle (0),
	m_bTheirCall (FALSE),
	m_bKeyUsed (FALSE),
	m_bKeyRefused (FALSE),
	m_bEncrypted (FALSE),
	m_nSignalling (CBTL2CAP::None),
	m_nMedia (CBTL2CAP::None),
	m_bPending (FALSE),
	m_nBonds (0),
	m_bBondChanged (FALSE),
	m_nLastCall (0),
	m_nCallNext (0),
	m_bUserHangUp (FALSE)
{
	memset (m_Address, 0, sizeof m_Address);
	memset (m_LinkAddress, 0, sizeof m_LinkAddress);
	memset (m_Bond, 0, sizeof m_Bond);
	m_LinkError[0] = '\0';
}

boolean CBluetooth::ParseAddress (const char *pText, u8 *pAddress)
{
	for (int i = 5; i >= 0; i--)
	{
		unsigned nByte = 0;
		for (unsigned k = 0; k < 2; k++, pText++)
		{
			char c = *pText;
			unsigned nDigit = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10
					  : c >= 'a' && c <= 'f' ? c - 'a' + 10 : 16;
			if (nDigit == 16)
			{
				return FALSE;
			}
			nByte = nByte << 4 | nDigit;
		}
		pAddress[i] = (u8) nByte;
		if (i && *pText++ != ':')
		{
			return FALSE;
		}
	}
	return TRUE;
}

void CBluetooth::FormatAddress (const u8 *pAddress, char *pText)
{
	CString Text;
	Text.Format ("%02X:%02X:%02X:%02X:%02X:%02X", pAddress[5], pAddress[4], pAddress[3], pAddress[2],
		     pAddress[1], pAddress[0]);
	strcpy (pText, Text);
}

void CBluetooth::SetEnabled (boolean bOn)
{
	if (bOn == IsEnabled ())
	{
		return;
	}
	if (!bOn)
	{
		// the speaker first let go as it should be (it knows at once, and
		// takes our call when Bluetooth is on again): its answer waited for,
		// a moment
		m_bPending = FALSE;
		if (m_State == StateReady && m_Link != LinkNone)
		{
			HangUp ("");
			unsigned nStart = CTimer::GetClockTicks ();
			while (m_Link != LinkNone && CTimer::GetClockTicks () - nStart < 500000)
			{
				m_HCI.Update ();
			}
		}
		if (m_Link != LinkNone)
		{
			m_Link = LinkClosing;		// (its channels go with it: nothing to hang up)
			m_L2CAP.LinkDown ();
			m_Link = LinkNone;
		}
		m_LinkError[0] = '\0';
		m_HCI.Restart ();
		m_HCI.Command (HCI_RESET);	// (quiet: no scans, no links)
		m_HCI.Update ();
		m_State = StateOff;
		m_bInquiring = m_bNaming = FALSE;
		m_nDevices = 0;
		m_nGeneration++;
		LOGNOTE ("Off");
		return;
	}
	if (!m_Uart.Initialize (BAUD_START))
	{
		Fail ("no UART");
		return;
	}
	m_HCI.Restart ();
	m_State = StateStarting;
	m_nStep = StepReset;
	m_nTries = 0;
	Step ();
}

void CBluetooth::Fail (const char *pWhy)
{
	LOGWARN ("The controller: %s", pWhy);
	m_State = StateFailed;
}

// the set-up: a command a step, the next when its answer has come
void CBluetooth::Step (void)
{
	m_nStepStart = CTimer::GetClockTicks ();
	switch (m_nStep)
	{
	case StepReset:
		m_HCI.Command (HCI_RESET);
		break;

	case StepVersion:
		m_HCI.Command (HCI_READ_LOCAL_VERSION);
		break;

	case StepSetAddress:
	{
		// The controller comes with no address of its own (AA:AA:AA:AA:AA:AA):
		// Raspberry Pi's block and the board's serial number's low three
		// bytes, each xor AA (as Raspberry Pi OS makes it)
		CBcmPropertyTags Tags;
		TPropertyTagSerial Serial;
		Serial.Serial[0] = 0;
		Tags.GetTag (PROPTAG_GET_BOARD_SERIAL, &Serial, sizeof Serial);
		u32 nSerial = Serial.Serial[0];
		const u8 Address[6] = {(u8) (nSerial ^ 0xAA), (u8) (nSerial >> 8 ^ 0xAA), (u8) (nSerial >> 16 ^ 0xAA),
				       0xEB, 0x27, 0xB8};
		m_HCI.Command (HCI_BCM_WRITE_BD_ADDR, Address, sizeof Address);
		break;
	}

	case StepAddress:
		m_HCI.Command (HCI_READ_BD_ADDR);
		break;

	case StepBuffers:
		m_HCI.Command (HCI_READ_BUFFER_SIZE);
		break;

	case StepBaud:
	{
		const u8 Params[6] = {0, 0, (u8) BAUD_RUN, (u8) (BAUD_RUN >> 8), (u8) (BAUD_RUN >> 16), (u8) (BAUD_RUN >> 24)};
		m_HCI.Command (HCI_BCM_UPDATE_BAUDRATE, Params, sizeof Params);
		break;
	}

	case StepEventMask:
	{
		// all up to the simple pairing's (as BlueZ)
		static const u8 Mask[8] = {0xFF, 0xFF, 0xFB, 0xFF, 0x07, 0xF8, 0xBF, 0x3D};
		m_HCI.Command (HCI_SET_EVENT_MASK, Mask, sizeof Mask);
		break;
	}

	case StepPairingMode:
	{
		const u8 nOn = 1;
		m_HCI.Command (HCI_WRITE_SIMPLE_PAIRING_MODE, &nOn, 1);
		break;
	}

	case StepInquiryMode:
	{
		const u8 nMode = 2;		// results with RSSI, or extended ones (the name in them)
		m_HCI.Command (HCI_WRITE_INQUIRY_MODE, &nMode, 1);
		break;
	}

	case StepClass:
		m_HCI.Command (HCI_WRITE_CLASS_OF_DEVICE, ClassOfDevice, sizeof ClassOfDevice);
		break;

	case StepName:
	{
		u8 Name[248];
		memset (Name, 0, sizeof Name);
		memcpy (Name, LocalName, sizeof LocalName);
		m_HCI.Command (HCI_WRITE_LOCAL_NAME, Name, sizeof Name);
		break;
	}

	case StepScanEnable:
	{
		const u8 nScans = 2;		// it can be connected to; it isn't found by others' inquiries
		m_HCI.Command (HCI_WRITE_SCAN_ENABLE, &nScans, 1);
		break;
	}

	case StepDone:
	{
		char Address[18];
		FormatAddress (m_Address, Address);
		LOGNOTE ("Ready: %s, \"%s\", %u baud", Address, LocalName, m_Uart.GetBaud ());
		m_State = StateReady;
		m_nScanIdle = CTimer::GetClockTicks () - SCAN_PAUSE_US;
		m_nLastCall = CTimer::GetClockTicks () - RECALL_US;
		m_bUserHangUp = FALSE;
		break;
	}
	}
}

void CBluetooth::CommandDone (u16 nOpcode, const u8 *pReturn, unsigned nBytes)
{
	u8 nStatus = nBytes ? pReturn[0] : 0xFF;
	if (m_State != StateStarting)
	{
		if (nStatus)
		{
			LOGWARN ("Command %04X: status %02X", nOpcode, nStatus);
		}
		return;
	}
	static const u16 Expected[] =
	{
		HCI_RESET, HCI_READ_LOCAL_VERSION, HCI_BCM_WRITE_BD_ADDR, HCI_READ_BD_ADDR, HCI_READ_BUFFER_SIZE, HCI_BCM_UPDATE_BAUDRATE,
		HCI_SET_EVENT_MASK, HCI_WRITE_SIMPLE_PAIRING_MODE, HCI_WRITE_INQUIRY_MODE, HCI_WRITE_CLASS_OF_DEVICE,
		HCI_WRITE_LOCAL_NAME, HCI_WRITE_SCAN_ENABLE
	};
	if (m_nStep >= StepDone || nOpcode != Expected[m_nStep])
	{
		return;
	}
	if (nStatus)
	{
		CString Why;
		Why.Format ("command %04X refused (status %02X)", nOpcode, nStatus);
		Fail (Why);
		return;
	}
	switch (m_nStep)
	{
	case StepVersion:
		if (nBytes >= 9)
		{
			LOGNOTE ("Controller: HCI %u.%u, LMP %u.%u, manufacturer %u", pReturn[1], pReturn[2] | pReturn[3] << 8,
				 pReturn[4], pReturn[7] | pReturn[8] << 8, pReturn[5] | pReturn[6] << 8);
		}
		break;

	case StepAddress:
		if (nBytes >= 7)
		{
			memcpy (m_Address, pReturn + 1, 6);
		}
		break;

	case StepBuffers:
		if (nBytes >= 8)
		{
			unsigned nPacketBytes = pReturn[1] | pReturn[2] << 8, nPackets = pReturn[4] | pReturn[5] << 8;
			LOGNOTE ("Controller: %u data packets of %u bytes", nPackets, nPacketBytes);
			m_HCI.SetBuffers (nPacketBytes, nPackets);
		}
		break;

	case StepBaud:
		m_Uart.SetBaud (BAUD_RUN);	// (its answer came at the old rate; from now the new)
		break;
	}
	m_nStep++;
	Step ();
}

void CBluetooth::Update (void)
{
	if (m_State == StateOff || m_State == StateFailed)
	{
		return;
	}
	m_HCI.Update ();

	unsigned nNow = CTimer::GetClockTicks ();
	if (m_State == StateStarting)
	{
		if (nNow - m_nStepStart > (m_nStep == StepReset ? RESET_TIMEOUT_US : STEP_TIMEOUT_US))
		{
			if (m_nStep == StepReset && ++m_nTries < 8)
			{
				// (it keeps the rate it was given through its reset: when it
				// was on before, it's still at that one)
				m_Uart.SetBaud (m_nTries & 1 ? BAUD_RUN : BAUD_START);
				m_HCI.Restart ();
				Step ();
			}
			else
			{
				CString Why;
				Why.Format ("no answer at step %u (%u bytes lost on the UART)", m_nStep, m_Uart.GetLost ());
				Fail (Why);
			}
		}
		return;
	}

	m_L2CAP.Update ();
	m_AVDTP.Update ();
	m_Audio.Update ();

	if (m_Link != LinkNone && m_Link != LinkReady)
	{
		if (nNow - m_nLinkSince > LINK_TIMEOUT_US)
		{
			HangUp ("It took too long");
		}
		else if (   m_Link == LinkSetup && m_bTheirCall && m_nSignalling == CBTL2CAP::None
			 && nNow - m_nLinkSince > THEIR_CHANNEL_US)
		{
			m_nSignalling = m_L2CAP.Open (PSM_AVDTP);	// (it didn't: from here, then)
		}
		return;
	}
	if (m_bPending && m_Link == LinkNone)
	{
		Call ();
	}
	else if (   m_Link == LinkNone && m_nBonds && !m_bUserHangUp && nNow - m_nLastCall >= RECALL_US
		 && !m_bInquiring && !m_bNaming)
	{
		// the speakers paired with: called in turn, till one is there (between
		// the scans too: one waiting to be called isn't found by them)
		m_nLastCall = nNow;
		Connect (m_Bond[m_nCallNext++ % m_nBonds].Address);
	}
	else if (m_bScanning && !m_AVDTP.IsStreaming ())
	{
		// (with a speaker connected too, while no sound goes to it: another one may be chosen)
		if (!m_bInquiring && !m_bNaming && nNow - m_nScanIdle >= SCAN_PAUSE_US)
		{
			Inquire ();
		}
	}
}

// ---- finding devices ---------------------------------------------------------------------

void CBluetooth::SetScanning (boolean bOn)
{
	if (bOn == m_bScanning)
	{
		return;
	}
	m_bScanning = bOn;
	if (!bOn && m_bInquiring)
	{
		m_HCI.Command (HCI_INQUIRY_CANCEL);
		m_bInquiring = FALSE;
		m_nScanIdle = CTimer::GetClockTicks ();
	}
	else if (bOn)
	{
		m_nScanIdle = CTimer::GetClockTicks () - SCAN_PAUSE_US;
	}
}

void CBluetooth::Inquire (void)
{
	// the general inquiry, 4 x 1.28 s, any number of answers
	static const u8 Params[5] = {0x33, 0x8B, 0x9E, 4, 0};
	m_HCI.Command (HCI_INQUIRY, Params, sizeof Params);
	m_bInquiring = TRUE;
	m_nScan++;
}

CBluetooth::TDevice *CBluetooth::Find (const u8 *pAddress)
{
	for (unsigned i = 0; i < m_nDevices; i++)
	{
		if (memcmp (m_Device[i].Address, pAddress, 6) == 0)
		{
			return &m_Device[i];
		}
	}
	return nullptr;
}

void CBluetooth::Found (const u8 *pAddress, u8 nPageScanMode, const u8 *pClass, u16 nClockOffset, int nRSSI,
			const u8 *pEIR)
{
	TDevice *p = Find (pAddress);
	if (!p)
	{
		if (m_nDevices == MaxDevices)
		{
			return;
		}
		p = &m_Device[m_nDevices++];
		memset (p, 0, sizeof *p);
		memcpy (p->Address, pAddress, 6);
		m_nGeneration++;
	}
	p->nClass = pClass[0] | pClass[1] << 8 | pClass[2] << 16;
	p->nPageScanMode = nPageScanMode;
	p->nClockOffset = nClockOffset;
	p->nSeen = m_nScan;
	if (nRSSI && nRSSI != p->nRSSI)
	{
		p->nRSSI = (s8) nRSSI;
	}
	// audio: the Audio/Video major class, or the audio service bit
	boolean bAudio = (p->nClass >> 8 & 0x1F) == 0x04 || (p->nClass & 0x200000);

	// the extended answer's fields: its name, the services' 16-bit UUIDs (0x110B: an audio sink)
	for (unsigned i = 0; pEIR && i + 1 < 240 && pEIR[i]; i += 1 + pEIR[i])
	{
		unsigned nLength = pEIR[i] - 1, nType = pEIR[i + 1];
		const u8 *pData = pEIR + i + 2;
		if (i + 1 + pEIR[i] > 240)
		{
			break;
		}
		if ((nType == 0x08 && !p->Name[0]) || nType == 0x09)
		{
			unsigned n = nLength < NameChars - 1 ? nLength : NameChars - 1;
			if (memcmp (p->Name, pData, n) != 0 || p->Name[n])
			{
				memcpy (p->Name, pData, n);
				p->Name[n] = '\0';
				m_nGeneration++;
			}
		}
		else if (nType == 0x02 || nType == 0x03)
		{
			for (unsigned k = 0; k + 1 < nLength; k += 2)
			{
				bAudio = bAudio || (pData[k] | pData[k + 1] << 8) == 0x110B;
			}
		}
	}
	if (bAudio != p->bAudio)
	{
		p->bAudio = bAudio;
		m_nGeneration++;
	}
}

// a scan is over: those not seen lately go; then the names not known yet, one at a time
void CBluetooth::InquiryDone (void)
{
	m_bInquiring = FALSE;
	for (unsigned i = 0; i < m_nDevices; )
	{
		if (m_nScan - m_Device[i].nSeen >= KEEP_SCANS)
		{
			m_Device[i] = m_Device[--m_nDevices];
			m_nGeneration++;
		}
		else
		{
			i++;
		}
	}
	AskName ();
}

void CBluetooth::AskName (void)
{
	m_bNaming = FALSE;
	for (unsigned i = 0; i < m_nDevices && m_bScanning; i++)
	{
		TDevice *p = &m_Device[i];
		if (p->bAudio && !p->Name[0] && !p->bNameAsked)
		{
			u8 Params[10];
			memcpy (Params, p->Address, 6);
			Params[6] = p->nPageScanMode;
			Params[7] = 0;
			Params[8] = (u8) p->nClockOffset;
			Params[9] = (u8) (p->nClockOffset >> 8) | 0x80;
			m_HCI.Command (HCI_REMOTE_NAME_REQUEST, Params, sizeof Params);
			p->bNameAsked = TRUE;
			m_bNaming = TRUE;
			return;
		}
	}
	m_nScanIdle = CTimer::GetClockTicks ();
}

unsigned CBluetooth::GetDevices (const TDevice **ppDevices) const
{
	// the audio devices, the strongest first
	TDevice *pSorted = const_cast<TDevice *> (m_Sorted);
	unsigned n = 0;
	for (unsigned i = 0; i < m_nDevices; i++)
	{
		if (!m_Device[i].bAudio)
		{
			continue;
		}
		unsigned k = n++;
		for (; k > 0 && (pSorted[k - 1].nRSSI ? pSorted[k - 1].nRSSI : -128) < (m_Device[i].nRSSI ? m_Device[i].nRSSI : -128); k--)
		{
			pSorted[k] = pSorted[k - 1];
		}
		pSorted[k] = m_Device[i];
	}
	*ppDevices = m_Sorted;
	return n;
}

// ---- the controller's events -----------------------------------------------------------

void CBluetooth::OnEvent (u8 nCode, const u8 *p, unsigned nBytes)
{
	switch (nCode)
	{
	case HCI_EV_COMMAND_COMPLETE:
		if (nBytes >= 3)
		{
			CommandDone (p[1] | p[2] << 8, p + 3, nBytes - 3);
		}
		break;

	case HCI_EV_COMMAND_STATUS:
		if (nBytes >= 4 && p[0])
		{
			u16 nOpcode = p[2] | p[3] << 8;
			LOGWARN ("Command %04X: status %02X", nOpcode, p[0]);
			if (nOpcode == HCI_INQUIRY)
			{
				m_bInquiring = FALSE;
				m_nScanIdle = CTimer::GetClockTicks ();
			}
			else if (nOpcode == HCI_CREATE_CONNECTION && m_Link == LinkConnecting)
			{
				LinkLost (StatusText (p[0]));
			}
			else if (nOpcode == HCI_REMOTE_NAME_REQUEST)
			{
				AskName ();
			}
		}
		break;

	case HCI_EV_HARDWARE_ERROR:
		LOGWARN ("The controller's hardware error %02X", nBytes ? p[0] : 0);
		break;

	case HCI_EV_INQUIRY_RESULT:			// each field of all the answers, one after another
		for (unsigned i = 0, n = nBytes ? p[0] : 0; i < n && 1 + 14 * n <= nBytes; i++)
		{
			const u8 *q = p + 1;
			Found (q + 6 * i, q[6 * n + i], q + 9 * n + 3 * i, q[12 * n + 2 * i] | q[12 * n + 2 * i + 1] << 8, 0, nullptr);
		}
		break;

	case HCI_EV_INQUIRY_RESULT_RSSI:
		for (unsigned i = 0, n = nBytes ? p[0] : 0; i < n && 1 + 14 * n <= nBytes; i++)
		{
			const u8 *q = p + 1;
			Found (q + 6 * i, q[6 * n + i], q + 8 * n + 3 * i, q[11 * n + 2 * i] | q[11 * n + 2 * i + 1] << 8,
			       (s8) q[13 * n + i], nullptr);
		}
		break;

	case HCI_EV_EXTENDED_INQUIRY_RESULT:
		if (nBytes >= 255)
		{
			Found (p + 1, p[7], p + 9, p[12] | p[13] << 8, (s8) p[14], p + 15);
		}
		break;

	case HCI_EV_INQUIRY_COMPLETE:
		if (m_bInquiring)
		{
			InquiryDone ();
		}
		break;

	case HCI_EV_CONNECTION_COMPLETE:
	case HCI_EV_CONNECTION_REQUEST:
	case HCI_EV_DISCONNECTION_COMPLETE:
	case HCI_EV_AUTHENTICATION_COMPLETE:
	case HCI_EV_ENCRYPTION_CHANGE:
	case HCI_EV_PIN_CODE_REQUEST:
	case HCI_EV_LINK_KEY_REQUEST:
	case HCI_EV_LINK_KEY_NOTIFICATION:
	case HCI_EV_IO_CAPABILITY_REQUEST:
	case HCI_EV_USER_CONFIRMATION_REQUEST:
	case HCI_EV_SIMPLE_PAIRING_COMPLETE:
		LinkEvent (nCode, p, nBytes);
		break;

	case HCI_EV_REMOTE_NAME_COMPLETE:
		if (nBytes >= 7)
		{
			TDevice *pDevice = Find (p + 1);
			if (pDevice && p[0] == 0)
			{
				unsigned n = 0;
				while (n < NameChars - 1 && 7 + n < nBytes && p[7 + n])
				{
					pDevice->Name[n] = (char) p[7 + n];
					n++;
				}
				pDevice->Name[n] = '\0';
				m_nGeneration++;
			}
			if (m_bNaming)
			{
				AskName ();
			}
		}
		break;
	}
}

void CBluetooth::OnFrame (u16 nHandle, const u8 *pFrame, unsigned nBytes)
{
	if (m_Link != LinkNone && nHandle == m_nHandle)
	{
		m_L2CAP.OnFrame (pFrame, nBytes);
	}
}

// ---- the connection ---------------------------------------------------------------------

const char *CBluetooth::StatusText (u8 nStatus)
{
	switch (nStatus)
	{
	case 0x04:	return "It doesn't answer";		// (page timeout)
	case 0x05:	return "Pairing failed";		// (authentication failure)
	case 0x06:	return "It has forgotten the pairing";	// (key missing)
	case 0x08:	return "The connection was lost";	// (connection timeout)
	case 0x0E:
	case 0x0F:	return "It refused the connection";
	case 0x13:	return "It hung up";
	case 0x15:	return "It was switched off";
	case 0x16:	return "";				// (we hung up)
	case 0x18:	return "It doesn't pair now";		// (pairing not allowed)
	case 0x22:	return "It didn't answer in time";
	default:	return "The connection failed";
	}
}

void CBluetooth::SetLink (TLink Link)
{
	m_Link = Link;
	m_nLinkSince = CTimer::GetClockTicks ();
	m_nGeneration++;
}

boolean CBluetooth::BondChanged (void)
{
	boolean bChanged = m_bBondChanged;
	m_bBondChanged = FALSE;
	return bChanged;
}

void CBluetooth::SetBonds (const TBond *pBonds, unsigned nBonds)
{
	m_nBonds = nBonds < MaxBonds ? nBonds : MaxBonds;
	memcpy (m_Bond, pBonds, m_nBonds * sizeof (TBond));
	m_nGeneration++;
}

const CBluetooth::TBond *CBluetooth::FindBond (const u8 *pAddress) const
{
	for (unsigned i = 0; i < m_nBonds; i++)
	{
		if (memcmp (m_Bond[i].Address, pAddress, 6) == 0)
		{
			return &m_Bond[i];
		}
	}
	return nullptr;
}

// a speaker paired with, or used: the list's first (the oldest dropped, if there's no room)
void CBluetooth::Remember (const TBond &Bond)
{
	TBond New = Bond;
	unsigned i = 0;
	while (i < m_nBonds && memcmp (m_Bond[i].Address, New.Address, 6) != 0)
	{
		i++;
	}
	if (i == 0 && m_nBonds && memcmp (&m_Bond[0], &New, sizeof New) == 0)
	{
		return;					// (it is already)
	}
	if (i == m_nBonds && m_nBonds < MaxBonds)
	{
		m_nBonds++;
	}
	else if (i == m_nBonds)
	{
		i = MaxBonds - 1;
	}
	for (; i > 0; i--)
	{
		m_Bond[i] = m_Bond[i - 1];
	}
	m_Bond[0] = New;
	m_bBondChanged = TRUE;
	m_nGeneration++;
}

void CBluetooth::Forget (const u8 *pAddress)
{
	for (unsigned i = 0; i < m_nBonds; i++)
	{
		if (memcmp (m_Bond[i].Address, pAddress, 6) == 0)
		{
			for (unsigned k = i; k + 1 < m_nBonds; k++)
			{
				m_Bond[k] = m_Bond[k + 1];
			}
			m_nBonds--;
			m_bBondChanged = TRUE;
			m_nGeneration++;
			break;
		}
	}
	if (m_Link != LinkNone && memcmp (pAddress, m_LinkAddress, 6) == 0)
	{
		Disconnect ();
	}
}

const char *CBluetooth::GetName (const u8 *pAddress)
{
	const TBond *pBond = FindBond (pAddress);
	const TDevice *pDevice = Find (pAddress);
	if (pDevice && pDevice->Name[0])
	{
		return pDevice->Name;
	}
	if (pBond && pBond->Name[0])
	{
		return pBond->Name;
	}
	FormatAddress (pAddress, m_NameText);
	return m_NameText;
}

void CBluetooth::Connect (const u8 *pAddress)
{
	if (m_State != StateReady)
	{
		return;
	}
	if (m_Link != LinkNone && memcmp (pAddress, m_LinkAddress, 6) == 0)
	{
		return;					// (it is, or is being)
	}
	memcpy (m_Pending, pAddress, 6);
	m_bPending = TRUE;
	m_bUserHangUp = FALSE;
	if (m_Link != LinkNone)				// the other one first let go
	{
		HangUp ("");
	}
}

void CBluetooth::Disconnect (void)
{
	m_bPending = FALSE;
	m_bUserHangUp = TRUE;
	if (m_Link != LinkNone)
	{
		HangUp ("");
	}
}

// the connection asked for (m_Pending), now that nothing else is connected
void CBluetooth::Call (void)
{
	m_bPending = FALSE;
	if (m_bInquiring)
	{
		m_HCI.Command (HCI_INQUIRY_CANCEL);
		m_bInquiring = FALSE;
	}
	memcpy (m_LinkAddress, m_Pending, 6);
	const TDevice *pDevice = Find (m_LinkAddress);
	u8 Params[13];
	memcpy (Params, m_LinkAddress, 6);
	Params[6] = 0x18;				// the packets it may use: DM1, DH1, DM3, DH3, DM5, DH5
	Params[7] = 0xCC;
	Params[8] = pDevice ? pDevice->nPageScanMode : 1;
	Params[9] = 0;
	Params[10] = pDevice ? (u8) pDevice->nClockOffset : 0;
	Params[11] = pDevice ? (u8) (pDevice->nClockOffset >> 8) | 0x80 : 0;
	Params[12] = 1;					// (it may ask to be the master)
	m_HCI.Command (HCI_CREATE_CONNECTION, Params, sizeof Params);
	m_bTheirCall = FALSE;
	m_bKeyUsed = m_bKeyRefused = m_bEncrypted = FALSE;
	m_nSignalling = m_nMedia = CBTL2CAP::None;
	m_LinkError[0] = '\0';
	char Address[18];
	FormatAddress (m_LinkAddress, Address);
	LOGNOTE ("Connecting to %s", Address);
	SetLink (LinkConnecting);
}

// the connection is to end: asked of the controller (LinkLost follows)
void CBluetooth::HangUp (const char *pWhy)
{
	if (pWhy[0])
	{
		strncpy (m_LinkError, pWhy, sizeof m_LinkError - 1);
		m_LinkError[sizeof m_LinkError - 1] = '\0';
	}
	if (m_Link == LinkConnecting && !m_bTheirCall)
	{
		m_HCI.Command (HCI_CREATE_CONNECTION_CANCEL, m_LinkAddress, 6);
	}
	else if (m_Link != LinkClosing && m_Link != LinkNone)
	{
		const u8 Params[3] = {(u8) m_nHandle, (u8) (m_nHandle >> 8), 0x13};
		m_HCI.Command (HCI_DISCONNECT, Params, sizeof Params);
	}
	if (m_Link != LinkNone)
	{
		SetLink (LinkClosing);
	}
}

// the connection is gone
void CBluetooth::LinkLost (const char *pWhy)
{
	if (pWhy[0] && !m_LinkError[0])
	{
		strncpy (m_LinkError, pWhy, sizeof m_LinkError - 1);
		m_LinkError[sizeof m_LinkError - 1] = '\0';
	}
	LOGNOTE ("Disconnected%s%s", m_LinkError[0] ? ": " : "", m_LinkError);
	m_Link = LinkClosing;				// (its channels go with it: nothing more to hang up)
	m_L2CAP.LinkDown ();
	m_HCI.Disconnected (m_nHandle);
	m_nSignalling = m_nMedia = CBTL2CAP::None;
	SetLink (LinkNone);
	m_nLastCall = CTimer::GetClockTicks ();
	m_nScanIdle = CTimer::GetClockTicks ();
}

void CBluetooth::LinkEvent (u8 nCode, const u8 *p, unsigned nBytes)
{
	switch (nCode)
	{
	case HCI_EV_CONNECTION_REQUEST:			// they call: the address, the class, the kind of link
		if (nBytes >= 10)
		{
			// taken from a speaker paired with, when nothing else is going on
			// (and it wasn't let go by hand)
			boolean bTake = p[9] == 1 && m_Link == LinkNone && !m_bUserHangUp && FindBond (p);
			u8 Params[7];
			memcpy (Params, p, 6);
			Params[6] = bTake ? 0x00 : 0x0D;	// (we'd be the master; or: no room for it)
			m_HCI.Command (bTake ? HCI_ACCEPT_CONNECTION_REQUEST : HCI_REJECT_CONNECTION_REQUEST, Params, sizeof Params);
			if (bTake)
			{
				memcpy (m_LinkAddress, p, 6);
				m_bTheirCall = TRUE;
				m_bPending = FALSE;
				m_bKeyUsed = m_bKeyRefused = m_bEncrypted = FALSE;
				m_nSignalling = m_nMedia = CBTL2CAP::None;
				m_LinkError[0] = '\0';
				LOGNOTE ("A paired speaker calls: \"%s\"", GetName (p));
				SetLink (LinkConnecting);
			}
		}
		break;

	case HCI_EV_CONNECTION_COMPLETE:		// the status, the handle, the address
		if (nBytes >= 11 && m_Link == LinkConnecting && memcmp (p + 3, m_LinkAddress, 6) == 0)
		{
			if (p[0])
			{
				LinkLost (StatusText (p[0]));
				break;
			}
			m_nHandle = (p[1] | p[2] << 8) & 0x0FFF;
			m_L2CAP.LinkUp (m_nHandle);
			LOGNOTE ("Connected (handle %u)", m_nHandle);
			SetLink (LinkPairing);
			const u8 Params[2] = {(u8) m_nHandle, (u8) (m_nHandle >> 8)};
			m_HCI.Command (HCI_AUTHENTICATION_REQUESTED, Params, sizeof Params);
		}
		break;

	case HCI_EV_DISCONNECTION_COMPLETE:		// the status, the handle, the reason
		if (nBytes >= 4 && m_Link != LinkNone && ((p[1] | p[2] << 8) & 0x0FFF) == m_nHandle && m_Link != LinkConnecting)
		{
			LinkLost (StatusText (p[3]));
		}
		break;

	case HCI_EV_LINK_KEY_REQUEST:			// the key we have for this address, or none
		if (nBytes >= 6)
		{
			u8 Params[22];
			memcpy (Params, p, 6);
			const TBond *pBond = FindBond (p);
			if (pBond && !m_bKeyRefused)
			{
				memcpy (Params + 6, pBond->Key, 16);
				m_HCI.Command (HCI_LINK_KEY_REQUEST_REPLY, Params, 22);
				m_bKeyUsed = TRUE;
			}
			else
			{
				m_HCI.Command (HCI_LINK_KEY_REQUEST_NEGATIVE_REPLY, Params, 6);
			}
		}
		break;

	case HCI_EV_IO_CAPABILITY_REQUEST:		// pairing: no display, no keys here; no proof against a man in the middle
		if (nBytes >= 6)
		{
			u8 Params[9];
			memcpy (Params, p, 6);
			Params[6] = 0x03;
			Params[7] = 0x00;
			Params[8] = 0x04;		// (bonding: the key is kept)
			m_HCI.Command (HCI_IO_CAPABILITY_REQUEST_REPLY, Params, sizeof Params);
		}
		break;

	case HCI_EV_USER_CONFIRMATION_REQUEST:		// (nobody to ask: yes)
		if (nBytes >= 6)
		{
			m_HCI.Command (HCI_USER_CONFIRMATION_REQUEST_REPLY, p, 6);
		}
		break;

	case HCI_EV_PIN_CODE_REQUEST:			// an old device: its PIN is 0000
		if (nBytes >= 6)
		{
			u8 Params[23];
			memset (Params, 0, sizeof Params);
			memcpy (Params, p, 6);
			Params[6] = 4;
			memcpy (Params + 7, "0000", 4);
			m_HCI.Command (HCI_PIN_CODE_REQUEST_REPLY, Params, sizeof Params);
		}
		break;

	case HCI_EV_SIMPLE_PAIRING_COMPLETE:
		if (nBytes >= 1 && p[0])
		{
			LOGNOTE ("Pairing: status %02X", p[0]);
		}
		break;

	case HCI_EV_LINK_KEY_NOTIFICATION:		// paired: the address, the key, its kind
		if (nBytes >= 23 && m_Link != LinkNone && memcmp (p, m_LinkAddress, 6) == 0)
		{
			TBond Bond;
			memset (&Bond, 0, sizeof Bond);
			memcpy (Bond.Address, p, 6);
			memcpy (Bond.Key, p + 6, 16);
			Bond.nKeyType = p[22];
			const TDevice *pDevice = Find (p);
			const TBond *pOld = FindBond (p);
			strcpy (Bond.Name, pDevice && pDevice->Name[0] ? pDevice->Name : pOld ? pOld->Name : "");
			Remember (Bond);
			m_bKeyRefused = FALSE;
			LOGNOTE ("Paired with \"%s\" (key type %u)", Bond.Name, Bond.nKeyType);
		}
		break;

	case HCI_EV_AUTHENTICATION_COMPLETE:		// the status, the handle
		if (nBytes >= 3 && m_Link == LinkPairing)
		{
			const u8 Params[3] = {(u8) m_nHandle, (u8) (m_nHandle >> 8), 1};
			if (p[0] == 0)
			{
				m_HCI.Command (HCI_SET_CONNECTION_ENCRYPTION, Params, sizeof Params);
			}
			else if (m_bKeyUsed && !m_bKeyRefused)
			{
				// (the device lost its key: paired anew)
				LOGNOTE ("The stored key wasn't taken (status %02X): pairing anew", p[0]);
				m_bKeyRefused = TRUE;
				m_HCI.Command (HCI_AUTHENTICATION_REQUESTED, Params, 2);
			}
			else
			{
				HangUp (StatusText (p[0] == 0x06 ? 0x05 : p[0]));
			}
		}
		break;

	case HCI_EV_ENCRYPTION_CHANGE:			// the status, the handle, on or off
		if (nBytes >= 4 && m_Link == LinkPairing)
		{
			if (p[0] || !p[3])
			{
				HangUp ("Encryption failed");
				break;
			}
			// The stream's signalling channel: opened by the one that called. A
			// speaker that called opens it itself, at once: opened from both
			// sides, each takes its own for the signalling and the other's for
			// the transport, and nothing answers (a JBL GO calling back)
			m_bEncrypted = TRUE;
			SetLink (LinkSetup);
			if (m_nSignalling == CBTL2CAP::None && !m_bTheirCall)
			{
				m_nSignalling = m_L2CAP.Open (PSM_AVDTP);
			}
		}
		break;
	}
}

// ---- the channels --------------------------------------------------------------------------

boolean CBluetooth::OnChannelAsked (u16 nPSM)
{
	// their questions about our service; the stream's two channels (the
	// signalling first, then the transport), if they open them
	return nPSM == PSM_SDP || (nPSM == PSM_AVDTP && (m_nSignalling == CBTL2CAP::None || m_nMedia == CBTL2CAP::None));
}

void CBluetooth::OnChannelOpen (unsigned nChannel, u16 nPSM, boolean bTheirs)
{
	if (nPSM != PSM_AVDTP)
	{
		return;
	}
	if (bTheirs && m_nSignalling == CBTL2CAP::None)
	{
		m_nSignalling = nChannel;
	}
	else if (bTheirs && nChannel != m_nSignalling && m_nMedia == CBTL2CAP::None)
	{
		m_nMedia = nChannel;
	}
	if (nChannel == m_nSignalling)
	{
		m_AVDTP.SignallingOpen ();
	}
	else if (nChannel == m_nMedia)
	{
		m_AVDTP.MediaOpen ();
	}
}

void CBluetooth::OnChannelData (unsigned nChannel, u16 nPSM, const u8 *pData, unsigned nBytes)
{
	if (nPSM == PSM_SDP)
	{
		u8 Answer[160];
		unsigned n = SDPAnswer (pData, nBytes, Answer, sizeof Answer);
		if (n)
		{
			m_L2CAP.Send (nChannel, Answer, n);
		}
	}
	else if (nChannel == m_nSignalling)
	{
		m_AVDTP.OnSignal (pData, nBytes);
	}
}

void CBluetooth::OnChannelClosed (unsigned nChannel, u16 nPSM)
{
	if (nChannel == m_nSignalling)
	{
		m_nSignalling = CBTL2CAP::None;
		m_AVDTP.SignallingClosed ();
		if (m_Link == LinkSetup || m_Link == LinkReady)
		{
			HangUp ("It doesn't take sound");
		}
	}
	else if (nChannel == m_nMedia)
	{
		m_nMedia = CBTL2CAP::None;
		m_AVDTP.MediaClosed ();
	}
}

boolean CBluetooth::SendSignal (const u8 *pData, unsigned nBytes)
{
	return m_L2CAP.Send (m_nSignalling, pData, nBytes);
}

void CBluetooth::OpenMedia (void)
{
	if (m_nMedia == CBTL2CAP::None)
	{
		m_nMedia = m_L2CAP.Open (PSM_AVDTP);
	}
}

void CBluetooth::CloseMedia (void)
{
	m_L2CAP.Close (m_nMedia);
}

// the stream changed: ready for sound, or not (any more)
void CBluetooth::OnStream (void)
{
	m_Audio.OnStream ();
	if (m_AVDTP.IsStreaming () && m_bInquiring)	// (sound goes: the air is its)
	{
		m_HCI.Command (HCI_INQUIRY_CANCEL);
		m_bInquiring = FALSE;
		m_nScanIdle = CTimer::GetClockTicks ();
	}
	if (m_Link == LinkSetup && m_AVDTP.IsReady ())
	{
		SetLink (LinkReady);
		const TBond *pBond = FindBond (m_LinkAddress);
		if (pBond)				// (the last used: the first called next time)
		{
			Remember (*pBond);
		}
	}
	else if ((m_Link == LinkSetup || m_Link == LinkReady) && m_AVDTP.GetState () == CBTAVDTP::Failed)
	{
		HangUp ("It doesn't take sound");
	}
}

unsigned CBluetooth::GetMediaMTU (void)
{
	return m_L2CAP.IsOpen (m_nMedia) ? m_L2CAP.GetMTU (m_nMedia) : 0;
}

// (a media packet waits for nothing: no room on the way, and it's dropped)
boolean CBluetooth::SendMedia (const u8 *pPacket, unsigned nBytes)
{
	return m_HCI.GetQueued () < 8 && m_L2CAP.Send (m_nMedia, pPacket, nBytes);
}
