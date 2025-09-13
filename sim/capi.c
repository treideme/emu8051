/* capi.c - see capi.h
 * Copyright 2025 Thomas Reidemeister, MIT License (see devices/hc573.h)
 */
#include <stdlib.h>
#include <string.h>
#include "capi.h"
#include "bus.h"
#include "pin.h"
#include "devices/digit_display.h"
#include "devices/hd44780.h"
#include "devices/ds1302.h"
#include "devices/xpt2046.h"
#include "boards/hc6800_es.h"

#define MAX_EXCEPTIONS 64
#define MAX_STIM 16

typedef struct
{
    int port;
    int bit;
    int active;
    int value;
} stim_entry_t;

struct sim
{
    struct em8051 cpu; // MUST stay the first member: on_exception() casts
                        // struct em8051* straight back to struct sim*
                        // instead of needing a second cpu->sim registry.
    sim_bus_t *bus;

    unsigned long clock_hz;

    digit_display_t *digit_display;
    int digit_count; // remembered so sim_reset() can recreate it identically
    hd44780_t *lcd;
    ds1302_t *ds1302;
    xpt2046_t *xpt2046;

    int exceptions[MAX_EXCEPTIONS];
    int exception_count;

    int total_tx_count; // see sim_uart_tx_count -- cpu.serial_out_idx alone
                         // wraps at 18 and can't tell "0 sent" from "18 sent"
    int last_tx_idx;

    long total_instructions;
    unsigned long total_ticks;

    stim_entry_t stim[MAX_STIM];
    int stim_count;
    int stim_port_registered[4];
};

static void on_sbuf_write(struct em8051 *aCPU, uint8_t aReg, uint8_t aValue, void *aUserData)
{
    // Writing SBUF only stages the byte in mSFR[]; the core's own
    // serial_tx() (core.c) does the actual bit-shifting and only runs
    // once serial_out_remaining_bits is armed -- mirroring emu.c's
    // emu_sfrwrite_SBUF, which is how the interactive GUI gets this
    // right. Without this, TX bytes never reach serial_out[] at all,
    // regardless of what the firmware writes to SBUF (found by tracing
    // sim_uart_tx_bytes() coming back empty even though SBUF itself was
    // confirmed correct via sim_peek_sfr()).
    (void)aCPU;
    (void)aReg;
    (void)aValue;
    ((struct sim *)aUserData)->cpu.serial_out_remaining_bits = 8;
}

static void on_exception(struct em8051 *aCPU, int aCode)
{
    struct sim *s = (struct sim *)aCPU;
    if (s->exception_count < MAX_EXCEPTIONS)
        s->exceptions[s->exception_count++] = aCode;
}

static void on_stim_read(struct em8051 *aCPU, uint8_t aReg, void *aUserData, uint8_t *aOutMask, uint8_t *aOutValue)
{
    struct sim *s = (struct sim *)aUserData;
    int port = aReg / 0x10;
    int i;
    (void)aCPU;

    for (i = 0; i < s->stim_count; i++)
    {
        if (!s->stim[i].active || s->stim[i].port != port)
            continue;
        *aOutMask = (uint8_t)(*aOutMask | (1u << s->stim[i].bit));
        if (s->stim[i].value)
            *aOutValue = (uint8_t)(*aOutValue | (1u << s->stim[i].bit));
    }
}

sim_handle_t sim_open(const char *aBoardName)
{
    struct sim *s;

    // Only one board catalog exists today (sim/boards/hc6800_es.*); this
    // is the dispatch point a second board's constants would plug into.
    if (!aBoardName || strcmp(aBoardName, "hc6800_es") != 0)
        return NULL;

    s = (struct sim *)calloc(1, sizeof(struct sim));
    s->cpu.mCodeMemMaxIdx = 65535;
    s->cpu.mCodeMem = (unsigned char *)calloc(65536, 1);
    s->cpu.mExtDataMaxIdx = 65535;
    s->cpu.mExtData = (unsigned char *)calloc(65536, 1);
    s->cpu.mUpperData = (unsigned char *)calloc(128, 1);
    s->cpu.except = on_exception;
    s->clock_hz = HC6800_ES_XTAL_HZ; // only one board catalog exists today

    s->bus = bus_create(&s->cpu);
    bus_on_write(s->bus, REG_SBUF, on_sbuf_write, s);
    reset(&s->cpu, 1);
    return s;
}

