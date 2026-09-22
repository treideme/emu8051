"""STC ISP/IAP flash model + minimal UART bootloader, end to end.

Runs the real SDCC-built images from the stc89c52-staging `bootloader/`
directory (set BOOTLOADER_BUILD to its build-direct/ folder, or keep the
sibling worktree layout) against sim/devices/iap.c:

  - IAP15 profile: program memory is IAP-writable, so the loader can really
    install, verify and run an application, and must survive power loss.
  - STC89C52RC profile: only Data Flash (2000h-2FFFh) is reachable; the
    loader must report application-area writes as failed rather than fooled,
    and its 'I' command must enter the ROM ISP monitor.

    uv run --no-project python -m unittest discover -s tests -v   (from python/)
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from pysim import Simulator  # noqa: E402

BUILD = os.environ.get(
    "BOOTLOADER_BUILD",
    os.path.join(os.path.dirname(__file__), "..", "..", "..",
                 "stc89c52-staging-bootloader", "build-direct"),
)
APP_BASE, BOOTREC, SECTOR = 0x0800, 0x1E00, 512
SCON, RI = 0x98, 0x01


def hexpath(name):
    return os.path.join(BUILD, name)


def need(*names):
    missing = [n for n in names if not os.path.isfile(hexpath(n))]
    return unittest.skipIf(missing, f"not built: {missing} (run bootloader/build.py)")


def load_hex_bytes(path):
    out = {}
    with open(path) as fh:
        for line in fh:
            line = line.strip()
            if not line.startswith(":") or int(line[7:9], 16) != 0:
                continue
            n, a = int(line[1:3], 16), int(line[3:7], 16)
            for i in range(n):
                out[a + i] = int(line[9 + 2 * i:11 + 2 * i], 16)
    return out


class Board:
    """One simulated HC6800-ES with the IAP model, driven over its UART."""

    CHUNK = 100  # instructions; keeps each TX burst inside the 18-byte ring

    def __init__(self, profile, *hexes):
        self.sim = Simulator("hc6800_es")
        self.sim.enable_iap(profile)          # before loading: FF-filled flash
        for h in hexes:
            self.sim.load_hex(hexpath(h))
        self.rx = bytearray()
        self._seen = 0

    def close(self):
        self.sim.close()

    def _drain(self):
        capi, h = self.sim._capi, self.sim._handle
        n = capi.sim_uart_tx_count(h)
        for i in range(self._seen, n):
            self.rx.append(capi.sim_uart_tx_byte(h, i) & 0xFF)
        self._seen = n

    def run(self, instructions):
        done = 0
        while done < instructions:
            self.sim.step_instructions(self.CHUNK)
            done += self.CHUNK
            self._drain()

    def run_until(self, needle, limit=3_000_000):
        done = 0
        while needle not in self.rx and done < limit:
            self.run(2000)
            done += 2000
        return needle in self.rx

    def power_cycle(self):
        self.sim.reset()
        self._seen = 0
        self.rx.clear()

    def send(self, data):
        for b in bytes(data):
            for _ in range(100_000):        # wait until the loader took the last byte
                if not self.sim.peek_sfr(SCON) & RI:
                    break
                self.run(self.CHUNK)
            self.sim.uart_inject_rx(b)

    def reply(self, n=1, limit=2_000_000):
        start = len(self.rx)
        done = 0
        while len(self.rx) < start + n and done < limit:
            self.run(500)
            done += 500
        return bytes(self.rx[start:start + n])

    def handshake(self, tries=200):
        """Send 'U' until '!' comes back, as a real host must: a byte that
        arrives before the loader has enabled its receiver (SCON) is lost,
        exactly as on hardware. Then let any extra '!' replies drain so they
        can't be mistaken for the answer to the next command."""
        start = len(self.rx)
        for _ in range(tries):
            self.send(b"U")
            self.run(1000)
            if b"!" in self.rx[start:]:
                self.run(20_000)
                return b"!"
        return bytes(self.rx[start:start + 1])

    def frame(self, cmd, addr, data=b"", csum_delta=0):
        body = bytes([ord(cmd), addr >> 8, addr & 0xFF, len(data)]) + bytes(data)
        return body + bytes([(-sum(body) + csum_delta) & 0xFF])

    def cmd(self, cmd, addr=0, data=b"", n=1, csum_delta=0):
        self.send(self.frame(cmd, addr, data, csum_delta))
        return self.reply(n)

    def program_app(self, hex_name):
        img = load_hex_bytes(hexpath(hex_name))
        top = max(img) + 1
        for sector in range(APP_BASE, top, SECTOR):
            assert self.cmd("E", sector) == b"k", f"erase {sector:#x}"
        for a in range(APP_BASE, top, 64):
            chunk = bytes(img.get(a + i, 0xFF) for i in range(min(64, top - a)))
            r = self.cmd("W", a, chunk)
            assert r == b"k", f"write {a:#x}: {r!r}"
        return img

    def app_sum(self):
        return sum(self.sim.iap_peek(a) for a in range(APP_BASE, BOOTREC)) & 0xFFFF


