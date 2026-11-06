# Writing a board plugin

A *board plugin* puts device models on the pins a board wires them to, steps
them, and lets a caller reach them by name. This tutorial goes through one,
with the HC6800-ES development board as the worked example:
`sim_open("hc6800_es")` is a plain 8051 with that plugin added, and every
`sim_enable_*` function in `capi.h` is a call into it.

The plugin uses nothing of the host but the function table in `plugin.h`,
the same table a plugin built in another repository gets. That is what lets
one source file be used two ways:

- linked into a program and added with `sim_add_plugin()`. `libem8051sim`
  does this, and so can a program built with no dynamic loader at all;
- built as a shared object (`build/hc6800_es.so`, or `hc6800_es.dll` on
  Windows) and opened with `sim_load_plugin()`.

Both give the same results, and the tests in section 11 compare them step
by step.

Read `plugin.h` alongside this, since it documents every member of the
table. The plugin itself is `sim/boards/hc6800_es_plugin.c`, about 560
lines, most of them small method functions.

## 1. The files

```
sim/boards/hc6800_es.h, .c       the pin catalog: which pins each part is on
sim/boards/hc6800_es_plugin.h    the init function, the plugin's name, its methods
sim/boards/hc6800_es_plugin.c    the plugin
sim/boards/hc6800_es_entry.c     the exported entry point, for the shared object only
sim/bus.c, sim/devices/*.c       the bus and the device models, compiled into both
```

The catalog is data: a `pin_t` (an SFR register and a bit) or a `*_pins_t`
struct for each part on the board, and nothing that creates anything. The
plugin decides when to create a model and hands it the catalog's pins.

A board of your own needs the same three pieces. It can live in a
repository of its own, compiled against this one's headers (the repository
root and `sim/` on the include path) and against the device model sources
it uses.

## 2. The entry point

A shared object exports one function, named by `EMU8051_PLUGIN_ENTRY`:

```c
EMU8051_PLUGIN_EXPORT int emu8051_plugin_init(const emu8051_host_t *aHost,
                                              emu8051_plugin_t *aPlugin, const char *aArgs);
```

The host fills in `aPlugin->abi` and `aPlugin->size`. Init fills in the
rest and returns 0, and any other value refuses the load.

A plugin that is also linked into programs needs its init under a name of
its own, because two plugins linked into one program cannot both define
`emu8051_plugin_init`. So the board's init is `hc6800_es_plugin_init()`,
declared in `hc6800_es_plugin.h`, and the exported entry point is in a file
of its own that only the shared object is built from:

```c
// sim/boards/hc6800_es_entry.c
#include "hc6800_es_plugin.h"

EMU8051_PLUGIN_EXPORT int emu8051_plugin_init(const emu8051_host_t *aHost, emu8051_plugin_t *aPlugin,
                                              const char *aArgs)
{
    return hc6800_es_plugin_init(aHost, aPlugin, aArgs);
}
```

Init checks three things before it allocates anything:

```c
if (aHost->abi != EMU8051_PLUGIN_ABI)
    return -1;
if (!EMU8051_HOST_HAS(aHost, on_tick))
{
    aHost->error(aHost->sim, "hc6800_es: the host has no on_tick");
    return -1;
}
if (aArgs[0])
{
    aHost->error(aHost->sim, "hc6800_es: takes no arguments");
    return -1;
}
```

- **The ABI.** A host of another `EMU8051_PLUGIN_ABI` has another table
  layout, so not even `error()` can be called safely. The plugin returns -1,
  and the load fails with `SIM_PLUGIN_INIT_FAILED`.
- **Members added later.** `on_tick` was appended to the table after ABI 1
  was fixed. `EMU8051_HOST_HAS()` compares the host's `size` with the
  member's offset, so a plugin built against this header refuses an older
  host with a message, and never calls past the end of its table.
- **The arguments.** `aArgs` is the string the caller passed, never NULL
  and `""` when there was none. The board takes no arguments and says so.
  A plugin that does take some parses them here.

`aHost->error()` sets what `sim_plugin_error()` returns and what pysim's
`PluginError` carries. Then init allocates its context and fills in the
plugin:

```c
aPlugin->name = HC6800_ES_PLUGIN_NAME; // "hc6800_es"
aPlugin->ctx = b;                      // passed to the three functions below
aPlugin->destroy = board_destroy;      // sim_close()
aPlugin->reset = board_reset;          // sim_reset(), after the CPU
aPlugin->call = board_call;            // sim_plugin_call()
```

