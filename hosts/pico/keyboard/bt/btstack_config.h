/*
 * BTstack configuration for picosdl's Bluetooth HID keyboard backend.
 *
 * Derived from the pico-examples Pico W Bluetooth config - Copyright (c) 2020
 * Raspberry Pi (Trading) Ltd., BSD-3-Clause; see README here - and trimmed to
 * what a dual-mode (Classic + LE) HID *host* actually needs. The buffer/flow-control
 * numbers are deliberately left as upstream tuned them: they exist to stop the
 * CYW43 shared SPI bus from overrunning, and lowering them causes dropped ACL
 * packets rather than saving anything useful.
 */
#ifndef PICO_BT_KEYBOARD_BTSTACK_CONFIG_H
#define PICO_BT_KEYBOARD_BTSTACK_CONFIG_H

// ---------------------------------------------------------------- features --

// ENABLE_LOG_ERROR is deliberately off. BTstack routes log_error through the
// HCI dump layer, which discards everything unless an implementation has been
// installed with hci_dump_init() -- so with dumping off the format strings are
// ~6 KB of flash that can never be printed. Turn it back on (together with an
// hci_dump_init call) when debugging the stack.
//#define ENABLE_LOG_ERROR

// Required to compile BTstack's HCI dump; costs nothing in the binary because
// the dump code is unreferenced and the linker drops it.
#define ENABLE_PRINTF_HEXDUMP

#ifdef ENABLE_BLE
#define ENABLE_GATT_CLIENT_PAIRING
#define ENABLE_LE_CENTRAL
#define ENABLE_LE_DATA_LENGTH_EXTENSION
// LE keyboards advertise with resolvable private addresses that rotate every
// few minutes, so address resolution is what makes reconnecting to a bonded
// keyboard work at all. It drags in ENABLE_LE_PERIPHERAL: BTstack's GAP privacy
// code touches advertising state that is only declared for the peripheral role.
#define ENABLE_LE_PERIPHERAL
#define ENABLE_LE_PRIVACY_ADDRESS_RESOLUTION
#define ENABLE_LE_SECURE_CONNECTIONS
#endif

// Note: ENABLE_L2CAP_ENHANCED_RETRANSMISSION_MODE is *not* set. The stock Pico
// config enables it for A2DP/AVDTP, which need it; HIDP runs its control and
// interrupt channels in L2CAP Basic mode and BTstack's hid_host.c contains no
// reference to ERTM at all. Leaving it out saves ~4 KB.

#if defined(ENABLE_CLASSIC) && defined(ENABLE_BLE)
// Lets a dual-mode keyboard paired over one transport be trusted on the other.
#define ENABLE_CROSS_TRANSPORT_KEY_DERIVATION
#endif

// ----------------------------------------------------------------- buffers --

#define HCI_OUTGOING_PRE_BUFFER_SIZE 4
#define HCI_ACL_PAYLOAD_SIZE (1691 + 4)
#define HCI_ACL_CHUNK_SIZE_ALIGNMENT 4

#define MAX_NR_BTSTACK_LINK_KEY_DB_MEMORY_ENTRIES 2
#define MAX_NR_GATT_CLIENTS 1
#define MAX_NR_HCI_CONNECTIONS 2
#define MAX_NR_HID_HOST_CONNECTIONS 1
#define MAX_NR_HIDS_CLIENTS 1
#define MAX_NR_L2CAP_CHANNELS 4
#define MAX_NR_L2CAP_SERVICES 3
#define MAX_NR_SERVICE_RECORD_ITEMS 4
#define MAX_NR_SM_LOOKUP_ENTRIES 3
#define MAX_NR_WHITELIST_ENTRIES 16
#define MAX_NR_LE_DEVICE_DB_ENTRIES 16

// Limit ACL buffers used by the stack to avoid CYW43 shared bus overrun.
#define MAX_NR_CONTROLLER_ACL_BUFFERS 3
#define MAX_NR_CONTROLLER_SCO_PACKETS 3

#define ENABLE_HCI_CONTROLLER_TO_HOST_FLOW_CONTROL
#define HCI_HOST_ACL_PACKET_LEN 1024
#define HCI_HOST_ACL_PACKET_NUM 3
#define HCI_HOST_SCO_PACKET_LEN 120
#define HCI_HOST_SCO_PACKET_NUM 3

// Bonding storage: TLV on the flash sector interface (set up by cyw43_arch_init).
#define NVM_NUM_DEVICE_DB_ENTRIES 16
#define NVM_NUM_LINK_KEYS 16

// No malloc is given to BTstack, so the ATT DB is fixed size.
#define MAX_ATT_DB_SIZE 512

// -------------------------------------------------------------------- port --

#define HAVE_EMBEDDED_TIME_MS
#define HAVE_ASSERT
#define HAVE_BTSTACK_STDIN

#define ENABLE_SOFTWARE_AES128
#define ENABLE_MICRO_ECC_FOR_LE_SECURE_CONNECTIONS

#endif // PICO_BT_KEYBOARD_BTSTACK_CONFIG_H
