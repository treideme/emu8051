#!/bin/sh
# Build build/libem8051sim.dll and build/hc6800_es.dll without meson or
# MSVC: this machine has no C toolchain on PATH, so use Zig's bundled clang
# from PyPI (ziglang), which uv fetches on demand (UV_OFFLINE=1 once it is
# cached). pysim.find_library() looks in build/ first.
set -e
cd "$(dirname "$0")"
mkdir -p build
CC="uv run --quiet --no-project --python 3.12 --with ziglang python -m ziglang cc"
BOARD="sim/bus.c sim/boards/hc6800_es.c sim/boards/hc6800_es_plugin.c \
  sim/devices/hd44780.c sim/devices/hc573.c sim/devices/hc138.c \
  sim/devices/digit_display.c sim/devices/ds1302.c sim/devices/xpt2046.c \
  sim/devices/servo.c sim/devices/enc28j60.c sim/devices/fan.c \
  sim/devices/iap.c"
# The library, with the HC6800-ES board linked in.
$CC -shared -O2 -std=gnu99 -I. -target x86_64-windows-gnu -o build/libem8051sim.dll \
  core.c disasm.c opcodes.c sim/capi.c sim/plugin.c $BOARD
# The same board as a plugin of its own, for sim_load_plugin(); it exports
# only emu8051_plugin_init.
$CC -shared -O2 -std=gnu99 -I. -target x86_64-windows-gnu -o build/hc6800_es.dll \
  sim/boards/hc6800_es_entry.c $BOARD
echo "built build/libem8051sim.dll build/hc6800_es.dll"
