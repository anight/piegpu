/*
 * Bluetooth Classic (BR/EDR) transport: HID Host profile.
 *
 * Older and "multi-device" keyboards speak Classic HID rather than LE. Finding
 * one means a GAP inquiry and filtering on Class of Device, then an SDP query
 * for the report map (BTstack's hid_host does that for us) and two L2CAP
 * channels, control and interrupt.
 *
 * Note the two pairing paths. Secure Simple Pairing (BT 2.1+) has the host
 * display a passkey that the user types on the keyboard. Legacy pairing has the
 * host choose a PIN and the user type that instead. A keyboard is an input
 * device with no display, so in both cases we are the side that shows a number
 * and the user is the side that types it -- which is why the IO capability is
 * DisplayOnly and why the legacy PIN is generated rather than hardcoded.
 */
#include "../psdl_pico_log.h"
#include "bt_app.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "pico/rand.h"

#include "debug.h"
#include "hid_report.h"
#include "kbd_decode.h"

// Report map fetched over SDP.
static uint8_t hid_descriptor_storage[512];

static btstack_packet_callback_registration_t hci_callback_registration;

static uint16_t hid_cid;
static bool     descriptor_available;
static bool     boot_mode;              // set if we fell back to Boot protocol
static bool     link_reported;          // whether bt_app has been told we are up
static hid_led_layout_t led_layout;

// Inquiry runs in units of 1.28s. Four units is long enough for a keyboard in
// pairing mode to answer, short enough to alternate with an LE scan.
#define INQUIRY_DURATION_UNITS 4

static void classic_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

// ------------------------------------------------------- device filtering --

// Class of Device layout: bits 8..12 are the major device class, bits 2..7 the
// minor. For the Peripheral major class, minor bits 6..7 say what kind:
// 1 = keyboard, 2 = pointing device, 3 = combo.
static bool cod_is_keyboard(uint32_t cod) {
    uint8_t major = (uint8_t) ((cod >> 8) & 0x1f);
    if (major != 0x05) return false;

    uint8_t peripheral_kind = (uint8_t) ((cod >> 6) & 0x03);
    return peripheral_kind == 0x01 || peripheral_kind == 0x03;
}

// -------------------------------------------------------------------- LEDs --

void bt_classic_set_leds(uint8_t led_mask) {
    if (hid_cid == 0) return;

    uint8_t report[8];
    uint8_t report_id;
    uint8_t report_len;

    if (boot_mode) {
        report_id = 0;
        report[0] = led_mask & 0x07;
        report_len = 1;
    } else {
        report_len = hid_led_build_report(&led_layout, led_mask, report, sizeof(report));
        if (report_len == 0) return;
        report_id = (uint8_t) led_layout.report_id;
    }

    hid_host_send_set_report(hid_cid, HID_REPORT_TYPE_OUTPUT, report_id, report, report_len);
}

// --------------------------------------------------------------- lifecycle --

void bt_classic_init(void) {
    hid_host_init(hid_descriptor_storage, sizeof(hid_descriptor_storage));
    hid_host_register_packet_handler(&classic_packet_handler);

    // Let the keyboard drop into sniff mode between keystrokes (this is most of
    // its battery life) and let it take the master role if it wants to.
    gap_set_default_link_policy_settings(LM_LINK_POLICY_ENABLE_SNIFF_MODE |
                                         LM_LINK_POLICY_ENABLE_ROLE_SWITCH);
    hci_set_master_slave_policy(HCI_ROLE_MASTER);

    // We can show a passkey but have no keyboard of our own to confirm with.
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_DISPLAY_ONLY);

    // Stay visible and connectable so a bonded keyboard can reconnect to us
    // on its own after a power cycle.
    gap_discoverable_control(1);
    gap_connectable_control(1);

    hci_callback_registration.callback = &classic_packet_handler;
    hci_add_event_handler(&hci_callback_registration);
}

void bt_classic_start_search(void) {
    uint8_t status = gap_inquiry_start(INQUIRY_DURATION_UNITS);
    if (status != ERROR_CODE_SUCCESS) {
        bt_app_search_finished(BT_LINK_CLASSIC);
    }
}

