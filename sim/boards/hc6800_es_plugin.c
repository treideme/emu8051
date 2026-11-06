/* hc6800_es_plugin.c - the HC6800-ES board as a plugin (see hc6800_es_plugin.h)
 * Copyright 2026 Thomas Reidemeister, MIT License (see ../devices/hc573.h)
 *
 * Written against plugin.h's host table, the device models in ../devices
 * and the pin catalog in hc6800_es.c, and nothing else of the host: no
 * struct sim, no capi.h. That is what lets the same file be linked into
 * libem8051sim and built into a shared object.
 */
#include <stdlib.h>
#include <string.h>
#include "hc6800_es_plugin.h"
#include "hc6800_es.h"
#include "../bus.h"
#include "../pin.h"
#include "../devices/digit_display.h"
#include "../devices/hd44780.h"
#include "../devices/ds1302.h"
#include "../devices/xpt2046.h"
#include "../devices/servo.h"
#include "../devices/enc28j60.h"
#include "../devices/fan.h"
#include "../devices/iap.h"

typedef struct
{
    const emu8051_host_t *host;
    void *sim;          // host->sim, the first argument of every host function
    struct em8051 *cpu; // host->cpu(): the device models read pins from it
    sim_bus_t *bus;     // forwarding: the models' subscriptions land on the host's bus
    unsigned ticks;     // the tick hooks installed, TICK_*

    digit_display_t *digit_display;
    hd44780_t *lcd;
    ds1302_t *ds1302;
    xpt2046_t *xpt2046;
    servo_t *servo; // the servo, ENC28J60 and fan are not on the board: the caller names their pins
    enc28j60_t *enc28j60;
    fan_t *fan;
    iap_t *iap; // the STC89's flash programming SFRs
} board_t;

enum
{
    TICK_LCD = 1,
    TICK_DS1302 = 2,
    TICK_SERVO = 4,
    TICK_FAN = 8,
    TICK_IAP = 16,
};

// ------------------------------------------------------------------ stepping

// Once per tick of 12 clocks, in both execution modes (plugin.h): the
// models that count time in machine cycles. Each has a tick hook of its
// own, installed when it is enabled, so a board with only the digit
// display (or nothing) costs the host nothing per tick, and one with an
// LCD costs one call. The models do not depend on each other, so their
// order does not matter.
static void lcd_tick(void *aUser)
{
    board_t *b = (board_t *)aUser;
    if (b->lcd)
        hd44780_step(b->lcd);
}

static void ds1302_tick(void *aUser)
{
    board_t *b = (board_t *)aUser;
    if (b->ds1302)
        ds1302_step(b->ds1302);
}

static void servo_tick(void *aUser)
{
    board_t *b = (board_t *)aUser;
    if (b->servo)
        servo_step(b->servo);
}

static void fan_tick(void *aUser)
{
    board_t *b = (board_t *)aUser;
    if (b->fan)
        fan_step(b->fan);
}

static void iap_tick(void *aUser)
{
    board_t *b = (board_t *)aUser;
    if (b->iap)
        iap_post_tick(b->iap);
}

// Installs aFn once (aWhich is its bit in b->ticks), before the model it
// steps exists: the hook skips a model that is not there.
static int need_tick(board_t *b, unsigned aWhich, emu8051_tick_fn aFn)
{
    if (b->ticks & aWhich)
        return 0;
    if (b->host->on_tick(b->sim, aFn, b) != 0)
        return -1;
    b->ticks |= aWhich;
    return 0;
}

// The IAP model asks for a software reset; the host's reset_cpu() is the
// core's reset() plus whatever the host keeps across it.
static void iap_reset(void *aUser)
{
    board_t *b = (board_t *)aUser;
    b->host->reset_cpu(b->sim, 0);
}

// ------------------------------------------------------------------ reset and destroy

// sim_reset(), after the host has reset the CPU. The LCD and the digit
// display only reflect what the CPU drove onto their pins, so they start
// over; the DS1302 (battery-backed), the XPT2046 (its readings are test
// stimulus) and the external peripherals keep their state. The IAP model's
// ISPEN/SWBS bits survive a software reset, not this power cycle.
static void board_reset(void *aCtx)
{
    board_t *b = (board_t *)aCtx;
    if (b->iap)
        iap_power_on(b->iap);
    if (b->digit_display)
        digit_display_reset(b->digit_display, b->cpu);
    if (b->lcd)
        hd44780_reset(b->lcd, b->host->clock_hz(b->sim));
}

