//
// tv_service.cpp
//
#include "tv_service.h"
#include <circle/logger.h>
#include <circle/sched/scheduler.h>
#include <circle/timer.h>
#include <circle/util.h>

LOGMODULE ("tv");

#define FOURCC(a, b, c, d)	((u32) (a) << 24 | (u32) (b) << 16 | (u32) (c) << 8 | (u32) (d))
#define ANSWER_US		3000000

// the TV service's commands (vc_tvservice_defs.h: VC_TV_CMD_CODE_T)
enum { TV_HDMI_ON_PREFERRED = 1, TV_HDMI_ON_EXPLICIT = 3 };

CTVService::CTVService (void)
:	m_bThere (FALSE)
{
}

void CTVService::Callback (void *pParam, const VCHI_CALLBACK_REASON_T Reason, void *hMessage)
{
	// (the answers are looked for: Exchange)
}

boolean CTVService::Initialize (void)
{
	if (vchi_initialise (&m_Instance) != 0 || vchi_connect (0, 0, m_Instance) != 0)
	{
		LOGWARN ("No VCHI: the HDMI mode stays as it was at boot");
		return FALSE;
	}
	SERVICE_CREATION_T Commands = {VCHI_VERSION (1), FOURCC ('G', 'C', 'M', 'D'), 0, 0, 0, Callback, this, 0, 0, 0};
	SERVICE_CREATION_T TV = {VCHI_VERSION (1), FOURCC ('T', 'V', 'S', 'V'), 0, 0, 0, Callback, this, 1, 1, 0};
	if (   vchi_service_open (m_Instance, &Commands, &m_hCommands) != 0
	    || vchi_service_open (m_Instance, &TV, &m_hTV) != 0)
	{
		LOGWARN ("The firmware's command or TV service didn't open: the HDMI mode stays as it was at boot");
		return FALSE;
	}
	vchi_service_release (m_hCommands);
	vchi_service_release (m_hTV);
	m_bThere = TRUE;

	return TRUE;
}

// a message, and the one that comes back (VCHIQ's tasks bring it: they run while we wait)
boolean CTVService::Exchange (VCHI_SERVICE_HANDLE_T hService, const void *pMessage, unsigned nBytes,
			      void *pReply, unsigned nMax, unsigned *pLength)
{
	if (!m_bThere)
	{
		return FALSE;
	}
	vchi_service_use (hService);
	uint32_t nLength = 0;
	while (vchi_msg_dequeue (hService, pReply, nMax, &nLength, VCHI_FLAGS_NONE) == 0 && nLength)
	{
		nLength = 0;			// (an answer nobody waited for)
	}
	boolean bOK = vchi_msg_queue (hService, pMessage, nBytes, VCHI_FLAGS_BLOCK_UNTIL_QUEUED, 0) == 0;
	unsigned nStart = CTimer::GetClockTicks ();
	nLength = 0;
	while (bOK && (vchi_msg_dequeue (hService, pReply, nMax, &nLength, VCHI_FLAGS_NONE) != 0 || !nLength))
	{
		nLength = 0;
		if (CTimer::GetClockTicks () - nStart > ANSWER_US)
		{
			bOK = FALSE;
			break;
		}
		CScheduler::Get ()->MsSleep (2);
	}
	vchi_service_release (hService);
	*pLength = nLength;

	return bOK;
}

boolean CTVService::Command (const char *pCommand, char *pAnswer, unsigned nSize)
{
	static u8 Reply[1024];			// its result (4 bytes), then its text
	unsigned nLength = 0;
	pAnswer[0] = '\0';
	if (!Exchange (m_hCommands, pCommand, strlen (pCommand) + 1, Reply, sizeof Reply - 1, &nLength) || nLength < 4)
	{
		return FALSE;
	}
	Reply[nLength] = '\0';
	strncpy (pAnswer, (const char *) Reply + 4, nSize - 1);
	pAnswer[nSize - 1] = '\0';

	return TRUE;
}

int CTVService::PowerOnPreferred (void)
{
	const u32 Message[2] = {TV_HDMI_ON_PREFERRED, 0};		// (not in 3D)
	s32 nResult = -1;
	unsigned nLength = 0;
	return Exchange (m_hTV, Message, sizeof Message, &nResult, sizeof nResult, &nLength) && nLength >= 4 ? nResult : -1;
}

int CTVService::PowerOnExplicit (unsigned nDVIOrHDMI, unsigned nGroup, unsigned nMode)
{
	const u32 Message[4] = {TV_HDMI_ON_EXPLICIT, nDVIOrHDMI, nGroup, nMode};
	s32 nResult = -1;
	unsigned nLength = 0;
	return Exchange (m_hTV, Message, sizeof Message, &nResult, sizeof nResult, &nLength) && nLength >= 4 ? nResult : -1;
}
