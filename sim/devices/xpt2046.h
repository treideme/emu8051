/* XPT2046 (ADS7846-compatible) SPI touch-ADC controller model
 * Copyright 2025 Thomas Reidemeister, MIT License (see hc573.h for full text)
 *
 * xpt2046.h
 *
 * Implements just the ADC-conversion half of the chip (no touch-pressure
 * detection logic): an 8-bit control byte in (S, A2-A0 channel, MODE
 * 12/8-bit, SER/DFR, PD1-PD0), MSB first, sampled on CLK rising edges;
 * then the requested channel's reading out on DOUT, MSB first, updated
 * on CLK falling edges -- the standard documented protocol, not tied to
 * any one firmware's specific bit-bang timing.
 *
 * All channels return the same settable reading by default (this
 * project's demos only ever read one channel); call
 * xpt2046_set_channel_reading for per-channel values if a future demo
 * needs to distinguish them.
 */
#ifndef SIM_DEVICES_XPT2046_H
#define SIM_DEVICES_XPT2046_H

#include "../bus.h"
#include "../pin.h"

typedef struct
{
    pin_t cs;  // chip select, active low
    pin_t clk;
    pin_t din;
    pin_t dout;
} xpt2046_pins_t;

typedef struct xpt2046 xpt2046_t;

xpt2046_t *xpt2046_create(sim_bus_t *aBus, struct em8051 *aCPU, xpt2046_pins_t aPins);
void xpt2046_destroy(xpt2046_t *aDev);

// Stimulus: the 12-bit (0-4095) reading channel aChannel (0-7) should
// report on its next conversion.
void xpt2046_set_channel_reading(xpt2046_t *aDev, int aChannel, uint16_t aValue12Bit);

// Convenience: set the same reading for every channel (0-7).
void xpt2046_set_reading(xpt2046_t *aDev, uint16_t aValue12Bit);

// Query: which channel (and mode) the last completed control byte
// requested, for asserting the firmware issued the command you expected.
int xpt2046_get_last_channel(const xpt2046_t *aDev);

#endif // SIM_DEVICES_XPT2046_H