The name matters more than it looks (section 8). Init keeps `aHost`, which
stays valid until `destroy` has returned.

## 3. The host table

Every host function takes `host->sim` first. The board keeps both in its
context, together with the CPU:

```c
typedef struct
{
    const emu8051_host_t *host;
    void *sim;          // host->sim
    struct em8051 *cpu; // host->cpu(): the device models read pins from it
    sim_bus_t *bus;     // the forwarding bus (section 4)
    unsigned ticks;     // the tick hooks installed (section 6)
    hd44780_t *lcd;     // one pointer per model, NULL until enabled
    /* ... */
} board_t;
```

Of the whole table the board calls seven functions:

| member | what for |
| --- | --- |
| `cpu` | the `struct em8051` the device models read port latches from |
| `clock_hz` | the oscillator, which the LCD, DS1302, servo and fan time against |
| `on_sfr_write`, `on_sfr_read` | SFR subscriptions, through the forwarding bus |
| `on_tick` | stepping the models that count time |
| `reset_cpu` | the IAP model's software reset (`ISP_CONTR.SWRST`) |
| `error` | saying why init refused |

It never sees `struct sim`, includes nothing of `capi.h` and calls no
function of `libem8051sim`. That is the property to keep, and the shared
object's link (section 9) fails if it is broken.

The board uses none of the execution members (`set_cycles`, `on_step`,
`on_interrupt`), so it leaves the simulation in tick mode. It runs just as
well on a simulation that another plugin has switched to instruction mode
(section 6).

## 4. SFR subscriptions: the forwarding bus

`struct em8051` has one write callback and one read callback per SFR. In
the host a bus (`bus.c`) occupies those slots and fans each access out to
any number of subscribers. Writes are notifications to every subscriber.
Reads compose: each subscriber sets the bits it owns in a mask, and their
levels in a value (`bus.h`). The device models subscribe with
`bus_on_write()` and `bus_on_read()` on a `sim_bus_t` they are given when
they are created.

A plugin cannot reach the host's bus, which is part of `struct sim`. It has
`host->on_sfr_write` and `host->on_sfr_read`, which subscribe on that bus.
`bus_create_forwarding()` joins the two:

```c
b->bus = bus_create_forwarding(aHost->sim, aHost->on_sfr_write, aHost->on_sfr_read);
```

The result is a bus with no CPU and no subscribers of its own. Every
`bus_on_write()` or `bus_on_read()` on it goes straight to the host's
function, so a model's callbacks land on the host's bus, in order, as if the
model had subscribed there itself, and the host calls them with its CPU. The
device models need no change for this. `bus_destroy()` frees the forwarding
bus.

Two rules follow from how the host works:

- **Never write to `cpu->sfrwrite[]` or `cpu->sfrread[]`.** The host's bus
  owns them, and a plugin that takes one over silently disconnects every
  other subscriber of that register.
- **There is no way to unsubscribe.** A model that subscribed stays
  subscribed until `sim_close()`. That is why a reset (section 7) puts
  models back to their power-on state in place and never creates them
  again.

A model written for one plugin does not need the bus at all: it can call
`host->on_sfr_write()` with a `bus_write_fn` itself. The forwarding bus
exists so that the library's models in `sim/devices/` work in a plugin
unchanged.

## 5. Wiring device models

Nothing is attached at init. A board that created all its parts up front
would subscribe to every port and step five models on every tick of a
simulation that uses none of them, and no real board has every part fitted
at once anyway. Each part is created by its `enable_*` method instead, on
the first call. A second call does nothing.

A board peripheral takes its pins from the catalog:

```c
static int m_enable_lcd(board_t *b, call_t *c)
{
    (void)c;
    if (b->lcd)
        return EMU8051_CALL_OK;
    if (need_tick(b, TICK_LCD, lcd_tick) != 0)
        return HC6800_ES_CALL_FAILED;
    b->lcd = hd44780_create(b->bus, b->cpu, HC6800_ES_LCD, b->host->clock_hz(b->sim));
    return b->lcd ? EMU8051_CALL_OK : HC6800_ES_CALL_FAILED;
}
```

`HC6800_ES_LCD` is an `hd44780_pins_t` from `hc6800_es.c`. Every model is
written in terms of the `pin_t` values it is handed, never a port of its
own, so the same `hd44780.c` serves a board that wires its HD44780 to other
pins.

