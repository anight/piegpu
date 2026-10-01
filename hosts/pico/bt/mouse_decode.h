/*
 * A mouse's reports, for whoever wants them (added in piegpu, beside
 * kbd_decode.h): the transports hand every report that has a mouse's fields to
 * mouse_decode_report, which passes it on with the buttons that changed.
 */
#ifndef PICO_BT_MOUSE_DECODE_H
#define PICO_BT_MOUSE_DECODE_H

#include <stdint.h>

#include "hid_report.h"

typedef struct {
    int16_t  dx, dy;        // moved since the last event: right and down positive
    int8_t   wheel, pan;    // the wheels: away from the user, and right, positive
    uint16_t buttons;       // held now: bit 0 left, 1 right, 2 middle, ...
    uint16_t changed;       // the buttons that went down or up with this event
} mouse_event_t;

// Called for every report (in the Bluetooth stack's context).
typedef void (*mouse_event_handler_t)(const mouse_event_t *event);

void mouse_decode_init(mouse_event_handler_t on_event);

// Forget the buttons held. Called on connect and disconnect: a button held
// when the link dropped comes up (an event with it in `changed`).
void mouse_decode_reset(void);

void mouse_decode_report(const mouse_report_t *report);

#endif // PICO_BT_MOUSE_DECODE_H
