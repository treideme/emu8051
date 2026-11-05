/* plugin.c - the plugin host (see plugin.h)
 * Copyright 2026 Thomas Reidemeister, MIT License (see devices/hc573.h)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sim_private.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

// Instruction lengths (Intel's MCS-51 instruction set table).
static const uint8_t OPLEN[256] = {
    1, 2, 3, 1, 1, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, // 00
    3, 2, 3, 1, 1, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, // 10
    3, 2, 1, 1, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, // 20
    3, 2, 1, 1, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, // 30
    2, 2, 2, 3, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, // 40
    2, 2, 2, 3, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, // 50
    2, 2, 2, 3, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, // 60
    2, 2, 2, 1, 2, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, // 70
    2, 2, 2, 1, 1, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, // 80
    3, 2, 2, 1, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, // 90
    2, 2, 2, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, // A0
    2, 2, 2, 1, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, // B0
    2, 2, 2, 1, 1, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, // C0
    2, 2, 2, 1, 1, 3, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, // D0
    1, 2, 1, 1, 1, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, // E0
    1, 2, 1, 1, 1, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, // F0
};

// Conditional jumps whose last byte is the relative offset: JBC, JB, JNB,
// JC, JNC, JZ, JNZ, CJNE, DJNZ.
static int cond_rel(uint8_t aOp)
{
    if ((aOp & 0x0F) == 0 && aOp >= 0x10 && aOp <= 0x70)
        return 1;
    return (aOp >= 0xB4 && aOp <= 0xBF) || aOp == 0xD5 || (aOp >= 0xD8 && aOp <= 0xDF);
}

#define SIMP(aSim) ((struct sim *)(aSim))

// ------------------------------------------------------------------ host table

static struct em8051 *h_cpu(void *aSim) { return &SIMP(aSim)->cpu; }

static int pow2(uint32_t v)
{
    return v >= 1 && v <= 65536 && (v & (v - 1)) == 0;
}

static int h_set_memory(void *aSim, uint32_t aCode, uint32_t aXdata, int aUpper)
{
    struct em8051 *cpu = &SIMP(aSim)->cpu;
    unsigned char *code, *xdata, *upper = NULL;
    uint32_t oc = (uint32_t)cpu->mCodeMemMaxIdx + 1, ox = (uint32_t)cpu->mExtDataMaxIdx + 1;
    if (!pow2(aCode) || !pow2(aXdata))
        return -1;
    code = (unsigned char *)calloc(aCode, 1);
    xdata = (unsigned char *)calloc(aXdata, 1);
    if (aUpper)
        upper = (unsigned char *)calloc(128, 1);
    memcpy(code, cpu->mCodeMem, oc < aCode ? oc : aCode);
    memcpy(xdata, cpu->mExtData, ox < aXdata ? ox : aXdata);
    if (upper && cpu->mUpperData)
        memcpy(upper, cpu->mUpperData, 128);
    free(cpu->mCodeMem);
    free(cpu->mExtData);
    free(cpu->mUpperData);
    cpu->mCodeMem = code;
    cpu->mCodeMemMaxIdx = (uint16_t)(aCode - 1);
    cpu->mExtData = xdata;
    cpu->mExtDataMaxIdx = (uint16_t)(aXdata - 1);
    cpu->mUpperData = upper;
    return 0;
}

static int h_load_hex(void *aSim, const char *aPath) { return sim_load_hex(aSim, aPath); }

static void h_reset_cpu(void *aSim, int aWipe)
{
    sim_cpu_reset(SIMP(aSim), aWipe);
}

static unsigned long h_clock_hz(void *aSim) { return SIMP(aSim)->clock_hz; }

static void h_raise(void *aSim, int aCode) { sim_record_exception(SIMP(aSim), aCode); }

static void h_on_exception(void *aSim, emu8051_exception_fn aFn, void *aUser)
{
    struct sim *s = SIMP(aSim);
    if (s->exc_hook_count >= MAX_PLUGIN_HOOKS || !aFn)
        return;
    s->exc_hooks[s->exc_hook_count].fn = aFn;
    s->exc_hooks[s->exc_hook_count].user = aUser;
    s->exc_hook_count++;
}

static void h_on_sfr_write(void *aSim, uint8_t aReg, bus_write_fn aFn, void *aUser)
{
    bus_on_write(SIMP(aSim)->bus, (uint8_t)(aReg & 0x7F), aFn, aUser);
}

static void h_on_sfr_read(void *aSim, uint8_t aReg, bus_read_fn aFn, void *aUser)
{
    bus_on_read(SIMP(aSim)->bus, (uint8_t)(aReg & 0x7F), aFn, aUser);
}

static int h_on_xdata(void *aSim, uint16_t aLo, uint16_t aHi, emu8051_xread_fn aRead, emu8051_xwrite_fn aWrite,
                      void *aUser)
{
    struct sim *s = SIMP(aSim);
    xdata_hook_t *x;
    if (s->xdata_hook_count >= MAX_XDATA_HOOKS || aLo > aHi)
        return -1;
    x = &s->xdata_hooks[s->xdata_hook_count++];
    x->lo = aLo;
    x->hi = aHi;
    x->rd = aRead;
    x->wr = aWrite;
    x->user = aUser;
    sim_set_xram_size(aSim, s->xram_size); // installs the dispatcher
    return 0;
}

static void h_set_cycles(void *aSim, const uint8_t aCycles[256], const uint8_t aTaken[256])
{
    struct sim *s = SIMP(aSim);
    memcpy(s->cycles, aCycles, 256);
    if (aTaken)
        memcpy(s->cycles_taken, aTaken, 256);
    else
        memset(s->cycles_taken, 0, 256);
    s->have_cycles = 1;
    s->instruction_mode = 1;
}

static uint8_t op_trampoline(struct em8051 *aCPU)
{
    struct sim *s = (struct sim *)aCPU; // cpu is the first member
    uint8_t op = aCPU->mCodeMem[aCPU->mPC & aCPU->mCodeMemMaxIdx];
    return s->ops[op].fn(s->ops[op].user, aCPU);
}

void plugin_apply_ops(struct sim *aSim)
{
    int i;
    if (!aSim->have_ops)
        return;
    for (i = 0; i < 256; i++)
        if (aSim->ops[i].fn)
            aSim->cpu.op[i] = op_trampoline;
}

static int h_set_opcode(void *aSim, uint8_t aOp, emu8051_op_fn aFn, void *aUser)
{
    struct sim *s = SIMP(aSim);
    s->ops[aOp].fn = aFn;
    s->ops[aOp].user = aUser;
    s->have_ops = 1;
    if (aFn)
        s->cpu.op[aOp] = op_trampoline;
    else
    {
        // the core's handler back: reset() rebuilds the whole table, so
        // take it from a scratch CPU
        struct em8051 tmp;
        unsigned char mem[1];
        memset(&tmp, 0, sizeof(tmp));
        tmp.mCodeMem = mem;
        tmp.mExtData = mem;
        reset(&tmp, 0);
        s->cpu.op[aOp] = tmp.op[aOp];
    }
    return 0;
}

static int h_on_step(void *aSim, const emu8051_step_hooks_t *aHooks, void *aUser)
{
    struct sim *s = SIMP(aSim);
    if (s->step_hook_count >= MAX_PLUGIN_HOOKS || !aHooks)
        return -1;
    s->step_hooks[s->step_hook_count].h = *aHooks;
    s->step_hooks[s->step_hook_count].user = aUser;
    s->step_hook_count++;
    if (aHooks->begin || aHooks->before || aHooks->after)
        s->instruction_mode = 1;
    return 0;
}

static int h_on_interrupt(void *aSim, emu8051_irq_fn aFn, void *aUser)
{
    struct sim *s = SIMP(aSim);
    if (s->irq_hook_count >= MAX_PLUGIN_HOOKS || !aFn)
        return -1;
    s->irq_hooks[s->irq_hook_count].fn = aFn;
    s->irq_hooks[s->irq_hook_count].user = aUser;
    s->irq_hook_count++;
    s->instruction_mode = 1;
    return 0;
}

static void enter_vector(struct sim *s, uint16_t aVector)
{
    struct em8051 *cpu = &s->cpu;
    push_to_stack(cpu, (uint8_t)(cpu->mPC & 0xFF));
    push_to_stack(cpu, (uint8_t)(cpu->mPC >> 8));
    cpu->mPC = aVector;
}

static void h_enter_vector(void *aSim, uint16_t aVector) { enter_vector(SIMP(aSim), aVector); }

static uint32_t h_step(void *aSim) { return plugin_step(SIMP(aSim)); }

static uint64_t h_clocks(void *aSim) { return SIMP(aSim)->total_clocks; }

static long h_instructions(void *aSim) { return SIMP(aSim)->total_instructions; }

static int h_pin_level(void *aSim, uint8_t aPort, uint8_t aBit)
{
    return sim_pin_level(SIMP(aSim), (uint8_t)(aPort & 0x7F), (uint8_t)(aBit & 7));
}

static void h_pin_drive(void *aSim, uint8_t aPort, uint8_t aBit, int aLevel)
{
    sim_drive_pin(SIMP(aSim), (uint8_t)(aPort & 0x7F), (uint8_t)(aBit & 7), aLevel);
}

static void on_watched_write(struct em8051 *aCPU, uint8_t aReg, uint8_t aValue, void *aUser)
{
    (void)aCPU;
    (void)aValue;
    plugin_pins_changed((struct sim *)aUser, aReg);
}

void plugin_pins_changed(struct sim *aSim, uint8_t aReg)
{
    int i;
    if (!aSim->pin_reg_watched[aReg & 0x7F])
        return;
    for (i = 0; i < aSim->pin_watch_count; i++)
    {
        pin_watch_t *w = &aSim->pin_watch[i];
        int lv;
        if (w->reg != aReg)
            continue;
        lv = sim_pin_level(aSim, w->reg, w->bit);
        if (lv != w->level)
        {
            w->level = lv;
            w->fn(w->user, w->reg, w->bit, lv);
        }
    }
}

static int h_on_pin(void *aSim, uint8_t aPort, uint8_t aBit, emu8051_pin_fn aFn, void *aUser)
{
    struct sim *s = SIMP(aSim);
    pin_watch_t *w;
    aPort &= 0x7F;
    if (s->pin_watch_count >= MAX_PIN_WATCH || !aFn || aBit > 7)
        return -1;
    w = &s->pin_watch[s->pin_watch_count++];
    w->reg = aPort;
    w->bit = aBit;
    w->fn = aFn;
    w->user = aUser;
    w->level = sim_pin_level(s, aPort, aBit);
    if (!s->pin_reg_watched[aPort])
    {
        s->pin_reg_watched[aPort] = 1;
        bus_on_write(s->bus, aPort, on_watched_write, s);
    }
    return 0;
}

static sim_plugin_t *plugin_named(struct sim *s, const char *aName)
{
    int i;
    if (!aName)
        return NULL;
    for (i = 0; i < s->plugin_count; i++)
        if (s->plugins[i].name[0] && strcmp(s->plugins[i].name, aName) == 0)
            return &s->plugins[i];
    return NULL;
}

static int h_call(void *aSim, const char *aPlugin, const char *aMethod, const uint8_t *aIn, int aInLen,
                  uint8_t *aOut, int aOutCap, int *aOutLen)
{
    return sim_plugin_call(aSim, aPlugin, aMethod, aIn, aInLen, aOut, aOutCap, aOutLen);
}

static void h_error(void *aSim, const char *aMessage)
{
    struct sim *s = SIMP(aSim);
    snprintf(s->plugin_error, sizeof(s->plugin_error), "%s", aMessage ? aMessage : "");
}

void plugin_host_init(struct sim *aSim)
{
    emu8051_host_t *h = &aSim->host;
    memset(h, 0, sizeof(*h));
    h->abi = EMU8051_PLUGIN_ABI;
    h->size = (uint32_t)sizeof(*h);
    h->sim = aSim;
    h->cpu = h_cpu;
    h->set_memory = h_set_memory;
    h->load_hex = h_load_hex;
    h->reset_cpu = h_reset_cpu;
    h->clock_hz = h_clock_hz;
    h->raise = h_raise;
    h->on_exception = h_on_exception;
    h->on_sfr_write = h_on_sfr_write;
    h->on_sfr_read = h_on_sfr_read;
    h->on_xdata = h_on_xdata;
    h->set_cycles = h_set_cycles;
    h->set_opcode = h_set_opcode;
    h->on_step = h_on_step;
    h->on_interrupt = h_on_interrupt;
    h->enter_vector = h_enter_vector;
    h->step = h_step;
    h->clocks = h_clocks;
    h->instructions = h_instructions;
    h->pin_level = h_pin_level;
    h->pin_drive = h_pin_drive;
    h->on_pin = h_on_pin;
    h->call = h_call;
    h->error = h_error;
}

// ------------------------------------------------------------------ hooks the host calls

int plugin_filter_exception(struct sim *aSim, int aCode)
{
    int i;
    for (i = 0; i < aSim->exc_hook_count && aCode >= 0; i++)
        aCode = aSim->exc_hooks[i].fn(aSim->exc_hooks[i].user, aCode);
    return aCode;
}

static xdata_hook_t *xdata_hook(struct sim *s, uint16_t aAddr)
{
    int i;
    for (i = s->xdata_hook_count - 1; i >= 0; i--)
        if (aAddr >= s->xdata_hooks[i].lo && aAddr <= s->xdata_hooks[i].hi)
            return &s->xdata_hooks[i];
    return NULL;
}

int plugin_xdata_read(struct sim *aSim, uint16_t aAddr, uint8_t *aValue)
{
    xdata_hook_t *x = xdata_hook(aSim, aAddr);
    if (!x || !x->rd)
        return 0;
    *aValue = x->rd(x->user, aAddr);
    return 1;
}

int plugin_xdata_write(struct sim *aSim, uint16_t aAddr, uint8_t aValue)
{
    xdata_hook_t *x = xdata_hook(aSim, aAddr);
    if (!x || !x->wr)
        return 0;
    x->wr(x->user, aAddr, aValue);
    return 1;
}

static void end_hooks(struct sim *s, uint32_t aClocks, int aKind)
{
    int i;
    for (i = 0; i < s->step_hook_count; i++)
        if (s->step_hooks[i].h.end)
            s->step_hooks[i].h.end(s->step_hooks[i].user, aClocks, aKind);
}

static uint32_t step_instruction(struct sim *s)
{
    struct em8051 *cpu = &s->cpu;
    uint32_t clocks = 0;
    int kind = EMU8051_STEP_INSTRUCTION;
    int i;

    for (i = 0; i < s->step_hook_count; i++)
        if (s->step_hooks[i].h.begin)
        {
            int32_t r = s->step_hooks[i].h.begin(s->step_hooks[i].user);
            if (r >= 0)
            {
                clocks = (uint32_t)r;
                kind = EMU8051_STEP_WAIT;
                goto done;
            }
        }
    for (i = 0; i < s->irq_hook_count; i++)
    {
        uint16_t vector = 0;
        uint32_t c = 24;
        if (s->irq_hooks[i].fn(s->irq_hooks[i].user, &vector, &c))
        {
            enter_vector(s, vector);
            clocks = c;
            kind = EMU8051_STEP_INTERRUPT;
            goto done;
        }
    }
    {
        uint16_t mask = cpu->mCodeMemMaxIdx;
        uint16_t pc = cpu->mPC;
        uint8_t op = cpu->mCodeMem[pc & mask];
        uint16_t next = (uint16_t)(pc + OPLEN[op]);
        uint16_t rel_at = (uint16_t)((next - 1) & mask);
        int patched = 0, taken;
        uint8_t ret, v;

        for (i = 0; i < s->step_hook_count; i++)
            if (s->step_hooks[i].h.before)
                s->step_hooks[i].h.before(s->step_hooks[i].user, op);
        // A conditional jump with offset 0 lands on the next instruction
        // whether it jumps or not; run it with the offset patched to 1 to
        // tell which, for the taken column.
        if (s->have_cycles && s->cycles_taken[op] && cond_rel(op) && cpu->mCodeMem[rel_at] == 0)
        {
            cpu->mCodeMem[rel_at] = 1;
            patched = 1;
        }
        ret = cpu->op[op](cpu);
        if (patched)
        {
            cpu->mCodeMem[rel_at] = 0;
            taken = cpu->mPC == (uint16_t)(next + 1);
            if (taken)
                cpu->mPC = next;
        }
        else
            taken = cpu->mPC != next;
        if (s->have_cycles)
            clocks = (taken && s->cycles_taken[op]) ? s->cycles_taken[op] : s->cycles[op];
        else
            clocks = 12u * (ret + 1u);

        // parity, as tick() does after every instruction
        v = cpu->mSFR[REG_ACC];
        v ^= v >> 4;
        v &= 0xF;
        v = (0x6996 >> v) & 1;
        cpu->mSFR[REG_PSW] = (uint8_t)((cpu->mSFR[REG_PSW] & ~PSWMASK_P) | (v * PSWMASK_P));

        for (i = 0; i < s->step_hook_count; i++)
            if (s->step_hooks[i].h.after)
                clocks = s->step_hooks[i].h.after(s->step_hooks[i].user, op, clocks, taken);
        s->total_instructions++;
    }
done:
    end_hooks(s, clocks, kind);
    s->total_clocks += clocks;
    // Board devices count ticks of 12 clocks; give them one per 12 clocks
    // executed, whatever the instructions cost (plugin.h, "The two
    // execution modes").
    s->device_clocks += clocks;
    while (s->device_clocks >= 12u)
    {
        s->device_clocks -= 12u;
        sim_post_tick(s);
    }
    return clocks;
}

uint32_t plugin_step(struct sim *aSim)
{
    if (aSim->instruction_mode)
        return step_instruction(aSim);
    if (tick(&aSim->cpu))
        aSim->total_instructions++;
    sim_post_tick(aSim);
    aSim->total_ticks++;
    end_hooks(aSim, 12, EMU8051_STEP_TICK);
    aSim->total_clocks += 12;
    return 12;
}

void plugin_reset_all(struct sim *aSim)
{
    int i;
    for (i = 0; i < aSim->plugin_count; i++)
        if (aSim->plugins[i].p.reset)
            aSim->plugins[i].p.reset(aSim->plugins[i].p.ctx);
}

static void close_dl(void *aDl)
{
    if (!aDl)
        return;
#if defined(_WIN32)
    FreeLibrary((HMODULE)aDl);
#else
    dlclose(aDl);
#endif
}

void plugin_destroy_all(struct sim *aSim)
{
    int i;
    for (i = aSim->plugin_count - 1; i >= 0; i--)
    {
        sim_plugin_t *p = &aSim->plugins[i];
        if (p->p.destroy)
            p->p.destroy(p->p.ctx);
    }
    // Only now: a plugin's destroy may still run code of another one it
    // was given (callbacks), so no object is unloaded before all are done.
    for (i = aSim->plugin_count - 1; i >= 0; i--)
    {
        close_dl(aSim->plugins[i].dl);
        free(aSim->plugins[i].path);
    }
    aSim->plugin_count = 0;
}

// ------------------------------------------------------------------ capi.h

int sim_load_plugin(sim_handle_t aSim, const char *aPath, const char *aArgs)
{
    struct sim *s = SIMP(aSim);
    sim_plugin_t *p;
    void *dl;
    emu8051_plugin_init_fn init;
    int rc;

    if (!s || !aPath)
        return SIM_PLUGIN_OPEN_FAILED;
    s->plugin_error[0] = 0;
    if (s->plugin_count >= MAX_PLUGINS)
    {
        h_error(s, "too many plugins");
        return SIM_PLUGIN_TOO_MANY;
    }
#if defined(_WIN32)
    dl = (void *)LoadLibraryA(aPath);
    if (!dl)
    {
        snprintf(s->plugin_error, sizeof(s->plugin_error), "cannot load %s (error %lu)", aPath,
                 (unsigned long)GetLastError());
        return SIM_PLUGIN_OPEN_FAILED;
    }
    init = (emu8051_plugin_init_fn)(void (*)(void))GetProcAddress((HMODULE)dl, EMU8051_PLUGIN_ENTRY);
#else
    dl = dlopen(aPath, RTLD_NOW | RTLD_LOCAL);
    if (!dl)
    {
        const char *e = dlerror();
        h_error(s, e ? e : "dlopen failed");
        return SIM_PLUGIN_OPEN_FAILED;
    }
    *(void **)(&init) = dlsym(dl, EMU8051_PLUGIN_ENTRY);
#endif
    if (!init)
    {
        snprintf(s->plugin_error, sizeof(s->plugin_error), "%s has no %s", aPath, EMU8051_PLUGIN_ENTRY);
        close_dl(dl);
        return SIM_PLUGIN_NO_ENTRY;
    }

    p = &s->plugins[s->plugin_count];
    memset(p, 0, sizeof(*p));
    p->p.abi = EMU8051_PLUGIN_ABI;
    p->p.size = (uint32_t)sizeof(p->p);
    p->dl = dl;
    // From here the slot is taken even if init fails: whatever the plugin
    // hooked in before it gave up still points into its object, so the
    // object stays loaded, inert (no name), until sim_close().
    s->plugin_count++;
    rc = init(&s->host, &p->p, aArgs ? aArgs : "");
    if (rc != 0)
    {
        if (!s->plugin_error[0])
            snprintf(s->plugin_error, sizeof(s->plugin_error), "%s: init returned %d", aPath, rc);
        memset(&p->p, 0, sizeof(p->p));
        return SIM_PLUGIN_INIT_FAILED;
    }
    if (p->p.abi != EMU8051_PLUGIN_ABI)
    {
        // Built against another header: its view of these structs cannot
        // be trusted, so nothing more of it is called, destroy included.
        snprintf(s->plugin_error, sizeof(s->plugin_error), "%s: built for plugin ABI %u, host has %u", aPath,
                 (unsigned)p->p.abi, (unsigned)EMU8051_PLUGIN_ABI);
        memset(&p->p, 0, sizeof(p->p));
        return SIM_PLUGIN_ABI_MISMATCH;
    }
    // The name the host knows it by: the plugin's, with "#2", "#3", ...
    // appended when one of that name is already loaded here.
    {
        const char *base = (p->p.name && p->p.name[0]) ? p->p.name : "plugin";
        int n = 1;
        snprintf(p->name, sizeof(p->name), "%s", base);
        while (plugin_named(s, p->name) != p)
            snprintf(p->name, sizeof(p->name), "%.36s#%d", base, ++n);
    }
    p->path = (char *)malloc(strlen(aPath) + 1);
    strcpy(p->path, aPath);
    return SIM_PLUGIN_OK;
}

const char *sim_plugin_error(sim_handle_t aSim)
{
    return SIMP(aSim)->plugin_error;
}

int sim_plugin_count(sim_handle_t aSim)
{
    struct sim *s = SIMP(aSim);
    int i, n = 0;
    for (i = 0; i < s->plugin_count; i++)
        if (s->plugins[i].name[0])
            n++;
    return n;
}

const char *sim_plugin_name(sim_handle_t aSim, int aIndex)
{
    struct sim *s = SIMP(aSim);
    int i;
    for (i = 0; i < s->plugin_count; i++)
        if (s->plugins[i].name[0] && aIndex-- == 0)
            return s->plugins[i].name;
    return NULL;
}

void *sim_plugin_context(sim_handle_t aSim, const char *aName)
{
    sim_plugin_t *p = plugin_named(SIMP(aSim), aName);
    return p ? p->p.ctx : NULL;
}

const char *sim_plugin_path(sim_handle_t aSim, const char *aName)
{
    sim_plugin_t *p = plugin_named(SIMP(aSim), aName);
    return p ? p->path : NULL;
}

int sim_plugin_call(sim_handle_t aSim, const char *aPlugin, const char *aMethod, const unsigned char *aIn,
                    int aInLen, unsigned char *aOut, int aOutCap, int *aOutLen)
{
    sim_plugin_t *p = plugin_named(SIMP(aSim), aPlugin);
    int scratch = 0;
    if (!aOutLen)
        aOutLen = &scratch;
    *aOutLen = 0;
    if (!p)
        return EMU8051_CALL_NO_PLUGIN;
    if (!p->p.call || !aMethod)
        return EMU8051_CALL_UNKNOWN;
    if (aInLen < 0 || (aInLen > 0 && !aIn) || aOutCap < 0 || (aOutCap > 0 && !aOut))
        return EMU8051_CALL_BAD_ARGS;
    return p->p.call(p->p.ctx, aMethod, aIn, aInLen, aOut, aOutCap, aOutLen);
}

unsigned long long sim_get_clock_count(sim_handle_t aSim)
{
    return SIMP(aSim)->total_clocks;
}

int sim_instruction_mode(sim_handle_t aSim)
{
    return SIMP(aSim)->instruction_mode;
}