A part that is not on the board (the servo, the ENC28J60 and the fan here)
has no catalog entry. Each project that adds one picks its own pins, so the
caller passes them as a port and a bit, and the plugin checks and converts
them:

```c
static int pin_ok(int32_t aPort, int32_t aBit)
{
    return aPort >= 0 && aPort <= 3 && aBit >= 0 && aBit <= 7;
}

static pin_t port_pin(int32_t aPort, int32_t aBit)
{
    return pin_make((uint8_t)(aPort * 0x10), (uint8_t)aBit);
}
```

A `pin_t`'s register is the SFR address minus 80h, as in `bus.h`, so P0 to
P3 (80h, 90h, A0h, B0h) are registers 00h, 10h, 20h and 30h.

Models that keep time read `host->clock_hz()` when they are created. A
caller that changes the clock with `sim_set_clock_hz()` does so before
enabling them, as `capi.h` says.

## 6. Stepping: tick hooks

The LCD's busy flag, the DS1302's seconds, the servo's and the fan's pulse
measurements and the IAP model's deferred reset all count time in machine
cycles. Each has a step function to call once per tick of 12 clocks, and
`host->on_tick` installs a function that the host calls at exactly that
rate:

```c
typedef void (*emu8051_tick_fn)(void *aUser);
int (*on_tick)(void *aSim, emu8051_tick_fn aFn, void *aUser);
```

In tick mode the hooks run after every `tick()`. In instruction mode, where
another plugin's cycle table times each instruction (for a 1T core, say),
the host counts clocks and runs the hooks once per 12 of them. So a model
stepped from a tick hook keeps its timing on any core. Installing one does
not change the mode, and it keeps the host in its plain `tick()` loop. A
step hook's `end` function also runs in both modes, but once per step with
that step's clocks, so the plugin would have to do the counting, and any
step hook moves the host to its slower loop.

The board installs one hook per model, when that model is enabled:

```c
static void lcd_tick(void *aUser)
{
    board_t *b = (board_t *)aUser;
    if (b->lcd)
        hd44780_step(b->lcd);
}

// Installs aFn once (aWhich is its bit in b->ticks), before the model it
// steps exists: the hook skips a model that is not there.
static int need_tick(board_t *b, unsigned aWhich, emu8051_tick_fn aFn)
{
    if (b->ticks & aWhich)
        return 0;
    if (b->host->on_tick(b->sim, aFn, b) != 0)
        return -1;
    b->ticks |= aWhich;
    return 0;
}
```

A hook is an indirect call on every tick, in the innermost loop of the
simulator. With one hook per model a simulation pays only for what it has
enabled: nothing with no device or with only the digit display (which has
no step function), one call with the LCD. A single hook for the whole board
would cost a call on every tick, plus a check for each model, as soon as any
one of them is enabled.

`need_tick()` runs before the model is created, so a failure leaves nothing
half attached, and the hook checks that its model is there. Hooks cannot be
removed, so `b->ticks` records which are installed and none is installed
twice. A simulation has room for 16 tick hooks, shared by all its plugins.

## 7. Reset and destroy

`sim_reset()` resets the CPU and then calls every plugin's `reset`. What a
board puts back is a question about the hardware: what does the reset
button on the real board change?

```c
static void board_reset(void *aCtx)
{
    board_t *b = (board_t *)aCtx;
    if (b->iap)
        iap_power_on(b->iap);
    if (b->digit_display)
        digit_display_reset(b->digit_display, b->cpu);
    if (b->lcd)
        hd44780_reset(b->lcd, b->host->clock_hz(b->sim));
}
```

The LCD and the digit display only show what the CPU drove onto their pins,
so they start over. The DS1302 is battery-backed and keeps running. The
XPT2046's readings and the state of the external parts are test stimulus,
which a reset button does not move either. For the IAP model `sim_reset()`
is a power cycle, which clears the `ISPEN` and `SWBS` bits that a software
reset keeps.

Each model is reset in place, by a `*_reset()` function of its own. A model
created again would subscribe again, and the old one's subscriptions would
stay on the host's bus, pointing at freed memory (section 4). Before the
board was a plugin, `sim_reset()` did exactly that, and the seventh reset
with the LCD and the digit display enabled tripped the bus's subscriber
limit. `python/tests/test_hc6800_plugin.py` checks 40 resets in a row.

