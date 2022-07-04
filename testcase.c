/* 8051 emulator test case
 * Copyright 2022 Thomas Reidemeister
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
 * Simple testcase just using core functions of emu8051
 */

#ifdef _MSC_VER
#include <windows.h>
#undef MOUSE_MOVED
#else
#include <sys/time.h>
#include <unistd.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __linux__
#include <curses.h>
#else
#include "curses.h"
#endif
#include "emu8051.h"
#include "emulator.h"

int runmode = 0;
// current run speed, lower is faster
int speed = 6;
// instruction count; needed to replay history correctly
unsigned int icount = 0;
// current clock count
unsigned int clocks = 0;

// returns time in 1ms units
int getTick()
{
#ifdef _MSC_VER
  return GetTickCount();
#else
  struct timeval now;
  gettimeofday(&now, NULL);
  return now.tv_sec * 1000 + now.tv_usec / 1000;
#endif
}

void emu_sleep(int value)
{
#ifdef _MSC_VER
  Sleep(value);
#else
  usleep(value * 1000);
#endif
}

void emu_exception(struct em8051 *aCPU, int aCode)
{
  (void)aCPU;
  switch (aCode)
  {
    case EXCEPTION_IRET_SP_MISMATCH:
      break;
    case EXCEPTION_IRET_ACC_MISMATCH:
      break;
    case EXCEPTION_IRET_PSW_MISMATCH:
      break;
    case EXCEPTION_ACC_TO_A:
      break;
    case EXCEPTION_STACK:
      break;
    case EXCEPTION_ILLEGAL_OPCODE:
      break;
  }
}

int emu_sfrread(struct em8051 *aCPU, int aRegister)
{
  return aCPU->mSFR[aRegister - 0x80];
}

int main(int parc, char ** pars)
{
  struct em8051 emu;
  int ticked = 1;

  memset(&emu, 0, sizeof(emu));
  emu.mCodeMem     = malloc(65536);
  emu.mCodeMemSize = 65536;
  emu.mExtData     = malloc(65536);
  emu.mExtDataSize = 65536;
  emu.mLowerData   = malloc(128);
  emu.mUpperData   = malloc(128);
  emu.mSFR         = malloc(128);
  emu.except       = &emu_exception;
  emu.sfrread      = &emu_sfrread;
  emu.xread = NULL;
  emu.xwrite = NULL;
  reset(&emu, 1);

  if(parc != 2) {
    fprintf(stderr, "Please provide a hex file for this test-case\n");
    return EXIT_FAILURE;
  }
  if (load_obj(&emu, pars[1]) != 0)
  {
    fprintf(stderr, "File '%s' load failure\n\n",pars[1]);
    return EXIT_FAILURE;
  }

  do {
    clocks += 12;
    ticked = tick(&emu);

    if (ticked) {
      icount++;
    }

    // Sucessfully abort when P2 is changed from 0xFF to 0xFE
    if(emu.mSFR[REG_P2] != 0xFF) {
      if(emu.mSFR[REG_P2] == 0xFE) {
        printf("Successfully toggled P2.0 after %i instructions and %i cycles\n", icount, clocks);
        return EXIT_SUCCESS;
      } else {
        fprintf(stderr, "Unexpected change of P2 after %i instructions and %i cycles", icount, clocks);
        return EXIT_FAILURE;
      }
    }
  } while(icount < 12000);
  fprintf(stderr, "Did not observe expected state change\n");

  return EXIT_FAILURE;
}