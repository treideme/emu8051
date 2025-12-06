/* fan.c - see fan.h
 * Copyright 2025 Thomas Reidemeister, MIT License (see hc573.h)
 */
#include <stdlib.h>
#include "fan.h"

#define FAN_PULSES_PER_REV 2       // standard 2-pole 4-wire fan tach convention
#define FAN_MAX_RPM 8000           // assumed ceiling for a small 40mm 5V fan
#define FAN_MIN_START_DUTY_PERCENT 20 // below this, the model assumes stall/no-spin

struct fan
{
    pin_t pwm_pin;
    pin_t tach_pin;
    unsigned long ticks_per_second; // aClockHz/12, see fan.h

    // PWM decode side
    int pwm_level;
    unsigned long ticks_since_rising;
    unsigned long last_period_ticks;
    unsigned long last_high_ticks;

    // Tach generation side
    int current_rpm;
    unsigned long tach_accum;
    int tach_level;
};

static void on_pwm_write(struct em8051 *aCPU, uint8_t aReg, uint8_t aValue, void *aUserData)
{
    fan_t *dev = (fan_t *)aUserData;
    int level = pin_get(aCPU, dev->pwm_pin);
    (void)aReg;
    (void)aValue;

    if (level && !dev->pwm_level)
    {
        // Rising edge: ticks_since_rising has been accumulating since the
        // *previous* rising edge (it's only ever reset here), so its
        // value right now is exactly last cycle's full period.
        if (dev->ticks_since_rising > 0)
            dev->last_period_ticks = dev->ticks_since_rising;
        dev->ticks_since_rising = 0;
    }
    else if (!level && dev->pwm_level)
    {
        dev->last_high_ticks = dev->ticks_since_rising;
    }
    dev->pwm_level = level;
}

static void on_tach_read(struct em8051 *aCPU, uint8_t aReg, void *aUserData, uint8_t *aOutMask, uint8_t *aOutValue)
{
    fan_t *dev = (fan_t *)aUserData;
    (void)aCPU;
    (void)aReg;

    *aOutMask = (uint8_t)(*aOutMask | pin_mask(dev->tach_pin));
    if (dev->tach_level)
        *aOutValue = (uint8_t)(*aOutValue | pin_mask(dev->tach_pin));
}

fan_t *fan_create(sim_bus_t *aBus, struct em8051 *aCPU, pin_t aPwmPin, pin_t aTachPin,
                   unsigned long aClockHz)
{
    fan_t *dev = (fan_t *)calloc(1, sizeof(fan_t));
    dev->pwm_pin = aPwmPin;
    dev->tach_pin = aTachPin;
    dev->ticks_per_second = aClockHz / 12;
    // Same reasoning as servo_create()/hc573_create(): read the pin's
    // actual level at creation time rather than assuming 0/low, so a PWM
    // signal already mid-high-phase isn't misread as a spurious edge.
    dev->pwm_level = pin_get(aCPU, aPwmPin);

    bus_on_write(aBus, aPwmPin.reg, on_pwm_write, dev);
    bus_on_read(aBus, aTachPin.reg, on_tach_read, dev);
    return dev;
}

void fan_destroy(fan_t *aDev)
{
    free(aDev);
}

int fan_get_duty_percent(const fan_t *aDev)
{
    if (aDev->last_period_ticks == 0)
        return 0;
    return (int)(aDev->last_high_ticks * 100UL / aDev->last_period_ticks);
}

int fan_get_rpm(const fan_t *aDev)
{
    return aDev->current_rpm;
}

static int compute_target_rpm(const fan_t *aDev)
{
    int duty = fan_get_duty_percent(aDev);
    if (duty < FAN_MIN_START_DUTY_PERCENT)
        return 0;
    return (int)((long)FAN_MAX_RPM * (duty - FAN_MIN_START_DUTY_PERCENT) /
                  (100 - FAN_MIN_START_DUTY_PERCENT));
}

void fan_step(fan_t *aDev)
{
    unsigned long half_period_ticks;

    aDev->ticks_since_rising++; // PWM period measurement, see on_pwm_write()

    aDev->current_rpm = compute_target_rpm(aDev);
    if (aDev->current_rpm <= 0 || aDev->ticks_per_second == 0)
    {
        aDev->tach_accum = 0; // stalled: stop toggling, leave tach line as-is
        return;
    }

    // pulses/sec = rpm/60 * FAN_PULSES_PER_REV; a full tach square wave is
    // two toggles per pulse, so the half-period is ticks_per_second
    // divided by twice that pulse rate.
    half_period_ticks = aDev->ticks_per_second * 60UL /
                         ((unsigned long)aDev->current_rpm * FAN_PULSES_PER_REV * 2UL);
    if (half_period_ticks == 0)
        half_period_ticks = 1;

    if (++aDev->tach_accum >= half_period_ticks)
    {
        aDev->tach_accum = 0;
        aDev->tach_level = !aDev->tach_level;
    }
}
