/*
 * Bluetooth LE transport: HID over GATT ("HOGP").
 *
 * Most keyboards sold in the last decade are LE. The flow is scan -> connect ->
 * pair (LE Secure Connections, keyboards want passkey entry) -> hand the link
 * to BTstack's hids_client, which does service discovery, fetches the report
 * map and subscribes to input report notifications.
 *
 * hids_client is used in preference to talking to the Boot Keyboard Input
 * Report characteristic directly: it negotiates Report protocol mode with an
 * automatic fallback to Boot mode, so keyboards that expose only one of the two
 * both work, and Report mode keeps the media/function keys that Boot mode drops.
 */
#include "../psdl_pico_log.h"
#include "bt_app.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "debug.h"
#include "hid_report.h"
#include "kbd_decode.h"

// Report map storage. Plain keyboards sit well under 200 bytes, but anything
// with media keys, a vendor page or several HID service instances runs much
// larger. hids_client silently truncates when this fills up -- it records the
// overflow internally and still reports the service as connected -- so an
// undersized buffer shows up as a keyboard that connects and then does nothing.
// RAM is not scarce here; the headroom is cheap insurance.
#define HID_DESCRIPTOR_STORAGE_SIZE 2048
static uint8_t hid_descriptor_storage[HID_DESCRIPTOR_STORAGE_SIZE];

static enum {
    LE_IDLE,
    LE_SCANNING,
    LE_CONNECTING,
    LE_PAIRING,
    LE_DISCOVERING,
    LE_ENABLING_NOTIFICATIONS,
    LE_READY,
} le_state = LE_IDLE;

static btstack_packet_callback_registration_t hci_callback_registration;
static btstack_packet_callback_registration_t sm_callback_registration;

static hci_con_handle_t connection_handle = HCI_CON_HANDLE_INVALID;
static uint16_t hids_cid;

// The mode we asked hids_client for. Do NOT use the protocol_mode field of
// GATTSERVICE_SUBEVENT_HID_SERVICE_CONNECTED instead: when the requested mode is
// not handled, hids_client stores the request verbatim as the negotiated mode
// (hids_client.c:939), so the event reports a value that was never negotiated.
static hid_protocol_mode_t active_protocol_mode = HID_PROTOCOL_MODE_REPORT;
static hid_led_layout_t led_layout;

static bd_addr_t      target_addr;
static bd_addr_type_t target_addr_type;

static btstack_timer_source_t connect_timer;

// Post-connect writes, run one at a time. hids_client accepts a new request only
// while it is idle (state CONNECTED), so these are stepped through on a timer
// and retried rather than fired back to back.
static btstack_timer_source_t post_connect_timer;
static uint8_t post_connect_step;
static uint8_t post_connect_attempts;
#define POST_CONNECT_STEPS       2
#define POST_CONNECT_MAX_ATTEMPTS 10
#define POST_CONNECT_INTERVAL_MS 250


// Appearance values that mean "this is a keyboard".
#define APPEARANCE_GENERIC_HID 0x03c0
#define APPEARANCE_KEYBOARD    0x03c1

static void le_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
static void post_connect_run(btstack_timer_source_t *ts);
static void sm_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
static void hids_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
static void le_ready(void);
static bool le_connect_hids(hid_protocol_mode_t mode);

// --------------------------------------------------------- advertisements --

// Pull the local name out of an advertisement, if it carries one.
static void ad_extract_name(const uint8_t *ad_data, uint8_t ad_len, char *out, size_t out_size) {
    out[0] = 0;

    ad_context_t context;
    for (ad_iterator_init(&context, ad_len, ad_data);
         ad_iterator_has_more(&context);
         ad_iterator_next(&context)) {

        uint8_t type = ad_iterator_get_data_type(&context);
        if (type != BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME &&
            type != BLUETOOTH_DATA_TYPE_SHORTENED_LOCAL_NAME) {
            continue;
        }

        uint8_t len = ad_iterator_get_data_len(&context);
        if (len >= out_size) len = (uint8_t) (out_size - 1);
        memcpy(out, ad_iterator_get_data(&context), len);
        out[len] = 0;
        return;
    }
}