void sim_close(sim_handle_t aSim)
{
    struct sim *s = (struct sim *)aSim;
    if (!s)
        return;
    if (s->digit_display)
        digit_display_destroy(s->digit_display);
    if (s->lcd)
        hd44780_destroy(s->lcd);
    if (s->ds1302)
        ds1302_destroy(s->ds1302);
    if (s->xpt2046)
        xpt2046_destroy(s->xpt2046);
    bus_destroy(s->bus);
    free(s->cpu.mCodeMem);
    free(s->cpu.mExtData);
    free(s->cpu.mUpperData);
    free(s);
}

int sim_load_hex(sim_handle_t aSim, const char *aPath)
{
    struct sim *s = (struct sim *)aSim;
    return load_obj(&s->cpu, (char *)aPath);
}

unsigned long sim_get_clock_hz(sim_handle_t aSim)
{
    return ((struct sim *)aSim)->clock_hz;
}

void sim_set_clock_hz(sim_handle_t aSim, unsigned long aClockHz)
{
    ((struct sim *)aSim)->clock_hz = aClockHz;
}

void sim_reset(sim_handle_t aSim)
{
    struct sim *s = (struct sim *)aSim;

    // CPU first: the digit-display/LCD recreate below reads pin/port state
    // at construction time (see hc573_create()), and that read needs to see
    // the *post*-reset power-on port state, not whatever was there a moment
    // ago.
    reset(&s->cpu, 0);

    s->exception_count = 0;
    s->total_tx_count = 0;
    s->last_tx_idx = 0;
    s->total_instructions = 0;
    s->total_ticks = 0;

    // Recreated fresh so they don't keep showing whatever they last
    // captured from the CPU before reset -- these are pure reflections of
    // CPU port writes, not independent state. DS1302/XPT2046/sim_set_pin
    // stimulus are deliberately left alone; see sim_reset()'s own comment
    // in capi.h for why.
    if (s->digit_display)
    {
        digit_display_destroy(s->digit_display);
        s->digit_display = NULL;
        sim_enable_digit_display(aSim, s->digit_count);
    }
    if (s->lcd)
    {
        hd44780_destroy(s->lcd);
        s->lcd = NULL;
        sim_enable_lcd(aSim);
    }
}

static void post_tick(struct sim *s)
{
    if (s->lcd)
        hd44780_step(s->lcd);
    if (s->ds1302)
        ds1302_step(s->ds1302);
    if (s->cpu.serial_out_idx != s->last_tx_idx)
    {
        // Can only ever differ by one byte per tick (transmitting a byte
        // takes many ticks), so this is an unambiguous count even across
        // the underlying 18-byte circular buffer wrapping.
        s->total_tx_count++;
        s->last_tx_idx = s->cpu.serial_out_idx;
    }
}

long sim_step(sim_handle_t aSim, long aTicks)
{
    struct sim *s = (struct sim *)aSim;
    long i;
    long completed = 0;

    for (i = 0; i < aTicks; i++)
    {
        if (tick(&s->cpu))
            completed++;
        post_tick(s);
    }
    s->total_instructions += completed;
    s->total_ticks += (unsigned long)aTicks;
    return completed;
}

long sim_step_instructions(sim_handle_t aSim, long aInstructions)
{
    struct sim *s = (struct sim *)aSim;
    long done = 0;
    long ticks_used = 0;
    long tick_budget = aInstructions * 100L; // generous safety net, see capi.h

    while (done < aInstructions && tick_budget-- > 0)
    {
        ticks_used++;
        if (tick(&s->cpu))
            done++;
        post_tick(s);
    }
    s->total_instructions += done;
    s->total_ticks += (unsigned long)ticks_used;
    return done;
}

