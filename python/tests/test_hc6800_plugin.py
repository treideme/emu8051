"""The HC6800-ES board as a plugin (sim/boards/hc6800_es_plugin.h).

Simulator("hc6800_es") is a bare 8051 with the board plugin linked into the
library; the same source built as a shared object (build/hc6800_es.so, or
hc6800_es.dll) is loaded with load_plugin() onto a "bare" one. These tests
run the same program on both and check that every device reads the same,
through the Simulator methods (which call the plugin of that name, the
loaded one included) and through the plugin's own methods.

The shared object needs the meson build; those tests are skipped without
it. With EMU8051_DEMO_BUILD set, the demo programs run on both as well.

    python -m unittest discover -s tests -v   (from python/)
"""

import os
import platform
import struct
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(REPO, "python"))
sys.path.insert(0, HERE)
from pysim import Plugin, PluginError, Simulator, find_library  # noqa: E402
from test_hc6800_es import hexpath  # noqa: E402

SUFFIX = ".dll" if platform.system() == "Windows" else ".so"


def _built(name):
    for d in ("build", os.path.join("build", "tests", "plugin")):
        p = os.path.join(REPO, d, name)
        if os.path.isfile(p):
            return p
    return None


LIBRARY = os.path.isfile(find_library())
BOARD = os.environ.get("EMU8051_HC6800_PLUGIN") or _built("hc6800_es" + SUFFIX)
DEMO = os.environ.get("EMU8051_DEMO_PLUGIN") or _built("demo_plugin" + SUFFIX)
NO_LIBRARY = None if LIBRARY else "emu8051 sim library not built"
NO_BOARD = NO_LIBRARY or (None if BOARD else "build/hc6800_es%s not built (meson setup build && ninja -C build)"
                          % SUFFIX)

# An 8-bit Galois LFSR (x^8+x^4+x^3+x^2+1) written to P0, P2, P1 and P3 in
# turn, so every device on the board sees edges; when the fifth value of a
# pass is 74h it goes to SBUF, and A7h sets ISP_CONTR.SWRST (a software
# reset, with the IAP model enabled). The same program as
# tests/test_hc6800_plugin.c.
LFSR = {
    0x0000: [0x02, 0x00, 0x30],
    0x0030: [0x75, 0x81, 0x60, 0x75, 0x98, 0x40, 0x75, 0x89, 0x20, 0x75, 0x8D, 0xFD, 0xD2, 0x8E, 0x7F, 0x01,
             0x11, 0x5F, 0xF5, 0x80, 0x11, 0x5F, 0xF5, 0xA0, 0x11, 0x5F, 0xF5, 0x90, 0x11, 0x5F, 0xF5, 0xB0,
             0x11, 0x5F, 0xB4, 0x74, 0x02, 0xF5, 0x99, 0xB4, 0xA7, 0x03, 0x75, 0xE7, 0x20, 0x80, 0xE1, 0xEF,
             0xC3, 0x33, 0x50, 0x02, 0x64, 0x1D, 0xFF, 0x22],
}


def hexfile(test, chunks):
    """Write {address: bytes} as Intel HEX; returns the path."""
    lines = []
    for addr, data in sorted(chunks.items()):
        for i in range(0, len(data), 16):
            part = bytes(data[i:i + 16])
            a = addr + i
            rec = bytes([len(part), a >> 8, a & 0xFF, 0]) + part
            lines.append(":%s%02X" % (rec.hex().upper(), (-sum(rec)) & 0xFF))
    lines.append(":00000001FF")
    fd, path = tempfile.mkstemp(suffix=".hex")
    with os.fdopen(fd, "w") as f:
        f.write("\n".join(lines) + "\n")
    test.addCleanup(os.unlink, path)
    return path


def enable_everything(sim):
    sim.enable_digit_display(8)
    sim.enable_lcd()
    sim.enable_ds1302()
    sim.ds1302_set_time(second=50, minute=59, hour=23, date=31, month=12, weekday=7, year=99)
    sim.enable_xpt2046()
    sim.xpt2046_set_channel_reading(1, 0x5A5)
    sim.enable_servo(1, 1, 1000, 2000)
    sim.enable_fan((1, 2), (1, 3))
    sim.enable_enc28j60((1, 4), (1, 5), (1, 6), (1, 7))


