/* plugin.h - loadable plugins for the sim layer
 * Copyright 2026 Thomas Reidemeister, MIT License (see devices/hc573.h)
 *
 * A plugin is a shared object that sim_load_plugin() (capi.h) opens by
 * path. It exports one function, named by EMU8051_PLUGIN_ENTRY:
 *
 *     int emu8051_plugin_init(const emu8051_host_t *aHost,
 *                             emu8051_plugin_t *aPlugin, const char *aArgs);
 *
 * The host fills aPlugin->abi and aPlugin->size, the plugin fills in the
 * rest (at least name), keeps aHost (it stays valid until the plugin's
 * destroy has returned) and returns 0. Anything else refuses the load;
 * aHost->error() can say why before it returns. aArgs is the string the
 * caller passed to sim_load_plugin(), never NULL; its meaning is the
 * plugin's.
 *
 * The same plugin can also be linked into a program, with no shared
 * object and no dynamic loader: sim_add_plugin() (capi.h) takes the init
 * function itself, and everything after that is the same. A plugin meant
 * for both gives its init function a name of its own and keeps the
 * exported emu8051_plugin_init, which only calls it, in a file of its own
 * that only the shared object is built from (two plugins linked into one
 * program cannot both define emu8051_plugin_init). sim/boards/
 * hc6800_es_plugin.c and hc6800_es_entry.c are laid out that way; see
 * sim/PLUGIN-TUTORIAL.md.
 *
 * Everything the plugin does to the simulation goes through the function
 * table in emu8051_host_t, each function taking aHost->sim first. The
 * table is the whole ABI, together with struct em8051 from emu8051.h,
 * which aHost->cpu() hands out and which a plugin may read and write
 * directly (registers, memory, the PC).
 *
 * Versioning: EMU8051_PLUGIN_ABI changes whenever an existing member of
 * either struct, or the layout of struct em8051, changes meaning or
 * position. A plugin must refuse a host whose abi differs from the one it
 * was built against. Members are only ever added at the end of
 * emu8051_host_t, so a plugin built against a newer header can check with
 * EMU8051_HOST_HAS() that an older host has what it needs; the host checks
 * aPlugin->abi after init the same way.
 *
 * The two execution modes. A host runs a classic 8051 with tick(): 12
 * clocks per machine cycle, and the core's own timers, UART and two-level
 * interrupt controller. A plugin that sets a cycle table (set_cycles),
 * step hooks with begin/before/after (on_step) or an interrupt hook
 * (on_interrupt) switches the host to instruction mode, in which one step
 * is one of: a wait (a step hook's begin consumed it), an interrupt entry,
 * or one instruction. In instruction mode the core's timers, UART and
 * interrupt controller do not run at all (they are tick()'s); the plugin
 * models whatever the part has instead. That is how a plugin describes a
 * different core, a 1T one for instance: the cycle table gives each
 * opcode's clocks, the interrupt hook its own vectors and priorities.
 *
 * Tick hooks (on_tick) count in ticks of 12 clocks in both modes: after
 * every tick() in tick mode, and once per 12 clocks executed in
 * instruction mode, however the instructions are timed. A device model
 * that counts time in machine cycles (an LCD's busy flag, an RTC) steps
 * from one and keeps its timing on any core. Unlike an end hook, a tick
 * hook keeps the host in its plain tick() loop.
 *
 * Everything is single-threaded and synchronous, as in capi.h: callbacks
 * run inside sim_step() and the other capi calls, never on their own.
 */
#ifndef SIM_PLUGIN_H
#define SIM_PLUGIN_H

#include <stddef.h>
#include <stdint.h>
#include "emu8051.h"
#include "bus.h"

#define EMU8051_PLUGIN_ABI 1
#define EMU8051_PLUGIN_ENTRY "emu8051_plugin_init"