long sim_get_instruction_count(sim_handle_t aSim)
{
    return ((struct sim *)aSim)->total_instructions;
}

unsigned long sim_get_tick_count(sim_handle_t aSim)
{
    return ((struct sim *)aSim)->total_ticks;
}

int sim_get_port(sim_handle_t aSim, int aPortIndex)
{
    struct sim *s = (struct sim *)aSim;
    if (aPortIndex < 0 || aPortIndex > 3)
        return -1;
    return s->cpu.mSFR[aPortIndex * 0x10];
}

int sim_peek_idata(sim_handle_t aSim, int aAddress)
{
    struct sim *s = (struct sim *)aSim;
    if (aAddress < 0 || aAddress > 127)
        return -1;
    return s->cpu.mLowerData[aAddress];
}

int sim_peek_sfr(sim_handle_t aSim, int aAddress)
{
    struct sim *s = (struct sim *)aSim;
    if (aAddress < 0x80 || aAddress > 0xff)
        return -1;
    return s->cpu.mSFR[aAddress - 0x80];
}

void sim_set_pin(sim_handle_t aSim, int aPortIndex, int aBit, int aValue)
{
    struct sim *s = (struct sim *)aSim;
    int i;

    if (aPortIndex < 0 || aPortIndex > 3 || aBit < 0 || aBit > 7)
        return;

    for (i = 0; i < s->stim_count; i++)
    {
        if (s->stim[i].port == aPortIndex && s->stim[i].bit == aBit)
        {
            s->stim[i].active = (aValue >= 0);
            s->stim[i].value = (aValue > 0);
            return;
        }
    }
    if (aValue < 0 || s->stim_count >= MAX_STIM)
        return;

    s->stim[s->stim_count].port = aPortIndex;
    s->stim[s->stim_count].bit = aBit;
    s->stim[s->stim_count].active = 1;
    s->stim[s->stim_count].value = (aValue > 0);
    s->stim_count++;

    if (!s->stim_port_registered[aPortIndex])
    {
        bus_on_read(s->bus, (uint8_t)(aPortIndex * 0x10), on_stim_read, s);
        s->stim_port_registered[aPortIndex] = 1;
    }
}

int sim_get_pin(sim_handle_t aSim, int aPortIndex, int aBit)
{
    struct sim *s = (struct sim *)aSim;
    pin_t p;
    if (aPortIndex < 0 || aPortIndex > 3 || aBit < 0 || aBit > 7)
        return -1;
    p = pin_make((uint8_t)(aPortIndex * 0x10), (uint8_t)aBit);
    return pin_get(&s->cpu, p);
}

int sim_get_exceptions(sim_handle_t aSim, int *aOutCodes, int aMaxCodes)
{
    struct sim *s = (struct sim *)aSim;
    int n = s->exception_count;
    if (n > aMaxCodes)
        n = aMaxCodes;
    memcpy(aOutCodes, s->exceptions, (size_t)n * sizeof(int));
    s->exception_count = 0;
    return n;
}

int sim_enable_digit_display(sim_handle_t aSim, int aDigitCount)
{
    struct sim *s = (struct sim *)aSim;
    digit_display_pins_t pins;
    if (s->digit_display)
        return 0;
    pins.latch = HC6800_ES_SEG_LATCH;
    pins.select = HC6800_ES_SEG_SELECT;
    pins.digit_count = aDigitCount;
    s->digit_display = digit_display_create(s->bus, &s->cpu, pins);
    s->digit_count = aDigitCount;
    return 0;
}

int sim_digit_get_segments(sim_handle_t aSim, int aDigit)
{
    struct sim *s = (struct sim *)aSim;
    if (!s->digit_display)
        return 0;
    return digit_display_get_segments(s->digit_display, aDigit);
}

int sim_digit_get_char(sim_handle_t aSim, int aDigit)
{
    return digit_display_decode((uint8_t)sim_digit_get_segments(aSim, aDigit));
}

