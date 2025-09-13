/* hd44780.c - see hd44780.h
 * Copyright 2025 Thomas Reidemeister, MIT License (see hd44780.h)
 * State machine logic adapted from logicboard.c (Copyright 2006 Jari Komppa)
 */
#include <stdlib.h>
#include <string.h>
#include "hd44780.h"

struct hd44780
{
    hd44780_pins_t pins;
    unsigned long clock_hz;

    uint8_t ddram[0x80];
    uint8_t cgram[0x40];

    int cursor_pos;     // DDRAM/CGRAM address counter
    int shift_ofs;      // display shift offset (entry-mode "S" / cursor-shift cmds)
    int dir;            // +1 or -1, address counter direction
    int shift_on_write; // entry-mode "S" bit
    int chargen_mode;   // 0 = address counter indexes DDRAM, 1 = CGRAM
    int mode_4bit;      // 0 = 8-bit interface, 1 = 4-bit interface
    int nibble_tick;    // which nibble (4-bit mode) is next
    long busy_ticks;    // counts down to 0 in hd44780_step()

    uint8_t latched_byte;      // last byte fetched from DDRAM/CGRAM or built from bus
    uint8_t pending_read_value; // what the data pins should present on next CPU read

    int last_e;
};

static uint8_t read_data_bus(const hd44780_t *aDev, const struct em8051 *aCPU)
{
    uint8_t v = 0;
    int i;
    for (i = 0; i < 8; i++)
        if (pin_is_connected(aDev->pins.data[i]))
            v = (uint8_t)(v | (pin_get(aCPU, aDev->pins.data[i]) << i));
    return v;
}

static void on_data_read(struct em8051 *aCPU, uint8_t aReg, void *aUserData, uint8_t *aOutMask, uint8_t *aOutValue)
{
    hd44780_t *dev = (hd44780_t *)aUserData;
    int i;
    (void)aCPU;
    (void)aReg;
    for (i = 0; i < 8; i++)
    {
        if (!pin_is_connected(dev->pins.data[i]))
            continue;
        *aOutMask = (uint8_t)(*aOutMask | (1u << i));
        if (dev->pending_read_value & (1u << i))
            *aOutValue = (uint8_t)(*aOutValue | (1u << i));
    }
}

// One command/data byte has just been fully assembled (either instantly in
// 8-bit mode, or after both nibbles arrived in 4-bit mode) and is in
// dev->latched_byte. aRs selects instruction (0) vs data/memory (1) mode,
// matching the pin level R/S had while this transfer happened.
static void commit_write(hd44780_t *dev, int aRs)
{
    uint8_t d = dev->latched_byte;

    if (aRs)
    {
        if (dev->busy_ticks)
            return; // ignore writes while busy, same as the real chip
        if (!dev->chargen_mode)
        {
            dev->ddram[dev->cursor_pos & 0x7f] = d;
            dev->cursor_pos += dev->dir;
            if (dev->shift_on_write)
                dev->shift_ofs += dev->dir;
        }
        else
        {
            dev->cgram[dev->cursor_pos & 0x3f] = d;
            dev->cursor_pos++;
        }
        dev->busy_ticks = (long)(250L * dev->clock_hz / 12000000L);
        return;
    }

    // Instruction mode -- decode the command byte.
    if (dev->busy_ticks)
        return; // only busy-flag reads are meaningful while busy

    if (d == 1)
    {
        // Clear display
        memset(dev->ddram, 0x20, sizeof(dev->ddram));
        dev->cursor_pos = 0;
        dev->shift_ofs = 0;
        dev->dir = 1; // HD44780U data sheet: clear also resets I/D to increment
        dev->busy_ticks = (long)(2L * dev->clock_hz / 12000L); // 2ms
    }
    else if ((d & ~1) == 2)
    {
        // Return home
        dev->cursor_pos = 0;
        dev->shift_ofs = 0;
        dev->busy_ticks = (long)(200L * dev->clock_hz / 12000000L);
    }
    else if ((d & ~3) == 4)
    {
        // Entry mode set
        dev->shift_on_write = (d & 1) != 0;
        dev->dir = (d & 2) ? 1 : -1;
        dev->busy_ticks = (long)(200L * dev->clock_hz / 12000000L);
    }
    else if ((d & ~7) == 8)
    {
        // Display on/off control (on/cursor/blink bits) -- not modeled
        // beyond accepting the command; nothing in this project's demos
        // reads these back, and DDRAM content is unaffected either way.
        dev->busy_ticks = (long)(200L * dev->clock_hz / 12000000L);
    }
    else if ((d & ~0xf) == 0x10)
    {
        // Cursor or display shift
        if (d & 8)
            dev->cursor_pos += (d & 4) ? 1 : -1;
        else
            dev->shift_ofs += (d & 4) ? 1 : -1;
        dev->busy_ticks = (long)(200L * dev->clock_hz / 12000000L);
    }
    else if ((d & ~0x1f) == 0x20)
    {
        // Function set (interface width / font size -- only width matters here)
        dev->mode_4bit = (d & 16) == 0;
        dev->nibble_tick = 0;
        dev->busy_ticks = (long)(200L * dev->clock_hz / 12000000L);
    }
    else if ((d & ~0x3f) == 0x40)
    {
        // CGRAM address set
        dev->chargen_mode = 1;
        dev->cursor_pos = d & 0x3f;
        dev->busy_ticks = (long)(200L * dev->clock_hz / 12000000L);
    }
    else if ((d & ~0x7f) == 0x80)
    {
        // DDRAM address set
        dev->chargen_mode = 0;
        dev->cursor_pos = d & 0x7f;
        dev->busy_ticks = (long)(200L * dev->clock_hz / 12000000L);
    }
}

