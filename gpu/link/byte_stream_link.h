//
// byte_stream_link.h
//
// A link whose command stream comes as bytes, not words (docs/protocol.md 4):
// the PC's, over USB (usb_link: the serial port; usb_bulk_link: the GL
// interface). Packets are found by their header and CRC; anything else (text
// before the stream, a host's zero padding) is skipped a byte at a time. The
// derived class supplies the bytes and takes the replies' bytes.
//
#ifndef _byte_stream_link_h
#define _byte_stream_link_h

#include "link.h"
#include <circle/types.h>

class CByteStreamLink : public CLink
{
public:
	CByteStreamLink (void);
	~CByteStreamLink (void);

	boolean Initialize (void);

	const u32 *GetPacket (u32 *pHeader);
	boolean SendReply (u8 uchOpcode, const u32 *pPayload, unsigned nLength);

	u32 GetCRCErrors (void) const		{ return m_nCRCErrors; }
	unsigned GetFreeBytes (void) const	{ return BufferBytes - m_nBytes; }
	unsigned GetBufferBytes (void) const	{ return BufferBytes; }

protected:
	/// \return Bytes of the stream copied to pBuffer (at most nMax, 0 if none)
	virtual unsigned ReadStream (void *pBuffer, unsigned nMax) = 0;

	/// \brief Send a reply's bytes (all of them, or none)
	/// \return FALSE if there's no room now
	virtual boolean WriteStream (const void *pData, unsigned nLength) = 0;

private:
	u32 CRC (u32 nCRC, u32 nWord) const;

private:
	static const unsigned BufferBytes = (16386 + 16) * 4;	// a maximum-size packet
	u8 *m_pBuffer;
	unsigned m_nBytes;			// in the buffer
	unsigned m_nStart;			// parse position
	unsigned m_nConsumed;			// size of the packet returned last
	u32 m_CRCTable[256];
	unsigned m_nCRCErrors;
};

#endif