// sim_close(). No host function may be called from here.
static void board_destroy(void *aCtx)
{
    board_t *b = (board_t *)aCtx;
    if (b->digit_display)
        digit_display_destroy(b->digit_display);
    if (b->lcd)
        hd44780_destroy(b->lcd);
    if (b->ds1302)
        ds1302_destroy(b->ds1302);
    if (b->xpt2046)
        xpt2046_destroy(b->xpt2046);
    if (b->servo)
        servo_destroy(b->servo);
    if (b->enc28j60)
        enc28j60_destroy(b->enc28j60);
    if (b->fan)
        fan_destroy(b->fan);
    if (b->iap)
        iap_destroy(b->iap);
    bus_destroy(b->bus);
    free(b);
}

// ------------------------------------------------------------------ methods

// One call, its integer arguments decoded.
typedef struct
{
    const int32_t *a;
    const uint8_t *in;
    int in_len;
    uint8_t *out;
    int out_cap;
    int *out_len;
} call_t;

typedef int (*method_fn)(board_t *b, call_t *c);

static int ret_int(call_t *c, int32_t aValue)
{
    uint32_t v = (uint32_t)aValue;
    *c->out_len = 4;
    if (c->out_cap < 4)
        return EMU8051_CALL_TOO_SMALL;
    c->out[0] = (uint8_t)v;
    c->out[1] = (uint8_t)(v >> 8);
    c->out[2] = (uint8_t)(v >> 16);
    c->out[3] = (uint8_t)(v >> 24);
    return EMU8051_CALL_OK;
}

// A port pin as capi.h numbers it: port 0-3 is P0-P3.
static int pin_ok(int32_t aPort, int32_t aBit)
{
    return aPort >= 0 && aPort <= 3 && aBit >= 0 && aBit <= 7;
}

static pin_t port_pin(int32_t aPort, int32_t aBit)
{
    return pin_make((uint8_t)(aPort * 0x10), (uint8_t)aBit);
}

// ---- board peripherals, wired from the catalog

static int m_enable_digit_display(board_t *b, call_t *c)
{
    digit_display_pins_t pins;
    if (b->digit_display)
        return EMU8051_CALL_OK;
    pins.latch = HC6800_ES_SEG_LATCH;
    pins.select = HC6800_ES_SEG_SELECT;
    pins.digit_count = c->a[0];
    b->digit_display = digit_display_create(b->bus, b->cpu, pins);
    return b->digit_display ? EMU8051_CALL_OK : HC6800_ES_CALL_FAILED;
}

static int segments(board_t *b, int32_t aDigit)
{
    return b->digit_display ? digit_display_get_segments(b->digit_display, aDigit) : 0;
}

static int m_digit_get_segments(board_t *b, call_t *c) { return ret_int(c, segments(b, c->a[0])); }

static int m_digit_get_char(board_t *b, call_t *c)
{
    return ret_int(c, digit_display_decode((uint8_t)segments(b, c->a[0])));
}

static int m_enable_lcd(board_t *b, call_t *c)
{
    (void)c;
    if (b->lcd)
        return EMU8051_CALL_OK;
    if (need_tick(b, TICK_LCD, lcd_tick) != 0)
        return HC6800_ES_CALL_FAILED;
    b->lcd = hd44780_create(b->bus, b->cpu, HC6800_ES_LCD, b->host->clock_hz(b->sim));
    return b->lcd ? EMU8051_CALL_OK : HC6800_ES_CALL_FAILED;
}

static int m_lcd_get_line(board_t *b, call_t *c)
{
    char line[256 + 1];
    int n = (uint8_t)c->a[1]; // the width, as hd44780_get_line() takes it
    *c->out_len = 0;
    if (!b->lcd)
        return EMU8051_CALL_OK;
    if (n > c->out_cap)
        n = c->out_cap;
    hd44780_get_line(b->lcd, c->a[0], (uint8_t)c->a[1], line, (size_t)n + 1);
    memcpy(c->out, line, (size_t)n);
    *c->out_len = n;
    return EMU8051_CALL_OK;
}

