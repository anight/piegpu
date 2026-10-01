/*
 * Connection manager.
 *
 * You cannot tell from the outside whether a keyboard is Classic BR/EDR or LE,
 * and the two use completely different discovery mechanisms (GAP inquiry vs.
 * advertisement scanning). So this alternates between them until something that
 * looks like a keyboard turns up, then hands off to the matching transport.
 *
 * The phases are run one at a time rather than concurrently. The controller can
 * technically do both at once, but interleaving an inquiry with an LE scan on
 * the CYW43's shared SPI bus costs duty cycle on both, and a keyboard in pairing
 * mode advertises for far longer than one search cycle -- so alternating finds
 * it just as reliably and is much easier to reason about.
 *
 * Once a keyboard is paired, its address and transport go in flash (BTstack's
 * TLV, the same store that holds the link keys), so a reboot reconnects
 * directly instead of searching again.
 *
 * From picosdl (backend/pico/bt). Added in piegpu: what a screen needs
 * (bt_app_get_status, bt_app_show_passkey), and the name it goes by.
 */
#include "../psdl_pico_log.h"
#include "bt_app.h"

#include <stdio.h>
#include <string.h>

#include "btstack_tlv.h"   // not pulled in by btstack.h

#include "kbd_host.h"    // generated from kbd_host.gatt at build time

#include "debug.h"
#include "hid_report.h"
#include "kbd_decode.h"

#define LE_SCAN_WINDOW_MS 6000

// An inquiry ends by itself and reports completion. This is only a backstop in
// case that event never arrives, so it sits comfortably past the real duration.
#define CLASSIC_INQUIRY_BACKSTOP_MS 9000

// Give up reconnecting to the remembered keyboard after this many tries and go
// back to searching -- the keyboard may well have been paired to something else.
#define MAX_RECONNECT_ATTEMPTS 3

#define TLV_TAG_KEYBOARD ((((uint32_t) 'P') << 24) | (((uint32_t) 'K') << 16) | \
                          (((uint32_t) 'B') << 8)  | ((uint32_t) '1'))

typedef struct {
    uint8_t   kind;        // bt_link_kind_t
    uint8_t   addr_type;   // bd_addr_type_t, only meaningful for LE
    bd_addr_t addr;
} stored_keyboard_t;

static enum {
    APP_BOOT,
    APP_SEARCH_LE,
    APP_SEARCH_CLASSIC,
    APP_CONNECTING,
    APP_CONNECTED,
    APP_RETRY_WAIT,
} app_state = APP_BOOT;

static btstack_packet_callback_registration_t hci_callback_registration;
static btstack_timer_source_t phase_timer;

static const btstack_tlv_t *tlv_impl;
static void                *tlv_context;

static bt_link_kind_t active_link = BT_LINK_NONE;

// The device the current connection attempt is aimed at. Only once the link is
// actually up (and, on LE, pairing has succeeded) is this worth storing.
static bt_link_kind_t pending_kind = BT_LINK_NONE;
static bd_addr_t      pending_addr;
static bd_addr_type_t pending_addr_type;

// For a screen (piegpu): the name the search reported, the digits pairing wants.
static char pending_name[32];
static char passkey[8];

static uint32_t key_events_seen;
static int  reconnect_attempts;
static bool have_stored_keyboard;
static stored_keyboard_t stored_keyboard;

static void start_le_search(void);
static void start_classic_search(void);

const char *bt_link_kind_name(bt_link_kind_t kind) {
    switch (kind) {
        case BT_LINK_LE:      return "LE";
        case BT_LINK_CLASSIC: return "Classic";
        default:              return "none";
    }
}

// ----------------------------------------------------------- key output --

static void on_key_event(const kbd_event_t *event) {
    key_events_seen++;

    // One line per transition: key down and key up each get their own, so every
    // scan code is readable next to what it means. Modifier keys are included
    // rather than folded into the keys they modify -- they are real transitions,
    // and reporting them only as a "mods=" suffix makes a Shift press with no
    // other key invisible.
    char line[80];
    kbd_event_format(event, line, sizeof(line));
    psdl_log("%s\n", line);
    fflush(stdout);
}

/*
 * Push the lock-key LED state to whichever transport owns the link.
 *
 * Not static, because a client that takes over the keyboard decoder's callbacks
 * has to be able to pass this one straight back - see bt_app.h.
 */
void bt_app_set_keyboard_leds(uint8_t led_mask) {
    switch (active_link) {
        case BT_LINK_LE:      bt_le_set_leds(led_mask);      break;
        case BT_LINK_CLASSIC: bt_classic_set_leds(led_mask); break;
        default: break;
    }
}