// Does this advertisement look like a keyboard? The HID service UUID is the
// reliable signal; appearance catches devices that only list it there.
static bool ad_looks_like_keyboard(const uint8_t *ad_data, uint8_t ad_len) {
    if (ad_data_contains_uuid16(ad_len, ad_data, ORG_BLUETOOTH_SERVICE_HUMAN_INTERFACE_DEVICE)) {
        return true;
    }

    ad_context_t context;
    for (ad_iterator_init(&context, ad_len, ad_data);
         ad_iterator_has_more(&context);
         ad_iterator_next(&context)) {

        if (ad_iterator_get_data_type(&context) != BLUETOOTH_DATA_TYPE_APPEARANCE) continue;
        if (ad_iterator_get_data_len(&context) < 2) continue;

        uint16_t appearance = little_endian_read_16(ad_iterator_get_data(&context), 0);
        if (appearance == APPEARANCE_KEYBOARD || appearance == APPEARANCE_GENERIC_HID) {
            return true;
        }
    }

    return false;
}

// -------------------------------------------------------------------- LEDs --

// Push the current lock-LED state back to the keyboard. Called by bt_app.c
// when kbd_decode reports a lock key toggling.
void bt_le_set_leds(uint8_t led_mask) {
    if (le_state != LE_READY) return;

    uint8_t report[8];
    uint8_t report_id;
    uint8_t report_len;

    if (active_protocol_mode == HID_PROTOCOL_MODE_BOOT) {
        // Boot keyboard output report is a single byte of LED bits, in the same
        // Num/Caps/Scroll order that kbd_decode uses. It must be addressed by
        // HID_BOOT_MODE_KEYBOARD_ID: that is the id hids_client files the boot
        // output report characteristic under (hids_client.c:1045), and a lookup
        // for report id 0 simply finds nothing.
        report_id = HID_BOOT_MODE_KEYBOARD_ID;
        report[0] = led_mask & 0x07;
        report_len = 1;
    } else {
        report_len = hid_led_build_report(&led_layout, led_mask, report, sizeof(report));
        if (report_len == 0) return;
        report_id = (uint8_t) led_layout.report_id;
    }

    hids_client_send_write_report(hids_cid, report_id, HID_REPORT_TYPE_OUTPUT, report, report_len);
}

// --------------------------------------------------------------- lifecycle --

static void le_fail(const char *reason) {
    btstack_run_loop_remove_timer(&post_connect_timer);
    le_state = LE_IDLE;
    if (connection_handle != HCI_CON_HANDLE_INVALID) {
        gap_disconnect(connection_handle);
        connection_handle = HCI_CON_HANDLE_INVALID;
    }
    bt_app_link_down(BT_LINK_LE, reason);
}

// Belt and braces: hids_client emits the notification-configuration event on
// both of its paths, but a stall here would otherwise be invisible -- the link
// would sit in LE_ENABLING_NOTIFICATIONS forever with nothing on the console.
static void le_notify_timeout(btstack_timer_source_t *ts) {
    UNUSED(ts);
    if (le_state != LE_ENABLING_NOTIFICATIONS) return;
    psdl_log("[le] no reply to the notification subscription, continuing anyway\n");
    le_ready();
}

static void le_connect_timeout(btstack_timer_source_t *ts) {
    UNUSED(ts);
    if (le_state != LE_CONNECTING) return;
    gap_connect_cancel();
    le_state = LE_IDLE;
    bt_app_link_down(BT_LINK_LE, "connection timed out");
}

void bt_le_init(void) {
    sm_set_io_capabilities(IO_CAPABILITY_DISPLAY_ONLY);
    sm_set_authentication_requirements(SM_AUTHREQ_SECURE_CONNECTION | SM_AUTHREQ_BONDING);

    gatt_client_init();
    hids_client_init(hid_descriptor_storage, HID_DESCRIPTOR_STORAGE_SIZE);

    hci_callback_registration.callback = &le_packet_handler;
    hci_add_event_handler(&hci_callback_registration);

    sm_callback_registration.callback = &sm_packet_handler;
    sm_add_event_handler(&sm_callback_registration);
}

void bt_le_start_search(void) {
    le_state = LE_SCANNING;
    // Passive scan, window == interval so we listen continuously.
    gap_set_scan_parameters(0, 48, 48);
    gap_start_scan();
}