#if defined(_WIN32)
#define EMU8051_PLUGIN_EXPORT __declspec(dllexport)
#else
#define EMU8051_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct emu8051_host emu8051_host_t;
    typedef struct emu8051_plugin emu8051_plugin_t;

    // xdata hooks: aAddr is the MOVX address.
    typedef uint8_t (*emu8051_xread_fn)(void *aUser, uint16_t aAddr);
    typedef void (*emu8051_xwrite_fn)(void *aUser, uint16_t aAddr, uint8_t aValue);

    // An opcode handler. Returns what the core's own handlers return: the
    // machine cycles beyond the first (used only when there is no cycle
    // table). It must advance the PC itself.
    typedef uint8_t (*emu8051_op_fn)(void *aUser, struct em8051 *aCPU);

    // Interrupt hook, instruction mode: called before each instruction.
    // Return 1 to enter an interrupt: the host pushes the PC (low byte
    // first), jumps to *aVector and charges *aClocks (preset to 24, two
    // classic machine cycles) for the step. Return 0 for none. The plugin
    // does its own bookkeeping (priorities in service, flags to clear)
    // before it returns; RETI is the core's and only pops the PC.
    typedef int (*emu8051_irq_fn)(void *aUser, uint16_t *aVector, uint32_t *aClocks);

    // A watched pin's level changed (see on_pin).
    typedef void (*emu8051_pin_fn)(void *aUser, uint8_t aPort, uint8_t aBit, int aLevel);

    // One tick of 12 clocks has passed (see on_tick).
    typedef void (*emu8051_tick_fn)(void *aUser);

    // Exception filter: return the code to record (sim_get_exceptions), a
    // different one to translate it, or -1 to drop it.
    typedef int (*emu8051_exception_fn)(void *aUser, int aCode);

    // A plugin method (see emu8051_plugin_t.call and sim_plugin_call).
    typedef int (*emu8051_call_fn)(void *aCtx, const char *aMethod, const uint8_t *aIn, int aInLen,
                                   uint8_t *aOut, int aOutCap, int *aOutLen);

    // Return codes of a call. A plugin returns EMU8051_CALL_UNKNOWN for a
    // method it does not have; other negative values are its own errors.
    enum
    {
        EMU8051_CALL_OK = 0,
        EMU8051_CALL_UNKNOWN = -1,   // no such method
        EMU8051_CALL_NO_PLUGIN = -2, // no plugin of that name (from the host)
        EMU8051_CALL_BAD_ARGS = -3,  // the input does not fit the method
        EMU8051_CALL_TOO_SMALL = -4, // aOut is too small; *aOutLen says how much is needed
    };

    // begin's return value to go on with the step.