def state(sim):
    """Everything the Simulator reads back."""
    return {
        "pc": sim.pc, "ports": [sim.port(i) for i in range(4)], "clocks": sim.clock_count,
        "instructions": sim.instruction_count, "ticks": sim.tick_count,
        "lcd": [sim.lcd_line(0), sim.lcd_line(1)],
        "segments": [sim.digit_segments(i) for i in range(8)],
        "chars": [sim.digit_char(i) for i in range(8)],
        "ds1302": [sim.ds1302_register(i) for i in range(8)],
        "xpt2046": sim.xpt2046_last_channel(),
        "servo": (sim.servo_pulse_us(), sim.servo_angle_decidegrees()),
        "fan": (sim.fan_duty_percent(), sim.fan_rpm()),
        "enc28j60": (sim.enc28j60_bank(), sim.enc28j60_last_opcode(), sim.enc28j60_tx_count(),
                     sim.enc28j60_buffer_byte_count(),
                     [sim.enc28j60_register(b, a) for b in range(4) for a in range(32)]),
        "iap": sim.iap_stats(), "flash": [sim.iap_peek(0x2000 + i) for i in range(4)],
        "uart": sim.uart_tx_bytes(), "exceptions": sim.exceptions(),
    }


class Fixture(unittest.TestCase):
    def linked(self, plugin_args=None):
        s = Simulator("hc6800_es")
        self.addCleanup(s.close)
        if plugin_args is not None:
            s.load_plugin(DEMO, plugin_args)
        return s

    def loaded(self, plugin_args=None):
        s = Simulator("bare")
        self.addCleanup(s.close)
        p = s.load_plugin(BOARD)
        if plugin_args is not None:
            s.load_plugin(DEMO, plugin_args)
        return s, p

    def run_both(self, a, b, slices, step):
        """Step both, comparing after every slice; reset both halfway."""
        changed = 0
        idle = state(a)
        for n in range(slices):
            if n == slices // 2:
                a.reset()
                b.reset()
            step(a)
            step(b)
            sa, sb = state(a), state(b)
            self.assertEqual(sa, sb, "slice %d" % n)
            changed += sa["lcd"] != idle["lcd"] or sa["segments"] != idle["segments"]
        self.assertGreater(changed, 0, "the program never reached the devices")
        return sa


@unittest.skipIf(NO_LIBRARY, NO_LIBRARY)
class LinkedInTests(Fixture):
    def test_the_board_is_a_plugin_linked_into_the_library(self):
        s = self.linked()
        self.assertEqual(s.plugins(), ["hc6800_es"])
        p = s.plugin("hc6800_es")
        self.assertIsNone(p.path)
        self.assertTrue(p.context)
        self.assertEqual(p.lib, s._capi)

    def test_bare_has_no_board_until_a_device_is_enabled(self):
        s = Simulator("bare")
        self.addCleanup(s.close)
        self.assertEqual(s.plugins(), [])
        s.enable_lcd()
        self.assertEqual(s.plugins(), ["hc6800_es"])
        self.assertEqual(s.lcd_line(0), " " * 16)

    def test_methods_answer_as_the_simulator_methods_do(self):
        s = self.linked()
        p = s.plugin("hc6800_es")
        s.load_hex(hexfile(self, LFSR))
        p.call("enable_lcd")
        p.call("enable_digit_display", struct.pack("<i", 8))
        s.step(50_000)
        line = p.call("lcd_get_line", struct.pack("<ii", 0, 16))
        self.assertEqual(line.decode("ascii"), s.lcd_line(0))
        for i in range(8):
            seg, = struct.unpack("<i", p.call("digit_get_segments", struct.pack("<i", i)))
            self.assertEqual(seg, s.digit_segments(i))
        # nothing enabled answers as capi.h does
        self.assertEqual(struct.unpack("<i", p.call("xpt2046_get_last_channel")), (-1,))
        self.assertEqual(p.call("lcd_get_line", struct.pack("<ii", 1, 4)), s.lcd_line(1, 4).encode())

    def test_bad_calls_are_refused(self):
        p = self.linked().plugin("hc6800_es")
        with self.assertRaises(PluginError) as e:
            p.call("no_such_method")
        self.assertEqual(e.exception.code, Plugin.CALL_UNKNOWN)
        with self.assertRaises(PluginError) as e:
            p.call("enable_digit_display", b"\x08")
        self.assertEqual(e.exception.code, Plugin.CALL_BAD_ARGS)
        with self.assertRaises(PluginError) as e:
            p.call("enable_servo", struct.pack("<4i", 4, 0, 1000, 2000))  # no P4
        self.assertEqual(e.exception.code, Plugin.CALL_BAD_ARGS)

    def test_resets_do_not_pile_up_subscriptions(self):
        # sim_reset() used to destroy and recreate the LCD and the digit
        # display without dropping the old ones' bus subscriptions: the
        # seventh reset tripped bus.c's assertion, and every reset before
        # it left the bus calling into freed memory.
        s = self.linked()
        s.load_hex(hexfile(self, LFSR))
        s.enable_lcd()
        s.enable_digit_display(8)
        for _ in range(40):
            s.reset()
        s.step(50_000)
        self.assertGreater(s.instruction_count, 0)
        self.assertNotEqual(s.lcd_line(0), " " * 16)


