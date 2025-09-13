/* xpt2046.c - see xpt2046.h
 * Copyright 2025 Thomas Reidemeister, MIT License (see hc573.h)
 */
#include <stdlib.h>
#include <string.h>
#include "xpt2046.h"

enum
{
    PHASE_IDLE = 0,
    PHASE_CMD,
    PHASE_RESULT
};

struct xpt2046
{
    xpt2046_pins_t pins;
    uint16_t channel_reading[8];

    int last_cs;
    int last_clk;
    int phase;
    int bitcount;
    uint8_t cur_byte;

    int channel;
    int bits_left; // 12 or 8, per the MODE bit
    uint16_t shift_reg;
    int out_bit;
};

static void begin_cmd_phase(xpt2046_t *dev)
{
    dev->phase = PHASE_CMD;
    dev->bitcount = 0;
    dev->cur_byte = 0;
}

static void on_cs_write(struct em8051 *aCPU, uint8_t aReg, uint8_t aValue, void *aUserData)
{
    xpt2046_t *dev = (xpt2046_t *)aUserData;
    int cs = pin_get(aCPU, dev->pins.cs);
    (void)aReg;
    (void)aValue;

    if (!cs && dev->last_cs) // active low: falling edge starts a transaction
        begin_cmd_phase(dev);
    if (cs && !dev->last_cs)
        dev->phase = PHASE_IDLE;

    dev->last_cs = cs;
}

static void on_clk_write(struct em8051 *aCPU, uint8_t aReg, uint8_t aValue, void *aUserData)
{
    xpt2046_t *dev = (xpt2046_t *)aUserData;
    int clk = pin_get(aCPU, dev->pins.clk);
    int rising = clk && !dev->last_clk;
    int falling = !clk && dev->last_clk;
    (void)aReg;
    (void)aValue;

    if (dev->last_cs)
    {
        dev->last_clk = clk;
        return; // chip not selected
    }

    if (rising && dev->phase == PHASE_CMD)
    {
        int bit = pin_get(aCPU, dev->pins.din);
        dev->cur_byte = (uint8_t)((dev->cur_byte << 1) | bit); // MSB first
        dev->bitcount++;

        if (dev->bitcount == 8)
        {
            int mode_8bit = (dev->cur_byte >> 3) & 1;
            dev->channel = (dev->cur_byte >> 4) & 7;
            dev->bits_left = mode_8bit ? 8 : 12;
            dev->shift_reg = dev->channel_reading[dev->channel];
            if (mode_8bit)
                dev->shift_reg >>= 4; // report the top 8 bits in 8-bit mode
            dev->phase = PHASE_RESULT;
            dev->out_bit = (dev->shift_reg >> (dev->bits_left - 1)) & 1;
        }
    }
    else if (falling && dev->phase == PHASE_RESULT)
    {
        dev->bits_left--;
        if (dev->bits_left > 0)
            dev->out_bit = (dev->shift_reg >> (dev->bits_left - 1)) & 1;
    }

    dev->last_clk = clk;
}

static void on_dout_read(struct em8051 *aCPU, uint8_t aReg, void *aUserData, uint8_t *aOutMask, uint8_t *aOutValue)
{
    xpt2046_t *dev = (xpt2046_t *)aUserData;
    (void)aCPU;
    (void)aReg;

    if (dev->phase != PHASE_RESULT)
        return;

    *aOutMask = (uint8_t)(*aOutMask | pin_mask(dev->pins.dout));
    if (dev->out_bit)
        *aOutValue = (uint8_t)(*aOutValue | pin_mask(dev->pins.dout));
}

xpt2046_t *xpt2046_create(sim_bus_t *aBus, struct em8051 *aCPU, xpt2046_pins_t aPins)
{
    xpt2046_t *dev = (xpt2046_t *)calloc(1, sizeof(xpt2046_t));
    dev->pins = aPins;
    dev->last_cs = 1; // idle high (deselected) until the firmware asserts it

    bus_on_write(aBus, aPins.cs.reg, on_cs_write, dev);
    bus_on_write(aBus, aPins.clk.reg, on_clk_write, dev);
    bus_on_read(aBus, aPins.dout.reg, on_dout_read, dev);

    (void)aCPU;
    return dev;
}

void xpt2046_destroy(xpt2046_t *aDev)
{
    free(aDev);
}

void xpt2046_set_channel_reading(xpt2046_t *aDev, int aChannel, uint16_t aValue12Bit)
{
    if (aChannel < 0 || aChannel > 7)
        return;
    aDev->channel_reading[aChannel] = (uint16_t)(aValue12Bit & 0x0fff);
}

void xpt2046_set_reading(xpt2046_t *aDev, uint16_t aValue12Bit)
{
    int i;
    for (i = 0; i < 8; i++)
        xpt2046_set_channel_reading(aDev, i, aValue12Bit);
}

int xpt2046_get_last_channel(const xpt2046_t *aDev)
{
    return aDev->channel;
}