void bt_classic_stop_search(void) {
    gap_inquiry_stop();
}

void bt_classic_connect(const bd_addr_t addr) {
    descriptor_available = false;
    boot_mode = false;

    uint8_t status = hid_host_connect((uint8_t *) addr,
                                      HID_PROTOCOL_MODE_REPORT_WITH_FALLBACK_TO_BOOT,
                                      &hid_cid);
    if (status != ERROR_CODE_SUCCESS) {
        psdl_log("[classic] connect refused, status 0x%02x\n", status);
        hid_cid = 0;
        bt_app_link_down(BT_LINK_CLASSIC, "connect refused");
    }
}

void bt_classic_disconnect(void) {
    if (hid_cid == 0) return;
    hid_host_disconnect(hid_cid);
}

// ------------------------------------------------------------ HID reports --

static void classic_handle_report(const uint8_t *report, uint16_t report_len) {
    dbg("[classic] report");
    dbg_bytes("", report, report_len);

    // Reports arrive with the HID transaction header still attached; 0xa1 is
    // HANDSHAKE/DATA with an Input report payload behind it.
    if (report_len < 1) return;
    if (report[0] != 0xa1) return;
    report++;
    report_len--;

    const uint8_t *descriptor;
    uint16_t descriptor_len;

    if (boot_mode) {
        descriptor = btstack_hid_get_boot_descriptor_data();
        descriptor_len = btstack_hid_get_boot_descriptor_len();
    } else {
        descriptor = hid_descriptor_storage_get_descriptor_data(hid_cid);
        descriptor_len = hid_descriptor_storage_get_descriptor_len(hid_cid);
    }

    if (descriptor_len == 0) {
        psdl_log("[classic] cannot decode: no HID descriptor\n");
        return;
    }

    kbd_report_t kbd;
    kbd_media_t  media;
    hid_report_to_kbd(descriptor, descriptor_len, report, report_len, &kbd, &media);
    /* Only the halves this report actually described. A media report says nothing
     * about which ordinary keys are held, and kbd_decode_report() would read that
     * silence as "all released". */
    if (media.describes_keyboard) kbd_decode_report(&kbd);
    kbd_decode_media(&media);
}

// Announce the link once we can actually interpret what the keyboard sends.
static void classic_ready(void) {
    if (link_reported) return;
    link_reported = true;

    if (boot_mode) {
        led_layout.found = false;
    } else {
        hid_led_layout_find(&led_layout,
                            hid_descriptor_storage_get_descriptor_data(hid_cid),
                            hid_descriptor_storage_get_descriptor_len(hid_cid));
        dbg("[classic] HID descriptor: %u bytes\n",
            hid_descriptor_storage_get_descriptor_len(hid_cid));
    }

    psdl_log("[classic] HID ready (%s protocol mode)\n", boot_mode ? "boot" : "report");
    bt_app_link_up(BT_LINK_CLASSIC);
}

// ---------------------------------------------------------------- events --