@unittest.skipIf(NO_BOARD, NO_BOARD)
class LoadedTests(Fixture):
    def test_the_shared_object_loads_as_hc6800_es(self):
        s, p = self.loaded()
        self.assertEqual(p.name, "hc6800_es")
        self.assertEqual(os.path.realpath(p.path), os.path.realpath(BOARD))
        s.enable_lcd()
        self.assertEqual(s.plugins(), ["hc6800_es"], "the Simulator methods went to the loaded board")

    def test_it_takes_no_arguments(self):
        s = Simulator("bare")
        self.addCleanup(s.close)
        with self.assertRaises(PluginError) as e:
            s.load_plugin(BOARD, "anything")
        self.assertIn("takes no arguments", str(e.exception))

    def compare_lfsr(self, iap, plugin_args=None, step=lambda s: s.step(20_000)):
        a = self.linked(plugin_args)
        b, _ = self.loaded(plugin_args)
        path = hexfile(self, LFSR)
        for s in (a, b):
            if iap:
                s.enable_iap(Simulator.IAP_STC89C52RC)  # before the program: it erases code memory
            s.load_hex(path)
            enable_everything(s)
        return self.run_both(a, b, 30, step)

    def test_linked_and_loaded_read_the_same_in_tick_mode(self):
        last = self.compare_lfsr(iap=False)
        self.assertTrue(last["uart"])

    def test_linked_and_loaded_read_the_same_with_software_resets(self):
        last = self.compare_lfsr(iap=True, step=lambda s: s.step_instructions(7_000))
        self.assertGreater(last["iap"]["soft_resets"], 0)

    @unittest.skipUnless(DEMO, "build/demo_plugin%s not built" % SUFFIX)
    def test_linked_and_loaded_read_the_same_on_a_1t_core(self):
        # instruction mode: the board's tick hooks run once per 12 clocks
        last = self.compare_lfsr(iap=True, plugin_args="1t")
        self.assertGreater(last["iap"]["soft_resets"], 0)

    def demo(self, name, setup, instructions=2_000_000):
        if not os.path.isfile(hexpath(name)):
            self.skipTest(f"{name} not found (EMU8051_DEMO_BUILD)")
        a = self.linked()
        b, _ = self.loaded()
        for s in (a, b):
            setup(s)
            s.load_hex(hexpath(name))
            s.step_instructions(instructions)
        self.assertEqual(state(a), state(b))
        return state(a)

    def test_demo_lcd(self):
        st = self.demo("18_lcd_name_id.hex", lambda s: s.enable_lcd())
        self.assertNotEqual(st["lcd"][0].strip(), "")

    def test_demo_clock_on_the_lcd(self):
        def setup(s):
            s.enable_lcd()
            s.enable_ds1302()
            s.ds1302_set_time(second=45, minute=30, hour=14, date=20, month=3, weekday=2, year=26)
        self.demo("344_clock_lcd.hex", setup)

    def test_demo_digits(self):
        st = self.demo("17_digit_tube_student_id.hex", lambda s: s.enable_digit_display(8), 500_000)
        self.assertNotEqual(st["segments"], [0] * 8)

    def test_demo_clock_on_the_digits(self):
        def setup(s):
            s.enable_ds1302()
            s.enable_digit_display(8)
        self.demo("3431_clock_digit_tube_1.hex", setup, 500_000)


if __name__ == "__main__":
    unittest.main()
