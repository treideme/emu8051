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
                                     hardcoded port letter. Board peripherals
                                     (hd44780, hc573, hc138, digit_display,
                                     ds1302, xpt2046) AND external/plugin
                                     peripherals (servo, enc28j60) both live
                                     here -- see "Board peripherals vs
                                     external peripheral plugins" below for
                                     what actually distinguishes them
sim/boards/hc6800_es.c           <- THIS project's pin_t/xxx_pins_t catalog for
                                     the one dev-kit this repo targets --
                                     real board peripherals only
sim/boards/hc6800_es_plugin.c    <- the HC6800-ES board as a plugin: creates
                                     the device models on the catalog's pins
                                     when asked, steps them, and answers
                                     methods. Linked into the library, and
                                     also built on its own as
                                     build/hc6800_es.so (see PLUGIN-TUTORIAL.md)
sim/capi.h, capi.c               <- flat C API (opaque handle, plain functions)
                                     for any external caller; its sim_enable_*
                                     and device functions call the board plugin
sim/plugin.h, plugin.c           <- the host side of plugins: shared objects
                                     opened at run time, or code linked into
                                     the program, that add models, a board, or
                                     a whole part (see "Loadable plugins" below)
python/pysim/                    <- ctypes wrapper over capi.h
python/sim_gui.py                <- PySide6 live view (polls sim_step(), no
                                     callback/threading machinery)
python/tests/                    <- unittest suite: test_hc6800_es.py (board
                                     peripherals) and test_external_peripherals.py
                                     (servo/enc28j60), both run against real
                                     compiled .hex files from a sibling demo repo;
                                     test_plugin.py, against tests/plugin/;
                                     test_hc6800_plugin.py, the board linked in
                                     against the board loaded as a shared object
tests/plugin/demo_plugin.c       <- a small plugin that uses every part of
                                     plugin.h once, for test_plugin.py
tests/test_hc6800_plugin.c       <- the same comparison in C: the board from
                                     sim_open(), from sim_add_plugin() and from
                                     sim_load_plugin(); also built with no
                                     dynamic loader (SIM_NO_DLOPEN)