A software reset is another matter. The IAP model resets the CPU, and only
the CPU, after the instruction that set `ISP_CONTR.SWRST`, and the plugin
gives it the function to do that with:

```c
static void iap_reset(void *aUser)
{
    board_t *b = (board_t *)aUser;
    b->host->reset_cpu(b->sim, 0);
}

/* in m_enable_iap(), after iap_create(): */
iap_set_reset(b->iap, iap_reset, b);
```

`destroy`, called from `sim_close()`, frees the models, the forwarding bus
and the context. It must not call any host function, because the
simulation is being taken apart.

## 8. Methods through `sim_plugin_call`

A caller reaches a plugin by its name, with a method name, bytes in and
bytes out:

```c
int sim_plugin_call(sim_handle_t aSim, const char *aPlugin, const char *aMethod,
                    const unsigned char *aIn, int aInLen,
                    unsigned char *aOut, int aOutCap, int *aOutLen);
```

The host knows nothing about the methods. It passes the call to the
plugin's `call` function, which is how a binding reaches any plugin with no
code of its own for it. The board's methods are a table of a name, the
number of 32-bit arguments (-1 for raw bytes) and a function:

```c
static const struct
{
    const char *name;
    int args;
    method_fn fn; // int (*)(board_t *b, call_t *c)
} METHODS[] = {
    {"enable_digit_display", 1, m_enable_digit_display},
    {"digit_get_segments", 1, m_digit_get_segments},
    /* ... */
    {"enc28j60_inject_rx", -1, m_enc28j60_inject_rx},
    /* ... */
};
```

`board_call()` looks the method up, checks that the input is exactly four
bytes per argument, decodes them as little-endian `int32_t` and calls the
function with a `call_t`, which holds the decoded arguments (`c->a[0]` and
on), the raw input and the output buffer. An integer result is four bytes
out, little-endian too, written by `ret_int()`. The
return codes are those of `plugin.h`, plus one of the board's own:

| code | when |
| --- | --- |
| `EMU8051_CALL_OK` (0) | done |
| `EMU8051_CALL_UNKNOWN` (-1) | no such method |
| `EMU8051_CALL_BAD_ARGS` (-3) | input of the wrong length, or a pin outside P0 to P3 |
| `EMU8051_CALL_TOO_SMALL` (-4) | `aOut` cannot hold the result; `*aOutLen` says how much is needed |
| `HC6800_ES_CALL_FAILED` (-16) | the device could not do it: no such frame, a poke outside the flash, out of memory |

A plugin's own error codes go below the host's, and its header documents
them. `hc6800_es_plugin.h` lists every method of the board.

Each method is named after the `capi.h` function it backs, without the
`sim_` prefix, and gives what that function gives, also for a device that
is not enabled (0, -1 or an empty line). That is because `capi.c` has no
device code left. Every `sim_enable_*` function and every device query is
one call of the method of the same name on the plugin named `"hc6800_es"`:

```c
int sim_enable_lcd(sim_handle_t aSim)
{
    return board_do(aSim, "enable_lcd", 0);
}
```

`board_do()` and `board_int()` pack the integer arguments and unpack the
result. When the simulation has no plugin of that name (a `"bare"` one),
the first such call adds the linked-in board, so `sim_enable_lcd()` on a
bare simulation works as it always did. And when a shared object named
`"hc6800_es"` has been loaded, the same functions reach it, and so do
pysim's `enable_*` methods, which call them.

Give a plugin of your own a name of its own. A second plugin of a name
already loaded is called `name#2`, and nothing that calls `name` reaches it.

A binding that wants typed functions can also call functions the plugin
exports (`EMU8051_PLUGIN_EXPORT`), passing `sim_plugin_context()` first, as
pysim's `Plugin.function()` does. The board exports none, because `capi.h`
already has typed functions for all of it.

## 9. Building on Linux and Windows

The shared object is the entry file plus everything the plugin uses: its
own source, the catalog, `bus.c` and the device models. It is complete in
itself and links against nothing but the C library.

meson (`meson setup build && ninja -C build`) builds `build/hc6800_es.so`
next to `build/libem8051sim.so`:

```meson
hc6800_es_plugin = shared_module('hc6800_es',
    ['sim/boards/hc6800_es_entry.c'] + sim_board_sources,
    include_directories : app_inc,
    name_prefix : '',
    gnu_symbol_visibility : 'hidden',
    link_args : plugin_link_args,      # -Wl,-z,defs where the linker has it
)
```

