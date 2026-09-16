/* 8051 emulator core -- auxiliary carry (PSW.AC) test
 *
 * Intel's "MCS(R) 51 Microcontroller Family User's Manual" (272383-002,
 * Feb 1994) defines AC by the nibble boundary:
 *
 *   ADD  (printed page 2-29): "The carry and auxiliary-carry flags are set,
 *        respectively, if there is a carry-out from bit 7 or bit 3".
 *   SUBB (printed page 2-70): "AC is set if a borrow is needed for bit 3".
 *
 * The first three cases below are the worked examples printed in that
 * manual, quoted with their stated results.  The rest are the same rule
 * applied to values that separate a carry out of bit 3 from a carry out of
 * bit 2, plus the BCD results DA A produces from them -- DA A reads AC
 * (printed page 2-39), so a wrong AC becomes a wrong decimal digit.
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

static long ac(void)  { return (rig.mSFR[REG_PSW] & PSWMASK_AC) ? 1 : 0; }
static long cy(void)  { return (rig.mSFR[REG_PSW] & PSWMASK_C)  ? 1 : 0; }
static long ov(void)  { return (rig.mSFR[REG_PSW] & PSWMASK_OV) ? 1 : 0; }
static long acc(void) { return rig.mSFR[REG_ACC]; }

int main(void)
{
    /* Intel, DA A example (printed page 2-40): A=56H, R3=67H, carry set.
     * "ADDC A,R3 ... resulting in the value 0BEH in the Accumulator.  The
     * carry and auxiliary carry flags will be cleared." */
    {
        static const unsigned char p[] = { 0xd3, 0x74, 0x56, 0x7b, 0x67, 0x3b };
        run_program(p, sizeof(p), 4);
        rig_check("ADDC A,R3  (56H+67H+1) ACC",  acc(), 0xbe);
        rig_check("ADDC A,R3  (56H+67H+1) CY",   cy(),  0);
        rig_check("ADDC A,R3  (56H+67H+1) AC",   ac(),  0);
    }

    /* Intel, SUBB example (printed page 2-70): A=C9H, R2=54H, carry set.
     * "will leave the value 74H in the accumulator, with the carry flag and
     * AC cleared but OV set." */
    {
        static const unsigned char p[] = { 0xd3, 0x74, 0xc9, 0x7a, 0x54, 0x9a };
        run_program(p, sizeof(p), 4);
        rig_check("SUBB A,R2  (C9H-54H-1) ACC",  acc(), 0x74);
        rig_check("SUBB A,R2  (C9H-54H-1) CY",   cy(),  0);
        rig_check("SUBB A,R2  (C9H-54H-1) AC",   ac(),  0);
        rig_check("SUBB A,R2  (C9H-54H-1) OV",   ov(),  1);
    }

    /* Intel, ADD example (printed page 2-29): A=C3H, R0=AAH.  "will leave
     * 6DH in the Accumulator with the AC flag cleared and both the carry
     * flag and OV set to 1."  This one already passes -- it is here as the
     * control that says the rig reads the right bits. */
    {
        static const unsigned char p[] = { 0xc3, 0x74, 0xc3, 0x78, 0xaa, 0x28 };
        run_program(p, sizeof(p), 4);
        rig_check("ADD A,R0    (C3H+AAH) ACC",   acc(), 0x6d);
        rig_check("ADD A,R0    (C3H+AAH) CY",    cy(),  1);
        rig_check("ADD A,R0    (C3H+AAH) AC",    ac(),  0);
        rig_check("ADD A,R0    (C3H+AAH) OV",    ov(),  1);
    }

    /* The rule, on values that separate bit 3 from bit 2.
     * 4+4 = 8: no carry out of bit 3 (but there is one out of bit 2). */
    {
        static const unsigned char p[] = { 0xc3, 0x74, 0x04, 0x24, 0x04 };
        run_program(p, sizeof(p), 3);
        rig_check("ADD A,#4       (A=4)  AC",    ac(),  0);
    }
    /* 8+8 = 10H: carry out of bit 3 (but none out of bit 2). */
    {
        static const unsigned char p[] = { 0xc3, 0x74, 0x08, 0x24, 0x08 };
        run_program(p, sizeof(p), 3);
        rig_check("ADD A,#8       (A=8)  AC",    ac(),  1);
    }
    /* 07H - 08H: borrow needed for bit 3, none for bit 2. */
    {
        static const unsigned char p[] = { 0xc3, 0x74, 0x07, 0x94, 0x08 };
        run_program(p, sizeof(p), 3);
        rig_check("SUBB A,#8      (A=7)  AC",    ac(),  1);
    }
    /* 0AH - 04H: no borrow for bit 3, but one for bit 2. */
    {
        static const unsigned char p[] = { 0xc3, 0x74, 0x0a, 0x94, 0x04 };
        run_program(p, sizeof(p), 3);
        rig_check("SUBB A,#4      (A=0AH) AC",   ac(),  0);
    }

    /* What a wrong AC does to BCD arithmetic, which is what AC is for.
     * packed-BCD 04 + 04 = 08, and 08 + 08 = 16. */
    {
        static const unsigned char p[] = { 0xc3, 0x74, 0x04, 0x24, 0x04, 0xd4 };
        run_program(p, sizeof(p), 4);
        rig_check("BCD 04 + 04 -> DA A",         acc(), 0x08);
    }
    {
        static const unsigned char p[] = { 0xc3, 0x74, 0x08, 0x24, 0x08, 0xd4 };
        run_program(p, sizeof(p), 4);
        rig_check("BCD 08 + 08 -> DA A",         acc(), 0x16);
    }

    /* Negative control: AC must not be a constant.  Re-run the two ADD
     * cases and require the flag to differ between them. */
    {
        static const unsigned char p4[] = { 0xc3, 0x74, 0x04, 0x24, 0x04 };
        static const unsigned char p8[] = { 0xc3, 0x74, 0x08, 0x24, 0x08 };
        long a4, a8;
        run_program(p4, sizeof(p4), 3); a4 = ac();
        run_program(p8, sizeof(p8), 3); a8 = ac();
        if (a4 == a8)
        {
            printf("  FAIL  %-42s both %ld\n", "negative control (AC is not constant)", a4);
            rig_failures++;
        }
        else
        {
            printf("  ok    %-42s %ld != %ld\n", "negative control (AC is not constant)", a4, a8);
        }
    }

    return rig_report("test_aux_carry");
}
