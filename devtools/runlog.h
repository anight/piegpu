//
// runlog.h
//
// The log of this run, kept in RAM that survives a restart (a watchdog reset
// or reboot () doesn't clear it; power-off does), so the next run can tell how
// the previous one ended and show its last lines: a crash can't send its
// message anywhere (the USB link needs the interrupts it just lost), a hang
// has none.
//
// The ring is the top of the ARM memory, which Circle keeps out of its heap
// and pages (MEM_PERSISTENT_SIZE: patches/circle-persistent-memory.patch,
// devtools/configure-circle.sh): the firmware leaves it alone at a restart
// (the low memory it clears: the Zero 2 W's first ~4 MB). It's written from the
// logger's event hook, before the logger's own target (so a panic's message
// is in it before anything else can fail).
//
#ifndef _devtools_runlog_h
#define _devtools_runlog_h

#include <circle/logger.h>
#include <circle/spinlock.h>
#include <circle/types.h>

class CRunLog
{
public:
	enum TEnd
	{
		EndNone,	// no previous run: power-on (or the RAM didn't keep it)
		EndRunning,	// it didn't say: hang (the watchdog), reset, power glitch
		EndPanic,	// crash: exception, assertion, panic
		EndRestart,	// restarted as asked (Restarting ())
		EndUnknown
	};

	CRunLog (void);

	/// \brief Takes the previous run's log and starts this one's
	/// \param nLogLevel The logger's: the ring keeps what the log shows
	/// \note Call right after the logger's Initialize ().
	boolean Initialize (CLogger *pLogger, unsigned nLogLevel);

	/// \brief Logs how the previous run ended (with its last lines if it
	/// didn't restart as asked)
	void Report (void);

	TEnd GetPreviousEnd (void) const	{ return m_PreviousEnd; }
	/// \return Runs since power-on, this one included
	unsigned GetRun (void) const		{ return m_nRun; }
	/// \return The previous run's last lines ("" if none)
	const char *GetPreviousLines (void) const	{ return m_PreviousLines; }

	static const char *GetEndName (TEnd End);

	/// \brief This run ends with a restart that was asked for (call before reboot ())
	static void Restarting (void);

private:
	void Append (const char *pText);
	void Flush (unsigned nFrom, unsigned nLength);

	static void EventHandler (void);

private:
	struct THeader
	{
		u32 Magic;
		u32 Check;		// ~Magic ^ nRun
		u32 nRun;
		u32 End;		// TEnd
		u32 nIn;		// write position in the ring
		u32 bWrapped;
	};

	THeader *m_pHeader;		// nullptr: no ring (the kernel doesn't leave room)
	char *m_pRing;

	CLogger *m_pLogger;
	unsigned m_nLogLevel;
	CSpinLock m_SpinLock;

	TEnd m_PreviousEnd;
	unsigned m_nRun;
	static const unsigned MaxLines = 10;
	char m_PreviousLines[MaxLines * (LOG_MAX_SOURCE + LOG_MAX_MESSAGE + 16) + 1];

	static CRunLog *s_pThis;
};

#endif
