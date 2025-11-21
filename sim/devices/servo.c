/* servo.c - see servo.h
 * Copyright 2025 Thomas Reidemeister, MIT License (see hc573.h)
 */
#include <stdlib.h>
#include "servo.h"

struct servo
{
    pin_t pwm_pin;
    unsigned long ticks_per_second; // aClockHz/12, see servo.h
    uint16_t pulse_min_us;
    uint16_t pulse_max_us;

    int last_level;
    unsigned long ticks_since_rising;
    uint16_t last_pulse_us;
};

static uint16_t ticks_to_us(const servo_t *aDev, unsigned long aTicks)
{
    if (aDev->ticks_per_second == 0)
        return 0;
    return (uint16_t)(aTicks * 1000000UL / aDev->ticks_per_second);
}

static void on_pwm_write(struct em8051 *aCPU, uint8_t aReg, uint8_t aValue, void *aUserData)
{
    servo_t *dev = (servo_t *)aUserData;
    int level = pin_get(aCPU, dev->pwm_pin);
    (void)aReg;
    (void)aValue;

    if (level && !dev->last_level)
    {
        dev->ticks_since_rising = 0; // rising edge: start timing the pulse
    }
    else if (!level && dev->last_level)
    {
        // falling edge: ticks_since_rising has been accumulating via
        // servo_step() calls since the rising edge above
        dev->last_pulse_us = ticks_to_us(dev, dev->ticks_since_rising);
    }
    dev->last_level = level;
}

servo_t *servo_create(sim_bus_t *aBus, struct em8051 *aCPU, pin_t aPwmPin,
                       unsigned long aClockHz, uint16_t aPulseMinUs, uint16_t aPulseMaxUs)
{
    servo_t *dev = (servo_t *)calloc(1, sizeof(servo_t));
    dev->pwm_pin = aPwmPin;
    dev->ticks_per_second = aClockHz / 12;
    dev->pulse_min_us = aPulseMinUs;
    dev->pulse_max_us = aPulseMaxUs;
    // Read the pin's actual level at creation time rather than assuming
    // 0/low -- the same reasoning as hc573_create()'s own fix: a PWM
    // signal that happens to be mid-high-phase when this device is
    // created shouldn't be misread as a spurious rising edge later.
    dev->last_level = pin_get(aCPU, aPwmPin);

    bus_on_write(aBus, aPwmPin.reg, on_pwm_write, dev);
    return dev;
}

void servo_destroy(servo_t *aDev)
{
    free(aDev);
}

void servo_step(servo_t *aDev)
{
    aDev->ticks_since_rising++;
}

uint16_t servo_get_pulse_us(const servo_t *aDev)
{
    return aDev->last_pulse_us;
}

int servo_get_angle_decidegrees(const servo_t *aDev)
{
    long span = (long)aDev->pulse_max_us - (long)aDev->pulse_min_us;
    long angle;
    if (span <= 0)
        return 0;
    angle = ((long)aDev->last_pulse_us - (long)aDev->pulse_min_us) * 1800L / span;
    if (angle < 0)
        angle = 0;
    if (angle > 1800)
        angle = 1800;
    return (int)angle;
}
