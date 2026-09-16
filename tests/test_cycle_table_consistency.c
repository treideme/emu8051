/* 8051 emulator core -- per-opcode cycle cost consistency test
 *
 * Separate from test_machine_cycles.c on purpose.  That test measures
 * elapsed ticks; this one looks at the delay an opcode handler asks for,
 * because four handlers ask for the wrong one and the error is invisible
 * in elapsed ticks (a handler returning 0 and a handler returning 1 both
 * occupy one tick under the current tick()).
 *
 * The check is deliberately convention-free.  Across all of opcodes.c the
 * handlers return only three distinct values -- 0, 1 and 3 -- and Intel's
 * "MCS(R) 51 Microcontroller Family User's Manual" (272383-002, Feb 1994)
 * Table 10 gives exactly three distinct costs: 12, 24 and 48 oscillator
 * periods, i.e. 1, 2 and 4 machine cycles.  Whatever the mapping between
 * the two is, it is one-to-one, so two instructions Table 10 gives the
 * same cost must produce the same delay.  This test only asserts that.
 *
 * (i.e. the MIT License, same as the rest of the emulator)
 */

#include "testrig.h"

/* Delay left behind after the opcode handler at address 0 has run. */
static long delay_after(const unsigned char *aBytes, unsigned aCount)
{
    rig_init();
    rig_poke(0x0000, aBytes, aCount);
    tick(&rig);                   /* nothing pending: this executes it */
    return (long)rig.mTickDelay;
}

static const unsigned char ORL_C_BIT[]      = { 0x72, 0x20 };  /* 24 osc */
static const unsigned char ANL_C_BIT[]      = { 0x82, 0x20 };  /* 24 osc */
static const unsigned char ANL_C_NOTBIT[]   = { 0xb0, 0x20 };  /* 24 osc */
static const unsigned char ORL_C_NOTBIT[]   = { 0xa0, 0x20 };  /* 24 osc */
static const unsigned char MOV_BIT_C[]      = { 0x92, 0x20 };  /* 24 osc */
static const unsigned char MOVC_A_DPTR[]    = { 0x93 };        /* 24 osc */
static const unsigned char MOVC_A_PC[]      = { 0x83 };        /* 24 osc */
static const unsigned char MOV_C_BIT[]      = { 0xa2, 0x20 };  /* 12 osc */
static const unsigned char SETB_BIT[]       = { 0xd2, 0x20 };  /* 12 osc */
static const unsigned char MUL_AB[]         = { 0xa4 };        /* 48 osc */

#define DELAY(x) delay_after(x, (unsigned)sizeof(x))

int main(void)
{
    long two_cycle = DELAY(ORL_C_BIT);   /* reference for 24 oscillator periods */
    long one_cycle = DELAY(SETB_BIT);    /* reference for 12 oscillator periods */

    printf("per-opcode delay, grouped by Intel Table 10 cost\n");

    /* Already consistent -- these are the control group. */
    rig_check("MOV bit,C  == ORL C,bit  (24 osc)", DELAY(MOV_BIT_C),    two_cycle);
    rig_check("MOVC A,@A+DPTR == ORL C,bit (24)",  DELAY(MOVC_A_DPTR),  two_cycle);
    rig_check("MOV C,bit  == SETB bit   (12 osc)", DELAY(MOV_C_BIT),    one_cycle);

    /* The four that disagree with instructions Table 10 costs identically. */
    rig_check("ANL C,bit  == ORL C,bit  (24 osc)", DELAY(ANL_C_BIT),    two_cycle);
    rig_check("ANL C,/bit == ORL C,bit  (24 osc)", DELAY(ANL_C_NOTBIT), two_cycle);
    rig_check("ORL C,/bit == ORL C,bit  (24 osc)", DELAY(ORL_C_NOTBIT), two_cycle);
    rig_check("MOVC A,@A+PC == MOVC A,@A+DPTR",    DELAY(MOVC_A_PC),    DELAY(MOVC_A_DPTR));

    /* Negative control: instructions Table 10 costs differently must not
     * land on the same delay, or this file proves nothing. */
    if (one_cycle == two_cycle)
    {
        printf("  FAIL  %-42s both %ld\n", "negative control (12 osc != 24 osc)", one_cycle);
        rig_failures++;
    }
    else
    {
        printf("  ok    %-42s %ld != %ld\n", "negative control (12 osc != 24 osc)",
               one_cycle, two_cycle);
    }
    if (DELAY(MUL_AB) == two_cycle)
    {
        printf("  FAIL  %-42s both %ld\n", "negative control (48 osc != 24 osc)", two_cycle);
        rig_failures++;
    }
    else
    {
        printf("  ok    %-42s %ld != %ld\n", "negative control (48 osc != 24 osc)",
               DELAY(MUL_AB), two_cycle);
    }

    return rig_report("test_cycle_table_consistency");
}
