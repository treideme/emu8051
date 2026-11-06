/* hc6800_es_plugin.h - the HC6800-ES board as a plugin (see ../plugin.h)
 * Copyright 2026 Thomas Reidemeister, MIT License (see ../devices/hc573.h)
 *
 * The board's devices, wired from the pin catalog in hc6800_es.h, as a
 * plugin named "hc6800_es". It uses nothing of the host but plugin.h's
 * function table, so it builds two ways from the same source:
 *
 * - linked in: libem8051sim compiles hc6800_es_plugin.c and adds it with
 *   sim_add_plugin(sim, hc6800_es_plugin_init, ""). sim_open("hc6800_es")
 *   does that, and capi.h's sim_enable_* and query functions are calls of
 *   the methods below.
 * - as a shared object: hc6800_es_plugin.c plus hc6800_es_entry.c (the
 *   exported emu8051_plugin_init), with the device models, give
 *   build/hc6800_es.so (hc6800_es.dll on Windows), which
 *   sim_load_plugin() opens like any plugin built outside this tree.
 *
 * Nothing is attached until a method enables it: the plugin alone
 * subscribes to no SFR and installs no hook, so a simulation with no
 * device enabled runs as a bare one does. The worked example of
 * sim/PLUGIN-TUTORIAL.md.
 *
 * Methods (sim_plugin_call). Each is named after the capi.h function it
 * backs, without the "sim_" prefix, and takes the same arguments and gives
 * the same results: arguments are 32-bit little-endian integers in the
 * order capi.h has them, and an integer result is one more. Where a device
 * is not enabled, a query answers what capi.h's function answers then (0,
 * -1, an empty line). Ports are 0-3 for P0-P3, bits 0-7.
 *
 *   enable_digit_display  count                  segment bank, 74HC573 + 74HC138
 *   digit_get_segments    digit             -> int
 *   digit_get_char        digit             -> int ('0'-'9', 'A'-'F', '?')
 *   enable_lcd                                    HD44780 1602
 *   lcd_get_line          line, width       -> the text, up to aOutCap bytes
 *   enable_ds1302
 *   ds1302_set_time       s, min, h, date, month, weekday, year
 *   ds1302_get_register   index             -> int
 *   enable_xpt2046
 *   xpt2046_set_reading   value
 *   xpt2046_set_channel_reading  channel, value
 *   xpt2046_get_last_channel                -> int
 *   enable_servo          port, bit, min_us, max_us
 *   servo_get_pulse_us                      -> int
 *   servo_get_angle_decidegrees             -> int
 *   enable_enc28j60       cs, sck, mosi, miso, each as port, bit
 *   enc28j60_get_register bank, address     -> int
 *   enc28j60_get_bank                       -> int
 *   enc28j60_get_last_opcode                -> int
 *   enc28j60_get_buffer_byte_count          -> int
 *   enc28j60_inject_rx    the frame's bytes -> int (ENC28J60_RX_*)
 *   enc28j60_poke_buffer  address, value
 *   enc28j60_peek_buffer  address           -> int
 *   enc28j60_tx_count                       -> int
 *   enc28j60_tx_frame     index             -> the frame's bytes
 *   enable_fan            pwm port, bit, tach port, bit
 *   fan_get_duty_percent                    -> int
 *   fan_get_rpm                             -> int
 *   enable_iap            profile (IAP_PROFILE_*)
 *   iap_peek              address           -> int
 *   iap_poke              address, value
 *   iap_stat              which             -> int
 *
 * Return codes: EMU8051_CALL_OK; EMU8051_CALL_BAD_ARGS for input of the
 * wrong length or a pin outside P0-P3; EMU8051_CALL_TOO_SMALL when aOut
 * cannot hold the result (enc28j60_tx_frame then fills aOut and sets
 * *aOutLen to the frame's full length); HC6800_ES_CALL_FAILED where the
 * capi.h function returns -1 for a reason other than its arguments (no
 * such frame, a poke outside the flash or into a device not enabled, out
 * of memory).
 */
#ifndef SIM_BOARDS_HC6800_ES_PLUGIN_H
#define SIM_BOARDS_HC6800_ES_PLUGIN_H

#include "../plugin.h"

#define HC6800_ES_PLUGIN_NAME "hc6800_es"
#define HC6800_ES_CALL_FAILED (-16)

#ifdef __cplusplus
extern "C"
{
#endif

    // The plugin's init (plugin.h's emu8051_plugin_init_fn). aArgs must be
    // empty.
    int hc6800_es_plugin_init(const emu8051_host_t *aHost, emu8051_plugin_t *aPlugin, const char *aArgs);

#ifdef __cplusplus
}
#endif

#endif // SIM_BOARDS_HC6800_ES_PLUGIN_H
