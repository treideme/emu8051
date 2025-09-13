/* Multiplexed 7-segment digit bank: a 74HC573 segment-data latch plus a
 * 74HC138 digit-select decoder, composed into one queryable N-digit display.
 * Copyright 2025 Thomas Reidemeister, MIT License (see hc573.h for full text)
 *
 * digit_display.h
 *
 * This is the common topology this project's board (and many similar
 * 8051 teaching boards) uses for an 8-digit 7-segment bank: one latch
 * captures whichever digit's segment pattern is currently on the data
 * bus, one decoder says which of the 8 physical digit positions that
 * pattern should be shown on, and the firmware cycles through all 8
 * quickly enough that persistence of vision makes it look like a steady
 * multi-digit display. hc573.h/hc138.c remain independently usable for
 * boards that don't happen to pair them this way.
 */
#ifndef SIM_DEVICES_DIGIT_DISPLAY_H
#define SIM_DEVICES_DIGIT_DISPLAY_H

#include "../bus.h"
#include "hc573.h"
#include "hc138.h"

#define DIGIT_DISPLAY_MAX_DIGITS 16

typedef struct
{
    hc573_pins_t latch;
    hc138_pins_t select;
    int digit_count; // <= DIGIT_DISPLAY_MAX_DIGITS
} digit_display_pins_t;

typedef struct digit_display digit_display_t;

digit_display_t *digit_display_create(sim_bus_t *aBus, struct em8051 *aCPU, digit_display_pins_t aPins);
void digit_display_destroy(digit_display_t *aDev);

// Last-seen raw segment byte (dp-g-f-e-d-c-b-a, matching this project's
// segment_map/ledteble tables) latched for physical position aDigit
// (0 .. digit_count-1). Never reset between reads -- this is a "last
// known" snapshot, not a per-frame event.
uint8_t digit_display_get_segments(const digit_display_t *aDev, int aDigit);

// Best-effort decode of a common-cathode 7-segment byte (bit0=a..bit6=g,
// bit7=dp, 0 for "segment off") back to '0'-'9'/'A'-'F', or '?' if the
// pattern doesn't match one of those 16. Matches the segment tables used
// throughout stc89c52-staging/stc89c52-demos.
char digit_display_decode(uint8_t aSegments);

#endif // SIM_DEVICES_DIGIT_DISPLAY_H