class Iap15BootloaderTests(unittest.TestCase):
    PROFILE = Simulator.IAP_IAP15

    def board(self, *hexes):
        b = Board(self.PROFILE, "loader_iap15.hex", *hexes)
        self.addCleanup(b.close)
        return b

    @need("loader_iap15.hex", "testapp_v1.hex")
    def test_handshake_timeout_falls_through_to_a_valid_app(self):
        b = self.board("testapp_v1.hex")
        s = b.app_sum()                           # sign it as 'C' would
        for off, v in ((2, s & 0xFF), (3, s >> 8), (1, 0x5A), (0, 0xA5)):
            b.sim.iap_poke(BOOTREC + off, v)
        self.assertTrue(b.run_until(b"T3\n"), f"app did not run: {bytes(b.rx)!r}")
        self.assertEqual(bytes(b.rx), b"APP1\nT3\n")  # T3 = Timer0 ISR via forwarded vector

    @need("loader_iap15.hex")
    def test_no_valid_app_stays_in_the_loader(self):
        b = self.board()
        b.run(400_000)
        self.assertEqual(bytes(b.rx), b"", "must not jump into blank flash")
        self.assertEqual(b.handshake(), b"!")

    @need("loader_iap15.hex", "testapp_v1.hex")
    def test_program_verify_commit_and_run(self):
        b = self.board()
        self.assertEqual(b.handshake(), b"!")
        img = b.program_app("testapp_v1.hex")
        for a in range(APP_BASE, max(img) + 1):
            self.assertEqual(b.sim.iap_peek(a), img.get(a, 0xFF), f"{a:#x}")
        # 'R' carries len payload bytes like every frame; replies len bytes + 'k'
        expect = bytes(img.get(APP_BASE + i, 0xFF) for i in range(8)) + b"k"
        self.assertEqual(b.cmd("R", APP_BASE, bytes(8), n=9), expect)
        self.assertEqual(b.cmd("C"), b"k")
        self.assertEqual(b.cmd("G"), b"k")
        self.assertTrue(b.run_until(b"T3\n"))
        self.assertTrue(bytes(b.rx).endswith(b"APP1\nT3\n"), bytes(b.rx))

    @need("loader_iap15.hex")
    def test_bad_checksum_is_rejected_and_nothing_is_written(self):
        b = self.board()
        b.handshake()
        before = b.sim.iap_stats()["programs"]
        self.assertEqual(b.cmd("W", APP_BASE, b"\x12\x34", csum_delta=1), b"x")
        self.assertEqual(b.sim.iap_stats()["programs"], before)
        self.assertEqual(b.sim.iap_peek(APP_BASE), 0xFF)

    @need("loader_iap15.hex")
    def test_loader_refuses_to_touch_itself_or_the_boot_record(self):
        b = self.board()
        loader = load_hex_bytes(hexpath("loader_iap15.hex"))
        b.handshake()
        self.assertEqual(b.cmd("E", 0x0000), b"r")
        self.assertEqual(b.cmd("W", APP_BASE - 16, bytes(16)), b"r")
        self.assertEqual(b.cmd("W", BOOTREC - 2, bytes(4)), b"r")
        self.assertEqual(b.cmd("E", BOOTREC), b"r")
        self.assertEqual(b.sim.iap_stats()["erases"], 0)
        for a, v in loader.items():
            self.assertEqual(b.sim.peek_code(a), v)

    @need("loader_iap15.hex", "testapp_v1.hex", "testapp_v2.hex")
    def test_power_loss_mid_update_leaves_the_loader_intact_and_the_app_unbootable(self):
        b = self.board()
        loader = load_hex_bytes(hexpath("loader_iap15.hex"))
        b.handshake()
        b.program_app("testapp_v1.hex")
        self.assertEqual(b.cmd("C"), b"k")

        # New session: erase and write only half of v2, then pull the plug.
        b.power_cycle()
        b.handshake()
        img2 = load_hex_bytes(hexpath("testapp_v2.hex"))
        self.assertEqual(b.cmd("E", APP_BASE), b"k")
        half = APP_BASE + (max(img2) + 1 - APP_BASE) // 2
        for a in range(APP_BASE, half, 64):
            chunk = bytes(img2.get(a + i, 0xFF) for i in range(min(64, half - a)))
            self.assertEqual(b.cmd("W", a, chunk), b"k")
        b.power_cycle()

        b.run(600_000)
        self.assertNotIn(b"APP", bytes(b.rx), "half-written app must not run")
        for a, v in loader.items():
            self.assertEqual(b.sim.peek_code(a), v, f"loader byte {a:#x} changed")
        self.assertEqual(b.handshake(), b"!")

        b.program_app("testapp_v2.hex")         # recovery
        self.assertEqual(b.cmd("C"), b"k")
        self.assertEqual(b.cmd("G"), b"k")
        self.assertTrue(b.run_until(b"APP2\n"))

    @need("loader_iap15.hex", "testapp_v1.hex", "testapp_v2.hex")
    def test_reprogramming_twice(self):
        b = self.board()
        for name, banner in (("testapp_v1.hex", b"APP1\n"),
                             ("testapp_v2.hex", b"APP2\n"),
                             ("testapp_v1.hex", b"APP1\n")):
            b.power_cycle()
            self.assertEqual(b.handshake(), b"!")
            b.program_app(name)
            self.assertEqual(b.cmd("C"), b"k")
            b.power_cycle()                      # boot unattended this time
            self.assertTrue(b.run_until(b"T3\n"), f"{name}: {bytes(b.rx)!r}")
            self.assertEqual(bytes(b.rx), banner + b"T3\n")