static int m_enable_ds1302(board_t *b, call_t *c)
{
    (void)c;
    if (b->ds1302)
        return EMU8051_CALL_OK;
    if (need_tick(b, TICK_DS1302, ds1302_tick) != 0)
        return HC6800_ES_CALL_FAILED;
    b->ds1302 = ds1302_create(b->bus, b->cpu, HC6800_ES_DS1302, b->host->clock_hz(b->sim));
    return b->ds1302 ? EMU8051_CALL_OK : HC6800_ES_CALL_FAILED;
}

static int m_ds1302_set_time(board_t *b, call_t *c)
{
    const int32_t *a = c->a;
    if (b->ds1302)
        ds1302_set_time(b->ds1302, a[0], a[1], a[2], a[3], a[4], a[5], a[6]);
    return EMU8051_CALL_OK;
}

static int m_ds1302_get_register(board_t *b, call_t *c)
{
    return ret_int(c, b->ds1302 ? ds1302_get_register(b->ds1302, c->a[0]) : 0);
}

static int m_enable_xpt2046(board_t *b, call_t *c)
{
    (void)c;
    if (!b->xpt2046)
        b->xpt2046 = xpt2046_create(b->bus, b->cpu, HC6800_ES_XPT2046);
    return b->xpt2046 ? EMU8051_CALL_OK : HC6800_ES_CALL_FAILED;
}

static int m_xpt2046_set_reading(board_t *b, call_t *c)
{
    if (b->xpt2046)
        xpt2046_set_reading(b->xpt2046, (uint16_t)c->a[0]);
    return EMU8051_CALL_OK;
}

static int m_xpt2046_set_channel_reading(board_t *b, call_t *c)
{
    if (b->xpt2046)
        xpt2046_set_channel_reading(b->xpt2046, c->a[0], (uint16_t)c->a[1]);
    return EMU8051_CALL_OK;
}

static int m_xpt2046_get_last_channel(board_t *b, call_t *c)
{
    return ret_int(c, b->xpt2046 ? xpt2046_get_last_channel(b->xpt2046) : -1);
}

// ---- external peripherals: the caller names the pins

static int m_enable_servo(board_t *b, call_t *c)
{
    const int32_t *a = c->a;
    if (b->servo)
        return EMU8051_CALL_OK;
    if (!pin_ok(a[0], a[1]))
        return EMU8051_CALL_BAD_ARGS;
    if (need_tick(b, TICK_SERVO, servo_tick) != 0)
        return HC6800_ES_CALL_FAILED;
    b->servo = servo_create(b->bus, b->cpu, port_pin(a[0], a[1]), b->host->clock_hz(b->sim), (uint16_t)a[2],
                            (uint16_t)a[3]);
    return b->servo ? EMU8051_CALL_OK : HC6800_ES_CALL_FAILED;
}

static int m_servo_get_pulse_us(board_t *b, call_t *c)
{
    return ret_int(c, b->servo ? servo_get_pulse_us(b->servo) : 0);
}

static int m_servo_get_angle_decidegrees(board_t *b, call_t *c)
{
    return ret_int(c, b->servo ? servo_get_angle_decidegrees(b->servo) : 0);
}

static int m_enable_enc28j60(board_t *b, call_t *c)
{
    const int32_t *a = c->a;
    enc28j60_pins_t pins;
    int i;
    if (b->enc28j60)
        return EMU8051_CALL_OK;
    for (i = 0; i < 8; i += 2)
        if (!pin_ok(a[i], a[i + 1]))
            return EMU8051_CALL_BAD_ARGS;
    pins.cs = port_pin(a[0], a[1]);
    pins.sck = port_pin(a[2], a[3]);
    pins.mosi = port_pin(a[4], a[5]);
    pins.miso = port_pin(a[6], a[7]);
    b->enc28j60 = enc28j60_create(b->bus, b->cpu, pins);
    return b->enc28j60 ? EMU8051_CALL_OK : HC6800_ES_CALL_FAILED;
}

static int m_enc28j60_get_register(board_t *b, call_t *c)
{
    return ret_int(c, b->enc28j60 ? enc28j60_get_register(b->enc28j60, c->a[0], c->a[1]) : 0);
}

static int m_enc28j60_get_bank(board_t *b, call_t *c)
{
    return ret_int(c, b->enc28j60 ? enc28j60_get_bank(b->enc28j60) : 0);
}

static int m_enc28j60_get_last_opcode(board_t *b, call_t *c)
{
    return ret_int(c, b->enc28j60 ? enc28j60_get_last_opcode(b->enc28j60) : 0);
}