#define EMU8051_STEP_RUN (-1)

    // What a step was (the step hooks' end).
    enum
    {
        EMU8051_STEP_WAIT = 0,        // begin consumed it: no instruction
        EMU8051_STEP_INTERRUPT = 1,   // an interrupt hook entered a vector
        EMU8051_STEP_INSTRUCTION = 2, // one instruction ran
        EMU8051_STEP_TICK = 3,        // tick mode: one tick() of 12 clocks
    };

    // Step hooks. Any member may be NULL. Hooks of several plugins run in
    // the order they were installed.
    typedef struct emu8051_step_hooks
    {
        // Instruction mode: first thing in every step. Return the clocks
        // the step took to end it there (a wait state, an idle or stalled
        // CPU, a reset; 0 is allowed) or EMU8051_STEP_RUN to go on.
        int32_t (*begin)(void *aUser);
        // Instruction mode: before the instruction at the PC; aOp is its
        // opcode.
        void (*before)(void *aUser, uint8_t aOp);
        // Instruction mode: after it. aClocks is what the cycle table
        // charges (the taken column for a conditional jump that jumped),
        // aTaken whether the PC went anywhere but the next instruction.
        // Returns the clocks to charge.
        uint32_t (*after)(void *aUser, uint8_t aOp, uint32_t aClocks, int aTaken);
        // Both modes: at the end of every step, with what it took and what
        // it was (EMU8051_STEP_*).
        void (*end)(void *aUser, uint32_t aClocks, int aKind);
    } emu8051_step_hooks_t;

    struct emu8051_host
    {
        uint32_t abi;  // EMU8051_PLUGIN_ABI
        uint32_t size; // sizeof(emu8051_host_t) as the host has it
        void *sim;     // the first argument of every function below

        // ---- CPU and memory
        struct em8051 *(*cpu)(void *aSim);
        // Code and xdata sizes, powers of two from 1 to 65536 bytes, and
        // whether there are the upper 128 bytes of idata. Contents are
        // kept as far as they fit. 0 on success, -1 for a bad size.
        int (*set_memory)(void *aSim, uint32_t aCodeBytes, uint32_t aXdataBytes, int aUpperIdata);
        // Intel HEX into code memory, as sim_load_hex does: 0 or negative.
        int (*load_hex)(void *aSim, const char *aPath);
        // The core's reset(): registers, PC and SP to the 8051's values,
        // and with aWipe all memory to zero. Only the CPU; sim_reset() is
        // the whole simulation and calls the plugins' reset.
        void (*reset_cpu)(void *aSim, int aWipe);
        // The oscillator in Hz (sim_get_clock_hz).
        unsigned long (*clock_hz)(void *aSim);

        // ---- exceptions
        // Record an exception, as the core's own are (filters apply).
        void (*raise)(void *aSim, int aCode);
        void (*on_exception)(void *aSim, emu8051_exception_fn aFn, void *aUser);

        // ---- SFRs, through the bus (bus.h). aReg is the register index
        // as bus.h has it: the SFR address minus 80h.
        void (*on_sfr_write)(void *aSim, uint8_t aReg, bus_write_fn aFn, void *aUser);
        void (*on_sfr_read)(void *aSim, uint8_t aReg, bus_read_fn aFn, void *aUser);

        // ---- xdata. MOVX to aLo..aHi (inclusive) goes to the hooks
        // instead of the xdata array; a later range wins where ranges
        // overlap. Either function may be NULL (that direction then goes to
        // the array). 0, or -1 when the table is full.
        int (*on_xdata)(void *aSim, uint16_t aLo, uint16_t aHi, emu8051_xread_fn aRead,
                        emu8051_xwrite_fn aWrite, void *aUser);

        // ---- execution
        // Clocks per opcode for instruction mode, and for conditional jumps
        // the clocks when they jump (0 = as aCycles). The host copies both;
        // aTaken may be NULL. Switches to instruction mode.
        void (*set_cycles)(void *aSim, const uint8_t aCycles[256], const uint8_t aTaken[256]);
        // Replace the core's handler for one opcode (both modes; survives
        // resets). NULL restores the core's. 0, or -1.
        int (*set_opcode)(void *aSim, uint8_t aOp, emu8051_op_fn aFn, void *aUser);
        // Install step hooks (the host copies the struct). 0, or -1 when
        // the table is full.
        int (*on_step)(void *aSim, const emu8051_step_hooks_t *aHooks, void *aUser);
        // Install an interrupt hook; several are asked in order, the first
        // to answer 1 wins. Switches to instruction mode. 0, or -1.
        int (*on_interrupt)(void *aSim, emu8051_irq_fn aFn, void *aUser);
        // Push the PC (low byte first) and jump to aVector: an interrupt
        // entry made from a hook (begin, say), outside on_interrupt.
        void (*enter_vector)(void *aSim, uint16_t aVector);
        // One step of the host's loop (a tick in tick mode); the clocks it
        // took. sim_step() is a loop of these.
        uint32_t (*step)(void *aSim);
        // Clocks and instructions since the last sim_reset().
        uint64_t (*clocks)(void *aSim);
        long (*instructions)(void *aSim);

        // ---- pins. aPort is a port's register index as in bus.h (REG_P1
        // for P1); any SFR may be a port.
        // The level the CPU reads: an external drive if there is one, else
        // the latch bit.
        int (*pin_level)(void *aSim, uint8_t aPort, uint8_t aBit);
        // Drive the pin from outside (as sim_set_pin does): 0 or 1, or -1
        // to release it.
        void (*pin_drive)(void *aSim, uint8_t aPort, uint8_t aBit, int aLevel);
        // Be told whenever pin_level() of that pin changes: by a write to
        // the port, a drive or a reset. 0, or -1 when the table is full.
        int (*on_pin)(void *aSim, uint8_t aPort, uint8_t aBit, emu8051_pin_fn aFn, void *aUser);

        // ---- plugins
        // Call a method of a plugin on this simulation (this one included).
        int (*call)(void *aSim, const char *aPlugin, const char *aMethod, const uint8_t *aIn, int aInLen,
                    uint8_t *aOut, int aOutCap, int *aOutLen);
        // Why init failed, for sim_plugin_error(); call before returning.
        void (*error)(void *aSim, const char *aMessage);

        // ---- added later: check for these with EMU8051_HOST_HAS
        // Be called once per tick (see "The two execution modes" above),
        // in the order installed, before the host counts UART output for
        // that tick. Does not switch modes. 0, or -1 when the table is
        // full.
        int (*on_tick)(void *aSim, emu8051_tick_fn aFn, void *aUser);
    };

    // True when the host's table has member aField.
#define EMU8051_HOST_HAS(aHost, aField)                                                                   \
    ((aHost)->size >= offsetof(emu8051_host_t, aField) + sizeof(((emu8051_host_t *)0)->aField) &&     \
     (aHost)->aField != NULL)

    struct emu8051_plugin
    {
        uint32_t abi;     // set by the host before init: its EMU8051_PLUGIN_ABI
        uint32_t size;    // set by the host: sizeof(emu8051_plugin_t) as it has it
        const char *name; // unique on one simulation; the plugin's storage
        void *ctx;        // passed to the three functions below
        // sim_close(): free everything. No host function may be called.
        void (*destroy)(void *aCtx);
        // sim_reset(), after the host has reset the CPU. May be NULL.
        void (*reset)(void *aCtx);
        // The plugin's methods (sim_plugin_call); may be NULL.
        emu8051_call_fn call;
    };

    typedef int (*emu8051_plugin_init_fn)(const emu8051_host_t *aHost, emu8051_plugin_t *aPlugin,
                                          const char *aArgs);

#ifdef __cplusplus
}
#endif

#endif // SIM_PLUGIN_H
