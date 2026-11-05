/* capi.c - see capi.h
 * Copyright 2025 Thomas Reidemeister, MIT License (see devices/hc573.h)
 */
#include <stdlib.h>
#include <string.h>
#include "sim_private.h"
#include "pin.h"
#include "boards/hc6800_es.h"

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
    int board;

    // Board catalogs: sim/boards/hc6800_es.*, and "bare", a plain 8051
    // with no board at all, for plugins (plugin.h) to build a part on.
    if (!aBoardName)
        return NULL;
    if (strcmp(aBoardName, "hc6800_es") == 0)
        board = SIM_BOARD_HC6800_ES;
    else if (strcmp(aBoardName, "bare") == 0)
        board = SIM_BOARD_BARE;
    else
        return NULL;

    s = (struct sim *)calloc(1, sizeof(struct sim));
    s->board = board;
    s->cpu.mCodeMemMaxIdx = 65535;
    s->cpu.mCodeMem = (unsigned char *)calloc(65536, 1);
    s->cpu.mExtDataMaxIdx = 65535;
    s->cpu.mExtData = (unsigned char *)calloc(65536, 1);
    s->cpu.mUpperData = (unsigned char *)calloc(128, 1);
    s->cpu.except = on_exception;
    s->clock_hz = HC6800_ES_XTAL_HZ; // bare: the same 12 MHz default

    s->bus = bus_create(&s->cpu);
    bus_on_write(s->bus, REG_SBUF, on_sbuf_write, s);
    plugin_host_init(s);
    reset(&s->cpu, 1);
    return s;
}

void sim_close(sim_handle_t aSim)
{
    struct sim *s = (struct sim *)aSim;
    if (!s)
        return;
    plugin_destroy_all(s);
    if (s->digit_display)
        digit_display_destroy(s->digit_display);
    if (s->lcd)
        hd44780_destroy(s->lcd);
    if (s->ds1302)
        ds1302_destroy(s->ds1302);
    if (s->xpt2046)
        xpt2046_destroy(s->xpt2046);
    if (s->servo)
        servo_destroy(s->servo);
    if (s->enc28j60)
        enc28j60_destroy(s->enc28j60);
    if (s->fan)
        fan_destroy(s->fan);
    if (s->iap)
        iap_destroy(s->iap);
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
    sim_cpu_reset(s, 0);
    if (s->iap)
        iap_power_on(s->iap); // host reset = power cycle: ISPEN/SWBS clear

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

    plugin_reset_all(s);
    notify_all_pins(s);
}

