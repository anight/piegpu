/*
 * Adafruit Gamepad QT, over Adafruit's seesaw protocol.
 *
 * The protocol is two bytes of register pointer - a module base and a function
 * within it - then either the data to write, or a separate read of the answer.
 * Written against Adafruit_CircuitPython_seesaw rather than from guesswork, which
 * matters more than it sounds: a first attempt here probed with a bare read, and a
 * seesaw with no register selected is entitled to NAK that. It looked exactly like
 * absent hardware. Every read below writes the pointer first, as the library does.
 *
 * Timing is measured. The library waits 8 ms between pointer and read, being a
 * default that has to suit any seesaw on any bus; this device needed no added delay
 * at all across 200 hardware-ID reads and 100 ADC reads. See the note on
 * PSDL_BOARD_GAMEPAD_READ_DELAY_US in picosdl's board.h for the numbers.
 *
 * From picosdl (backend/pico/seesaw_gamepad.c), as it is there but for what it
 * prints.
 */
#include "seesaw_gamepad.h"

#include <stdio.h>

#include "hardware/i2c.h"
#include "pico/stdlib.h"

#include "board.h"

#define ADDR   PSDL_BOARD_GAMEPAD_ADDR
#define BUS    PSDL_BOARD_GAMEPAD_I2C
#define DELAY  PSDL_BOARD_GAMEPAD_READ_DELAY_US

/* Register map, from the library's constants. */
#define SS_STATUS_BASE  0x00
#define  SS_HW_ID         0x01
#define  SS_VERSION       0x02
#define  SS_SWRST         0x7f
#define SS_GPIO_BASE    0x01
#define  SS_DIRCLR_BULK   0x03
#define  SS_GPIO_BULK     0x04
#define  SS_BULK_SET      0x05
#define  SS_PULLENSET     0x0b
#define SS_ADC_MODULE   0x09
#define  SS_ADC_OFFSET    0x07

#define SS_HW_ID_ATTINY817 0x87   /* what a Gamepad QT is */

/* Where the pad's controls sit on the seesaw's own pins, from
 * examples/seesaw_gamepad_qt.py. */
#define BTN_SELECT  0
#define BTN_B       1
#define BTN_Y       2
#define BTN_A       5
#define BTN_X       6
#define BTN_START  16
#define BUTTON_MASK ((1u << BTN_SELECT) | (1u << BTN_B) | (1u << BTN_Y) | \
                     (1u << BTN_A) | (1u << BTN_X) | (1u << BTN_START))
#define ADC_CH_X   14
#define ADC_CH_Y   15

static bool s_present;

static bool ss_write(uint8_t base, uint8_t func, const uint8_t *data, size_t len)
{
    uint8_t buf[6];
    if (len > sizeof buf - 2)
        return false;
    buf[0] = base;
    buf[1] = func;
    for (size_t i = 0; i < len; ++i)
        buf[2 + i] = data[i];
    return i2c_write_blocking(BUS, ADDR, buf, 2 + len, false) == (int)(2 + len);
}

static bool ss_read(uint8_t base, uint8_t func, uint8_t *out, size_t len)
{
    uint8_t ptr[2] = { base, func };
    if (i2c_write_blocking(BUS, ADDR, ptr, 2, false) != 2)
        return false;
    busy_wait_us(DELAY);
    return i2c_read_blocking(BUS, ADDR, out, len, false) == (int)len;
}

static bool ss_write_mask(uint8_t func, uint32_t mask)
{
    uint8_t m[4] = { (uint8_t)(mask >> 24), (uint8_t)(mask >> 16),
                     (uint8_t)(mask >> 8),  (uint8_t)mask };
    return ss_write(SS_GPIO_BASE, func, m, 4);
}

static uint32_t be32(const uint8_t *b)
{
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8)  | (uint32_t)b[3];
}

bool seesaw_gamepad_present(void) { return s_present; }

