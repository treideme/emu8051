"""The plugin host (sim/plugin.h), through tests/plugin/demo_plugin.c.

The demo plugin models nothing real; it uses each part of the host's
function table once, and these tests check each from outside: loading and
its failures, plugin methods, SFR and xdata hooks, a 1T cycle table,
interrupt vectors past 2Bh with the plugin's own priorities, opcode
replacement, exception filtering, memory sizes, pins, reset, and calls
from one plugin to another. The programs are a few hand-assembled bytes
each, written out as Intel HEX.

Needs the meson build (build/demo_plugin.so); skipped without it.

    python -m unittest discover -s tests -v   (from python/)
"""

import os
import struct
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(REPO, "python"))
from pysim import Plugin, PluginError, Simulator, find_library  # noqa: E402


def _built(name):
    for d in ("build", os.path.join("build", "tests", "plugin")):
        p = os.path.join(REPO, d, name)
        if os.path.isfile(p):
            return p
    return None


DEMO = os.environ.get("EMU8051_DEMO_PLUGIN") or _built("demo_plugin.so")
SKIP = None
if not os.path.isfile(find_library()):
    SKIP = "emu8051 sim library not built"
elif not DEMO:
    SKIP = "build/demo_plugin.so not built (meson setup build && ninja -C build)"

REG_P1, REG_P3 = 0x10, 0x30


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


