/*
 * A game controller's state, for whoever wants it (added in piegpu, beside
 * mouse_decode.h): the transports hand every report to gamepad_decode_report
 * (its stick, hat and buttons) and gamepad_decode_keys (the keys held, for a
 * controller in its keyboard mode); a game reads the latest of both with
 * gamepad_decode_get, from its own loop (the copy is taken with the
 * interrupts held off: the reports come in the Bluetooth stack's context).
 */
#ifndef PICO_BT_GAMEPAD_DECODE_H
#define PICO_BT_GAMEPAD_DECODE_H

#include <stdbool.h>
#include <stdint.h>

#include "hid_report.h"

typedef struct {
    bool             connected;      // reports are coming
    gamepad_report_t pad;            // the last of its controller's reports
    uint8_t          keys[KBD_MAX_KEYS];
    uint8_t          modifiers;      // the keys held (a keyboard's usages), its last keyboard report
    uint32_t         reports;        // so far
} gamepad_state_t;

void gamepad_decode_reset(void);
void gamepad_decode_report(const gamepad_report_t *report);
void gamepad_decode_keys(const kbd_report_t *report);
void gamepad_decode_get(gamepad_state_t *out);

#endif // PICO_BT_GAMEPAD_DECODE_H
