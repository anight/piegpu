#include "../psdl_pico_log.h"
#include "debug.h"

#include <stdarg.h>
#include <stdio.h>

// Off by default: normal use is a keyboard that works, and what you want to see
// then is the text you typed. Press 'd' when a keyboard misbehaves.
bool pico_bt_keyboard_verbose = false;

void dbg(const char *fmt, ...) {
    if (!pico_bt_keyboard_verbose) return;

    /* Formatted here and handed over as one string: psdl_log takes varargs, not a
     * va_list, and the whole point is that the line arrives whole. */
    char line[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    psdl_log("%s", line);
}

#define DBG_BYTES_MAX 12

void dbg_bytes(const char *label, const uint8_t *data, uint16_t len) {
    if (!pico_bt_keyboard_verbose) return;

    psdl_log("%s len=%u", label, len);
    if (data == NULL) {
        psdl_log(" <null>\n");
        return;
    }

    uint16_t shown = (len > DBG_BYTES_MAX) ? DBG_BYTES_MAX : len;
    for (uint16_t i = 0; i < shown; i++) {
        psdl_log(" %02x", data[i]);
    }
    if (shown < len) psdl_log(" ...");
    psdl_log("\n");
}
