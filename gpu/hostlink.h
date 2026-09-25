//
// hostlink.h
//
// The command stream over the Zero's USB (CDevLink stream mode): the same
// packets as on the I2S link (docs/protocol.md 4), sent by a PC that runs the
// GL layer itself (pico/gpulink/host: tests such as dEQP). Replies go back the
// same way. While the host streams, the I2S input is ignored.
//
#ifndef _hostlink_h
#define _hostlink_h

#include <devlink.h>
#include <circle/types.h>

class CHostLink
{
public:
	CHostLink (CDevLink *pDevLink);
	~CHostLink (void);

	boolean Initialize (void);

	/// \brief The next complete packet with a good CRC
	/// \return Its payload (valid until the next call), or nullptr
	const u32 *GetPacket (u32 *pHeader);

	/// \brief Send a reply packet (header and CRC are added)
	boolean SendReply (u8 uchOpcode, const u32 *pPayload, unsigned nLength);

	unsigned GetCRCErrors (void) const	{ return m_nCRCErrors; }

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
};

#endif
