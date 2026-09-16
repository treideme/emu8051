/* 8051 emulator core -- stack above 7FH test
 *
 * Intel's "MCS(R) 51 Microcontroller Family User's Manual" (272383-002,
 * Feb 1994), MCS-51 Architectural Overview, printed page 1-13:
 *
 *   "... but the stack itself is accessed by indirect addressing using the
 *    SP register.  This means the stack can go into the Upper 128, if they
 *    are implemented, but not into SFR space."
 *
 * and, four pages earlier (printed page 1-9): "The Upper 128 ... can only
 * be accessed by indirect addressing", while the SFRs "can only be
 * accessed by direct addressing".
 *
 * emu.c builds a part with the Upper 128 present (mUpperData is allocated
 * at startup), and SDCC puts the stack straight through 7FH on any 8052
 * target, so this is the ordinary case rather than a corner.
 *
 * (i.e. the MIT License, same as the rest of the emulator)
 */

#include "testrig.h"

static void run_at(uint16_t aStart, const unsigned char *aBytes, unsigned aCount,
                   unsigned aInsns)
{
    unsigned retired = 0, guard = 0;
    rig_poke(aStart, aBytes, aCount);
    rig.mPC = aStart;
    while (retired < aInsns && guard++ < 10000)
        if (tick(&rig))
            retired++;
}

int main(void)
{
    /* PUSH with SP crossing 7FH.  The byte belongs in the Upper 128; P0
     * lives at direct address 80H and must not see it. */
    {
        /* MOV SP,#7FH | MOV A,#5AH | PUSH ACC */
        static const unsigned char p[] = { 0x75, 0x81, 0x7f, 0x74, 0x5a, 0xc0, 0xe0 };
        rig_init();
        run_at(0x0000, p, sizeof(p), 3);
        rig_check("PUSH ACC    SP",               rig.mSFR[REG_SP],     0x80);
        rig_check("PUSH ACC    upper RAM (80H)",  rig.mUpperData[0x00], 0x5a);
        rig_check("PUSH ACC    SFR P0 untouched", rig.mSFR[REG_P0],     0xff);
    }

    /* ... and POP it back. */
    {
        /* MOV SP,#7FH | MOV A,#5AH | PUSH ACC | POP 30H */
        static const unsigned char p[] =
            { 0x75, 0x81, 0x7f, 0x74, 0x5a, 0xc0, 0xe0, 0xd0, 0x30 };
        rig_init();
        run_at(0x0000, p, sizeof(p), 4);
        rig_check("POP 30H     value",            rig.mLowerData[0x30], 0x5a);
        rig_check("POP 30H     SP",               rig.mSFR[REG_SP],     0x7f);
        rig_check("POP 30H     SFR P0 untouched", rig.mSFR[REG_P0],     0xff);
    }

    /* A subroutine call and return across the same boundary -- both return
     * address bytes land above 7FH. */
    {
        /* 0100: MOV SP,#7FH | 0103: LCALL 0200H | 0106: NOP   0200: RET */
        static const unsigned char p[]   = { 0x75, 0x81, 0x7f, 0x12, 0x02, 0x00, 0x00 };
        static const unsigned char sub[] = { 0x22 };
        rig_init();
        rig_poke(0x0200, sub, sizeof(sub));
        run_at(0x0100, p, sizeof(p), 3);          /* MOV, LCALL, RET */
        rig_check("LCALL/RET   upper RAM (80H)",  rig.mUpperData[0x00], 0x06);
        rig_check("LCALL/RET   upper RAM (81H)",  rig.mUpperData[0x01], 0x01);
        rig_check("LCALL/RET   SFR P0 untouched", rig.mSFR[REG_P0],     0xff);
        rig_check("LCALL/RET   SFR SP restored",  rig.mSFR[REG_SP],     0x7f);
        rig_check("LCALL/RET   returned to",      rig.mPC,              0x0106);
    }

    /* Control: the same sequence entirely inside the Lower 128, which the
     * direct and indirect paths agree about. */
    {
        /* MOV SP,#30H | MOV A,#5AH | PUSH ACC */
        static const unsigned char p[] = { 0x75, 0x81, 0x30, 0x74, 0x5a, 0xc0, 0xe0 };
        rig_init();
        run_at(0x0000, p, sizeof(p), 3);
        rig_check("PUSH ACC    lower RAM (31H)",  rig.mLowerData[0x31], 0x5a);
        rig_check("PUSH ACC    SP (control)",     rig.mSFR[REG_SP],     0x31);
    }

    return rig_report("test_stack_upper_ram");
}