static int m_enc28j60_get_buffer_byte_count(board_t *b, call_t *c)
{
    return ret_int(c, b->enc28j60 ? (int32_t)enc28j60_get_buffer_byte_count(b->enc28j60) : 0);
}

static int m_enc28j60_inject_rx(board_t *b, call_t *c)
{
    if (!b->enc28j60)
        return ret_int(c, ENC28J60_RX_DISABLED);
    return ret_int(c, enc28j60_inject_rx(b->enc28j60, c->in, c->in_len));
}

static int m_enc28j60_poke_buffer(board_t *b, call_t *c)
{
    if (!b->enc28j60)
        return HC6800_ES_CALL_FAILED;
    enc28j60_poke_buffer(b->enc28j60, c->a[0], c->a[1]);
    return EMU8051_CALL_OK;
}

static int m_enc28j60_peek_buffer(board_t *b, call_t *c)
{
    return ret_int(c, b->enc28j60 ? enc28j60_peek_buffer(b->enc28j60, c->a[0]) : -1);
}

static int m_enc28j60_tx_count(board_t *b, call_t *c)
{
    return ret_int(c, b->enc28j60 ? enc28j60_tx_count(b->enc28j60) : 0);
}

static int m_enc28j60_tx_frame(board_t *b, call_t *c)
{
    int n;
    if (!b->enc28j60)
        return HC6800_ES_CALL_FAILED;
    // copies what fits and returns the frame's full length
    n = enc28j60_tx_frame(b->enc28j60, c->a[0], c->out, c->out_cap);
    if (n < 0)
        return HC6800_ES_CALL_FAILED;
    *c->out_len = n;
    return n > c->out_cap ? EMU8051_CALL_TOO_SMALL : EMU8051_CALL_OK;
}

static int m_enable_fan(board_t *b, call_t *c)
{
    const int32_t *a = c->a;
    if (b->fan)
        return EMU8051_CALL_OK;
    if (!pin_ok(a[0], a[1]) || !pin_ok(a[2], a[3]))
        return EMU8051_CALL_BAD_ARGS;
    if (need_tick(b, TICK_FAN, fan_tick) != 0)
        return HC6800_ES_CALL_FAILED;
    b->fan = fan_create(b->bus, b->cpu, port_pin(a[0], a[1]), port_pin(a[2], a[3]), b->host->clock_hz(b->sim));
    return b->fan ? EMU8051_CALL_OK : HC6800_ES_CALL_FAILED;
}

static int m_fan_get_duty_percent(board_t *b, call_t *c)
{
    return ret_int(c, b->fan ? fan_get_duty_percent(b->fan) : 0);
}

static int m_fan_get_rpm(board_t *b, call_t *c)
{
    return ret_int(c, b->fan ? fan_get_rpm(b->fan) : 0);
}

// ---- the STC89's flash programming SFRs

static int m_enable_iap(board_t *b, call_t *c)
{
    if (b->iap)
        return EMU8051_CALL_OK;
    if (need_tick(b, TICK_IAP, iap_tick) != 0)
        return HC6800_ES_CALL_FAILED;
    b->iap = iap_create(b->bus, b->cpu, (iap_profile_t)c->a[0]);
    if (!b->iap)
        return HC6800_ES_CALL_FAILED;
    iap_set_reset(b->iap, iap_reset, b);
    return EMU8051_CALL_OK;
}

static int m_iap_peek(board_t *b, call_t *c) { return ret_int(c, iap_peek(b->iap, c->a[0])); }

static int m_iap_poke(board_t *b, call_t *c)
{
    return iap_poke(b->iap, c->a[0], c->a[1]) == 0 ? EMU8051_CALL_OK : HC6800_ES_CALL_FAILED;
}

static int m_iap_stat(board_t *b, call_t *c) { return ret_int(c, (int32_t)iap_stat(b->iap, c->a[0])); }

