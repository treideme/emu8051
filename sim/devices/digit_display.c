/* digit_display.c - see digit_display.h
 * Copyright 2025 Thomas Reidemeister, MIT License (see hc573.h)
 */
#include <stdlib.h>
#include <string.h>
#include "digit_display.h"

struct digit_display
{
    hc573_t *latch;
    hc138_t *select;
    int digit_count;
    uint8_t digits[DIGIT_DISPLAY_MAX_DIGITS];
};

static void resample(struct em8051 *aCPU, uint8_t aReg, uint8_t aValue, void *aUserData)
{
    digit_display_t *dev = (digit_display_t *)aUserData;
    int idx;
    uint8_t segs;
    (void)aReg;
    (void)aValue;

    idx = hc138_get_active_output(dev->select, aCPU);
    if (idx < 0 || idx >= dev->digit_count)
        return;

    // Multiplexed displays conventionally blank the segment data (all
    // segments off) while switching between digits, to avoid ghosting --
    // that's a deliberately transient state a real eye/camera never
    // actually perceives (persistence of vision), so it's ignored here
    // rather than overwriting the last real pattern this digit showed.
    segs = hc573_get_output(dev->latch);
    if (segs != 0)
        dev->digits[idx] = segs;
}

digit_display_t *digit_display_create(sim_bus_t *aBus, struct em8051 *aCPU, digit_display_pins_t aPins)
{
    digit_display_t *dev = (digit_display_t *)calloc(1, sizeof(digit_display_t));
    int reg_done[128] = {0};
    pin_t watch[6];
    int i;

    dev->digit_count = aPins.digit_count;
    if (dev->digit_count > DIGIT_DISPLAY_MAX_DIGITS)
        dev->digit_count = DIGIT_DISPLAY_MAX_DIGITS;

    dev->latch = hc573_create(aBus, aCPU, aPins.latch);
    dev->select = hc138_create(aPins.select);

    watch[0] = aPins.latch.le;
    watch[1] = aPins.select.a;
    watch[2] = aPins.select.b;
    watch[3] = aPins.select.c;
    watch[4] = aPins.select.g1;
    watch[5] = aPins.select.g2a;
    for (i = 0; i < 6; i++)
    {
        pin_t p = watch[i];
        if (pin_is_connected(p) && !reg_done[p.reg])
        {
            bus_on_write(aBus, p.reg, resample, dev);
            reg_done[p.reg] = 1;
        }
    }
    // (aPins.select.g2b intentionally not separately watched: on every
    // board this project targets it's tied with g2a or unconnected; add
    // it to `watch` above if a future board drives it independently.)

    // Also resample on every write to the segment-data bus itself: a
    // firmware that never pulses LE at all (permanently transparent) and
    // writes select-then-data with no further signal in between --
    // exactly what 17_digit_tube_student_id does -- would otherwise only
    // ever get resampled at the moment the *previous* digit's data was
    // still on the bus, one write too early.
    for (i = 0; i < 8; i++)
    {
        pin_t p = aPins.latch.data[i];
        if (pin_is_connected(p) && !reg_done[p.reg])
        {
            bus_on_write(aBus, p.reg, resample, dev);
            reg_done[p.reg] = 1;
        }
    }

    return dev;
}

void digit_display_destroy(digit_display_t *aDev)
{
    hc573_destroy(aDev->latch);
    hc138_destroy(aDev->select);
    free(aDev);
}

uint8_t digit_display_get_segments(const digit_display_t *aDev, int aDigit)
{
    if (aDigit < 0 || aDigit >= aDev->digit_count)
        return 0;
    return aDev->digits[aDigit];
}

char digit_display_decode(uint8_t aSegments)
{
    static const uint8_t table[16] = {
        0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07,
        0x7f, 0x6f, 0x77, 0x7c, 0x39, 0x5e, 0x79, 0x71};
    static const char chars[16] = "0123456789AbCdEF";
    uint8_t masked = aSegments & 0x7f; // ignore decimal point
    int i;
    for (i = 0; i < 16; i++)
        if (table[i] == masked)
            return chars[i];
    return '?';
}
