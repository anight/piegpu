#include "../psdl_pico_log.h"
#include "kbd_decode.h"
#include "hid_report.h"

#include <stdio.h>
#include <string.h>

// US layout, indexed by HID usage starting at KEY_FIRST. 0 means "not a
// character key" -- look it up in key_names[] instead.
#define KEY_FIRST 0x04
#define KEY_LAST  0x38

static const char key_plain[KEY_LAST - KEY_FIRST + 1] = {
    'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j',   // 04-0d
    'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't',   // 0e-17
    'u', 'v', 'w', 'x', 'y', 'z',                       // 18-1d
    '1', '2', '3', '4', '5', '6', '7', '8', '9', '0',   // 1e-27
    '\n', 0, '\b', '\t', ' ',                           // 28-2c (Esc maps to no character:
                                                        // emitting a raw 0x1b would be read
                                                        // as a terminal escape sequence)
    '-', '=', '[', ']', '\\', '\\', ';', '\'', '`',     // 2d-35 (32 is non-US #/~)
    ',', '.', '/',                                      // 36-38
};

static const char key_shift[KEY_LAST - KEY_FIRST + 1] = {
    'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J',
    'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T',
    'U', 'V', 'W', 'X', 'Y', 'Z',
    '!', '@', '#', '$', '%', '^', '&', '*', '(', ')',
    '\n', 0, '\b', '\t', ' ',                           // 28-2c
    '_', '+', '{', '}', '|', '|', ':', '"', '~',
    '<', '>', '?',
};

// Named keys that carry no character. Sparse ranges, so two small tables.
#define NAMED_FIRST 0x28
#define NAMED_LAST  0x53
static const char *const key_names[NAMED_LAST - NAMED_FIRST + 1] = {
    "Enter", "Esc", "Backspace", "Tab", NULL,           // 28-2c (space has a char)
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,     // 2d-34
    NULL, NULL, NULL, NULL,                             // 35-38
    "CapsLock",                                         // 39
    "F1", "F2", "F3", "F4", "F5", "F6",                 // 3a-3f
    "F7", "F8", "F9", "F10", "F11", "F12",              // 40-45
    "PrintScreen", "ScrollLock", "Pause",               // 46-48
    "Insert", "Home", "PageUp", "Delete", "End", "PageDown", // 49-4e
    "Right", "Left", "Down", "Up",                      // 4f-52
    "NumLock",                                          // 53
};

// Keypad, usages 0x54..0x63. With Num Lock on the digit keys type digits; with
// it off they act as navigation, which is what the second table covers.
#define KEYPAD_FIRST 0x54
#define KEYPAD_LAST  0x63
static const char keypad_chars[KEYPAD_LAST - KEYPAD_FIRST + 1] = {
    '/', '*', '-', '+', '\n',                           // 54-58
    '1', '2', '3', '4', '5', '6', '7', '8', '9', '0',   // 59-62
    '.',                                                // 63
};
static const char *const keypad_names[KEYPAD_LAST - KEYPAD_FIRST + 1] = {
    NULL, NULL, NULL, NULL, NULL,                       // 54-58 always characters
    "End", "Down", "PageDown", "Left", "Clear",         // 59-5d
    "Right", "Home", "Up", "PageUp", "Insert",          // 5e-62
    "Delete",                                           // 63
};

// Modifier keys, usages 0xe0..0xe7, in the same bit order as the modifier byte.
#define MOD_FIRST 0xe0
#define MOD_LAST  0xe7
static const char *const mod_names[MOD_LAST - MOD_FIRST + 1] = {
    "LeftCtrl", "LeftShift", "LeftAlt", "LeftGUI",
    "RightCtrl", "RightShift", "RightAlt", "RightGUI",
};

#define USAGE_CAPS_LOCK   0x39
#define USAGE_SCROLL_LOCK 0x47
#define USAGE_NUM_LOCK    0x53

#define LED_NUM_LOCK    0x01
#define LED_CAPS_LOCK   0x02
#define LED_SCROLL_LOCK 0x04

static kbd_event_handler_t event_handler;
static kbd_led_handler_t   led_handler;

static uint8_t held[KBD_MAX_KEYS];
/* Media keys keep their own held-set; see kbd_decode_media(). */
static uint8_t held_media[KBD_MAX_MEDIA];
static uint8_t held_modifiers;
static uint8_t led_mask;

