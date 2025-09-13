/* hc138.c - see hc138.h
 * Copyright 2025 Thomas Reidemeister, MIT License (see hc573.h)
 */
#include <stdlib.h>
#include "hc138.h"

struct hc138
{
    hc138_pins_t pins;
};

hc138_t *hc138_create(hc138_pins_t aPins)
{
    hc138_t *dev = (hc138_t *)calloc(1, sizeof(hc138_t));
    dev->pins = aPins;
    return dev;
}

void hc138_destroy(hc138_t *aDev)
{
    free(aDev);
}

int hc138_get_active_output(const hc138_t *aDev, const struct em8051 *aCPU)
{
    const hc138_pins_t *p = &aDev->pins;

    if (pin_is_connected(p->g1) && !pin_get(aCPU, p->g1))
        return -1;
    if (pin_is_connected(p->g2a) && pin_get(aCPU, p->g2a))
        return -1;
    if (pin_is_connected(p->g2b) && pin_get(aCPU, p->g2b))
        return -1;

    return pin_get(aCPU, p->a) | (pin_get(aCPU, p->b) << 1) | (pin_get(aCPU, p->c) << 2);
}