```

## Python setup

The Python bindings/GUI/tests are kept out of the system interpreter --
use `uv` to get an isolated environment scoped to this directory:

```
cd python
uv venv .venv
uv pip install -r requirements.txt
.venv/Scripts/python sim_gui.py path/to/some/344_clock_lcd.hex   # or: uv run --no-project python sim_gui.py ...
.venv/Scripts/python -m unittest discover -s tests -v
```

`.venv/` is gitignored; `pysim` itself has no dependencies (stdlib
`ctypes` only) so the unittest suite runs fine even without the venv, but
`sim_gui.py` needs PySide6 from it.

## Reset, real-time playback, and the DS1302's own clock

`sim_reset()`/`Simulator.reset()` resets the CPU (registers/PC/SFRs back to
power-on state) and anything that's purely a *reflection* of what the CPU
drove onto pins: the digit display and the LCD go back to their state when
enabled, so they don't keep showing a stale pre-reset capture, the same
class of bug `hc573_create()` had (below). The board plugin does this in
place, with `digit_display_reset()` and `hd44780_reset()`. `sim_reset()`
used to destroy and recreate both, which left the old ones' bus
subscriptions behind, pointing at freed memory, until the seventh reset
tripped the bus's subscriber limit. A reset deliberately leaves DS1302,
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

`sim_open()` starts every simulation at 12MHz, the HC6800-ES crystal, but
`sim_set_clock_hz()`/`Simulator.set_clock_hz()`/`sim_gui.py --clock-hz HZ`
(or `sim_test`'s `clock_hz=N`) override it for a different real part --
still assumed 12-clocks-per-machine-cycle, just a different oscillator.
Call it right after opening, before any `sim_enable_*()`/`enable_*()`:
`hd44780`/`ds1302` both capture `clock_hz` at their own creation time, so
setting it later only affects a peripheral enabled afterwards.

That matters because DS1302 now keeps its own free-running clock:
`ds1302_step()` (called once per tick from the board plugin's tick hook,
same convention as `hd44780_step()`) rolls the 8 BCD registers forward by
whatever the same `clock_hz/12` conversion says is one real second,
including minute/hour/date/month/year carry and leap years -- a real
DS1302 does this off its own 32.768kHz crystal, independent of the host
MCU's clock or whether anyone's watching, so this stays correct whether
you're fast-forwarding a test or watching it live in real-time mode.
`sim_gui.py`'s `--lcd`/`--digits`/`--ds1302`/`--adc`/`--servo`/
`--enc28j60` flags and `--instr-per-tick`/`--interval-ms` pre-set what
used to be GUI-only options; see `sim_gui.py --help`. The servo panel
shows a rotating arrow over the current 0-180 degree angle plus the raw
pulse width; the ENC28J60 panel shows the last decoded SPI opcode, the
currently-selected register bank, a running buffer-byte count, and an
activity dot that flashes when that count changes -- both panels gray
out (same mechanism as the digit/LCD panels) until their checkbox is
checked, and both need an explicit pin (there's no board default to fall
back to -- see "Board peripherals vs external peripheral plugins" below).

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
   If a CPU reset should put it back, add a `foo_reset()` that does so in
   place: a model is never created twice, because its bus subscriptions
   cannot be removed. If it counts time, add a `foo_step()` to call once
   per tick.
2. Subscribe to whichever registers your pins land in via `bus_on_write`/
   `bus_on_read` inside `foo_create()`. Don't assume you own the whole
   register.
3. Give the board plugin (`sim/boards/hc6800_es_plugin.c`) an `enable_foo`
   method that creates it, with a tick hook if it has a `foo_step()`, and
   methods for its queries and stimulus. Then add `sim_enable_foo()` and
   the rest to `sim/capi.h`/`capi.c` as calls of those methods if you want
   it reachable from Python/the GUI. If it's a real board peripheral,
   `enable_foo` takes no pin arguments and reaches into `sim/boards/*`'s
   catalog, like `enable_lcd` does. If it's an external part with no board
   wiring to reach for, it takes the pin(s) as explicit `(port, bit)`
   arguments, like `enable_servo`/`enable_enc28j60` do. See "Board
   peripherals vs external peripheral plugins" below for which one applies
   and why.

A part that only one project needs can also go into a plugin of its own,
outside this repository (see "Loadable plugins" below).

## Adding a board

A board is a plugin. It has two parts: a pin catalog,
`sim/boards/some_other_kit.h`/`.c`, with its own `pin_t`/`xxx_pins_t`
constants, and a plugin that creates device models on those pins when a
method asks for one, steps them and answers queries. The catalog is flat
data like `hc6800_es.c`, with no "create everything" function, since real
boards don't populate every peripheral at once. Every existing device model
works against it unchanged.

[`PLUGIN-TUTORIAL.md`](PLUGIN-TUTORIAL.md) writes one step by step, with
`hc6800_es_plugin.c` as the worked example. For `sim_open("some_other_kit")`
to give the new board, add a line to the `BOARDS` table in `capi.c`. A board
kept in another repository goes onto `sim_open("bare")`, loaded with
`sim_load_plugin()` or linked in and added with `sim_add_plugin()`. The
`sim_enable_*` functions in `capi.h` are the HC6800-ES board's methods;
another board's methods are reached with `sim_plugin_call()`, or pysim's
`Plugin.call()`.

## Board peripherals vs external peripheral plugins

Two different things live side by side under `sim/devices/`, and the
difference is purely about *where a pin assignment comes from*, not the
device model's own code:

- **Board peripherals** (HD44780, 74HC573/74HC138, DS1302, XPT2046) are
  wired to specific pins on a *real* HC6800-ES board. Their pins live as
  named constants in `sim/boards/hc6800_es.c`, and
  `sim_enable_lcd()`/`sim_enable_ds1302()`/etc (the board plugin's
  `enable_lcd`/`enable_ds1302` methods) take no pin arguments at all. They
  reach into that one board's catalog, because on this board there's only
  one legitimate answer to "which pins is the LCD on".
- **External peripheral plugins** (the servo, the ENC28J60, the fan) are
  *not* on the real board at all -- there's no servo header, no Ethernet
  module, and no fan header on an HC6800-ES. A project that wants one has
  to invent a wiring assumption for it (see the sibling demo repo's
  `05_enc_servo/enc.c`, `09_ethernet/enc28j60_cfg.h`, and
  `06_fan_tach/main.c` for what each one actually picked, and why).
  Because that pin choice is a per-project invention rather than a board
  fact, it does not belong in `sim/boards/hc6800_es.c` -- baking a
  made-up pin into the board catalog would make it look like a real
  HC6800-ES fact to the next reader. Instead,
  `sim_enable_servo()`/`sim_enable_enc28j60()`/`sim_enable_fan()` (and the
  plugin methods behind them) take the pin(s) as explicit `(port, bit)`
  arguments (`sim_gui.py
  --servo`/`--enc28j60`/`--fan` mirror this), and the *caller* -- a
  project's own test, or whoever points the GUI at that project's `.hex`
  -- is the one who knows what pin that particular project assumed.

The device models themselves don't know or care which category they're
in -- both kinds are written purely in terms of `pin_t` (see "Adding a
peripheral" below), which is exactly what makes an external plugin
possible to reuse across unrelated projects that each pick their own pin
for it, the same way a board peripheral is reusable across boards that
each wire it to their own pin.

## Loadable plugins (`plugin.h`)

A *plugin* adds models, a board or a whole part to a simulation, and it
reaches the simulation through a table of host functions and nothing else.
The HC6800-ES board is one: `sim/boards/hc6800_es_plugin.c` is linked into
this library, where `sim_open("hc6800_es")` adds it, and the same source
is built on its own as `build/hc6800_es.so`.

A model that does not belong here (another vendor's part, with its own
core timing, interrupt controller and peripheral set, or a peripheral only
one project needs) can live in its own repository as a plugin: a shared
object that this library opens by path at run time, or sources linked into
a program and added with `sim_add_plugin()`. The plugin builds against
these headers (no link against this library; everything goes through the
table), and this repository does not change for it.
[`PLUGIN-TUTORIAL.md`](PLUGIN-TUTORIAL.md) writes one step by step, with
the HC6800-ES board as the worked example.

```
sim_handle_t s = sim_open("bare");            // or "hc6800_es"
sim_load_plugin(s, "path/to/plugin.so", "args the plugin understands");
sim_add_plugin(s, its_init_function, "args"); // the same, linked in
sim_plugin_call(s, "name", "method", in, nin, out, cap, &nout);
```

or from Python:

```python
sim = Simulator("bare")
p = sim.load_plugin("path/to/plugin.so", "args")
p.call("method", b"input bytes")          # -> output bytes
f = p.function("exported_fn", ctypes.c_int, [ctypes.c_double])
f(1.5)                                     # the plugin's context goes first
```

`"bare"` is a plain 8051 with no board: 64 KB of code and xdata, 256
bytes of idata, nothing attached, for a plugin to build a part on.

**The ABI.** The plugin exports one function, `emu8051_plugin_init(host,
plugin, args)`. `host` is a versioned table (`abi`, `size`, then function
pointers, each taking `host->sim` first); the plugin fills in `plugin`
(its name, a context pointer, and `destroy`, `reset` and `call`). The
table is the whole interface, together with `struct em8051` from
`emu8051.h`, which `host->cpu()` hands out for direct access to
registers, memory and the PC. `EMU8051_PLUGIN_ABI` changes when an
existing member, or the layout of `struct em8051`, changes; new
functions are only appended, so `EMU8051_HOST_HAS(host, member)` lets a
plugin built against a newer header check an older host. A plugin
refuses a host of another ABI, and the host refuses a plugin that reports
another one after init.

What the table offers, by group (`plugin.h` documents each):

- CPU and memory: the CPU, memory sizes (`set_memory`: code and xdata up
  to 64 KB, upper idata or not), loading HEX, the core's `reset()`, the
  oscillator.
- Exceptions: raise one, and filter (drop or translate) the core's own.
- SFRs: read and write subscription through the bus, exactly as the
  in-tree devices do it.
- xdata: hooks for address ranges, ahead of the xdata array.
- Execution: a per-opcode cycle table with a column for conditional jumps
  that jump; replacing an opcode's handler; step hooks (`begin` before
  each step, which may consume it without an instruction, `before` and
  `after` each instruction, `end` after every step); an interrupt hook
  that answers with any vector, so a plugin can have vectors past 2Bh and
  priority rules of its own; entering a vector from a hook; one step of
  the host's loop; clocks and instructions so far.
- Pins: the level the CPU reads, driving a pin from outside (as
  `sim_set_pin()` does, but on any SFR, not only P0-P3), and being told
  when a pin's level changes.
- Plugins: calling a method of another plugin on the same simulation;
  reporting why init failed.
- Ticks (added after ABI 1 was fixed, so check with `EMU8051_HOST_HAS`):
  a function called once per tick of 12 clocks in both modes, for models
  that count time in machine cycles. The board plugin steps its LCD,
  DS1302, servo, fan and IAP models from these.

**Two execution modes.** Without plugins, or with plugins that only use
SFRs, xdata, pins, tick hooks and the `end` hook, the host runs `tick()`
as before: 12 clocks per machine cycle, the core's timers, UART and
two-level interrupt controller. A plugin that sets a cycle table, a `begin`,
`before` or `after` hook, or an interrupt hook switches the simulation
to *instruction mode*: each step is a wait (a `begin` hook consumed it),
an interrupt entry or one instruction, timed by the cycle table (12
clocks per machine cycle without one). The core's timers, UART,
interrupt controller and PCON idle and power-down do not run in that
mode; they belong to `tick()`, and a part described by a plugin has its
own. `sim_step(ticks)` still counts 12 clocks per tick, and tick hooks
(the board's LCD, DS1302, servo, fan and IAP models among them) run once
per 12 clocks executed, however the instructions are timed, so those
models keep their timing. A tick hook leaves the host in its plain
`tick()` loop; any step hook moves it to a slower one. RETI is the core's
and only pops the PC: a plugin that tracks priority levels
in service watches for opcode 32h in its `after` hook. A conditional
jump to the very next instruction lands in the same place whether it
jumps or not; for the taken column the host runs such a jump with its
offset patched to 1 and puts the byte back.

**Methods.** `sim_plugin_call()` passes bytes in and bytes out, with a
method name, so a binding needs nothing per plugin; each plugin documents
its own methods. The board's are listed in `sim/boards/hc6800_es_plugin.h`,
and `capi.h`'s device functions are calls of them. A binding that wants
typed functions can open the same
object (`sim_plugin_path()`, already loaded) and call what the plugin
exports, passing `sim_plugin_context()` first; pysim's
`Plugin.function()` does that.

**Failure.** A load can fail at `dlopen`, for a missing entry point, an
ABI mismatch or a refusing init, and `sim_plugin_error()` says which in
words. A plugin may have hooked in before it gave up, so its object stays
loaded (and inert) until `sim_close()`; treat the simulation as spoilt
and close it. A second plugin of a name already loaded is called
`name#2`. Limits: 8 plugins per simulation, 16 hooks of each kind, 16
xdata ranges, 64 watched pins, 64 driven pins.

**Building one.** Compile with the repository root and `sim/` on the
include path, as a shared module (`-fPIC -shared`), and export only the
entry point and whatever a binding calls (`EMU8051_PLUGIN_EXPORT`, with
`-fvisibility=hidden`). `-Wl,-z,defs` makes the link fail if the plugin
calls anything that is neither in it nor in the C library.
`tests/plugin/demo_plugin.c` is a complete one in about 450 lines; meson
builds it as `build/demo_plugin.so` and `python/tests/test_plugin.py`
tests the host through it. The HC6800-ES board is another, built as
`build/hc6800_es.so` by meson and `make sim`, and as
`build/hc6800_es.dll` by `build_sim_windows.sh`.

**Linking one in.** A plugin can also be compiled into a program and added
with `sim_add_plugin(sim, init, args)`, given its init function; after
that it is the same as a loaded one, except that `sim_plugin_path()` is
NULL. Such a plugin gives its init a name of its own and keeps the
exported `emu8051_plugin_init` in a file that only the shared object is
built from, so that two of them can be linked into one program
(`sim/boards/hc6800_es_entry.c`). A program with no dynamic loader at all
compiles `plugin.c` with `SIM_NO_DLOPEN` defined: `sim_load_plugin()` then
fails and says why, and nothing refers to `dlopen` or `LoadLibrary`.

**What changed here for loadable plugins, and why.** Only generic pieces:

- `bus.c` takes 16 subscribers per SFR instead of 4. A part built as a
  plugin puts many models on the registers they share (the ports, the
  interrupt enable and priority registers, pin-switch registers), and 4
  tripped the assertion in `bus_on_write`.
- `bus.c`: the registry that maps a CPU back to its bus grows as buses
  are created. It had eight slots, and a ninth simulation open at once
  asserted in `bus_create()`; a test suite that opens one simulator per
  test and leaves some to the garbage collector reaches nine quickly.
- `sim_set_pin()`'s forced pins are kept per SFR, where they were kept per
  P0-P3 index, so that a plugin can drive pins of ports at other addresses;
  `sim_set_pin()` itself is unchanged, and there are 64 of them (16
  before).
- Interrupt numbers are not limited by the host: the interrupt hook
  returns a 16-bit vector, so an 8n+3 table up to interrupt 127 (3FBh) or
  any other layout is the plugin's business.
- `struct sim` moved from `capi.c` to `sim_private.h`, shared with
  `plugin.c`; it is not part of any ABI.

**What changed for the board plugin, and why.** For the HC6800-ES board to
use only the table an outside plugin gets, this needed:

- `on_tick`, appended to the host table (the ABI stays 1). The board's
  models count time in ticks of 12 clocks, and `capi.c` used to step them
  itself in `sim_post_tick()`, which now runs the tick hooks.
- `sim_add_plugin()` and `SIM_NO_DLOPEN`, so that one plugin source can be
  linked in or built as a shared object.
- `bus_create_forwarding()`: a bus whose subscriptions go to the host's
  `on_sfr_write`/`on_sfr_read`, so the models in `sim/devices/` work in a
  plugin with no change to their own interfaces.
- Reset functions for the three models that reflect CPU pins
  (`hd44780_reset()`, `hc573_reset()`, `digit_display_reset()`), so a
  reset no longer recreates them and leaves their old subscriptions
  behind.
- `iap_set_reset()`: the IAP model no longer calls the core's `reset()`
  itself; whoever creates it says how to reset the CPU (the board passes
  the host's `reset_cpu`).
- `capi.c` keeps no device: `struct sim` lost its device pointers, and
  every `sim_enable_*` and device function is a call of the board's method
  of the same name, through `sim_plugin_call()`. On a `"bare"` simulation
  the first such call adds the linked-in board, as before; a loaded
  `hc6800_es.so` answers them in its place.
- pysim's `Plugin.path` is `None` for a linked-in plugin, and
  `Plugin.lib` is the simulator library itself.

The core (`core.c`, `opcodes.c`, `disasm.c`) and the board catalog are
unchanged, and the device models changed only as listed above. A plugin
needing a variant of a model (an HD44780 controller on other wiring,
another IAP register layout) carries its own copy.

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
- Servo (external plugin): measures a PWM input pin's high-time and maps
  it to a 0-180 degree angle over a settable pulse-width range (1000-
  2000us for a standard hobby servo). No mechanical model -- it reports
  the angle a real servo would be *commanded* to, not simulated inertia
  or torque.
- Fan (external plugin): a 4-wire PC-style PWM fan model, in two
  independent halves. PWM decode measures the last full cycle on the
  watched pin and reports a 0-100% duty cycle -- it doesn't care what
  frequency the firmware actually drives (a bit-banged 8051 won't hit a
  real fan's ~25kHz PWM spec, and doesn't need to for this to work).
  Tachometer generation drives a pulse train back at
  `FAN_PULSES_PER_REV`(2, the near-universal convention for a 2-pole
  4-wire fan's open-collector tach -- the Waveshare Fan-4020-PWM-5V this
  was grounded against doesn't specify it) pulses per simulated
  revolution, at an RPM that responds to the *measured* duty cycle via a
  simple linear mapping between `FAN_MIN_START_DUTY_PERCENT`(20, below
  which the model assumes the fan doesn't spin, matching real fans'
  typical minimum-start-duty behavior) and `FAN_MAX_RPM`(8000, an assumed
  ceiling for a small 40mm 5V fan). No spin-up/spin-down inertia, no real
  fan curve, RPM tracks a duty-cycle change instantly -- see
  `sim/devices/fan.h`'s own header comment for the full reasoning.
- ENC28J60 (external plugin): a register/bank/buffer *protocol* model --
  the bit-banged SPI mode-0 shift register, the 4-bank x 32-slot control-
  register file (with the five all-bank registers shared across banks,
  matching the real chip), READ_BUF_MEM/WRITE_BUF_MEM against an 8KB
  buffer with ERDPT/EWRPT auto-increment, SOFT_RESET, and the RCR
  dummy-byte quirk for MAC/MII registers. Since branch `enc28j60-net` it
  also has a **packet layer**: `sim_enc28j60_inject_rx()` lays a frame into
  the circular RX buffer exactly as the chip does (6-byte header, FCS,
  even alignment, EPKTCNT/PKTIF/PKTDEC, read wrap at ERXND, the ERXFCON
  unicast/broadcast/multicast filter against MAADR), and setting
  ECON1.TXRTS captures ETXST+1..ETXND as a transmitted frame
  (`sim_enc28j60_tx_frame()`). Still **not** modelled: the PHY/link, DMA and
  checksum engine, TX status vectors, pattern-match/hash filters, and any
  electrical timing -- including the silicon errata on slow SPI clocks, so a
  passing bit-banged driver here proves protocol, not that the real part
  accepts it. Used by `python/tests/test_enc28j60_net.py`.
- None of this models a peripheral chip's *electrical* behavior --
  there's no ADC reference voltage, no RTC crystal drift/inaccuracy
  (DS1302's clock advances at exactly `clock_hz/12` real seconds per
  second, not a real crystal's few-ppm wobble), no LCD contrast.
  It models the *protocol/register* behavior precisely enough that a
  firmware bug and a simulator bug are actually distinguishable, which is
  the property that mattered for finding these, all confirmed by tracing
  real compiled output from the sibling demo repo's projects against a
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
    least once. a clock demo of that shape never does -- it relies on the
    8051's port power-on default (P1=0xFF) to leave LE permanently high
    -- so the flag stayed stuck closed forever, since the only other
    place it's set is the LE-write callback, which needs a write *event*
    that firmware never produces. Surfaced as one digit position per
    multiplex cycle latching a single spurious all-segments-on snapshot
    at boot and never updating again. Fixed by reading LE's actual level
    at device-creation time instead of assuming it; covered by
    `python/tests/test_hc6800_es.py`'s
    `test_digits_survive_boot_without_stale_latch`.
  - **`sim/devices/enc28j60.c`'s READ_BUF_MEM handling** over-counted by
    one byte on every read: the output byte for a READ_BUF_MEM data byte
    has to be staged *before* it starts shifting out (so it's ready in
    time), but the natural place to prepare the *next* byte's output --
    immediately after the current one finishes -- also advanced the
    buffer pointer for a byte that may never actually get clocked out if
    CS deasserts right after. A real 4-byte read+write round trip
    (the sibling demo repo's `09_ethernet_diag.hex`) read
    `buffer_byte_count()` as 9, not the correct 8. Fixed by splitting the
    read side into a peek (compute the next output byte, no side effect)
    and a commit (advance the pointer, only once a byte has actually
    finished shifting out) -- see `enc28j60.c`'s own comment on
    `peek_read_byte()`/`commit_read_byte()`; covered by
    `python/tests/test_external_peripherals.py`'s
    `test_buffer_round_trip_advances_pointers_by_exactly_one_per_byte`.

## Known limitation: UART only ever fires off Timer1

`core.c`'s `timer_tick()` only calls `serial_tx()` when Timer1 overflows
(`TCONMASK_TF1`) with `SCON.SM1` set -- there's no equivalent path for
Timer2 as the baud-rate source, even though `#ifdef __8052__` blocks exist
elsewhere in `core.c` for other Timer2 behavior (all still `// TODO`, and
`__8052__` isn't defined anywhere in this project's build regardless).
Real STC89C52/8052 parts can source UART baud from either timer.
the sibling demo repo's `09_ethernet/ethernet.c` configures Timer2 (real
hardware behavior, ported as-is) and never touches Timer1, so its
`PUTS()` calls block forever in this simulator waiting for `TI` -- not a
bug in that driver, a gap in this core's Timer2 model. That's why
`09_ethernet_diag.hex` (a UART-free variant of the same real
`enc28j60.c` driver) is what the tests and `sim_gui.py`'s ENC28J60 panel
actually exercise; see `09_ethernet/diag_no_uart.c`'s own comment.

## STC ISP/IAP flash programming (`devices/iap.c`)

`sim_enable_iap(sim, profile)` models the six SFRs STC parts use to read,
byte-program and sector-erase their own flash, plus `SWRST`/`SWBS` software
reset. Call it **before** `sim_load_hex()`: it fills code memory with `FFh`
so unprogrammed flash reads as erased flash does (the core's array is
`calloc`-zeroed).

| Profile | SFRs | Trigger | Writable from user code |
| --- | --- | --- | --- |
| `0` STC89C52RC | `E2h–E7h` | `46h`, `B9h` | Data Flash `2000h–2FFFh` only (a separate array); application-area commands are counted as *ignored* (EN-271 p. 204) |
| `1` IAP15 | `C2h–C7h` | `5Ah`, `A5h` | program memory itself (STC15 datasheet p. 487) |

Program is a bitwise AND, erase sets a 512-byte sector to `FFh`. A `SWRST`
write is performed after the current instruction completes (resetting inside
the SFR-write callback would let that instruction's own PC update run after
the reset), and keeps `ISPEN`/`SWBS`. `SWBS=1` is counted as an entry into
STC's ROM ISP monitor, which is not modelled; like the real monitor with no
host, it falls back to the application. Not modelled: erase/program time,
endurance, brown-out during a command. `sim_iap_peek/poke/stat` and
`sim_peek_code` give tests the host view. See
`python/tests/test_iap_bootloader.py`.

`sim_reset()` now re-anchors the UART TX ring: before, the core's ring
position survived a reset while the counters didn't, so the first byte read
back after any reset was a stale pre-reset byte.
