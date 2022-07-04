project('emu8051', 'c',
    version : '0.0.1',
    license : 'MIT',
    meson_version : '>= 0.52',
    default_options : ['cpp_std=gnu99',
                       'warning_level=3',
                       'buildtype=release',
                       ],
)

curses_dep = dependency('curses')

app_deps = [
    curses_dep,
]

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
     dependencies: app_deps
 )

# emulator build
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
     dependencies: app_deps
)

testprog = executable(
    'testprog',
    ['testcase.c'],
    include_directories : app_inc,
         install: true,
         link_with: core_library,
         dependencies: app_deps
)
