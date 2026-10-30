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
#include "mouse_decode.h"
#include "gamepad_decode.h"

// Report map fetched over SDP.
static uint8_t hid_descriptor_storage[512];

static btstack_packet_callback_registration_t hci_callback_registration;

static uint16_t hid_cid;
static bool     descriptor_available;
static bool     boot_mode;              // set if we fell back to Boot protocol
static bool     link_reported;          // whether bt_app has been told we are up
static hid_led_layout_t led_layout;

// A report map asked for again (ask_for_report_map): the attribute, and the map within it.
static uint8_t  late_attribute[512];
static uint16_t late_attribute_len;
static const uint8_t *late_map;
static uint16_t late_map_len;
static bd_addr_t remote_addr;
static btstack_context_callback_registration_t late_query;

// Inquiry runs in units of 1.28s. Four units is long enough for a keyboard in
// pairing mode to answer, short enough to alternate with an LE scan.
#define INQUIRY_DURATION_UNITS 4

static void classic_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

// ------------------------------------------------------- device filtering --

// Class of Device layout: bits 8..12 are the major device class, bits 2..7 the
// minor. For the Peripheral major class, minor bits 6..7 say what kind:
// 1 = keyboard, 2 = pointing device, 3 = combo.
// (In piegpu: or a pointing device, when that is what the search takes.)
static bool cod_is_keyboard(uint32_t cod) {
    uint8_t major = (uint8_t) ((cod >> 8) & 0x1f);
    if (major != 0x05) return false;
    if (bt_app_looking_for() == BT_DEVICE_GAMEPAD) return true;   // (any peripheral: see bt_app.h)

    uint8_t peripheral_kind = (uint8_t) ((cod >> 6) & 0x03);
    uint8_t wanted_kind = bt_app_looking_for() == BT_DEVICE_MOUSE ? 0x02 : 0x01;
    return peripheral_kind == wanted_kind || peripheral_kind == 0x03;
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
    if (bt_app_looking_for() == BT_DEVICE_GAMEPAD) {
        static uint8_t last[24];
        static uint16_t last_len;
        static int shown;
        if (shown < 40 && (report_len != last_len || memcmp(report, last, report_len < sizeof(last) ? report_len : sizeof(last)) != 0)) {
            shown++;
            psdl_log("[classic] report %u:", (unsigned) report_len);       // (the first 40 changes: a new controller's)
            for (uint16_t i = 0; i < report_len && i < 20; i++) psdl_log(" %02x", report[i]);
            psdl_log(" (%lu ms)\n", (unsigned long) btstack_run_loop_get_time_ms());
            last_len = report_len;
            memcpy(last, report, report_len < sizeof(last) ? report_len : sizeof(last));
        }
    }

    // Reports arrive with the HID transaction header still attached; 0xa1 is
    // HANDSHAKE/DATA with an Input report payload behind it.
    if (report_len < 1) return;
    if (report[0] != 0xa1) return;
    report++;
    report_len--;

    const uint8_t *descriptor;
    uint16_t descriptor_len;

    if (late_map_len) {
        descriptor = late_map;
        descriptor_len = late_map_len;
    } else if (boot_mode) {
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

    mouse_report_t mouse;
    if (hid_report_to_mouse(descriptor, descriptor_len, report, report_len, &mouse)) {
        mouse_decode_report(&mouse);
    }

    // a game controller's fields, and (a controller in its keyboard mode) its keys
    if (bt_app_looking_for() == BT_DEVICE_GAMEPAD) {
        gamepad_report_t pad;
        if (hid_report_to_gamepad(descriptor, descriptor_len, report, report_len, &pad)) {
            gamepad_decode_report(&pad);
        }
        if (media.describes_keyboard) gamepad_decode_keys(&kbd);
    }
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

    psdl_log("[classic] HID ready (%s protocol mode) (%lu ms)\n", boot_mode ? "boot" : "report",
             (unsigned long) btstack_run_loop_get_time_ms());
    if (bt_app_looking_for() == BT_DEVICE_GAMEPAD && !boot_mode) {
        const uint8_t *d = hid_descriptor_storage_get_descriptor_data(hid_cid);
        uint16_t n = hid_descriptor_storage_get_descriptor_len(hid_cid);
        psdl_log("[classic] its descriptor, %u bytes:", (unsigned) n);         // (what its reports hold)
        for (uint16_t i = 0; i < n; i++) psdl_log("%s%02x", i % 32 ? " " : "\n  ", d[i]);
        psdl_log("\n");
    }
    bt_app_link_up(BT_LINK_CLASSIC);
}

// ---------------------------------------------------------------- events --

// A remembered controller reached just after our reset can fail hid_host's query
// for its report map (status 0x11, the link fine): hid_host falls back to boot
// mode, and the controller goes on sending reports only its map can read. So a
// controller's map is asked for once more, over the link that's up.
static void late_query_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    UNUSED(packet_type);
    UNUSED(channel);
    UNUSED(size);

    switch (hci_event_packet_get_type(packet)) {
        case SDP_EVENT_QUERY_ATTRIBUTE_VALUE: {
            if (sdp_event_query_attribute_byte_get_attribute_id(packet) != BLUETOOTH_ATTRIBUTE_HID_DESCRIPTOR_LIST) break;
            uint16_t at = sdp_event_query_attribute_byte_get_data_offset(packet);
            if (at < sizeof(late_attribute)) {
                late_attribute[at] = sdp_event_query_attribute_byte_get_data(packet);
                late_attribute_len = at + 1;
            }
            break;
        }
        case SDP_EVENT_QUERY_COMPLETE: {
            uint8_t status = sdp_event_query_complete_get_status(packet);
            des_iterator_t list, item;
            if (status == ERROR_CODE_SUCCESS && hid_cid && late_attribute_len) {
                // a list of (a type, the map)
                for (des_iterator_init(&list, late_attribute); des_iterator_has_more(&list); des_iterator_next(&list)) {
                    if (des_iterator_get_type(&list) != DE_DES) continue;
                    for (des_iterator_init(&item, des_iterator_get_element(&list)); des_iterator_has_more(&item);
                         des_iterator_next(&item)) {
                        if (des_iterator_get_type(&item) != DE_STRING || late_map_len) continue;
                        uint8_t *string = des_iterator_get_element(&item);
                        late_map = string + de_get_header_size(string);
                        late_map_len = (uint16_t) de_get_data_size(string);
                    }
                }
            }
            psdl_log("[classic] its report map asked for again: status 0x%02x, %u bytes (%lu ms)\n", status,
                     (unsigned) late_map_len, (unsigned long) btstack_run_loop_get_time_ms());
            if (late_map_len) {
                boot_mode = false;
                hid_host_send_set_protocol_mode(hid_cid, HID_PROTOCOL_MODE_REPORT);
            }
            break;
        }
        default:
            break;
    }
}

