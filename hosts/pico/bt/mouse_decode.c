#include "mouse_decode.h"

#include <stddef.h>
#include <string.h>

static mouse_event_handler_t handler;
static uint16_t held;

void mouse_decode_init(mouse_event_handler_t on_event) {
    handler = on_event;
    held = 0;
}

void mouse_decode_reset(void) {
    if (held != 0 && handler != NULL) {
        mouse_event_t event;
        memset(&event, 0, sizeof(event));
        event.changed = held;
        held = 0;
        handler(&event);
    }
    held = 0;
}

void mouse_decode_report(const mouse_report_t *report) {
    mouse_event_t event;
    event.dx = report->dx;
    event.dy = report->dy;
    event.wheel = report->wheel;
    event.pan = report->pan;
    event.buttons = report->buttons;
    event.changed = (uint16_t) (report->buttons ^ held);
    held = report->buttons;
    if (handler != NULL) handler(&event);
}