// -------------------------------------------------------- stored keyboard --

static void load_stored_keyboard(void) {
    have_stored_keyboard = false;
    if (tlv_impl == NULL) return;

    int len = tlv_impl->get_tag(tlv_context, TLV_TAG_KEYBOARD,
                                (uint8_t *) &stored_keyboard, sizeof(stored_keyboard));
    if (len != sizeof(stored_keyboard)) return;
    if (stored_keyboard.kind != BT_LINK_LE && stored_keyboard.kind != BT_LINK_CLASSIC) return;

    have_stored_keyboard = true;
}

static void save_stored_keyboard(bt_link_kind_t kind, const bd_addr_t addr, bd_addr_type_t addr_type) {
    stored_keyboard.kind = (uint8_t) kind;
    stored_keyboard.addr_type = (uint8_t) addr_type;
    memcpy(stored_keyboard.addr, addr, sizeof(bd_addr_t));
    have_stored_keyboard = true;

    if (tlv_impl == NULL) return;
    tlv_impl->store_tag(tlv_context, TLV_TAG_KEYBOARD,
                        (const uint8_t *) &stored_keyboard, sizeof(stored_keyboard));
}

void bt_app_forget_pairing(void) {
    have_stored_keyboard = false;
    reconnect_attempts = 0;
    if (tlv_impl != NULL) {
        tlv_impl->delete_tag(tlv_context, TLV_TAG_KEYBOARD);
    }
    // Drop the security material too, otherwise the keyboard stays bonded even
    // though we have forgotten which device it was. Classic link keys and the
    // LE device DB are separate stores.
    gap_delete_all_link_keys();
    if (stored_keyboard.kind == BT_LINK_LE) {
        gap_delete_bonding((bd_addr_type_t) stored_keyboard.addr_type, stored_keyboard.addr);
    }
    psdl_log("[app] forgot the paired keyboard\n");
}

// --------------------------------------------------------- search phases --

static void phase_timeout(btstack_timer_source_t *ts) {
    UNUSED(ts);

    switch (app_state) {
        case APP_SEARCH_LE:
            bt_le_stop_search();
            start_classic_search();
            break;
        case APP_SEARCH_CLASSIC:
            // Inquiry never reported completion; move on anyway.
            bt_classic_stop_search();
            start_le_search();
            break;
        case APP_RETRY_WAIT:
            if (have_stored_keyboard && reconnect_attempts < MAX_RECONNECT_ATTEMPTS) {
                reconnect_attempts++;
                psdl_log("[app] reconnecting to %s (attempt %d/%d)\n",
                       bd_addr_to_str(stored_keyboard.addr),
                       reconnect_attempts, MAX_RECONNECT_ATTEMPTS);
                app_state = APP_CONNECTING;
                pending_kind = (bt_link_kind_t) stored_keyboard.kind;
                memcpy(pending_addr, stored_keyboard.addr, sizeof(bd_addr_t));
                pending_addr_type = (bd_addr_type_t) stored_keyboard.addr_type;

                if (pending_kind == BT_LINK_LE) {
                    bt_le_connect(pending_addr, pending_addr_type);
                } else {
                    bt_classic_connect(pending_addr);
                }
            } else {
                start_le_search();
            }
            break;
        default:
            break;
    }
}

static void set_phase_timer(uint32_t ms) {
    btstack_run_loop_remove_timer(&phase_timer);
    btstack_run_loop_set_timer(&phase_timer, ms);
    btstack_run_loop_set_timer_handler(&phase_timer, &phase_timeout);
    btstack_run_loop_add_timer(&phase_timer);
}

// Both of these arm the phase timer before kicking off the search. A search
// that fails immediately reports completion synchronously, which advances to
// the next phase and arms its timer -- arming ours afterwards would clobber the
// new phase's timer with this phase's duration.

static void start_le_search(void) {
    app_state = APP_SEARCH_LE;
    psdl_log("[app] scanning for LE keyboards...\n");
    // LE scanning has no natural end, so we time-box it ourselves.
    set_phase_timer(LE_SCAN_WINDOW_MS);
    bt_le_start_search();
}

static void start_classic_search(void) {
    app_state = APP_SEARCH_CLASSIC;
    psdl_log("[app] scanning for Classic keyboards...\n");
    // The inquiry ends on its own and reports GAP_EVENT_INQUIRY_COMPLETE,
    // which comes back as bt_app_search_finished; this is only a backstop.
    set_phase_timer(CLASSIC_INQUIRY_BACKSTOP_MS);
    bt_classic_start_search();
}

