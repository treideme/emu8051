/* Generic 4-wire PC-style PWM fan model: decodes a PWM duty cycle and
 * drives a tachometer pulse train back, both scaled against simulated
 * elapsed time.
 * Copyright 2025 Thomas Reidemeister, MIT License (see hc573.h for full text)
 *
 * fan.h
 *
 * Not part of any real HC6800-ES peripheral -- there's no fan header on
 * that board (see sim/README.md's "plugin vs board definition" note). A
 * project that drives a fan picks its own pins and passes them to
 * fan_create() explicitly; it is never baked into a board catalog like
 * sim/boards/hc6800_es.c.
 *
 * Modeled on a standard 40mm 5V 4-pin PWM fan (GND, +5V, tach, PWM) --
 * see the ported demo's own header for the specific part this was
 * grounded against. Two independent halves:
 *
 * - PWM decode (MCU -> fan): measures the last full PWM cycle on the
 *   watched pin and reports it as a 0-100% duty cycle. A real 4-wire
 *   fan's PWM input is conventionally ~25kHz; this model doesn't care
 *   what frequency the firmware actually drives (bit-banged software PWM
 *   from an 8051 won't hit 25kHz, and doesn't need to for this to work),
 *   it just measures whatever period is presented.
 *
 * - Tachometer generation (fan -> MCU): drives the tach pin with a pulse
 *   train at FAN_PULSES_PER_REV (2, the near-universal convention for a
 *   2-pole 4-wire fan's open-collector tach output) pulses per simulated
 *   revolution, at a simulated RPM that responds to the current duty
 *   cycle via a simple linear mapping between FAN_MIN_START_DUTY_PERCENT
 *   (below which the model assumes the fan doesn't spin at all, matching
 *   real fans' typical minimum-start-duty behavior) and FAN_MAX_RPM. This
 *   is a simplification, not a real fan curve: no spin-up/spin-down
 *   inertia, no stall detection beyond the fixed start-duty floor, RPM
 *   responds to a duty-cycle change instantly.
 */
#ifndef SIM_DEVICES_FAN_H
#define SIM_DEVICES_FAN_H

#include "../bus.h"
#include "../pin.h"

typedef struct fan fan_t;

// aClockHz is the board oscillator (e.g. 12000000) -- ticks-per-second is
// aClockHz/12, same convention as servo_create()/ds1302_create() (see
// ds1302.c's own comment deriving this from hd44780.c's busy-timing
// constants).
fan_t *fan_create(sim_bus_t *aBus, struct em8051 *aCPU, pin_t aPwmPin, pin_t aTachPin,
                   unsigned long aClockHz);
void fan_destroy(fan_t *aDev);

// Call once per tick(), same convention as servo_step()/ds1302_step().
void fan_step(fan_t *aDev);

// Last fully-measured PWM duty cycle, 0-100. 0 if no complete cycle
// (rising-high-falling-low-rising) has been seen yet.
int fan_get_duty_percent(const fan_t *aDev);

// Current simulated RPM, derived from the last measured duty cycle (see
// this header's own note on the mapping and its limitations).
int fan_get_rpm(const fan_t *aDev);

#endif // SIM_DEVICES_FAN_H
