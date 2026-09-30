//
// usb_link.h
//
// The command stream over the RPi's USB serial port (CDevLink stream mode):
// the same packets as on the I2S link (docs/protocol.md 4), sent by a PC that
// runs the GL layer itself (hosts/pc, transports/pc-usb) without access to
// the GL interface (usb_bulk_link). Replies go back the same way, with CREDIT
// replies for flow control. Active once the host has switched the serial port
// to its binary stream.
//
#ifndef _usb_link_h
#define _usb_link_h

#include "byte_stream_link.h"
#include <devlink.h>
#include <circle/types.h>

class CUSBLink : public CByteStreamLink	/// a PC over the RPi's USB serial port (CDevLink)
{
public:
	CUSBLink (CDevLink *pDevLink);
	~CUSBLink (void);

	boolean IsActive (void);

	/// \brief Tell the host how much of its stream has been taken (CREDIT)
	void Update (void);
	void EndSession (void);

private:
	unsigned ReadStream (void *pBuffer, unsigned nMax);
	boolean WriteStream (const void *pData, unsigned nLength);

private:
	CDevLink *m_pDevLink;
	u32 m_nCredited;			// stream bytes reported to the host
};

#endif
