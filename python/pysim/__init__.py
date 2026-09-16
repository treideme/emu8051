"""Thin ctypes wrapper over sim/capi.h.

Deliberately ctypes rather than a hand-written CPython C-extension: the
capi.h surface is already a plain-C, opaque-handle ABI (see that header's
own rationale), so there's nothing a real extension module would buy here
except a second, Python-version-specific artifact to build and ship.
ctypes loads the same shared library everything else (the GUI, a future
board's own tooling) uses, with no extra build step.

Usage::

    from pysim import Simulator

    with Simulator("hc6800_es", "18_lcd_name_id.hex") as sim:
        sim.enable_lcd()
        sim.step_instructions(2_000_000)
        assert sim.lcd_line(0) == "hongXkeX"
"""
import ctypes
import os
import platform
import sys

__all__ = ["Simulator", "Exception8051", "find_library"]


def find_library():
    """Locate the built em8051sim shared library.

    Checks (in order): $EM8051SIM_LIB, the meson build/ directory next to
    this repo (both "build" and "build/sim" -- meson's own layout, depends
    on version/backend), then falls back to the platform's usual shared
    library name on the current directory / default search path.
    """
    env = os.environ.get("EM8051SIM_LIB")
    if env and os.path.isfile(env):
        return env

    name = {
        "Windows": "libem8051sim.dll",
        "Darwin": "libem8051sim.dylib",
    }.get(platform.system(), "libem8051sim.so")

    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    candidates = [
        os.path.join(repo_root, "build", name),
        os.path.join(repo_root, "build", "sim", name),
        name,
    ]
    for c in candidates:
        if os.path.isfile(c):
            return c
    # Let ctypes make one more attempt (e.g. it's on PATH/LD_LIBRARY_PATH)
    return name


class Exception8051(RuntimeError):
    """Raised for sim_open()/sim_load_hex() failures. CPU exceptions
    recorded during execution (illegal opcode, stack overflow, etc, see
    EM8051_EXCEPTION in emu8051.h) are not raised as Python exceptions --
    call .exceptions() to check for those explicitly, since they're often
    expected/benign in a test that's deliberately probing edge behavior.
    """


