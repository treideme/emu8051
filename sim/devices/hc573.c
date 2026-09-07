/* hc573.c - see hc573.h
 * Copyright 2025 Thomas Reidemeister, MIT License (see hc573.h)
 */
#include <stdlib.h>
#include "hc573.h"

struct hc573
{
    hc573_pins_t pins;
    uint8_t output;
    int transparent;
};

static uint8_t read_data_bus(const hc573_t *aDev, const struct em8051 *aCPU)
{
    uint8_t v = 0;
    int i;
    for (i = 0; i < 8; i++)
        if (pin_is_connected(aDev->pins.data[i]))
            v = (uint8_t)(v | (pin_get(aCPU, aDev->pins.data[i]) << i));
    return v;
}

static void on_data_write(struct em8051 *aCPU, uint8_t aReg, uint8_t aValue, void *aUserData)
{
    hc573_t *dev = (hc573_t *)aUserData;
    (void)aReg;
    (void)aValue;
    if (dev->transparent)
        dev->output = read_data_bus(dev, aCPU);
}

static void on_le_write(struct em8051 *aCPU, uint8_t aReg, uint8_t aValue, void *aUserData)
{
    hc573_t *dev = (hc573_t *)aUserData;
    int le = pin_get(aCPU, dev->pins.le);
    (void)aReg;
    (void)aValue;

    if (le)
    {
        dev->transparent = 1;
        dev->output = read_data_bus(dev, aCPU);
    }
    else
    {
        if (dev->transparent)
            dev->output = read_data_bus(dev, aCPU); // capture final value at the falling edge
        dev->transparent = 0;
    }
}

hc573_t *hc573_create(sim_bus_t *aBus, struct em8051 *aCPU, hc573_pins_t aPins)
{
    hc573_t *dev = (hc573_t *)calloc(1, sizeof(hc573_t));
    int i;
    int reg_done[128] = {0};

    dev->pins = aPins;
    // Read LE's *actual current* level rather than assuming 0/latched:
    // a firmware that never explicitly drives LE (relying on the 8051's
    // power-on port default of 0xFF, i.e. permanently transparent, rather
    // than writing it itself -- confirmed in the wild via
    // the sibling demo repo's 3432_clock_digit_tube_2, which has no LE
    // assignment anywhere) would otherwise leave transparent stuck at
    // its calloc'd 0 forever, since the only other place it's ever set
    // is on_le_write(), which needs an actual write *event* to fire and
    // therefore never runs for a pin nothing ever writes.
    dev->transparent = pin_get(aCPU, aPins.le);
    dev->output = read_data_bus(dev, aCPU); // reflect whatever's on the bus at power-up

    bus_on_write(aBus, aPins.le.reg, on_le_write, dev);
    for (i = 0; i < 8; i++)
    {
        pin_t p = aPins.data[i];
        if (pin_is_connected(p) && !reg_done[p.reg] && p.reg != aPins.le.reg)
        {
            bus_on_write(aBus, p.reg, on_data_write, dev);
            reg_done[p.reg] = 1;
        }
    }
    return dev;
}

void hc573_destroy(hc573_t *aDev)
{
    free(aDev);
}

uint8_t hc573_get_output(const hc573_t *aDev)
{
    return aDev->output;
}
