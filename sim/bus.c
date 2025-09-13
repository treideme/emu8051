/* bus.c - see bus.h
 * Copyright 2025 Thomas Reidemeister, MIT License (see bus.h)
 */
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "bus.h"

#define SIM_MAX_SUBS_PER_REG 4
#define SIM_NUM_REGS 128
#define SIM_MAX_BUSES 8

typedef struct
{
    bus_write_fn fn;
    void *userdata;
} write_sub_t;

typedef struct
{
    bus_read_fn fn;
    void *userdata;
} read_sub_t;

struct sim_bus
{
    struct em8051 *cpu;
    write_sub_t writes[SIM_NUM_REGS][SIM_MAX_SUBS_PER_REG];
    uint8_t write_count[SIM_NUM_REGS];
    read_sub_t reads[SIM_NUM_REGS][SIM_MAX_SUBS_PER_REG];
    uint8_t read_count[SIM_NUM_REGS];
};

// Registry mapping a live struct em8051* back to the sim_bus_t that owns
// it. Needed because cpu->sfrwrite/sfrread callbacks only receive
// (cpu, register) -- no way to smuggle a "this" pointer through the
// vendored core's callback signature otherwise.
static sim_bus_t *g_registry[SIM_MAX_BUSES];

static sim_bus_t *bus_for_cpu(struct em8051 *aCPU)
{
    int i;
    for (i = 0; i < SIM_MAX_BUSES; i++)
    {
        if (g_registry[i] && g_registry[i]->cpu == aCPU)
            return g_registry[i];
    }
    return NULL;
}

static void trampoline_write(struct em8051 *aCPU, uint8_t aAddress)
{
    sim_bus_t *bus = bus_for_cpu(aCPU);
    uint8_t reg = aAddress - 0x80;
    uint8_t value;
    int i;

    if (!bus)
        return;

    value = aCPU->mSFR[reg];
    for (i = 0; i < bus->write_count[reg]; i++)
        bus->writes[reg][i].fn(aCPU, reg, value, bus->writes[reg][i].userdata);
}

static uint8_t trampoline_read(struct em8051 *aCPU, uint8_t aAddress)
{
    sim_bus_t *bus = bus_for_cpu(aCPU);
    uint8_t reg = aAddress - 0x80;
    uint8_t result;
    int i;

    if (!bus)
        return aCPU->mSFR[reg];

    result = aCPU->mSFR[reg];
    for (i = 0; i < bus->read_count[reg]; i++)
    {
        uint8_t mask = 0, value = 0;
        bus->reads[reg][i].fn(aCPU, reg, bus->reads[reg][i].userdata, &mask, &value);
        result = (uint8_t)((result & ~mask) | (value & mask));
    }
    return result;
}

sim_bus_t *bus_create(struct em8051 *aCPU)
{
    int i;
    sim_bus_t *bus = calloc(1, sizeof(sim_bus_t));
    bus->cpu = aCPU;

    for (i = 0; i < SIM_MAX_BUSES; i++)
    {
        if (!g_registry[i])
        {
            g_registry[i] = bus;
            return bus;
        }
    }
    // Out of registry slots; still usable stand-alone (subscriptions will
    // simply never fire, since the trampolines can't find it), but this
    // indicates SIM_MAX_BUSES needs raising for whatever is calling this.
    assert(0 && "bus_create: registry full, raise SIM_MAX_BUSES");
    return bus;
}

void bus_destroy(sim_bus_t *aBus)
{
    int i;
    if (!aBus)
        return;
    for (i = 0; i < SIM_MAX_BUSES; i++)
    {
        if (g_registry[i] == aBus)
            g_registry[i] = NULL;
    }
    free(aBus);
}

void bus_on_write(sim_bus_t *aBus, uint8_t aReg, bus_write_fn aFn, void *aUserData)
{
    uint8_t n = aBus->write_count[aReg];
    assert(n < SIM_MAX_SUBS_PER_REG && "bus_on_write: raise SIM_MAX_SUBS_PER_REG");
    aBus->writes[aReg][n].fn = aFn;
    aBus->writes[aReg][n].userdata = aUserData;
    aBus->write_count[aReg] = (uint8_t)(n + 1);
    aBus->cpu->sfrwrite[aReg] = trampoline_write;
}

void bus_on_read(sim_bus_t *aBus, uint8_t aReg, bus_read_fn aFn, void *aUserData)
{
    uint8_t n = aBus->read_count[aReg];
    assert(n < SIM_MAX_SUBS_PER_REG && "bus_on_read: raise SIM_MAX_SUBS_PER_REG");
    aBus->reads[aReg][n].fn = aFn;
    aBus->reads[aReg][n].userdata = aUserData;
    aBus->read_count[aReg] = (uint8_t)(n + 1);
    aBus->cpu->sfrread[aReg] = trampoline_read;
}