class _CAPI:
    """Loads the library once and declares every function's signature.
    Not part of the public API -- use Simulator.
    """

    _lib = None

    @classmethod
    def get(cls):
        if cls._lib is None:
            lib = ctypes.CDLL(find_library())
            HANDLE = ctypes.c_void_p

            lib.sim_open.restype = HANDLE
            lib.sim_open.argtypes = [ctypes.c_char_p]
            lib.sim_close.argtypes = [HANDLE]

            lib.sim_load_hex.restype = ctypes.c_int
            lib.sim_load_hex.argtypes = [HANDLE, ctypes.c_char_p]
            lib.sim_reset.argtypes = [HANDLE]

            lib.sim_step.restype = ctypes.c_long
            lib.sim_step.argtypes = [HANDLE, ctypes.c_long]
            lib.sim_step_instructions.restype = ctypes.c_long
            lib.sim_step_instructions.argtypes = [HANDLE, ctypes.c_long]
            lib.sim_get_instruction_count.restype = ctypes.c_long
            lib.sim_get_instruction_count.argtypes = [HANDLE]
            lib.sim_get_tick_count.restype = ctypes.c_ulong
            lib.sim_get_tick_count.argtypes = [HANDLE]
            lib.sim_get_pc.restype = ctypes.c_int
            lib.sim_get_pc.argtypes = [HANDLE]
            lib.sim_get_clock_hz.restype = ctypes.c_ulong
            lib.sim_get_clock_hz.argtypes = [HANDLE]
            lib.sim_set_clock_hz.argtypes = [HANDLE, ctypes.c_ulong]

            lib.sim_get_port.restype = ctypes.c_int
            lib.sim_get_port.argtypes = [HANDLE, ctypes.c_int]
            lib.sim_peek_idata.restype = ctypes.c_int
            lib.sim_peek_idata.argtypes = [HANDLE, ctypes.c_int]
            lib.sim_peek_sfr.restype = ctypes.c_int
            lib.sim_peek_sfr.argtypes = [HANDLE, ctypes.c_int]
            lib.sim_set_pin.argtypes = [HANDLE, ctypes.c_int, ctypes.c_int, ctypes.c_int]
            lib.sim_get_pin.restype = ctypes.c_int
            lib.sim_get_pin.argtypes = [HANDLE, ctypes.c_int, ctypes.c_int]

            lib.sim_get_exceptions.restype = ctypes.c_int
            lib.sim_get_exceptions.argtypes = [HANDLE, ctypes.POINTER(ctypes.c_int), ctypes.c_int]

            lib.sim_enable_digit_display.restype = ctypes.c_int
            lib.sim_enable_digit_display.argtypes = [HANDLE, ctypes.c_int]
            lib.sim_digit_get_segments.restype = ctypes.c_int
            lib.sim_digit_get_segments.argtypes = [HANDLE, ctypes.c_int]
            lib.sim_digit_get_char.restype = ctypes.c_int
            lib.sim_digit_get_char.argtypes = [HANDLE, ctypes.c_int]

            lib.sim_enable_lcd.restype = ctypes.c_int
            lib.sim_enable_lcd.argtypes = [HANDLE]
            lib.sim_lcd_get_line.argtypes = [HANDLE, ctypes.c_int, ctypes.c_int, ctypes.c_char_p, ctypes.c_int]

            lib.sim_enable_ds1302.restype = ctypes.c_int
            lib.sim_enable_ds1302.argtypes = [HANDLE]
            lib.sim_ds1302_set_time.argtypes = [HANDLE] + [ctypes.c_int] * 7
            lib.sim_ds1302_get_register.restype = ctypes.c_int
            lib.sim_ds1302_get_register.argtypes = [HANDLE, ctypes.c_int]

            lib.sim_enable_xpt2046.restype = ctypes.c_int
            lib.sim_enable_xpt2046.argtypes = [HANDLE]
            lib.sim_xpt2046_set_reading.argtypes = [HANDLE, ctypes.c_int]
            lib.sim_xpt2046_set_channel_reading.argtypes = [HANDLE, ctypes.c_int, ctypes.c_int]
            lib.sim_xpt2046_get_last_channel.restype = ctypes.c_int
            lib.sim_xpt2046_get_last_channel.argtypes = [HANDLE]

            lib.sim_uart_tx_count.restype = ctypes.c_int
            lib.sim_uart_tx_count.argtypes = [HANDLE]
            lib.sim_uart_tx_byte.restype = ctypes.c_int
            lib.sim_uart_tx_byte.argtypes = [HANDLE, ctypes.c_int]
            lib.sim_uart_inject_rx.argtypes = [HANDLE, ctypes.c_ubyte]

            lib.sim_enable_servo.restype = ctypes.c_int
            lib.sim_enable_servo.argtypes = [HANDLE, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int]
            lib.sim_servo_get_pulse_us.restype = ctypes.c_int
            lib.sim_servo_get_pulse_us.argtypes = [HANDLE]
            lib.sim_servo_get_angle_decidegrees.restype = ctypes.c_int
            lib.sim_servo_get_angle_decidegrees.argtypes = [HANDLE]

            lib.sim_enable_enc28j60.restype = ctypes.c_int
            lib.sim_enable_enc28j60.argtypes = [HANDLE] + [ctypes.c_int] * 8
            lib.sim_enc28j60_get_register.restype = ctypes.c_int
            lib.sim_enc28j60_get_register.argtypes = [HANDLE, ctypes.c_int, ctypes.c_int]
            lib.sim_enc28j60_get_bank.restype = ctypes.c_int
            lib.sim_enc28j60_get_bank.argtypes = [HANDLE]
            lib.sim_enc28j60_get_last_opcode.restype = ctypes.c_int
            lib.sim_enc28j60_get_last_opcode.argtypes = [HANDLE]
            lib.sim_enc28j60_get_buffer_byte_count.restype = ctypes.c_ulong
            lib.sim_enc28j60_get_buffer_byte_count.argtypes = [HANDLE]

            lib.sim_enable_fan.restype = ctypes.c_int
            lib.sim_enable_fan.argtypes = [HANDLE, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int]
            lib.sim_fan_get_duty_percent.restype = ctypes.c_int
            lib.sim_fan_get_duty_percent.argtypes = [HANDLE]
            lib.sim_fan_get_rpm.restype = ctypes.c_int
            lib.sim_fan_get_rpm.argtypes = [HANDLE]

            cls._lib = lib
        return cls._lib