static void start_late_query(void *context) {
    UNUSED(context);
    if (hid_cid == 0) return;
    sdp_client_query_uuid16(&late_query_handler, remote_addr, BLUETOOTH_SERVICE_CLASS_HUMAN_INTERFACE_DEVICE_SERVICE);
}

static void ask_for_report_map(void) {
    late_query.callback = &start_late_query;
    sdp_client_register_query_callback(&late_query);
}

static void handle_hid_event(uint8_t *packet) {
    uint8_t status;

    switch (hci_event_hid_meta_get_subevent_code(packet)) {
        case HID_SUBEVENT_INCOMING_CONNECTION:
            // A bonded keyboard reconnecting to us after waking up.
            psdl_log("[classic] it connects to us\n");
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
            hid_subevent_connection_opened_get_bd_addr(packet, remote_addr);
            descriptor_available = false;
            link_reported = false;
            late_attribute_len = 0;
            late_map_len = 0;
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
                if (bt_app_looking_for() == BT_DEVICE_GAMEPAD) ask_for_report_map();
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
            late_map_len = 0;
            descriptor_available = false;
            link_reported = false;
            bt_app_link_down(BT_LINK_CLASSIC, "device disconnected");
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
            gap_event_inquiry_result_get_bd_addr(packet, addr);
            if (bt_app_looking_for() == BT_DEVICE_GAMEPAD) {
                // (what's about, while a controller's looked for: what a new one calls itself)
                psdl_log("[classic] seen %s, class %06" PRIx32 "%s\n", bd_addr_to_str(addr), cod,
                         cod_is_keyboard(cod) ? ": a peripheral, taken" : "");
            }
            if (!cod_is_keyboard(cod)) break;

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

        case HCI_EVENT_DISCONNECTION_COMPLETE:
            psdl_log("[classic] disconnected, reason 0x%02x (%lu ms)\n",
                     hci_event_disconnection_complete_get_reason(packet), (unsigned long) btstack_run_loop_get_time_ms());
            break;

        default:
            break;
    }
}
