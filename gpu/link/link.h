//
// link.h
//
// A link from the host: the command stream in, replies out (docs/protocol.md
// 4 and 9). The kernel takes packets from the first active link of its list
// and hands the Commands the link to reply on. i2s_link: the Pico's I2S (PCM
// slave); usb_link: a PC over the RPi's USB (tests). A new transport (USB 2,
// ...) is another class of this kind.
//
#ifndef _link_h
#define _link_h

#include <circle/types.h>

class CLink
{
public:
	virtual ~CLink (void) {}

	virtual boolean Initialize (void) = 0;

	/// \return TRUE while the host uses this link (the kernel serves the first
	///	    active link of its list and discards what the others receive)
	virtual boolean IsActive (void)				{ return TRUE; }

	/// \brief Housekeeping, once per main loop (flow control, ...)
	virtual void Update (void)				{ }

	/// \brief The next complete packet with a good CRC
	/// \param pHeader Receives the header word
	/// \return Its payload (LENGTH words, valid until the next call), or nullptr
	virtual const u32 *GetPacket (u32 *pHeader) = 0;

	/// \brief Send a reply packet (header and CRC are added)
	/// \return FALSE if it was dropped (the backlog is full: Flush and retry)
	virtual boolean SendReply (u8 uchOpcode, const u32 *pPayload, unsigned nLength) = 0;

	/// \brief Let queued replies go out (after SendReply returned FALSE)
	virtual void Flush (void)				{ }

	/// \return Packets dropped for a bad CRC since boot (STATUS)
	virtual u32 GetCRCErrors (void) const = 0;

	/// \return Room for commands, bytes (STATUS)
	virtual unsigned GetFreeBytes (void) const = 0;

	/// \return Size of the command buffer, bytes (INFO)
	virtual unsigned GetBufferBytes (void) const = 0;
};

#endif
