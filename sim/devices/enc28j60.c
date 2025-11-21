/* enc28j60.c - see enc28j60.h
 * Copyright 2025 Thomas Reidemeister, MIT License (see hc573.h)
 */
#include <stdlib.h>
#include "enc28j60.h"

#define ADDR_MASK 0x1F
#define BUFFER_SIZE 0x2000 // 8KB packet buffer RAM, matching the real chip

#define OP_RCR 0x00
#define OP_WCR 0x40
#define OP_BFS 0x80
#define OP_BFC 0xA0
#define OP_RBM 0x3A
#define OP_WBM 0x7A
#define OP_SOFT_RESET 0xFF

// EIE=0x1B, EIR=0x1C, ESTAT=0x1D, ECON2=0x1E, ECON1=0x1F -- shared across
// every bank on the real chip, so stored outside banked_regs[].
#define GLOBAL_ECON1 4

struct enc28j60
{
    enc28j60_pins_t pins;

    int last_cs;
    int last_sck;

    uint8_t shift_in;  // accumulating incoming byte (MOSI, MSB first)
    uint8_t out_byte;  // byte currently being clocked out (MISO)
    int out_bit;       // this clock's single MISO bit, from out_byte
    int bit_count;     // 0-7: bits received/sent so far in the current byte
    int byte_index;    // 0 = opcode byte, 1+ = data bytes, within one CS-low run

    uint8_t op;
    uint8_t addr;
    uint8_t last_opcode_byte;
    int needs_dummy;

    uint8_t banked_regs[4][0x1B];
    uint8_t global_regs[5];
    uint8_t buffer[BUFFER_SIZE];
    unsigned long buffer_byte_count;
};

static uint8_t *reg_slot_ptr(struct enc28j60 *dev, int bank, uint8_t addr)
{
    addr = (uint8_t)(addr & ADDR_MASK);
    if (addr >= 0x1B)
        return &dev->global_regs[addr - 0x1B];
    if (bank < 0)
        bank = 0;
    if (bank > 3)
        bank = 3;
    return &dev->banked_regs[bank][addr];
}

static int current_bank(struct enc28j60 *dev)
{
    return dev->global_regs[GLOBAL_ECON1] & 0x03;
}

// Which (bank, address) combinations are MAC/MII registers needing an
// extra dummy byte before the real one on a control-register read (the
// datasheet's SPRD_MASK bit) -- derived from the ported demo's own
// 09_ethernet/enc28j60.h, the only driver this model needs to match:
// every named bank-2 register there carries the bit, and in bank 3 only
// MAADR0-5 and MISTAT do (EBST*/EREVID/ECOCON/EFLOCON/EPAUS* don't).
static int needs_dummy_byte(int bank, uint8_t addr)
{
    if (bank == 2)
        return 1;
    if (bank == 3)
        return (addr <= 0x05) || (addr == 0x0A);
    return 0;
}

// Read side is a peek/commit pair rather than one combined step: the
// output byte for a READ_BUF_MEM data byte must be ready *before* that
// byte starts shifting out (peek, no side effect), but must only be
// counted as actually consumed *after* the master has finished clocking
// it (commit) -- otherwise the read side over-counts by one, since the
// natural place to prepare the *next* byte's output (immediately after
// the current byte finishes) would otherwise also advance the pointer
// for a byte that may never actually be clocked out if CS deasserts
// right after (found by testing 09_ethernet_diag.hex's real 4-byte
// buffer round trip against buffer_byte_count: got 9, not 8).
static uint8_t peek_read_byte(struct enc28j60 *dev)
{
    uint8_t *ptrlo = reg_slot_ptr(dev, 0, 0x00); // ERDPTL
    uint8_t *ptrhi = reg_slot_ptr(dev, 0, 0x01); // ERDPTH
    uint16_t ptr = (uint16_t)(((*ptrhi << 8) | *ptrlo) % BUFFER_SIZE);
    return dev->buffer[ptr];
}

