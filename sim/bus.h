/* 8051 emulator peripheral simulation - SFR dispatcher ("connector")
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
 * bus.h
 *
 * struct em8051 has exactly one write callback slot and one read callback
 * slot per SFR register (emu8051.h's sfrwrite[128]/sfrread[128]). Real
 * boards routinely put more than one logical peripheral on the same port
 * (this project's own HC6800-ES wires DS1302 and a 74HC595 LED bank to
 * overlapping bits of P3, in different demos), so a single device model
 * cannot just claim cpu->sfrwrite[REG_P3] for itself -- the next device
 * that needs P3 would silently clobber it.
 *
 * The bus is the connector: device models never touch cpu->sfrwrite/
 * sfrread directly. They subscribe to the bus for the specific registers
 * their pins live in, and the bus is the one thing that actually occupies
 * the CPU's single callback slot per register, fanning each event out to
 * every subscriber.
 *
 * Write subscribers are pure notifications (the byte is already stored in
 * mSFR[] by the time they run -- see opcodes.c's write_mem -- so multiple
 * subscribers observing the same write is never a conflict). Read
 * subscribers instead each contribute a (mask, value) pair: "for these
 * bits, force this value; leave every other bit as whatever's already in
 * mSFR[]". The bus composes all contributions with the stored byte as the
 * base, which is how e.g. a DS1302 model can own just its one I/O bit of
 * a port without knowing or caring what else lives on that byte.
 */
#ifndef SIM_BUS_H
#define SIM_BUS_H

#include "emu8051.h"

typedef struct sim_bus sim_bus_t;

typedef void (*bus_write_fn)(struct em8051 *aCPU, uint8_t aReg, uint8_t aValue, void *aUserData);

// aOutMask/aOutValue are pre-zeroed by the bus before the first subscriber
// runs. Set the bits you own in aOutMask, and their levels in aOutValue.
typedef void (*bus_read_fn)(struct em8051 *aCPU, uint8_t aReg, void *aUserData,
                             uint8_t *aOutMask, uint8_t *aOutValue);

// Create a bus bound to aCPU. Safe to call any time before tick() starts
// running (does not depend on reset() having run). aCPU must outlive the
// bus.
sim_bus_t *bus_create(struct em8051 *aCPU);

// Releases the bus's subscriber lists and un-registers its cpu<->bus
// association. Does not touch aCPU's memory. cpu->sfrwrite/sfrread slots
// the bus had claimed are left pointing at the bus's trampoline (harmless,
// since the bus is gone and the registry lookup will just fail closed --
// but destroy the em8051 alongside the bus in practice).
void bus_destroy(sim_bus_t *aBus);

// Subscribe to every write to SFR register aReg (a REG_* value from
// emu8051.h, e.g. REG_P2). Multiple subscribers on the same register all
// fire, in subscription order.
void bus_on_write(sim_bus_t *aBus, uint8_t aReg, bus_write_fn aFn, void *aUserData);

// Subscribe to reads of SFR register aReg. See the mask/value composition
// note above.
void bus_on_read(sim_bus_t *aBus, uint8_t aReg, bus_read_fn aFn, void *aUserData);

#endif // SIM_BUS_H