int sim_enable_lcd(sim_handle_t aSim)
{
    struct sim *s = (struct sim *)aSim;
    if (s->lcd)
        return 0;
    s->lcd = hd44780_create(s->bus, &s->cpu, HC6800_ES_LCD, s->clock_hz);
    return 0;
}

void sim_lcd_get_line(sim_handle_t aSim, int aLine, int aWidth, char *aOutBuf, int aOutBufSize)
{
    struct sim *s = (struct sim *)aSim;
    if (!s->lcd)
    {
        if (aOutBufSize > 0)
            aOutBuf[0] = '\0';
        return;
    }
    hd44780_get_line(s->lcd, aLine, (uint8_t)aWidth, aOutBuf, (size_t)aOutBufSize);
}

int sim_enable_ds1302(sim_handle_t aSim)
{
    struct sim *s = (struct sim *)aSim;
    if (s->ds1302)
        return 0;
    s->ds1302 = ds1302_create(s->bus, &s->cpu, HC6800_ES_DS1302, s->clock_hz);
    return 0;
}

void sim_ds1302_set_time(sim_handle_t aSim, int aSeconds, int aMinutes, int aHours,
                          int aDate, int aMonth, int aWeekday, int aYear)
{
    struct sim *s = (struct sim *)aSim;
    if (!s->ds1302)
        return;
    ds1302_set_time(s->ds1302, aSeconds, aMinutes, aHours, aDate, aMonth, aWeekday, aYear);
}

int sim_ds1302_get_register(sim_handle_t aSim, int aRegister)
{
    struct sim *s = (struct sim *)aSim;
    if (!s->ds1302)
        return 0;
    return ds1302_get_register(s->ds1302, aRegister);
}

int sim_enable_xpt2046(sim_handle_t aSim)
{
    struct sim *s = (struct sim *)aSim;
    if (s->xpt2046)
        return 0;
    s->xpt2046 = xpt2046_create(s->bus, &s->cpu, HC6800_ES_XPT2046);
    return 0;
}

void sim_xpt2046_set_reading(sim_handle_t aSim, int aValue12Bit)
{
    struct sim *s = (struct sim *)aSim;
    if (!s->xpt2046)
        return;
    xpt2046_set_reading(s->xpt2046, (uint16_t)aValue12Bit);
}

void sim_xpt2046_set_channel_reading(sim_handle_t aSim, int aChannel, int aValue12Bit)
{
    struct sim *s = (struct sim *)aSim;
    if (!s->xpt2046)
        return;
    xpt2046_set_channel_reading(s->xpt2046, aChannel, (uint16_t)aValue12Bit);
}

int sim_xpt2046_get_last_channel(sim_handle_t aSim)
{
    struct sim *s = (struct sim *)aSim;
    if (!s->xpt2046)
        return -1;
    return xpt2046_get_last_channel(s->xpt2046);
}

int sim_uart_tx_count(sim_handle_t aSim)
{
    struct sim *s = (struct sim *)aSim;
    return s->total_tx_count;
}

int sim_uart_tx_byte(sim_handle_t aSim, int aIndex)
{
    // Note: the underlying core only retains the last 18 bytes
    // (sizeof(serial_out)) -- if sim_uart_tx_count() > 18, the earliest
    // bytes are already gone and aIndex is taken modulo 18 into what's
    // left, not "the actual Nth byte sent overall".
    struct sim *s = (struct sim *)aSim;
    int cap = (int)sizeof(s->cpu.serial_out);
    if (aIndex < 0 || aIndex >= s->total_tx_count)
        return -1;
    return (unsigned char)s->cpu.serial_out[aIndex % cap];
}

void sim_uart_inject_rx(sim_handle_t aSim, unsigned char aByte)
{
    struct sim *s = (struct sim *)aSim;
    s->cpu.mSFR[REG_SBUF] = aByte;
    s->cpu.mSFR[REG_SCON] |= SCONMASK_RI;
    if (s->cpu.mSFR[REG_IE] & IEMASK_ES)
        s->cpu.serial_interrupt_trigger = 1;
}
