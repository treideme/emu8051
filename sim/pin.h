/* 8051 emulator peripheral simulation - pin abstraction
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
 * pin.h
 *
 * A pin_t names one bit of one SFR register (P0-P3 typically). Device
 * models (under sim/devices) are written entirely in terms of pin_t
 * values handed to them at creation time -- they never hardcode a port
 * letter. A board file (under sim/boards) is what actually says "P2.6"
 * for a given dev-kit; the same device model is reused unchanged across
 * boards that wire the same peripheral to different pins.
 */
#ifndef SIM_PIN_H
#define SIM_PIN_H

#include <stdint.h>
#include <stdbool.h>
#include "emu8051.h"

typedef struct
{
    uint8_t reg; // SFR register index, pre-offset by 0x80 (i.e. a REG_* value
                 // from emu8051.h, such as REG_P0..REG_P3). 0xFF = unconnected.
    uint8_t bit; // bit number 0-7 within that register
} pin_t;

#define PIN_UNCONNECTED ((pin_t){ 0xFF, 0xFF })

static inline pin_t pin_make(uint8_t aReg, uint8_t aBit)
{
    pin_t p;
    p.reg = aReg;
    p.bit = aBit;
    return p;
}

static inline bool pin_is_connected(pin_t aPin)
{
    return aPin.reg != 0xFF;
}

// Current level of a single pin, straight from the CPU's SFR state.
static inline int pin_get(const struct em8051 *aCPU, pin_t aPin)
{
    if (!pin_is_connected(aPin))
        return 0;
    return (aCPU->mSFR[aPin.reg] >> aPin.bit) & 1;
}

// Force a single pin's level. Used by device models that drive an input
// back into the CPU (e.g. an ADC's DOUT line, a DS1302's bidirectional
// I/O line while it's driving) via the bus's read-side mask/value
// composition -- see bus.h. Not used to simulate the CPU's own output pins.
static inline uint8_t pin_mask(pin_t aPin)
{
    if (!pin_is_connected(aPin))
        return 0;
    return (uint8_t)(1u << aPin.bit);
}

static inline uint8_t pin_value(pin_t aPin, int aLevel)
{
    if (!pin_is_connected(aPin) || !aLevel)
        return 0;
    return (uint8_t)(1u << aPin.bit);
}

#endif // SIM_PIN_H
