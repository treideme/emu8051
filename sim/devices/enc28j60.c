/* enc28j60.c - see enc28j60.h
 * Copyright 2025 Thomas Reidemeister, MIT License (see hc573.h)
 */
#include <stdlib.h>
#include <string.h>
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

    // Packet layer (host-injected RX, captured TX) -- see enc28j60.h.
    uint16_t rx_wr;                            // hardware RX write pointer (ERXWRPT)
    int tx_count;                              // frames transmitted since creation
    uint16_t tx_len[ENC28J60_TX_LOG];          // ring of the most recent frames
    uint8_t tx_frame[ENC28J60_TX_LOG][ENC28J60_MAX_FRAME];
};

// Register addresses the packet layer reacts to (bank, address).
#define B0_ERDPTL 0x00
#define B0_ETXSTL 0x04
#define B0_ETXNDL 0x06
#define B0_ERXSTL 0x08
#define B0_ERXSTH 0x09
#define B0_ERXNDL 0x0A
#define B0_ERXRDPTL 0x0C
#define B0_ERXWRPTL 0x0E
#define B1_ERXFCON 0x18
#define B1_EPKTCNT 0x19
#define REG_EIR 0x1C
#define REG_ESTAT 0x1D
#define REG_ECON2 0x1E
#define REG_ECON1 0x1F
#define B2_MACON3 0x02

#define ECON1_TXRTS 0x08
#define ECON1_RXEN 0x04
#define ECON2_PKTDEC 0x40
#define ECON2_AUTOINC 0x80
#define EIR_PKTIF 0x40
#define EIR_TXIF 0x08
#define EIR_RXERIF 0x01
#define ESTAT_CLKRDY 0x01
#define ERXFCON_UCEN 0x80
#define ERXFCON_ANDOR 0x40
#define ERXFCON_BCEN 0x01
#define ERXFCON_MCEN 0x02
#define ERXFCON_UNMODELED 0x1C // PMEN | MPEN | HTEN

static uint16_t reg_pair(struct enc28j60 *dev, int bank, uint8_t lo_addr);
static void set_reg_pair(struct enc28j60 *dev, int bank, uint8_t lo_addr, uint16_t value);

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
    // Like the chip: a read that reaches ERXND wraps to ERXST, so a packet
    // straddling the end of the circular RX buffer reads back contiguously.
    if (ptr == reg_pair(dev, 0, B0_ERXNDL))
        ptr = reg_pair(dev, 0, B0_ERXSTL);
    else
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
    // Non-zero reset values the packet layer depends on (datasheet table
    // 3-2): AUTOINC on, clock ready, and the default receive filter
    // UCEN|CRCEN|BCEN.
    dev->global_regs[REG_ECON2 - 0x1B] = ECON2_AUTOINC;
    dev->global_regs[REG_ESTAT - 0x1B] = ESTAT_CLKRDY;
    dev->banked_regs[1][B1_ERXFCON] = 0xA1;
    dev->rx_wr = 0;
    // Buffer memory (and its byte-count instrumentation) is left alone --
    // the real chip's soft reset doesn't clear packet buffer RAM.
}

static uint16_t reg_pair(struct enc28j60 *dev, int bank, uint8_t lo_addr)
{
    return (uint16_t)((*reg_slot_ptr(dev, bank, lo_addr) |
                       (*reg_slot_ptr(dev, bank, (uint8_t)(lo_addr + 1)) << 8)) &
                      (BUFFER_SIZE - 1));
}

static void set_reg_pair(struct enc28j60 *dev, int bank, uint8_t lo_addr, uint16_t value)
{
    *reg_slot_ptr(dev, bank, lo_addr) = (uint8_t)(value & 0xFF);
    *reg_slot_ptr(dev, bank, (uint8_t)(lo_addr + 1)) = (uint8_t)(value >> 8);
}

