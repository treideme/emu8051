/* 8051 emulator core -- shared test harness
 *
 * Header-only, dependency-free helper for the standalone regression tests
 * in this directory.  A test is one .c file plus core.c/opcodes.c/disasm.c;
 * there is no framework to install and nothing links against the curses
 * front end.
 *
 * (i.e. the MIT License, same as the rest of the emulator)
 */

#ifndef EMU8051_TESTRIG_H
#define EMU8051_TESTRIG_H

#include <stdio.h>
#include <string.h>
#include "emu8051.h"

static struct em8051 rig;
static unsigned char rig_code[0x10000];
static unsigned char rig_ext[0x10000];
static unsigned char rig_upper[128];
static int rig_failures = 0;

/* An 8052-shaped part: 256 bytes of internal RAM, 64k code, 64k xdata.
 * This is the same shape emu.c builds at startup. */
static inline void rig_init(void)
{
    memset(&rig, 0, sizeof(rig));
    memset(rig_code, 0, sizeof(rig_code));
    memset(rig_ext, 0, sizeof(rig_ext));
    memset(rig_upper, 0, sizeof(rig_upper));
    rig.mCodeMem = rig_code;
    rig.mCodeMemMaxIdx = 0xffff;
    rig.mExtData = rig_ext;
    rig.mExtDataMaxIdx = 0xffff;
    rig.mUpperData = rig_upper;
    reset(&rig, 1);
}

static inline void rig_poke(uint16_t aAddress, const unsigned char *aBytes, unsigned aCount)
{
    memcpy(rig_code + aAddress, aBytes, aCount);
}


/* Run until the instruction stored at aAddress is executed, and return how
 * many ticks that took, counting the executing tick itself.  Returns 0 if
 * aAddress was never reached within aBudget ticks.
 *
 * This is deliberately the measurement a simulator breakpoint makes: the
 * value returned by a second call, made immediately after the first, is the
 * number of machine cycles the instruction at the first address occupied. */
static inline unsigned rig_run_until_exec(uint16_t aAddress, unsigned aBudget)
{
    unsigned t;
    for (t = 1; t <= aBudget; t++)
    {
        uint16_t pc = rig.mPC;
        bool executed = tick(&rig);
        if (executed && pc == aAddress)
            return t;
    }
    return 0;
}

/* Machine cycles occupied by a single instruction placed at address 0,
 * where aNextPC is the address of the instruction that follows it. */
static inline unsigned rig_cycles_of(const unsigned char *aBytes, unsigned aCount, uint16_t aNextPC)
{
    rig_poke(0x0000, aBytes, aCount);
    if (rig_run_until_exec(0x0000, 8) == 0)
        return 0;
    return rig_run_until_exec(aNextPC, 64);
}

static inline void rig_check(const char *aWhat, long aGot, long aWant)
{
    if (aGot == aWant)
    {
        printf("  ok    %-42s %ld\n", aWhat, aGot);
    }
    else
    {
        printf("  FAIL  %-42s got %ld, expected %ld\n", aWhat, aGot, aWant);
        rig_failures++;
    }
}

static inline int rig_report(const char *aName)
{
    if (rig_failures)
        printf("%s: %d check(s) FAILED\n", aName, rig_failures);
    else
        printf("%s: all checks passed\n", aName);
    return rig_failures ? 1 : 0;
}

#endif /* EMU8051_TESTRIG_H */
