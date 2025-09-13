/* ds1302.c - see ds1302.h
 * Copyright 2025 Thomas Reidemeister, MIT License (see hc573.h)
 */
#include <stdlib.h>
#include <string.h>
#include "ds1302.h"

enum
{
    PHASE_IDLE = 0,
    PHASE_CMD,
    PHASE_WRITE,
    PHASE_READ
};

struct ds1302
{
    ds1302_pins_t pins;
    uint8_t regs[8]; // seconds,minutes,hours,date,month,day,year,wp (raw BCD as applicable)

    int last_ce;
    int last_sclk;
    int phase;
    int bitcount;
    uint8_t cur_byte;
    int addr_field; // 0-7 single register, 0x1F burst
    int burst_index;
    uint8_t data_byte;
    int read_bit;
    int skip_next_falling; // see decode_command()'s PHASE_READ transition

    unsigned long clock_hz;
    unsigned long tick_accum; // see ds1302_step()
};

static uint8_t to_bcd(int aValue)
{
    return (uint8_t)(((aValue / 10) << 4) | (aValue % 10));
}

static int from_bcd(uint8_t aValue)
{
    return ((aValue >> 4) & 0xf) * 10 + (aValue & 0xf);
}

static int is_leap_year(int aYear /* 0-99, last two digits */)
{
    return (aYear % 4) == 0; // good enough for a 2-digit-year teaching chip
}

