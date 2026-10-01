#include "../psdl_pico_log.h"
#include "hid_report.h"

#include <string.h>

#include "btstack.h"

#include <stdio.h>

#include "debug.h"

#define USAGE_PAGE_KEYBOARD 0x07
#define USAGE_PAGE_CONSUMER 0x0c

/*
 * Consumer-page media usages, and the Keyboard-page usages that mean the same
 * thing. The keyboard page has its own Mute/Volume Up/Volume Down at 0x7f-0x81,
 * and those are already SDL_SCANCODE_MUTE / VOLUMEUP / VOLUMEDOWN, so translating
 * to them keeps picosdl's "a usage *is* a scancode" property intact and needs no
 * mapping table anywhere else.
 *
 * The Consumer usage cannot simply be passed through: kbd_report_t holds usages
 * as uint8_t, and Consumer 0xe2 (Mute) would collide with Keyboard 0xe2, which is
 * Left Alt.
 */
static const struct { uint16_t consumer; uint8_t keyboard; } media_map[] = {
    { 0x00e2, 0x7f },   // Mute
    { 0x00e9, 0x80 },   // Volume Increment
    { 0x00ea, 0x81 },   // Volume Decrement
};
#define USAGE_MOD_FIRST     0xe0
#define USAGE_MOD_LAST      0xe7

void hid_report_to_kbd(const uint8_t *descriptor, uint16_t descriptor_len,
                       const uint8_t *report, uint16_t report_len,
                       kbd_report_t *out, kbd_media_t *media) {
    memset(out, 0, sizeof(*out));
    if (media != NULL) memset(media, 0, sizeof(*media));
    if (descriptor == NULL || descriptor_len == 0) return;

    // The parser skips usages whose Report ID does not match report[0], so a
    // descriptor that declares IDs against a payload missing its ID byte yields
    // nothing at all. Log enough to tell that case apart from a real mismatch.

    btstack_hid_parser_t parser;
    btstack_hid_parser_init(&parser, descriptor, descriptor_len,
                            HID_REPORT_TYPE_INPUT, report, report_len);

    int count = 0;
    int fields = 0;
    while (btstack_hid_parser_has_more(&parser)) {
        uint16_t usage_page;
        uint16_t usage;
        int32_t  value;
        btstack_hid_parser_get_field(&parser, &usage_page, &usage, &value);
        fields++;

        if (usage_page == USAGE_PAGE_CONSUMER) {
            if (media == NULL) continue;
            media->describes_media = true;
            if (value == 0 || usage == 0) continue;
            for (unsigned m = 0; m < sizeof(media_map)/sizeof(media_map[0]); m++) {
                if (media_map[m].consumer != usage) continue;
                if (media->count < KBD_MAX_MEDIA)
                    media->usage[media->count++] = media_map[m].keyboard;
                break;
            }
            continue;
        }

        if (usage_page != USAGE_PAGE_KEYBOARD) continue;
        if (media != NULL) media->describes_keyboard = true;

        // For array fields the parser reports value == 1 and puts the keycode
        // in usage; for variable (bitmap) fields usage is fixed and value is
        // the bit. Testing value therefore works for both, and correctly drops
        // the unpressed bits of an NKRO bitmap descriptor.
        if (value == 0) continue;
        if (usage == 0) continue;   // empty slot in a key array

        if (usage >= USAGE_MOD_FIRST && usage <= USAGE_MOD_LAST) {
            out->modifiers |= (uint8_t) (1u << (usage - USAGE_MOD_FIRST));
            continue;
        }

        if (count < KBD_MAX_KEYS) {
            out->keys[count++] = (uint8_t) usage;
        }
    }

    if (fields == 0) {
        // Almost always a Report ID mismatch: the parser only yields usages whose
        // Report ID matches report[0], so name both when it comes up empty.
        dbg("[hid] no fields decoded (report[0]=0x%02x, descriptor %s report IDs)\n",
            report_len > 0 ? report[0] : 0,
            btstack_hid_report_id_declared(descriptor, descriptor_len) ? "uses" : "has no");
    }
}

void hid_led_layout_find(hid_led_layout_t *layout,
                         const uint8_t *descriptor, uint16_t descriptor_len) {
    memset(layout, 0, sizeof(*layout));
    if (descriptor == NULL || descriptor_len == 0) return;

    uint16_t first_report_id = HID_REPORT_ID_UNDEFINED;

    btstack_hid_usage_iterator_t iterator;
    btstack_hid_usage_iterator_init(&iterator, descriptor, descriptor_len,
                                    HID_REPORT_TYPE_OUTPUT);

    while (btstack_hid_usage_iterator_has_more(&iterator)) {
        btstack_hid_usage_item_t item;
        btstack_hid_usage_iterator_get_item(&iterator, &item);

        if (item.usage_page != HID_USAGE_PAGE_LED) continue;

        int index;
        switch (item.usage) {
            case HID_USAGE_LED_NUM_LOCK:    index = 0; break;
            case HID_USAGE_LED_CAPS_LOCK:   index = 1; break;
            case HID_USAGE_LED_SCROLL_LOCK: index = 2; break;
            default: continue;
        }

        // All three LEDs live in the same Output report in practice; take the
        // report id from whichever we see first and ignore any others.
        if (!layout->found) {
            layout->found = true;
            // A descriptor with no Report ID items reports HID_REPORT_ID_UNDEFINED
            // (0xffff); on the wire that case is report id 0.
            layout->report_id = (item.report_id == HID_REPORT_ID_UNDEFINED)
                              ? 0 : (uint8_t) item.report_id;
            first_report_id = item.report_id;
            int size = btstack_hid_get_report_size_for_id(
                item.report_id, HID_REPORT_TYPE_OUTPUT, descriptor, descriptor_len);
            layout->report_len = (size > 0) ? (uint8_t) size : 1;
        } else if (item.report_id != first_report_id) {
            continue;
        }

        layout->bit[index] = item.bit_pos;
        layout->has_bit[index] = true;
    }
}

uint8_t hid_led_build_report(const hid_led_layout_t *layout, uint8_t led_mask,
                             uint8_t *out, uint8_t out_size) {
    if (!layout->found) return 0;
    if (layout->report_len == 0 || layout->report_len > out_size) return 0;

    memset(out, 0, layout->report_len);

    for (int index = 0; index < 3; index++) {
        if (!layout->has_bit[index]) continue;
        if ((led_mask & (1u << index)) == 0) continue;

        uint16_t byte = layout->bit[index] >> 3;
        if (byte >= layout->report_len) continue;
        out[byte] |= (uint8_t) (1u << (layout->bit[index] & 0x07));
    }

    return layout->report_len;
}