`make sim` builds the same two files with no meson. By hand, from the
repository root:

```sh
cc -std=gnu99 -O2 -fPIC -I. -fvisibility=hidden -shared -Wl,-z,defs \
   -o hc6800_es.so sim/boards/hc6800_es_entry.c sim/boards/hc6800_es_plugin.c \
   sim/boards/hc6800_es.c sim/bus.c sim/devices/*.c
```

Two of those flags matter:

- **`-fvisibility=hidden`** exports only what `EMU8051_PLUGIN_EXPORT`
  marks. The plugin carries its own copies of `bus.c` and the models, with
  the same names as the library's. Hidden, they stay the plugin's own.
  Without it, the dynamic linker may bind the plugin's calls to the
  library's copies, which come from another build.
- **`-Wl,-z,defs`** fails the link on any symbol that is neither in the
  plugin nor in the C library. A plugin that called into `libem8051sim`
  directly, around the host table, fails here and not at load time.

Then check what it exports:

```
$ nm -D --defined-only build/hc6800_es.so
0000000000001280 T emu8051_plugin_init
```

For Windows, `build_sim_windows.sh` builds `build/libem8051sim.dll` and
`build/hc6800_es.dll` with no MSVC and no meson. It uses Zig's bundled clang
from PyPI (the `ziglang` package), which `uv` fetches once, and
`UV_OFFLINE=1` keeps it off the network after that. The compiler targets
`x86_64-windows-gnu` explicitly, so the script runs from a POSIX shell on
Windows or on Linux alike:

```sh
UV_OFFLINE=1 sh build_sim_windows.sh
objdump -p build/hc6800_es.dll
```

There `EMU8051_PLUGIN_EXPORT` is `__declspec(dllexport)`, and once one
symbol is marked, the linker exports only the marked ones. The export table
should have one name:

```
[Ordinal/Name Pointer] Table -- Ordinal Base 1
          Ordinal   Hint Name
    [   0] +base[   1]  0000 emu8051_plugin_init
```

and the import tables only `KERNEL32.dll` and the C runtime's
`api-ms-win-crt-*.dll`. A DLL must resolve every symbol when it is linked,
so on Windows the `-z defs` check comes with the platform.

## 10. Static registration

`sim_add_plugin()` adds a plugin that is linked into the program. It takes
the init function itself:

```c
SIM_API int sim_add_plugin(sim_handle_t aSim,
                           int (*aInit)(const struct emu8051_host *aHost, struct emu8051_plugin *aPlugin,
                                        const char *aArgs),
                           const char *aArgs);
```

`aInit` has the type of `plugin.h`'s `emu8051_plugin_init_fn`. `capi.h`
spells it out so that it does not need `plugin.h`. A NULL `aInit` gives
`SIM_PLUGIN_NO_ENTRY`.

```c
#include "sim/capi.h"
#include "sim/boards/hc6800_es_plugin.h"

sim_handle_t s = sim_open("bare");
if (sim_add_plugin(s, hc6800_es_plugin_init, "") != SIM_PLUGIN_OK)
    fprintf(stderr, "%s\n", sim_plugin_error(s));
```

Everything after that is the same as for a loaded plugin: the same init,
the same checks, the same name. `sim_plugin_path()` is NULL for it, so
pysim's `Plugin.path` is `None`, and `Plugin.function()` looks for exported
functions in the simulator library itself.

`sim_open()` does this for the boards it knows. Its table in `capi.c` pairs
a name with an init function:

```c
static const struct
{
    const char *name;
    emu8051_plugin_init_fn init;
} BOARDS[] = {
    {"hc6800_es", hc6800_es_plugin_init},
    {"bare", NULL},
};
```

So `sim_open("hc6800_es")` is `sim_open("bare")` followed by
`sim_add_plugin(hc6800_es_plugin_init)`, with nothing to configure. A board
added to this repository gets a line in that table. A program outside it
opens `"bare"` and adds its board with `sim_add_plugin()`.

A program with no dynamic loader at all (no `dlopen`, no `LoadLibrary`)
compiles `sim/plugin.c` with `SIM_NO_DLOPEN` defined. `plugin.c` then
includes neither `dlfcn.h` nor `windows.h`, `sim_load_plugin()` fails with
`SIM_PLUGIN_OPEN_FAILED` and says why, and nothing else changes. Such a
program compiles the plugin's sources in with its own and leaves the entry
file out. `tests/Makefile` builds its plugin test that way as well, and
checks with `nm` that the result has no reference to `dlopen`.

