/* Flat C API for scripting the simulator from Python (or anything else
 * that can load a shared library and call C functions -- ctypes, cffi,
 * Lua FFI, ...).
 * Copyright 2025 Thomas Reidemeister, MIT License (see devices/hc573.h)
 *
 * capi.h
 *
 * Deliberately kept to an opaque handle plus plain functions taking only
 * primitive types (ints, strings, byte buffers) -- no structs cross this
 * boundary, so there's no struct-layout/padding/ABI fragility for a
 * ctypes.CDLL caller to get wrong. This is the only header a language
 * binding needs; sim/bus.h and the device models under sim/devices are the C-to-C plugin
 * surface, not meant to be bound directly from another language.
 *
 * Everything here is synchronous and single-threaded: call sim_step(),
 * then query whatever you care about, then call sim_step() again. There
 * are no background threads and no callback-based push notifications --
 * a GUI or test driver is expected to poll in its own loop (e.g. a Qt
 * QTimer calling sim_step() then re-reading state), which avoids needing
 * any cross-thread/GIL-juggling machinery here.
 */
#ifndef SIM_CAPI_H
#define SIM_CAPI_H

#include <stdint.h>

#if defined(_WIN32)
#define SIM_API __declspec(dllexport)
#else
#define SIM_API __attribute__((visibility("default")))
#endif

typedef void *sim_handle_t;