static void retry_later(uint32_t ms) {
    app_state = APP_RETRY_WAIT;
    set_phase_timer(ms);
}

void bt_app_search_again(void) {
    btstack_run_loop_remove_timer(&phase_timer);
    bt_le_stop_search();
    bt_classic_stop_search();

    if (app_state == APP_CONNECTED) {
        if (active_link == BT_LINK_LE) bt_le_disconnect();
        else if (active_link == BT_LINK_CLASSIC) bt_classic_disconnect();
    }

    reconnect_attempts = MAX_RECONNECT_ATTEMPTS;   // skip straight to searching
    start_le_search();
}

// Called on HCI_STATE_WORKING and whenever we need to start over.
static void start_connecting(void) {
    load_stored_keyboard();
    reconnect_attempts = 0;

    if (have_stored_keyboard) {
        psdl_log("[app] remembered a %s keyboard at %s, connecting...\n",
               bt_link_kind_name((bt_link_kind_t) stored_keyboard.kind),
               bd_addr_to_str(stored_keyboard.addr));
        reconnect_attempts = 1;
        app_state = APP_CONNECTING;
        pending_kind = (bt_link_kind_t) stored_keyboard.kind;
        memcpy(pending_addr, stored_keyboard.addr, sizeof(bd_addr_t));
        pending_addr_type = (bd_addr_type_t) stored_keyboard.addr_type;

        if (pending_kind == BT_LINK_LE) {
            bt_le_connect(pending_addr, pending_addr_type);
        } else {
            bt_classic_connect(pending_addr);
        }
        return;
    }

    start_le_search();
}

// ------------------------------------------------- callbacks from transports --

void bt_app_device_found(bt_link_kind_t kind, const bd_addr_t addr,
                         bd_addr_type_t addr_type, const char *name) {
    if (app_state != APP_SEARCH_LE && app_state != APP_SEARCH_CLASSIC) return;

    btstack_run_loop_remove_timer(&phase_timer);
    bt_le_stop_search();
    bt_classic_stop_search();

    psdl_log("[app] found %s keyboard %s%s%s, connecting...\n",
           bt_link_kind_name(kind), bd_addr_to_str(addr),
           (name != NULL && name[0] != 0) ? " " : "",
           (name != NULL && name[0] != 0) ? name : "");

    app_state = APP_CONNECTING;
    pending_kind = kind;
    memcpy(pending_addr, addr, sizeof(bd_addr_t));
    pending_addr_type = addr_type;
    snprintf(pending_name, sizeof(pending_name), "%s", name != NULL ? name : "");

    if (kind == BT_LINK_LE) {
        bt_le_connect(addr, addr_type);
    } else {
        bt_classic_connect(addr);
    }
}

void bt_app_search_finished(bt_link_kind_t kind) {
    // Ignore the completion event that our own stop_search call produces.
    if (kind == BT_LINK_CLASSIC && app_state == APP_SEARCH_CLASSIC) {
        btstack_run_loop_remove_timer(&phase_timer);
        start_le_search();
    }
}

void bt_app_note_identity_address(bt_link_kind_t kind, const bd_addr_t addr,
                                  bd_addr_type_t addr_type) {
    if (pending_kind != kind) return;
    if (memcmp(pending_addr, addr, sizeof(bd_addr_t)) == 0) return;

    // Not every LE device has an identity to resolve to. A keyboard that
    // advertises a *static random* address rather than a resolvable private one
    // never rotates, so there is nothing to resolve -- and BTstack reports the
    // identity address as all zeros. Taking that at face value replaces a
    // perfectly good address with one that cannot exist, and every later
    // reconnect fails with "gap_connect refused" until the pairing is forgotten.
    //
    // Keep what we already have in that case: the advertised address is the
    // right one to reconnect to precisely because it does not rotate.
    static const bd_addr_t no_identity = { 0 };
    if (memcmp(addr, no_identity, sizeof(bd_addr_t)) == 0) {
        psdl_log("[app] %s reported no identity address; keeping %s\n",
               bt_link_kind_name(kind), bd_addr_to_str(pending_addr));
        return;
    }

    psdl_log("[app] %s resolves to identity address %s\n",
           bt_link_kind_name(kind), bd_addr_to_str((uint8_t *) addr));
    memcpy(pending_addr, addr, sizeof(bd_addr_t));
    pending_addr_type = addr_type;
}