void sim_post_tick(struct sim *s)
{
    if (s->lcd)
        hd44780_step(s->lcd);
    if (s->ds1302)
        ds1302_step(s->ds1302);
    if (s->servo)
        servo_step(s->servo);
    if (s->fan)
        fan_step(s->fan);
    if (s->iap)
        iap_post_tick(s->iap);
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

int sim_enable_servo(sim_handle_t aSim, int aPwmPortIndex, int aPwmBit,
                      int aPulseMinUs, int aPulseMaxUs)
{
    struct sim *s = (struct sim *)aSim;
    pin_t pwm;
    if (s->servo)
        return 0;
    if (aPwmPortIndex < 0 || aPwmPortIndex > 3 || aPwmBit < 0 || aPwmBit > 7)
        return -1;
    pwm = pin_make((uint8_t)(aPwmPortIndex * 0x10), (uint8_t)aPwmBit);
    s->servo = servo_create(s->bus, &s->cpu, pwm, s->clock_hz,
                             (uint16_t)aPulseMinUs, (uint16_t)aPulseMaxUs);
    return 0;
}

int sim_servo_get_pulse_us(sim_handle_t aSim)
{
    struct sim *s = (struct sim *)aSim;
    if (!s->servo)
        return 0;
    return servo_get_pulse_us(s->servo);
}

int sim_servo_get_angle_decidegrees(sim_handle_t aSim)
{
    struct sim *s = (struct sim *)aSim;
    if (!s->servo)
        return 0;
    return servo_get_angle_decidegrees(s->servo);
}

int sim_enable_enc28j60(sim_handle_t aSim, int aCsPort, int aCsBit,
                         int aSckPort, int aSckBit,
                         int aMosiPort, int aMosiBit,
                         int aMisoPort, int aMisoBit)
{
    struct sim *s = (struct sim *)aSim;
    enc28j60_pins_t pins;
    int ports[4] = {aCsPort, aSckPort, aMosiPort, aMisoPort};
    int bits[4] = {aCsBit, aSckBit, aMosiBit, aMisoBit};
    int i;
    if (s->enc28j60)
        return 0;
    for (i = 0; i < 4; i++)
        if (ports[i] < 0 || ports[i] > 3 || bits[i] < 0 || bits[i] > 7)
            return -1;
    pins.cs = pin_make((uint8_t)(aCsPort * 0x10), (uint8_t)aCsBit);
    pins.sck = pin_make((uint8_t)(aSckPort * 0x10), (uint8_t)aSckBit);
    pins.mosi = pin_make((uint8_t)(aMosiPort * 0x10), (uint8_t)aMosiBit);
    pins.miso = pin_make((uint8_t)(aMisoPort * 0x10), (uint8_t)aMisoBit);
    s->enc28j60 = enc28j60_create(s->bus, &s->cpu, pins);
    return 0;
}

int sim_enable_fan(sim_handle_t aSim, int aPwmPort, int aPwmBit,
                    int aTachPort, int aTachBit)
{
    struct sim *s = (struct sim *)aSim;
    pin_t pwm, tach;
    if (s->fan)
        return 0;
    if (aPwmPort < 0 || aPwmPort > 3 || aPwmBit < 0 || aPwmBit > 7 ||
        aTachPort < 0 || aTachPort > 3 || aTachBit < 0 || aTachBit > 7)
        return -1;
    pwm = pin_make((uint8_t)(aPwmPort * 0x10), (uint8_t)aPwmBit);
    tach = pin_make((uint8_t)(aTachPort * 0x10), (uint8_t)aTachBit);
    s->fan = fan_create(s->bus, &s->cpu, pwm, tach, s->clock_hz);
    return 0;
}

int sim_fan_get_duty_percent(sim_handle_t aSim)
{
    struct sim *s = (struct sim *)aSim;
    if (!s->fan)
        return 0;
    return fan_get_duty_percent(s->fan);
}

int sim_fan_get_rpm(sim_handle_t aSim)
{
    struct sim *s = (struct sim *)aSim;
    if (!s->fan)
        return 0;
    return fan_get_rpm(s->fan);
}

int sim_enc28j60_get_register(sim_handle_t aSim, int aBank, int aAddress)
{
    struct sim *s = (struct sim *)aSim;
    if (!s->enc28j60)
        return 0;
    return enc28j60_get_register(s->enc28j60, aBank, aAddress);
}

int sim_enc28j60_get_bank(sim_handle_t aSim)
{
    struct sim *s = (struct sim *)aSim;
    if (!s->enc28j60)
        return 0;
    return enc28j60_get_bank(s->enc28j60);
}

int sim_enc28j60_get_last_opcode(sim_handle_t aSim)
{
    struct sim *s = (struct sim *)aSim;
    if (!s->enc28j60)
        return 0;
    return enc28j60_get_last_opcode(s->enc28j60);
}

unsigned long sim_enc28j60_get_buffer_byte_count(sim_handle_t aSim)
{
    struct sim *s = (struct sim *)aSim;
    if (!s->enc28j60)
        return 0;
    return enc28j60_get_buffer_byte_count(s->enc28j60);
}

int sim_enc28j60_inject_rx(sim_handle_t aSim, const unsigned char *aFrame, int aLen)
{
    struct sim *s = (struct sim *)aSim;
    if (!s->enc28j60)
        return ENC28J60_RX_DISABLED;
    return enc28j60_inject_rx(s->enc28j60, aFrame, aLen);
}

int sim_enc28j60_poke_buffer(sim_handle_t aSim, int aAddress, int aValue)
{
    struct sim *s = (struct sim *)aSim;
    if (!s->enc28j60)
        return -1;
    enc28j60_poke_buffer(s->enc28j60, aAddress, aValue);
    return 0;
}

int sim_enc28j60_peek_buffer(sim_handle_t aSim, int aAddress)
{
    struct sim *s = (struct sim *)aSim;
    if (!s->enc28j60)
        return -1;
    return enc28j60_peek_buffer(s->enc28j60, aAddress);
}

int sim_enc28j60_tx_count(sim_handle_t aSim)
{
    struct sim *s = (struct sim *)aSim;
    if (!s->enc28j60)
        return 0;
    return enc28j60_tx_count(s->enc28j60);
}

int sim_enc28j60_tx_frame(sim_handle_t aSim, int aIndex, unsigned char *aOut, int aMax)
{
    struct sim *s = (struct sim *)aSim;
    if (!s->enc28j60)
        return -1;
    return enc28j60_tx_frame(s->enc28j60, aIndex, aOut, aMax);
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
    struct sim *s = (struct sim *)aSim;
    if (!s)
        return -1;
    if (!s->iap)
        s->iap = iap_create(s->bus, &s->cpu, (iap_profile_t)aProfile);
    return s->iap ? 0 : -1;
}

int sim_iap_peek(sim_handle_t aSim, int aAddress)
{
    return iap_peek(((struct sim *)aSim)->iap, aAddress);
}

int sim_iap_poke(sim_handle_t aSim, int aAddress, int aValue)
{
    return iap_poke(((struct sim *)aSim)->iap, aAddress, aValue);
}

long sim_iap_stat(sim_handle_t aSim, int aWhich)
{
    return iap_stat(((struct sim *)aSim)->iap, aWhich);
}

int sim_peek_code(sim_handle_t aSim, int aAddress)
{
    struct sim *s = (struct sim *)aSim;
    if (!s || aAddress < 0 || aAddress > s->cpu.mCodeMemMaxIdx)
        return -1;
    return s->cpu.mCodeMem[aAddress];
}