@unittest.skipIf(SKIP, SKIP)
class PluginTests(unittest.TestCase):
    def sim(self, args="", program=None, board="bare"):
        s = Simulator(board)
        self.addCleanup(s.close)
        p = s.load_plugin(DEMO, args)
        if program is not None:
            s.load_hex(hexfile(self, program))
        return s, p

    def stats(self, p):
        keys = ("resets", "instructions", "waits", "interrupts", "ticks", "exceptions")
        return dict(zip(keys, struct.unpack("<6I", p.call("stats"))))

    # ---- loading

    def test_loads_and_answers_its_methods(self):
        s, p = self.sim()
        self.assertIsInstance(p, Plugin)
        self.assertEqual(p.name, "demo")
        self.assertEqual(s.plugins(), ["demo"])
        self.assertEqual(p.call("echo", b"abc"), b"abc")
        self.assertEqual(os.path.realpath(p.path), os.path.realpath(DEMO))
        self.assertTrue(p.context)
        with self.assertRaises(PluginError) as e:
            p.call("no such method")
        self.assertEqual(e.exception.code, Plugin.CALL_UNKNOWN)
        with self.assertRaises(PluginError) as e:
            p.call("raise", b"\x07")
        self.assertEqual(e.exception.code, Plugin.CALL_BAD_ARGS)

    def test_a_large_answer_is_asked_for_again(self):
        _, p = self.sim()
        self.assertEqual(p.call("echo", bytes(range(200)), cap=16), bytes(range(200)))

    def test_exported_functions_take_the_context(self):
        _, p = self.sim()
        import ctypes
        waiting = p.function("demo_rx_waiting", ctypes.c_int)
        self.assertEqual(waiting(), 0)
        p.call("inject", b"xyz")
        self.assertEqual(waiting(), 3)

    def test_a_missing_file_is_refused_with_the_reason(self):
        s = Simulator("bare")
        self.addCleanup(s.close)
        with self.assertRaises(PluginError) as e:
            s.load_plugin(os.path.join(REPO, "build", "no-such-plugin.so"))
        self.assertEqual(e.exception.code, -1)
        self.assertIn("no-such-plugin", str(e.exception))

    def test_a_library_without_the_entry_point_is_refused(self):
        core = _built("libcore.so")
        if not core:
            self.skipTest("build/libcore.so not built")
        s = Simulator("bare")
        self.addCleanup(s.close)
        with self.assertRaises(PluginError) as e:
            s.load_plugin(core)
        self.assertEqual(e.exception.code, -2)

    def test_init_can_refuse_with_a_message(self):
        s = Simulator("bare")
        self.addCleanup(s.close)
        with self.assertRaises(PluginError) as e:
            s.load_plugin(DEMO, "fail")
        self.assertEqual(e.exception.code, -4)
        self.assertIn("demo: asked to fail", str(e.exception))
        self.assertEqual(s.plugins(), [])

    def test_another_abi_is_refused(self):
        s = Simulator("bare")
        self.addCleanup(s.close)
        with self.assertRaises(PluginError) as e:
            s.load_plugin(DEMO, "abi=99")
        self.assertEqual(e.exception.code, -3)
        self.assertIn("99", str(e.exception))

    def test_the_same_name_twice_is_numbered(self):
        s, a = self.sim()
        b = s.load_plugin(DEMO)
        self.assertEqual((a.name, b.name), ("demo", "demo#2"))
        self.assertEqual(s.plugins(), ["demo", "demo#2"])

    def test_a_plugin_calls_another(self):
        s, a = self.sim("name=a")
        b = s.load_plugin(DEMO, "name=b")
        b.call("inject", b"q")
        self.assertEqual(a.call("relay", b"b\0echo\0hello"), b"hello")
        with self.assertRaises(PluginError) as e:
            a.call("relay", b"c\0echo\0x")
        self.assertEqual(e.exception.code, Plugin.CALL_NO_PLUGIN)

    def test_it_loads_on_a_board_too(self):
        s, p = self.sim(board="hc6800_es")
        self.assertEqual(p.call("echo", b"!"), b"!")

    # ---- SFRs and xdata

    # 0000 MOV DPTR,#FF0Dh / 0003 MOVX A,@DPTR / 0004 JZ 0003
    # 0006 MOV DPTR,#FF0Eh / 0009 MOVX A,@DPTR / 000A INC A
    # 000B MOV DPTR,#FF0Fh / 000E MOVX @DPTR,A / 000F MOV F9h,A / 0011 SJMP 0000
    ECHO = {0: [0x90, 0xFF, 0x0D, 0xE0, 0x60, 0xFD, 0x90, 0xFF, 0x0E, 0xE0, 0x04,
                0x90, 0xFF, 0x0F, 0xF0, 0xF5, 0xF9, 0x80, 0xED]}

    def test_xdata_window_and_sfr_writes(self):
        s, p = self.sim(program=self.ECHO)
        p.call("inject", b"\x01\x02\x03")
        s.step_instructions(200)
        self.assertEqual(p.call("tx"), b"\x02\x03\x04")
        self.assertEqual(p.call("sfr_log"), b"\x02\x03\x04")
        self.assertEqual(s.exceptions(), [])

    def test_sfr_reads(self):
        # MOV A,FAh / MOV 30h,A / MOV A,FAh / MOV 31h,A / SJMP $
        s, _ = self.sim(program={0: [0xE5, 0xFA, 0xF5, 0x30, 0xE5, 0xFA, 0xF5, 0x31, 0x80, 0xFE]})
        s.step_instructions(10)
        self.assertEqual((s.peek_idata(0x30), s.peek_idata(0x31)), (1, 2))

    def test_xdata_outside_the_window_is_ram_and_wraps_with_the_size(self):
        # MOV DPTR,#0400h / MOV A,#5Ah / MOVX @DPTR,A / MOV DPTR,#0 /
        # MOVX A,@DPTR / MOV 30h,A / SJMP $
        prog = {0: [0x90, 0x04, 0x00, 0x74, 0x5A, 0xF0, 0x90, 0x00, 0x00, 0xE0, 0xF5, 0x30, 0x80, 0xFE]}
        s, p = self.sim("code=4096 xdata=1024", prog)
        self.assertEqual(struct.unpack("<IIB", p.call("mem")), (4096, 1024, 1))
        s.step_instructions(10)
        self.assertEqual(s.peek_idata(0x30), 0x5A)
        s2, _ = self.sim(program=prog)
        s2.step_instructions(10)
        self.assertEqual(s2.peek_idata(0x30), 0x00)

    def test_a_bad_memory_size_refuses_the_load(self):
        s = Simulator("bare")
        self.addCleanup(s.close)
        with self.assertRaises(PluginError):
            s.load_plugin(DEMO, "code=3000")

    # ---- timing

    # MOV R7,#10 / DJNZ R7,$ / SJMP $
    LOOP = {0: [0x7F, 0x0A, 0xDF, 0xFE, 0x80, 0xFE]}

    def test_classic_timing_without_a_cycle_table(self):
        s, p = self.sim(program=self.LOOP)
        self.assertFalse(s.instruction_mode)
        # tick(): an instruction runs in its first machine cycle, so the
        # eleventh (DJNZ, two cycles) has run but not yet burnt its second
        s.step_instructions(11)
        self.assertEqual(s.clock_count, 12 + 9 * 24 + 12)
        self.assertEqual(s.tick_count, 20)
        # the end hook sees every tick
        self.assertEqual(self.stats(p)["ticks"], 20)

    def test_a_cycle_table_makes_a_1t_core(self):
        s, p = self.sim("1t", self.LOOP)
        self.assertTrue(s.instruction_mode)
        self.assertEqual(s.step_instructions(11), 11)
        self.assertEqual(s.clock_count, 1 + 9 * 3 + 1)
        self.assertEqual(s.pc, 4)
        self.assertEqual(self.stats(p)["instructions"], 11)
        clocks, _, instructions = struct.unpack("<III", p.call("clocks"))
        self.assertEqual((clocks, instructions), (29, 11))
        # sim_step's ticks are 12 clocks in instruction mode too
        s.step(10)
        self.assertEqual(s.clock_count, 29 + 120)

    def test_a_jump_to_the_next_instruction_is_timed_by_whether_it_jumped(self):
        # MOV 30h,#2 / DJNZ 30h,+0 (jumps) / DJNZ 30h,+0 (falls through) / SJMP $
        prog = {0: [0x75, 0x30, 0x02, 0xD5, 0x30, 0x00, 0xD5, 0x30, 0x00, 0x80, 0xFE]}
        s, _ = self.sim("1t", prog)
        s.step_instructions(3)
        self.assertEqual(s.clock_count, 1 + 3 + 1)
        self.assertEqual((s.pc, s.peek_idata(0x30)), (9, 0))
        self.assertEqual((s.peek_code(5), s.peek_code(8)), (0, 0))  # the offsets put back

    def test_begin_can_take_a_step_without_an_instruction(self):
        s, p = self.sim("1t", self.LOOP)
        s.step_instructions(1)
        self.assertEqual(s.clock_count, 1)
        p.call("stall", struct.pack("<I", 50))
        s.step_instructions(1)
        self.assertEqual(s.clock_count, 1 + 50 + 3)
        self.assertEqual(s.instruction_count, 2)
        self.assertEqual(self.stats(p)["waits"], 1)

    # ---- interrupts

    # 0000 LJMP 0060h / 004B LJMP 0080h / 0053 LJMP 0090h
    # 0060 SETB EA / MOV FBh,#3 / SJMP $
    # 0080 INC 31h / 0082 MOV A,30h / 0084 JZ 0082 / INC 32h / RETI   (source 1)
    # 0090 MOV 30h,#1 / INC 33h / RETI                                 (source 2)
    IRQ = {
        0x00: [0x02, 0x00, 0x60],
        0x4B: [0x02, 0x00, 0x80],
        0x53: [0x02, 0x00, 0x90],
        0x60: [0xD2, 0xAF, 0x75, 0xFB, 0x03, 0x80, 0xFE],
        0x80: [0x05, 0x31, 0xE5, 0x30, 0x60, 0xFC, 0x05, 0x32, 0x32],
        0x90: [0x75, 0x30, 0x01, 0x05, 0x33, 0x32],
    }

    def test_vectors_past_2bh_with_the_plugins_priorities(self):
        s, p = self.sim("irq", self.IRQ)
        self.assertTrue(s.instruction_mode)
        s.step_instructions(50)
        self.assertEqual(s.pc, 0x65)
        p.call("raise", b"\x01")
        s.step_instructions(50)
        self.assertEqual((s.peek_idata(0x31), s.peek_idata(0x32)), (1, 0))
        self.assertIn(s.pc, (0x82, 0x84))
        # the same source again waits: same level, already in service
        p.call("raise", b"\x01")
        s.step_instructions(50)
        self.assertEqual(s.peek_idata(0x31), 1)
        # the higher one preempts; then the first finishes and the second
        # request of source 1 is taken after its RETI
        p.call("raise", b"\x02")
        s.step_instructions(100)
        self.assertEqual([s.peek_idata(a) for a in (0x30, 0x31, 0x32, 0x33)], [1, 2, 2, 1])
        self.assertEqual(s.pc, 0x65)
        self.assertEqual(struct.unpack("<II", p.call("irq_counts")), (2, 1))
        self.assertEqual(self.stats(p)["interrupts"], 3)
        self.assertEqual(s.peek_sfr(0x81), 7)  # every RETI popped what entry pushed

    def test_ea_gates_the_plugins_sources(self):
        prog = dict(self.IRQ)
        prog[0x60] = [0x75, 0xFB, 0x03, 0x80, 0xFE]  # no SETB EA
        s, p = self.sim("irq", prog)
        s.step_instructions(20)
        p.call("raise", b"\x02")
        s.step_instructions(50)
        self.assertEqual(s.peek_idata(0x33), 0)
        self.assertEqual(struct.unpack("<II", p.call("irq_counts")), (0, 0))

    # ---- opcodes and exceptions

    def test_an_opcode_can_be_replaced(self):
        s, _ = self.sim("a5", {0: [0xA5, 0xA5, 0x80, 0xFE]})
        s.step_instructions(3)
        self.assertEqual(s.peek_sfr(0xE0), 2)
        self.assertEqual(s.exceptions(), [])
        s.reset()  # survives the core's reset(), which rebuilds the table
        s.step_instructions(3)
        self.assertEqual(s.peek_sfr(0xE0), 2)

    def test_exceptions_pass_the_filter(self):
        prog = {0: [0xA5, 0xA5, 0x80, 0xFE]}
        s, p = self.sim(program=prog)
        s.step_instructions(3)
        self.assertEqual(s.exceptions(), ["ILLEGAL_OPCODE", "ILLEGAL_OPCODE"])
        s, p = self.sim("drop=5", prog)
        s.step_instructions(3)
        self.assertEqual(s.exceptions(), [])
        self.assertEqual(self.stats(p)["exceptions"], 2)

    # ---- pins and reset

    def test_pins(self):
        # CPL P1.0 three times / MOV A,P3 / MOV 30h,A / SJMP $
        prog = {0: [0xB2, 0x90, 0xB2, 0x90, 0xB2, 0x90, 0xE5, 0xB0, 0xF5, 0x30, 0x80, 0xFE]}
        s, p = self.sim(program=prog)
        p.call("drive", bytes([REG_P3, 2, 0]))
        self.assertEqual(p.call("level", bytes([REG_P3, 2])), b"\x00")
        s.step_instructions(6)
        self.assertEqual(s.peek_idata(0x30), 0xFB)
        self.assertEqual(p.call("pin_log"), b"\x00\x01\x00")
        # a drive from the host side is seen too, and so is its release
        s.set_pin(1, 0, 1)
        s.set_pin(1, 0, None)
        self.assertEqual(p.call("pin_log"), b"\x00\x01\x00\x01\x00")
        p.call("drive", bytes([REG_P3, 2, 0xFF]))
        self.assertEqual(p.call("level", bytes([REG_P3, 2])), b"\x01")
        # a reset puts the latch back to 1, and the watcher hears it
        s.reset()
        self.assertEqual(p.call("pin_log")[-1:], b"\x01")

    def test_reset_reaches_the_plugin(self):
        s, p = self.sim("1t", self.LOOP)
        p.call("inject", b"ab")
        s.step_instructions(5)
        s.reset()
        st = self.stats(p)
        self.assertEqual(st["resets"], 1)
        self.assertEqual((s.clock_count, s.instruction_count, s.pc), (0, 0, 0))
        import ctypes
        self.assertEqual(p.function("demo_rx_waiting", ctypes.c_int)(), 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
