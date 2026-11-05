/* demo_plugin.c - a plugin that exercises sim/plugin.h, for the tests
 * Copyright 2026 Thomas Reidemeister, MIT License (see sim/devices/hc573.h)
 *
 * It models nothing real. Every part of the host's function table is used
 * once, in the smallest way that a test can check from outside
 * (python/tests/test_plugin.py):
 *
 *   SFR F9h      writes are logged (method "sfr_log")
 *   SFR FAh      reads count up from 1
 *   SFR FBh      bit 0 enables source 1, bit 1 source 2 (with "irq")
 *   xdata FF00h-FF0Fh, a register window: FF0Dh reads how many bytes wait
 *                in the receive queue ("inject"), FF0Eh reads the next one,
 *                FF0Fh is written to send ("tx"); the rest are plain cells
 *   P1.0         every change of its level is logged ("pin_log")
 *
 * Arguments, separated by spaces, all optional:
 *
 *   name=X       the plugin's name (default "demo")
 *   1t           a one-clock-per-instruction cycle table: 1 clock, 3 for a
 *                conditional jump that jumps, 4 for MUL and DIV, plus the
 *                step hooks (begin, before, after) and so instruction mode
 *   irq          an interrupt hook with two sources of its own: source 1
 *                at vector 4Bh (interrupt 9, low priority), source 2 at
 *                53h (interrupt 10, high priority, preempts source 1);
 *                EA gates both; "raise" sets one pending
 *   a5           opcode A5h (reserved on the 8051) becomes INC A
 *   drop=N       exception code N is dropped (the others are counted)
 *   code=N xdata=N upper=0|1   memory sizes
 *   fail         init refuses, with a message
 *   abi=N        claims to be built against ABI N
 *
 * Methods: echo, inject, tx, sfr_log, raise (1 byte: the source),
 * irq_counts, stall (4 bytes LE: clocks for the next step's begin), stats,
 * pin_log, drive (port register, bit, level or FFh to release), level
 * (port register, bit), mem, clocks, relay ("plugin\0method\0payload").
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sim/plugin.h"

#define SFR_LOG 0xF9
#define SFR_COUNT 0xFA
#define SFR_ENABLE 0xFB
#define RX_LEN 64

typedef struct
{
    const emu8051_host_t *h;
    char name[32];
    int one_t, irq, drop;

    uint8_t sfr_log[64];
    int nsfr;
    uint8_t reads;

    uint8_t cells[16];
    uint8_t rx[RX_LEN];
    int rx_head, rx_n;
    uint8_t tx[64];
    int ntx;

    int pending[3];
    int levels[8]; // priority levels in service, innermost last
    int depth;
    uint32_t irq_entries[3];

    uint32_t stall;
    uint32_t resets, instructions, waits, interrupts, ticks, exceptions;

    uint8_t pin_log[64];
    int npin;
} demo_t;

static void put32(uint8_t *aOut, uint32_t aValue)
{
    int i;
    for (i = 0; i < 4; i++)
        aOut[i] = (uint8_t)(aValue >> (8 * i));
}

// ------------------------------------------------------------------ SFRs, xdata, pins

static void on_log_write(struct em8051 *aCPU, uint8_t aReg, uint8_t aValue, void *aUser)
{
    demo_t *d = (demo_t *)aUser;
    (void)aCPU;
    (void)aReg;
    if (d->nsfr < (int)sizeof(d->sfr_log))
        d->sfr_log[d->nsfr++] = aValue;
}

static void on_count_read(struct em8051 *aCPU, uint8_t aReg, void *aUser, uint8_t *aMask, uint8_t *aValue)
{
    demo_t *d = (demo_t *)aUser;
    (void)aCPU;
    (void)aReg;
    *aMask = 0xFF;
    *aValue = ++d->reads;
}

static uint8_t x_read(void *aUser, uint16_t aAddr)
{
    demo_t *d = (demo_t *)aUser;
    uint8_t v;
    switch (aAddr & 0x0F)
    {
    case 0x0D:
        return (uint8_t)d->rx_n;
    case 0x0E:
        if (!d->rx_n)
            return 0;
        v = d->rx[d->rx_head];
        d->rx_head = (d->rx_head + 1) % RX_LEN;
        d->rx_n--;
        return v;
    default:
        return d->cells[aAddr & 0x0F];
    }
}

static void x_write(void *aUser, uint16_t aAddr, uint8_t aValue)
{
    demo_t *d = (demo_t *)aUser;
    if ((aAddr & 0x0F) == 0x0F)
    {
        if (d->ntx < (int)sizeof(d->tx))
            d->tx[d->ntx++] = aValue;
    }
    else
        d->cells[aAddr & 0x0F] = aValue;
}

static void on_pin(void *aUser, uint8_t aPort, uint8_t aBit, int aLevel)
{
    demo_t *d = (demo_t *)aUser;
    (void)aPort;
    (void)aBit;
    if (d->npin < (int)sizeof(d->pin_log))
        d->pin_log[d->npin++] = (uint8_t)aLevel;
}

// ------------------------------------------------------------------ execution

static int32_t step_begin(void *aUser)
{
    demo_t *d = (demo_t *)aUser;
    uint32_t n = d->stall;
    if (!n)
        return EMU8051_STEP_RUN;
    d->stall = 0;
    return (int32_t)n;
}

static uint32_t step_after(void *aUser, uint8_t aOp, uint32_t aClocks, int aTaken)
{
    demo_t *d = (demo_t *)aUser;
    (void)aTaken;
    d->instructions++;
    if (aOp == 0x32 && d->depth > 0) // RETI: the core pops the PC only
        d->depth--;
    return aClocks;
}

static void step_end(void *aUser, uint32_t aClocks, int aKind)
{
    demo_t *d = (demo_t *)aUser;
    (void)aClocks;
    switch (aKind)
    {
    case EMU8051_STEP_WAIT: d->waits++; break;
    case EMU8051_STEP_INTERRUPT: d->interrupts++; break;
    case EMU8051_STEP_TICK: d->ticks++; break;
    default: break;
    }
}

static int irq_poll(void *aUser, uint16_t *aVector, uint32_t *aClocks)
{
    demo_t *d = (demo_t *)aUser;
    struct em8051 *cpu = d->h->cpu(d->h->sim);
    uint8_t en = cpu->mSFR[SFR_ENABLE - 0x80];
    int current = d->depth ? d->levels[d->depth - 1] : -1;
    int src;
    if (!(cpu->mSFR[REG_IE] & 0x80))
        return 0;
    for (src = 2; src >= 1; src--) // the higher priority first
    {
        int level = src - 1;
        if (!d->pending[src] || !(en & (1u << (src - 1))) || level <= current)
            continue;
        d->pending[src] = 0;
        if (d->depth < 8)
            d->levels[d->depth++] = level;
        d->irq_entries[src]++;
        *aVector = (uint16_t)(src == 1 ? 0x4B : 0x53);
        if (d->one_t)
            *aClocks = 3;
        return 1;
    }
    return 0;
}

static uint8_t op_inc_a(void *aUser, struct em8051 *aCPU)
{
    (void)aUser;
    aCPU->mSFR[REG_ACC]++;
    aCPU->mPC++;
    return 0;
}

static int on_exception(void *aUser, int aCode)
{
    demo_t *d = (demo_t *)aUser;
    d->exceptions++;
    return aCode == d->drop ? -1 : aCode;
}

// ------------------------------------------------------------------ plugin

static void demo_reset(void *aCtx)
{
    demo_t *d = (demo_t *)aCtx;
    d->resets++;
    d->rx_n = d->rx_head = 0;
    d->depth = 0;
    memset(d->pending, 0, sizeof(d->pending));
}

static void demo_destroy(void *aCtx)
{
    free(aCtx);
}

static int give(const void *aData, int aLen, uint8_t *aOut, int aCap, int *aOutLen)
{
    *aOutLen = aLen;
    if (aLen > aCap)
        return EMU8051_CALL_TOO_SMALL;
    memcpy(aOut, aData, (size_t)aLen);
    return EMU8051_CALL_OK;
}

static int demo_call(void *aCtx, const char *aMethod, const uint8_t *aIn, int aInLen, uint8_t *aOut, int aCap,
                     int *aOutLen)
{
    demo_t *d = (demo_t *)aCtx;
    const emu8051_host_t *h = d->h;
    uint8_t buf[64];
    int i;

    if (!strcmp(aMethod, "echo"))
        return give(aIn, aInLen, aOut, aCap, aOutLen);
    if (!strcmp(aMethod, "inject"))
    {
        for (i = 0; i < aInLen && d->rx_n < RX_LEN; i++)
        {
            d->rx[(d->rx_head + d->rx_n) % RX_LEN] = aIn[i];
            d->rx_n++;
        }
        return EMU8051_CALL_OK;
    }
    if (!strcmp(aMethod, "tx"))
        return give(d->tx, d->ntx, aOut, aCap, aOutLen);
    if (!strcmp(aMethod, "sfr_log"))
        return give(d->sfr_log, d->nsfr, aOut, aCap, aOutLen);
    if (!strcmp(aMethod, "pin_log"))
        return give(d->pin_log, d->npin, aOut, aCap, aOutLen);
    if (!strcmp(aMethod, "raise"))
    {
        if (aInLen != 1 || aIn[0] < 1 || aIn[0] > 2)
            return EMU8051_CALL_BAD_ARGS;
        d->pending[aIn[0]] = 1;
        return EMU8051_CALL_OK;
    }
    if (!strcmp(aMethod, "irq_counts"))
    {
        put32(buf, d->irq_entries[1]);
        put32(buf + 4, d->irq_entries[2]);
        return give(buf, 8, aOut, aCap, aOutLen);
    }
    if (!strcmp(aMethod, "stall"))
    {
        if (aInLen != 4)
            return EMU8051_CALL_BAD_ARGS;
        d->stall = (uint32_t)aIn[0] | (uint32_t)aIn[1] << 8 | (uint32_t)aIn[2] << 16 | (uint32_t)aIn[3] << 24;
        return EMU8051_CALL_OK;
    }
    if (!strcmp(aMethod, "stats"))
    {
        uint32_t v[6] = {d->resets, d->instructions, d->waits, d->interrupts, d->ticks, d->exceptions};
        for (i = 0; i < 6; i++)
            put32(buf + 4 * i, v[i]);
        return give(buf, 24, aOut, aCap, aOutLen);
    }
    if (!strcmp(aMethod, "drive"))
    {
        if (aInLen != 3)
            return EMU8051_CALL_BAD_ARGS;
        h->pin_drive(h->sim, aIn[0], aIn[1], aIn[2] == 0xFF ? -1 : aIn[2]);
        return EMU8051_CALL_OK;
    }
    if (!strcmp(aMethod, "level"))
    {
        if (aInLen != 2)
            return EMU8051_CALL_BAD_ARGS;
        buf[0] = (uint8_t)h->pin_level(h->sim, aIn[0], aIn[1]);
        return give(buf, 1, aOut, aCap, aOutLen);
    }
    if (!strcmp(aMethod, "mem"))
    {
        struct em8051 *cpu = h->cpu(h->sim);
        put32(buf, (uint32_t)cpu->mCodeMemMaxIdx + 1);
        put32(buf + 4, (uint32_t)cpu->mExtDataMaxIdx + 1);
        buf[8] = cpu->mUpperData != NULL;
        return give(buf, 9, aOut, aCap, aOutLen);
    }
    if (!strcmp(aMethod, "clocks"))
    {
        uint64_t c = h->clocks(h->sim);
        put32(buf, (uint32_t)c);
        put32(buf + 4, (uint32_t)(c >> 32));
        put32(buf + 8, (uint32_t)h->instructions(h->sim));
        return give(buf, 12, aOut, aCap, aOutLen);
    }
    if (!strcmp(aMethod, "relay"))
    {
        // "plugin\0method\0payload"
        const char *plugin = (const char *)aIn, *method;
        int n1, n2;
        if (aInLen < 4 || !memchr(aIn, 0, (size_t)aInLen))
            return EMU8051_CALL_BAD_ARGS;
        n1 = (int)strlen(plugin) + 1;
        if (n1 >= aInLen || !memchr(aIn + n1, 0, (size_t)(aInLen - n1)))
            return EMU8051_CALL_BAD_ARGS;
        method = (const char *)aIn + n1;
        n2 = n1 + (int)strlen(method) + 1;
        return h->call(h->sim, plugin, method, aIn + n2, aInLen - n2, aOut, aCap, aOutLen);
    }
    return EMU8051_CALL_UNKNOWN;
}

static long arg_long(const char *aArgs, const char *aKey, long aDefault)
{
    const char *p = strstr(aArgs, aKey);
    size_t n = strlen(aKey);
    while (p && !((p == aArgs || p[-1] == ' ') && p[n] == '='))
        p = strstr(p + 1, aKey);
    return p ? strtol(p + n + 1, NULL, 0) : aDefault;
}

static int has_flag(const char *aArgs, const char *aFlag)
{
    const char *p = aArgs;
    size_t n = strlen(aFlag);
    while ((p = strstr(p, aFlag)) != NULL)
    {
        if ((p == aArgs || p[-1] == ' ') && (p[n] == 0 || p[n] == ' '))
            return 1;
        p += n;
    }
    return 0;
}

EMU8051_PLUGIN_EXPORT int emu8051_plugin_init(const emu8051_host_t *aHost, emu8051_plugin_t *aPlugin,
                                              const char *aArgs)
{
    demo_t *d;
    const char *nm;
    emu8051_step_hooks_t hooks;
    long code, xdata;

    if (aHost->abi != EMU8051_PLUGIN_ABI || !EMU8051_HOST_HAS(aHost, error))
        return -1;
    if (has_flag(aArgs, "fail"))
    {
        aHost->error(aHost->sim, "demo: asked to fail");
        return -1;
    }
    d = (demo_t *)calloc(1, sizeof(*d));
    d->h = aHost;
    snprintf(d->name, sizeof(d->name), "demo");
    nm = strstr(aArgs, "name=");
    if (nm && (nm == aArgs || nm[-1] == ' '))
        sscanf(nm + 5, "%31s", d->name);
    d->one_t = has_flag(aArgs, "1t");
    d->irq = has_flag(aArgs, "irq");
    d->drop = (int)arg_long(aArgs, "drop", -1);

    code = arg_long(aArgs, "code", 0);
    xdata = arg_long(aArgs, "xdata", 0);
    if (code || xdata)
    {
        struct em8051 *cpu = aHost->cpu(aHost->sim);
        if (aHost->set_memory(aHost->sim, code ? (uint32_t)code : (uint32_t)cpu->mCodeMemMaxIdx + 1,
                              xdata ? (uint32_t)xdata : (uint32_t)cpu->mExtDataMaxIdx + 1,
                              (int)arg_long(aArgs, "upper", 1)) != 0)
        {
            aHost->error(aHost->sim, "demo: bad memory size");
            free(d);
            return -1;
        }
    }

    aHost->on_sfr_write(aHost->sim, SFR_LOG - 0x80, on_log_write, d);
    aHost->on_sfr_read(aHost->sim, SFR_COUNT - 0x80, on_count_read, d);
    aHost->on_xdata(aHost->sim, 0xFF00, 0xFF0F, x_read, x_write, d);
    aHost->on_pin(aHost->sim, REG_P1, 0, on_pin, d);
    aHost->on_exception(aHost->sim, on_exception, d);

    memset(&hooks, 0, sizeof(hooks));
    hooks.end = step_end;
    if (d->one_t || d->irq)
    {
        hooks.begin = step_begin;
        hooks.after = step_after;
    }
    aHost->on_step(aHost->sim, &hooks, d);
    if (d->one_t)
    {
        uint8_t cycles[256], taken[256];
        int op;
        memset(cycles, 1, sizeof(cycles));
        memset(taken, 0, sizeof(taken));
        cycles[0xA4] = cycles[0x84] = 4; // MUL AB, DIV AB
        for (op = 0; op < 256; op++)
            if (((op & 0x0F) == 0 && op >= 0x10 && op <= 0x70) || (op >= 0xB4 && op <= 0xBF) || op == 0xD5 ||
                (op >= 0xD8 && op <= 0xDF))
                taken[op] = 3;
        aHost->set_cycles(aHost->sim, cycles, taken);
    }
    if (d->irq)
        aHost->on_interrupt(aHost->sim, irq_poll, d);
    if (has_flag(aArgs, "a5"))
        aHost->set_opcode(aHost->sim, 0xA5, op_inc_a, d);

    aPlugin->name = d->name;
    aPlugin->ctx = d;
    aPlugin->destroy = demo_destroy;
    aPlugin->reset = demo_reset;
    aPlugin->call = demo_call;
    aPlugin->abi = (uint32_t)arg_long(aArgs, "abi", EMU8051_PLUGIN_ABI);
    return 0;
}

// For a binding that calls the plugin directly (Plugin.function in pysim).
EMU8051_PLUGIN_EXPORT int demo_rx_waiting(void *aCtx)
{
    return ((demo_t *)aCtx)->rx_n;
}
