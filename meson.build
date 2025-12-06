project('emu8051', 'c',
    version : '0.0.1',
    license : 'MIT',
    meson_version : '>= 0.52',
    default_options : ['cpp_std=gnu99',
                       'warning_level=3',
                       'buildtype=release',
                       ],
)

# Only the interactive ncurses GUI (emu.c + logicboard.c/mainview.c/...)
# needs curses. The CPU core, the sim/ peripheral layer, and testcase.c
# are plain ANSI C with no UI dependency at all -- curses being absent
# (e.g. no ncurses/PDCurses on a given Windows box) should not block
# building or testing any of that.
curses_dep = dependency('curses', required : false)

app_inc = include_directories(
	'.',
	)

# Core 8051 simulation functions
core_library = library('core',
    ['core.c',
     'disasm.c',
     'opcodes.c'],
     include_directories : app_inc,
     install: true,
 )

# emulator build (interactive curses GUI) -- only if curses is available
if curses_dep.found()
    emu = executable(
        'emu',
        ['emu.c',
         'logicboard.c',
         'mainview.c',
         'memeditor.c',
         'options.c',
         'popups.c'],
         include_directories : app_inc,
         install: true,
         link_with: core_library,
         dependencies: [curses_dep]
    )
endif

testprog = executable(
    'testprog',
    ['testcase.c'],
    include_directories : app_inc,
    install: true,
    link_with: core_library,
)

# Peripheral simulation layer: the bus/pin connector abstraction, device
# models (sim/devices/*), board pinout catalogs (sim/boards/*), and the
# flat C API (sim/capi.h) other languages bind against. See
# sim/bus.h/sim/capi.h for the design rationale.
sim_library = shared_library('em8051sim',
    ['sim/bus.c',
     'sim/capi.c',
     'sim/devices/hd44780.c',
     'sim/devices/hc573.c',
     'sim/devices/hc138.c',
     'sim/devices/digit_display.c',
     'sim/devices/ds1302.c',
     'sim/devices/xpt2046.c',
     'sim/devices/servo.c',
     'sim/devices/enc28j60.c',
     'sim/devices/fan.c',
     'sim/boards/hc6800_es.c'],
    include_directories : app_inc,
    install: true,
    link_with: core_library,
)

# Same core+sim code, but as a static library -- for the sim_test C
# executable below, which doesn't need a relocatable shared object.
sim_library_static = static_library('em8051sim_static',
    ['sim/bus.c',
     'sim/capi.c',
     'sim/devices/hd44780.c',
     'sim/devices/hc573.c',
     'sim/devices/hc138.c',
     'sim/devices/digit_display.c',
     'sim/devices/ds1302.c',
     'sim/devices/xpt2046.c',
     'sim/devices/servo.c',
     'sim/devices/enc28j60.c',
     'sim/devices/fan.c',
     'sim/boards/hc6800_es.c'],
    include_directories : app_inc,
    link_with: core_library,
)

sim_test = executable(
    'sim_test',
    ['sim/sim_test.c'],
    include_directories : app_inc,
    install: true,
    link_with: sim_library_static,
)