#ifdef __cplusplus
extern "C"
{
#endif

    // aBoardName selects a sim/boards/* pinout catalog. Currently only
    // "hc6800_es" exists; add more as new dev-kits show up.
    SIM_API sim_handle_t sim_open(const char *aBoardName);
    SIM_API void sim_close(sim_handle_t aSim);

    // Returns 0 on success, negative on failure (bad path / bad file).
    SIM_API int sim_load_hex(sim_handle_t aSim, const char *aPath);

    // Resets the CPU (registers/PC/SFRs back to power-on state) and
    // anything that's purely a *reflection* of what the CPU has driven
    // onto pins (the digit display's captured segments, the LCD's DDRAM/
    // cursor state) -- both re-created fresh so they don't show stale
    // pre-reset content. Also zeros the instruction/tick/UART-TX counters
    // and the exception log, so a caller can treat this as "start this
    // run over". Does NOT touch DS1302 (a real one is battery-backed and
    // keeps running across an MCU reset), XPT2046 (its reading is
    // host-injected test stimulus, not CPU-derived state), or sim_set_pin
    // overrides (same reasoning -- a forced pin represents something
    // external, like a button, that a reset button doesn't move).
    SIM_API void sim_reset(sim_handle_t aSim);

    // The board's oscillator frequency in Hz -- ticks-per-second in the
    // sim_step()/sim_get_tick_count() sense (1 tick = 1 oscillator cycle,
    // see sim_step's own comment). Needed by a caller that wants to pace
    // itself against real elapsed time (real-time playback) or that wants
    // to convert a peripheral's own tick-scaled timing, like DS1302's
    // real-time-in-simulated-time clock, back into seconds.
    SIM_API unsigned long sim_get_clock_hz(sim_handle_t aSim);

    // Overrides the board's default oscillator frequency (e.g. a 10MHz
    // part instead of hc6800_es's stock 12MHz) -- still assumes a classic
    // 12-clocks-per-machine-cycle core (see sim/devices/ds1302.c's own
    // clock_hz/12 derivation); there's no per-part cycles-per-instruction
    // knob. Call right after sim_open(), before any sim_enable_*() --
    // hd44780/ds1302 both capture clock_hz at their own creation time, so
    // changing it afterwards only affects a peripheral enabled later, not
    // one already running.
    SIM_API void sim_set_clock_hz(sim_handle_t aSim, unsigned long aClockHz);

    // Advance the simulation. sim_step advances aTicks 12-clock ticks
    // (the core's own unit -- see tick() in emu8051.h) and returns how
    // many of those ticks completed a new instruction. sim_step_instructions
    // is a convenience that instead runs until aInstructions instructions
    // have completed (capped internally at aInstructions*100 ticks as a
    // safety net -- every legal opcode completes in well under 100 ticks).
    SIM_API long sim_step(sim_handle_t aSim, long aTicks);
    SIM_API long sim_step_instructions(sim_handle_t aSim, long aInstructions);

    SIM_API long sim_get_instruction_count(sim_handle_t aSim);
    SIM_API unsigned long sim_get_tick_count(sim_handle_t aSim);

    // Program counter, i.e. the address of the instruction that has NOT yet
    // been executed. Read it after sim_step_instructions(aSim, 1) and it is
    // an instruction boundary, which is what makes address breakpoints
    // possible from the host: step one instruction, compare against the
    // address a symbol table gives you, act before it runs. That is the same
    // "stop before executing" convention ucSim's `break` uses, so cycle
    // counts bracketed this way are directly comparable with its.
    SIM_API int sim_get_pc(sim_handle_t aSim);

    // Raw SFR port access. aPortIndex is 0-3 for P0-P3.
    SIM_API int sim_get_port(sim_handle_t aSim, int aPortIndex);

    // Raw internal RAM peek (address 0-127), mostly a debugging aid for
    // cross-checking a .map file's reported address of some C variable.
    SIM_API int sim_peek_idata(sim_handle_t aSim, int aAddress);

    // Raw SFR peek. aAddress is the real SFR address (0x80-0xFF), e.g. 0xA8
    // for IE, 0x98 for SCON -- not the pre-offset REG_* form.
    SIM_API int sim_peek_sfr(sim_handle_t aSim, int aAddress);

    // Force aPortIndex.aBit to read as aValue regardless of what the CPU
    // itself last drove there -- the general-purpose way to fake a button
    // press or any other external signal that isn't handled by one of the
    // named peripheral models below. Call with aValue=-1 to release the
    // pin back to normal (CPU-driven) behavior.
    SIM_API void sim_set_pin(sim_handle_t aSim, int aPortIndex, int aBit, int aValue);
    SIM_API int sim_get_pin(sim_handle_t aSim, int aPortIndex, int aBit);

    // Exceptional CPU conditions (see EM8051_EXCEPTION in emu8051.h),
    // recorded since the last call to this function (it clears the log
    // as it returns the count). aOutCodes must hold at least aMaxCodes ints.
    SIM_API int sim_get_exceptions(sim_handle_t aSim, int *aOutCodes, int aMaxCodes);

    // Model a part's on-chip XRAM size (e.g. 256 for STC89C52RC, 1024 for
    // STC89C5xRD+). Any MOVX at or above it is recorded as exception
    // SIM_EXCEPTION_XRAM_RANGE (the access itself still goes to the 64 KB
    // backing store). 0 (the default) disables the check.
#define SIM_EXCEPTION_XRAM_RANGE 16
    SIM_API void sim_set_xram_size(sim_handle_t aSim, unsigned long aBytes);

    // --- Peripheral models: each is created lazily on its first
    // sim_enable_*() call (idempotent -- calling again is a no-op) and
    // wired to whatever pins aBoardName's catalog says. Query/stimulus
    // functions for a peripheral that was never enabled return 0/empty.

    SIM_API int sim_enable_digit_display(sim_handle_t aSim, int aDigitCount);
    SIM_API int sim_digit_get_segments(sim_handle_t aSim, int aDigit);
    SIM_API int sim_digit_get_char(sim_handle_t aSim, int aDigit); // ASCII '0'-'9'/'A'-'F'/'?'

    SIM_API int sim_enable_lcd(sim_handle_t aSim);
    // Writes up to aOutBufSize-1 chars plus a NUL into aOutBuf.
    SIM_API void sim_lcd_get_line(sim_handle_t aSim, int aLine, int aWidth, char *aOutBuf, int aOutBufSize);

    SIM_API int sim_enable_ds1302(sim_handle_t aSim);
    SIM_API void sim_ds1302_set_time(sim_handle_t aSim, int aSeconds, int aMinutes, int aHours,
                                      int aDate, int aMonth, int aWeekday, int aYear);
    SIM_API int sim_ds1302_get_register(sim_handle_t aSim, int aRegister);

    SIM_API int sim_enable_xpt2046(sim_handle_t aSim);
    SIM_API void sim_xpt2046_set_reading(sim_handle_t aSim, int aValue12Bit);
    SIM_API void sim_xpt2046_set_channel_reading(sim_handle_t aSim, int aChannel, int aValue12Bit);
    SIM_API int sim_xpt2046_get_last_channel(sim_handle_t aSim);

    // --- External peripherals: unlike the board-catalog peripherals
    // above, these are NOT part of any real HC6800-ES pin map (see
    // sim/README.md's "plugin vs board definition" note) -- there's no
    // servo header and no ENC28J60 module on that board. A caller passes
    // its own assumed pin wiring explicitly (aPortIndex 0-3 for P0-P3,
    // aBit 0-7, same convention as sim_get_pin/sim_set_pin above) rather
    // than picking it from a board constant.

    // aPulseMinUs/aPulseMaxUs are typically 1000/2000 for a standard
    // hobby servo (0-180 degrees over a 1-2ms pulse).
    SIM_API int sim_enable_servo(sim_handle_t aSim, int aPwmPortIndex, int aPwmBit,
                                 int aPulseMinUs, int aPulseMaxUs);
    SIM_API int sim_servo_get_pulse_us(sim_handle_t aSim);
    SIM_API int sim_servo_get_angle_decidegrees(sim_handle_t aSim); // 0-1800, tenths of a degree

    // ENC28J60: a register/bank/buffer SPI protocol model, not a network
    // stack -- see sim/devices/enc28j60.h for the exact scope.
    SIM_API int sim_enable_enc28j60(sim_handle_t aSim, int aCsPort, int aCsBit,
                                     int aSckPort, int aSckBit,
                                     int aMosiPort, int aMosiBit,
                                     int aMisoPort, int aMisoBit);
    SIM_API int sim_enc28j60_get_register(sim_handle_t aSim, int aBank, int aAddress);
    SIM_API int sim_enc28j60_get_bank(sim_handle_t aSim);
    SIM_API int sim_enc28j60_get_last_opcode(sim_handle_t aSim);
    SIM_API unsigned long sim_enc28j60_get_buffer_byte_count(sim_handle_t aSim);
    // Packet layer (see sim/devices/enc28j60.h): deliver a frame into the RX
    // buffer (returns 0 or a negative ENC28J60_RX_* code), and read back the
    // frames the firmware transmitted, oldest first.
    SIM_API int sim_enc28j60_inject_rx(sim_handle_t aSim, const unsigned char *aFrame, int aLen);
    SIM_API int sim_enc28j60_tx_count(sim_handle_t aSim);
    SIM_API int sim_enc28j60_tx_frame(sim_handle_t aSim, int aIndex, unsigned char *aOut, int aMax);

    // 4-wire PWM fan: aPwmPort/aPwmBit is the MCU-driven duty-cycle input,
    // aTachPort/aTachBit is the fan-driven tachometer output -- see
    // sim/devices/fan.h for the RPM-mapping/pulses-per-revolution
    // assumptions.
    SIM_API int sim_enable_fan(sim_handle_t aSim, int aPwmPort, int aPwmBit,
                               int aTachPort, int aTachBit);
    SIM_API int sim_fan_get_duty_percent(sim_handle_t aSim); // 0-100
    SIM_API int sim_fan_get_rpm(sim_handle_t aSim);

    // UART TX capture (RX, i.e. the reverse direction, is injected with
    // sim_uart_inject_rx -- there is no separate "enable", the core
    // simulates UART TX unconditionally).
    SIM_API int sim_uart_tx_count(sim_handle_t aSim);
    SIM_API int sim_uart_tx_byte(sim_handle_t aSim, int aIndex);
    // Deposits aByte into SBUF and raises the serial interrupt condition
    // (RI + the same trigger path the core's own TX completion uses),
    // simulating a byte having just arrived over UART.
    SIM_API void sim_uart_inject_rx(sim_handle_t aSim, unsigned char aByte);

    // STC ISP/IAP flash-programming SFRs (sim/devices/iap.h). aProfile:
    // 0 = STC89C52RC (E2h-E7h, 46h/B9h, only Data Flash 2000h-2FFFh is
    // reachable - application-area commands are ignored, as EN-271 p. 204
    // says), 1 = IAP15-style (C2h-C7h, 5Ah/A5h, program memory itself is
    // writable). Call BEFORE sim_load_hex(): it fills code memory with FFh
    // so unprogrammed flash reads as erased flash does.
    SIM_API int sim_enable_iap(sim_handle_t aSim, int aProfile);
    // Host view of the flash IAP acts on (Data Flash / program memory);
    // -1 outside the writable range. poke is for test setup only.
    SIM_API int sim_iap_peek(sim_handle_t aSim, int aAddress);
    SIM_API int sim_iap_poke(sim_handle_t aSim, int aAddress, int aValue);
    // 0 reads, 1 programs, 2 erases, 3 ignored (out of range),
    // 4 software resets to AP, 5 entries into the ROM ISP monitor.
    SIM_API long sim_iap_stat(sim_handle_t aSim, int aWhich);
    // Raw code-memory byte (what MOVC / the CPU fetch sees).
    SIM_API int sim_peek_code(sim_handle_t aSim, int aAddress);

#ifdef __cplusplus
}
#endif

#endif // SIM_CAPI_H