// Transmit: the frame is ETXST+1 .. ETXND inclusive (ETXST holds the
// per-packet control byte), exactly as the chip reads it -- so a driver
// that never programs ETXST transmits from wherever ETXST points, as the
// real part would. Padding to 60 bytes follows MACON3.PADCFG; the CRC the
// MAC would append (TXCRCEN) is not included in the captured frame.
static void do_transmit(struct enc28j60 *dev)
{
    uint16_t st = reg_pair(dev, 0, B0_ETXSTL);
    uint16_t nd = reg_pair(dev, 0, B0_ETXNDL);
    int slot = dev->tx_count % ENC28J60_TX_LOG;
    int len = 0;
    uint16_t p = (uint16_t)((st + 1) & (BUFFER_SIZE - 1));

    if (nd >= st)
    {
        while (len < ENC28J60_MAX_FRAME)
        {
            dev->tx_frame[slot][len++] = dev->buffer[p];
            if (p == nd)
                break;
            p = (uint16_t)((p + 1) & (BUFFER_SIZE - 1));
        }
    }
    if ((dev->banked_regs[2][B2_MACON3] & 0xE0) != 0)
        while (len < 60)
            dev->tx_frame[slot][len++] = 0;
    dev->tx_len[slot] = (uint16_t)len;
    dev->tx_count++;

    dev->global_regs[REG_ECON1 - 0x1B] &= (uint8_t)~ECON1_TXRTS;
    dev->global_regs[REG_EIR - 0x1B] |= EIR_TXIF;
}

// Side effects of a control-register write (WCR/BFS/BFC).
static void after_register_write(struct enc28j60 *dev, int bank, uint8_t addr)
{
    addr = (uint8_t)(addr & ADDR_MASK);
    if (addr == REG_ECON1 && (dev->global_regs[REG_ECON1 - 0x1B] & ECON1_TXRTS))
        do_transmit(dev);
    if (addr == REG_ECON2 && (dev->global_regs[REG_ECON2 - 0x1B] & ECON2_PKTDEC))
    {
        uint8_t *cnt = &dev->banked_regs[1][B1_EPKTCNT];
        if (*cnt)
            (*cnt)--;
        if (!*cnt)
            dev->global_regs[REG_EIR - 0x1B] &= (uint8_t)~EIR_PKTIF;
        dev->global_regs[REG_ECON2 - 0x1B] &= (uint8_t)~ECON2_PKTDEC; // self-clearing
    }
    // Programming ERXST also moves the hardware write pointer there
    // (datasheet 6.1). Modelled assumption; the chip documents ERXWRPT as
    // read-only and hardware-maintained.
    if (bank == 0 && (addr == B0_ERXSTL || addr == B0_ERXSTH))
    {
        dev->rx_wr = reg_pair(dev, 0, B0_ERXSTL);
        set_reg_pair(dev, 0, B0_ERXWRPTL, dev->rx_wr);
    }
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
        after_register_write(dev, current_bank(dev), dev->addr);
        break;
    case OP_BFS:
        *reg_slot_ptr(dev, current_bank(dev), dev->addr) |= byte;
        after_register_write(dev, current_bank(dev), dev->addr);
        break;
    case OP_BFC:
        *reg_slot_ptr(dev, current_bank(dev), dev->addr) &= (uint8_t)~byte;
        after_register_write(dev, current_bank(dev), dev->addr);
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
    do_soft_reset(dev); // power-on register values, not all-zero
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

static uint32_t crc32_ieee(const uint8_t *data, int len)
{
    uint32_t crc = 0xFFFFFFFFu;
    int i, b;
    for (i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1));
    }
    return ~crc;
}

static int rx_accept(struct enc28j60 *dev, const uint8_t *f)
{
    uint8_t fc = dev->banked_regs[1][B1_ERXFCON];
    static const uint8_t maadr_slot[6] = {0x04, 0x05, 0x02, 0x03, 0x00, 0x01};
    int bcast = 1, ucast = 1, mcast, i, any = 0, all = 1;

    if (fc == 0)
        return 1; // promiscuous
    for (i = 0; i < 6; i++)
    {
        if (f[i] != 0xFF)
            bcast = 0;
        if (f[i] != dev->banked_regs[3][maadr_slot[i]])
            ucast = 0;
    }
    mcast = (f[0] & 1) && !bcast;

    // PMEN/MPEN/HTEN are not modelled: an enabled one never matches.
    if (fc & ERXFCON_UCEN) { any |= ucast; all &= ucast; }
    if (fc & ERXFCON_BCEN) { any |= bcast; all &= bcast; }
    if (fc & ERXFCON_MCEN) { any |= mcast; all &= mcast; }
    if (fc & ERXFCON_UNMODELED) all = 0;
    return (fc & ERXFCON_ANDOR) ? all : any;
}