static void handle_write_edge(hd44780_t *dev, struct em8051 *cpu, int rs)
{
    uint8_t bus = read_data_bus(dev, cpu);

    if (!dev->mode_4bit)
    {
        dev->latched_byte = bus;
    }
    else
    {
        if (!dev->nibble_tick)
            dev->latched_byte = (uint8_t)((dev->latched_byte & 0x0f) | (bus & 0xf0));
        else
            dev->latched_byte = (uint8_t)((dev->latched_byte & 0xf0) | ((bus & 0xf0) >> 4));
        dev->nibble_tick = !dev->nibble_tick;
    }

    if (!dev->mode_4bit || !dev->nibble_tick)
        commit_write(dev, rs);
}

static void handle_read_edge(hd44780_t *dev, int rs)
{
    if (rs)
    {
        if (!dev->busy_ticks)
        {
            if (!dev->chargen_mode)
                dev->latched_byte = dev->ddram[dev->cursor_pos & 0x7f];
            else
                dev->latched_byte = dev->cgram[dev->cursor_pos & 0x3f];

            if (!dev->mode_4bit || dev->nibble_tick)
            {
                if (!dev->chargen_mode)
                {
                    dev->cursor_pos += dev->dir;
                    if (dev->shift_on_write)
                        dev->shift_ofs += dev->dir;
                }
                else
                {
                    dev->cursor_pos++;
                }
                dev->busy_ticks = (long)(250L * dev->clock_hz / 12000000L);
            }
        }
        // else: leave latched_byte as whatever it was (matches the busy
        // behavior of the reference implementation)
    }
    else
    {
        dev->latched_byte = (uint8_t)(dev->cursor_pos & 0x7f);
        if (dev->busy_ticks)
            dev->latched_byte |= 0x80;
    }

    if (!dev->mode_4bit)
    {
        dev->pending_read_value = dev->latched_byte;
    }
    else
    {
        if (dev->nibble_tick)
            dev->pending_read_value = (uint8_t)((dev->latched_byte << 4) & 0xf0);
        else
            dev->pending_read_value = (uint8_t)(dev->latched_byte & 0xf0);
        dev->nibble_tick = !dev->nibble_tick;
    }
}

static void on_e_write(struct em8051 *aCPU, uint8_t aReg, uint8_t aValue, void *aUserData)
{
    hd44780_t *dev = (hd44780_t *)aUserData;
    int e = pin_get(aCPU, dev->pins.e);
    int rs = pin_get(aCPU, dev->pins.rs);
    int rw = pin_is_connected(dev->pins.rw) ? pin_get(aCPU, dev->pins.rw) : 0;
    (void)aReg;
    (void)aValue;

    if (e && !dev->last_e && rw)
        handle_read_edge(dev, rs);

    if (!e && dev->last_e && !rw)
        handle_write_edge(dev, aCPU, rs);

    dev->last_e = e;
}

hd44780_t *hd44780_create(sim_bus_t *aBus, struct em8051 *aCPU, hd44780_pins_t aPins, unsigned long aClockHz)
{
    hd44780_t *dev = (hd44780_t *)calloc(1, sizeof(hd44780_t));
    dev->pins = aPins;
    dev->clock_hz = aClockHz;
    dev->dir = 1;
    memset(dev->ddram, 0x20, sizeof(dev->ddram)); // power-on DDRAM reads as spaces

    bus_on_write(aBus, dev->pins.e.reg, on_e_write, dev);

    {
        int i;
        int reg_done[128] = {0};
        for (i = 0; i < 8; i++)
        {
            pin_t p = dev->pins.data[i];
            if (pin_is_connected(p) && !reg_done[p.reg])
            {
                bus_on_read(aBus, p.reg, on_data_read, dev);
                reg_done[p.reg] = 1;
            }
        }
    }

    (void)aCPU;
    return dev;
}

void hd44780_destroy(hd44780_t *aDev)
{
    free(aDev);
}

void hd44780_step(hd44780_t *aDev)
{
    if (aDev->busy_ticks > 0)
        aDev->busy_ticks--;
}

uint8_t hd44780_get_ddram(const hd44780_t *aDev, uint8_t aAddress)
{
    return aDev->ddram[aAddress & 0x7f];
}

uint8_t hd44780_get_cgram(const hd44780_t *aDev, uint8_t aAddress)
{
    return aDev->cgram[aAddress & 0x3f];
}

char *hd44780_get_line(const hd44780_t *aDev, int aLine, uint8_t aWidth, char *aBuf, size_t aBufSize)
{
    static const uint8_t line_base[4] = {0x00, 0x40, 0x00, 0x40};
    uint8_t start;
    size_t i;
    size_t n = aWidth;

    if (aBufSize < 1)
        return aBuf;
    if (n > aBufSize - 1)
        n = aBufSize - 1;

    start = (uint8_t)(line_base[aLine & 3] + ((aLine >= 2) ? aWidth : 0));
    for (i = 0; i < n; i++)
    {
        uint8_t c = hd44780_get_ddram((hd44780_t *)aDev, (uint8_t)(start + i));
        aBuf[i] = (c >= 0x20 && c < 0x7f) ? (char)c : ' ';
    }
    aBuf[n] = '\0';
    return aBuf;
}
