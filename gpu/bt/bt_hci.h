//
// bt_hci.h
//
// The controller's host interface over the UART (H4: a packet's type in a
// byte before it): commands out, one at a time as the controller allows;
// events in; data both ways (ACL), an L2CAP frame cut into the controller's
// packets on the way out, as many under way as it has buffers for, and put
// together again on the way in.
//
// Opcodes and event codes: the Bluetooth Core specification, vol 4 part E.
//
#ifndef _gpu_bt_bt_hci_h
#define _gpu_bt_bt_hci_h

#include "bt_uart.h"
#include <circle/types.h>

#define HCI_OP(ogf, ocf)			((ogf) << 10 | (ocf))
// link control
#define HCI_INQUIRY				HCI_OP (1, 0x01)
#define HCI_INQUIRY_CANCEL			HCI_OP (1, 0x02)
#define HCI_CREATE_CONNECTION			HCI_OP (1, 0x05)
#define HCI_DISCONNECT				HCI_OP (1, 0x06)
#define HCI_CREATE_CONNECTION_CANCEL		HCI_OP (1, 0x08)
#define HCI_ACCEPT_CONNECTION_REQUEST		HCI_OP (1, 0x09)
#define HCI_REJECT_CONNECTION_REQUEST		HCI_OP (1, 0x0A)
#define HCI_LINK_KEY_REQUEST_REPLY		HCI_OP (1, 0x0B)
#define HCI_LINK_KEY_REQUEST_NEGATIVE_REPLY	HCI_OP (1, 0x0C)
#define HCI_PIN_CODE_REQUEST_REPLY		HCI_OP (1, 0x0D)
#define HCI_AUTHENTICATION_REQUESTED		HCI_OP (1, 0x11)
#define HCI_SET_CONNECTION_ENCRYPTION		HCI_OP (1, 0x13)
#define HCI_REMOTE_NAME_REQUEST			HCI_OP (1, 0x19)
#define HCI_REMOTE_NAME_REQUEST_CANCEL		HCI_OP (1, 0x1A)
#define HCI_IO_CAPABILITY_REQUEST_REPLY		HCI_OP (1, 0x2B)
#define HCI_USER_CONFIRMATION_REQUEST_REPLY	HCI_OP (1, 0x2C)
// link policy
#define HCI_WRITE_LINK_POLICY_SETTINGS		HCI_OP (2, 0x0D)
#define HCI_WRITE_DEFAULT_LINK_POLICY		HCI_OP (2, 0x0F)
// the controller and the baseband
#define HCI_SET_EVENT_MASK			HCI_OP (3, 0x01)
#define HCI_RESET				HCI_OP (3, 0x03)
#define HCI_WRITE_LOCAL_NAME			HCI_OP (3, 0x13)
#define HCI_WRITE_PAGE_TIMEOUT			HCI_OP (3, 0x18)
#define HCI_WRITE_SCAN_ENABLE			HCI_OP (3, 0x1A)
#define HCI_WRITE_CLASS_OF_DEVICE		HCI_OP (3, 0x24)
#define HCI_WRITE_INQUIRY_MODE			HCI_OP (3, 0x45)
#define HCI_WRITE_EXTENDED_INQUIRY_RESPONSE	HCI_OP (3, 0x52)
#define HCI_WRITE_SIMPLE_PAIRING_MODE		HCI_OP (3, 0x56)
// what the controller is
#define HCI_READ_LOCAL_VERSION			HCI_OP (4, 0x01)
#define HCI_READ_BUFFER_SIZE			HCI_OP (4, 0x05)
#define HCI_READ_BD_ADDR			HCI_OP (4, 0x09)
// Broadcom's
#define HCI_BCM_WRITE_BD_ADDR			HCI_OP (0x3F, 0x01)
#define HCI_BCM_UPDATE_BAUDRATE			HCI_OP (0x3F, 0x18)
#define HCI_BCM_DOWNLOAD_MINIDRIVER		HCI_OP (0x3F, 0x2E)
#define HCI_BCM_WRITE_RAM			HCI_OP (0x3F, 0x4C)
#define HCI_BCM_LAUNCH_RAM			HCI_OP (0x3F, 0x4E)