int enc28j60_inject_rx(enc28j60_t *aDev, const uint8_t *aFrame, int aLen)
{
    struct enc28j60 *dev = aDev;
    uint16_t st = reg_pair(dev, 0, B0_ERXSTL);
    uint16_t nd = reg_pair(dev, 0, B0_ERXNDL);
    uint16_t rd = reg_pair(dev, 0, B0_ERXRDPTL);
    uint16_t size, used, need, next, p;
    uint8_t hdr[6];
    uint32_t crc;
    int count, i, total;

    if (aLen < 14 || aLen > ENC28J60_MAX_FRAME - 4)
        return ENC28J60_RX_BAD_LENGTH;
    if (!(dev->global_regs[REG_ECON1 - 0x1B] & ECON1_RXEN))
        return ENC28J60_RX_DISABLED;
    if (nd <= st)
        return ENC28J60_RX_DISABLED;
    if (!rx_accept(dev, aFrame))
        return ENC28J60_RX_FILTERED;

    size = (uint16_t)(nd - st + 1);
    count = aLen + 4;                       // byte count includes the FCS
    need = (uint16_t)((6 + count + 1) & ~1u); // packets start on even addresses
    used = (uint16_t)((dev->rx_wr + size - rd) % size);
    if ((uint16_t)(used + need) >= size)
    {
        dev->global_regs[REG_EIR - 0x1B] |= EIR_RXERIF;
        return ENC28J60_RX_OVERFLOW;
    }

    next = (uint16_t)(st + ((dev->rx_wr - st + need) % size));
    crc = crc32_ieee(aFrame, aLen);
    hdr[0] = (uint8_t)(next & 0xFF);
    hdr[1] = (uint8_t)(next >> 8);
    hdr[2] = (uint8_t)(count & 0xFF);
    hdr[3] = (uint8_t)(count >> 8);
    hdr[4] = 0x80;                          // status bit 23: received OK
    hdr[5] = (uint8_t)(((aFrame[0] & 1) ? 0x01 : 0) | // bit 24 multicast
                       ((aFrame[0] == 0xFF) ? 0x02 : 0)); // bit 25 broadcast

    p = dev->rx_wr;
    total = 6 + count;
    for (i = 0; i < total; i++)
    {
        uint8_t b;
        if (i < 6)
            b = hdr[i];
        else if (i < 6 + aLen)
            b = aFrame[i - 6];
        else
            b = (uint8_t)(crc >> (8 * (i - 6 - aLen)));
        dev->buffer[p] = b;
        p = (p == nd) ? st : (uint16_t)(p + 1);
    }
    dev->rx_wr = next;
    set_reg_pair(dev, 0, B0_ERXWRPTL, next);
    if (dev->banked_regs[1][B1_EPKTCNT] < 0xFF)
        dev->banked_regs[1][B1_EPKTCNT]++;
    dev->global_regs[REG_EIR - 0x1B] |= EIR_PKTIF;
    return ENC28J60_RX_OK;
}

int enc28j60_tx_count(const enc28j60_t *aDev)
{
    return aDev->tx_count;
}

int enc28j60_tx_frame(const enc28j60_t *aDev, int aIndex, uint8_t *aOut, int aMax)
{
    int slot, len;
    if (aIndex < 0 || aIndex >= aDev->tx_count || aIndex < aDev->tx_count - ENC28J60_TX_LOG)
        return -1;
    slot = aIndex % ENC28J60_TX_LOG;
    len = aDev->tx_len[slot];
    if (len > aMax)
        len = aMax;
    memcpy(aOut, aDev->tx_frame[slot], (size_t)len);
    return aDev->tx_len[slot];
}
