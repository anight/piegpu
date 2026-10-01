/*
 * Helpers for turning a device's HID descriptor plus a raw report into
 * something kbd_decode.h understands, and for building the Output report that
 * drives the lock LEDs. Shared by the Classic and LE transports.
 */
#ifndef PICO_BT_KEYBOARD_HID_REPORT_H
#define PICO_BT_KEYBOARD_HID_REPORT_H

#include <stdbool.h>
#include <stdint.h>

#include "kbd_decode.h"

// Media keys found in a report, already translated to their keyboard-page
// equivalents (0x7f Mute, 0x80 Volume Up, 0x81 Volume Down) so that everything
// downstream keeps treating a usage as a scancode.
//
// Kept apart from kbd_report_t on purpose. kbd_decode_report() treats its input
// as a complete snapshot and releases every key absent from it, but a keyboard
// usually puts its media keys in a *separate* report with its own Report ID. Merge
// the two and pressing Volume Up would release whatever was being held - which
// during a game means the character stops moving. The two `describes_` flags say
// which halves of the keyboard a given report actually spoke about, so neither
// half is clobbered by a report that said nothing about it.
#define KBD_MAX_MEDIA 4
typedef struct kbd_media_s {
    uint8_t usage[KBD_MAX_MEDIA];
    uint8_t count;
    bool    describes_keyboard;  // report carried Keyboard-page fields
    bool    describes_media;     // report carried Consumer-page fields
} kbd_media_t;

// Reduce a HID Input report to a modifier byte plus the usages currently held.
// Handles both descriptor styles: the usual 6-byte key array, and the bitmap
// ("N-key rollover") form where every key is its own one-bit field.
//
// `media` may be NULL if the caller does not care about media keys.
void hid_report_to_kbd(const uint8_t *descriptor, uint16_t descriptor_len,
                       const uint8_t *report, uint16_t report_len,
                       kbd_report_t *out, kbd_media_t *media);

// Where a keyboard keeps its lock LEDs inside its Output report.
typedef struct {
    bool     found;
    uint8_t  report_id;    // 0 when the descriptor declares no report IDs
    uint8_t  report_len;   // in bytes
    uint16_t bit[3];       // bit position of Num, Caps, Scroll Lock
    bool     has_bit[3];
} hid_led_layout_t;

// Scan an Output report descriptor for the three lock LEDs.
void hid_led_layout_find(hid_led_layout_t *layout,
                         const uint8_t *descriptor, uint16_t descriptor_len);

// Render led_mask (bit 0 Num, bit 1 Caps, bit 2 Scroll -- matching
// kbd_decode_led_mask) into an Output report. Returns the report length in
// bytes, or 0 if this keyboard exposes no LED report.
uint8_t hid_led_build_report(const hid_led_layout_t *layout, uint8_t led_mask,
                             uint8_t *out, uint8_t out_size);

#endif // PICO_BT_KEYBOARD_HID_REPORT_H