static void commit_read_byte(struct enc28j60 *dev)
{
    uint8_t *ptrlo = reg_slot_ptr(dev, 0, 0x00);
    uint8_t *ptrhi = reg_slot_ptr(dev, 0, 0x01);
    uint16_t ptr = (uint16_t)(((*ptrhi << 8) | *ptrlo) % BUFFER_SIZE);
    ptr = (uint16_t)((ptr + 1) % BUFFER_SIZE);
    *ptrlo = (uint8_t)(ptr & 0xFF);
    *ptrhi = (uint8_t)(ptr >> 8);
    dev->buffer_byte_count++;
}

// Write side has no equivalent hazard: the byte value isn't needed
// until after it has been fully received, so committing exactly once
// per completed byte (in on_byte_complete()) is already correct.
static void commit_write_byte(struct enc28j60 *dev, uint8_t value)
{
    uint8_t *ptrlo = reg_slot_ptr(dev, 0, 0x02); // EWRPTL
    uint8_t *ptrhi = reg_slot_ptr(dev, 0, 0x03); // EWRPTH
    uint16_t ptr = (uint16_t)(((*ptrhi << 8) | *ptrlo) % BUFFER_SIZE);
    dev->buffer[ptr] = value;
    ptr = (uint16_t)((ptr + 1) % BUFFER_SIZE);
    *ptrlo = (uint8_t)(ptr & 0xFF);
    *ptrhi = (uint8_t)(ptr >> 8);
    dev->buffer_byte_count++;
}

static void do_soft_reset(struct enc28j60 *dev)
{
    int b, i;
    for (b = 0; b < 4; b++)
        for (i = 0; i < 0x1B; i++)
            dev->banked_regs[b][i] = 0;
    for (i = 0; i < 5; i++)
        dev->global_regs[i] = 0;
    // Buffer memory (and its byte-count instrumentation) is left alone --
    // the real chip's soft reset doesn't clear packet buffer RAM.
}

static void decode_opcode(struct enc28j60 *dev, uint8_t byte)
{
    dev->last_opcode_byte = byte;

    if (byte == OP_SOFT_RESET)
    {
        dev->op = OP_SOFT_RESET;
        do_soft_reset(dev);
        dev->out_byte = 0;
        return;
    }
    if (byte == OP_RBM)
    {
        dev->op = OP_RBM;
        dev->out_byte = peek_read_byte(dev); // stream starts right after the opcode, no dummy; not yet consumed
        return;
    }
    if (byte == OP_WBM)
    {
        dev->op = OP_WBM;
        dev->out_byte = 0;
        return;
    }

    dev->op = (uint8_t)(byte & 0xE0);
    dev->addr = (uint8_t)(byte & ADDR_MASK);
    dev->needs_dummy = 0;
    if (dev->op == OP_RCR)
    {
        dev->needs_dummy = needs_dummy_byte(current_bank(dev), dev->addr);
        dev->out_byte = dev->needs_dummy ? 0 : *reg_slot_ptr(dev, current_bank(dev), dev->addr);
    }
    else
    {
        dev->out_byte = 0;
    }
}

static void on_byte_complete(struct enc28j60 *dev, uint8_t byte)
{
    if (dev->byte_index == 0)
    {
        decode_opcode(dev, byte);
        return;
    }

    switch (dev->op)
    {
    case OP_WCR:
        *reg_slot_ptr(dev, current_bank(dev), dev->addr) = byte;
        break;
    case OP_BFS:
        *reg_slot_ptr(dev, current_bank(dev), dev->addr) |= byte;
        break;
    case OP_BFC:
        *reg_slot_ptr(dev, current_bank(dev), dev->addr) &= (uint8_t)~byte;
        break;
    case OP_RCR:
        if (dev->needs_dummy)
        {
            dev->out_byte = *reg_slot_ptr(dev, current_bank(dev), dev->addr);
            dev->needs_dummy = 0;
        }
        break;
    case OP_RBM:
        commit_read_byte(dev); // the byte just finished shifting out has now actually been read
        dev->out_byte = peek_read_byte(dev); // prepare the next one, in case another follows
        break;
    case OP_WBM:
        commit_write_byte(dev, byte);
        break;
    default:
        break;
    }
}