## 11. Loading and testing it

From C, onto a bare simulation:

```c
sim_handle_t s = sim_open("bare");
if (sim_load_plugin(s, "build/hc6800_es.so", "") != SIM_PLUGIN_OK)
{
    fprintf(stderr, "%s\n", sim_plugin_error(s));
    sim_close(s); // a failed load leaves the simulation in an unknown state
    return 1;
}
sim_load_hex(s, "program.hex");
sim_enable_lcd(s); // the loaded plugin answers: it is "hc6800_es"
sim_step(s, 100000);

char line[17];
sim_lcd_get_line(s, 0, 16, line, sizeof line);

unsigned char in[8] = {0, 0, 0, 0, 16, 0, 0, 0}; // line 0, width 16
unsigned char out[16];
int n;
sim_plugin_call(s, "hc6800_es", "lcd_get_line", in, sizeof in, out, sizeof out, &n);
```

Load it before calling any `sim_enable_*` function. On a bare simulation
such a call adds the linked-in board, and the loaded one would then be
`hc6800_es#2`, which those functions do not reach. Loading it onto
`sim_open("hc6800_es")` gives `hc6800_es#2` for the same reason.

From Python:

```python
import struct
from pysim import Simulator

sim = Simulator("bare")
board = sim.load_plugin("build/hc6800_es.so")  # PluginError with the reason if it fails
sim.load_hex("program.hex")
sim.enable_lcd()
sim.step(100_000)
print(sim.lcd_line(0))
print(board.call("lcd_get_line", struct.pack("<ii", 0, 16)).decode())
```

`Plugin.call()` takes and returns bytes. It raises `PluginError`, with the
code, for anything but `EMU8051_CALL_OK`, and on `EMU8051_CALL_TOO_SMALL` it
asks again with a buffer of the size the plugin asked for.

To test a plugin that is built two ways, run both copies side by side and
compare everything they show. Both tests here do that with one program: an
8-bit LFSR written to P0, P2, P1 and P3 in turn, so that every device on the
board sees edges, which now and then writes SBUF and sets
`ISP_CONTR.SWRST`.

- `tests/test_hc6800_plugin.c` runs three simulations:
  `sim_open("hc6800_es")`, a bare one with `sim_add_plugin()`, and a bare
  one with `sim_load_plugin()` of `tests/hc6800_es.so`. It enables every
  device on each, steps them in slices with a `sim_reset()` halfway, and
  compares every device's state after each slice. A negative control checks
  that the devices did change, so the test is not comparing three idle
  boards. `make -C tests check` runs it a second time as
  `test_hc6800_plugin_nodl`, built with `SIM_NO_DLOPEN`, which checks that
  `sim_load_plugin()` refuses there.
- `python/tests/test_hc6800_plugin.py` does the same through pysim: in tick
  mode, with software resets, and in instruction mode with the 1T cycle
  table of `tests/plugin/demo_plugin.c`. With `EMU8051_DEMO_BUILD` set to a
  directory of compiled demo programs, it compares the two on those too. It
  needs the meson build. `EMU8051_HC6800_PLUGIN` points it at a shared
  object built somewhere else.

```sh
make -C tests check
meson setup build && ninja -C build
cd python && python -m unittest discover -s tests -p 'test_hc6800_plugin.py' -v
```

## 12. Checklist for a board of your own

- A pin catalog for the board, with no code in it.
- An init function with a name of its own, and the exported
  `emu8051_plugin_init` in a file of its own.
- In init: the ABI first, then `EMU8051_HOST_HAS()` for every member added
  later that you use, then the arguments, with `host->error()` for each
  refusal.
- Create nothing until a method asks for it.
- Subscribe through `bus_create_forwarding()` or the host's functions,
  never through `cpu->sfrwrite[]` or `cpu->sfrread[]`.
- One tick hook per model that keeps time, installed once, when the model is
  enabled.
- Reset each model in place. Never create a model a second time.
- No host calls in `destroy`.
- A method table with fixed argument sizes that are checked,
  `EMU8051_CALL_UNKNOWN` for a method you do not have, and error codes of
  your own below -4, documented in your header.
- Build with hidden visibility and `-Wl,-z,defs`, and check the exports with
  `nm -D` or `objdump -p`.
- Test the shared object against the linked-in copy, with a negative
  control.
