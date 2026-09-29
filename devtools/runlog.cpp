//
// runlog.cpp
//
#include "runlog.h"
#include <circle/bcmpropertytags.h>
#include <circle/synchronize.h>
#include <circle/string.h>
#include <circle/timer.h>
#include <circle/util.h>
#include <assert.h>

#ifndef MEM_PERSISTENT_SIZE
	#error Circle keeps no memory for the run log: devtools/configure-circle.sh
#endif

#define RUNLOG_MAGIC		0x52554E4C	// "RUNL"
#define RUNLOG_RING_SIZE	(MEM_PERSISTENT_SIZE - sizeof (THeader))

LOGMODULE ("runlog");

CRunLog *CRunLog::s_pThis = nullptr;

CRunLog::CRunLog (void)
:	m_pHeader (nullptr),
	m_pRing (nullptr),
	m_pLogger (nullptr),
	m_nLogLevel (LogNotice),
	m_PreviousEnd (EndNone),
	m_nRun (1)
{
	m_PreviousLines[0] = '\0';
}

boolean CRunLog::Initialize (CLogger *pLogger, unsigned nLogLevel)
{
	assert (pLogger);
	m_pLogger = pLogger;
	m_nLogLevel = nLogLevel;

	CBcmPropertyTags Tags;
	TPropertyTagMemory TagMemory;
	if (!Tags.GetTag (PROPTAG_GET_ARM_MEMORY, &TagMemory, sizeof TagMemory))
	{
		LOGWARN ("No run log: no ARM memory size");

		return TRUE;
	}

	m_pHeader = (THeader *) (uintptr) (TagMemory.nBaseAddress + TagMemory.nSize - MEM_PERSISTENT_SIZE);
	m_pRing = (char *) (m_pHeader + 1);

	// the previous run's (checked: after power-on the RAM holds anything)
	THeader *p = m_pHeader;
	if (   p->Magic == RUNLOG_MAGIC
	    && p->Check == (~RUNLOG_MAGIC ^ p->nRun)
	    && p->End > EndNone && p->End < EndUnknown
	    && p->nIn < RUNLOG_RING_SIZE)
	{
		m_PreviousEnd = (TEnd) p->End;
		m_nRun = p->nRun + 1;

		// its last lines, from the end back
		unsigned nCount = p->bWrapped ? RUNLOG_RING_SIZE : p->nIn;
		unsigned nOldest = p->bWrapped ? p->nIn : 0;
		unsigned nLines = 0, nStart = nCount;
		while (nStart > 0 && nCount - nStart < sizeof m_PreviousLines - 1)
		{
			char c = m_pRing[(nOldest + nStart - 1) % RUNLOG_RING_SIZE];
			if (c == '\n' && nStart < nCount && ++nLines == MaxLines)
			{
				break;
			}
			nStart--;
		}
		unsigned n = 0;
		for (unsigned i = nStart; i < nCount; i++)
		{
			char c = m_pRing[(nOldest + i) % RUNLOG_RING_SIZE];
			m_PreviousLines[n++] = c == '\n' || (c >= ' ' && c < 0x7F) ? c : '?';
		}
		m_PreviousLines[n] = '\0';
	}

	// this run's
	p->Magic = RUNLOG_MAGIC;
	p->nRun = m_nRun;
	p->Check = ~RUNLOG_MAGIC ^ m_nRun;
	p->End = EndRunning;
	p->nIn = 0;
	p->bWrapped = FALSE;
	Flush (0, 0);

	s_pThis = this;
	pLogger->RegisterEventNotificationHandler (EventHandler);

	return TRUE;
}

void CRunLog::Report (void)
{
	switch (m_PreviousEnd)
	{
	case EndNone:
		LOGNOTE ("Run 1 since power-on");
		return;

	case EndRestart:
		LOGNOTE ("Run %u since power-on (the previous one restarted as asked)", m_nRun);
		return;

	default:
		break;
	}

	LOGWARN ("Run %u since power-on: the previous one %s; its last lines:", m_nRun,
		 m_PreviousEnd == EndPanic ? "crashed" : "stopped without a word (hang: the watchdog; or a reset)");

	char *pLine = m_PreviousLines;
	while (*pLine)
	{
		char *pEnd = strchr (pLine, '\n');
		if (pEnd)
		{
			*pEnd = '\0';
		}
		LOGWARN ("| %s", pLine);
		if (!pEnd)
		{
			break;
		}
		*pEnd = '\n';
		pLine = pEnd + 1;
	}
}

const char *CRunLog::GetEndName (TEnd End)
{
	switch (End)
	{
	case EndNone:		return "none";
	case EndRunning:	return "stopped";
	case EndPanic:		return "crash";
	case EndRestart:	return "restart";
	default:		return "unknown";
	}
}

void CRunLog::Restarting (void)
{
	if (s_pThis && s_pThis->m_pHeader)
	{
		s_pThis->m_pHeader->End = EndRestart;
		s_pThis->Flush (0, 0);
	}
}

void CRunLog::Append (const char *pText)
{
	THeader *p = m_pHeader;
	while (*pText)
	{
		m_pRing[p->nIn++] = *pText++;
		if (p->nIn == RUNLOG_RING_SIZE)
		{
			p->nIn = 0;
			p->bWrapped = TRUE;
		}
	}
}

// the header and nLength bytes of the ring from nFrom out of the data cache:
// a reset doesn't write it back (the Zero's Circle maps the memory cached)
void CRunLog::Flush (unsigned nFrom, unsigned nLength)
{
	CleanAndInvalidateDataCacheRange ((uintptr) m_pHeader, sizeof (THeader));
	if (nFrom + nLength > RUNLOG_RING_SIZE)
	{
		CleanAndInvalidateDataCacheRange ((uintptr) m_pRing, nFrom + nLength - RUNLOG_RING_SIZE);
		nLength = RUNLOG_RING_SIZE - nFrom;
	}
	if (nLength)
	{
		CleanAndInvalidateDataCacheRange ((uintptr) (m_pRing + nFrom), nLength);
	}
}

void CRunLog::EventHandler (void)
{
	CRunLog *pThis = s_pThis;
	assert (pThis);

	TLogSeverity Severity;
	char Source[LOG_MAX_SOURCE];
	char Message[LOG_MAX_MESSAGE];
	time_t Time;
	unsigned nHundredth;
	int nTimeZone;
	while (pThis->m_pLogger->ReadEvent (&Severity, Source, Message, &Time, &nHundredth, &nTimeZone))
	{
		if ((unsigned) Severity > pThis->m_nLogLevel)
		{
			continue;
		}

		unsigned nTicks = CTimer::Get ()->GetTicks ();
		CString Line;
		Line.Format ("%u.%02u %s: %s\n", nTicks / HZ, nTicks % HZ * 100 / HZ, Source, Message);

		pThis->m_SpinLock.Acquire ();
		unsigned nFrom = pThis->m_pHeader->nIn;
		pThis->Append (Line);
		if (Severity == LogPanic)
		{
			pThis->m_pHeader->End = EndPanic;
		}
		pThis->Flush (nFrom, Line.GetLength ());
		pThis->m_SpinLock.Release ();
	}
}
