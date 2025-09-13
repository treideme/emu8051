/* 8051 emulator core -- MOV direct,@Ri test
 *
 * Intel's "MCS(R) 51 Microcontroller Family User's Manual" (272383-002,
 * Feb 1994), Table 10 "8051 Instruction Set Summary", printed page 2-22,
 * lists opcode 1000 011i as "MOV direct,@Ri -- Move indirect RAM to direct
 * byte".  The instruction carries exactly one direct address, and that
 * address is the destination; @Ri is the source.  Its counterpart,
 * MOV @Ri,direct (1010 011i), goes the other way.
 *
 * (i.e. the MIT License, same as the rest of the emulator)
 */

#include "testrig.h"

static void run_program(const unsigned char *aBytes, unsigned aCount, unsigned aInsns)
{
    unsigned retired = 0, guard = 0;
    rig_init();
    rig_poke(0x0000, aBytes, aCount);
    while (retired < aInsns && guard++ < 10000)
        if (tick(&rig))
            retired++;
}

int main(void)
{
    /* MOV 30H,@R1 with R1 = 40H and (40H) = AAH.  30H is loaded from 40H;
     * 40H is a source and does not change. */
    {
        /* MOV R1,#40H | MOV A,#AAH | MOV @R1,A | MOV 30H,#11H | MOV 30H,@R1 */
        static const unsigned char p[] =
            { 0x79, 0x40, 0x74, 0xaa, 0xf7, 0x75, 0x30, 0x11, 0x87, 0x30 };
        run_program(p, sizeof(p), 5);
        rig_check("MOV 30H,@R1  destination (30H)", rig.mLowerData[0x30], 0xaa);
        rig_check("MOV 30H,@R1  source (40H) kept", rig.mLowerData[0x40], 0xaa);
    }

    /* Same instruction with an SFR as the destination -- the direct operand
     * is a direct address, so P1 is a legal target. */
    {
        /* MOV R1,#40H | MOV A,#AAH | MOV @R1,A | MOV P1,@R1 */
        static const unsigned char p[] =
            { 0x79, 0x40, 0x74, 0xaa, 0xf7, 0x87, 0x90 };
        run_program(p, sizeof(p), 4);
        rig_check("MOV P1,@R1   destination P1",    rig.mSFR[REG_P1],     0xaa);
        rig_check("MOV P1,@R1   source (40H) kept", rig.mLowerData[0x40], 0xaa);
    }

    /* Source in the Upper 128, which only indirect addressing can reach. */
    {
        /* MOV R1,#90H | MOV A,#5AH | MOV @R1,A | MOV 30H,@R1 */
        static const unsigned char p[] =
            { 0x79, 0x90, 0x74, 0x5a, 0xf7, 0x87, 0x30 };
        run_program(p, sizeof(p), 4);
        rig_check("MOV 30H,@R1  from upper RAM",    rig.mLowerData[0x30], 0x5a);
        rig_check("MOV 30H,@R1  upper RAM kept",    rig.mUpperData[0x10], 0x5a);
    }

    /* Control: the opposite instruction, MOV @Ri,direct, which shares the
     * operand decode and the memory helpers. */
    {
        /* MOV R0,#41H | MOV 31H,#55H | MOV @R0,31H */
        static const unsigned char p[] =
            { 0x78, 0x41, 0x75, 0x31, 0x55, 0xa6, 0x31 };
        run_program(p, sizeof(p), 3);
        rig_check("MOV @R0,31H  destination (41H)", rig.mLowerData[0x41], 0x55);
        rig_check("MOV @R0,31H  source (31H) kept", rig.mLowerData[0x31], 0x55);
    }

    return rig_report("test_mov_direct_indir");
}
