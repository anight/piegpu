/*
 * Runtime-toggleable diagnostics.
 *
 * Verbose logging is on by default and can be switched off from the serial
 * console with 'd' -- once a keyboard is working the per-report output is far
 * too noisy to type through.
 */
#ifndef PICO_BT_KEYBOARD_DEBUG_H
#define PICO_BT_KEYBOARD_DEBUG_H

#include <stdbool.h>
#include <stdint.h>

extern bool pico_bt_keyboard_verbose;


// Printed only when verbose is on. Adds no newline of its own.
void dbg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Bytes on a single line, truncated with "..." past a handful. Deliberately
// never a multi-line block: these are printed on the same console the decoded
// keystrokes come out of, and a screen of hex buries them.
void dbg_bytes(const char *label, const uint8_t *data, uint16_t len);

#endif // PICO_BT_KEYBOARD_DEBUG_H
