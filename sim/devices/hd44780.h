/* HD44780-compatible character LCD controller model
 * Copyright 2025 Thomas Reidemeister
 * Adapted from the HD44780 state machine in ../../logicboard.c
 * (Copyright 2006 Jari Komppa), rewired from that file's hardcoded P1/P3
 * wiring onto the generic pin_t abstraction so it can be wired to any
 * board's actual pins.
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
 * hd44780.h
 *
 * Models the controller itself: command decode (clear/home/entry-mode/
 * display-on-off/cursor-shift/function-set/CGRAM-addr/DDRAM-addr), busy
 * flag timing, 80-byte DDRAM, 64-byte CGRAM, 4-bit and 8-bit interface
 * modes. Triggered by edges on the E (enable) pin; RS/R-W/data are simply
 * read as pin levels at the moment E edges, matching the real chip's bus
 * timing, so this doesn't need or care which SFR registers RS/R-W/data
 * actually live on.
 *
 * Only data[4..7] need be connected for 4-bit mode (DB4-DB7); leave
 * data[0..3] as PIN_UNCONNECTED. R-W may also be PIN_UNCONNECTED if the
 * board ties it permanently low (write-only wiring) -- busy-flag reads
 * then simply can't happen, which most of this project's demos never
 * attempt anyway (see the sibling demo repo's doc/simulation-notes.md).
 */
#ifndef SIM_DEVICES_HD44780_H
#define SIM_DEVICES_HD44780_H

#include <stddef.h>
#include "../bus.h"
#include "../pin.h"

typedef struct
{
    pin_t data[8]; // DB0..DB7; for 4-bit mode only data[4..7] need be set
    pin_t rs;
    pin_t rw; // PIN_UNCONNECTED if tied low on the board
    pin_t e;
} hd44780_pins_t;

typedef struct hd44780 hd44780_t;

// aClockHz is the CPU clock the busy-flag timing is scaled against (e.g.
// 12000000 for a 12MHz part) -- matches this project's convention of
// counting instruction/tick time rather than wall-clock time.
hd44780_t *hd44780_create(sim_bus_t *aBus, struct em8051 *aCPU, hd44780_pins_t aPins, unsigned long aClockHz);
void hd44780_destroy(hd44780_t *aDev);

// Advance the busy-flag countdown. Call once per tick()/instruction, same
// cadence aClockHz was given in.
void hd44780_step(hd44780_t *aDev);

// Raw DDRAM/CGRAM access.
uint8_t hd44780_get_ddram(const hd44780_t *aDev, uint8_t aAddress);
uint8_t hd44780_get_cgram(const hd44780_t *aDev, uint8_t aAddress);

// Convenience: read one display line as a NUL-terminated ASCII string
// (non-printable DDRAM bytes come back as ' '). aLine is 0-based; line
// start addresses follow the standard HD44780 convention (0=0x00, 1=0x40,
// 2=aWidth, 3=0x40+aWidth), which is what every 16x2/20x4 module uses.
// aBuf must hold at least aWidth+1 bytes. Returns aBuf.
char *hd44780_get_line(const hd44780_t *aDev, int aLine, uint8_t aWidth, char *aBuf, size_t aBufSize);

#endif // SIM_DEVICES_HD44780_H