void bt_le_stop_search(void) {
    if (le_state != LE_SCANNING) return;
    gap_stop_scan();
    le_state = LE_IDLE;
}

void bt_le_connect(const bd_addr_t addr, bd_addr_type_t addr_type) {
    memcpy(target_addr, addr, sizeof(bd_addr_t));
    target_addr_type = addr_type;

    le_state = LE_CONNECTING;
    btstack_run_loop_set_timer(&connect_timer, 10000);
    btstack_run_loop_set_timer_handler(&connect_timer, &le_connect_timeout);
    btstack_run_loop_add_timer(&connect_timer);

    uint8_t status = gap_connect(target_addr, target_addr_type);
    if (status != ERROR_CODE_SUCCESS) {
        btstack_run_loop_remove_timer(&connect_timer);
        le_state = LE_IDLE;
        bt_app_link_down(BT_LINK_LE, "gap_connect refused");
    }
}

void bt_le_disconnect(void) {
    if (connection_handle == HCI_CON_HANDLE_INVALID) return;
    gap_disconnect(connection_handle);
}

// ----------------------------------------------------------- HCI/GAP events --

static void le_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    UNUSED(channel);
    UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET) return;

    switch (hci_event_packet_get_type(packet)) {
        case GAP_EVENT_ADVERTISING_REPORT: {
            if (le_state != LE_SCANNING) break;

            const uint8_t *ad_data = gap_event_advertising_report_get_data(packet);
            uint8_t ad_len = gap_event_advertising_report_get_data_length(packet);
            if (!ad_looks_like_keyboard(ad_data, ad_len)) break;

            bd_addr_t addr;
            gap_event_advertising_report_get_address(packet, addr);
            bd_addr_type_t addr_type = gap_event_advertising_report_get_address_type(packet);

            char name[32];
            ad_extract_name(ad_data, ad_len, name, sizeof(name));

            bt_app_device_found(BT_LINK_LE, addr, addr_type, name);
            break;
        }

        case HCI_EVENT_META_GAP:
            if (hci_event_gap_meta_get_subevent_code(packet) != GAP_SUBEVENT_LE_CONNECTION_COMPLETE) break;
            if (le_state != LE_CONNECTING) break;

            btstack_run_loop_remove_timer(&connect_timer);

            if (gap_subevent_le_connection_complete_get_status(packet) != ERROR_CODE_SUCCESS) {
                le_state = LE_IDLE;
                bt_app_link_down(BT_LINK_LE, "connection failed");
                break;
            }

            connection_handle = gap_subevent_le_connection_complete_get_connection_handle(packet);
            le_state = LE_PAIRING;
            psdl_log("[le] connected, requesting pairing\n");
            sm_request_pairing(connection_handle);
            break;

        case HCI_EVENT_DISCONNECTION_COMPLETE: {
            if (connection_handle == HCI_CON_HANDLE_INVALID) break;
            if (hci_event_disconnection_complete_get_connection_handle(packet) != connection_handle) break;

            btstack_run_loop_remove_timer(&post_connect_timer);
            connection_handle = HCI_CON_HANDLE_INVALID;
            hids_cid = 0;
            bool was_ready = (le_state == LE_READY);
            le_state = LE_IDLE;
            bt_app_link_down(BT_LINK_LE, was_ready ? "keyboard disconnected" : "link lost during setup");
            break;
        }

        default:
            break;
    }
}

// ------------------------------------------------------- Security Manager --