// The method table: name, number of integer arguments (-1: raw bytes).
static const struct
{
    const char *name;
    int args;
    method_fn fn;
} METHODS[] = {
    {"enable_digit_display", 1, m_enable_digit_display},
    {"digit_get_segments", 1, m_digit_get_segments},
    {"digit_get_char", 1, m_digit_get_char},
    {"enable_lcd", 0, m_enable_lcd},
    {"lcd_get_line", 2, m_lcd_get_line},
    {"enable_ds1302", 0, m_enable_ds1302},
    {"ds1302_set_time", 7, m_ds1302_set_time},
    {"ds1302_get_register", 1, m_ds1302_get_register},
    {"enable_xpt2046", 0, m_enable_xpt2046},
    {"xpt2046_set_reading", 1, m_xpt2046_set_reading},
    {"xpt2046_set_channel_reading", 2, m_xpt2046_set_channel_reading},
    {"xpt2046_get_last_channel", 0, m_xpt2046_get_last_channel},
    {"enable_servo", 4, m_enable_servo},
    {"servo_get_pulse_us", 0, m_servo_get_pulse_us},
    {"servo_get_angle_decidegrees", 0, m_servo_get_angle_decidegrees},
    {"enable_enc28j60", 8, m_enable_enc28j60},
    {"enc28j60_get_register", 2, m_enc28j60_get_register},
    {"enc28j60_get_bank", 0, m_enc28j60_get_bank},
    {"enc28j60_get_last_opcode", 0, m_enc28j60_get_last_opcode},
    {"enc28j60_get_buffer_byte_count", 0, m_enc28j60_get_buffer_byte_count},
    {"enc28j60_inject_rx", -1, m_enc28j60_inject_rx},
    {"enc28j60_poke_buffer", 2, m_enc28j60_poke_buffer},
    {"enc28j60_peek_buffer", 1, m_enc28j60_peek_buffer},
    {"enc28j60_tx_count", 0, m_enc28j60_tx_count},
    {"enc28j60_tx_frame", 1, m_enc28j60_tx_frame},
    {"enable_fan", 4, m_enable_fan},
    {"fan_get_duty_percent", 0, m_fan_get_duty_percent},
    {"fan_get_rpm", 0, m_fan_get_rpm},
    {"enable_iap", 1, m_enable_iap},
    {"iap_peek", 1, m_iap_peek},
    {"iap_poke", 2, m_iap_poke},
    {"iap_stat", 1, m_iap_stat},
};

#define MAX_ARGS 8

// emu8051_plugin_t.call: find the method, check and decode its arguments.
static int board_call(void *aCtx, const char *aMethod, const uint8_t *aIn, int aInLen, uint8_t *aOut, int aOutCap,
                      int *aOutLen)
{
    size_t m;
    for (m = 0; m < sizeof(METHODS) / sizeof(METHODS[0]); m++)
    {
        int32_t args[MAX_ARGS] = {0};
        call_t c;
        int i;
        if (strcmp(METHODS[m].name, aMethod) != 0)
            continue;
        if (METHODS[m].args >= 0)
        {
            if (aInLen != 4 * METHODS[m].args)
                return EMU8051_CALL_BAD_ARGS;
            for (i = 0; i < METHODS[m].args; i++)
            {
                const uint8_t *p = aIn + 4 * i;
                args[i] = (int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
                                    (uint32_t)p[3] << 24);
            }
        }
        c.a = args;
        c.in = aIn;
        c.in_len = aInLen;
        c.out = aOut;
        c.out_cap = aOutCap;
        c.out_len = aOutLen;
        return METHODS[m].fn((board_t *)aCtx, &c);
    }
    return EMU8051_CALL_UNKNOWN;
}

// ------------------------------------------------------------------ init

int hc6800_es_plugin_init(const emu8051_host_t *aHost, emu8051_plugin_t *aPlugin, const char *aArgs)
{
    board_t *b;

    // A host of another ABI has another table: nothing in it can be called.
    if (aHost->abi != EMU8051_PLUGIN_ABI)
        return -1;
    if (!EMU8051_HOST_HAS(aHost, on_tick))
    {
        aHost->error(aHost->sim, "hc6800_es: the host has no on_tick");
        return -1;
    }
    if (aArgs[0])
    {
        aHost->error(aHost->sim, "hc6800_es: takes no arguments");
        return -1;
    }

    b = (board_t *)calloc(1, sizeof(*b));
    if (!b)
        return -1;
    b->host = aHost;
    b->sim = aHost->sim;
    b->cpu = aHost->cpu(aHost->sim);
    b->bus = bus_create_forwarding(aHost->sim, aHost->on_sfr_write, aHost->on_sfr_read);
    if (!b->bus)
    {
        free(b);
        return -1;
    }

    aPlugin->name = HC6800_ES_PLUGIN_NAME;
    aPlugin->ctx = b;
    aPlugin->destroy = board_destroy;
    aPlugin->reset = board_reset;
    aPlugin->call = board_call;
    return 0;
}
