//
// usb_link.h
//
// The command stream over the Zero's USB (CDevLink stream mode): the same
// packets as on the I2S link (docs/protocol.md 4), sent by a PC that runs the
// GL layer itself (hosts/pc, transports/pc-usb: tests such as dEQP). Replies
// go back the same way, with CREDIT replies for flow control. Active once the
// host has switched the USB link to its binary stream.
//
#ifndef _usb_link_h
#define _usb_link_h

#include "link.h"
#include <devlink.h>
#include <circle/types.h>

class CUSBLink : public CLink	/// a PC over the Zero's USB (CDevLink)
{
public:
	CUSBLink (CDevLink *pDevLink);
	~CUSBLink (void);

	boolean Initialize (void);

	boolean IsActive (void);

	/// \brief Tell the host how much of its stream has been taken (CREDIT)
	void Update (void);

	const u32 *GetPacket (u32 *pHeader);

	boolean SendReply (u8 uchOpcode, const u32 *pPayload, unsigned nLength);

	u32 GetCRCErrors (void) const		{ return m_nCRCErrors; }
	unsigned GetFreeBytes (void) const	{ return BufferBytes - m_nBytes; }
	unsigned GetBufferBytes (void) const	{ return BufferBytes; }

private:
	u32 CRC (u32 nCRC, u32 nWord) const;

private:
	CDevLink *m_pDevLink;

	static const unsigned BufferBytes = (16386 + 16) * 4;	// a maximum-size packet
	u8 *m_pBuffer;
	unsigned m_nBytes;			// in the buffer
	unsigned m_nStart;			// parse position
	unsigned m_nConsumed;			// size of the packet returned last

	u32 m_CRCTable[256];
	unsigned m_nCRCErrors;

	u32 m_nCredited;			// stream bytes reported to the host
};

#endif
