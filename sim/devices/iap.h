/* STC ISP/IAP flash-programming SFR model.
 * Copyright 2026 Thomas Reidemeister, MIT License (see devices/hc573.h)
 *
 * iap.h
 *
 * Models the six ISP/IAP SFRs STC parts use to let firmware read, byte-program
 * and sector-erase on-chip flash, plus the ISP_CONTR software reset (SWRST) and
 * boot-select (SWBS) bits. Two families are covered, because they differ in
 * exactly the places a bootloader cares about:
 *
 *   IAP_PROFILE_STC89C52RC  STC89C51RC/RD+ datasheet (EN-271) ch. 9:
 *                           SFRs E2h-E7h, trigger 46h then B9h, 512-byte
 *                           sectors. From the application (AP) area, IAP can
 *                           only reach the separate Data Flash ("EEPROM"),
 *                           IAP addresses 2000h-2FFFh on the STC89C52RC;
 *                           "if the application area of IAP write Data/erase
 *                           sector of the action, the statements will be
 *                           ignore" (p. 204). Data Flash is its own array; it
 *                           is not program memory and cannot be executed.
 *
 *   IAP_PROFILE_IAP15       STC15 datasheet ch. on IAP, p. 487: "The user
 *                           program can directly modify the user program area
 *                           in the user program area for IAP15 series."
 *                           SFRs C2h-C7h, trigger 5Ah then A5h. Here the IAP
 *                           address space IS program memory: a programmed byte
 *                           is executable. Used to run a real app-rewriting
 *                           bootloader in simulation; a 12T/12 MHz core is
 *                           still assumed (the real IAP15 is 1T), so this
 *                           profile models the flash semantics, not the part.
 *
 * Flash semantics (both): erase sets a whole 512-byte sector to FFh; program
 * can only clear bits (new = old & data), exactly like real NOR flash; read
 * copies the byte into the data SFR. Commands need the enable bit (bit 7 of
 * the control SFR) set and the two trigger bytes written back to back.
 *
 * Not modelled (see sim/README.md): erase/program *time* (the wait-state
 * bits WT2..0 are stored but the CPU is not stalled), endurance, brown-out
 * during an operation (each command is atomic here), and STC's ROM ISP
 * monitor itself - a SWBS=1 software reset is counted and then falls back to
 * the application, which is what the real monitor does when no ISP host
 * answers (EN-271 p. 37, warm-boot table).
 */
#ifndef SIM_IAP_H
#define SIM_IAP_H

#include <stdint.h>
#include "../../emu8051.h"
#include "../bus.h"

typedef enum
{
    IAP_PROFILE_STC89C52RC = 0,
    IAP_PROFILE_IAP15 = 1,
} iap_profile_t;

typedef struct iap iap_t;

// Registers on aBus; aCodeMem is the CPU's 64 KB code array (the IAP15
// profile writes into it). Fills code memory with FFh so unprogrammed flash
// reads like erased flash: call before loading a hex image.
iap_t *iap_create(sim_bus_t *aBus, struct em8051 *aCPU, iap_profile_t aProfile);
void iap_destroy(iap_t *aIap);

// Call once per tick, after the instruction completes: performs a software
// reset requested by writing SWRST, outside the SFR-write callback (resetting
// mid-instruction would let that instruction's own PC update run after it).
void iap_post_tick(iap_t *aIap);

// Host power-on: clears ISPEN/SWBS (they survive only a software reset).
void iap_power_on(iap_t *aIap);

// Host-side view of the flash the IAP commands act on (Data Flash for the
// STC89 profile, code memory for IAP15). Returns -1 outside it.
int iap_peek(iap_t *aIap, int aAddress);
int iap_poke(iap_t *aIap, int aAddress, int aValue); // test setup only

enum
{
    IAP_STAT_READS = 0,
    IAP_STAT_PROGRAMS,
    IAP_STAT_ERASES,
    IAP_STAT_IGNORED,      // command aimed outside the writable range
    IAP_STAT_SOFT_RESETS,  // SWRST with SWBS = 0 (back to AP)
    IAP_STAT_ISP_ENTRIES,  // SWRST with SWBS = 1 (into the ROM ISP monitor)
    IAP_STAT_COUNT
};
long iap_stat(iap_t *aIap, int aWhich);

#endif // SIM_IAP_H
