/*
 * Interface between the connection manager (bt_app.c) and the two transports.
 *
 * A Bluetooth keyboard is either a Classic BR/EDR HID device or a Bluetooth LE
 * HID-over-GATT device, and you cannot tell which from the outside. bt_app.c
 * searches both, alternating, and hands the winner to the matching transport.
 */
#ifndef PICO_BT_KEYBOARD_BT_APP_H
#define PICO_BT_KEYBOARD_BT_APP_H

#include <stdbool.h>
#include <stdint.h>

#include "btstack.h"

typedef enum {
    BT_LINK_NONE = 0,
    BT_LINK_LE,
    BT_LINK_CLASSIC,
} bt_link_kind_t;

const char *bt_link_kind_name(bt_link_kind_t kind);

// -- called by the transports, implemented by bt_app.c ---------------------

// A plausible keyboard turned up during a search. First one wins.
void bt_app_device_found(bt_link_kind_t kind, const bd_addr_t addr,
                         bd_addr_type_t addr_type, const char *name);

// A search phase ran to completion without finding anything.
void bt_app_search_finished(bt_link_kind_t kind);

// The device we are connecting to turned out to have a different identity
// address than the one it advertised (LE privacy). Remember the identity one,
// since the advertised address rotates and will not be valid next time.
void bt_app_note_identity_address(bt_link_kind_t kind, const bd_addr_t addr,
                                  bd_addr_type_t addr_type);

// The keyboard is connected and delivering reports.
void bt_app_link_up(bt_link_kind_t kind);

// The link dropped, or an attempt to bring it up failed.
void bt_app_link_down(bt_link_kind_t kind, const char *reason);

// -- LE transport (bt_le.c) ------------------------------------------------

void bt_le_init(void);
void bt_le_start_search(void);
void bt_le_stop_search(void);
void bt_le_connect(const bd_addr_t addr, bd_addr_type_t addr_type);
void bt_le_disconnect(void);
void bt_le_set_leds(uint8_t led_mask);

// -- Classic transport (bt_classic.c) --------------------------------------

void bt_classic_init(void);
void bt_classic_start_search(void);
void bt_classic_stop_search(void);
void bt_classic_connect(const bd_addr_t addr);
void bt_classic_disconnect(void);
void bt_classic_set_leds(uint8_t led_mask);

// -- connection manager (bt_app.c) -----------------------------------------

// Bring up both transports and start looking. Call once, before HCI power on.
void bt_app_setup(void);

// Push Caps/Num/Scroll Lock state to the connected keyboard, routing to whichever
// transport owns the link. This is bt_app.c's own kbd_led_handler_t: a client
// that replaces the decoder's callbacks with kbd_decode_init() must pass this as
// the LED handler, or the lock-key LEDs stop being updated.
void bt_app_set_keyboard_leds(uint8_t led_mask);

// Console commands, wired up in main.c.
void bt_app_forget_pairing(void);
void bt_app_search_again(void);
void bt_app_print_status(void);

// -- for a screen (added in piegpu) -----------------------------------------

typedef enum {
    BT_APP_STARTING = 0,
    BT_APP_SEARCHING,       // kind: the transport being searched
    BT_APP_CONNECTING,      // name, address: the keyboard being reached for
    BT_APP_CONNECTED,
    BT_APP_WAITING,         // between tries to reach the remembered keyboard
} bt_app_phase_t;

typedef struct {
    bt_app_phase_t phase;
    bt_link_kind_t kind;
    char name[32];          // "" when it is not known (a remembered keyboard)
    char address[18];       // "" while searching
    char passkey[8];        // digits to type on the keyboard, "" when none are wanted
    bool remembered;        // a keyboard is stored for the next start
} bt_app_status_t;

// What the connection manager is doing. Call with the BTstack context locked.
void bt_app_get_status(bt_app_status_t *out);

// Called by the transports when pairing wants digits typed on the keyboard.
void bt_app_show_passkey(const char *digits);

// Is a keyboard connected right now? Distinct from "a keyboard has been seen":
// this is the link state, so it goes false the moment the link drops rather than
// waiting for the next keypress that does not arrive.
bool bt_app_keyboard_connected(void);

#endif // PICO_BT_KEYBOARD_BT_APP_H
