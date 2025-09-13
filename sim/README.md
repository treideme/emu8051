# sim/ - peripheral simulation layer

This directory adds peripheral models on top of the emu8051 CPU core
(`../core.c`, `../opcodes.c`, `../disasm.c` -- untouched, board-agnostic
8051 emulation). It exists to answer one question concretely: *when a
board wires several 8051 demos to overlapping pins, can the simulator do
better than "the MCU wrote some bytes to some ports"?* For the HD44780
LCD and the 7-segment digit bank, yes -- you get back actual displayed
text and digit characters. For DS1302 and XPT2046, you get a real,
settable virtual chip on the other end of the wire, not silence.

## Layers

```
core.c / opcodes.c / disasm.c   <- CPU core (untouched, from jarikomppa/emu8051)
sim/pin.h                        <- names one bit of one SFR register
sim/bus.h, bus.c                 <- the "connector": multiplexes struct em8051's
                                     single sfrread[]/sfrwrite[] slot per register
                                     across any number of subscribers
sim/devices/*.c                  <- peripheral models, written only in terms of
                                     pin_t values handed to them -- never a
                                     hardcoded port letter
sim/boards/hc6800_es.c           <- THIS project's pin_t/xxx_pins_t catalog for
                                     the one dev-kit this repo targets
sim/capi.h, capi.c               <- flat C API (opaque handle, plain functions)
                                     that ties board+devices+CPU together for
                                     any external caller
python/pysim/                    <- ctypes wrapper over capi.h
python/sim_gui.py                <- PySide6 live view (polls sim_step(), no
                                     callback/threading machinery)
python/tests/                    <- unittest suite, run against real compiled
                                     .hex files from demo-projects
```

## Why a bus/connector layer at all

`struct em8051` (emu8051.h) has exactly one write callback slot and one
read callback slot per SFR register. Real boards routinely put more than
one logical peripheral on the same port -- this project's own board wires
DS1302 and XPT2046 to overlapping bits of P3, in different demos, and a
74HC573 segment latch shares its enable line's register with nothing else
but could. A device model that just claimed `cpu->sfrwrite[REG_P3]` for
itself would silently break the next device that also needs P3.

The bus is the one thing that actually occupies those slots. Devices
subscribe to it (`bus_on_write`/`bus_on_read`) instead, and it fans events
out to every subscriber -- see `bus.h`'s own comment for the write/read
composition rules (writes are pure notifications; reads compose
mask/value pairs so two devices can each own different bits of the same
byte).

## Adding a peripheral

1. Write `sim/devices/foo.h`/`foo.c`: a `foo_pins_t` struct of `pin_t`
   fields (never a raw port/bit number in the .c file itself), a
   `foo_create(bus, cpu, pins)`/`foo_destroy()` pair, and whatever
   query/stimulus functions make sense (see `ds1302.h`'s
   `ds1302_set_time()`/`ds1302_get_register()` for the shape of this).
2. Subscribe to whichever registers your pins land in via `bus_on_write`/
   `bus_on_read` inside `foo_create()`. Don't assume you own the whole
   register.
3. Expose it through `sim/capi.h`/`capi.c` (`sim_enable_foo()` +
   query/stimulus functions) if you want it reachable from Python/the GUI.

## Adding a board

Add `sim/boards/some_other_kit.h`/`.c` with its own `pin_t`/`xxx_pins_t`
constants (see `hc6800_es.c` -- it's a flat catalog, not a "create
everything" function, since real boards don't populate every peripheral
at once). Every existing device model works against it unchanged. Wiring
a new board into `capi.c`'s `sim_open()` board-name dispatch is the one
place that currently assumes `hc6800_es` is the only option (flagged
explicitly in that function).

## Fidelity notes and the two real bugs found building this

- HD44780: adapted from `../logicboard.c`'s command/DDRAM/CGRAM state
  machine (rewired off that file's hardcoded P1/P3 wiring onto `pin_t`).
  Full command decode, busy-flag timing, 4-bit and 8-bit interface modes.
- DS1302: full 3-wire protocol, 8 clock/calendar registers, burst mode.
- XPT2046: control-byte decode (channel, 8/12-bit mode) and a settable
  per-channel reading; no touch-pressure modeling.
- 74HC573/74HC138: generic, reusable single-chip primitives; composed by
  `digit_display.*` for the common "latch + decoder + 7-segment bank"
  topology this board (and most similar teaching boards) use.
- None of this models a peripheral chip's *electrical* behavior --
  there's no ADC reference voltage, no RTC crystal drift, no LCD contrast.
  It models the *protocol/register* behavior precisely enough that a
  firmware bug and a simulator bug are actually distinguishable, which is
  the property that mattered for finding these two, both confirmed by
  tracing real compiled output from the demo projects against a
  known-correct protocol/ISA reference rather than assumed:
  - **`mov_mem_indir_rx` in `../opcodes.c`** (opcode 0x86/0x87, `MOV
    direct,@Ri`) had source and destination completely swapped -- a
    regression from the recent upstream sync. Affects any program that
    copies data through this addressing mode (e.g. `dest = array[i]`
    compiled through an index register), not just this repo's demos.
  - A timing bug in `sim/devices/ds1302.c`'s own read-phase state machine:
    the phase transition into read-mode happens on the *rising* edge of
    the command byte's 8th bit, but the very next event is the *falling*
    edge completing that same clock pulse -- and the code was treating
    that leftover falling edge as the first data bit's advance, silently
    skipping bit 0 of every read.

## Known limitation

`clock_digit_tube` (one of the demo projects' three DS1302 demos)
still shows a stale/incorrect digit-display pattern despite the DS1302
protocol itself being independently confirmed correct (bit-level trace
verified; the other two DS1302 demos, `3431_clock_digit_tube_1` and
`344_clock_lcd`, both display correctly). Not yet root-caused -- flagging
rather than silently leaving unexplained.
