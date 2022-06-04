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

emu = executable(
    'emu',
    ['core.c', 
     'disasm.c', 
     'emu.c', 
     'logicboard.c', 
     'mainview.c', 
     'memeditor.c', 
     'opcodes.c',
     'options.c',
     'popups.c'],
     include_directories : app_inc,
     install: true,
     dependencies: app_deps
 )
