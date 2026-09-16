/* 8051 emulator core -- machine-cycle regression test
 *
 * Every instruction below is checked against the machine-cycle count
 * published in Intel's "MCS(R) 51 Microcontroller Family User's Manual"
 * (order number 272383-002, February 1994), Table 10 "8051 Instruction Set
 * Summary", printed pages 2-21 through 2-24.  That table gives the cost in
 * oscillator periods; one machine cycle is 12 oscillator periods, so 12/24/48
 * in the table is 1/2/4 machine cycles here.
 *
 * A tick() of this emulator is one machine cycle -- emu8051.h says so
 * ("run one emulator tick, or 12 hardware clock cycles") and emu.c counts
 * `clocks += 12` per tick to drive its real-time display.
 *
 * (i.e. the MIT License, same as the rest of the emulator)
 */

#include "testrig.h"

struct insn_cost
{
    const char *name;
    unsigned char bytes[3];
    unsigned len;
    uint16_t next_pc;   /* address of the instruction that follows */
    unsigned cycles;    /* Intel Table 10, converted to machine cycles */
};

static const struct insn_cost costs[] =
{
    /* --- one machine cycle (12 oscillator periods) --- */
    { "NOP",                {0x00},             1, 1, 1 },
    { "INC A",              {0x04},             1, 1, 1 },
    { "ADD A,#data",        {0x24, 0x01},       2, 2, 1 },
    { "MOV A,#data",        {0x74, 0x00},       2, 2, 1 },
    { "MOV A,direct",       {0xe5, 0x30},       2, 2, 1 },
    { "MOV direct,A",       {0xf5, 0x30},       2, 2, 1 },
    { "MOV A,Rn",           {0xe8},             1, 1, 1 },
    { "MOV Rn,A",           {0xf8},             1, 1, 1 },
    { "MOV @Ri,#data",      {0x76, 0x11},       2, 2, 1 },
    { "DA A",               {0xd4},             1, 1, 1 },
    { "CLR C",              {0xc3},             1, 1, 1 },
    { "SETB bit",           {0xd2, 0x20},       2, 2, 1 },
    { "MOV C,bit",          {0xa2, 0x20},       2, 2, 1 },
    { "XCH A,direct",       {0xc5, 0x30},       2, 2, 1 },
    { "XCHD A,@Ri",         {0xd6},             1, 1, 1 },
    { "ANL A,direct",       {0x55, 0x30},       2, 2, 1 },
    { "INC direct",         {0x05, 0x30},       2, 2, 1 },
    { "ORL direct,A",       {0x42, 0x30},       2, 2, 1 },

    /* --- two machine cycles (24 oscillator periods) --- */
    { "MOV direct,#data",   {0x75, 0x30, 0x55}, 3, 3, 2 },
    { "MOV DPTR,#data16",   {0x90, 0x12, 0x34}, 3, 3, 2 },
    { "MOV direct,direct",  {0x85, 0x30, 0x31}, 3, 3, 2 },
    { "MOV direct,@Ri",     {0x86, 0x30},       2, 2, 2 },
    { "MOV @Ri,direct",     {0xa6, 0x30},       2, 2, 2 },
    { "MOV direct,Rn",      {0x88, 0x30},       2, 2, 2 },
    { "MOV Rn,direct",      {0xa8, 0x30},       2, 2, 2 },
    { "MOV bit,C",          {0x92, 0x20},       2, 2, 2 },
    { "ORL C,bit",          {0x72, 0x20},       2, 2, 2 },
    { "ANL direct,#data",   {0x53, 0x30, 0x0f}, 3, 3, 2 },
    { "INC DPTR",           {0xa3},             1, 1, 2 },
    { "MOVC A,@A+DPTR",     {0x93},             1, 1, 2 },
    { "MOVX A,@DPTR",       {0xe0},             1, 1, 2 },
    { "MOVX @DPTR,A",       {0xf0},             1, 1, 2 },
    { "MOVX A,@Ri",         {0xe2},             1, 1, 2 },
    { "PUSH direct",        {0xc0, 0x30},       2, 2, 2 },
    { "POP direct",         {0xd0, 0x30},       2, 2, 2 },
    { "SJMP rel",           {0x80, 0x00},       2, 2, 2 },
    { "AJMP addr11",        {0x01, 0x02},       2, 2, 2 },
    { "LJMP addr16",        {0x02, 0x00, 0x03}, 3, 3, 2 },
    { "LCALL addr16",       {0x12, 0x00, 0x03}, 3, 3, 2 },
    { "JZ rel",             {0x60, 0x00},       2, 2, 2 },
    { "JC rel",             {0x40, 0x00},       2, 2, 2 },
    { "JB bit,rel",         {0x20, 0x20, 0x00}, 3, 3, 2 },
    { "JNB bit,rel",        {0x30, 0x20, 0x00}, 3, 3, 2 },
    { "JBC bit,rel",        {0x10, 0x20, 0x00}, 3, 3, 2 },
    { "CJNE A,#data,rel",   {0xb4, 0x00, 0x00}, 3, 3, 2 },
    { "DJNZ direct,rel",    {0xd5, 0x30, 0x00}, 3, 3, 2 },

    /* --- four machine cycles (48 oscillator periods) --- */
    { "MUL AB",             {0xa4},             1, 1, 4 },
    { "DIV AB",             {0x84},             1, 1, 4 },
};

static unsigned measure(const struct insn_cost *c)
{
    rig_init();
    return rig_cycles_of(c->bytes, c->len, c->next_pc);
}

int main(void)
{
    unsigned i;

    printf("machine cycles per instruction (Intel 272383-002, Table 10)\n");
    for (i = 0; i < sizeof(costs) / sizeof(costs[0]); i++)
        rig_check(costs[i].name, measure(&costs[i]), costs[i].cycles);


    /* The interrupt vector entry is a hardware-generated LCALL.  Intel's
     * manual, "Hardware Description of the 8051, 8052 and 80C51" ->
     * Response Time (printed page 3-25), states "The call itself takes two
     * cycles."  This check exists so that any change to the cycle
     * bookkeeping has to keep that true as well. */
    {
        unsigned arrive, entry;
        rig_init();
        rig.mSFR[REG_IE] = IEMASK_EA | IEMASK_ET0;
        rig.mSFR[REG_TCON] |= TCONMASK_TF0;
        arrive = rig_run_until_exec(ISR_TF0, 32); /* ticks until the ISR starts */
        entry = arrive ? arrive - 1 : 0;          /* drop the ISR's own cycle */
        rig_check("interrupt vector entry (LCALL)", entry, 2);
    }

    /* Negative control.  A suite that can only pass is not a measurement:
     * assert something known to be false and require the comparison to
     * notice.  NOP is one machine cycle, so this must NOT match. */
    {
        unsigned n = measure(&costs[0]);
        if (n == 2)
        {
            printf("  FAIL  negative control                        NOP measured as 2 cycles\n");
            rig_failures++;
        }
        else
        {
            printf("  ok    %-42s %u != 2\n", "negative control (NOP is not 2 cycles)", n);
        }
    }

    return rig_report("test_machine_cycles");
}
