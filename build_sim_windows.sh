#!/bin/sh
# Build build/libem8051sim.dll without meson or MSVC: this machine has no C
# toolchain on PATH, so use Zig's bundled clang from PyPI (ziglang), which
# uv fetches on demand. pysim.find_library() looks in build/ first.
set -e
cd "$(dirname "$0")"
mkdir -p build
uv run --quiet --no-project --python 3.12 --with ziglang python -m ziglang cc \
  -shared -O2 -std=gnu99 -I. -target x86_64-windows-gnu -o build/libem8051sim.dll \
  core.c disasm.c opcodes.c \
  sim/bus.c sim/capi.c sim/boards/hc6800_es.c \
  sim/devices/hd44780.c sim/devices/hc573.c sim/devices/hc138.c \
  sim/devices/digit_display.c sim/devices/ds1302.c sim/devices/xpt2046.c \
  sim/devices/servo.c sim/devices/enc28j60.c sim/devices/fan.c
echo "built build/libem8051sim.dll"