class Simulator:
    """One simulated CPU + its enabled peripherals. Not thread-safe (same
    restriction as the C API: single-threaded, synchronous, poll-driven --
    see capi.h)."""

    # EM8051_EXCEPTION, mirrored from emu8051.h for readable error reports.
    EXCEPTION_NAMES = [
        "STACK",
        "ACC_TO_A",
        "IRET_PSW_MISMATCH",
        "IRET_SP_MISMATCH",
        "IRET_ACC_MISMATCH",
        "ILLEGAL_OPCODE",
    ]

    def __init__(self, board: str, hexfile: str | None = None):
        self._capi = _CAPI.get()
        self._handle = self._capi.sim_open(board.encode())
        if not self._handle:
            raise Exception8051(f"unknown board {board!r}")
        if hexfile is not None:
            self.load_hex(hexfile)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
        return False

    def close(self):
        if self._handle:
            self._capi.sim_close(self._handle)
            self._handle = None

    def load_hex(self, path: str):
        rc = self._capi.sim_load_hex(self._handle, os.fspath(path).encode())
        if rc != 0:
            raise Exception8051(f"failed to load {path!r} (rc={rc})")

    def reset(self):
        self._capi.sim_reset(self._handle)

    def step(self, ticks: int) -> int:
        """Advance ticks (12-clock units). Returns instructions completed."""
        return self._capi.sim_step(self._handle, ticks)

    def step_instructions(self, count: int) -> int:
        """Run until count instructions have completed (or an internal
        safety cap is hit -- see capi.h). Returns instructions completed."""
        return self._capi.sim_step_instructions(self._handle, count)

    @property
    def instruction_count(self) -> int:
        return self._capi.sim_get_instruction_count(self._handle)

    @property
    def tick_count(self) -> int:
        return self._capi.sim_get_tick_count(self._handle)

    @property
    def pc(self) -> int:
        """Address of the next instruction to execute. After
        step_instructions(1) this is an instruction boundary, so comparing it
        against a .map symbol gives you an address breakpoint."""
        return self._capi.sim_get_pc(self._handle)

    @property
    def clock_hz(self) -> int:
        """Board oscillator frequency; ticks-per-second is clock_hz/12 (1
        tick = 1 machine cycle, see sim/devices/ds1302.c's own derivation
        of this from hd44780.c's busy-timing constants)."""
        return self._capi.sim_get_clock_hz(self._handle)

    def set_clock_hz(self, hz: int):
        """Override the board's default oscillator frequency. Call right
        after opening, before enable_lcd()/enable_ds1302() -- both capture
        clock_hz at their own creation time (see capi.h's own note)."""
        self._capi.sim_set_clock_hz(self._handle, hz)

    def port(self, index: int) -> int:
        return self._capi.sim_get_port(self._handle, index)

    def peek_idata(self, address: int) -> int:
        return self._capi.sim_peek_idata(self._handle, address)

    def peek_sfr(self, address: int) -> int:
        return self._capi.sim_peek_sfr(self._handle, address)

    def set_pin(self, port: int, bit: int, value: int | None):
        """value=None releases the pin back to normal (CPU-driven) behavior."""
        self._capi.sim_set_pin(self._handle, port, bit, -1 if value is None else int(bool(value)))

    def get_pin(self, port: int, bit: int) -> int:
        return self._capi.sim_get_pin(self._handle, port, bit)

    def exceptions(self) -> list[str]:
        """Returns and clears CPU exceptions recorded since the last call."""
        buf = (ctypes.c_int * 64)()
        n = self._capi.sim_get_exceptions(self._handle, buf, 64)
        return [self.EXCEPTION_NAMES[c] if 0 <= c < len(self.EXCEPTION_NAMES) else str(c) for c in buf[:n]]

    # --- peripherals ---

    def enable_digit_display(self, digit_count: int):
        self._capi.sim_enable_digit_display(self._handle, digit_count)
        self._digit_count = digit_count

    def digit_segments(self, digit: int) -> int:
        return self._capi.sim_digit_get_segments(self._handle, digit)

    def digit_char(self, digit: int) -> str:
        return chr(self._capi.sim_digit_get_char(self._handle, digit))

    def digits_text(self) -> str:
        return "".join(self.digit_char(i) for i in range(self._digit_count))

    def enable_lcd(self):
        self._capi.sim_enable_lcd(self._handle)

    def lcd_line(self, line: int, width: int = 16) -> str:
        buf = ctypes.create_string_buffer(width + 1)
        self._capi.sim_lcd_get_line(self._handle, line, width, buf, len(buf))
        return buf.value.decode("ascii", errors="replace")

    def enable_ds1302(self):
        self._capi.sim_enable_ds1302(self._handle)

    def ds1302_set_time(self, second, minute, hour, date, month, weekday, year):
        self._capi.sim_ds1302_set_time(self._handle, second, minute, hour, date, month, weekday, year)

    def ds1302_register(self, index: int) -> int:
        """Raw BCD byte, register 0=seconds .. 6=year, 7=write-protect."""
        return self._capi.sim_ds1302_get_register(self._handle, index)

    def enable_xpt2046(self):
        self._capi.sim_enable_xpt2046(self._handle)

    def xpt2046_set_reading(self, value_12bit: int):
        self._capi.sim_xpt2046_set_reading(self._handle, value_12bit)

    def xpt2046_set_channel_reading(self, channel: int, value_12bit: int):
        self._capi.sim_xpt2046_set_channel_reading(self._handle, channel, value_12bit)

    def xpt2046_last_channel(self) -> int:
        return self._capi.sim_xpt2046_get_last_channel(self._handle)

    def uart_tx_bytes(self) -> bytes:
        n = self._capi.sim_uart_tx_count(self._handle)
        return bytes(self._capi.sim_uart_tx_byte(self._handle, i) & 0xFF for i in range(n))

    def uart_inject_rx(self, byte: int):
        self._capi.sim_uart_inject_rx(self._handle, byte)

    # --- external peripherals: not part of any board catalog (see
    # capi.h's own note) -- caller passes its own assumed pin wiring,
    # port 0-3 for P0-P3, bit 0-7, same convention as set_pin()/get_pin().

    def enable_servo(self, pwm_port: int, pwm_bit: int, pulse_min_us: int = 1000, pulse_max_us: int = 2000):
        self._capi.sim_enable_servo(self._handle, pwm_port, pwm_bit, pulse_min_us, pulse_max_us)

    def servo_pulse_us(self) -> int:
        return self._capi.sim_servo_get_pulse_us(self._handle)

    def servo_angle_decidegrees(self) -> int:
        """Tenths of a degree, 0-1800, so a caller doesn't need floats."""
        return self._capi.sim_servo_get_angle_decidegrees(self._handle)

    def enable_enc28j60(self, cs, sck, mosi, miso):
        """Each of cs/sck/mosi/miso is a (port, bit) tuple."""
        self._capi.sim_enable_enc28j60(
            self._handle, cs[0], cs[1], sck[0], sck[1], mosi[0], mosi[1], miso[0], miso[1]
        )

    def enc28j60_register(self, bank: int, address: int) -> int:
        return self._capi.sim_enc28j60_get_register(self._handle, bank, address)

    def enc28j60_bank(self) -> int:
        return self._capi.sim_enc28j60_get_bank(self._handle)

    def enc28j60_last_opcode(self) -> int:
        return self._capi.sim_enc28j60_get_last_opcode(self._handle)

    def enc28j60_buffer_byte_count(self) -> int:
        return self._capi.sim_enc28j60_get_buffer_byte_count(self._handle)

    def enable_fan(self, pwm, tach):
        """Each of pwm/tach is a (port, bit) tuple."""
        self._capi.sim_enable_fan(self._handle, pwm[0], pwm[1], tach[0], tach[1])

    def fan_duty_percent(self) -> int:
        return self._capi.sim_fan_get_duty_percent(self._handle)

    def fan_rpm(self) -> int:
        return self._capi.sim_fan_get_rpm(self._handle)
