/* Generic RC servo model: decodes a PWM input pulse into an angle.
 * Copyright 2025 Thomas Reidemeister, MIT License (see hc573.h for full text)
 *
 * servo.h
 *
 * Not part of any real HC6800-ES peripheral -- there's no servo header on
 * that board (see sim/README.md's "plugin vs board definition" note and
 * a project ported alongside this simulator's own demo repo). A project that drives a
 * servo picks its own pin and passes it to servo_create() explicitly; it
 * is never baked into a board catalog like sim/boards/hc6800_es.c.
 *
 * Measures the high-time of whatever pin it's told to watch and reports
 * it both as a raw microsecond pulse width and as a 0-180 degree angle
 * (a standard hobby-servo mapping, pulse_min_us..pulse_max_us -> 0..180),
 * clamped at the ends rather than extrapolated for an out-of-spec pulse.
 */
#ifndef SIM_DEVICES_SERVO_H
#define SIM_DEVICES_SERVO_H

#include "../bus.h"
#include "../pin.h"

typedef struct servo servo_t;

// aClockHz is the board oscillator (e.g. 12000000) -- ticks-per-second is
// aClockHz/12, same convention as ds1302_create()/hd44780_create() (see
// ds1302.c's own comment deriving this from hd44780.c's busy-timing
// constants). aPulseMinUs/aPulseMaxUs are typically 1000/2000 for a
// standard hobby servo's 0-180 degree range.
servo_t *servo_create(sim_bus_t *aBus, struct em8051 *aCPU, pin_t aPwmPin,
                       unsigned long aClockHz, uint16_t aPulseMinUs, uint16_t aPulseMaxUs);
void servo_destroy(servo_t *aDev);

// Call once per tick(), same convention as hd44780_step()/ds1302_step().
void servo_step(servo_t *aDev);

// Last fully-measured pulse width, in microseconds. 0 if no complete
// (rising-then-falling) pulse has been seen yet.
uint16_t servo_get_pulse_us(const servo_t *aDev);

// Last measured pulse converted to an angle, in tenths of a degree
// (0-1800) so a caller doesn't need floating point. Clamped to
// [0, 1800] even for an out-of-spec pulse width.
int servo_get_angle_decidegrees(const servo_t *aDev);

#endif // SIM_DEVICES_SERVO_H