class Stc89BootloaderTests(unittest.TestCase):
    """The fitted part: IAP cannot reach the application area."""

    def board(self, *hexes):
        b = Board(Simulator.IAP_STC89C52RC, *hexes)
        self.addCleanup(b.close)
        return b

    @need("loader_stc89.hex", "testapp_v1.hex")
    def test_app_area_write_is_reported_as_failed_not_fooled(self):
        b = self.board("loader_stc89.hex", "testapp_v1.hex")   # both flashed by stcgal
        self.assertEqual(b.handshake(), b"!")
        before = b.sim.peek_code(APP_BASE + 0x40)
        self.assertEqual(b.cmd("W", APP_BASE + 0x40, b"\x00"), b"v")
        self.assertEqual(b.sim.peek_code(APP_BASE + 0x40), before)
        self.assertGreater(b.sim.iap_stats()["ignored"], 0)

    @need("loader_stc89.hex", "testapp_v1.hex")
    def test_timeout_runs_the_stcgal_flashed_app(self):
        b = self.board("loader_stc89.hex", "testapp_v1.hex")
        self.assertTrue(b.run_until(b"T3\n"))
        self.assertEqual(bytes(b.rx), b"APP1\nT3\n")

    @need("loader_stc89.hex")
    def test_I_enters_the_rom_isp_without_a_power_cycle(self):
        b = self.board("loader_stc89.hex")
        self.assertEqual(b.handshake(), b"!")
        self.assertEqual(b.cmd("I"), b"k")
        b.run(2000)
        self.assertEqual(b.sim.iap_stats()["isp_entries"], 1)
        # The ROM monitor is not modelled; with no ISP host it returns to the
        # AP area (EN-271 p. 37), so the loader is back and answering.
        self.assertEqual(b.handshake(), b"!")

    @need("iapprobe.hex")
    def test_iap_probe_output_matches_the_datasheet_model(self):
        b = self.board("iapprobe.hex")
        self.assertTrue(b.run_until(b"DONE\n"))
        lines = bytes(b.rx).decode().splitlines()
        self.assertEqual(lines[0], "IAPPROBE")
        self.assertEqual(lines[2], "E2000 FF FF FF FF FF FF FF FF")
        self.assertEqual(lines[3], "P2000 A0 A1 A2 A3 A4 A5 A6 A7")
        self.assertEqual(lines[4], "S3C")        # 0x3000 outside the 4 KB Data Flash
        self.assertEqual(lines[5], "AFF FF 3C")  # app-area program ignored
        self.assertEqual(lines[6], "DONE")


if __name__ == "__main__":
    unittest.main(verbosity=2)
