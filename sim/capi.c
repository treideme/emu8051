/* capi.c - see capi.h
 * Copyright 2025 Thomas Reidemeister, MIT License (see devices/hc573.h)
 */
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "sim_private.h"
#include "pin.h"
#include "boards/hc6800_es_plugin.h"
#include "devices/enc28j60.h" // ENC28J60_RX_DISABLED

#define SIM_DEFAULT_CLOCK_HZ 12000000UL // the HC6800-ES crystal; "bare" starts at the same

// The boards sim_open() knows: a name, and the plugin that is the board,
// linked into this library (sim_add_plugin). "bare" is a plain 8051 with
// none, for plugins (plugin.h) to build a part on.
static const struct
{
    const char *name;
    emu8051_plugin_init_fn init;
} BOARDS[] = {
    {"hc6800_es", hc6800_es_plugin_init},
    {"bare", NULL},
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

void sim_record_exception(struct sim *s, int aCode)
{
    aCode = plugin_filter_exception(s, aCode);
    if (aCode >= 0 && s->exception_count < MAX_EXCEPTIONS)
        s->exceptions[s->exception_count++] = aCode;
}

static void on_exception(struct em8051 *aCPU, int aCode)
{
    sim_record_exception((struct sim *)aCPU, aCode);
}

// MOVX past the part's on-chip XRAM. On an STC89 with EXTRAM=0 such an access
// does not fault: it silently goes to the external bus, i.e. drives P0/P2 --
// on the HC6800-ES that is the ENC28J60's SPI lines and the LEDs. So it is
// recorded as a CPU exception rather than allowed to pass unnoticed.
// A plugin's xdata hooks (plugin.h) come first: their ranges are not RAM.
static uint8_t on_xread(struct em8051 *aCPU, uint16_t aAddress)
{
    struct sim *s = (struct sim *)aCPU;
    uint8_t v;
    if (s->xdata_hook_count && plugin_xdata_read(s, aAddress, &v))
        return v;
    if (s->xram_size && aAddress >= s->xram_size)
        on_exception(aCPU, SIM_EXCEPTION_XRAM_RANGE);
    return aCPU->mExtData[aAddress & aCPU->mExtDataMaxIdx];
}

static void on_xwrite(struct em8051 *aCPU, uint16_t aAddress, uint8_t aValue)
{
    struct sim *s = (struct sim *)aCPU;
    if (s->xdata_hook_count && plugin_xdata_write(s, aAddress, aValue))
        return;
    if (s->xram_size && aAddress >= s->xram_size)
        on_exception(aCPU, SIM_EXCEPTION_XRAM_RANGE);
    aCPU->mExtData[aAddress & aCPU->mExtDataMaxIdx] = aValue;
}

void sim_set_xram_size(sim_handle_t aSim, unsigned long aBytes)
{
    struct sim *s = (struct sim *)aSim;
    int hooked = aBytes || s->xdata_hook_count;
    s->xram_size = aBytes;
    s->cpu.xread = hooked ? on_xread : NULL;
    s->cpu.xwrite = hooked ? on_xwrite : NULL;
}

static void on_stim_read(struct em8051 *aCPU, uint8_t aReg, void *aUserData, uint8_t *aOutMask, uint8_t *aOutValue)
{
    struct sim *s = (struct sim *)aUserData;
    int i;
    (void)aCPU;

    for (i = 0; i < s->stim_count; i++)
    {
        if (!s->stim[i].active || s->stim[i].reg != aReg)
            continue;
        *aOutMask = (uint8_t)(*aOutMask | (1u << s->stim[i].bit));
        if (s->stim[i].value)
            *aOutValue = (uint8_t)(*aOutValue | (1u << s->stim[i].bit));
    }
}

void sim_cpu_reset(struct sim *s, int aWipe)
{
    reset(&s->cpu, aWipe != 0);
    plugin_apply_ops(s); // reset() rebuilt the opcode table
}

static void notify_all_pins(struct sim *s)
{
    int r;
    for (r = 0; r < 128; r++)
        if (s->pin_reg_watched[r])
            plugin_pins_changed(s, (uint8_t)r);
}

sim_handle_t sim_open(const char *aBoardName)
{
    struct sim *s;
    size_t board;

    if (!aBoardName)
        return NULL;
    for (board = 0; board < sizeof(BOARDS) / sizeof(BOARDS[0]); board++)
        if (strcmp(aBoardName, BOARDS[board].name) == 0)
            break;
    if (board == sizeof(BOARDS) / sizeof(BOARDS[0]))
        return NULL;

    s = (struct sim *)calloc(1, sizeof(struct sim));
    s->cpu.mCodeMemMaxIdx = 65535;
    s->cpu.mCodeMem = (unsigned char *)calloc(65536, 1);
    s->cpu.mExtDataMaxIdx = 65535;
    s->cpu.mExtData = (unsigned char *)calloc(65536, 1);
    s->cpu.mUpperData = (unsigned char *)calloc(128, 1);
    s->cpu.except = on_exception;
    s->clock_hz = SIM_DEFAULT_CLOCK_HZ;

    s->bus = bus_create(&s->cpu);
    if (!s->bus)
    {
        free(s->cpu.mCodeMem);
        free(s->cpu.mExtData);
        free(s->cpu.mUpperData);
        free(s);
        return NULL;
    }
    bus_on_write(s->bus, REG_SBUF, on_sbuf_write, s);
    plugin_host_init(s);
    reset(&s->cpu, 1);
    // The board, as the first plugin: nothing of it is attached until a
    // sim_enable_* call (or its method) asks for a device.
    if (BOARDS[board].init && sim_add_plugin(s, BOARDS[board].init, "") != SIM_PLUGIN_OK)
    {
        sim_close(s);
        return NULL;
    }
    return s;
}

void sim_close(sim_handle_t aSim)
{
    struct sim *s = (struct sim *)aSim;
    if (!s)
        return;
    plugin_destroy_all(s); // the board's devices with it
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

    // CPU first: the plugins' reset below (the board's digit display and
    // LCD among them) samples pin state, and needs the post-reset power-on
    // port state, not whatever was there a moment ago.
    sim_cpu_reset(s, 0);

    s->exception_count = 0;
    s->total_tx_count = 0;
    // The core's ring position survives reset(); byte 0 of the new run lands
    // wherever it points. Re-anchoring here (rather than assuming slot 0)
    // stops sim_uart_tx_byte() from returning pre-reset bytes, and stops
    // post_tick() counting a phantom byte because last_tx_idx != serial_out_idx.
    s->last_tx_idx = s->cpu.serial_out_idx;
    s->tx_origin = s->cpu.serial_out_idx;
    s->total_instructions = 0;
    s->total_ticks = 0;
    s->total_clocks = 0;
    s->device_clocks = 0;

    // The board's devices are the board plugin's to reset (what starts
    // over and what does not: capi.h, sim_reset). sim_set_pin stimulus is
    // left alone.
    plugin_reset_all(s);
    notify_all_pins(s);
}

void sim_post_tick(struct sim *s)
{
    int i;
    for (i = 0; i < s->tick_hook_count; i++)
        s->tick_hooks[i].fn(s->tick_hooks[i].user);
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
    long before = s->total_instructions;
    long i;

    if (s->instruction_mode)
    {
        // a tick is 12 clocks here too (plugin.h, "The two execution modes")
        uint64_t end = s->total_clocks + (uint64_t)(aTicks > 0 ? aTicks : 0) * 12u;
        while (s->total_clocks < end)
            plugin_step(s);
        return s->total_instructions - before;
    }
    if (s->step_hook_count)
    {
        for (i = 0; i < aTicks; i++)
            plugin_step(s);
        return s->total_instructions - before;
    }
    // no hooks: the plain loop, as fast as before plugins existed
    for (i = 0; i < aTicks; i++)
    {
        if (tick(&s->cpu))
            s->total_instructions++;
        sim_post_tick(s);
    }
    if (aTicks > 0)
    {
        s->total_ticks += (unsigned long)aTicks;
        s->total_clocks += 12u * (uint64_t)aTicks;
    }
    return s->total_instructions - before;
}

long sim_step_instructions(sim_handle_t aSim, long aInstructions)
{
    struct sim *s = (struct sim *)aSim;
    long before = s->total_instructions;
    long budget = aInstructions * 100L; // generous safety net, see capi.h

    if (s->instruction_mode || s->step_hook_count)
    {
        while (s->total_instructions - before < aInstructions && budget-- > 0)
            plugin_step(s);
        return s->total_instructions - before;
    }
    while (s->total_instructions - before < aInstructions && budget-- > 0)
    {
        if (tick(&s->cpu))
            s->total_instructions++;
        sim_post_tick(s);
        s->total_ticks++;
        s->total_clocks += 12u;
    }
    return s->total_instructions - before;
}

long sim_get_instruction_count(sim_handle_t aSim)
{
    return ((struct sim *)aSim)->total_instructions;
}

unsigned long sim_get_tick_count(sim_handle_t aSim)
{
    struct sim *s = (struct sim *)aSim;
    if (s->instruction_mode)
        return (unsigned long)(s->total_clocks / 12u);
    return s->total_ticks;
}

int sim_get_pc(sim_handle_t aSim)
{
    struct sim *s = (struct sim *)aSim;
    return s->cpu.mPC;
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

int sim_pin_level(struct sim *s, uint8_t aReg, uint8_t aBit)
{
    int i;
    for (i = 0; i < s->stim_count; i++)
        if (s->stim[i].active && s->stim[i].reg == aReg && s->stim[i].bit == aBit)
            return s->stim[i].value;
    return (s->cpu.mSFR[aReg & 0x7F] >> (aBit & 7)) & 1;
}

void sim_drive_pin(struct sim *s, uint8_t aReg, uint8_t aBit, int aValue)
{
    int i;

    aReg &= 0x7F;
    for (i = 0; i < s->stim_count; i++)
    {
        if (s->stim[i].reg == aReg && s->stim[i].bit == aBit)
        {
            s->stim[i].active = (aValue >= 0);
            s->stim[i].value = (aValue > 0);
            plugin_pins_changed(s, aReg);
            return;
        }
    }
    if (aValue < 0 || s->stim_count >= MAX_STIM)
        return;

    s->stim[s->stim_count].reg = aReg;
    s->stim[s->stim_count].bit = aBit;
    s->stim[s->stim_count].active = 1;
    s->stim[s->stim_count].value = (aValue > 0);
    s->stim_count++;

    if (!s->stim_reg_registered[aReg])
    {
        bus_on_read(s->bus, aReg, on_stim_read, s);
        s->stim_reg_registered[aReg] = 1;
    }
    plugin_pins_changed(s, aReg);
}

void sim_set_pin(sim_handle_t aSim, int aPortIndex, int aBit, int aValue)
{
    if (aPortIndex < 0 || aPortIndex > 3 || aBit < 0 || aBit > 7)
        return;
    sim_drive_pin((struct sim *)aSim, (uint8_t)(aPortIndex * 0x10), (uint8_t)aBit, aValue);
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

// ------------------------------------------------------------------ the board's devices
//
// Everything from here to the UART is the board plugin's
// (sim/boards/hc6800_es_plugin.h): each function calls the plugin method
// of its own name through sim_plugin_call(), as any binding could.
// sim_open("hc6800_es") adds the plugin; on any other simulation the first
// of these calls adds it, so they work on "bare" as they always have. A
// plugin of that name loaded from a shared object (sim_load_plugin)
// answers them in its place.

static void put32(unsigned char *aOut, int aValue)
{
    uint32_t v = (uint32_t)aValue;
    aOut[0] = (unsigned char)v;
    aOut[1] = (unsigned char)(v >> 8);
    aOut[2] = (unsigned char)(v >> 16);
    aOut[3] = (unsigned char)(v >> 24);
}

static int get32(const unsigned char *aIn)
{
    return (int)((uint32_t)aIn[0] | (uint32_t)aIn[1] << 8 | (uint32_t)aIn[2] << 16 | (uint32_t)aIn[3] << 24);
}

static int board_call(sim_handle_t aSim, const char *aMethod, const unsigned char *aIn, int aInLen,
                      unsigned char *aOut, int aOutCap, int *aOutLen)
{
    int rc;
    *aOutLen = 0;
    if (!aSim)
        return EMU8051_CALL_NO_PLUGIN;
    rc = sim_plugin_call(aSim, HC6800_ES_PLUGIN_NAME, aMethod, aIn, aInLen, aOut, aOutCap, aOutLen);
    if (rc == EMU8051_CALL_NO_PLUGIN && sim_add_plugin(aSim, hc6800_es_plugin_init, "") == SIM_PLUGIN_OK)
        rc = sim_plugin_call(aSim, HC6800_ES_PLUGIN_NAME, aMethod, aIn, aInLen, aOut, aOutCap, aOutLen);
    return rc;
}

// A method with aCount int arguments (the variadic ones) and an int
// result; aFail when there is no answer.
static int board_int(sim_handle_t aSim, const char *aMethod, int aFail, int aCount, ...)
{
    unsigned char in[4 * 8], out[4];
    int i, n;
    va_list ap;
    va_start(ap, aCount);
    for (i = 0; i < aCount; i++)
        put32(in + 4 * i, va_arg(ap, int));
    va_end(ap);
    if (board_call(aSim, aMethod, in, 4 * aCount, out, (int)sizeof(out), &n) != EMU8051_CALL_OK || n != 4)
        return aFail;
    return get32(out);
}

// A method with aCount int arguments and no result: 0, or -1.
static int board_do(sim_handle_t aSim, const char *aMethod, int aCount, ...)
{
    unsigned char in[4 * 8];
    int i, n;
    va_list ap;
    va_start(ap, aCount);
    for (i = 0; i < aCount; i++)
        put32(in + 4 * i, va_arg(ap, int));
    va_end(ap);
    return board_call(aSim, aMethod, in, 4 * aCount, NULL, 0, &n) == EMU8051_CALL_OK ? 0 : -1;
}

int sim_enable_digit_display(sim_handle_t aSim, int aDigitCount)
{
    return board_do(aSim, "enable_digit_display", 1, aDigitCount);
}

int sim_digit_get_segments(sim_handle_t aSim, int aDigit)
{
    return board_int(aSim, "digit_get_segments", 0, 1, aDigit);
}

int sim_digit_get_char(sim_handle_t aSim, int aDigit)
{
    return board_int(aSim, "digit_get_char", '?', 1, aDigit); // '?': what no segments decode to
}

int sim_enable_lcd(sim_handle_t aSim)
{
    return board_do(aSim, "enable_lcd", 0);
}

void sim_lcd_get_line(sim_handle_t aSim, int aLine, int aWidth, char *aOutBuf, int aOutBufSize)
{
    unsigned char in[8];
    int n = 0;
    if (aOutBufSize < 1)
        return;
    put32(in, aLine);
    put32(in + 4, aWidth);
    if (board_call(aSim, "lcd_get_line", in, 8, (unsigned char *)aOutBuf, aOutBufSize - 1, &n) != EMU8051_CALL_OK ||
        n < 0 || n > aOutBufSize - 1)
        n = 0;
    aOutBuf[n] = '\0';
}

int sim_enable_ds1302(sim_handle_t aSim)
{
    return board_do(aSim, "enable_ds1302", 0);
}

void sim_ds1302_set_time(sim_handle_t aSim, int aSeconds, int aMinutes, int aHours,
                          int aDate, int aMonth, int aWeekday, int aYear)
{
    board_do(aSim, "ds1302_set_time", 7, aSeconds, aMinutes, aHours, aDate, aMonth, aWeekday, aYear);
}

int sim_ds1302_get_register(sim_handle_t aSim, int aRegister)
{
    return board_int(aSim, "ds1302_get_register", 0, 1, aRegister);
}

int sim_enable_xpt2046(sim_handle_t aSim)
{
    return board_do(aSim, "enable_xpt2046", 0);
}

void sim_xpt2046_set_reading(sim_handle_t aSim, int aValue12Bit)
{
    board_do(aSim, "xpt2046_set_reading", 1, aValue12Bit);
}

void sim_xpt2046_set_channel_reading(sim_handle_t aSim, int aChannel, int aValue12Bit)
{
    board_do(aSim, "xpt2046_set_channel_reading", 2, aChannel, aValue12Bit);
}

int sim_xpt2046_get_last_channel(sim_handle_t aSim)
{
    return board_int(aSim, "xpt2046_get_last_channel", -1, 0);
}

int sim_enable_servo(sim_handle_t aSim, int aPwmPortIndex, int aPwmBit,
                      int aPulseMinUs, int aPulseMaxUs)
{
    return board_do(aSim, "enable_servo", 4, aPwmPortIndex, aPwmBit, aPulseMinUs, aPulseMaxUs);
}

int sim_servo_get_pulse_us(sim_handle_t aSim)
{
    return board_int(aSim, "servo_get_pulse_us", 0, 0);
}

int sim_servo_get_angle_decidegrees(sim_handle_t aSim)
{
    return board_int(aSim, "servo_get_angle_decidegrees", 0, 0);
}

int sim_enable_enc28j60(sim_handle_t aSim, int aCsPort, int aCsBit,
                         int aSckPort, int aSckBit,
                         int aMosiPort, int aMosiBit,
                         int aMisoPort, int aMisoBit)
{
    return board_do(aSim, "enable_enc28j60", 8, aCsPort, aCsBit, aSckPort, aSckBit, aMosiPort, aMosiBit, aMisoPort,
                    aMisoBit);
}

int sim_enable_fan(sim_handle_t aSim, int aPwmPort, int aPwmBit,
                    int aTachPort, int aTachBit)
{
    return board_do(aSim, "enable_fan", 4, aPwmPort, aPwmBit, aTachPort, aTachBit);
}

int sim_fan_get_duty_percent(sim_handle_t aSim)
{
    return board_int(aSim, "fan_get_duty_percent", 0, 0);
}

int sim_fan_get_rpm(sim_handle_t aSim)
{
    return board_int(aSim, "fan_get_rpm", 0, 0);
}

int sim_enc28j60_get_register(sim_handle_t aSim, int aBank, int aAddress)
{
    return board_int(aSim, "enc28j60_get_register", 0, 2, aBank, aAddress);
}

int sim_enc28j60_get_bank(sim_handle_t aSim)
{
    return board_int(aSim, "enc28j60_get_bank", 0, 0);
}

int sim_enc28j60_get_last_opcode(sim_handle_t aSim)
{
    return board_int(aSim, "enc28j60_get_last_opcode", 0, 0);
}

unsigned long sim_enc28j60_get_buffer_byte_count(sim_handle_t aSim)
{
    return (unsigned long)(uint32_t)board_int(aSim, "enc28j60_get_buffer_byte_count", 0, 0);
}

int sim_enc28j60_inject_rx(sim_handle_t aSim, const unsigned char *aFrame, int aLen)
{
    unsigned char out[4];
    int n;
    if (aLen < 0)
        aLen = 0; // the model refuses it as too short, as it did a negative length
    if (board_call(aSim, "enc28j60_inject_rx", aFrame, aLen, out, (int)sizeof(out), &n) != EMU8051_CALL_OK || n != 4)
        return ENC28J60_RX_DISABLED;
    return get32(out);
}

int sim_enc28j60_poke_buffer(sim_handle_t aSim, int aAddress, int aValue)
{
    return board_do(aSim, "enc28j60_poke_buffer", 2, aAddress, aValue);
}

int sim_enc28j60_peek_buffer(sim_handle_t aSim, int aAddress)
{
    return board_int(aSim, "enc28j60_peek_buffer", -1, 1, aAddress);
}

int sim_enc28j60_tx_count(sim_handle_t aSim)
{
    return board_int(aSim, "enc28j60_tx_count", 0, 0);
}

int sim_enc28j60_tx_frame(sim_handle_t aSim, int aIndex, unsigned char *aOut, int aMax)
{
    unsigned char in[4];
    int n, rc;
    put32(in, aIndex);
    rc = board_call(aSim, "enc28j60_tx_frame", in, 4, aOut, aMax, &n);
    // too small: aOut holds what fitted, n is the frame's length
    return (rc == EMU8051_CALL_OK || rc == EMU8051_CALL_TOO_SMALL) ? n : -1;
}

// ------------------------------------------------------------------ UART

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
    return (unsigned char)s->cpu.serial_out[(s->tx_origin + aIndex) % cap];
}

void sim_uart_inject_rx(sim_handle_t aSim, unsigned char aByte)
{
    struct sim *s = (struct sim *)aSim;
    s->cpu.mSFR[REG_SBUF] = aByte;
    s->cpu.mSFR[REG_SCON] |= SCONMASK_RI;
    if (s->cpu.mSFR[REG_IE] & IEMASK_ES)
        s->cpu.serial_interrupt_trigger = 1;
}

int sim_enable_iap(sim_handle_t aSim, int aProfile)
{
    return board_do(aSim, "enable_iap", 1, aProfile);
}

int sim_iap_peek(sim_handle_t aSim, int aAddress)
{
    return board_int(aSim, "iap_peek", -1, 1, aAddress);
}

int sim_iap_poke(sim_handle_t aSim, int aAddress, int aValue)
{
    return board_do(aSim, "iap_poke", 2, aAddress, aValue);
}

long sim_iap_stat(sim_handle_t aSim, int aWhich)
{
    return board_int(aSim, "iap_stat", 0, 1, aWhich);
}

int sim_peek_code(sim_handle_t aSim, int aAddress)
{
    struct sim *s = (struct sim *)aSim;
    if (!s || aAddress < 0 || aAddress > s->cpu.mCodeMemMaxIdx)
        return -1;
    return s->cpu.mCodeMem[aAddress];
}
