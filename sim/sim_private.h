/* sim_private.h - the simulation object behind capi.h's handle
 * Copyright 2025 Thomas Reidemeister, MIT License (see devices/hc573.h)
 *
 * Shared by capi.c (the flat API) and plugin.c (the plugin host, see
 * plugin.h). The boards' devices are not here: a board is a plugin
 * (sim/boards/hc6800_es_plugin.c). Not installed, not part of any ABI: a
 * plugin sees emu8051_host_t, never this struct.
 */
#ifndef SIM_PRIVATE_H
#define SIM_PRIVATE_H

#include "capi.h"
#include "bus.h"
#include "plugin.h"

#define MAX_EXCEPTIONS 64
#define MAX_STIM 64
#define MAX_PLUGINS 8
#define MAX_PLUGIN_HOOKS 16
#define MAX_XDATA_HOOKS 16
#define MAX_PIN_WATCH 64
#define PLUGIN_ERROR_LEN 256

typedef struct
{
    uint8_t reg; // port register index (address - 80h)
    uint8_t bit;
    int active;
    int value;
} stim_entry_t;

typedef struct
{
    void *dl; // the shared object (dlopen / LoadLibrary handle); NULL when linked in
    emu8051_plugin_t p;
    char *path; // NULL when linked in (sim_add_plugin)
    char name[48]; // p.name, made unique on this simulation; "" = inert
} sim_plugin_t;

typedef struct
{
    emu8051_step_hooks_t h;
    void *user;
} step_hook_t;

typedef struct
{
    emu8051_irq_fn fn;
    void *user;
} irq_hook_t;

typedef struct
{
    emu8051_exception_fn fn;
    void *user;
} exc_hook_t;

typedef struct
{
    uint16_t lo, hi;
    emu8051_xread_fn rd;
    emu8051_xwrite_fn wr;
    void *user;
} xdata_hook_t;

typedef struct
{
    emu8051_op_fn fn;
    void *user;
} op_hook_t;

typedef struct
{
    uint8_t reg, bit;
    int level;
    emu8051_pin_fn fn;
    void *user;
} pin_watch_t;

typedef struct
{
    emu8051_tick_fn fn;
    void *user;
} tick_hook_t;

struct sim
{
    struct em8051 cpu; // MUST stay the first member: on_exception() casts
                        // struct em8051* straight back to struct sim*
                        // instead of needing a second cpu->sim registry.
    sim_bus_t *bus;

    unsigned long clock_hz;

    int exceptions[MAX_EXCEPTIONS];
    int exception_count;

    int total_tx_count; // see sim_uart_tx_count -- cpu.serial_out_idx alone
                         // wraps at 18 and can't tell "0 sent" from "18 sent"
    int last_tx_idx;
    int tx_origin; // serial_out[] slot of TX byte 0 since the last reset

    long total_instructions;
    unsigned long total_ticks;
    uint64_t total_clocks;
    uint32_t device_clocks; // instruction mode: clocks not yet handed to sim_post_tick()

    stim_entry_t stim[MAX_STIM];
    int stim_count;
    uint8_t stim_reg_registered[128];

    unsigned long xram_size; // 0 = unchecked; see sim_set_xram_size

    // ---- plugins (plugin.c)
    emu8051_host_t host;
    sim_plugin_t plugins[MAX_PLUGINS];
    int plugin_count;
    char plugin_error[PLUGIN_ERROR_LEN];

    int instruction_mode; // see plugin.h, "The two execution modes"
    int have_cycles;
    uint8_t cycles[256];
    uint8_t cycles_taken[256];
    op_hook_t ops[256];
    int have_ops;
    step_hook_t step_hooks[MAX_PLUGIN_HOOKS];
    int step_hook_count;
    irq_hook_t irq_hooks[MAX_PLUGIN_HOOKS];
    int irq_hook_count;
    exc_hook_t exc_hooks[MAX_PLUGIN_HOOKS];
    int exc_hook_count;
    xdata_hook_t xdata_hooks[MAX_XDATA_HOOKS];
    int xdata_hook_count;
    pin_watch_t pin_watch[MAX_PIN_WATCH];
    int pin_watch_count;
    uint8_t pin_reg_watched[128];
    tick_hook_t tick_hooks[MAX_PLUGIN_HOOKS];
    int tick_hook_count;
};

// capi.c
void sim_record_exception(struct sim *aSim, int aCode);
void sim_post_tick(struct sim *aSim);
void sim_cpu_reset(struct sim *aSim, int aWipe); // reset() plus the plugins' opcodes
int sim_pin_level(struct sim *aSim, uint8_t aReg, uint8_t aBit);
void sim_drive_pin(struct sim *aSim, uint8_t aReg, uint8_t aBit, int aLevel);

// plugin.c
void plugin_host_init(struct sim *aSim);
void plugin_destroy_all(struct sim *aSim);
void plugin_reset_all(struct sim *aSim);
uint32_t plugin_step(struct sim *aSim);
void plugin_apply_ops(struct sim *aSim);
void plugin_pins_changed(struct sim *aSim, uint8_t aReg);
int plugin_filter_exception(struct sim *aSim, int aCode);
int plugin_xdata_read(struct sim *aSim, uint16_t aAddr, uint8_t *aValue);
int plugin_xdata_write(struct sim *aSim, uint16_t aAddr, uint8_t aValue);

#endif // SIM_PRIVATE_H