void bt_app_link_up(bt_link_kind_t kind) {
    btstack_run_loop_remove_timer(&phase_timer);

    app_state = APP_CONNECTED;
    active_link = kind;
    reconnect_attempts = 0;
    key_events_seen = 0;
    passkey[0] = 0;
    kbd_decode_reset();

    if (pending_kind == kind) {
        save_stored_keyboard(kind, pending_addr, pending_addr_type);
    }

    // Our lock state starts clear; push it so the keyboard's LEDs agree.
    bt_app_set_keyboard_leds(kbd_decode_led_mask());

    psdl_log("\n=== keyboard connected over %s - start typing ===\n", bt_link_kind_name(kind));
}

void bt_app_link_down(bt_link_kind_t kind, const char *reason) {
    // Only meaningful while we are attached to, or reaching for, this link.
    // A stray event during a search phase must not restart the search.
    if (app_state != APP_CONNECTING && app_state != APP_CONNECTED) return;
    if (app_state == APP_CONNECTED && kind != active_link) return;

    psdl_log("\n[app] %s link down: %s\n", bt_link_kind_name(kind), reason);

    active_link = BT_LINK_NONE;
    passkey[0] = 0;
    kbd_decode_reset();

    // A drop from a working link is usually the keyboard idling out, so retry
    // the same device quickly before falling back to a full search.
    retry_later(1000);
}

bool bt_app_keyboard_connected(void) {
    return app_state == APP_CONNECTED;
}

void bt_app_show_passkey(const char *digits) {
    snprintf(passkey, sizeof(passkey), "%s", digits);
}

void bt_app_get_status(bt_app_status_t *out) {
    memset(out, 0, sizeof(*out));
    out->remembered = have_stored_keyboard;
    switch (app_state) {
        case APP_SEARCH_LE:      out->phase = BT_APP_SEARCHING; out->kind = BT_LINK_LE; return;
        case APP_SEARCH_CLASSIC: out->phase = BT_APP_SEARCHING; out->kind = BT_LINK_CLASSIC; return;
        case APP_CONNECTING:     out->phase = BT_APP_CONNECTING; out->kind = pending_kind; break;
        case APP_CONNECTED:      out->phase = BT_APP_CONNECTED; out->kind = active_link; break;
        case APP_RETRY_WAIT:     out->phase = BT_APP_WAITING; out->kind = pending_kind; break;
        default:                 out->phase = BT_APP_STARTING; return;
    }
    snprintf(out->name, sizeof(out->name), "%s", pending_name);
    snprintf(out->address, sizeof(out->address), "%s", bd_addr_to_str(pending_addr));
    snprintf(out->passkey, sizeof(out->passkey), "%s", passkey);
}

void bt_app_print_status(void) {
    psdl_log("\n[app] state: ");
    switch (app_state) {
        case APP_BOOT:           psdl_log("starting up"); break;
        case APP_SEARCH_LE:      psdl_log("scanning (LE)"); break;
        case APP_SEARCH_CLASSIC: psdl_log("scanning (Classic)"); break;
        case APP_CONNECTING:     psdl_log("connecting"); break;
        case APP_CONNECTED:      psdl_log("connected over %s", bt_link_kind_name(active_link)); break;
        case APP_RETRY_WAIT:     psdl_log("waiting to retry"); break;
    }
    psdl_log(", %lu key events decoded", (unsigned long) key_events_seen);
    if (have_stored_keyboard) {
        psdl_log(", paired with %s keyboard %s",
               bt_link_kind_name((bt_link_kind_t) stored_keyboard.kind),
               bd_addr_to_str(stored_keyboard.addr));
    } else {
        psdl_log(", no keyboard paired");
    }
    psdl_log("\n");
}

// ------------------------------------------------------------------ setup --

static void app_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    UNUSED(channel);
    UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != BTSTACK_EVENT_STATE) return;
    if (btstack_event_state_get_state(packet) != HCI_STATE_WORKING) return;
    if (app_state != APP_BOOT) return;

    bd_addr_t local_addr;
    gap_local_bd_addr(local_addr);
    psdl_log("[app] Bluetooth up on %s\n", bd_addr_to_str(local_addr));

    btstack_tlv_get_instance(&tlv_impl, &tlv_context);
    start_connecting();
}

void bt_app_setup(void) {
    l2cap_init();
    sm_init();
    att_server_init(profile_data, NULL, NULL);
    sdp_init();

    gap_set_local_name("piegpu");

    kbd_decode_init(&on_key_event, &bt_app_set_keyboard_leds);

    bt_le_init();
    bt_classic_init();

    hci_callback_registration.callback = &app_packet_handler;
    hci_add_event_handler(&hci_callback_registration);
}
