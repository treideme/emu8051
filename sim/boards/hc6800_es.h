/* Board pinout catalog: HC6800-ES V2.0 (Shenzhen Puzhong "51 MCU" teaching
 * board), as wired for stc89c52-demos / stc89c52-staging.
 * Copyright 2025 Thomas Reidemeister, MIT License (see ../devices/hc573.h)
 *
 * hc6800_es.h
 *
 * This is deliberately just a catalog of pin_t/xxx_pins_t constants, not a
 * "create the whole board" function: the real board doesn't populate every
 * peripheral at once. P3.4-P3.6 are wired to a DS1302 RTC module in some
 * demos and an XPT2046 ADC module in others -- different external modules
 * plugged into the same header, never both at once. A test picks whichever
 * of these constants its demo actually uses and creates only those device
 * instances. To support a different dev-kit, add a new file in this
 * directory with its own constants; every device model under sim/devices works
 * unchanged against it.
 *
 * Pin mapping cross-referenced against the board schematic and this
 * project's own already-working SDCC ports (see
 * stc89c52-staging/doc/sdcc-porting-notes.md and
 * stc89c52-staging/doc/simulation-notes.md for how this was derived).
 */
#ifndef SIM_BOARDS_HC6800_ES_H
#define SIM_BOARDS_HC6800_ES_H

#include "../pin.h"
#include "../devices/hc573.h"
#include "../devices/hc138.h"
#include "../devices/hd44780.h"
#include "../devices/ds1302.h"
#include "../devices/xpt2046.h"

#define HC6800_ES_XTAL_HZ 12000000UL

// P0: shared 8-bit data bus (7-segment latch input, HD44780/ST7920 data)
extern const pin_t HC6800_ES_P0[8];

// 74HC573 latching P0 into the 7-segment digit bank; LE on P1.0
extern const hc573_pins_t HC6800_ES_SEG_LATCH;

// 74LS138 digit-select decoder for the 7-segment bank; A/B/C on P2.2-P2.4,
// no separate enable lines on this board (permanently enabled)
extern const hc138_pins_t HC6800_ES_SEG_SELECT;

// HD44780 1602 character LCD: 8-bit parallel, data on P0, RW/RS/E on P2.5-7
extern const hd44780_pins_t HC6800_ES_LCD;

// DS1302 RTC: I/O, CE(RST), SCLK on P3.4-P3.6
extern const ds1302_pins_t HC6800_ES_DS1302;

// XPT2046 touch-ADC read as a generic 12-bit ADC: DIN/CS/CLK/DOUT on P3.4-P3.7
extern const xpt2046_pins_t HC6800_ES_XPT2046;

#endif // SIM_BOARDS_HC6800_ES_H