#define HCI_EV_INQUIRY_COMPLETE			0x01
#define HCI_EV_INQUIRY_RESULT			0x02
#define HCI_EV_CONNECTION_COMPLETE		0x03
#define HCI_EV_CONNECTION_REQUEST		0x04
#define HCI_EV_DISCONNECTION_COMPLETE		0x05
#define HCI_EV_AUTHENTICATION_COMPLETE		0x06
#define HCI_EV_REMOTE_NAME_COMPLETE		0x07
#define HCI_EV_ENCRYPTION_CHANGE		0x08
#define HCI_EV_COMMAND_COMPLETE			0x0E
#define HCI_EV_COMMAND_STATUS			0x0F
#define HCI_EV_HARDWARE_ERROR			0x10
#define HCI_EV_ROLE_CHANGE			0x12
#define HCI_EV_NUMBER_OF_COMPLETED_PACKETS	0x13
#define HCI_EV_PIN_CODE_REQUEST			0x16
#define HCI_EV_LINK_KEY_REQUEST			0x17
#define HCI_EV_LINK_KEY_NOTIFICATION		0x18
#define HCI_EV_MAX_SLOTS_CHANGE			0x1B
#define HCI_EV_INQUIRY_RESULT_RSSI		0x22
#define HCI_EV_EXTENDED_INQUIRY_RESULT		0x2F
#define HCI_EV_IO_CAPABILITY_REQUEST		0x31
#define HCI_EV_IO_CAPABILITY_RESPONSE		0x32
#define HCI_EV_USER_CONFIRMATION_REQUEST	0x33
#define HCI_EV_SIMPLE_PAIRING_COMPLETE		0x36

class CBTHCIClient
{
public:
	virtual ~CBTHCIClient (void) {}
	/// \param pParams The event's parameters (nBytes of them)
	virtual void OnEvent (u8 nCode, const u8 *pParams, unsigned nBytes) = 0;
	/// \param pFrame A whole L2CAP frame: its length, its channel, its payload
	virtual void OnFrame (u16 nHandle, const u8 *pFrame, unsigned nBytes) = 0;
};

class CBTHCI
{
public:
	static const unsigned MaxFrame = 2048;		// an L2CAP frame, with its header

	CBTHCI (CBTUart *pUart, CBTHCIClient *pClient);

	/// \brief As after the controller's reset: nothing queued, one command allowed
	void Restart (void);
	/// \brief The controller's data buffers (Read Buffer Size)
	void SetBuffers (unsigned nPacketBytes, unsigned nPackets);
	/// \brief Log every packet (its first bytes)
	void SetTrace (boolean bTrace)		{ m_bTrace = bTrace; }

	/// \brief Call often: what was received, what waits to be sent
	void Update (void);

	/// \brief Queue a command
	boolean Command (u16 nOpcode, const void *pParams = nullptr, unsigned nBytes = 0);
	/// \return Commands not yet sent
	unsigned GetCommandsQueued (void) const	{ return m_nCommandIn - m_nCommandOut; }

	/// \brief Queue an L2CAP frame (its header included) for a connection
	/// \return FALSE if there's no room (it's dropped)
	boolean SendFrame (u16 nHandle, const u8 *pFrame, unsigned nBytes);
	/// \return The controller's packets a frame of nBytes takes, if all could be queued now; else 0
	unsigned GetRoom (unsigned nBytes) const;
	/// \return Packets queued here, and those the controller holds
	unsigned GetQueued (void) const		{ return m_nPacketIn - m_nPacketOut; }
	unsigned GetPending (void) const	{ return m_nPending; }
	/// \brief The connection is gone: what the controller held of it is too
	void Disconnected (u16 nHandle);

private:
	void Receive (void);
	void Packet (void);
	void Send (void);
	void Trace (const char *pWhat, const u8 *p, unsigned nBytes);

private:
	static const unsigned Commands = 16, CommandBytes = 4 + 255;
	static const unsigned Packets = 32, PacketBytes = 5 + 1024;

	CBTUart *m_pUart;
	CBTHCIClient *m_pClient;
	boolean m_bTrace;

	// receiving: the packet being read
	u8 m_Rx[5 + 1024 + 16];
	unsigned m_nRx, m_nRxNeed;
	unsigned m_nSkipped;
	// the frame being put together
	u8 m_Frame[MaxFrame];
	unsigned m_nFrame, m_nFrameNeed;
	u16 m_nFrameHandle;

	// commands: a ring of them, sent as the controller allows
	u8 (*m_pCommand)[CommandBytes];
	unsigned m_nCommandIn, m_nCommandOut;
	unsigned m_nCommandCredits;

	// data: a ring of packets, sent while the controller has buffers
	struct TPacket
	{
		u16 nBytes;
		u8 Data[PacketBytes];
	};
	TPacket *m_pPacket;
	unsigned m_nPacketIn, m_nPacketOut;
	unsigned m_nPacketBytes;		// a packet's data, at most
	unsigned m_nBuffers;			// the controller's
	unsigned m_nPending;			// packets it holds
};

#endif