static void handle_hid_event(uint8_t *packet) {
    uint8_t status;

    switch (hci_event_hid_meta_get_subevent_code(packet)) {
        case HID_SUBEVENT_INCOMING_CONNECTION:
            // A bonded keyboard reconnecting to us after waking up.
            hid_host_accept_connection(hid_subevent_incoming_connection_get_hid_cid(packet),
                                       HID_PROTOCOL_MODE_REPORT_WITH_FALLBACK_TO_BOOT);
            break;

        case HID_SUBEVENT_CONNECTION_OPENED:
            status = hid_subevent_connection_opened_get_status(packet);
            if (status != ERROR_CODE_SUCCESS) {
                psdl_log("[classic] connection failed, status 0x%02x\n", status);
                hid_cid = 0;
                bt_app_link_down(BT_LINK_CLASSIC, "connection failed");
                break;
            }
            hid_cid = hid_subevent_connection_opened_get_hid_cid(packet);
            descriptor_available = false;
            link_reported = false;
            psdl_log("[classic] connected\n");
            break;

        case HID_SUBEVENT_DESCRIPTOR_AVAILABLE:
            status = hid_subevent_descriptor_available_get_status(packet);
            if (status == ERROR_CODE_SUCCESS) {
                descriptor_available = true;
                classic_ready();
            } else {
                // Without the report map we cannot decode Report mode data,
                // but Boot mode has a fixed layout we already know.
                psdl_log("[classic] no HID descriptor (status 0x%02x), assuming boot layout\n", status);
                boot_mode = true;
                descriptor_available = true;
                classic_ready();
            }
            break;

        case HID_SUBEVENT_SET_PROTOCOL_RESPONSE:
            status = hid_subevent_set_protocol_response_get_handshake_status(packet);
            if (status != HID_HANDSHAKE_PARAM_TYPE_SUCCESSFUL) {
                psdl_log("[classic] set protocol failed, status 0x%02x\n", status);
                break;
            }
            // Only reported when the negotiation landed on Boot mode, which is
            // exactly the case where we must switch descriptors.
            if ((hid_protocol_mode_t) hid_subevent_set_protocol_response_get_protocol_mode(packet)
                    == HID_PROTOCOL_MODE_BOOT) {
                boot_mode = true;
            }
            break;

        case HID_SUBEVENT_REPORT:
            if (!descriptor_available) break;
            classic_handle_report(hid_subevent_report_get_report(packet),
                                  hid_subevent_report_get_report_len(packet));
            break;

        case HID_SUBEVENT_CONNECTION_CLOSED:
            hid_cid = 0;
            descriptor_available = false;
            link_reported = false;
            bt_app_link_down(BT_LINK_CLASSIC, "keyboard disconnected");
            break;

        default:
            break;
    }
}

static void classic_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    UNUSED(channel);
    UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET) return;

    bd_addr_t addr;

    switch (hci_event_packet_get_type(packet)) {
        case GAP_EVENT_INQUIRY_RESULT: {
            uint32_t cod = gap_event_inquiry_result_get_class_of_device(packet);
            if (!cod_is_keyboard(cod)) break;

            gap_event_inquiry_result_get_bd_addr(packet, addr);

            char name[32];
            name[0] = 0;
            if (gap_event_inquiry_result_get_name_available(packet)) {
                uint8_t len = gap_event_inquiry_result_get_name_len(packet);
                if (len >= sizeof(name)) len = sizeof(name) - 1;
                memcpy(name, gap_event_inquiry_result_get_name(packet), len);
                name[len] = 0;
            }

            bt_app_device_found(BT_LINK_CLASSIC, addr, BD_ADDR_TYPE_ACL, name);
            break;
        }

        case GAP_EVENT_INQUIRY_COMPLETE:
            bt_app_search_finished(BT_LINK_CLASSIC);
            break;

        case HCI_EVENT_PIN_CODE_REQUEST: {
            // Legacy pairing: we pick the PIN, the user types it. A fixed
            // "0000" only works on keyboards that ship with that PIN.
            hci_event_pin_code_request_get_bd_addr(packet, addr);
            char pin[7];
            snprintf(pin, sizeof(pin), "%06" PRIu32, get_rand_32() % 1000000u);
            psdl_log("\n>>> Type %s on the keyboard, then press Enter <<<\n", pin);
            bt_app_show_passkey(pin);
            gap_pin_code_response(addr, pin);
            break;
        }

        case HCI_EVENT_USER_PASSKEY_NOTIFICATION: {
            // Secure Simple Pairing: the controller picked the passkey.
            char digits[7];
            snprintf(digits, sizeof(digits), "%06" PRIu32,
                     hci_event_user_passkey_notification_get_numeric_value(packet));
            psdl_log("\n>>> Type %s on the keyboard, then press Enter <<<\n", digits);
            bt_app_show_passkey(digits);
            break;
        }

        case HCI_EVENT_USER_CONFIRMATION_REQUEST:
            // Auto-accepted by BTstack; shown so the log explains itself.
            psdl_log("[classic] pairing confirmation %06" PRIu32 "\n",
                   little_endian_read_32(packet, 8));
            break;

        case HCI_EVENT_HID_META:
            handle_hid_event(packet);
            break;

        default:
            break;
    }
}