void kbd_decode_init(kbd_event_handler_t on_event, kbd_led_handler_t on_leds) {
    event_handler = on_event;
    led_handler = on_leds;
    kbd_decode_reset();
}

void kbd_decode_reset(void) {
    memset(held, 0, sizeof(held));
    memset(held_media, 0, sizeof(held_media));
    held_modifiers = 0;
    led_mask = 0;
}

uint8_t kbd_decode_led_mask(void) {
    return led_mask;
}

void kbd_event_format(const kbd_event_t *event, char *out, size_t out_size) {
    // How the key reads: printable characters in quotes, named keys in
    // brackets, and a bare "?" for anything with no mapping -- an unrecognised
    // key still shows its scan code, which is the part worth knowing.
    char meaning[16];
    if (event->ch >= 0x20 && event->ch < 0x7f) {
        snprintf(meaning, sizeof(meaning), "'%c'", event->ch);
    } else if (event->name != NULL) {
        snprintf(meaning, sizeof(meaning), "[%s]", event->name);
    } else {
        // Keys that carry a control character rather than a printable one.
        // These have a character *and* a name, and the character wins in
        // describe(), so name them here instead of printing a bare hex byte.
        switch (event->ch) {
            case '\n': snprintf(meaning, sizeof(meaning), "[Enter]");     break;
            case '\t': snprintf(meaning, sizeof(meaning), "[Tab]");       break;
            case '\b': snprintf(meaning, sizeof(meaning), "[Backspace]"); break;
            case 0:     snprintf(meaning, sizeof(meaning), "?");           break;
            default:    snprintf(meaning, sizeof(meaning), "0x%02x", (uint8_t) event->ch); break;
        }
    }

    char mods[48];
    mods[0] = 0;
    if (event->modifiers != 0) {
        snprintf(mods, sizeof(mods), " mods=0x%02x%s%s%s%s", event->modifiers,
                 (event->modifiers & KBD_MOD_CTRL)  ? " ctrl"  : "",
                 (event->modifiers & KBD_MOD_SHIFT) ? " shift" : "",
                 (event->modifiers & KBD_MOD_ALT)   ? " alt"   : "",
                 (event->modifiers & KBD_MOD_GUI)   ? " gui"   : "");
    }

    // Pad the meaning column only when something follows it, so lines without
    // modifiers do not end in a run of spaces.
    snprintf(out, out_size, mods[0] != 0 ? "%-4s 0x%02x %-3u %-12s%s"
                                         : "%-4s 0x%02x %-3u %s%s",
             event->pressed ? "down" : "up", event->usage, event->usage,
             meaning, mods);
}

static bool is_held(const uint8_t *set, uint8_t usage) {
    for (int i = 0; i < KBD_MAX_KEYS; i++) {
        if (set[i] == usage) return true;
    }
    return false;
}

// Fill in ch/name for a usage given the current modifier and lock state.
static void describe(uint8_t usage, uint8_t modifiers, kbd_event_t *event) {
    event->ch = 0;
    event->name = NULL;

    bool shift = (modifiers & KBD_MOD_SHIFT) != 0;

    if (usage >= KEY_FIRST && usage <= KEY_LAST) {
        // Caps Lock inverts shift for letters only, not for the number row.
        bool letter = usage <= 0x1d;
        bool upper = shift ^ (letter && (led_mask & LED_CAPS_LOCK) != 0);
        event->ch = upper ? key_shift[usage - KEY_FIRST] : key_plain[usage - KEY_FIRST];
    } else if (usage >= KEYPAD_FIRST && usage <= KEYPAD_LAST) {
        const char *nav = keypad_names[usage - KEYPAD_FIRST];
        if (nav != NULL && (led_mask & LED_NUM_LOCK) == 0) {
            event->name = nav;
        } else {
            event->ch = keypad_chars[usage - KEYPAD_FIRST];
        }
    }

    if (event->ch == 0 && event->name == NULL &&
        usage >= NAMED_FIRST && usage <= NAMED_LAST) {
        event->name = key_names[usage - NAMED_FIRST];
    }

    if (event->ch == 0 && event->name == NULL &&
        usage >= MOD_FIRST && usage <= MOD_LAST) {
        event->name = mod_names[usage - MOD_FIRST];
    }
}

