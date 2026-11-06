/* sim layer -- the HC6800-ES board plugin, linked in and loaded
 *
 * The board is a plugin (sim/boards/hc6800_es_plugin.c) that reaches the
 * simulation only through plugin.h's host table. This runs the same
 * program on three simulations and checks that every device reads the
 * same on all of them, step by step:
 *
 *   built-in  sim_open("hc6800_es"): the copy linked into the library
 *   added     sim_open("bare") + sim_add_plugin(hc6800_es_plugin_init)
 *   loaded    sim_open("bare") + sim_load_plugin(argv[1]), the same source
 *             built as a shared object (skipped in a SIM_NO_DLOPEN build,
 *             which checks that sim_load_plugin() refuses instead)
 *
 * The program writes a pseudo-random byte sequence (an 8-bit Galois LFSR)
 * to P0-P3, so the LCD, the segment latch, the DS1302 and XPT2046 lines,
 * the servo and fan PWM pins and the ENC28J60's SPI lines all see edges;
 * now and then it writes SBUF, and, with the IAP model enabled, sets
 * ISP_CONTR.SWRST so that the model resets the CPU through the host.
 * A negative control checks that the devices did change, so that the
 * comparison is not between three idle boards.
 *
 *     make -C tests check
 *
 * (i.e. the MIT License, same as the rest of the emulator)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sim/capi.h"
#include "sim/plugin.h"
#include "sim/boards/hc6800_es_plugin.h"

static int failures = 0;

static void check(const char *aWhat, long aGot, long aWant)
{
    if (aGot == aWant)
        printf("  ok    %-52s %ld\n", aWhat, aGot);
    else
    {
        printf("  FAIL  %-52s got %ld, expected %ld\n", aWhat, aGot, aWant);
        failures++;
    }
}

// 0000 LJMP 0030h
// 0030 MOV SP,#60h / MOV SCON,#40h / MOV TMOD,#20h / MOV TH1,#0FDh /
//      SETB TR1 / MOV R7,#1
// 0040 loop: ACALL next / MOV P0,A / ACALL next / MOV P2,A / ACALL next /
//      MOV P1,A / ACALL next / MOV P3,A / ACALL next /
//      CJNE A,#74h,+2 / MOV SBUF,A / CJNE A,#0A7h,+3 / MOV ISP_CONTR,#20h /
//      SJMP loop
// The fifth byte of each pass is one of 51 values; 74h comes up in the
// second pass and A7h in the 41st, so with the IAP model each run up to
// the software reset sends one byte.
// 005F next: MOV A,R7 / CLR C / RLC A / JNC +2 / XRL A,#1Dh / MOV R7,A / RET
static const unsigned char PROGRAM_0000[] = {0x02, 0x00, 0x30};
static const unsigned char PROGRAM_0030[] = {
    0x75, 0x81, 0x60, 0x75, 0x98, 0x40, 0x75, 0x89, 0x20, 0x75, 0x8D, 0xFD, 0xD2, 0x8E, 0x7F, 0x01,
    0x11, 0x5F, 0xF5, 0x80, 0x11, 0x5F, 0xF5, 0xA0, 0x11, 0x5F, 0xF5, 0x90, 0x11, 0x5F, 0xF5, 0xB0,
    0x11, 0x5F, 0xB4, 0x74, 0x02, 0xF5, 0x99, 0xB4, 0xA7, 0x03, 0x75, 0xE7, 0x20, 0x80, 0xE1, 0xEF,
    0xC3, 0x33, 0x50, 0x02, 0x64, 0x1D, 0xFF, 0x22};

static const char *HEX = "test_hc6800_plugin.hex";

static void hex_chunk(FILE *aF, unsigned aAddr, const unsigned char *aData, unsigned aLen)
{
    unsigned i, j;
    for (i = 0; i < aLen; i += 16)
    {
        unsigned n = aLen - i < 16 ? aLen - i : 16, a = aAddr + i, sum = n + (a >> 8) + (a & 0xFF);
        fprintf(aF, ":%02X%04X00", n, a);
        for (j = 0; j < n; j++)
        {
            fprintf(aF, "%02X", aData[i + j]);
            sum += aData[i + j];
        }
        fprintf(aF, "%02X\n", (0x100 - (sum & 0xFF)) & 0xFF);
    }
}

static void write_hex(void)
{
    FILE *f = fopen(HEX, "w");
    if (!f)
    {
        perror(HEX);
        exit(2);
    }
    hex_chunk(f, 0x0000, PROGRAM_0000, sizeof(PROGRAM_0000));
    hex_chunk(f, 0x0030, PROGRAM_0030, sizeof(PROGRAM_0030));
    fprintf(f, ":00000001FF\n");
    fclose(f);
}

// Everything a caller can read back, device by device.
typedef struct
{
    int pc, ports[4];
    unsigned long long clocks;
    long instructions;
    unsigned long ticks;
    char lcd[2][17];
    int segments[8], chars[8];
    int ds1302[8];
    int xpt_channel;
    int servo_us, servo_angle;
    int fan_duty, fan_rpm;
    int enc_bank, enc_opcode, enc_tx, enc_regs[4][32];
    unsigned long enc_bytes;
    long iap[6];
    int iap_flash[4];
    int tx_count, tx[18];
    int exception_count, exceptions[64];
} snapshot_t;

static void snapshot(sim_handle_t aSim, snapshot_t *aOut)
{
    int i, j;
    memset(aOut, 0, sizeof(*aOut));
    aOut->pc = sim_get_pc(aSim);
    for (i = 0; i < 4; i++)
        aOut->ports[i] = sim_get_port(aSim, i);
    aOut->clocks = sim_get_clock_count(aSim);
    aOut->instructions = sim_get_instruction_count(aSim);
    aOut->ticks = sim_get_tick_count(aSim);
    for (i = 0; i < 2; i++)
        sim_lcd_get_line(aSim, i, 16, aOut->lcd[i], (int)sizeof(aOut->lcd[i]));
    for (i = 0; i < 8; i++)
    {
        aOut->segments[i] = sim_digit_get_segments(aSim, i);
        aOut->chars[i] = sim_digit_get_char(aSim, i);
        aOut->ds1302[i] = sim_ds1302_get_register(aSim, i);
    }
    aOut->xpt_channel = sim_xpt2046_get_last_channel(aSim);
    aOut->servo_us = sim_servo_get_pulse_us(aSim);
    aOut->servo_angle = sim_servo_get_angle_decidegrees(aSim);
    aOut->fan_duty = sim_fan_get_duty_percent(aSim);
    aOut->fan_rpm = sim_fan_get_rpm(aSim);
    aOut->enc_bank = sim_enc28j60_get_bank(aSim);
    aOut->enc_opcode = sim_enc28j60_get_last_opcode(aSim);
    aOut->enc_tx = sim_enc28j60_tx_count(aSim);
    aOut->enc_bytes = sim_enc28j60_get_buffer_byte_count(aSim);
    for (i = 0; i < 4; i++)
        for (j = 0; j < 32; j++)
            aOut->enc_regs[i][j] = sim_enc28j60_get_register(aSim, i, j);
    for (i = 0; i < 6; i++)
        aOut->iap[i] = sim_iap_stat(aSim, i);
    for (i = 0; i < 4; i++)
        aOut->iap_flash[i] = sim_iap_peek(aSim, 0x2000 + i);
    aOut->tx_count = sim_uart_tx_count(aSim);
    for (i = 0; i < aOut->tx_count && i < 18; i++)
        aOut->tx[i] = sim_uart_tx_byte(aSim, i);
    aOut->exception_count = sim_get_exceptions(aSim, aOut->exceptions, 64);
}

// The devices of the run, enabled the same way on every simulation.
static void enable(sim_handle_t aSim, int aWithIap)
{
    if (aWithIap)
        sim_enable_iap(aSim, 0); // fills code memory with FFh: before the program
    if (sim_load_hex(aSim, HEX) != 0)
    {
        printf("  FAIL  cannot load %s\n", HEX);
        failures++;
    }
    sim_enable_digit_display(aSim, 8);
    sim_enable_lcd(aSim);
    sim_enable_ds1302(aSim);
    sim_ds1302_set_time(aSim, 50, 59, 23, 31, 12, 7, 99);
    sim_enable_xpt2046(aSim);
    sim_xpt2046_set_channel_reading(aSim, 1, 0x5A5);
    sim_enable_servo(aSim, 1, 1, 1000, 2000);
    sim_enable_fan(aSim, 1, 2, 1, 3);
    sim_enable_enc28j60(aSim, 1, 4, 1, 5, 1, 6, 1, 7);
}

#define FLAVOURS 3
static const char *NAMES[FLAVOURS] = {"built-in", "added", "loaded"};

// Runs the program on every simulation given (NULL ones skipped), comparing
// after each slice and resetting all of them halfway. Returns the number
// of comparisons that differed.
static int run_and_compare(sim_handle_t aSims[FLAVOURS], const char *aWhat, int *aChanged)
{
    snapshot_t first, other, idle;
    int slice, f, differ = 0;
    char what[128];

    memset(&idle, 0, sizeof(idle));
    snapshot(aSims[0], &idle);
    *aChanged = 0;
    for (slice = 0; slice < 40; slice++)
    {
        if (slice == 20)
            for (f = 0; f < FLAVOURS; f++)
                if (aSims[f])
                    sim_reset(aSims[f]);
        for (f = 0; f < FLAVOURS; f++)
            if (aSims[f])
                sim_step(aSims[f], 25000);
        snapshot(aSims[0], &first);
        for (f = 1; f < FLAVOURS; f++)
        {
            if (!aSims[f])
                continue;
            snapshot(aSims[f], &other);
            if (memcmp(&first, &other, sizeof(first)) != 0)
            {
                if (!differ)
                    printf("  FAIL  %s: %s differs from %s after slice %d\n", aWhat, NAMES[f], NAMES[0], slice);
                differ++;
            }
        }
        if (strcmp(first.lcd[0], idle.lcd[0]) != 0 || first.segments[0] != idle.segments[0] ||
            first.ds1302[0] != idle.ds1302[0])
            (*aChanged)++;
    }
    snprintf(what, sizeof(what), "%s: slices where a flavour differed", aWhat);
    check(what, differ, 0);
    return differ;
}

static void close_all(sim_handle_t aSims[FLAVOURS])
{
    int f;
    for (f = 0; f < FLAVOURS; f++)
        if (aSims[f])
            sim_close(aSims[f]);
}

static void open_all(sim_handle_t aSims[FLAVOURS], const char *aPath)
{
    aSims[0] = sim_open("hc6800_es");
    aSims[1] = sim_open("bare");
    check("sim_add_plugin(bare, hc6800_es_plugin_init)", sim_add_plugin(aSims[1], hc6800_es_plugin_init, ""),
          SIM_PLUGIN_OK);
    aSims[2] = NULL;
    if (aPath)
    {
        aSims[2] = sim_open("bare");
        if (sim_load_plugin(aSims[2], aPath, "") != SIM_PLUGIN_OK)
        {
            printf("  FAIL  sim_load_plugin(%s): %s\n", aPath, sim_plugin_error(aSims[2]));
            failures++;
            sim_close(aSims[2]);
            aSims[2] = NULL;
        }
    }
}

int main(int aArgc, char **aArgv)
{
    sim_handle_t sims[FLAVOURS];
    const char *path = aArgc > 1 ? aArgv[1] : NULL;
    int changed, i;
    snapshot_t s;

    write_hex();

#if defined(SIM_NO_DLOPEN)
    {
        sim_handle_t b = sim_open("bare");
        printf("SIM_NO_DLOPEN build: linked-in plugins only\n");
        check("sim_load_plugin() refuses", sim_load_plugin(b, path ? path : "hc6800_es.so", ""),
              SIM_PLUGIN_OPEN_FAILED);
        check("  and says why", strstr(sim_plugin_error(b), "SIM_NO_DLOPEN") != NULL, 1);
        sim_close(b);
        path = NULL;
    }
#else
    if (!path)
    {
        printf("usage: %s path/to/hc6800_es.so\n", aArgv[0]);
        return 2;
    }
#endif

    // ---- what each flavour looks like from outside
    open_all(sims, path);
    check("built-in: one plugin", sim_plugin_count(sims[0]), 1);
    check("built-in: named hc6800_es", strcmp(sim_plugin_name(sims[0], 0), HC6800_ES_PLUGIN_NAME), 0);
    check("built-in: no path (linked in)", sim_plugin_path(sims[0], HC6800_ES_PLUGIN_NAME) == NULL, 1);
    check("added: named hc6800_es", strcmp(sim_plugin_name(sims[1], 0), HC6800_ES_PLUGIN_NAME), 0);
    if (sims[2])
    {
        check("loaded: named hc6800_es", strcmp(sim_plugin_name(sims[2], 0), HC6800_ES_PLUGIN_NAME), 0);
        check("loaded: has its path", sim_plugin_path(sims[2], HC6800_ES_PLUGIN_NAME) != NULL, 1);
    }
    close_all(sims);

    // ---- bare: the first sim_enable_* call adds the linked-in board
    {
        sim_handle_t b = sim_open("bare");
        char line[17];
        check("bare: no plugin", sim_plugin_count(b), 0);
        check("bare: sim_enable_lcd()", sim_enable_lcd(b), 0);
        check("bare: now the board plugin", sim_plugin_count(b), 1);
        sim_lcd_get_line(b, 0, 16, line, (int)sizeof(line));
        check("bare: LCD powered up blank", strcmp(line, "                "), 0);
        check("bare: bad servo pin refused", sim_enable_servo(b, 4, 0, 1000, 2000), -1);
        check("bare: unknown method", sim_plugin_call(b, HC6800_ES_PLUGIN_NAME, "no_such", NULL, 0, NULL, 0, NULL),
              EMU8051_CALL_UNKNOWN);
        check("bare: wrong argument length",
              sim_plugin_call(b, HC6800_ES_PLUGIN_NAME, "enable_digit_display", (const unsigned char *)"x", 1, NULL,
                              0, NULL),
              EMU8051_CALL_BAD_ARGS);
        sim_close(b);
    }
    {
        sim_handle_t b = sim_open("bare");
        check("arguments are refused", sim_add_plugin(b, hc6800_es_plugin_init, "x"), SIM_PLUGIN_INIT_FAILED);
        check("  and the reason given", strstr(sim_plugin_error(b), "takes no arguments") != NULL, 1);
        check("NULL init refused", sim_add_plugin(b, NULL, ""), SIM_PLUGIN_NO_ENTRY);
        sim_close(b);
    }

    // ---- the run, without and with the IAP model
    for (i = 0; i < 2; i++)
    {
        const char *what = i ? "with IAP" : "without IAP";
        int f;
        open_all(sims, path);
        for (f = 0; f < FLAVOURS; f++)
            if (sims[f])
                enable(sims[f], i);
        run_and_compare(sims, what, &changed);
        // negative control: the run did reach the devices
        check(i ? "with IAP: slices where devices had changed" : "without IAP: slices where devices had changed",
              changed > 0, 1);
        snapshot(sims[0], &s);
        if (i)
            check("with IAP: software resets through the host", s.iap[4] > 0, 1);
        else
            check("without IAP: UART bytes sent", s.tx_count > 0, 1);
        // the LCD and the digit display survive many resets (each used to
        // add a second set of bus subscriptions)
        for (f = 0; f < 40; f++)
            sim_reset(sims[0]);
        sim_step(sims[0], 50000);
        snapshot(sims[0], &s);
        check("40 resets later the program still runs", s.instructions > 0, 1);
        close_all(sims);
    }

    remove(HEX);
    if (failures)
        printf("test_hc6800_plugin: %d check(s) FAILED\n", failures);
    else
        printf("test_hc6800_plugin: all checks passed\n");
    return failures ? 1 : 0;
}
