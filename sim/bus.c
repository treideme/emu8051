/* bus.c - see bus.h
 * Copyright 2025 Thomas Reidemeister, MIT License (see bus.h)
 */
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "bus.h"

#define SIM_MAX_SUBS_PER_REG 16
#define SIM_NUM_REGS 128

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
// (cpu, register): the vendored core's callback signature has no room
// for a "this" pointer. It grows as buses are created, so the number of
// simulations open at once is not limited; the last bus found is
// checked first, since one simulation usually runs for a while.
static sim_bus_t **g_registry;
static int g_registry_cap;
static sim_bus_t *g_last;

static sim_bus_t *bus_for_cpu(struct em8051 *aCPU)
{
    int i;
    if (g_last && g_last->cpu == aCPU)
        return g_last;
    for (i = 0; i < g_registry_cap; i++)
    {
        if (g_registry[i] && g_registry[i]->cpu == aCPU)
        {
            g_last = g_registry[i];
            return g_last;
        }
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
    if (!bus)
        return NULL;
    bus->cpu = aCPU;

    for (i = 0; i < g_registry_cap; i++)
    {
        if (!g_registry[i])
        {
            g_registry[i] = bus;
            return bus;
        }
    }
    {
        int cap = g_registry_cap ? g_registry_cap * 2 : 8;
        sim_bus_t **grown = realloc(g_registry, (size_t)cap * sizeof(*grown));
        if (!grown)
        {
            free(bus);
            return NULL;
        }
        for (i = g_registry_cap; i < cap; i++)
            grown[i] = NULL;
        grown[g_registry_cap] = bus;
        g_registry = grown;
        g_registry_cap = cap;
    }
    return bus;
}

void bus_destroy(sim_bus_t *aBus)
{
    int i, live = 0;
    if (!aBus)
        return;
    if (g_last == aBus)
        g_last = NULL;
    for (i = 0; i < g_registry_cap; i++)
    {
        if (g_registry[i] == aBus)
            g_registry[i] = NULL;
        live += g_registry[i] != NULL;
    }
    if (!live)
    {
        // the last one gone: give the registry back
        free(g_registry);
        g_registry = NULL;
        g_registry_cap = 0;
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
