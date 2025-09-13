/* 74LS138/74HC138 3-to-8 line decoder model
 * Copyright 2025 Thomas Reidemeister, MIT License (see hc573.h for full text)
 *
 * hc138.h
 *
 * Purely combinational (no clock, no state) -- the output is just a
 * function of the current pin levels, so unlike the other devices this
 * one has no bus subscription at all; querying it re-reads the select/
 * enable pins on demand. Enable pins are optional (PIN_UNCONNECTED): if
 * left unconnected, the decoder is treated as permanently enabled, which
 * matches how most small boards wire it (enables tied to fixed levels,
 * not driven by GPIO).
 */
#ifndef SIM_DEVICES_HC138_H
#define SIM_DEVICES_HC138_H

#include "../pin.h"

typedef struct
{
    pin_t a, b, c;          // select lines (c is MSB)
    pin_t g1;                // enable, active HIGH; PIN_UNCONNECTED = always enabled
    pin_t g2a, g2b;          // enable, active LOW each; PIN_UNCONNECTED = treated as asserted
} hc138_pins_t;

typedef struct hc138 hc138_t;

hc138_t *hc138_create(hc138_pins_t aPins);
void hc138_destroy(hc138_t *aDev);

// 0-7 for whichever output is currently selected, or -1 if the decoder is
// disabled (g1/g2a/g2b not asserted).
int hc138_get_active_output(const hc138_t *aDev, const struct em8051 *aCPU);

#endif // SIM_DEVICES_HC138_H