static int days_in_month(int aMonth, int aYear)
{
    static const int dim[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (aMonth == 2 && is_leap_year(aYear))
        return 29;
    if (aMonth < 1 || aMonth > 12)
        return 31;
    return dim[aMonth - 1];
}

// One BCD second's worth of carry, cascading into minutes/hours/date
// (with days-in-month/leap-year awareness)/month/year, plus the
// once-a-day weekday increment. dev->regs[7] (write-protect) is untouched.
static void advance_one_second(ds1302_t *dev)
{
    int sec = from_bcd(dev->regs[0]) + 1;
    int min = from_bcd(dev->regs[1]);
    int hour = from_bcd(dev->regs[2]);
    int date = from_bcd(dev->regs[3]);
    int month = from_bcd(dev->regs[4]);
    int weekday = from_bcd(dev->regs[5]);
    int year = from_bcd(dev->regs[6]);
    int day_rolled = 0;

    if (sec >= 60)
    {
        sec -= 60;
        min++;
    }
    if (min >= 60)
    {
        min -= 60;
        hour++;
    }
    if (hour >= 24)
    {
        hour -= 24;
        date++;
        day_rolled = 1;
    }
    if (day_rolled)
    {
        weekday++;
        if (weekday > 7)
            weekday = 1;

        if (date > days_in_month(month, year))
        {
            date = 1;
            month++;
            if (month > 12)
            {
                month = 1;
                year = (year + 1) % 100;
            }
        }
    }

    dev->regs[0] = to_bcd(sec);
    dev->regs[1] = to_bcd(min);
    dev->regs[2] = to_bcd(hour);
    dev->regs[3] = to_bcd(date);
    dev->regs[4] = to_bcd(month);
    dev->regs[5] = to_bcd(weekday);
    dev->regs[6] = to_bcd(year);
}

void ds1302_step(ds1302_t *aDev)
{
    if (aDev->clock_hz == 0)
        return;
    if (++aDev->tick_accum < aDev->clock_hz)
        return;
    aDev->tick_accum = 0; // deliberately dropped, not carried -- a fractional
                           // leftover tick is well under measurement noise
                           // for what this model is used for
    advance_one_second(aDev);
}

void ds1302_set_time(ds1302_t *aDev, int aSeconds, int aMinutes, int aHours,
                      int aDate, int aMonth, int aWeekday, int aYear)
{
    aDev->regs[0] = to_bcd(aSeconds);
    aDev->regs[1] = to_bcd(aMinutes);
    aDev->regs[2] = to_bcd(aHours);
    aDev->regs[3] = to_bcd(aDate);
    aDev->regs[4] = to_bcd(aMonth);
    aDev->regs[5] = to_bcd(aWeekday);
    aDev->regs[6] = to_bcd(aYear);
}

uint8_t ds1302_get_register(const ds1302_t *aDev, int aRegister)
{
    if (aRegister < 0 || aRegister > 7)
        return 0;
    return aDev->regs[aRegister];
}

static void begin_command_phase(ds1302_t *dev)
{
    dev->phase = PHASE_CMD;
    dev->bitcount = 0;
    dev->cur_byte = 0;
}

static void on_ce_write(struct em8051 *aCPU, uint8_t aReg, uint8_t aValue, void *aUserData)
{
    ds1302_t *dev = (ds1302_t *)aUserData;
    int ce = pin_get(aCPU, dev->pins.ce);
    (void)aReg;
    (void)aValue;

    if (ce && !dev->last_ce)
        begin_command_phase(dev);
    if (!ce && dev->last_ce)
        dev->phase = PHASE_IDLE;

    dev->last_ce = ce;
}

static void decode_command(ds1302_t *dev)
{
    int rw = dev->cur_byte & 1;
    int addr_field = (dev->cur_byte >> 1) & 0x1f;

    dev->addr_field = addr_field;
    dev->burst_index = 0;

    if (rw)
    {
        int idx = (addr_field == 0x1f) ? 0 : addr_field;
        dev->phase = PHASE_READ;
        dev->bitcount = 0;
        dev->data_byte = (idx < 8) ? dev->regs[idx] : 0;
        dev->read_bit = dev->data_byte & 1;
        // The command byte's own 8th bit is still completing its clock
        // pulse: this rising edge just decoded the command, but the
        // falling edge that finishes *this same* pulse comes next, before
        // the firmware ever samples bit 0. Without this, that leftover
        // falling edge gets misread as the first *data* bit's falling
        // edge and bit 0 is skipped entirely (found by tracing a real
        // read against stc89c52-staging's 3432_clock_digit_tube_2 -- knew
        // the register held DisplayData correctly since the write-side
        // core bug fix, but the firmware still decoded garbage on read).
        dev->skip_next_falling = 1;
    }
    else
    {
        dev->phase = PHASE_WRITE;
        dev->bitcount = 0;
        dev->cur_byte = 0;
    }
}

static void on_sclk_write(struct em8051 *aCPU, uint8_t aReg, uint8_t aValue, void *aUserData)
{
    ds1302_t *dev = (ds1302_t *)aUserData;
    int sclk = pin_get(aCPU, dev->pins.sclk);
    int rising = sclk && !dev->last_sclk;
    int falling = !sclk && dev->last_sclk;
    (void)aReg;
    (void)aValue;

    if (!dev->last_ce)
    {
        dev->last_sclk = sclk;
        return; // CE not asserted, ignore clock entirely
    }

    if (rising && (dev->phase == PHASE_CMD || dev->phase == PHASE_WRITE))
    {
        int bit = pin_get(aCPU, dev->pins.io);
        if (bit)
            dev->cur_byte = (uint8_t)(dev->cur_byte | (1u << dev->bitcount));
        dev->bitcount++;

        if (dev->bitcount == 8)
        {
            if (dev->phase == PHASE_CMD)
            {
                decode_command(dev);
            }
            else // finished a WRITE data byte
            {
                int idx = (dev->addr_field == 0x1f) ? dev->burst_index : dev->addr_field;
                if (idx >= 0 && idx < 8)
                    dev->regs[idx] = dev->cur_byte;

                if (dev->addr_field == 0x1f && dev->burst_index < 7)
                {
                    dev->burst_index++;
                    dev->bitcount = 0;
                    dev->cur_byte = 0;
                }
                // else: transaction's data phase is complete; further
                // clocks before the next CE low->high are ignored (real
                // firmware always toggles CE between transactions).
            }
        }
    }
    else if (falling && dev->phase == PHASE_READ)
    {
        if (dev->skip_next_falling)
        {
            dev->skip_next_falling = 0;
        }
        else if (dev->bitcount < 7)
        {
            dev->bitcount++;
            dev->read_bit = (dev->data_byte >> dev->bitcount) & 1;
        }
        else if (dev->addr_field == 0x1f && dev->burst_index < 7)
        {
            dev->burst_index++;
            dev->data_byte = dev->regs[dev->burst_index];
            dev->bitcount = 0;
            dev->read_bit = dev->data_byte & 1;
        }
    }

    dev->last_sclk = sclk;
}

static void on_io_read(struct em8051 *aCPU, uint8_t aReg, void *aUserData, uint8_t *aOutMask, uint8_t *aOutValue)
{
    ds1302_t *dev = (ds1302_t *)aUserData;
    (void)aCPU;
    (void)aReg;

    if (dev->phase != PHASE_READ)
        return; // not driving; leave the bit as whatever the MCU last wrote

    *aOutMask = (uint8_t)(*aOutMask | pin_mask(dev->pins.io));
    if (dev->read_bit)
        *aOutValue = (uint8_t)(*aOutValue | pin_mask(dev->pins.io));
}

ds1302_t *ds1302_create(sim_bus_t *aBus, struct em8051 *aCPU, ds1302_pins_t aPins, unsigned long aClockHz)
{
    ds1302_t *dev = (ds1302_t *)calloc(1, sizeof(ds1302_t));
    dev->pins = aPins;
    // tick() (core.c) advances one machine cycle per call, i.e. 12
    // oscillator periods on a classic 8051 -- cross-checked against
    // hd44780.c's own busy-timing constants (e.g. its "2ms" comment only
    // holds if 1 tick = 1us at a 12MHz aClockHz). So ticks-per-real-second
    // is aClockHz/12, not aClockHz itself.
    dev->clock_hz = aClockHz / 12;

    // The bus dispatches to every subscriber of a register regardless of
    // how many there are, so this works whether or not CE/SCLK/IO happen
    // to share a register on a given board -- each callback only looks
    // at its own pin's bit.
    bus_on_write(aBus, aPins.ce.reg, on_ce_write, dev);
    bus_on_write(aBus, aPins.sclk.reg, on_sclk_write, dev);
    bus_on_read(aBus, aPins.io.reg, on_io_read, dev);

    (void)aCPU; // kept for signature consistency with the other device _create()s
    return dev;
}

void ds1302_destroy(ds1302_t *aDev)
{
    free(aDev);
}
