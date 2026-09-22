/* iap.c - see iap.h
 * Copyright 2026 Thomas Reidemeister, MIT License (see hc573.h)
 */
#include <stdlib.h>
#include <string.h>
#include "iap.h"

#define SECTOR 512u

typedef struct
{
    uint8_t sfr_base;   // address of the DATA register; ADDRH..CONTR follow
    uint8_t trig1, trig2;
    int program_is_code; // IAP address space is program memory itself
    uint32_t lo, hi;     // writable IAP address range [lo, hi)
} profile_t;

static const profile_t PROFILES[] = {
    // EN-271 ch. 9: E2h-E7h, 46h/B9h, Data Flash 2000h-2FFFh (STC89C52RC).
    {0xE2, 0x46, 0xB9, 0, 0x2000, 0x3000},
    // STC15 datasheet p. 487ff: C2h-C7h, 5Ah/A5h; whole program area.
    // Upper bound 0xF400 mirrors the STC89 RD+ tables' F3FFh ceiling; the
    // bootloader itself decides what it refuses to touch.
    {0xC2, 0x5A, 0xA5, 1, 0x0000, 0xF400},
};

struct iap
{
    struct em8051 *cpu;
    profile_t p;
    uint8_t *flash;       // data flash (STC89) or == cpu->mCodeMem (IAP15)
    uint8_t *own_flash;   // allocated only for the STC89 profile
    uint8_t last_trig;
    int pending_reset;    // 0 none, 1 = to AP, 2 = to ISP monitor
    uint8_t keep_contr;   // ISPEN|SWBS survive a software reset
    long stats[IAP_STAT_COUNT];
};

static uint8_t reg(const iap_t *d, int aOffset)
{
    return (uint8_t)(d->p.sfr_base - 0x80 + aOffset); // REG_* index
}

static uint8_t sfr(iap_t *d, int aOffset)
{
    return d->cpu->mSFR[reg(d, aOffset)];
}

static int writable(const iap_t *d, uint32_t aAddr)
{
    return aAddr >= d->p.lo && aAddr < d->p.hi;
}

static uint32_t index_of(const iap_t *d, uint32_t aAddr)
{
    return d->p.program_is_code ? aAddr : aAddr - d->p.lo;
}

static void run_command(iap_t *d)
{
    uint8_t contr = sfr(d, 5);
    // EN-271 p. 202 says "ISP_ADDRH[7:5] must be cleared to 000", but its own
    // table (p. 204) puts the STC89C52RC's Data Flash at 2000h-2FFFh, where
    // ADDRH = 20h has bit 5 set. The rule contradicts the documented range, so
    // it is not enforced; the address range check below is what matters.
    uint32_t addr = (uint32_t)sfr(d, 1) << 8 | sfr(d, 2);
    int cmd = sfr(d, 3) & 0x03; // MS1..0; STC89's MS2 is always 0 in use

    if (!(contr & 0x80)) // ISPEN / IAPEN clear: global disable
        return;
    if (cmd == 0)
        return;
    if (!writable(d, addr))
    {
        d->stats[IAP_STAT_IGNORED]++;
        return;
    }
    switch (cmd)
    {
    case 1: // read
        d->cpu->mSFR[reg(d, 0)] = d->flash[index_of(d, addr)];
        d->stats[IAP_STAT_READS]++;
        break;
    case 2: // byte program: can only clear bits
        d->flash[index_of(d, addr)] &= sfr(d, 0);
        d->stats[IAP_STAT_PROGRAMS]++;
        break;
    case 3: // sector erase: ADDRL ignored, whole 512-byte sector -> FFh
    {
        uint32_t start = addr & ~(uint32_t)(SECTOR - 1);
        if (!writable(d, start) || !writable(d, start + SECTOR - 1))
        {
            d->stats[IAP_STAT_IGNORED]++;
            return;
        }
        memset(d->flash + index_of(d, start), 0xFF, SECTOR);
        d->stats[IAP_STAT_ERASES]++;
        break;
    }
    }
}

static void on_trig(struct em8051 *aCPU, uint8_t aReg, uint8_t aValue, void *aUserData)
{
    iap_t *d = (iap_t *)aUserData;
    (void)aCPU;
    (void)aReg;
    if (d->last_trig == d->p.trig1 && aValue == d->p.trig2)
    {
        run_command(d);
        d->last_trig = 0;
        return;
    }
    d->last_trig = aValue;
}

static void on_contr(struct em8051 *aCPU, uint8_t aReg, uint8_t aValue, void *aUserData)
{
    iap_t *d = (iap_t *)aUserData;
    (void)aCPU;
    (void)aReg;
    d->keep_contr = (uint8_t)(aValue & 0xC0);
    if (aValue & 0x20) // SWRST: defer to after this instruction completes
        d->pending_reset = (aValue & 0x40) ? 2 : 1;
}

iap_t *iap_create(sim_bus_t *aBus, struct em8051 *aCPU, iap_profile_t aProfile)
{
    iap_t *d = (iap_t *)calloc(1, sizeof(iap_t));
    if (!d)
        return NULL;
    d->cpu = aCPU;
    d->p = PROFILES[aProfile == IAP_PROFILE_IAP15 ? 1 : 0];

    // Erased flash reads FFh; the simulator's code array starts zeroed.
    memset(aCPU->mCodeMem, 0xFF, (size_t)aCPU->mCodeMemMaxIdx + 1);
    if (d->p.program_is_code)
    {
        d->flash = aCPU->mCodeMem;
    }
    else
    {
        d->own_flash = (uint8_t *)malloc(d->p.hi - d->p.lo);
        memset(d->own_flash, 0xFF, d->p.hi - d->p.lo);
        d->flash = d->own_flash;
    }
    bus_on_write(aBus, reg(d, 4), on_trig, d);
    bus_on_write(aBus, reg(d, 5), on_contr, d);
    return d;
}

void iap_destroy(iap_t *aIap)
{
    if (!aIap)
        return;
    free(aIap->own_flash);
    free(aIap);
}

void iap_post_tick(iap_t *d)
{
    uint8_t keep;
    if (!d || !d->pending_reset)
        return;
    keep = d->keep_contr;
    if (d->pending_reset == 2)
        d->stats[IAP_STAT_ISP_ENTRIES]++;
    else
        d->stats[IAP_STAT_SOFT_RESETS]++;
    d->pending_reset = 0;
    // The ROM ISP monitor is not modelled: with no ISP host answering, the
    // real one software-resets back to the AP area (EN-271 p. 37), which is
    // what happens here in both cases.
    reset(d->cpu, 0);
    d->cpu->mSFR[reg(d, 5)] = keep; // ISPEN/SWBS survive a software reset
    d->last_trig = 0;
}

void iap_power_on(iap_t *d)
{
    if (!d)
        return;
    d->keep_contr = 0;
    d->pending_reset = 0;
    d->last_trig = 0;
}

int iap_peek(iap_t *d, int aAddress)
{
    if (!d || aAddress < 0 || !writable(d, (uint32_t)aAddress))
        return -1;
    return d->flash[index_of(d, (uint32_t)aAddress)];
}

int iap_poke(iap_t *d, int aAddress, int aValue)
{
    if (!d || aAddress < 0 || !writable(d, (uint32_t)aAddress))
        return -1;
    d->flash[index_of(d, (uint32_t)aAddress)] = (uint8_t)aValue;
    return 0;
}

long iap_stat(iap_t *d, int aWhich)
{
    if (!d || aWhich < 0 || aWhich >= IAP_STAT_COUNT)
        return 0;
    return d->stats[aWhich];
}