static void sm_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    UNUSED(channel);
    UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET) return;

    bool secured = false;

    switch (hci_event_packet_get_type(packet)) {
        case SM_EVENT_IDENTITY_CREATED: {
            bd_addr_t identity_addr;
            sm_event_identity_created_get_identity_address(packet, identity_addr);
            bt_app_note_identity_address(
                BT_LINK_LE, identity_addr,
                (bd_addr_type_t) sm_event_identity_created_get_identity_addr_type(packet));
            break;
        }

        case SM_EVENT_IDENTITY_RESOLVING_SUCCEEDED: {
            bd_addr_t identity_addr;
            sm_event_identity_resolving_succeeded_get_identity_address(packet, identity_addr);
            bt_app_note_identity_address(
                BT_LINK_LE, identity_addr,
                (bd_addr_type_t) sm_event_identity_resolving_succeeded_get_identity_addr_type(packet));
            break;
        }

        case SM_EVENT_JUST_WORKS_REQUEST:
            sm_just_works_confirm(sm_event_just_works_request_get_handle(packet));
            break;

        case SM_EVENT_NUMERIC_COMPARISON_REQUEST:
            psdl_log("[le] numeric comparison %06" PRIu32 " - accepting\n",
                   sm_event_numeric_comparison_request_get_passkey(packet));
            sm_numeric_comparison_confirm(sm_event_numeric_comparison_request_get_handle(packet));
            break;

        case SM_EVENT_PASSKEY_DISPLAY_NUMBER: {
            // The keyboard is the input device: the user types this on it.
            char digits[7];
            snprintf(digits, sizeof(digits), "%06" PRIu32,
                     sm_event_passkey_display_number_get_passkey(packet));
            psdl_log("\n>>> Type %s on the keyboard, then press Enter <<<\n", digits);
            bt_app_show_passkey(digits);
            break;
        }

        case SM_EVENT_PAIRING_COMPLETE:
            switch (sm_event_pairing_complete_get_status(packet)) {
                case ERROR_CODE_SUCCESS:
                    psdl_log("[le] paired\n");
                    secured = true;
                    break;
                case ERROR_CODE_AUTHENTICATION_FAILURE:
                    psdl_log("[le] pairing failed, reason %u\n",
                           sm_event_pairing_complete_get_reason(packet));
                    le_fail("pairing failed");
                    break;
                default:
                    psdl_log("[le] pairing failed, status 0x%02x\n",
                           sm_event_pairing_complete_get_status(packet));
                    le_fail("pairing failed");
                    break;
            }
            break;

        case SM_EVENT_REENCRYPTION_COMPLETE:
            if (sm_event_reencryption_complete_get_status(packet) == ERROR_CODE_SUCCESS) {
                psdl_log("[le] re-used stored bonding\n");
                secured = true;
            } else {
                // The keyboard forgot us -- most often it was re-paired to
                // another host. Drop our copy so the next attempt pairs fresh.
                psdl_log("[le] stored bonding rejected by keyboard\n");
                bt_app_forget_pairing();
                le_fail("bonding no longer valid");
            }
            break;

        default:
            break;
    }

    if (!secured) return;
    if (le_state != LE_PAIRING) return;

    le_state = LE_DISCOVERING;
    psdl_log("[le] discovering HID service...\n");
    if (!le_connect_hids(HID_PROTOCOL_MODE_REPORT)) {
        le_fail("HID service unavailable");
    }
}

// ------------------------------------------------------------ HID reports --

