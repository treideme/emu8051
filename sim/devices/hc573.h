/* 74HC573 octal transparent latch model
 * Copyright 2025 Thomas Reidemeister
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject
 * to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * (i.e. the MIT License)
 *
 * hc573.h
 *
 * While LE is high, Q follows D (transparent). When LE goes low, Q holds
 * whatever D was at that instant. Generic over which pins D0-D7/LE
 * actually are -- reusable for any board wiring, not just this project's.
 */
#ifndef SIM_DEVICES_HC573_H
#define SIM_DEVICES_HC573_H

#include "../bus.h"
#include "../pin.h"

typedef struct
{
    pin_t data[8]; // D0..D7
    pin_t le;      // Latch Enable, active high (transparent while 1)
} hc573_pins_t;

typedef struct hc573 hc573_t;

hc573_t *hc573_create(sim_bus_t *aBus, struct em8051 *aCPU, hc573_pins_t aPins);
void hc573_destroy(hc573_t *aDev);

// Currently latched (or, while LE=1, currently passed-through) Q0..Q7.
uint8_t hc573_get_output(const hc573_t *aDev);

#endif // SIM_DEVICES_HC573_H
