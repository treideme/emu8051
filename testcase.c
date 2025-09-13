/* 8051 emulator test case
 * Copyright 2025 Thomas Reidemeister
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject
 * to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * (i.e. the MIT License)
 *
 * testcase.c, stripped down version of emu.c
 * Simple testcase just using core functions of emu8051 -- no curses, no
 * peripheral models (see sim/ + sim/capi.h for those). This exists purely
 * as a minimal, dependency-free smoke test that the core itself still
 * builds and runs: does the CPU boot and execute the expected first port
 * write. Rewritten against the current struct em8051 layout (fixed-size
 * mLowerData/mSFR arrays, mCodeMemMaxIdx/mExtDataMaxIdx, per-register
 * sfrread[]/sfrwrite[] callback arrays) -- the previous version predated a
 * core refactor and no longer matched emu8051.h at all.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "emu8051.h"

static void emu_exception(struct em8051 *aCPU, int aCode)
{
    (void)aCPU;
    (void)aCode;
}

int main(int parc, char **pars)
{
    struct em8051 emu;
    int ticked;
    unsigned int icount = 0;
    unsigned int clocks = 0;

    memset(&emu, 0, sizeof(emu));
    emu.mCodeMemMaxIdx = 65535;
    emu.mCodeMem = calloc(emu.mCodeMemMaxIdx + 1, sizeof(unsigned char));
    emu.mExtDataMaxIdx = 65535;
    emu.mExtData = calloc(emu.mExtDataMaxIdx + 1, sizeof(unsigned char));
    emu.mUpperData = calloc(128, sizeof(unsigned char));
    emu.except = emu_exception;
    reset(&emu, 1);

    if (parc != 2)
    {
        fprintf(stderr, "Please provide a hex file for this test-case\n");
        return EXIT_FAILURE;
    }
    if (load_obj(&emu, pars[1]) != 0)
    {
        fprintf(stderr, "File '%s' load failure\n\n", pars[1]);
        return EXIT_FAILURE;
    }

    do
    {
        clocks += 12;
        ticked = tick(&emu);
        if (ticked)
            icount++;

        // Succeed as soon as P2 is changed from its 0xff init value to 0xfe
        // (the "toggle P2.0 low" pattern every LED demo in this project's
        // sibling repos starts with).
        if (emu.mSFR[REG_P2] != 0xFF)
        {
            if (emu.mSFR[REG_P2] == 0xFE)
            {
                printf("Successfully toggled P2.0 after %u instructions and %u cycles\n", icount, clocks);
                free(emu.mCodeMem);
                free(emu.mExtData);
                free(emu.mUpperData);
                return EXIT_SUCCESS;
            }
            fprintf(stderr, "Unexpected change of P2 (0x%02x) after %u instructions and %u cycles\n",
                    emu.mSFR[REG_P2], icount, clocks);
            free(emu.mCodeMem);
            free(emu.mExtData);
            free(emu.mUpperData);
            return EXIT_FAILURE;
        }
    } while (icount < 12000);

    fprintf(stderr, "Did not observe expected state change\n");
    free(emu.mCodeMem);
    free(emu.mExtData);
    free(emu.mUpperData);
    return EXIT_FAILURE;
}
