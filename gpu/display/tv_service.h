//
// tv_service.h
//
// Two of the VideoCore's services, over VCHIQ (as vcgencmd and tvservice on
// Linux use them; their messages: Circle's addon/vc4/interface/vmcs_host):
// the general commands ("GCMD": a line of text, a line back) and the TV
// service ("TVSV"), which turns HDMI on in a mode of our choice. With them
// the HDMI mode can follow a monitor plugged in after boot: config.txt's
// settings are the firmware's at boot only.
//
#ifndef _gpu_display_tv_service_h
#define _gpu_display_tv_service_h

#include <circle/types.h>
#include <vc4/vchi/vchi.h>

class CTVService
{
public:
	// (vc_hdmi.h: HDMI_RES_GROUP_T, HDMI_MODE_T)
	enum { GroupCEA = 1, GroupDMT = 2 };
	enum { ModeDVI = 1, ModeHDMI = 2 };

	CTVService (void);

	/// \brief Open the services (VCHIQ is up)
	boolean Initialize (void);
	boolean IsThere (void) const		{ return m_bThere; }

	/// \brief A general command, as vcgencmd's
	/// \param pAnswer the firmware's text (nSize bytes of room)
	/// \return FALSE if no answer came
	boolean Command (const char *pCommand, char *pAnswer, unsigned nSize);

	/// \brief HDMI on in the mode the firmware prefers for the monitor
	/// \return The firmware's result (0: it will), -1: no answer
	int PowerOnPreferred (void);
	/// \brief HDMI on in this mode (of a group: CEA's or DMT's numbers; 87 is the custom one)
	int PowerOnExplicit (unsigned nDVIOrHDMI, unsigned nGroup, unsigned nMode);

private:
	static void Callback (void *pParam, const VCHI_CALLBACK_REASON_T Reason, void *hMessage);
	boolean Exchange (VCHI_SERVICE_HANDLE_T hService, const void *pMessage, unsigned nBytes,
			  void *pReply, unsigned nMax, unsigned *pLength);

private:
	boolean m_bThere;
	VCHI_INSTANCE_T m_Instance;
	VCHI_SERVICE_HANDLE_T m_hCommands, m_hTV;
};

#endif
