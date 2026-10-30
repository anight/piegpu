/*
 * gamepad_decode.c - a game controller's latest state (gamepad_decode.h)
 */
#include "gamepad_decode.h"

#include <string.h>

#include "hardware/sync.h"

static gamepad_state_t state;

void gamepad_decode_reset(void) {
    uint32_t s = save_and_disable_interrupts();
    memset(&state, 0, sizeof(state));
    state.pad.hat = -1;
    restore_interrupts(s);
}

void gamepad_decode_report(const gamepad_report_t *report) {
    uint32_t s = save_and_disable_interrupts();
    state.pad = *report;
    state.connected = true;
    state.reports++;
    restore_interrupts(s);
}

void gamepad_decode_keys(const kbd_report_t *report) {
    uint32_t s = save_and_disable_interrupts();
    memcpy(state.keys, report->keys, sizeof(state.keys));
    state.modifiers = report->modifiers;
    state.connected = true;
    state.reports++;
    restore_interrupts(s);
}

void gamepad_decode_get(gamepad_state_t *out) {
    uint32_t s = save_and_disable_interrupts();
    *out = state;
    restore_interrupts(s);
}