bool seesaw_gamepad_init(void)
{
    s_present = false;

    i2c_init(BUS, PSDL_BOARD_GAMEPAD_BAUD);
    gpio_set_function(PSDL_BOARD_GAMEPAD_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(PSDL_BOARD_GAMEPAD_SCL_PIN, GPIO_FUNC_I2C);
    /* The Stemma QT breakout carries its own 10k pull-ups; these cost nothing and
     * keep the bus defined if a bare module is wired without them. */
    gpio_pull_up(PSDL_BOARD_GAMEPAD_SDA_PIN);
    gpio_pull_up(PSDL_BOARD_GAMEPAD_SCL_PIN);

    /* Presence is a write that gets acknowledged, not a read. See the note above. */
    uint8_t ptr[2] = { SS_STATUS_BASE, SS_HW_ID };
    if (i2c_write_blocking(BUS, ADDR, ptr, 2, false) != 2) {
        printf("pad: no game controller on I2C at 0x%02x\n", ADDR);
        return false;
    }

    /* Software reset, as Seesaw.__init__ does by default. The 500 ms is the
     * library's and is not worth shaving: this runs once, at start-up. */
    uint8_t ff = 0xff;
    ss_write(SS_STATUS_BASE, SS_SWRST, &ff, 1);
    sleep_ms(500);

    uint8_t id = 0;
    if (!ss_read(SS_STATUS_BASE, SS_HW_ID, &id, 1)) {
        printf("pad: 0x%02x answers but will not report a seesaw ID - ignoring\n",
               ADDR);
        return false;
    }
    if (id != SS_HW_ID_ATTINY817) {
        /* Not fatal: another seesaw board with the same pin layout would work.
         * Say so rather than silently treating it as a gamepad. */
        printf("pad: seesaw at 0x%02x reports ID 0x%02x, not the Gamepad QT's "
               "0x%02x - trying anyway\n", ADDR, id, SS_HW_ID_ATTINY817);
    }

    uint8_t ver[4] = { 0 };
    ss_read(SS_STATUS_BASE, SS_VERSION, ver, 4);

    /* pin_mode_bulk(BUTTON_MASK, INPUT_PULLUP): clear direction, enable the
     * pull-up, then drive the pull-up high. All three, in that order. */
    ss_write_mask(SS_DIRCLR_BULK, BUTTON_MASK);
    ss_write_mask(SS_PULLENSET,   BUTTON_MASK);
    ss_write_mask(SS_BULK_SET,    BUTTON_MASK);

    s_present = true;
    printf("pad: a game controller on I2C at 0x%02x, seesaw ID 0x%02x, "
           "firmware 0x%08x\n", ADDR, id, (unsigned)be32(ver));
    return true;
}

bool seesaw_gamepad_read(seesaw_gamepad_state_t *out)
{
    if (!s_present || out == NULL)
        return false;

    uint8_t g[4], ax[2], ay[2];
    if (!ss_read(SS_GPIO_BASE, SS_GPIO_BULK, g, 4))
        return false;
    if (!ss_read(SS_ADC_MODULE, SS_ADC_OFFSET + ADC_CH_X, ax, 2))
        return false;
    if (!ss_read(SS_ADC_MODULE, SS_ADC_OFFSET + ADC_CH_Y, ay, 2))
        return false;

    uint32_t pins = be32(g);

    /* The buttons are inputs with pull-ups, so a press reads low. */
    out->a      = !(pins & (1u << BTN_A));
    out->b      = !(pins & (1u << BTN_B));
    out->x_btn  = !(pins & (1u << BTN_X));
    out->y_btn  = !(pins & (1u << BTN_Y));
    out->select = !(pins & (1u << BTN_SELECT));
    out->start  = !(pins & (1u << BTN_START));

    uint16_t x = (uint16_t)((ax[0] << 8) | ax[1]);
    uint16_t y = (uint16_t)((ay[0] << 8) | ay[1]);
    /* The ATtiny's ADC is 10-bit. Anything above that is a bad transfer rather
     * than a real reading, and passing it on would look like a hard deflection. */
    if (x > 1023 || y > 1023)
        return false;
    out->x = x;
    out->y = y;
    return true;
}
