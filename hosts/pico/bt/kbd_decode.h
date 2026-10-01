/*
 * Transport-independent HID keyboard decoding.
 *
 * Both the Classic (HID Report protocol, parsed out of the device's HID
 * descriptor) and the LE (HID-over-GATT Boot protocol, fixed 8-byte layout)
 * paths reduce their input to the same thing: a modifier byte plus the set of
 * key usages currently held down. Everything downstream of that -- edge
 * detection, shift/caps handling, US layout mapping -- lives here once.
 */
#ifndef PICO_BT_KEYBOARD_KBD_DECODE_H
#define PICO_BT_KEYBOARD_KBD_DECODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KBD_MAX_KEYS 6

// HID keyboard modifier bits (usage page 0x07, usages 0xE0..0xE7).
#define KBD_MOD_LEFT_CTRL   0x01
#define KBD_MOD_LEFT_SHIFT  0x02
#define KBD_MOD_LEFT_ALT    0x04
#define KBD_MOD_LEFT_GUI    0x08
#define KBD_MOD_RIGHT_CTRL  0x10
#define KBD_MOD_RIGHT_SHIFT 0x20
#define KBD_MOD_RIGHT_ALT   0x40
#define KBD_MOD_RIGHT_GUI   0x80

#define KBD_MOD_CTRL  (KBD_MOD_LEFT_CTRL  | KBD_MOD_RIGHT_CTRL)
#define KBD_MOD_SHIFT (KBD_MOD_LEFT_SHIFT | KBD_MOD_RIGHT_SHIFT)
#define KBD_MOD_ALT   (KBD_MOD_LEFT_ALT   | KBD_MOD_RIGHT_ALT)
#define KBD_MOD_GUI   (KBD_MOD_LEFT_GUI   | KBD_MOD_RIGHT_GUI)

// A snapshot of the keyboard: which modifiers are held, and which key usages.
// Unused slots are 0. Order is not significant.
typedef struct {
    uint8_t modifiers;
    uint8_t keys[KBD_MAX_KEYS];
} kbd_report_t;

// One decoded state change for a single key.
typedef struct {
    uint8_t usage;      // HID usage, page 0x07
    uint8_t modifiers;  // modifiers held at the time of the event
    bool    pressed;    // true on key down, false on key up
    char    ch;         // US-layout character, or 0 if the key has no character
    const char *name;   // human-readable name for non-character keys, else NULL
} kbd_event_t;

// Called for every key transition. Set via kbd_decode_init.
typedef void (*kbd_event_handler_t)(const kbd_event_t *event);

// Called when Caps/Num/Scroll Lock state changes, so the transport can push a
// new LED output report to the keyboard. Mask uses the HID LED page bit order:
// bit 0 Num Lock, bit 1 Caps Lock, bit 2 Scroll Lock.
typedef void (*kbd_led_handler_t)(uint8_t led_mask);

void kbd_decode_init(kbd_event_handler_t on_event, kbd_led_handler_t on_leds);

// Forget all held-key and lock state. Call on connect/disconnect so a key that
// was down when the link dropped does not suppress its next press.
void kbd_decode_reset(void);

// Feed the media keys of one report. Edge-detected against its own state, not
// against the key array, so it cannot disturb which ordinary keys are held - see
// kbd_media_t in hid_report.h for why that matters. Events come out through the
// same handler as any other key.
struct kbd_media_s;
void kbd_decode_media(const struct kbd_media_s *media);

// Feed a snapshot. Emits an event per key that changed since the last call.
void kbd_decode_report(const kbd_report_t *report);

// Render an event as one human-readable line, without a trailing newline:
//   "down 0x04 4   'a'"
//   "down 0x28 40  [Enter]"
//   "down 0x04 4   'A'          mods=0x02 shift"
void kbd_event_format(const kbd_event_t *event, char *out, size_t out_size);

// Current LED mask, for transports that need to resend it after a reconnect.
uint8_t kbd_decode_led_mask(void);

// Convenience: decode the 8-byte HID Boot keyboard report layout
// [modifiers, reserved, usage x6] straight into kbd_decode_report.
void kbd_decode_boot_report(const uint8_t *data, uint16_t len);

#endif // PICO_BT_KEYBOARD_KBD_DECODE_H