static void on_spi_write(struct em8051 *aCPU, uint8_t aReg, uint8_t aValue, void *aUserData)
{
    struct enc28j60 *dev = (struct enc28j60 *)aUserData;
    int cs = pin_get(aCPU, dev->pins.cs);
    int sck = pin_get(aCPU, dev->pins.sck);
    int cs_falling = !cs && dev->last_cs;
    int sck_rising = sck && !dev->last_sck;
    (void)aReg;
    (void)aValue;

    if (cs_falling)
    {
        dev->bit_count = 0;
        dev->shift_in = 0;
        dev->byte_index = 0;
        dev->needs_dummy = 0;
    }

    if (!cs && sck_rising)
    {
        int mosi_bit = pin_get(aCPU, dev->pins.mosi);
        dev->shift_in = (uint8_t)((dev->shift_in << 1) | mosi_bit);
        dev->bit_count++;
        dev->out_bit = (dev->out_byte >> (8 - dev->bit_count)) & 1;

        if (dev->bit_count == 8)
        {
            on_byte_complete(dev, dev->shift_in);
            dev->bit_count = 0;
            dev->shift_in = 0;
            dev->byte_index++;
        }
    }

    dev->last_cs = cs;
    dev->last_sck = sck;
}

static void on_miso_read(struct em8051 *aCPU, uint8_t aReg, void *aUserData, uint8_t *aOutMask, uint8_t *aOutValue)
{
    struct enc28j60 *dev = (struct enc28j60 *)aUserData;
    (void)aCPU;
    (void)aReg;

    if (dev->last_cs) // CS deasserted (idle-high): not driving
        return;
    *aOutMask = (uint8_t)(*aOutMask | pin_mask(dev->pins.miso));
    if (dev->out_bit)
        *aOutValue = (uint8_t)(*aOutValue | pin_mask(dev->pins.miso));
}

enc28j60_t *enc28j60_create(sim_bus_t *aBus, struct em8051 *aCPU, enc28j60_pins_t aPins)
{
    struct enc28j60 *dev = (struct enc28j60 *)calloc(1, sizeof(struct enc28j60));
    int reg_done[128] = {0};
    pin_t watch[3];
    int i;

    dev->pins = aPins;
    dev->last_cs = pin_get(aCPU, aPins.cs);
    dev->last_sck = pin_get(aCPU, aPins.sck);

    watch[0] = aPins.cs;
    watch[1] = aPins.sck;
    watch[2] = aPins.mosi;
    for (i = 0; i < 3; i++)
    {
        pin_t p = watch[i];
        if (pin_is_connected(p) && !reg_done[p.reg])
        {
            bus_on_write(aBus, p.reg, on_spi_write, dev);
            reg_done[p.reg] = 1;
        }
    }
    bus_on_read(aBus, aPins.miso.reg, on_miso_read, dev);

    return dev;
}

void enc28j60_destroy(enc28j60_t *aDev)
{
    free(aDev);
}

uint8_t enc28j60_get_register(const enc28j60_t *aDev, int aBank, int aAddress)
{
    uint8_t addr = (uint8_t)(aAddress & ADDR_MASK);
    if (addr >= 0x1B)
        return aDev->global_regs[addr - 0x1B];
    if (aBank < 0)
        aBank = 0;
    if (aBank > 3)
        aBank = 3;
    return aDev->banked_regs[aBank][addr];
}

int enc28j60_get_bank(const enc28j60_t *aDev)
{
    return aDev->global_regs[GLOBAL_ECON1] & 0x03;
}

uint8_t enc28j60_get_last_opcode(const enc28j60_t *aDev)
{
    return aDev->last_opcode_byte;
}

unsigned long enc28j60_get_buffer_byte_count(const enc28j60_t *aDev)
{
    return aDev->buffer_byte_count;
}
