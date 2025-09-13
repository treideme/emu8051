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

## Python setup

The Python bindings/GUI/tests are kept out of the system interpreter --
use `uv` to get an isolated environment scoped to this directory:

```
cd python
uv venv .venv
uv pip install -r requirements.txt
.venv/Scripts/python sim_gui.py ../../demo-projects/build/344_clock_lcd.hex   # or: uv run --no-project python sim_gui.py ...
.venv/Scripts/python -m unittest discover -s tests -v
```

`.venv/` is gitignored; `pysim` itself has no dependencies (stdlib
`ctypes` only) so the unittest suite runs fine even without the venv, but
`sim_gui.py` needs PySide6 from it.

## Reset, real-time playback, and the DS1302's own clock

`sim_reset()`/`Simulator.reset()` resets the CPU (registers/PC/SFRs back to
power-on state) and anything that's purely a *reflection* of what the CPU
drove onto pins -- the digit display and LCD are destroyed and recreated
fresh so they don't keep showing a stale pre-reset capture, the same class
of bug `hc573_create()` had (above). It deliberately leaves DS1302,
XPT2046, and any `sim_set_pin()` overrides alone: a real DS1302 is
battery-backed and keeps running across an MCU reset, and an XPT2046
reading or a forced pin is host-injected test stimulus, not CPU-derived
state -- a reset button on the board wouldn't touch either. See
`capi.h`'s own comment on `sim_reset()` and
`python/tests/test_hc6800_es.py`'s `ResetTests` for what's covered.

`sim_step()`'s ticks map to wall-clock time at `sim_get_clock_hz(sim)/12`
ticks per second: `tick()` (`core.c`) advances one machine cycle per call
(12 oscillator periods on a classic 8051), a conversion cross-checked
against `hd44780.c`'s own busy-timing constants (its "2ms" comment only
holds if 1 tick = 1us at a 12MHz `clock_hz`). `sim_gui.py --realtime` (or
the "Real-time" checkbox) uses exactly that conversion to pace
`sim.step()` against measured wall-clock time instead of running a fixed
instruction count per GUI refresh, so what you see on screen advances at
the same rate a real board would. Without it, `sim_step_instructions()` is
effectively unthrottled -- fine for tests, but a DS1302 demo's seconds
display would otherwise blow past a full minute in well under a second of
wall-clock time.

`hc6800_es`'s stock clock is a hardcoded 12MHz (`HC6800_ES_XTAL_HZ`), but
`sim_set_clock_hz()`/`Simulator.set_clock_hz()`/`sim_gui.py --clock-hz HZ`
(or `sim_test`'s `clock_hz=N`) override it for a different real part --
still assumed 12-clocks-per-machine-cycle, just a different oscillator.
Call it right after opening, before any `sim_enable_*()`/`enable_*()`:
`hd44780`/`ds1302` both capture `clock_hz` at their own creation time, so
setting it later only affects a peripheral enabled afterwards.

That matters because DS1302 now keeps its own free-running clock:
`ds1302_step()` (called once per tick from `capi.c`'s `post_tick()`, same
convention as `hd44780_step()`) rolls the 8 BCD registers forward by
whatever the same `clock_hz/12` conversion says is one real second,
including minute/hour/date/month/year carry and leap years -- a real
DS1302 does this off its own 32.768kHz crystal, independent of the host
MCU's clock or whether anyone's watching, so this stays correct whether
you're fast-forwarding a test or watching it live in real-time mode.
`sim_gui.py`'s `--lcd`/`--digits`/`--ds1302`/`--adc` flags and
`--instr-per-tick`/`--interval-ms` pre-set what used to be GUI-only
options; see `sim_gui.py --help`.

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

## Fidelity notes and the real bugs found building this

- HD44780: adapted from `../logicboard.c`'s command/DDRAM/CGRAM state
  machine (rewired off that file's hardcoded P1/P3 wiring onto `pin_t`).
  Full command decode, busy-flag timing, 4-bit and 8-bit interface modes.
- DS1302: full 3-wire protocol, 8 clock/calendar registers, burst mode,
  and (see below) a free-running clock that advances in step with
  simulated time, same as the real chip's own crystal would.
- XPT2046: control-byte decode (channel, 8/12-bit mode) and a settable
  per-channel reading; no touch-pressure modeling.
- 74HC573/74HC138: generic, reusable single-chip primitives; composed by
  `digit_display.*` for the common "latch + decoder + 7-segment bank"
  topology this board (and most similar teaching boards) use.
- None of this models a peripheral chip's *electrical* behavior --
  there's no ADC reference voltage, no RTC crystal drift/inaccuracy
  (DS1302's clock advances at exactly `clock_hz/12` real seconds per
  second, not a real crystal's few-ppm wobble), no LCD contrast.
  It models the *protocol/register* behavior precisely enough that a
  firmware bug and a simulator bug are actually distinguishable, which is
  the property that mattered for finding these, all confirmed by tracing
  real compiled output from the demo projects against a
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
  - **`hc573_create()` in `sim/devices/hc573.c`** initialized its
    `transparent` (LE-open) flag to 0 unconditionally, on the assumption
    that any firmware driving this latch would explicitly write LE at
    least once. `clock_digit_tube` never does -- it relies on the
    8051's port power-on default (P1=0xFF) to leave LE permanently high
    -- so the flag stayed stuck closed forever, since the only other
    place it's set is the LE-write callback, which needs a write *event*
    that firmware never produces. Surfaced as one digit position per
    multiplex cycle latching a single spurious all-segments-on snapshot
    at boot and never updating again. Fixed by reading LE's actual level
    at device-creation time instead of assuming it; covered by
    `python/tests/test_hc6800_es.py`'s
    `test_digits_survive_boot_without_stale_latch`.