static void le_handle_report(uint8_t service_index, const uint8_t *report, uint16_t report_len) {
    const uint8_t *descriptor;
    uint16_t descriptor_len;

    // hids_client prepends the Report ID to the notification payload, which is
    // what BTstack's parser expects to find at report[0].
    dbg("[le] report svc=%u id=%u", service_index, report_len > 0 ? report[0] : 0);
    dbg_bytes("", report, report_len);

    if (active_protocol_mode == HID_PROTOCOL_MODE_BOOT) {
        descriptor = btstack_hid_get_boot_descriptor_data();
        descriptor_len = btstack_hid_get_boot_descriptor_len();
    } else {
        descriptor = hids_client_descriptor_storage_get_descriptor_data(hids_cid, service_index);
        descriptor_len = hids_client_descriptor_storage_get_descriptor_len(hids_cid, service_index);
    }

    if (descriptor_len == 0) {
        psdl_log("[le] cannot decode: no report map for service index %u\n", service_index);
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

// Ask hids_client for a specific protocol mode.
//
// HID_PROTOCOL_MODE_REPORT_WITH_FALLBACK_TO_BOOT must not be used here. Despite
// being a hid_protocol_mode_t, it is only meaningful to the Classic hid_host API
// -- btstack_hid.h says so, and hids_client's mode switch (hids_client.c:1270)
// has no case for it, so it lands in the default branch, silently selects Boot
// mode and never fetches the report map. The guarding btstack_assert is compiled
// out by -DNDEBUG in any non-Debug build, so it fails silently. Do the fallback
// here instead: try Report mode, drop to Boot if the device cannot do it.
static bool le_connect_hids(hid_protocol_mode_t mode) {
    active_protocol_mode = mode;

    uint8_t status = hids_client_connect(connection_handle, &hids_packet_handler, mode, &hids_cid);
    if (status != ERROR_CODE_SUCCESS) {
        psdl_log("[le] hids_client_connect(%s) failed, status 0x%02x\n",
               mode == HID_PROTOCOL_MODE_BOOT ? "boot" : "report", status);
        return false;
    }

    dbg("[le] requesting %s protocol mode\n",
        mode == HID_PROTOCOL_MODE_BOOT ? "boot" : "report");
    return true;
}

static void post_connect_run(btstack_timer_source_t *ts) {
    UNUSED(ts);
    if (le_state != LE_READY) return;
    if (post_connect_step >= POST_CONNECT_STEPS) return;

    uint8_t status;
    const char *what;

    switch (post_connect_step) {
        case 0:
            what = "set protocol mode = report";
            status = hids_client_send_set_protocol_mode(hids_cid, 0, active_protocol_mode);
            break;
        default:
            // Harmless when the keyboard is already awake, but some devices stay
            // quiet until the host says the HID session is active.
            what = "exit suspend";
            status = hids_client_send_exit_suspend(hids_cid, 0);
            break;
    }

    if (status == ERROR_CODE_SUCCESS) {
        dbg("[le] %s: ok\n", what);
        post_connect_step++;
        post_connect_attempts = 0;
    } else {
        // ERROR_CODE_COMMAND_DISALLOWED just means the client is still busy.
        post_connect_attempts++;
        dbg("[le] %s: status 0x%02x (attempt %u)\n", what, status, post_connect_attempts);
        if (post_connect_attempts >= POST_CONNECT_MAX_ATTEMPTS) {
            psdl_log("[le] gave up on '%s'\n", what);
            post_connect_step++;
            post_connect_attempts = 0;
        }
    }

    if (post_connect_step < POST_CONNECT_STEPS) {
        btstack_run_loop_set_timer(&post_connect_timer, POST_CONNECT_INTERVAL_MS);
        btstack_run_loop_add_timer(&post_connect_timer);
    }
}

static void le_ready(void) {
    btstack_run_loop_remove_timer(&connect_timer);
    le_state = LE_READY;

    // Protocol Mode is meant to reset to Report on every connection, but it is
    // a writable characteristic and devices do persist it -- and any host that
    // previously put this keyboard into Boot mode left it there. In Boot mode it
    // reports on the boot characteristics, which are not the ones subscribed to
    // in Report mode, so it would connect perfectly and then appear silent.
    // Write the mode we actually want rather than assuming the default.
    post_connect_step = 0;
    post_connect_attempts = 0;
    btstack_run_loop_set_timer(&post_connect_timer, POST_CONNECT_INTERVAL_MS);
    btstack_run_loop_set_timer_handler(&post_connect_timer, &post_connect_run);
    btstack_run_loop_add_timer(&post_connect_timer);
    psdl_log("[le] HID ready (%s protocol mode)\n",
           active_protocol_mode == HID_PROTOCOL_MODE_BOOT ? "boot" : "report");
    bt_app_link_up(BT_LINK_LE);
}

static void hids_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    UNUSED(channel);
    UNUSED(size);

    // packet_type is deliberately ignored. hids_client is inconsistent about it:
    // most events are emitted with HCI_EVENT_PACKET, but the two report paths
    // (hids_client.c:638 and :672) pass HCI_EVENT_GATTSERVICE_META as the
    // packet_type argument instead. Filtering on HCI_EVENT_PACKET therefore
    // discards every HID report while letting connect/disconnect through -- a
    // keyboard that connects cleanly and then appears silent. The event code in
    // packet[0] is the reliable discriminator, so test only that.
    UNUSED(packet_type);

    if (size < 3) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_GATTSERVICE_META) return;

    switch (hci_event_gattservice_meta_get_subevent_code(packet)) {
        case GATTSERVICE_SUBEVENT_HID_SERVICE_CONNECTED: {
            uint8_t status = gattservice_subevent_hid_service_connected_get_status(packet);
            if (status != ERROR_CODE_SUCCESS) {
                // A device with no report map cannot do Report mode; Boot mode
                // needs no descriptor, so it is worth one more try.
                if (active_protocol_mode == HID_PROTOCOL_MODE_REPORT) {
                    psdl_log("[le] report mode unavailable (status 0x%02x), trying boot mode\n",
                           status);
                    if (le_connect_hids(HID_PROTOCOL_MODE_BOOT)) break;
                }
                psdl_log("[le] HID service connect failed, status 0x%02x\n", status);
                le_fail("HID service connect failed");
                break;
            }

            dbg("[le] event reported protocol mode %u (using requested mode %u)\n",
                gattservice_subevent_hid_service_connected_get_protocol_mode(packet),
                (unsigned) active_protocol_mode);

            uint8_t num_instances =
                gattservice_subevent_hid_service_connected_get_num_instances(packet);
            dbg("[le] %u HID service instance(s), protocol mode %u\n",
                num_instances, (unsigned) active_protocol_mode);

            for (uint8_t i = 0; i < num_instances; i++) {
                uint16_t dlen = hids_client_descriptor_storage_get_descriptor_len(hids_cid, i);
                dbg("[le] service %u report map: %u bytes\n", i, dlen);

                if (dlen == 0) {
                    psdl_log("[le] WARNING: service %u has an empty report map; "
                           "falling back to the boot keyboard layout\n", i);
                    // Better than decoding nothing: boot reports have a fixed
                    // layout, so a keyboard sending them still works.
                    active_protocol_mode = HID_PROTOCOL_MODE_BOOT;
                } else if (dlen >= HID_DESCRIPTOR_STORAGE_SIZE) {
                    psdl_log("[le] WARNING: report map filled the %u byte buffer and was "
                           "probably truncated; raise HID_DESCRIPTOR_STORAGE_SIZE\n",
                           HID_DESCRIPTOR_STORAGE_SIZE);
                }
            }

            // Service index 0: a keyboard that exposes several HID service
            // instances puts the keyboard first in practice.
            hid_led_layout_find(&led_layout,
                                hids_client_descriptor_storage_get_descriptor_data(hids_cid, 0),
                                hids_client_descriptor_storage_get_descriptor_len(hids_cid, 0));

            // Subscribe to input report notifications explicitly. hids_client
            // only enables them along one of its two discovery paths: if a
            // device's reports are resolved without needing the Report ID/Type
            // read, it jumps straight to CONNECTED and reports success having
            // written no CCCDs at all. The result is a keyboard that pairs,
            // reports "ready", and then never sends anything. Asking again is
            // harmless when they are already on.
            uint8_t notify_status = hids_client_enable_notifications(hids_cid);
            if (notify_status == ERROR_CODE_SUCCESS) {
                le_state = LE_ENABLING_NOTIFICATIONS;
                dbg("[le] subscribing to input report notifications...\n");
                btstack_run_loop_set_timer(&connect_timer, 5000);
                btstack_run_loop_set_timer_handler(&connect_timer, &le_notify_timeout);
                btstack_run_loop_add_timer(&connect_timer);
                break;
            }

            psdl_log("[le] could not request notifications (status 0x%02x), "
                   "continuing anyway\n", notify_status);
            le_ready();
            break;
        }

        case GATTSERVICE_SUBEVENT_HID_SERVICE_REPORTS_NOTIFICATION:
            dbg("[le] notification configuration = 0x%04x\n",
                gattservice_subevent_hid_service_reports_notification_get_configuration(packet));
            if (le_state == LE_ENABLING_NOTIFICATIONS) {
                le_ready();
            }
            break;

        case GATTSERVICE_SUBEVENT_HID_SERVICE_DISCONNECTED:
            hids_cid = 0;
            if (le_state == LE_READY || le_state == LE_DISCOVERING ||
                le_state == LE_ENABLING_NOTIFICATIONS) {
                le_state = LE_IDLE;
                bt_app_link_down(BT_LINK_LE, "HID service disconnected");
            }
            break;

        case GATTSERVICE_SUBEVENT_HID_REPORT:
            le_handle_report(gattservice_subevent_hid_report_get_service_index(packet),
                             gattservice_subevent_hid_report_get_report(packet),
                             gattservice_subevent_hid_report_get_report_len(packet));
            break;

        default:
            dbg("[le] unhandled gattservice subevent 0x%02x\n",
                hci_event_gattservice_meta_get_subevent_code(packet));
            break;
    }
}

