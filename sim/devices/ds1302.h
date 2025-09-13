/* DS1302 trickle-charge RTC model (3-wire CE/SCLK/I-O protocol)
 * Copyright 2025 Thomas Reidemeister, MIT License (see hc573.h for full text)
 *
 * ds1302.h
 *
 * Implements the 8 clock/calendar registers (seconds, minutes, hours,
 * date, month, day-of-week, year, write-protect) and burst mode
 * (command byte 0xBE/0xBF), LSB-first, command+data sampled on SCLK
 * rising edges and driven on falling edges, matching the DS1302
 * datasheet. RAM registers (command bytes 0xC0+) are not modeled -- none
 * of this project's demos use DS1302 RAM.
 */
#ifndef SIM_DEVICES_DS1302_H
#define SIM_DEVICES_DS1302_H

#include "../bus.h"
#include "../pin.h"

typedef struct
{
    pin_t ce;   // chip enable / reset, active high
    pin_t sclk; // serial clock
    pin_t io;   // bidirectional data line
} ds1302_pins_t;

typedef struct ds1302 ds1302_t;

// aClockHz is the CPU clock ds1302_step()'s real-time-in-simulated-time
// advancement is scaled against (see ds1302_step()).
ds1302_t *ds1302_create(sim_bus_t *aBus, struct em8051 *aCPU, ds1302_pins_t aPins, unsigned long aClockHz);
void ds1302_destroy(ds1302_t *aDev);

// Advance the free-running clock/calendar by one tick. A real DS1302 keeps
// time off its own 32.768kHz crystal, independent of the host MCU -- this
// approximates that by counting CPU ticks and rolling the register file
// forward one full BCD second (with minute/hour/date/month/year carry,
// including leap years) once a real second's worth of them, scaled
// against aClockHz (passed to ds1302_create()), has accumulated. Call once
// per tick(), same convention as hd44780_step().
void ds1302_step(ds1302_t *aDev);

// Stimulus: set the simulated real-time clock's current date/time. Values
// are plain decimal (e.g. aSeconds=45), not BCD -- converted internally.
// aYear is 0-99 (last two digits). aWeekday is 1-7, whatever convention
// the firmware under test expects (this model doesn't interpret it).
void ds1302_set_time(ds1302_t *aDev, int aSeconds, int aMinutes, int aHours,
                      int aDate, int aMonth, int aWeekday, int aYear);

// Query: read back one of the 8 clock/calendar registers as raw BCD, the
// same byte format the real chip returns (register 0=seconds .. 6=year,
// 7=write-protect). This is what the firmware under test actually
// received, useful for asserting a port-side capture matches what the
// model was told to present.
uint8_t ds1302_get_register(const ds1302_t *aDev, int aRegister);

#endif // SIM_DEVICES_DS1302_H
