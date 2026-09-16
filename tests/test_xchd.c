/* 8051 emulator core -- XCHD A,@Ri test
 *
 * Intel's "MCS(R) 51 Microcontroller Family User's Manual" (272383-002,
 * Feb 1994), printed page 2-73:
 *
 *   "XCHD exchanges the low-order nibble of the Accumulator (bits 3-0)
 *    ... with that of the internal RAM location indirectly addressed by
 *    the specified register.  The high-order nibbles (bits 7-4) of each
 *    register are not affected."
 *
 * and the worked example printed with it: R0 = 20H, A = 36H, internal RAM
 * 20H = 75H; "XCHD A,@R0 will leave RAM location 20H holding the value
 * 76H ... and 35H in the Accumulator."
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
    /* Intel's worked example, in the Lower 128. */
    {
        /* MOV R0,#20H | MOV A,#75H | MOV @R0,A | MOV A,#36H | XCHD A,@R0 */
        static const unsigned char p[] =
            { 0x78, 0x20, 0x74, 0x75, 0xf6, 0x74, 0x36, 0xd6 };
        run_program(p, sizeof(p), 5);
        rig_check("XCHD A,@R0  A       (36H/75H)", rig.mSFR[REG_ACC],      0x35);
        rig_check("XCHD A,@R0  (20H)   (36H/75H)", rig.mLowerData[0x20],   0x76);
    }

    /* Same instruction reaching the Upper 128, which only indirect
     * addressing can see. */
    {
        /* MOV R0,#90H | MOV A,#75H | MOV @R0,A | MOV A,#36H | XCHD A,@R0 */
        static const unsigned char p[] =
            { 0x78, 0x90, 0x74, 0x75, 0xf6, 0x74, 0x36, 0xd6 };
        run_program(p, sizeof(p), 5);
        rig_check("XCHD A,@R0  A       (upper RAM)", rig.mSFR[REG_ACC],    0x35);
        rig_check("XCHD A,@R0  (90H)   (upper RAM)", rig.mUpperData[0x10], 0x76);
    }

    /* Control: the full-byte exchange next to it, XCH A,@Ri, which shares
     * the same operand decode and the same memory helpers. */
    {
        /* MOV R0,#20H | MOV A,#75H | MOV @R0,A | MOV A,#36H | XCH A,@R0 */
        static const unsigned char p[] =
            { 0x78, 0x20, 0x74, 0x75, 0xf6, 0x74, 0x36, 0xc6 };
        run_program(p, sizeof(p), 5);
        rig_check("XCH  A,@R0  A       (control)", rig.mSFR[REG_ACC],      0x75);
        rig_check("XCH  A,@R0  (20H)   (control)", rig.mLowerData[0x20],   0x36);
    }

    /* Stated the other way round, without naming an expected value: after
     * an exchange the memory byte cannot still hold what it held before. */
    {
        static const unsigned char p[] =
            { 0x78, 0x20, 0x74, 0x75, 0xf6, 0x74, 0x36, 0xd6 };
        run_program(p, sizeof(p), 5);
        if (rig.mLowerData[0x20] == 0x75)
        {
            printf("  FAIL  %-42s unchanged at 75H\n", "(20H) must not still hold 75H");
            rig_failures++;
        }
        else
        {
            printf("  ok    %-42s %02lXH != 75H\n", "(20H) must not still hold 75H",
                   (unsigned long)rig.mLowerData[0x20]);
        }
    }

    return rig_report("test_xchd");
}
