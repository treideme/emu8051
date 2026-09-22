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
 *
 * Packet layer (added on branch enc28j60-net): host-injected RX frames land
 * in the circular RX buffer ERXST..ERXND exactly as the chip lays them out
 * (6-byte header: next-packet pointer, byte count incl. FCS, status with
 * RX-OK/multicast/broadcast bits; frame; CRC-32; even-address alignment),
 * EPKTCNT/EIR.PKTIF count them, ECON2.PKTDEC releases them, a buffer read
 * reaching ERXND wraps to ERXST, and the receive filter honours ERXFCON
 * UCEN/BCEN/MCEN/ANDOR against MAADR (PMEN/MPEN/HTEN are not modelled: an
 * enabled one never matches). Setting ECON1.TXRTS captures ETXST+1..ETXND
 * as a transmitted frame (padded to 60 bytes per MACON3.PADCFG, FCS not
 * included) and sets EIR.TXIF. Still NOT modelled: the PHY/link, DMA and
 * checksum engine, TX status vectors, interrupts on the INT pin, the MII
 * busy timing, and all electrical timing -- including the silicon errata
 * on slow SPI clocks, so a pass here says nothing about whether a
 * bit-banged SCK of ~30 kHz works on the real part. The RCR dummy-byte quirk for MAC/MII
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

// --- Packet layer ---------------------------------------------------------
#define ENC28J60_MAX_FRAME 1536 // largest frame the model stores (incl. FCS on RX)
#define ENC28J60_TX_LOG 32      // most recent transmitted frames kept

#define ENC28J60_RX_OK 0
#define ENC28J60_RX_DISABLED -1   // ECON1.RXEN clear, or RX buffer not set up
#define ENC28J60_RX_FILTERED -2   // rejected by ERXFCON
#define ENC28J60_RX_OVERFLOW -3   // not enough free space before ERXRDPT
#define ENC28J60_RX_BAD_LENGTH -4

// Deliver a frame (destination MAC first, no FCS -- the model appends it)
// as if it had arrived on the wire. Returns an ENC28J60_RX_* code.
int enc28j60_inject_rx(enc28j60_t *aDev, const uint8_t *aFrame, int aLen);

// Frames the firmware has transmitted (ECON1.TXRTS), oldest index 0.
int enc28j60_tx_count(const enc28j60_t *aDev);
// Copies frame aIndex into aOut (up to aMax bytes); returns its full length,
// or -1 if that index has aged out of the ENC28J60_TX_LOG ring.
int enc28j60_tx_frame(const enc28j60_t *aDev, int aIndex, uint8_t *aOut, int aMax);

#endif // SIM_DEVICES_ENC28J60_H