static void emit(uint8_t usage, uint8_t modifiers, bool pressed) {
    if (event_handler == NULL) return;
    kbd_event_t event = { .usage = usage, .modifiers = modifiers, .pressed = pressed };
    describe(usage, modifiers, &event);
    event_handler(&event);
}

// Toggle the lock LED for usage, if it is a lock key. Returns true if it was.
static bool apply_lock(uint8_t usage) {
    uint8_t bit;
    switch (usage) {
        case USAGE_CAPS_LOCK:   bit = LED_CAPS_LOCK;   break;
        case USAGE_NUM_LOCK:    bit = LED_NUM_LOCK;    break;
        case USAGE_SCROLL_LOCK: bit = LED_SCROLL_LOCK; break;
        default: return false;
    }
    led_mask ^= bit;
    if (led_handler != NULL) led_handler(led_mask);
    return true;
}

void kbd_decode_report(const kbd_report_t *report) {
    // Rollover: the keyboard is reporting more keys than it can distinguish.
    // Nothing in the key array is meaningful, so hold the previous state.
    for (int i = 0; i < KBD_MAX_KEYS; i++) {
        if (report->keys[i] == 0x01) return;
    }

    // Modifier keys never appear in the key array, so derive their events from
    // the modifier byte. Report them first: a press of Shift-A should read as
    // Shift down, then A.
    uint8_t mod_changed = held_modifiers ^ report->modifiers;
    for (int bit = 0; bit < 8; bit++) {
        if ((mod_changed & (1u << bit)) == 0) continue;
        bool pressed = (report->modifiers & (1u << bit)) != 0;
        emit((uint8_t) (MOD_FIRST + bit), report->modifiers, pressed);
    }

    // Releases first, so a chord that swaps keys reads in a sensible order.
    for (int i = 0; i < KBD_MAX_KEYS; i++) {
        uint8_t usage = held[i];
        if (usage == 0) continue;
        if (!is_held(report->keys, usage)) {
            emit(usage, report->modifiers, false);
        }
    }

    for (int i = 0; i < KBD_MAX_KEYS; i++) {
        uint8_t usage = report->keys[i];
        if (usage == 0) continue;
        if (is_held(held, usage)) continue;
        // Lock keys update state before the event so the event reflects it.
        apply_lock(usage);
        emit(usage, report->modifiers, true);
    }

    memcpy(held, report->keys, sizeof(held));
    held_modifiers = report->modifiers;
}

void kbd_decode_boot_report(const uint8_t *data, uint16_t len) {
    if (len < 3) return;

    kbd_report_t report;
    memset(&report, 0, sizeof(report));
    report.modifiers = data[0];
    // data[1] is reserved; usages start at data[2].
    uint16_t count = len - 2;
    if (count > KBD_MAX_KEYS) count = KBD_MAX_KEYS;
    memcpy(report.keys, &data[2], count);

    kbd_decode_report(&report);
}


/*
 * Media keys.
 *
 * Their own held-set, because they arrive in their own report: see kbd_media_t.
 * Only acted on when the report actually carried Consumer-page fields, so an
 * ordinary keyboard report - which says nothing about them - does not read as
 * "all media keys released".
 */
static bool media_is_held(const uint8_t *set, uint8_t usage) {
    for (int i = 0; i < KBD_MAX_MEDIA; i++)
        if (set[i] == usage) return true;
    return false;
}

void kbd_decode_media(const struct kbd_media_s *media_in) {
    const kbd_media_t *media = (const kbd_media_t *)media_in;
    if (media == NULL || !media->describes_media) return;

    for (int i = 0; i < KBD_MAX_MEDIA; i++) {
        uint8_t usage = held_media[i];
        if (usage != 0 && !media_is_held(media->usage, usage))
            emit(usage, held_modifiers, false);
    }
    for (int i = 0; i < media->count; i++) {
        uint8_t usage = media->usage[i];
        if (usage != 0 && !media_is_held(held_media, usage))
            emit(usage, held_modifiers, true);
    }

    memset(held_media, 0, sizeof(held_media));
    for (int i = 0; i < media->count && i < KBD_MAX_MEDIA; i++)
        held_media[i] = media->usage[i];
}
