/* ENC28J60 SPI Ethernet controller: register/bank protocol model only.
 * Copyright 2025 Thomas Reidemeister, MIT License (see hc573.h for full text)
 *
 * enc28j60.h
 *
 * Not part of any real HC6800-ES peripheral (see sim/README.md's "plugin
 * vs board definition" note and this simulator's sibling demo repo's blog-topics.md planning doc,
 * which also records that real ENC28J60 hardware needs an SPI clock this
 * chip can't reach by bit-banging -- this model exists to exercise a
 * driver's protocol framing, not to claim real-hardware feasibility).
 *
 * Models the bit-banged SPI mode-0 shift register (CS/SCK/MOSI/MISO), the
 * 4-bank x 32-slot control-register file (with the five all-bank
 * registers EIE/EIR/ESTAT/ECON2/ECON1 shared across banks, matching the
 * real chip), READ_BUF_MEM/WRITE_BUF_MEM against an 8KB buffer with
 * ERDPT/EWRPT auto-increment, and SOFT_RESET -- enough to prove a
 * driver's opcode framing and bank-select sequencing are correct.
 * Deliberately NOT a network stack: no packet TX/RX, no PHY link
 * simulation, no electrical timing. The RCR dummy-byte quirk for MAC/MII
 * registers is modeled from which registers this project's own driver
 * marks with the datasheet's SPRD_MASK bit, not derived from first
 * principles -- see needs_dummy_byte() in enc28j60.c.
 */
#ifndef SIM_DEVICES_ENC28J60_H
#define SIM_DEVICES_ENC28J60_H

#include "../bus.h"
#include "../pin.h"

typedef struct
{
    pin_t cs;
    pin_t sck;
    pin_t mosi;
    pin_t miso;
} enc28j60_pins_t;

typedef struct enc28j60 enc28j60_t;

enc28j60_t *enc28j60_create(sim_bus_t *aBus, struct em8051 *aCPU, enc28j60_pins_t aPins);
void enc28j60_destroy(enc28j60_t *aDev);

// Raw register file access: aBank 0-3 (ignored for the five all-bank
// registers 0x1B-0x1F), aAddress 0-0x1F (masked internally).
uint8_t enc28j60_get_register(const enc28j60_t *aDev, int aBank, int aAddress);

// Currently-selected bank (0-3), tracked via ECON1's BSEL1:BSEL0 bits.
int enc28j60_get_bank(const enc28j60_t *aDev);

// The most recently fully-decoded SPI opcode byte, exactly as sent on the
// wire (e.g. 0xFF for SOFT_RESET, 0x3A for READ_BUF_MEM, opcode|address
// for RCR/WCR/BFS/BFC). 0 before the first transaction.
uint8_t enc28j60_get_last_opcode(const enc28j60_t *aDev);

// Total bytes moved through READ_BUF_MEM + WRITE_BUF_MEM since creation
// (SOFT_RESET does not clear this -- it's activity instrumentation, not
// chip state).
unsigned long enc28j60_get_buffer_byte_count(const enc28j60_t *aDev);

#endif // SIM_DEVICES_ENC28J60_H
